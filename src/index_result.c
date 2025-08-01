/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#include "index_result.h"
#include "varint.h"
#include "types_rs.h"
#include "rmalloc.h"
#include <math.h>
#include <sys/param.h>
#include "src/util/arr.h"
#include "value.h"

/* Allocate a new aggregate result of a given type with a given capacity*/
RSIndexResult *__newAggregateResult(size_t cap, RSResultType t, double weight) {
  RSIndexResult *res = rm_new(RSIndexResult);

  *res = (RSIndexResult){
      .type = t,
      .docId = 0,
      .freq = 0,
      .fieldMask = 0,
      .isCopy = 0,
      .weight = weight,
      .metrics = NULL,
      .data.agg = AggregateResult_New(cap)
  };
  return res;
}

void IndexResult_ConcatMetrics(RSIndexResult *parent, RSIndexResult *child) {
  if (child->metrics) {
    // Passing ownership over the RSValues in the child metrics, but not on the array itself
    parent->metrics = array_ensure_append_n(parent->metrics, child->metrics, array_len(child->metrics));
    array_clear(child->metrics);
  }
}

/* Clear / free the metrics of a result */
void ResultMetrics_Free(RSIndexResult *r) {
  array_free_ex(r->metrics, RSValue_Decref(((RSYieldableMetric *)ptr)->value));
  r->metrics = NULL;
}

void Term_Offset_Data_Free(RSTermRecord *tr) {
  RSOffsetVector_FreeData(&tr->offsets);
}

/* Allocate a new intersection result with a given capacity*/
RSIndexResult *NewIntersectResult(size_t cap, double weight) {
  return __newAggregateResult(cap, RSResultType_Intersection, weight);
}

/* Allocate a new union result with a given capacity*/
RSIndexResult *NewUnionResult(size_t cap, double weight) {
  return __newAggregateResult(cap, RSResultType_Union, weight);
}

/* Allocate a new hybrid result with a given capacity (currently relevant for
 * hybrid vector similarity queries)*/
RSIndexResult *NewHybridResult() {
  return __newAggregateResult(2, RSResultType_HybridMetric, 1);
}

/* Allocate a new token record result for a given term */
RSIndexResult *NewTokenRecord(RSQueryTerm *term, double weight) {
  RSIndexResult *res = rm_new(RSIndexResult);

  *res = (RSIndexResult){.type = RSResultType_Term,
                         .docId = 0,
                         .fieldMask = 0,
                         .isCopy = 0,
                         .freq = 0,
                         .weight = weight,
                         .metrics = NULL,
                         .data.term = (RSTermRecord){
                             .term = term,
                             .offsets = (RSOffsetVector){},
                         }};
  return res;
}

RSIndexResult *NewNumericResult() {
  RSIndexResult *res = rm_new(RSIndexResult);

  *res = (RSIndexResult){.type = RSResultType_Numeric,
                         .docId = 0,
                         .isCopy = 0,
                         .fieldMask = RS_FIELDMASK_ALL,
                         .freq = 1,
                         .weight = 1,
                         .metrics = NULL,
                         .data.num = (RSNumericRecord){.value = 0}};
  return res;
}

RSIndexResult *NewVirtualResult(double weight, t_fieldMask fieldMask) {
  RSIndexResult *res = rm_new(RSIndexResult);

  *res = (RSIndexResult){
      .type = RSResultType_Virtual,
      .docId = 0,
      .fieldMask = fieldMask,
      .freq = 0,
      .weight = weight,
      .metrics = NULL,
      .isCopy = 0,
  };
  return res;
}

RSIndexResult *NewMetricResult() {
  RSIndexResult *res = rm_new(RSIndexResult);

  *res = (RSIndexResult){.type = RSResultType_Metric,
                         .docId = 0,
                         .isCopy = 0,
                         .fieldMask = RS_FIELDMASK_ALL,
                         .freq = 0,
                         .weight = 1,
                         .metrics = NULL,
                         .data.num = (RSNumericRecord){.value = 0}};
  return res;
}

