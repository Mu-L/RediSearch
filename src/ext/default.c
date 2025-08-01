/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/param.h>

#include "redisearch.h"
#include "spec.h"
#include "types_rs.h"
#include "query.h"
#include "synonym_map.h"
#include "snowball/include/libstemmer.h"
#include "default.h"
#include "tokenize.h"
#include "rmutil/vector.h"
#include "stemmer.h"
#include "phonetic_manager.h"
#include "score_explain.h"

/******************************************************************************************
 *
 * TF-IDF Scoring Functions
 *
 * We have 2 TF-IDF scorers - one where TF is normalized by max frequency, the other where it is
 * normalized by total weighted number of terms in the document
 *
 ******************************************************************************************/

// normalize TF by max frequency
#define NORM_MAXFREQ 1
// normalize TF by number of tokens (weighted)
#define NORM_DOCLEN 2

static void strExpCreateParent(const ScoringFunctionArgs *ctx, RSScoreExplain **scrExp) {
  if (*scrExp) {
    RSScoreExplain *finalScrExp = rm_calloc(1, sizeof(RSScoreExplain));
    finalScrExp->numChildren = 1;
    finalScrExp->children = *scrExp;
    ((ScoringFunctionArgs *)ctx)->scrExp = *scrExp = finalScrExp;
  }
}

// recursively calculate tf-idf
static double tfidfRecursive(const RSIndexResult *r, const RSDocumentMetadata *dmd,
                             RSScoreExplain *scrExp) {
  if (r->type == RSResultType_Term) {
    double idf = r->data.term.term ? r->data.term.term->idf : 0;
    double res = r->weight * ((double)r->freq) * idf;
    EXPLAIN(scrExp, "(TFIDF %.2f = Weight %.2f * TF %d * IDF %.2f)", res, r->weight, r->freq, idf);
    return res;
  }
  if (r->type & (RSResultType_Intersection | RSResultType_Union | RSResultType_HybridMetric)) {
    double ret = 0;
    if (!scrExp) {
      RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
      RSIndexResult *child = NULL;

      while (AggregateResultIter_Next(iter, &child)) {
        ret += tfidfRecursive(child, dmd, NULL);
      }

      AggregateResultIter_Free(iter);
    } else {
      size_t numChildren = AggregateResult_NumChildren(&r->data.agg);
      scrExp->numChildren = numChildren;
      scrExp->children = rm_calloc(numChildren, sizeof(RSScoreExplain));

      int i = 0;
      RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
      RSIndexResult *child = NULL;

      while (AggregateResultIter_Next(iter, &child)) {
        ret += tfidfRecursive(child, dmd, &scrExp->children[i]);
        i++;
      }

      AggregateResultIter_Free(iter);

      EXPLAIN(scrExp, "(Weight %.2f * total children TFIDF %.2f)", r->weight, ret);
    }
    return r->weight * ret;
  }
  EXPLAIN(scrExp, "(TFIDF %.2f = Weight %.2f * Frequency %d)", r->weight * (double)r->freq,
          r->weight, r->freq);
  return r->weight * (double)r->freq;
}

/* internal common tf-idf function, where just the normalization method changes */
static inline double tfIdfInternal(const ScoringFunctionArgs *ctx, const RSIndexResult *h,
                                   const RSDocumentMetadata *dmd, double minScore, int normMode) {
  RSScoreExplain *scrExp = (RSScoreExplain *)ctx->scrExp;
  if (dmd->score == 0) {
    EXPLAIN(scrExp, "Document score is 0");
    return 0;
  }
  uint32_t norm = normMode == NORM_MAXFREQ ? dmd->maxFreq : dmd->len;
  if (norm == 0) {
    EXPLAIN(scrExp, "Document %s is 0", normMode == NORM_MAXFREQ ? "max frequency" : "length");
    return 0;
  }
  double rawTfidf = tfidfRecursive(h, dmd, scrExp);
  double tfidf = dmd->score * rawTfidf / norm;
  strExpCreateParent(ctx, &scrExp);

  // no need to factor the distance if tfidf is already below minimal score
  if (tfidf < minScore) {
    EXPLAIN(scrExp, "TFIDF score of %.2f is smaller than minimum score %.2f", tfidf, minScore);
    return 0;
  }

  int slop = ctx->GetSlop(h);
  tfidf /= slop;

  EXPLAIN(scrExp, "Final TFIDF : words TFIDF %.2f * document score %.2f / norm %d / slop %d",
          rawTfidf, dmd->score, norm, slop);

  return tfidf;
}

