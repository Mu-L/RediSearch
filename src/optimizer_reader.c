/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#include "optimizer_reader.h"
#include "aggregate/aggregate.h"

int cmpAsc(const void *v1, const void *v2, const void *udata) {
  RSIndexResult *res1 = (RSIndexResult *)v1;
  RSIndexResult *res2 = (RSIndexResult *)v2;

  if (res1->data.num.value > res2->data.num.value) return 1;
  if (res1->data.num.value < res2->data.num.value) return -1;
  return res1->docId < res2->docId ? -1 : 1;
}

int cmpDesc(const void *v1, const void *v2, const void *udata) {
  RSIndexResult *res1 = (RSIndexResult *)v1;
  RSIndexResult *res2 = (RSIndexResult *)v2;

  if (res1->data.num.value > res2->data.num.value) return -1;
  if (res1->data.num.value < res2->data.num.value) return 1;
  return res1->docId < res2->docId ? -1 : 1;
}

static inline double getSuccessRatio(const OptimizerIterator *optIt) {
  double resultsCollectedSinceLast = heap_count(optIt->heap) - optIt->heapOldSize;
  return resultsCollectedSinceLast / optIt->lastLimitEstimate;
}


static size_t OPT_NumEstimated(void *ctx) {
  OptimizerIterator *opt = ctx;
  return MIN(opt->child->NumEstimated(opt->child->ctx) ,
              opt->numericIter->NumEstimated(opt->numericIter->ctx));
}

static size_t OPT_Len(void *ctx) {
  return OPT_NumEstimated(ctx);
}

static void OPT_Abort(void *ctx) {
  OptimizerIterator *opt = ctx;
  opt->base.isValid = 0;
}

static t_docId OPT_LastDocId(void *ctx) {
  OptimizerIterator *opt = ctx;
  return opt->lastDocId;
}

static void OPT_Rewind(void *ctx) {
  OptimizerIterator *optIt = ctx;
  QOptimizer *qOpt = optIt->optim;
  heap_t *heap = optIt->heap;
  IndexIterator *child = optIt->child;

  // rewind child iterator
  child->Rewind(child->ctx);

  // update numeric filter with old iterator result estimation
  // used to skip ranges when creating new numeric iterator
  IndexIterator *numeric = optIt->numericIter;
  NumericFilter *nf = qOpt->nf;
  nf->offset += numeric->NumEstimated(numeric->ctx);
  numeric->Free(numeric);
  optIt->numericIter = NULL;

  double successRatio = getSuccessRatio(optIt);

  // very low success, lets get all remaining results
  if (successRatio < 0.01 || optIt->numIterations == 3) {
    nf->limit = optIt->numDocs;
  } else {
    int resultsMissing = heap_size(heap) - heap_count(heap);
    size_t limitEstimate = QOptimizer_EstimateLimit(optIt->numDocs, optIt->childEstimate, resultsMissing);
    optIt->lastLimitEstimate = nf->limit = limitEstimate * successRatio;
  }

  FieldFilterContext filterCtx = {.field = {.isFieldMask = false, .value = {.index= optIt->numericFieldIndex}}, .predicate = FIELD_EXPIRATION_DEFAULT};
  // create new numeric filter
  optIt->numericIter = NewNumericFilterIterator(qOpt->sctx, qOpt->nf, qOpt->conc, INDEXFLD_T_NUMERIC, optIt->config, &filterCtx);

  optIt->heapOldSize = heap_count(heap);
  optIt->numIterations++;
}

static int OPT_HasNext(void *ctx) {
  OptimizerIterator *opt = ctx;
  return opt->base.isValid;
}

void OptimizerIterator_Free(struct indexIterator *self) {
  OptimizerIterator *it = self->ctx;
  if (it == NULL) {
    return;
  }

  if(it->flags & OPTIM_OWN_NF) {
    NumericFilter_Free(it->optim->nf);
  }

  it->child->Free(it->child);

  if (it->numericIter) {
    it->numericIter->Free(it->numericIter);
  }

  IndexResult_Free(it->base.current);
  // we always use the array as RSResultType_Numeric. no need for IndexResult_Free
  rm_free(it->resArr);
  heap_free(it->heap);

  rm_free(it);
}

int OPT_ReadYield(void *ctx, RSIndexResult **e) {
  OptimizerIterator *it = ctx;
  if (heap_count(it->heap) > 0) {
    *e = heap_poll(it->heap);
    return INDEXREAD_OK;
  }
  return INDEXREAD_EOF;
}