RSIndexResult *IndexResult_DeepCopy(const RSIndexResult *src) {
  RSIndexResult *ret = rm_new(RSIndexResult);
  *ret = *src;
  ret->isCopy = 1;

  if (src->metrics) {
    // Create a copy of the array and increase the refcount for each element's value
    ret->metrics = NULL;
    ret->metrics = array_ensure_append_n(ret->metrics, src->metrics, array_len(src->metrics));
    for (size_t i = 0; i < array_len(ret->metrics); i++)
      RSValue_IncrRef(ret->metrics[i].value);
  }

  switch (src->type) {
    // copy aggregate types
    case RSResultType_Intersection:
    case RSResultType_Union:
    case RSResultType_HybridMetric:
    {
      // allocate a new child pointer array
      size_t numChildren = AggregateResult_NumChildren(&src->data.agg);
      ret->data.agg = AggregateResult_New(numChildren);

      // deep copy recursively all children
      RSAggregateResultIter *iter = AggregateResult_Iter(&src->data.agg);
      RSIndexResult *child = NULL;

      while (AggregateResultIter_Next(iter, &child)) {
        AggregateResult_AddChild(ret, IndexResult_DeepCopy(child));
      }

      AggregateResultIter_Free(iter);

      break;
    }

    // copy term results
    case RSResultType_Term:
      // copy the offset vectors
      RSOffsetVector_CopyData(&ret->data.term.offsets, &src->data.term.offsets);
      break;

    // the rest have no dynamic stuff, we can just copy the base result
    default:
      break;
  }
  return ret;
}

RSQueryTerm *NewQueryTerm(RSToken *tok, int id) {
  RSQueryTerm *ret = rm_malloc(sizeof(RSQueryTerm));
  ret->idf = 1;
  ret->str = tok->str ? rm_strndup(tok->str, tok->len) : NULL;
  ret->len = tok->len;
  ret->flags = tok->flags;
  ret->id = id;
  return ret;
}

void Term_Free(RSQueryTerm *t) {
  if (t) {
    if (t->str) rm_free(t->str);
    rm_free(t);
  }
}

int RSIndexResult_HasOffsets(const RSIndexResult *res) {
  switch (res->type) {
    case RSResultType_Term:
      return RSOffsetVector_Len(&res->data.term.offsets) > 0;
    case RSResultType_Intersection:
    case RSResultType_Union:
      // the intersection and union aggregates can have offsets if they are not purely made of
      // virtual results
      return AggregateResult_TypeMask(&res->data.agg) != RSResultType_Virtual && AggregateResult_TypeMask(&res->data.agg) != RS_RESULT_NUMERIC;

    // a virtual result doesn't have offsets!
    case RSResultType_Virtual:
    case RSResultType_Numeric:
    case RSResultType_Metric:
    default:
      return 0;
  }
}

void IndexResult_Free(RSIndexResult *r) {
  if (!r) return;
  ResultMetrics_Free(r);
  if (r->type == RSResultType_Intersection || r->type == RSResultType_Union || r->type == RSResultType_HybridMetric) {
    // for deep-copy results we also free the children
    if (r->isCopy) {
      RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
      RSIndexResult *child = NULL;

      while (AggregateResultIter_Next(iter, &child)) {
        IndexResult_Free(child);
      }

      AggregateResultIter_Free(iter);
    }
    AggregateResult_Free(r->data.agg);
  } else if (r->type == RSResultType_Term) {
    if (r->isCopy) {
      RSOffsetVector_FreeData(&r->data.term.offsets);

    } else {  // non copy result...

      // we only free up terms for non copy results
      if (r->data.term.term != NULL) {
        Term_Free(r->data.term.term);
      }
    }
  }

  rm_free(r);
}