/* Calculate sum(TF-IDF)*document score for each result, where TF is normalized by maximum frequency
 * in this document*/
static double TFIDFScorer(const ScoringFunctionArgs *ctx, const RSIndexResult *h,
                          const RSDocumentMetadata *dmd, double minScore) {
  return tfIdfInternal(ctx, h, dmd, minScore, NORM_MAXFREQ);
}

/* Identical scorer to TFIDFScorer, only the normalization is by total weighted frequency in the doc
 */
static double TFIDFNormDocLenScorer(const ScoringFunctionArgs *ctx, const RSIndexResult *h,
                                    const RSDocumentMetadata *dmd, double minScore) {

  return tfIdfInternal(ctx, h, dmd, minScore, NORM_DOCLEN);
}

/******************************************************************************************
 *
 * BM25 Scoring Functions
 * NOTE: this is a legacy *non-standard* computation of BM25, and is deprecated after introducing
 * the BM25STD scorer.
 *
 ******************************************************************************************/

/* recursively calculate score for each token, summing up sub tokens */
static double bm25Recursive(const ScoringFunctionArgs *ctx, const RSIndexResult *r,
                            const RSDocumentMetadata *dmd, RSScoreExplain *scrExp) {
  static const float b = 0.5;
  static const float k1 = 1.2;
  double f = (double)r->freq;
  double ret = 0;
  if (r->type == RSResultType_Term) {
    double idf = (r->data.term.term ? r->data.term.term->idf : 0);
    ret = r->weight * idf * f / (f + k1 * (1.0f - b + b * ctx->indexStats.avgDocLen));
    EXPLAIN(scrExp,
            "(%.2f = Weight %.2f * IDF %.2f * F %d / (F %d + k1 1.2 * (1 - b 0.5 + b 0.5 * Average Len %.2f)))",
            ret, r->weight, idf, r->freq, r->freq, ctx->indexStats.avgDocLen);

  } else if (r->type & (RSResultType_Intersection | RSResultType_Union | RSResultType_HybridMetric)) {
    if (!scrExp) {
      RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
      RSIndexResult *child = NULL;

      while (AggregateResultIter_Next(iter, &child)) {
        ret += bm25Recursive(ctx, child, dmd, NULL);
      }

      AggregateResultIter_Free(iter);
    } else {
      size_t numChildren = AggregateResult_NumChildren(&r->data.agg);
      scrExp->numChildren = numChildren;
      scrExp->children = rm_calloc(numChildren, sizeof(RSScoreExplain));

      int i = 0;
      RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
      RSIndexResult *child = NULL;

      while (AggregateResultIter_Next(iter, &child)) {
        ret += bm25Recursive(ctx, child, dmd, &scrExp->children[i]);
        i++;
      }

      AggregateResultIter_Free(iter);

      EXPLAIN(scrExp, "(Weight %.2f * children BM25 %.2f)", r->weight, ret);
    }
    ret *= r->weight;
  } else if (f) {  // default for virtual type -just disregard the idf
    ret = r->weight * f / (f + k1 * (1.0f - b + b * ctx->indexStats.avgDocLen));
    EXPLAIN(
        scrExp,
        "(%.2f = Weight %.2f * F %d / (F %d + k1 1.2 * (1 - b 0.5 + b 0.5 * Average Len %.2f)))",
        ret, r->weight, r->freq, r->freq, ctx->indexStats.avgDocLen);
  } else {
    EXPLAIN(scrExp, "Frequency 0 -> value 0");
  }

  return ret;
}