int OPT_Read(void *ctx, RSIndexResult **e) {
  int rc1, rc2;
  OptimizerIterator *it = ctx;
  QOptimizer *opt = it->optim;

  IndexIterator *child = it->child;
  IndexIterator *numeric = it->numericIter;
  RSIndexResult *childRes = NULL;
  RSIndexResult *numericRes = NULL;

  it->hitCounter = 0;

  while (1) {
    ResultMetrics_Free(it->base.current);


    while (1) {
      // get next result
      if (numericRes == NULL || childRes->docId == numericRes->docId) {
        rc1 = child->Read(child->ctx, &childRes);
        if (rc1 == INDEXREAD_EOF) break;
        rc2 = numeric->SkipTo(numeric->ctx, childRes->docId, &numericRes);
      } else if (childRes->docId > numericRes->docId) {
        rc2 = numeric->SkipTo(numeric->ctx, childRes->docId, &numericRes);
      } else {
        rc1 = child->SkipTo(child->ctx, numericRes->docId, &childRes);
      }

      if (rc1 == INDEXREAD_EOF || rc2 == INDEXREAD_EOF) {
        break;
      }

      it->hitCounter++;
      if (childRes->docId == numericRes->docId) {
        it->lastDocId = childRes->docId;

        // copy the numeric result for the sorting heap
        if (numericRes->type == RSResultType_Numeric) {
          *it->pooledResult = *numericRes;
        } else {
          const RSIndexResult *child = AggregateResult_Get(&numericRes->data.agg, 0);
          RS_LOG_ASSERT(child->type == RSResultType_Numeric, "???");
          *it->pooledResult = *(child);
        }

        // handle expired results
        const RSDocumentMetadata *dmd = DocTable_Borrow(&opt->sctx->spec->docs, childRes->docId);
        if (!dmd) {
          continue;
        }
        it->pooledResult->dmd = dmd;

        // heap is not full. insert
        if (heap_count(it->heap) < heap_size(it->heap)) {
          heap_offer(&it->heap, it->pooledResult);
          it->pooledResult++;

        // heap is full. try to replace
        } else {
          RSIndexResult *tempRes = heap_peek(it->heap);
          if (it->cmp(tempRes, it->pooledResult, NULL) > 0) {
            heap_replace(it->heap, it->pooledResult);
            it->pooledResult = tempRes;
          }
          DMD_Return(it->pooledResult->dmd);
        }
      }
    }

    // Not enough result, try to rewind
    if (heap_size(it->heap) > heap_count(it->heap) && it->offset < it->childEstimate) {
      if (getSuccessRatio(it) < 1) {
        OPT_Rewind(it->base.ctx);
        childRes = numericRes = NULL;
        // rewind was successful, continue iteration
        if (it->numericIter != NULL) {
          numeric = it->numericIter;
          it->hitCounter = 0;
          it->numIterations++;
          continue;
        }
      } else {
        RedisModule_Log(RSDummyContext, "verbose", "Not enough results collected, but success ratio is %f", getSuccessRatio(it));
        RedisModule_Log(RSDummyContext, "debug", "Heap size: %d, heap count: %d, offset: %ld, childEstimate: %ld",
                                        heap_size(it->heap), heap_count(it->heap), it->offset, it->childEstimate);
      }
    }

    it->base.Read = OPT_ReadYield;
    return OPT_ReadYield(ctx, e);
  }
}

IndexIterator *NewOptimizerIterator(QOptimizer *qOpt, IndexIterator *root, IteratorsConfig *config) {
  OptimizerIterator *oi = rm_calloc(1, sizeof(*oi));
  oi->child = root;
  oi->optim = qOpt;
  oi->lastDocId = 0;

  oi->cmp = qOpt->asc ? cmpAsc : cmpDesc;
  oi->resArr = rm_malloc((qOpt->limit + 1) * sizeof(RSIndexResult));
  oi->pooledResult = oi->resArr;
  oi->heap = rm_malloc(heap_sizeof(qOpt->limit));
  heap_init(oi->heap, oi->cmp, NULL, qOpt->limit);

  oi->numDocs = qOpt->sctx->spec->docs.size;
  oi->childEstimate = root->NumEstimated(root->ctx);

  const FieldSpec *field = IndexSpec_GetFieldWithLength(qOpt->sctx->spec, qOpt->fieldName, strlen(qOpt->fieldName));
  // if there is no numeric range query but sortby, create a Numeric Filter
  if (!qOpt->nf) {
    qOpt->nf = NewNumericFilter(-INFINITY, INFINITY, 1, 1, qOpt->asc, field);
    oi->flags |= OPTIM_OWN_NF;
  }
  oi->lastLimitEstimate = qOpt->nf->limit =
    QOptimizer_EstimateLimit(oi->numDocs, oi->childEstimate, qOpt->limit);

  FieldFilterContext filterCtx = {.field = {.isFieldMask = false, .value = {.index= field->index}}, .predicate = FIELD_EXPIRATION_DEFAULT};
  oi->numericFieldIndex = field->index;
  oi->numericIter = NewNumericFilterIterator(qOpt->sctx, qOpt->nf, qOpt->conc, INDEXFLD_T_NUMERIC, config, &filterCtx);
  if (!oi->numericIter) {
    oi->base.ctx = oi;
    OptimizerIterator_Free(&oi->base);
    return NewEmptyIterator();
  }

  oi->offset = oi->numericIter->NumEstimated(oi->numericIter->ctx);
  oi->config = config;

  IndexIterator *ri = &oi->base;
  ri->ctx = oi;
  ri->type = OPTIMUS_ITERATOR;

  ri->NumEstimated = OPT_NumEstimated;
  ri->LastDocId = OPT_LastDocId;
  ri->Free = OptimizerIterator_Free;
  ri->Len = OPT_Len;
  ri->Abort = OPT_Abort;
  ri->Rewind = OPT_Rewind;
  ri->HasNext = OPT_HasNext;
  ri->SkipTo = NULL;            // The iterator is always on top and and Read() is called
  ri->Read = OPT_Read;
  ri->current = NewNumericResult();

  return &oi->base;
}