#define __absdelta(x, y) (x > y ? x - y : y - x)
/**
Find the minimal distance between members of the vectos.
e.g. if V1 is {2,4,8} and V2 is {0,5,12}, the distance is 1 - abs(4-5)
@param vs a list of vector pointers
@param num the size of the list
*/
int IndexResult_MinOffsetDelta(const RSIndexResult *r) {
  if (!IndexResult_IsAggregate(r) || AggregateResult_NumChildren(&r->data.agg) <= 1) {
    return 1;
  }

  const RSAggregateResult *agg = &r->data.agg;
  int dist = 0;
  size_t num = AggregateResult_NumChildren(agg);

  RSOffsetIterator v1, v2;
  int i = 0;
  while (i < num) {
    // if either
    while (i < num && !RSIndexResult_HasOffsets(AggregateResult_Get(agg, i))) {
      i++;
      continue;
    }
    if (i == num) break;
    v1 = RSIndexResult_IterateOffsets(AggregateResult_Get(agg, i));
    i++;

    while (i < num && !RSIndexResult_HasOffsets(AggregateResult_Get(agg, i))) {
      i++;
      continue;
    }
    if (i == num) {
      v1.Free(v1.ctx);
      break;
    }
    v2 = RSIndexResult_IterateOffsets(AggregateResult_Get(agg, i));

    uint32_t p1 = v1.Next(v1.ctx, NULL);
    uint32_t p2 = v2.Next(v2.ctx, NULL);
    int cd = __absdelta(p2, p1);
    while (cd > 1 && p1 != RS_OFFSETVECTOR_EOF && p2 != RS_OFFSETVECTOR_EOF) {
      cd = MIN(__absdelta(p2, p1), cd);
      if (p2 > p1) {
        p1 = v1.Next(v1.ctx, NULL);
      } else {
        p2 = v2.Next(v2.ctx, NULL);
      }
    }

    v1.Free(v1.ctx);
    v2.Free(v2.ctx);

    dist += cd * cd;
  }

  // we return 1 if distance could not be calculate, to avoid division by zero
  return dist ? sqrt(dist) : AggregateResult_NumChildren(agg) - 1;
}

void result_GetMatchedTerms(RSIndexResult *r, RSQueryTerm *arr[], size_t cap, size_t *len) {
  if (*len == cap) return;

  switch (r->type) {
    case RSResultType_Intersection:
    case RSResultType_Union:
    {
      RSAggregateResultIter *iter = AggregateResult_Iter(&r->data.agg);
      RSIndexResult *child = NULL;

      while (AggregateResultIter_Next(iter, &child)) {
        result_GetMatchedTerms(child, arr, cap, len);
      }

      AggregateResultIter_Free(iter);

      break;
    }
    case RSResultType_Term:
      if (r->data.term.term) {
        const char *s = r->data.term.term->str;
        // make sure we have a term string and it's not an expansion
        if (s) {
          arr[(*len)++] = r->data.term.term;
        }
      }
    default:
      return;
  }
}

size_t IndexResult_GetMatchedTerms(RSIndexResult *r, RSQueryTerm **arr, size_t cap) {
  size_t arrlen = 0;
  result_GetMatchedTerms(r, arr, cap, &arrlen);
  return arrlen;
}

int __indexResult_withinRangeInOrder(RSOffsetIterator *iters, uint32_t *positions, int num,
                                     int maxSlop) {
  while (1) {

    // we start from the beginning, and a span of 0
    int span = 0;
    for (int i = 0; i < num; i++) {
      // take the current position and the position of the previous iterator.
      // For the first iterator we always advance once
      uint32_t pos = i ? positions[i] : iters[i].Next(iters[i].ctx, NULL);
      uint32_t lastPos = i ? positions[i - 1] : 0;

      // read while we are not in order
      while (pos != RS_OFFSETVECTOR_EOF && pos < lastPos) {
        pos = iters[i].Next(iters[i].ctx, NULL);
      }

      // we've read through the entire list and it's not in order relative to the last pos
      if (pos == RS_OFFSETVECTOR_EOF) {
        return 0;
      }
      positions[i] = pos;

      // add the diff from the last pos to the total span
      if (i > 0) {
        span += ((int)pos - (int)lastPos - 1);
        // if we are already out of slop - just quit
        if (span > maxSlop) {
          break;
        }
      }
    }

    if (span <= maxSlop) {
      return 1;
    }
  }

  return 0;
}