/* BM25 scoring function */
static double BM25Scorer(const ScoringFunctionArgs *ctx, const RSIndexResult *r,
                         const RSDocumentMetadata *dmd, double minScore) {
  RSScoreExplain *scrExp = (RSScoreExplain *)ctx->scrExp;
  double bm25res = bm25Recursive(ctx, r, dmd, scrExp);
  double score = dmd->score * bm25res;
  strExpCreateParent(ctx, &scrExp);

  // no need to factor the distance if tfidf is already below minimal score
  if (score < minScore) {
    EXPLAIN(scrExp, "BM25 score of %.2f is smaller than minimum score %.2f", bm25res, score);
    return 0;
  }
  int slop = ctx->GetSlop(r);
  score /= slop;

  EXPLAIN(scrExp, "Final BM25 : words BM25 %.2f * document score %.2f / slop %d", bm25res,
          dmd->score, slop);

  return score;
}

/******************************************************************************************
 *
 * BM25 Scoring Functions - standard version according to https://en.wikipedia.org/wiki/Okapi_BM25
 *
 ******************************************************************************************/

static double inline CalculateBM25Std(float b, float k1, double idf, double f, int doc_len,
                                      double avg_doc_len, double weight, RSScoreExplain *scrExp, const char *term) {
  double ret = weight * idf * f * (k1 + 1) / (f + k1 * (1.0f - b + b * (float)doc_len/avg_doc_len));
  EXPLAIN(scrExp,
          "%s: (%.2f = Weight %.2f * IDF %.2f * (F %.2f * (k1 1.2 + 1)) / (F %.2f + k1 1.2 * (1 - b 0.75 + b 0.75 *"
          " Doc Len %d / Average Doc Len %.2f)))",
          term, ret, weight, idf, f, f, doc_len, avg_doc_len);
  return ret;
}

/* recursively calculate score for each token, summing up sub tokens */
static double bm25StdRecursive(const ScoringFunctionArgs *ctx, const RSIndexResult *r,
                            const RSDocumentMetadata *dmd, RSScoreExplain *scrExp) {
  static const float b = 0.75f;
  static const float k1 = 1.2f;
  double f = (double)r->freq;
  double ret = 0;
  if (r->type == RSResultType_Term) {
    // Compute IDF based on total number of docs in the index and the term's total frequency.
    double idf = r->data.term.term->bm25_idf;
    ret = CalculateBM25Std(b, k1, idf, f, dmd->len, ctx->indexStats.avgDocLen, r->weight, scrExp,
                           r->data.term.term->str);
  } else if (r->type & (RSResultType_Intersection | RSResultType_Union | RSResultType_HybridMetric)) {
    if (!scrExp) {
      RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
      RSIndexResult *child = NULL;

      while (AggregateResultIter_Next(iter, &child)) {
        ret += bm25StdRecursive(ctx, child, dmd, NULL);
      }

      AggregateResultIter_Free(iter);
    } else {
      size_t numChildren = AggregateResult_NumChildren(&r->data.agg);
      scrExp->numChildren = numChildren;
      scrExp->children = rm_calloc(numChildren, sizeof(RSScoreExplain));

      int i = 0;
      RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
      RSIndexResult *child = NULL;

      while (AggregateResultIter_Next(iter, &child)) {
        ret += bm25StdRecursive(ctx, child, dmd, &scrExp->children[i]);
        i++;
      }

      AggregateResultIter_Free(iter);

      EXPLAIN(scrExp, "(Weight %.2f * children BM25 %.2f)", r->weight, ret);
    }
    ret *= r->weight;
  } else if (r->type == RSResultType_Virtual && f && r->weight) {
    // For wildcard, score should be determined only by the weight
    // and the document's length (so we set idf and f to be 1).
    double idf = 1.0;
    ret = CalculateBM25Std(b, k1, idf, 1, dmd->len, ctx->indexStats.avgDocLen, r->weight, scrExp, "*");
  } else {
    // Record is either optional term with no match or non text token.
    // For optional term with no match - we would expect 0 contribution to the score
    // (the weight should be set to 0).
    EXPLAIN(scrExp, "Irrelevant token -> score is 0");
  }

  return ret;
}

/* BM25 scoring function - standard version */
static double BM25StdScorer(const ScoringFunctionArgs *ctx, const RSIndexResult *r,
                         const RSDocumentMetadata *dmd, double minScore) {
  RSScoreExplain *scrExp = (RSScoreExplain *)ctx->scrExp;
  double bm25res = bm25StdRecursive(ctx, r, dmd, scrExp);
  double score = dmd->score * bm25res;
  strExpCreateParent(ctx, &scrExp);

  EXPLAIN(scrExp, "Final BM25 : words BM25 %.2f * document score %.2f", bm25res,
          dmd->score);

  return score;
}

/******************************************************************************************
 *
 * Normalized BM25 Scoring Function
 *
 ******************************************************************************************/

/* Stretched tanh.
 * The stretching is in the sense that we increase the range in which the tanh
 * function behaves as a linear function, thus more suiting to our scoring
 * expectations.
 */
static inline double tanhStretched(double x, double stretch) {
  return tanh((1 / stretch) * x);
}

/* Normalized BM25 scoring function (of the standard version)
 * The normalization is done by applying the stretched hyperbolic tangent function
 * on the standard BM25 score of the result, resulting in a score in the range [0,1].
 * The stretch factor is used to control the range of the linear part of the
 * tanh function, after which the scores are mapped to ~1.
*/
static double BM25StdTanhScorer(const ScoringFunctionArgs *ctx, const RSIndexResult *r,
                         const RSDocumentMetadata *dmd, double minScore) {
  RSScoreExplain *scrExp = (RSScoreExplain *)ctx->scrExp;
  double bm25res = bm25StdRecursive(ctx, r, dmd, scrExp);
  double score = dmd->score * bm25res;
  strExpCreateParent(ctx, &scrExp);

  EXPLAIN(scrExp, "Final BM25 : words BM25 %.2f * document score %.2f", bm25res,
          dmd->score);

  // Normalize the score
  double normalizedScore = tanhStretched(score, ctx->tanhFactor);

  // Modify the explanation to include the normalization
  strExpCreateParent(ctx, &scrExp);
  EXPLAIN(scrExp,
    "Final Normalized BM25 : tanh(stretch factor 1/%d * Final BM25 %.2f)",
    ctx->tanhFactor, score);

  return normalizedScore;
}

/******************************************************************************************
 *
 * Raw document-score scorer. Just returns the document score
 *
 ******************************************************************************************/
static double DocScoreScorer(const ScoringFunctionArgs *ctx, const RSIndexResult *r,
                             const RSDocumentMetadata *dmd, double minScore) {
  RSScoreExplain *scrExp = (RSScoreExplain *)ctx->scrExp;
  EXPLAIN(scrExp, "Document's score is %.2f", dmd->score);
  return dmd->score;
}

/******************************************************************************************
 *
 * DISMAX-style scorer
 *
 ******************************************************************************************/