static inline uint32_t _arrayMin(uint32_t *arr, int len, uint32_t *pos) {
  int m = arr[0];
  *pos = 0;
  for (int i = 1; i < len; i++) {
    if (arr[i] < m) {
      m = arr[i];
      *pos = i;
    }
  }
  return m;
}

static inline uint32_t _arrayMax(uint32_t *arr, int len, uint32_t *pos) {
  int m = arr[0];
  *pos = 0;
  for (int i = 1; i < len; i++) {
    if (arr[i] >= m) {
      m = arr[i];
      *pos = i;
    }
  }
  return m;
}

/* Check the index result for maximal slop, in an unordered fashion.
 * The algorithm is simple - we find the first offsets min and max such that max-min<=maxSlop */
int __indexResult_withinRangeUnordered(RSOffsetIterator *iters, uint32_t *positions, int num,
                                       int maxSlop) {
  for (int i = 0; i < num; i++) {
    positions[i] = iters[i].Next(iters[i].ctx, NULL);
  }
  uint32_t minPos, maxPos, min, max;
  // find the max member
  max = _arrayMax(positions, num, &maxPos);

  while (1) {

    // we start from the beginning, and a span of 0
    min = _arrayMin(positions, num, &minPos);
    if (min != max) {
      // calculate max - min
      int span = (int)max - (int)min - (num - 1);
      // if it matches the condition - just return success
      if (span <= maxSlop) {
        return 1;
      }
    }

    // if we are not meeting the conditions - advance the minimal iterator
    positions[minPos] = iters[minPos].Next(iters[minPos].ctx, NULL);
    // If the minimal iterator is larger than the max iterator, the minimal iterator is the new
    // maximal iterator.
    if (positions[minPos] != RS_OFFSETVECTOR_EOF && positions[minPos] > max) {
      maxPos = minPos;
      max = positions[maxPos];

    } else if (positions[minPos] == RS_OFFSETVECTOR_EOF) {
      // this means we've reached the end
      break;
    }
  }

  return 0;
}

/** Test the result offset vectors to see if they fall within a max "slop" or distance between the
 * terms. That is the total number of non matched offsets between the terms is no bigger than
 * maxSlop.
 * e.g. for an exact match, the slop allowed is 0.
 */
int IndexResult_IsWithinRange(RSIndexResult *ir, int maxSlop, int inOrder) {

  // check if calculation is even relevant here...
  if ((ir->type & (RSResultType_Term | RSResultType_Virtual | RS_RESULT_NUMERIC)) ||
      AggregateResult_NumChildren(&ir->data.agg) <= 1) {
    return 1;
  }
  RSAggregateResult *r = &ir->data.agg;
  size_t num = AggregateResult_NumChildren(r);

  // Fill a list of iterators and the last read positions
  RSOffsetIterator iters[num];
  uint32_t positions[num];
  int n = 0;

  RSAggregateResultIter *iter = AggregateResult_Iter(r);
  RSIndexResult *child = NULL;

  while (AggregateResultIter_Next(iter, &child)) {
    // collect only iterators for nodes that can have offsets
    if (RSIndexResult_HasOffsets(child)) {
      iters[n] = RSIndexResult_IterateOffsets(child);
      positions[n] = 0;
      n++;
    }
  }

  AggregateResultIter_Free(iter);

  // No applicable offset children - just return 1
  if (n == 0) {
    return 1;
  }

  int rc;
  // cal the relevant algorithm based on ordered/unordered condition
  if (inOrder)
    rc = __indexResult_withinRangeInOrder(iters, positions, n, maxSlop);
  else
    rc = __indexResult_withinRangeUnordered(iters, positions, n, maxSlop);
  for (int i = 0; i < n; i++) {
    iters[i].Free(iters[i].ctx);
  }
  return rc;
}