static double dismaxRecursive(const ScoringFunctionArgs *ctx, const RSIndexResult *r,
                              RSScoreExplain *scrExp) {
  // for terms - we return the term frequency
  double ret = 0;
  switch (r->type) {
    case RSResultType_Term:
    case RSResultType_Metric:
    case RSResultType_Numeric:
    case RSResultType_Virtual:
      ret = r->freq;
      EXPLAIN(scrExp, "DISMAX %.2f = Weight %.2f * Frequency %d", r->weight * ret, r->weight,
              r->freq);
      break;
    // for intersections - we sum up the term scores
    case RSResultType_Intersection:
      if (!scrExp) {
        RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
        RSIndexResult *child = NULL;

        while (AggregateResultIter_Next(iter, &child)) {
          ret += dismaxRecursive(ctx, child, NULL);
        }

        AggregateResultIter_Free(iter);
      } else {
        size_t numChildren = AggregateResult_NumChildren(&r->data.agg);
        scrExp->numChildren = numChildren;
        scrExp->children = rm_calloc(numChildren, sizeof(RSScoreExplain));

        int i = 0;
        RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
        RSIndexResult *child = NULL;

        while (AggregateResultIter_Next(iter, &child)) {
          ret += dismaxRecursive(ctx, child, &scrExp->children[i]);
          i++;
        }

        AggregateResultIter_Free(iter);

        EXPLAIN(scrExp, "%.2f = Weight %.2f * children DISMAX %.2f", r->weight * ret, r->weight,
                ret);
      }
      break;
    // for unions - we take the max frequency
    case RSResultType_Union:
      if (!scrExp) {
        RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
        RSIndexResult *child = NULL;

        while (AggregateResultIter_Next(iter, &child)) {
          ret = MAX(ret, dismaxRecursive(ctx, child, NULL));
        }

        AggregateResultIter_Free(iter);
      } else {
        size_t numChildren = AggregateResult_NumChildren(&r->data.agg);
        scrExp->numChildren = numChildren;
        scrExp->children = rm_calloc(numChildren, sizeof(RSScoreExplain));

        int i = 0;
        RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
        RSIndexResult *child = NULL;

        while (AggregateResultIter_Next(iter, &child)) {
          ret = MAX(ret, dismaxRecursive(ctx, child, &scrExp->children[i]));
          i++;
        }

        AggregateResultIter_Free(iter);

        EXPLAIN(scrExp, "%.2f = Weight %.2f * children DISMAX %.2f", r->weight * ret, r->weight,
                ret);
      }
      break;
    // for hybrid - just take the non-vector child score (the second one).
    case RSResultType_HybridMetric:
      return dismaxRecursive(ctx, AggregateResult_Get(&r->data.agg, 1), scrExp);
  }
  return r->weight * ret;
}
/* Calculate sum(TF-IDF)*document score for each result */
static double DisMaxScorer(const ScoringFunctionArgs *ctx, const RSIndexResult *h,
                           const RSDocumentMetadata *dmd, double minScore) {
  // if (dmd->score == 0 || h == NULL) return 0;
  return dismaxRecursive(ctx, h, ctx->scrExp);
}
/* taken from redis - bitops.c */
static const unsigned char bitsinbyte[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8};

/* HAMMING - Scorer using Hamming distance between the query payload and the document payload. Only
 * works if both have the payloads the same length */
static double HammingDistanceScorer(const ScoringFunctionArgs *ctx, const RSIndexResult *h,
                                    const RSDocumentMetadata *dmd, double minScore) {
  RSScoreExplain *scrExp = (RSScoreExplain *)ctx->scrExp;
  // the strings must be of the same length > 0
  if (!hasPayload(dmd->flags) || !dmd->payload->len || dmd->payload->len != ctx->qdatalen) {
    EXPLAIN(scrExp, "Payloads provided to scorer vary in length");
    return 0;
  }
  size_t ret = 0;
  size_t len = ctx->qdatalen;
  // if the strings are not aligned to 64 bit - calculate the diff byte by

  const unsigned char *a = (unsigned char *)ctx->qdata;
  const unsigned char *b = (unsigned char *)dmd->payload->data;
  for (size_t i = 0; i < len; i++) {
    ret += bitsinbyte[(unsigned char)(a[i] ^ b[i])];
  }
  double result = 1.0 / (double)(ret + 1);
  EXPLAIN(scrExp, "String length is %zu. Bit count is %zu. Result is (1 / count + 1) = %.2f", len,
          ret, result);
  // we inverse the distance, and add 1 to make sure a distance of 0 yields a perfect score of 1
  return result;
}

typedef struct {
  int isCn;
  union {
    struct {
      RSTokenizer *tokenizer;
      Vector *tokList;
    } cn;
    struct sb_stemmer *latin;
  } data;
} defaultExpanderCtx;

static void expandCn(RSQueryExpanderCtx *ctx, RSToken *token) {
  defaultExpanderCtx *dd = ctx->privdata;
  RSTokenizer *tokenizer;
  if (!dd) {
    dd = ctx->privdata = rm_calloc(1, sizeof(*dd));
    dd->isCn = 1;
  }
  if (!dd->data.cn.tokenizer) {
    tokenizer = dd->data.cn.tokenizer = NewChineseTokenizer(NULL, NULL, 0);
    dd->data.cn.tokList = NewVector(char *, 4);
  }

  tokenizer = dd->data.cn.tokenizer;
  Vector *tokVec = dd->data.cn.tokList;

  tokVec->top = 0;
  tokenizer->Start(tokenizer, token->str, token->len, 0);

  Token tTok = {0};
  while (tokenizer->Next(tokenizer, &tTok)) {
    char *s = rm_strndup(tTok.tok, tTok.tokLen);
    Vector_Push(tokVec, s);
  }

  ctx->ExpandTokenWithPhrase(ctx, (const char **)tokVec->data, tokVec->top, token->flags, 0, 0);
}

/******************************************************************************************
 *
 * Stemmer based query expander
 *
 ******************************************************************************************/
int StemmerExpander(RSQueryExpanderCtx *ctx, RSToken *token) {

  // we store the stemmer as private data on the first call to expand
  defaultExpanderCtx *dd = ctx->privdata;
  struct sb_stemmer *sb;

  if (!ctx->privdata) {
    if (ctx->language == RS_LANG_CHINESE) {
      expandCn(ctx, token);
      return REDISMODULE_OK;
    } else {
      dd = ctx->privdata = rm_calloc(1, sizeof(*dd));
      dd->isCn = 0;
      sb = dd->data.latin = sb_stemmer_new(RSLanguage_ToString(ctx->language), NULL);
    }
  }

  if (dd->isCn) {
    expandCn(ctx, token);
    return REDISMODULE_OK;
  }

  sb = dd->data.latin;

  // No stemmer available for this language - just return the node so we won't
  // be called again
  if (!sb) {
    return REDISMODULE_OK;
  }

  const sb_symbol *b = (const sb_symbol *)token->str;
  const sb_symbol *stemmed = sb_stemmer_stem(sb, b, token->len);

  if (stemmed) {
    int sl = sb_stemmer_length(sb);

    // Make a copy of the stemmed buffer with the + prefix given to stems
    char *dup = rm_malloc(sl + 2);
    dup[0] = STEM_PREFIX;
    memcpy(dup + 1, stemmed, sl + 1);

    // Get fieldMask which includes only expandable fields
    QueryNode *qn = *ctx->currentNode;
    t_fieldMask orig_fm = qn->opts.fieldMask;
    t_fieldMask expandable_fm = qn->opts.fieldMask;
    if (orig_fm != RS_FIELDMASK_ALL) {
      t_fieldMask fm = qn->opts.fieldMask;
      t_fieldMask bit_mask = 1;
      while (fm) {
        if (fm & bit_mask) {
            const FieldSpec *fs = IndexSpec_GetFieldByBit(ctx->handle->spec, bit_mask);
            if (fs && FieldSpec_IsNoStem(fs)) {
              expandable_fm &= ~bit_mask;
            }
        }
        fm &= ~bit_mask;
        bit_mask <<= 1;
      }
    }

    /* Replace current node with a new union node if needed */
    if (qn->type != QN_UNION) {
      QueryNode *un = NewUnionNode();

      un->opts.fieldMask = qn->opts.fieldMask;

      /* Append current node to the new union node as a child */
      QueryNode_AddChild(un, qn);
      *ctx->currentNode = un;
    }

    // Add expanded nodes with corresponding field mask
    qn = *ctx->currentNode;
    qn->opts.fieldMask = expandable_fm;
    ctx->ExpandToken(ctx, dup, sl + 1, 0x0);  // TODO: Set proper flags here
    if (sl != token->len || strncmp((const char *)stemmed, token->str, token->len)) {
      ctx->ExpandToken(ctx, rm_strndup((const char *)stemmed, sl), sl, 0x0);
    }
    // Restore field mask of UNION node
    qn->opts.fieldMask = orig_fm;
  }
  return REDISMODULE_OK;
}

void StemmerExpanderFree(void *p) {
  if (!p) {
    return;
  }
  defaultExpanderCtx *dd = p;
  if (dd->isCn) {
    dd->data.cn.tokenizer->Free(dd->data.cn.tokenizer);
    Vector_Free(dd->data.cn.tokList);
  } else if (dd->data.latin) {
    sb_stemmer_delete(dd->data.latin);
  }
  rm_free(dd);
}

/******************************************************************************************
 *
 * phonetic based query expander
 *
 ******************************************************************************************/
int PhoneticExpand(RSQueryExpanderCtx *ctx, RSToken *token) {
  char *primary = NULL;

  PhoneticManager_ExpandPhonetics(NULL, token->str, token->len, &primary, NULL);

  if (primary) {
    ctx->ExpandToken(ctx, primary, strlen(primary), 0x0);
  }
  return REDISMODULE_OK;
}

/******************************************************************************************
 *
 * Synonyms based query expander
 *
 ******************************************************************************************/
int SynonymExpand(RSQueryExpanderCtx *ctx, RSToken *token) {
#define BUFF_LEN 100
  IndexSpec *spec = ctx->handle->spec;
  if (!spec->smap) {
    return REDISMODULE_OK;
  }

  TermData *t_data = SynonymMap_GetIdsBySynonym(spec->smap, token->str, token->len);

  if (t_data == NULL) {
    return REDISMODULE_OK;
  }

  for (int i = 0; i < array_len(t_data->groupIds); ++i) {
    ctx->ExpandToken(ctx, rm_strdup(t_data->groupIds[i]), strlen(t_data->groupIds[i]), 0x0);
  }
  return REDISMODULE_OK;
}

/******************************************************************************************
 *
 * Default query expander
 *
 ******************************************************************************************/
// Assumes that the spec (ctx->handle->spec) is properly guarded for reading by the caller (read lock or redis lock)
int DefaultExpander(RSQueryExpanderCtx *ctx, RSToken *token) {
  int phonetic = (*(ctx->currentNode))->opts.phonetic;
  SynonymExpand(ctx, token);

  if (phonetic == PHONETIC_DEFAULT) {
    // Eliminate the phonetic expansion if we know that none of the fields
    // actually use phonetic matching
    if (IndexSpec_CheckPhoneticEnabled(ctx->handle->spec, (*ctx->currentNode)->opts.fieldMask)) {
      phonetic = PHONETIC_ENABLED;
    }
  } else if (phonetic == PHONETIC_ENABLED || phonetic == PHONETIC_DISABLED) {
    // Verify that the field is actually phonetic
    int isValid = 0;
    if ((*ctx->currentNode)->opts.fieldMask == RS_FIELDMASK_ALL) {
      if (ctx->handle->spec->flags & Index_HasPhonetic) {
        isValid = 1;
      }
    } else {
      t_fieldMask fm = (*ctx->currentNode)->opts.fieldMask;
      for (size_t ii = 0; ii < ctx->handle->spec->numFields; ++ii) {
        const FieldSpec *fs = ctx->handle->spec->fields + ii;
        if (!(fm & FIELD_BIT(fs))) {
          continue;
        }
        if (FieldSpec_IsPhonetics(fs)) {
          isValid = 1;
        }
      }
    }
    if (!isValid) {
      QueryError_SetError(ctx->status, QUERY_EINVAL, "field does not support phonetics");
      return REDISMODULE_ERR;
    }
  }
  if (phonetic == PHONETIC_ENABLED) {
    PhoneticExpand(ctx, token);
  }

  // stemmer is happening last because it might free the given 'RSToken *token'
  // this is a bad solution and should be fixed, but for now its good enough
  // todo: fix the free of the 'RSToken *token' by the stemmer and allow any
  //       expnders ordering!!
  StemmerExpander(ctx, token);
  return REDISMODULE_OK;
}

void DefaultExpanderFree(void *p) {
  StemmerExpanderFree(p);
}

/* Register the default extension */
int DefaultExtensionInit(RSExtensionCtx *ctx) {

  /* TF-IDF scorer */
  if (ctx->RegisterScoringFunction(TFIDF_SCORER_NAME, TFIDFScorer, NULL, NULL) ==
      REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  /* DisMax-alike scorer */
  if (ctx->RegisterScoringFunction(DISMAX_SCORER_NAME, DisMaxScorer, NULL, NULL) ==
      REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  /* Register BM25 scorer - DEPRECATED NON-STANDARD VARIATION */
  if (ctx->RegisterScoringFunction(BM25_SCORER_NAME, BM25Scorer, NULL, NULL) == REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  /* Register BM25 scorer - STANDARD VARIATION */
  if (ctx->RegisterScoringFunction(BM25_STD_SCORER_NAME, BM25StdScorer, NULL, NULL) == REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  /* Register BM25 scorer - NORMALIZED STANDARD VARIATION - TANH */
  if (ctx->RegisterScoringFunction(BM25_STD_NORMALIZED_TANH_SCORER_NAME, BM25StdTanhScorer, NULL, NULL) == REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  /* Register BM25 scorer - NORMALIZED STANDARD VARIATION - MAX */
  if (ctx->RegisterScoringFunction(BM25_STD_NORMALIZED_MAX_SCORER_NAME, BM25StdScorer, NULL, NULL) == REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  /* Register HAMMING scorer */
  if (ctx->RegisterScoringFunction(HAMMINGDISTANCE_SCORER, HammingDistanceScorer, NULL, NULL) ==
      REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }
  /* Register TFIDF.DOCNORM */
  if (ctx->RegisterScoringFunction(TFIDF_DOCNORM_SCORER_NAME, TFIDFNormDocLenScorer, NULL, NULL) ==
      REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  /* Register DOCSCORE scorer */
  if (ctx->RegisterScoringFunction(DOCSCORE_SCORER, DocScoreScorer, NULL, NULL) == REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  /* Snowball Stemmer is the default expander */
  if (ctx->RegisterQueryExpander(STEMMER_EXPENDER_NAME, StemmerExpander, StemmerExpanderFree,
                                 NULL) == REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  /* Synonyms expender */
  if (ctx->RegisterQueryExpander(SYNONYMS_EXPENDER_NAME, SynonymExpand, NULL, NULL) ==
      REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  /* Phonetic expender */
  if (ctx->RegisterQueryExpander(PHONETIC_EXPENDER_NAME, PhoneticExpand, NULL, NULL) ==
      REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  /* Default expender */
  if (ctx->RegisterQueryExpander(DEFAULT_EXPANDER_NAME, DefaultExpander, DefaultExpanderFree,
                                 NULL) == REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  return REDISEARCH_OK;
}
