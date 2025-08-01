/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#include "forward_index.h"
#include "index.h"
#include "varint.h"
#include "spec.h"
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/param.h>
#include "rmalloc.h"
#include "rmutil/rm_assert.h"
#include "util/heap.h"
#include "profile.h"
#include "hybrid_reader.h"
#include "metric_iterator.h"
#include "optimizer_reader.h"
#include "util/units.h"

static int UI_SkipTo(void *ctx, t_docId docId, RSIndexResult **hit);
static int UI_SkipToHigh(void *ctx, t_docId docId, RSIndexResult **hit);
static inline int UI_ReadUnsorted(void *ctx, RSIndexResult **hit);
static int UI_ReadSorted(void *ctx, RSIndexResult **hit);
static int UI_ReadSortedHigh(void *ctx, RSIndexResult **hit);
static size_t UI_NumEstimated(void *ctx);
static size_t UI_Len(void *ctx);

static int II_SkipTo(void *ctx, t_docId docId, RSIndexResult **hit);
static int II_ReadSorted(void *ctx, RSIndexResult **hit);
static size_t II_NumEstimated(void *ctx);
static size_t II_Len(void *ctx);
static t_docId II_LastDocId(void *ctx);

#define CURRENT_RECORD(ii) (ii)->base.current

int cmpMinId(const void *e1, const void *e2, const void *udata) {
  const IndexIterator *it1 = e1, *it2 = e2;
  if (it1->minId < it2->minId) {
    return 1;
  } else if (it1->minId > it2->minId) {
    return -1;
  }
  return 0;
}

// Profile iterator, used for profiling. PI is added between all iterator
typedef struct {
  IndexIterator base;
  IndexIterator *child;
  ProfileCounters counters;
  clock_t cpuTime;
} ProfileIterator, ProfileIteratorCtx;

typedef struct {
  IndexIterator base;
  /**
   * We maintain two iterator arrays. One is the original iterator list, and
   * the other is the list of currently active iterators. When an iterator
   * reaches EOF, it is set to NULL in the `its` list, but is still retained in
   * the `origits` list, for the purpose of supporting things like Rewind() and
   * Free()
   */
  IndexIterator **its;
  IndexIterator **origits;
  uint32_t num;
  uint32_t norig;
  uint32_t currIt;
  t_docId minDocId;
  heap_t *heapMinId;

  // If set to 1, we exit skips after the first hit found and not merge further results
  int quickExit;
  size_t nexpected;
  double weight;
  uint64_t len;

  // type of query node UNION,GEO,NUMERIC...
  QueryNodeType origType;
  // original string for fuzzy or prefix unions
  const char *qstr;
} UnionIterator;

static void resetMinIdHeap(UnionIterator *ui) {
  heap_t *hp = ui->heapMinId;
  heap_clear(hp);

  for (int i = 0; i < ui->num; i++) {
    heap_offerx(hp, ui->its[i]);
  }
  RS_LOG_ASSERT(heap_count(hp) == ui->num,
                "count should be equal to number of iterators");
}

static void UI_HeapAddChildren(UnionIterator *ui, IndexIterator *it) {
  AggregateResult_AddChild(CURRENT_RECORD(ui), IITER_CURRENT_RECORD(it));
}

static inline t_docId UI_LastDocId(void *ctx) {
  return ((UnionIterator *)ctx)->minDocId;
}

static void UI_SyncIterList(UnionIterator *ui) {
  ui->num = ui->norig;
  memcpy(ui->its, ui->origits, sizeof(*ui->its) * ui->norig);
  for (size_t ii = 0; ii < ui->num; ++ii) {
    ui->its[ii]->minId = 0;
  }
  if (ui->heapMinId) {
    resetMinIdHeap(ui);
  }
}

/**
 * Removes the exhausted iterator from the active list, so that future
 * reads will no longer iterate over it
 */
static size_t UI_RemoveExhausted(UnionIterator *it, size_t badix) {
  // e.g. assume we have 10 entries, and we want to remove index 8, which means
  // one more valid entry at the end. This means we use
  // source: its + 8 + 1
  // destination: its + 8
  // number: it->len (10) - (8) - 1 == 1
  memmove(it->its + badix, it->its + badix + 1, sizeof(*it->its) * (it->num - badix - 1));
  it->num--;
  // Repeat the same index again, because we have a new iterator at the same
  // position
  return badix - 1;
}

static void UI_Abort(void *ctx) {
  UnionIterator *it = ctx;
  IITER_SET_EOF(&it->base);
  for (int i = 0; i < it->num; i++) {
    if (it->its[i]) {
      it->its[i]->Abort(it->its[i]->ctx);
    }
  }
}

static void UI_Rewind(void *ctx) {
  UnionIterator *ui = ctx;
  IITER_CLEAR_EOF(&ui->base);
  ui->minDocId = 0;
  CURRENT_RECORD(ui)->docId = 0;

  UI_SyncIterList(ui);

  // rewind all child iterators
  for (size_t i = 0; i < ui->num; i++) {
    ui->its[i]->minId = 0;
    ui->its[i]->Rewind(ui->its[i]->ctx);
  }
}

IndexIterator *NewUnionIterator(IndexIterator **its, int num, int quickExit,
                                double weight, QueryNodeType type, const char *qstr, IteratorsConfig *config) {
  // create union context
  UnionIterator *ctx = rm_calloc(1, sizeof(UnionIterator));
  ctx->origits = its;
  ctx->weight = weight;
  ctx->origType = type;
  ctx->num = num;
  ctx->norig = num;
  IITER_CLEAR_EOF(&ctx->base);
  CURRENT_RECORD(ctx) = NewUnionResult(num, weight);
  ctx->len = 0;
  ctx->quickExit = quickExit;
  ctx->its = rm_calloc(ctx->num, sizeof(*ctx->its));
  ctx->nexpected = 0;
  ctx->currIt = 0;
  ctx->heapMinId = NULL;
  ctx->qstr = qstr;

  // bind the union iterator calls
  IndexIterator *it = &ctx->base;
  it->ctx = ctx;
  it->type = UNION_ITERATOR;
  it->NumEstimated = UI_NumEstimated;
  it->LastDocId = UI_LastDocId;
  it->Read = UI_ReadSorted;
  it->SkipTo = UI_SkipTo;
  it->HasNext = NULL;
  it->Free = UnionIterator_Free;
  it->Len = UI_Len;
  it->Abort = UI_Abort;
  it->Rewind = UI_Rewind;
  UI_SyncIterList(ctx);

  for (size_t i = 0; i < num; ++i) {
    ctx->nexpected += IITER_NUM_ESTIMATED(its[i]);
  }

  if (ctx->norig > config->minUnionIterHeap) {
    it->Read = UI_ReadSortedHigh;
    it->SkipTo = UI_SkipToHigh;
    ctx->heapMinId = rm_malloc(heap_sizeof(num));
    heap_init(ctx->heapMinId, cmpMinId, NULL, num);
    resetMinIdHeap(ctx);
  }

  return it;
}

void UI_Foreach(IndexIterator *index_it, void (*callback)(IndexReader *it)) {
  UnionIterator *ui = index_it->ctx;
  for (int i = 0; i < ui->num; ++i) {
    IndexIterator *it = ui->its[i];
    if (it->type == PROFILE_ITERATOR) {
      // If this is a profile query, each IndexReader is wrapped in a ProfileIterator
      it = ((ProfileIterator *)(it->ctx))->child;
    }
    RS_LOG_ASSERT_FMT(it->type == READ_ITERATOR, "Expected read iterator, got %d", it->type);
    callback(it->ctx);
  }
}

static size_t UI_NumEstimated(void *ctx) {
  UnionIterator *ui = ctx;
  return ui->nexpected;
}

static inline int UI_ReadUnsorted(void *ctx, RSIndexResult **hit) {
  UnionIterator *ui = ctx;
  int rc = INDEXREAD_OK;
  RSIndexResult *res = NULL;
  while (ui->currIt < ui->num) {
    rc = ui->origits[ui->currIt]->Read(ui->origits[ui->currIt]->ctx, &res);
    if (rc == INDEXREAD_OK) {
      *hit = res;
      return rc;
    }
    ++ui->currIt;
  }
  return INDEXREAD_EOF;
}

static inline int UI_ReadSorted(void *ctx, RSIndexResult **hit) {
  UnionIterator *ui = ctx;
  // nothing to do
  if (ui->num == 0 || !IITER_HAS_NEXT(&ui->base)) {
    IITER_SET_EOF(&ui->base);
    return INDEXREAD_EOF;
  }

  int numActive = 0;
  IndexResult_ResetAggregate(CURRENT_RECORD(ui));

  do {

    // find the minimal iterator
    t_docId minDocId = DOCID_MAX;
    IndexIterator *minIt = NULL;
    numActive = 0;
    int rc = INDEXREAD_EOF;
    unsigned nits = ui->num;

    for (unsigned i = 0; i < nits; i++) {
      IndexIterator *it = ui->its[i];
      RSIndexResult *res = IITER_CURRENT_RECORD(it);
      rc = INDEXREAD_OK;
      // if this hit is behind the min id - read the next entry
      // printf("ui->docIds[%d]: %d, ui->minDocId: %d\n", i, ui->docIds[i], ui->minDocId);
      while (it->minId <= ui->minDocId && rc != INDEXREAD_EOF) {
        rc = INDEXREAD_NOTFOUND;
        // read while we're not at the end and perhaps the flags do not match
        while (rc == INDEXREAD_NOTFOUND) {
          rc = it->Read(it->ctx, &res);
          if (res) {
            it->minId = res->docId;
          }
        }
      }

      if (rc != INDEXREAD_EOF) {
        numActive++;
      } else {
        // Remove this from the active list
        i = UI_RemoveExhausted(ui, i);
        nits = ui->num;
        continue;
      }

      if (rc == INDEXREAD_OK && res->docId <= minDocId) {
        minDocId = res->docId;
        minIt = it;
      }
    }

    // take the minimum entry and collect all results matching to it
    if (minIt) {
      UI_SkipTo(ui, minIt->minId, hit);
      // return INDEXREAD_OK;
      ui->minDocId = minIt->minId;
      ui->len++;
      return INDEXREAD_OK;
    }

  } while (numActive > 0);
  IITER_SET_EOF(&ui->base);

  return INDEXREAD_EOF;
}

// UI_Read for iterator with high count of children
static inline int UI_ReadSortedHigh(void *ctx, RSIndexResult **hit) {
  UnionIterator *ui = ctx;
  IndexIterator *it = NULL;
  RSIndexResult *res;
  heap_t *hp = ui->heapMinId;

  // nothing to do
  if (!IITER_HAS_NEXT(&ui->base)) {
    IITER_SET_EOF(&ui->base);
    return INDEXREAD_EOF;
  }
  IndexResult_ResetAggregate(CURRENT_RECORD(ui));
  t_docId nextValidId = ui->minDocId + 1;

  /*
   * A min-heap maintains all sub-iterators which are not EOF.
   * In a loop, the iterator in heap root is checked. If it is valid, it is used,
   * otherwise, Read() is called on sub-iterator and it is returned into the heap
   * for future calls.
   */
  while (heap_count(hp)) {
    it = heap_peek(hp);
    res = IITER_CURRENT_RECORD(it);
    if (it->minId >= nextValidId && it->minId != 0) {
      // valid result since id at root of min-heap is higher than union min id
      break;
    }
    // read the next result and if valid, return the iterator into the heap
    int rc = it->SkipTo(it->ctx, nextValidId, &res);

    // refresh heap with iterator with updated minId
    if (rc == INDEXREAD_EOF) {
      heap_poll(hp);
    } else {
      it->minId = res->docId;
      heap_replace(hp, it);
      // after SkipTo, try test again for validity
      if (ui->quickExit && it->minId == nextValidId) {
        break;
      }
    }
  }

  if (!heap_count(hp)) {
    IITER_SET_EOF(&ui->base);
    return INDEXREAD_EOF;
  }

  ui->minDocId = it->minId;

  // On quickExit we just return one result.
  // Otherwise, we collect all the results that equal to the root of the heap.
  if (ui->quickExit) {
    AggregateResult_AddChild(CURRENT_RECORD(ui), res);
  } else {
    heap_cb_root(hp, (HeapCallback)UI_HeapAddChildren, ui);
  }

  *hit = CURRENT_RECORD(ui);
  return INDEXREAD_OK;
}

/**
Skip to the given docId, or one place after it
@param ctx IndexReader context
@param docId docId to seek to
@param hit an index hit we put our reads into
@return INDEXREAD_OK if found, INDEXREAD_NOTFOUND if not found, INDEXREAD_EOF
if
at EOF
*/
static int UI_SkipTo(void *ctx, t_docId docId, RSIndexResult **hit) {
  UnionIterator *ui = ctx;

  if (docId == 0) {
    return UI_ReadSorted(ctx, hit);
  }

  if (!IITER_HAS_NEXT(&ui->base)) {
    return INDEXREAD_EOF;
  }

  // reset the current hitf
  IndexResult_ResetAggregate(CURRENT_RECORD(ui));
  CURRENT_RECORD(ui)->weight = ui->weight;
  int numActive = 0;
  int found = 0;
  int rc = INDEXREAD_EOF;
  unsigned num = ui->num;
  const int quickExit = ui->quickExit;
  t_docId minDocId = UINT32_MAX;
  IndexIterator *it;
  RSIndexResult *res;
  RSIndexResult *minResult = NULL;
  // skip all iterators to docId
  for (unsigned i = 0; i < num; i++) {
    it = ui->its[i];
    // this happens for non existent words
    res = NULL;
    // If the requested docId is larger than the last read id from the iterator,
    // we need to read an entry from the iterator, seeking to this docId
    if (it->minId < docId) {
      if ((rc = it->SkipTo(it->ctx, docId, &res)) == INDEXREAD_EOF) {
        i = UI_RemoveExhausted(ui, i);
        num = ui->num;
        continue;
      }
      if (res) {
        it->minId = res->docId;
      }
    } else {
      // if the iterator is ahead of docId - we avoid reading the entry
      // in this case, we are either past or at the requested docId, no need to actually read
      rc = (it->minId == docId) ? INDEXREAD_OK : INDEXREAD_NOTFOUND;
      res = IITER_CURRENT_RECORD(it);
    }

    // if we've read successfully, update the minimal docId we've found
    if (it->minId && rc != INDEXREAD_EOF) {
      if (it->minId < minDocId || !minResult) {
        minResult = res;
        minDocId = it->minId;
      }
      // sminDocId = MIN(ui->docIds[i], minDocId);
    }

    // we found a hit - continue to all results matching the same docId
    if (rc == INDEXREAD_OK) {

      // add the result to the aggregate result we are holding
      if (hit) {
        AggregateResult_AddChild(CURRENT_RECORD(ui), res ? res : IITER_CURRENT_RECORD(it));
      }
      ui->minDocId = it->minId;
      ++found;
    }
    ++numActive;
    // If we've found a single entry and we are iterating in quick exit mode - exit now
    if (found && quickExit) break;
  }

  // all iterators are at the end
  if (numActive == 0) {
    IITER_SET_EOF(&ui->base);
    return INDEXREAD_EOF;
  }

  // copy our aggregate to the upstream hit
  *hit = CURRENT_RECORD(ui);
  if (found > 0) {
    return INDEXREAD_OK;
  }
  if (minResult) {
    *hit = minResult;
    AggregateResult_AddChild(CURRENT_RECORD(ui), minResult);
  }
  // not found...
  ui->minDocId = minDocId;
  return INDEXREAD_NOTFOUND;
}

// UI_SkipTo for iterator with high count of children
static int UI_SkipToHigh(void *ctx, t_docId docId, RSIndexResult **hit) {
  UnionIterator *ui = ctx;

  if (docId == 0) {
    return UI_ReadSorted(ctx, hit);
  }

  if (!IITER_HAS_NEXT(&ui->base)) {
    return INDEXREAD_EOF;
  }

  IndexResult_ResetAggregate(CURRENT_RECORD(ui));
  CURRENT_RECORD(ui)->weight = ui->weight;
  int rc = INDEXREAD_EOF;
  IndexIterator *it = NULL;
  RSIndexResult *res;
  heap_t *hp = ui->heapMinId;

  while (heap_count(hp)) {
    it = heap_peek(hp);
    if (it->minId >= docId) {
      // if the iterator is at or ahead of docId - we avoid reading the entry
      // in this case, we are either past or at the requested docId, no need to actually read
      break;
    }

    rc = it->SkipTo(it->ctx, docId, &res);
    if (rc == INDEXREAD_EOF) {
      heap_poll(hp); // return value was already received from heap_peak
      // iterator is not returned to heap
      continue;
    }
    RS_LOG_ASSERT(res, "should not be NULL");

    // refresh heap with iterator with updated minId
    it->minId = res->docId;
    heap_replace(hp, it);
    if (ui->quickExit && it->minId == docId) {
      break;
    }
  }

  if (heap_count(hp) == 0) {
    IITER_SET_EOF(&ui->base);
    return INDEXREAD_EOF;
  }

  rc = (it->minId == docId) ? INDEXREAD_OK : INDEXREAD_NOTFOUND;

  // On quickExit we just return one result.
  // Otherwise, we collect all the results that equal to the root of the heap.
  if (ui->quickExit) {
    AggregateResult_AddChild(CURRENT_RECORD(ui), IITER_CURRENT_RECORD(it));
  } else {
    heap_cb_root(hp, (HeapCallback)UI_HeapAddChildren, ui);
  }

  ui->minDocId = it->minId;
  *hit = CURRENT_RECORD(ui);
  return rc;
}

void UnionIterator_Free(IndexIterator *itbase) {
  if (itbase == NULL) return;

  UnionIterator *ui = itbase->ctx;
  for (int i = 0; i < ui->norig; i++) {
    IndexIterator *it = ui->origits[i];
    if (it) {
      it->Free(it);
    }
  }

  IndexResult_Free(CURRENT_RECORD(ui));
  if (ui->heapMinId) heap_free(ui->heapMinId);
  rm_free(ui->its);
  rm_free(ui->origits);
  rm_free(ui);
}

static size_t UI_Len(void *ctx) {
  return ((UnionIterator *)ctx)->len;
}

void trimUnionIterator(IndexIterator *iter, size_t offset, size_t limit, bool asc) {
  RS_LOG_ASSERT(iter->type == UNION_ITERATOR, "trim applies to union iterators only");
  UnionIterator *ui = (UnionIterator *)iter;
  if (ui->norig <= 2) { // nothing to trim
    return;
  }

  size_t curTotal = 0;
  int i;
  if (offset == 0) {
    if (asc) {
      for (i = 1; i < ui->num; ++i) {
        IndexIterator *it = ui->origits[i];
        curTotal += it->NumEstimated(it->ctx);
        if (curTotal > limit) {
          ui->num = i + 1;
          memset(ui->its + ui->num, 0, ui->norig - ui->num);
          break;
        }
      }
    } else {  //desc
      for (i = ui->num - 2; i > 0; --i) {
        IndexIterator *it = ui->origits[i];
        curTotal += it->NumEstimated(it->ctx);
        if (curTotal > limit) {
          ui->num -= i;
          memmove(ui->its, ui->its + i, ui->num);
          memset(ui->its + ui->num, 0, ui->norig - ui->num);
          break;
        }
      }
    }
  } else {
    UI_SyncIterList(ui);
  }
  iter->Read = UI_ReadUnsorted;
}

/* The context used by the intersection methods during iterating an intersect
 * iterator */
typedef struct {
  IndexIterator base;
  IndexIterator **its;
  t_docId *docIds;
  int *rcs;
  unsigned num;
  size_t len;
  int maxSlop;
  int inOrder;
  // the last read docId from any child
  t_docId lastDocId;
  // the last id that was found on all children
  t_docId lastFoundId;

  // RSIndexResult *result;
  DocTable *docTable;
  t_fieldMask fieldMask;
  double weight;
  size_t nexpected;
} IntersectIterator;

void IntersectIterator_Free(IndexIterator *it) {
  if (it == NULL) return;
  IntersectIterator *ui = it->ctx;
  for (int i = 0; i < ui->num; i++) {
    if (ui->its[i] != NULL) {
      ui->its[i]->Free(ui->its[i]);
    }
  }

  rm_free(ui->docIds);
  rm_free(ui->its);
  IndexResult_Free(it->current);
  rm_free(it);
}

static void II_Abort(void *ctx) {
  IntersectIterator *it = ctx;
  it->base.isValid = 0;
  for (int i = 0; i < it->num; i++) {
    if (it->its[i]) {
      it->its[i]->Abort(it->its[i]->ctx);
    }
  }
}

static void II_Rewind(void *ctx) {
  IntersectIterator *ii = ctx;
  ii->base.isValid = 1;
  ii->lastDocId = 0;

  // rewind all child iterators
  for (int i = 0; i < ii->num; i++) {
    ii->docIds[i] = 0;
    if (ii->its[i]) {
      ii->its[i]->Rewind(ii->its[i]->ctx);
    }
  }
}

typedef int (*CompareFunc)(const void *a, const void *b);
static int cmpIter(IndexIterator **it1, IndexIterator **it2) {
  if (!*it1 && !*it2) return 0;
  if (!*it1) return -1;
  if (!*it2) return 1;

  double factor1 = 1;
  double factor2 = 1;
  enum IteratorType it_1_type = (*it1)->type;
  enum IteratorType it_2_type = (*it2)->type;

  /*
   * on INTERSECT iterator, we divide the estimate by the number of children
   * since we skip as soon as a number is not in all iterators */
  if (it_1_type == INTERSECT_ITERATOR) {
    factor1 = 1 / MAX(1, ((IntersectIterator *)*it1)->num);
  } else if (it_1_type == UNION_ITERATOR && RSGlobalConfig.prioritizeIntersectUnionChildren) {
    factor1 = ((UnionIterator *)*it1)->num;
  }
  if (it_2_type == INTERSECT_ITERATOR) {
    factor2 = 1 / MAX(1, ((IntersectIterator *)*it2)->num);
  } else if (it_2_type == UNION_ITERATOR && RSGlobalConfig.prioritizeIntersectUnionChildren) {
    factor2 = ((UnionIterator *)*it2)->num;
  }

  return (int)((*it1)->NumEstimated((*it1)->ctx) * factor1 - (*it2)->NumEstimated((*it2)->ctx) * factor2);
}

static void II_SortChildren(IntersectIterator *ctx) {
  /**
   * 1. Go through all the iterators, ensuring none of them is NULL
   *    (replace with empty if indeed NULL)
   */
  IndexIterator **its = rm_malloc(sizeof(IndexIterator *) * ctx->num);
  size_t itsSize = 0;
  for (size_t i = 0; i < ctx->num; ++i) {
    IndexIterator *curit = ctx->its[i];

    if (!curit) {
      // If the current iterator is empty, then the entire
      // query will fail; just free all the iterators and call it good
      if (its) {
        rm_free(its);
      }
      ctx->nexpected = IITER_INVALID_NUM_ESTIMATED_RESULTS;
      return;
    }

    size_t amount = IITER_NUM_ESTIMATED(curit);
    if (amount < ctx->nexpected) {
      ctx->nexpected = amount;
    }

    its[itsSize++] = curit;
  }

  rm_free(ctx->its);
  ctx->its = its;
  ctx->num = itsSize;
}

void AddIntersectIterator(IndexIterator *parentIter, IndexIterator *childIter) {
  RS_LOG_ASSERT(parentIter->type == INTERSECT_ITERATOR, "add applies to intersect iterators only");
  IntersectIterator *ii = (IntersectIterator *)parentIter;
  ii->num++;
  ii->its = rm_realloc(ii->its, ii->num);
  ii->its[ii->num - 1] = childIter;
}

IndexIterator *NewIntersectIterator(IndexIterator **its_, size_t num, DocTable *dt,
                                   t_fieldMask fieldMask, int maxSlop, int inOrder, double weight) {
  // printf("Creating new intersection iterator with fieldMask=%llx\n", fieldMask);
  IntersectIterator *ctx = rm_calloc(1, sizeof(*ctx));
  ctx->lastDocId = 0;
  ctx->lastFoundId = 0;
  ctx->len = 0;
  ctx->maxSlop = maxSlop;
  ctx->inOrder = inOrder;
  ctx->fieldMask = fieldMask;
  ctx->weight = weight;
  ctx->docIds = rm_calloc(num, sizeof(t_docId));
  ctx->docTable = dt;
  ctx->nexpected = IITER_INVALID_NUM_ESTIMATED_RESULTS;

  ctx->base.isValid = 1;
  ctx->base.current = NewIntersectResult(num, weight);
  ctx->its = its_;
  ctx->num = num;

  // Sort children iterators from low count to high count which reduces the number of iterations.
  if (!ctx->inOrder) {
    qsort(ctx->its, ctx->num, sizeof(*ctx->its), (CompareFunc)cmpIter);
  }

  // bind the iterator calls
  IndexIterator *it = &ctx->base;
  it->ctx = ctx;
  it->type = INTERSECT_ITERATOR;
  it->LastDocId = II_LastDocId;
  it->NumEstimated = II_NumEstimated;
  it->Read = II_ReadSorted;
  it->SkipTo = II_SkipTo;
  it->Len = II_Len;
  it->Free = IntersectIterator_Free;
  it->Abort = II_Abort;
  it->Rewind = II_Rewind;
  it->HasNext = NULL;
  II_SortChildren(ctx);
  return it;
}

static int II_SkipTo(void *ctx, t_docId docId, RSIndexResult **hit) {
  /* A seek with docId 0 is equivalent to a read */
  if (docId == 0) {
    return II_ReadSorted(ctx, hit);
  }
  IntersectIterator *ic = ctx;
  IndexResult_ResetAggregate(ic->base.current);
  int nfound = 0;

  int rc = INDEXREAD_EOF;
  // skip all iterators to docId
  for (int i = 0; i < ic->num; i++) {
    IndexIterator *it = ic->its[i];

    if (!it || !IITER_HAS_NEXT(it)) return INDEXREAD_EOF;

    RSIndexResult *res = IITER_CURRENT_RECORD(it);
    rc = INDEXREAD_OK;

    // only read if we are not already at the seek to position
    if (ic->docIds[i] != docId) {
      rc = it->SkipTo(it->ctx, docId, &res);
      if (rc != INDEXREAD_EOF) {
        if (res) docId = ic->docIds[i] = res->docId;
      }
    }

    if (rc == INDEXREAD_EOF) {
      // we are at the end!
      ic->base.isValid = 0;
      return rc;
    } else if (rc == INDEXREAD_OK) {
      // YAY! found!
      if (res && res->docId == docId) {
        AggregateResult_AddChild(ic->base.current, res);
      }
      ic->lastDocId = docId;

      ++nfound;
    } else if (ic->docIds[i] > ic->lastDocId) {
      ic->lastDocId = ic->docIds[i];
      break;
    }
  }

  // unless we got an EOF - we put the current record into hit

  // if the requested id was found on all children - we return OK
  if (nfound == ic->num) {
    // printf("Skipto %d hit @%d\n", docId, ic->current->docId);

    // Update the last found id
    // if maxSlop == -1 there is no need to verify maxSlop and inorder, otherwise lets verify
    if (ic->maxSlop == -1 ||
        IndexResult_IsWithinRange(ic->base.current, ic->maxSlop, ic->inOrder)) {
      ic->lastFoundId = ic->base.current->docId;
      ic->lastDocId++;
      if (hit) *hit = ic->base.current;
      return INDEXREAD_OK;
    }
  }

  // Not found - but we need to read the next valid result into hit
  rc = II_ReadSorted(ic, hit);
  // this might have brought us to our end, in which case we just terminate
  if (rc == INDEXREAD_EOF) return INDEXREAD_EOF;

  // otherwise - not found
  return INDEXREAD_NOTFOUND;
}

static size_t II_NumEstimated(void *ctx) {
  IntersectIterator *ic = ctx;
  return ic->nexpected;
}

static int II_ReadSorted(void *ctx, RSIndexResult **hit) {
  IntersectIterator *ic = ctx;
  if (ic->num == 0) return INDEXREAD_EOF;

  int nh = 0;
  int i = 0;

  do {
    nh = 0;
    IndexResult_ResetAggregate(ic->base.current);

    for (i = 0; i < ic->num; i++) {
      IndexIterator *it = ic->its[i];

      if (!it) goto eof;

      RSIndexResult *h = IITER_CURRENT_RECORD(it);
      // skip to the next
      int rc = INDEXREAD_OK;
      if (ic->docIds[i] != ic->lastDocId || ic->lastDocId == 0) {

        if (i == 0 && ic->docIds[i] >= ic->lastDocId) {
          rc = it->Read(it->ctx, &h);
        } else {
          rc = it->SkipTo(it->ctx, ic->lastDocId, &h);
        }
        // printf("II %p last docId %d, it %d read docId %d(%d), rc %d\n", ic, ic->lastDocId, i,
        //        h->docId, it->LastDocId(it->ctx), rc);

        if (rc == INDEXREAD_EOF) goto eof;
        ic->docIds[i] = h->docId;
      }

      if (ic->docIds[i] > ic->lastDocId) {
        ic->lastDocId = ic->docIds[i];
        break;
      }
      if (rc == INDEXREAD_OK) {
        ++nh;
        AggregateResult_AddChild(ic->base.current, h);
      } else {
        ic->lastDocId++;
      }
    }

    if (nh == ic->num) {
      // printf("II %p HIT @ %d\n", ic, ic->current->docId);
      // sum up all hits
      if (hit != NULL) {
        *hit = ic->base.current;
      }
      // Update the last valid found id
      ic->lastFoundId = ic->base.current->docId;

      // advance the doc id so next time we'll read a new record
      ic->lastDocId++;

      // // make sure the flags are matching.
      if ((ic->base.current->fieldMask & ic->fieldMask) == 0) {
        // printf("Field masks don't match!\n");
        continue;
      }

      // If we need to match slop and order, we do it now, and possibly skip the result
      if (ic->maxSlop >= 0) {
        // printf("Checking SLOP... (%d)\n", ic->maxSlop);
        if (!IndexResult_IsWithinRange(ic->base.current, ic->maxSlop, ic->inOrder)) {
          // printf("Not within range!\n");
          continue;
        }
      }

      //      for(size_t i = 0 ; i < array_len(ic->testers) ; ++i){
      //        if(!ic->testers[i]->TextCriteria(ic->testers[i]->ctx, ic->lastFoundId)){
      //          continue;
      //        }
      //      }
      ic->len++;
      // printf("Returning OK\n");
      return INDEXREAD_OK;
    }
  } while (1);
eof:
  ic->base.isValid = 0;
  return INDEXREAD_EOF;
}

static t_docId II_LastDocId(void *ctx) {
  // return last FOUND id, not last read id form any child
  return ((IntersectIterator *)ctx)->lastFoundId;
}

static size_t II_Len(void *ctx) {
  return ((IntersectIterator *)ctx)->len;
}

/* A Not iterator works by wrapping another iterator, and returning OK for
 * misses, and NOTFOUND for hits. It takes its reference from a wildcard iterator
 * if `INDEXALL` is on (optimization). */
typedef struct {
  IndexIterator base;         // base index iterator
  IndexIterator *wcii;        // wildcard index iterator
  IndexIterator *child;       // child index iterator
  t_docId lastDocId;
  t_docId maxDocId;
  size_t len;
  double weight;
  TimeoutCtx timeoutCtx;
} NotIterator, NotContext;

static void NI_Abort(void *ctx) {
  NotContext *nc = ctx;
  nc->base.isValid = 0;
  if (nc->wcii) {
    nc->wcii->Abort(nc->wcii->ctx);
  }
  nc->child->Abort(nc->child->ctx);
}

static void NI_Rewind(void *ctx) {
  NotContext *nc = ctx;
  nc->lastDocId = 0;
  if (nc->wcii) {
    nc->wcii->Rewind(nc->wcii->ctx);
  }
  nc->base.current->docId = 0;
  nc->base.isValid = 1;
  nc->child->Rewind(nc->child->ctx);
}

static void NI_Free(IndexIterator *it) {
  NotContext *nc = it->ctx;
  nc->child->Free(nc->child);
  if (nc->wcii) {
    nc->wcii->Free(nc->wcii);
  }
  IndexResult_Free(nc->base.current);
  rm_free(it);
}

/* SkipTo for NOT iterator - Non-optimized version. If we have a match - return
 * NOTFOUND. If we don't or we're at the end - return OK */
static int NI_SkipTo_NO(void *ctx, t_docId docId, RSIndexResult **hit) {
  NotContext *nc = ctx;

  // do not skip beyond max doc id
  if (docId > nc->maxDocId) {
    IITER_SET_EOF(&nc->base);
    return INDEXREAD_EOF;
  }

  // Get the child's last read docId
  // if lastDocId is 0, Read & Skipto weren't called yet and child lastId
  // might not be be updated (ex. NUMERIC filter) (PR-2440)
  t_docId childId = 0;
  if (nc->lastDocId != 0) {
    childId = nc->child->LastDocId(nc->child->ctx);
  }

  // If the child is ahead of the skipto id, it means the child doesn't have this id.
  // So we are okay!
  if (childId > docId || !IITER_HAS_NEXT(nc->child)) {
    goto ok;
  }

  // If the child docId is the one we are looking for, it's an anti match!
  if (childId == docId) {
    nc->base.current->docId = nc->lastDocId = docId;
    *hit = nc->base.current;
    return INDEXREAD_NOTFOUND;
  }

  // read the next entry from the child
  int rc = nc->child->SkipTo(nc->child->ctx, docId, hit);

  // OK means not found
  if (rc == INDEXREAD_OK) {
    return INDEXREAD_NOTFOUND;
  }

ok:
  // NOT FOUND or end means OK. We need to set the docId to the hit we will bubble up
  nc->base.current->docId = nc->lastDocId = docId;
  *hit = nc->base.current;
  return INDEXREAD_OK;
}

/* SkipTo for NOT iterator - Optimized version. If we have a match - return
 * NOTFOUND. If we don't or we're at the end - return OK */
int NI_SkipTo_O(void *ctx, t_docId docId, RSIndexResult **hit) {
  NotContext *nc = ctx;

  // do not skip beyond max doc id
  if (docId > nc->maxDocId) {
    IITER_SET_EOF(nc->wcii);
    IITER_SET_EOF(&nc->base);
    return INDEXREAD_EOF;
  }

  // Get the child's last read docId
  // if lastDocId is 0, Read & Skipto weren't called yet and child lastId
  // might not be be updated (ex. NUMERIC filter) (PR-2440)
  t_docId childId = 0;
  if (nc->lastDocId != 0) {
    childId = nc->child->LastDocId(nc->child->ctx);
  }

  // If the child is ahead of the skipto id, it means the child doesn't have this id.
  // So we are okay!
  if (childId > docId || !IITER_HAS_NEXT(nc->child)) {
    goto ok;
  }

  // If the child docId is the one we are looking for, it's an anti match!
  int wcii_rc;
  if (childId == docId) {
    // Skip the inner wildcard to `docId`, and return NOTFOUND
    wcii_rc = nc->wcii->SkipTo(nc->wcii->ctx, docId, hit);
    if (wcii_rc == INDEXREAD_EOF) {
      IITER_SET_EOF(&nc->base);
    }
    // Note: If this is the last block in the child index and not in the wildcard
    // index, we may have a docId in the child that does not exist in the
    // wildcard index
    nc->base.current->docId = nc->lastDocId = nc->wcii->LastDocId(nc->wcii->ctx);
    *hit = nc->base.current;
    return INDEXREAD_NOTFOUND;
  }

  // read the next entry from the child
  int rc = nc->child->SkipTo(nc->child->ctx, docId, hit);

  // OK means not found
  if (rc == INDEXREAD_OK) {
    return INDEXREAD_NOTFOUND;
  }

ok:
  // NOT FOUND or end at child means OK. We need to set the docId to the hit we
  // will bubble up
  wcii_rc = nc->wcii->SkipTo(nc->wcii->ctx, docId, hit);
  nc->base.current->docId = nc->lastDocId = nc->wcii->LastDocId(nc->wcii->ctx);
  if (wcii_rc == INDEXREAD_EOF) {
    IITER_SET_EOF(&nc->base);
    return INDEXREAD_EOF;
  } else if (wcii_rc == INDEXREAD_NOTFOUND) {
    // This doc-id was deleted
    return INDEXREAD_NOTFOUND;
  }
  RS_LOG_ASSERT_FMT(nc->lastDocId == docId, "Expected docId to be %zu, got %zu", docId, nc->lastDocId);
  return INDEXREAD_OK;
}

static size_t NI_NumEstimated(void *ctx) {
  NotContext *nc = ctx;
  return nc->maxDocId;
}

/* Read from a NOT iterator - Non-Optimized version. This is applicable only if
 * the only or leftmost node of a query is a NOT node. We simply read until max
 * docId, skipping docIds that exist in the child */
static int NI_ReadSorted_NO(void *ctx, RSIndexResult **hit) {
  NotContext *nc = ctx;
  if (nc->lastDocId > nc->maxDocId) {
    IITER_SET_EOF(&nc->base);
    return INDEXREAD_EOF;
  }

  RSIndexResult *cr = NULL;
  // if we have a child, get the latest result from the child
  cr = IITER_CURRENT_RECORD(nc->child);

  if (cr == NULL || cr->docId == 0) {
    nc->child->Read(nc->child->ctx, &cr);
  }

  // advance our reader by one, and let's test if it's a valid value or not
  nc->base.current->docId++;

  // If we don't have a child result, or the child result is ahead of the current counter,
  // we just increment our virtual result's id until we hit the child result's
  // in which case we'll read from the child and bypass it by one.
  if (cr == NULL || cr->docId > nc->base.current->docId || !IITER_HAS_NEXT(nc->child)) {
    goto ok;
  }

  while (cr->docId == nc->base.current->docId) {
    // advance our docId to the next possible id
    nc->base.current->docId++;

    // read the next entry from the child
    if (nc->child->Read(nc->child->ctx, &cr) == INDEXREAD_EOF) {
      break;
    }

    // Check for timeout with low granularity (MOD-5512)
    if (TimedOut_WithCtx_Gran(&nc->timeoutCtx, 5000)) {
      IITER_SET_EOF(&nc->base);
      return INDEXREAD_TIMEOUT;
    }
  }
  nc->timeoutCtx.counter = 0;

ok:
  // make sure we did not overflow
  if (nc->base.current->docId > nc->maxDocId) {
    IITER_SET_EOF(&nc->base);
    return INDEXREAD_EOF;
  }

  // Set the next entry and return ok
  nc->lastDocId = nc->base.current->docId;
  if (hit) *hit = nc->base.current;
  ++nc->len;

  return INDEXREAD_OK;
}

/* Read from a NOT iterator - Optimized version, utilizing the `existing docs`
 * inverted index. This is applicable only if the only or leftmost node of a
 * query is a NOT node. We simply read until max docId, skipping docIds that
 * exist in the child */
static int NI_ReadSorted_O(void *ctx, RSIndexResult **hit) {
  NotContext *nc = ctx;
  RSIndexResult *cr = NULL;
  int wcii_rc;
  int child_rc = INDEXREAD_OK;

  if (nc->lastDocId > nc->maxDocId) {
    IITER_SET_EOF(&nc->base);
    return INDEXREAD_EOF;
  }

  // if we have a child, get the latest result from the child
  cr = IITER_CURRENT_RECORD(nc->child);

  if (cr == NULL || cr->docId == 0) {
    nc->child->Read(nc->child->ctx, &cr);
  }

  // Advance the embedded wildcard iterator
  RSIndexResult *wcii_res = NULL;
  wcii_rc = nc->wcii->Read(nc->wcii->ctx, &wcii_res);

  if (wcii_rc == INDEXREAD_EOF) {
    // If the wildcard iterator hit EOF, we're done
    IITER_SET_EOF(&nc->base);
    return INDEXREAD_EOF;
  }
  nc->base.current->docId = wcii_res->docId;

  // If there is no child result, or the child result is ahead of the wildcard
  // iterator result, we wish to return the current docId.
  if (cr == NULL || cr->docId > wcii_res->docId || !IITER_HAS_NEXT(nc->child)) {
    goto ok;
  }

  while (cr->docId == wcii_res->docId && child_rc != INDEXREAD_EOF) {
    wcii_rc = nc->wcii->Read(nc->wcii->ctx, &wcii_res);
    nc->base.current->docId = wcii_res->docId;

    if (wcii_rc == INDEXREAD_EOF) {
      // No more valid docs --> Done.
      IITER_SET_EOF(&nc->base);
      return INDEXREAD_EOF;
    }

    // read next entry from child
    // If the child docId is smaller than the wildcard docId, it was cleaned from
    // the `existingDocs` inverted index but not yet from child -> skip it.
    do {
      child_rc = nc->child->Read(nc->child->ctx, &cr);
    } while (child_rc != INDEXREAD_EOF && cr->docId < wcii_res->docId);

    // Check for timeout
    if (TimedOut_WithCtx_Gran(&nc->timeoutCtx, 5000)) {
      IITER_SET_EOF(nc->wcii);
      IITER_SET_EOF(&nc->base);
      return INDEXREAD_TIMEOUT;
    }
  }
  nc->timeoutCtx.counter = 0;

ok:
  // Set the next entry and return ok
  nc->lastDocId = nc->base.current->docId;
  if (hit) *hit = nc->base.current;
  ++nc->len;

  return INDEXREAD_OK;
}

/* We always have next, in case anyone asks... ;) */
static int NI_HasNext(void *ctx) {
  NotContext *nc = ctx;
  return nc->lastDocId <= nc->maxDocId;
}

/* Our len is the child's len? TBD it might be better to just return 0 */
static size_t NI_Len(void *ctx) {
  NotContext *nc = ctx;
  return nc->len;
}

/* Last docId */
static t_docId NI_LastDocId(void *ctx) {
  NotContext *nc = ctx;
  return nc->lastDocId;
}

IndexIterator *NewNotIterator(IndexIterator *it, t_docId maxDocId,
  double weight, struct timespec timeout, QueryEvalCtx *q) {

  NotContext *nc = rm_calloc(1, sizeof(*nc));
  bool optimized = q && q->sctx->spec->rule && q->sctx->spec->rule->index_all;
  if (optimized) {
    nc->wcii = NewWildcardIterator(q);
  }
  nc->base.current = NewVirtualResult(weight, RS_FIELDMASK_ALL);
  nc->base.current->docId = 0;
  nc->base.isValid = 1;
  IndexIterator *ret = &nc->base;

  nc->child = it ? it : NewEmptyIterator();
  nc->lastDocId = 0;
  nc->maxDocId = maxDocId;          // Valid for the optimized case as well, since this is the maxDocId of the embedded wildcard iterator
  nc->len = 0;
  nc->weight = weight;
  nc->timeoutCtx = (TimeoutCtx){ .timeout = timeout, .counter = 0 };

  ret->ctx = nc;
  ret->type = NOT_ITERATOR;
  ret->NumEstimated = NI_NumEstimated;
  ret->Free = NI_Free;
  ret->HasNext = NI_HasNext;
  ret->LastDocId = NI_LastDocId;
  ret->Len = NI_Len;
  ret->Read = optimized ? NI_ReadSorted_O : NI_ReadSorted_NO;
  ret->SkipTo = optimized ? NI_SkipTo_O : NI_SkipTo_NO;
  ret->Abort = NI_Abort;
  ret->Rewind = NI_Rewind;

  return ret;
}

// LCOV_EXCL_START
IndexIterator* _New_NotIterator_With_WildCardIterator(IndexIterator *child, IndexIterator *wcii, t_docId maxDocId, double weight, struct timespec timeout) {
  NotContext *nc = rm_calloc(1, sizeof(*nc));
  nc->child = child;
  nc->wcii = wcii;
  nc->base.current = NewVirtualResult(weight, RS_FIELDMASK_ALL);
  nc->base.current->docId = 0;
  nc->base.isValid = 1;
  IndexIterator *ret = &nc->base;

  nc->lastDocId = 0;
  nc->maxDocId = maxDocId;          // Valid for the optimized case as well, since this is the maxDocId of the embedded wildcard iterator
  nc->len = 0;
  nc->weight = weight;
  nc->timeoutCtx = (TimeoutCtx){ .timeout = timeout, .counter = 0 };

  ret->ctx = nc;
  ret->type = NOT_ITERATOR;
  ret->NumEstimated = NI_NumEstimated;
  ret->Free = NI_Free;
  ret->HasNext = NI_HasNext;
  ret->LastDocId = NI_LastDocId;
  ret->Len = NI_Len;
  ret->Read = NI_ReadSorted_O;
  ret->SkipTo = NI_SkipTo_O;
  ret->Abort = NI_Abort;
  ret->Rewind = NI_Rewind;

  return ret;
}
// LCOV_EXCL_STOP

/**********************************************************
 * Optional clause iterator
 **********************************************************/

typedef struct {
  IndexIterator base;     // base index iterator
  IndexIterator *wcii;    // wildcard index iterator
  IndexIterator *child;   // child index iterator
  RSIndexResult *virt;
  t_fieldMask fieldMask;
  t_docId lastDocId;
  t_docId maxDocId;
  t_docId nextRealId;
  double weight;
} OptionalMatchContext, OptionalIterator;

static void OI_Abort(void *ctx) {
  OptionalMatchContext *nc = ctx;
  if (nc->wcii) {
    nc->wcii->Abort(nc->wcii->ctx);
  }
  if (nc->child) {
    nc->child->Abort(nc->child->ctx);
  }
}

static void OI_Rewind(void *ctx) {
  OptionalMatchContext *nc = ctx;
  nc->lastDocId = 0;
  if (nc->wcii) {
    nc->wcii->Rewind(nc->wcii->ctx);
  }
  nc->virt->docId = 0;
  nc->nextRealId = 0;
  if (nc->child) {
    nc->child->Rewind(nc->child->ctx);
  }
}

static void OI_Free(IndexIterator *it) {
  OptionalMatchContext *nc = it->ctx;
  if (nc->child) {
    nc->child->Free(nc->child);
  }
  if (nc->wcii) {
    nc->wcii->Free(nc->wcii);
  }
  IndexResult_Free(nc->virt);
  rm_free(it);
}

// SkipTo for OPTIONAL iterator - Non-optimized version.
static int OI_SkipTo_NO(void *ctx, t_docId docId, RSIndexResult **hit) {
  OptionalMatchContext *nc = ctx;
  //  printf("OI_SkipTo => %llu!. NextReal: %llu. Max: %llu. Last: %llu\n", docId, nc->nextRealId,
  //  nc->maxDocId, nc->lastDocId);

  bool found = false;

  // Set the current ID
  nc->lastDocId = docId;

  if (nc->lastDocId > nc->maxDocId) {
    IITER_SET_EOF(&nc->base);
    return INDEXREAD_EOF;
  }

  if (docId == 0) {
    // No doc was read yet - read the first doc
    return nc->base.Read(ctx, hit);
  }

  if (docId == nc->nextRealId) {
    // Edge case -- match on the docid we just looked for
    found = true;
    // reset current pointer since this might have been a prior
    // virt return
    nc->base.current = nc->child->current;

  } else if (docId > nc->nextRealId) {
    int rc = nc->child->SkipTo(nc->child->ctx, docId, &nc->base.current);
    if (rc == INDEXREAD_OK) {
      found = true;
    }
    if (nc->base.current) {
      nc->nextRealId = nc->base.current->docId;
    }
  }

  if (found) {
    // Has a real hit on the child iterator
    nc->base.current->weight = nc->weight;
  } else {
    nc->virt->docId = docId;
    nc->virt->weight = 0;
    nc->base.current = nc->virt;
  }

  *hit = nc->base.current;
  return INDEXREAD_OK;
}

// SkipTo for OPTIONAL iterator - Optimized version.
static int OI_SkipTo_O(void *ctx, t_docId docId, RSIndexResult **hit) {
  OptionalMatchContext *nc = ctx;

  bool found = false;

  if (nc->lastDocId > nc->maxDocId) {
    IITER_SET_EOF(nc->wcii);
    IITER_SET_EOF(&nc->base);
    return INDEXREAD_EOF;
  }

  if (docId == 0) {
    // No doc was read yet - read the first doc
    return nc->base.Read(ctx, hit);
  }

  int rc;

  if (docId == nc->nextRealId) {
    found = true;
    nc->base.current = nc->child->current;
  } else if (docId > nc->nextRealId) {
    rc = nc->child->SkipTo(nc->child->ctx, docId, &nc->base.current);
    if (rc == INDEXREAD_OK) {
      found = true;
    }
    if (nc->base.current) {
      nc->nextRealId = nc->base.current->docId;
    }
  }

  // Promote the wildcard iterator to the requested docId if the docId
  RSIndexResult *wcii_res = NULL;
  if (docId > nc->wcii->LastDocId(nc->wcii->ctx)) {
    rc = nc->wcii->SkipTo(nc->wcii->ctx, docId, &wcii_res);
    if (rc != INDEXREAD_OK) {
      if (rc != INDEXREAD_NOTFOUND) {
        // EOF or timeout, set invalid
        IITER_SET_EOF(&nc->base);
      }
      return rc;
    }
  }

  if (found) {
    // Has a real hit on the child iterator
    nc->base.current->weight = nc->weight;
  } else {
    nc->virt->docId = nc->lastDocId = docId;
    nc->virt->weight = 0;
    nc->base.current = nc->virt;
  }

  *hit = nc->base.current;
  return INDEXREAD_OK;
}

static size_t OI_NumEstimated(void *ctx) {
  OptionalMatchContext *nc = ctx;
  return nc->maxDocId;
}

// Read from an OPTIONAL iterator - Non-Optimized version.
static int OI_ReadSorted_NO(void *ctx, RSIndexResult **hit) {
  OptionalMatchContext *nc = ctx;
  if (nc->lastDocId >= nc->maxDocId) {
    return INDEXREAD_EOF;
  }

  // Increase the size by one
  nc->lastDocId++;

  if (nc->lastDocId > nc->nextRealId) {
    int rc = nc->child->Read(nc->child->ctx, &nc->base.current);
    if (rc == INDEXREAD_EOF) {
      nc->nextRealId = nc->maxDocId + 1;
    } else if (rc == INDEXREAD_TIMEOUT) {
      return rc;
    } else {
      nc->nextRealId = nc->base.current->docId;
    }
  }

  if (nc->lastDocId != nc->nextRealId) {
    nc->base.current = nc->virt;
    nc->base.current->weight = 0;
  } else {
    nc->base.current = nc->child->current;
    nc->base.current->weight = nc->weight;
  }

  nc->base.current->docId = nc->lastDocId;
  *hit = nc->base.current;
  return INDEXREAD_OK;
}

// Read from optional iterator - Optimized version, utilizing the `existing docs`
// inverted index.
static int OI_ReadSorted_O(void *ctx, RSIndexResult **hit) {
  OptionalMatchContext *nc = ctx;
  if (nc->lastDocId >= nc->maxDocId) {
    return INDEXREAD_EOF;
  }

  // Get the next docId
  RSIndexResult *wcii_res = NULL;
  int wcii_rc = nc->wcii->Read(nc->wcii->ctx, &wcii_res);
  if (wcii_rc != INDEXREAD_OK) {
    // EOF, set invalid
    IITER_SET_EOF(&nc->base);
    return wcii_rc;
  }

  int rc;
  // We loop over this condition, since it reflects that the index is not up to date.
  while (wcii_res->docId > nc->nextRealId) {
    rc = nc->child->Read(nc->child->ctx, &nc->base.current);
    if (rc == INDEXREAD_EOF) {
      nc->nextRealId = nc->maxDocId + 1;
    } else if (rc == INDEXREAD_TIMEOUT) {
      return rc;
    } else {
      nc->nextRealId = nc->base.current->docId;
    }
  }

  nc->lastDocId = nc->base.current->docId = wcii_res->docId;

  if (nc->lastDocId != nc->nextRealId) {
    nc->base.current = nc->virt;
    nc->base.current->weight = 0;
  } else {
    nc->base.current = nc->child->current;
    nc->base.current->weight = nc->weight;
  }

  nc->base.current->docId = nc->lastDocId;
  *hit = nc->base.current;
  return INDEXREAD_OK;
}

/* We always have next, in case anyone asks... ;) */
static int OI_HasNext(void *ctx) {
  OptionalMatchContext *nc = ctx;
  return (nc->lastDocId <= nc->maxDocId);
}

/* Our len is the child's len? TBD it might be better to just return 0 */
static size_t OI_Len(void *ctx) {
  OptionalMatchContext *nc = ctx;
  return nc->child ? nc->child->Len(nc->child->ctx) : 0;
}

/* Last docId */
static t_docId OI_LastDocId(void *ctx) {
  OptionalMatchContext *nc = ctx;
  return nc->lastDocId;
}

IndexIterator *NewOptionalIterator(IndexIterator *it, QueryEvalCtx *q, double weight) {
  OptionalMatchContext *nc = rm_calloc(1, sizeof(*nc));

  bool optimized = q && q->sctx->spec->rule && q->sctx->spec->rule->index_all;
  if (optimized) {
    nc->wcii = NewWildcardIterator(q);
  }
  nc->virt = NewVirtualResult(weight, RS_FIELDMASK_ALL);
  nc->virt->freq = 1;
  nc->base.current = nc->virt;
  nc->child = it ? it : NewEmptyIterator();
  nc->lastDocId = 0;
  nc->maxDocId = q->docTable->maxDocId;
  nc->weight = weight;
  nc->nextRealId = 0;

  IndexIterator *ret = &nc->base;
  ret->ctx = nc;
  ret->type = OPTIONAL_ITERATOR;
  ret->NumEstimated = OI_NumEstimated;
  ret->Free = OI_Free;
  ret->HasNext = OI_HasNext;
  ret->LastDocId = OI_LastDocId;
  ret->Len = OI_Len;
  ret->Read = optimized ? OI_ReadSorted_O : OI_ReadSorted_NO;
  ret->SkipTo = optimized ? OI_SkipTo_O : OI_SkipTo_NO;
  ret->Abort = OI_Abort;
  ret->Rewind = OI_Rewind;

  return ret;
}

/* Wildcard iterator, matching all documents in the database. */
typedef struct {
  IndexIterator base;
  t_docId topId;
  t_docId current;
  t_docId numDocs;
} WildcardIterator, WildcardIteratorCtx;

/* Free a wildcard iterator */
static void WI_Free(IndexIterator *it) {

  WildcardIteratorCtx *nc = it->ctx;
  IndexResult_Free(CURRENT_RECORD(nc));
  rm_free(it);
}

/* Read reads the next consecutive id, unless we're at the end */
static int WI_Read(void *ctx, RSIndexResult **hit) {
  WildcardIteratorCtx *nc = ctx;
  CURRENT_RECORD(nc)->docId = ++nc->current;
  if (nc->current > nc->topId) {
    return INDEXREAD_EOF;
  }
  if (hit) {
    *hit = CURRENT_RECORD(nc);
  }
  return INDEXREAD_OK;
}

/* Skipto for wildcard iterator - always succeeds, but this should normally not happen as it has
 * no
 * meaning */
static int WI_SkipTo(void *ctx, t_docId docId, RSIndexResult **hit) {
  // printf("WI_Skipto %d\n", docId);
  WildcardIteratorCtx *nc = ctx;

  if (nc->current > nc->topId) return INDEXREAD_EOF;

  if (docId == 0) return WI_Read(ctx, hit);

  nc->current = docId;
  CURRENT_RECORD(nc)->docId = docId;
  if (hit) {
    *hit = CURRENT_RECORD(nc);
  }
  return INDEXREAD_OK;
}

static void WI_Abort(void *ctx) {
  WildcardIteratorCtx *nc = ctx;
  nc->current = nc->topId + 1;
}

/* We always have next, in case anyone asks... ;) */
static int WI_HasNext(void *ctx) {
  WildcardIteratorCtx *nc = ctx;

  return nc->current <= nc->topId;
}

/* Our len is the len of the index... */
static size_t WI_Len(void *ctx) {
  WildcardIteratorCtx *nc = ctx;
  return nc->topId;
}

/* Last docId */
static t_docId WI_LastDocId(void *ctx) {
  WildcardIteratorCtx *nc = ctx;

  return nc->current;
}

static void WI_Rewind(void *p) {
  WildcardIteratorCtx *ctx = p;
  ctx->current = 0;
}

static size_t WI_NumEstimated(void *p) {
  WildcardIteratorCtx *ctx = p;
  return ctx->numDocs;
}

/* Create a new wildcard iterator */
IndexIterator *NewWildcardIterator_NonOptimized(t_docId maxId, size_t numDocs) {
  WildcardIteratorCtx *c = rm_calloc(1, sizeof(*c));
  c->current = 0;
  c->topId = maxId;
  c->numDocs = numDocs;

  CURRENT_RECORD(c) = NewVirtualResult(1, RS_FIELDMASK_ALL);
  CURRENT_RECORD(c)->freq = 1;

  IndexIterator *ret = &c->base;
  ret->ctx = c;
  ret->type = WILDCARD_ITERATOR;
  ret->Free = WI_Free;
  ret->HasNext = WI_HasNext;
  ret->LastDocId = WI_LastDocId;
  ret->Len = WI_Len;
  ret->Read = WI_Read;
  ret->SkipTo = WI_SkipTo;
  ret->Abort = WI_Abort;
  ret->Rewind = WI_Rewind;
  ret->NumEstimated = WI_NumEstimated;
  return ret;
}

// Returns a new wildcard iterator.
IndexIterator *NewWildcardIterator(QueryEvalCtx *q) {
  IndexIterator *ret;
  if (q->sctx->spec->rule->index_all == true) {
    if (q->sctx->spec->existingDocs) {
      IndexReader *ir = NewGenericIndexReader(q->sctx->spec->existingDocs,
        q->sctx, 1, 1, RS_INVALID_FIELD_INDEX, FIELD_EXPIRATION_DEFAULT);
      ret = NewReadIterator(ir);
      ret->type = WILDCARD_ITERATOR;
    } else {
      ret = NewEmptyIterator();
    }
    return ret;
  }

  // Non-optimized wildcard iterator, using a simple doc-id increment as its base.
  return NewWildcardIterator_NonOptimized(q->docTable->maxDocId, q->docTable->size);
}

static int EOI_Read(void *p, RSIndexResult **e) {
  return INDEXREAD_EOF;
}
static void EOI_Free(struct indexIterator *self) {
  // Nothing
}
static size_t EOI_NumEstimated(void *ctx) {
  return 0;
}
static size_t EOI_Len(void *ctx) {
  return 0;
}
static t_docId EOI_LastDocId(void *ctx) {
  return 0;
}

static int EOI_SkipTo(void *ctx, t_docId docId, RSIndexResult **hit) {
  return INDEXREAD_EOF;
}
static void EOI_Abort(void *ctx) {
}
static void EOI_Rewind(void *ctx) {
}

static IndexIterator eofIterator = {.Read = EOI_Read,
                                    .Free = EOI_Free,
                                    .SkipTo = EOI_SkipTo,
                                    .Len = EOI_Len,
                                    .LastDocId = EOI_LastDocId,
                                    .NumEstimated = EOI_NumEstimated,
                                    .Abort = EOI_Abort,
                                    .Rewind = EOI_Rewind,
                                    .type = EMPTY_ITERATOR};

IndexIterator *NewEmptyIterator(void) {
  return &eofIterator;
}

/**********************************************************
 * Profile printing functions
 **********************************************************/

static int PI_Read(void *ctx, RSIndexResult **e) {
  ProfileIterator *pi = ctx;
  clock_t begin = clock();
  pi->counters.read++;
  int ret = pi->child->Read(pi->child->ctx, e);
  if (ret == INDEXREAD_EOF) {
    pi->counters.eof = 1;
  }
  pi->base.current = pi->child->current;
  pi->cpuTime += clock() - begin;
  return ret;
}

static int PI_SkipTo(void *ctx, t_docId docId, RSIndexResult **hit) {
  ProfileIterator *pi = ctx;
  clock_t begin = clock();
  pi->counters.skipTo++;
  int ret = pi->child->SkipTo(pi->child->ctx, docId, hit);
  if (ret == INDEXREAD_EOF) {
    pi->counters.eof = 1;
  }
  pi->base.current = pi->child->current;
  pi->cpuTime += clock() - begin;
  return ret;
}

static void PI_Free(IndexIterator *it) {
  ProfileIterator *pi = (ProfileIterator *)it;
  pi->child->Free(pi->child);
  rm_free(it);
}

#define PROFILE_ITERATOR_FUNC_SIGN(func, rettype)     \
static rettype PI_##func(void *ctx) {                 \
  ProfileIterator *pi = ctx;                          \
  return pi->child->func(pi->child->ctx);             \
}

PROFILE_ITERATOR_FUNC_SIGN(Abort, void);
PROFILE_ITERATOR_FUNC_SIGN(Len, size_t);
PROFILE_ITERATOR_FUNC_SIGN(Rewind, void);
PROFILE_ITERATOR_FUNC_SIGN(LastDocId, t_docId);
PROFILE_ITERATOR_FUNC_SIGN(NumEstimated, size_t);

static int PI_HasNext(void *ctx) {
  ProfileIterator *pi = ctx;
  return IITER_HAS_NEXT(pi->child);
}

/* Create a new wildcard iterator */
IndexIterator *NewProfileIterator(IndexIterator *child) {
  ProfileIteratorCtx *pc = rm_calloc(1, sizeof(*pc));
  pc->child = child;
  pc->counters.read = 0;
  pc->counters.skipTo = 0;
  pc->cpuTime = 0;
  pc->counters.eof = 0;

  IndexIterator *ret = &pc->base;
  ret->ctx = pc;
  ret->type = PROFILE_ITERATOR;
  ret->Free = PI_Free;
  ret->HasNext = PI_HasNext;
  ret->LastDocId = PI_LastDocId;
  ret->Len = PI_Len;
  ret->Read = PI_Read;
  ret->SkipTo = PI_SkipTo;
  ret->Abort = PI_Abort;
  ret->Rewind = PI_Rewind;
  ret->NumEstimated = PI_NumEstimated;
  return ret;
}

#define PRINT_PROFILE_FUNC(name) static void name(RedisModule_Reply *reply,   \
                                                  IndexIterator *root,        \
                                                  ProfileCounters *counters,  \
                                                  double cpuTime,             \
                                                  int depth,                  \
                                                  int limited,                \
                                                  PrintProfileConfig *config)

PRINT_PROFILE_FUNC(printUnionIt) {
  UnionIterator *ui = (UnionIterator *)root;
  int printFull = !limited  || (ui->origType & QN_UNION);

  RedisModule_Reply_Map(reply);

  printProfileType("UNION");

  RedisModule_Reply_SimpleString(reply, "Query type");
  char *unionTypeStr;
  switch (ui->origType) {
  case QN_GEO : unionTypeStr = "GEO"; break;
  case QN_GEOMETRY : unionTypeStr = "GEOSHAPE"; break;
  case QN_TAG : unionTypeStr = "TAG"; break;
  case QN_UNION : unionTypeStr = "UNION"; break;
  case QN_FUZZY : unionTypeStr = "FUZZY"; break;
  case QN_PREFIX : unionTypeStr = "PREFIX"; break;
  case QN_NUMERIC : unionTypeStr = "NUMERIC"; break;
  case QN_LEXRANGE : unionTypeStr = "LEXRANGE"; break;
  case QN_WILDCARD_QUERY : unionTypeStr = "WILDCARD"; break;
  default:
    RS_ABORT_ALWAYS("Invalid type for union");
  }
  if (!ui->qstr) {
    RedisModule_Reply_SimpleString(reply, unionTypeStr);
  } else {
    const char *qstr = ui->qstr;
    if (isUnsafeForSimpleString(qstr)) qstr = escapeSimpleString(qstr);
    RedisModule_Reply_SimpleStringf(reply, "%s - %s", unionTypeStr, qstr);
    if (qstr != ui->qstr) rm_free((char*)qstr);
  }

  if (config->printProfileClock) {
    printProfileTime(cpuTime);
  }

  printProfileCounters(counters);

  RedisModule_Reply_SimpleString(reply, "Child iterators");
  if (printFull) {
    RedisModule_Reply_Array(reply);
      for (int i = 0; i < ui->norig; i++) {
        printIteratorProfile(reply, ui->origits[i], 0, 0, depth + 1, limited, config);
      }
    RedisModule_Reply_ArrayEnd(reply);
  } else {
    RedisModule_Reply_SimpleStringf(reply, "The number of iterators in the union is %d", ui->norig);
  }

  RedisModule_Reply_MapEnd(reply);
}

PRINT_PROFILE_FUNC(printIntersectIt) {
  IntersectIterator *ii = (IntersectIterator *)root;

  RedisModule_Reply_Map(reply);

  printProfileType("INTERSECT");

  if (config->printProfileClock) {
    printProfileTime(cpuTime);
  }

  printProfileCounters(counters);

  RedisModule_ReplyKV_Array(reply, "Child iterators");
    for (int i = 0; i < ii->num; i++) {
      if (ii->its[i]) {
        printIteratorProfile(reply, ii->its[i], 0, 0, depth + 1, limited, config);
      } else {
        RedisModule_Reply_Null(reply);
      }
    }
  RedisModule_Reply_ArrayEnd(reply);

  RedisModule_Reply_MapEnd(reply);
}

PRINT_PROFILE_FUNC(printMetricIt) {
  RedisModule_Reply_Map(reply);

  switch (GetMetric(root)) {
    case VECTOR_DISTANCE: {
      printProfileType("METRIC - VECTOR DISTANCE");
      break;
    }
    default: {
      RS_ABORT("Invalid type for metric");
      break;
    }
  }

  if (config->printProfileClock) {
    printProfileTime(cpuTime);
  }

  printProfileCounters(counters);

  RedisModule_Reply_MapEnd(reply);
}

void PrintIteratorChildProfile(RedisModule_Reply *reply, IndexIterator *root, ProfileCounters *counters, double cpuTime,
                  int depth, int limited, PrintProfileConfig *config, IndexIterator *child, const char *text) {
  size_t nlen = 0;
  RedisModule_Reply_Map(reply);
    printProfileType(text);
    if (config->printProfileClock) {
      printProfileTime(cpuTime);
    }
    printProfileCounters(counters);

    if (root->type == HYBRID_ITERATOR) {
      HybridIterator *hi = root->ctx;
      if (hi->searchMode == VECSIM_HYBRID_BATCHES ||
          hi->searchMode == VECSIM_HYBRID_BATCHES_TO_ADHOC_BF) {
        printProfileNumBatches(hi);
      }
    }

    if (root->type == OPTIMUS_ITERATOR) {
      OptimizerIterator *oi = root->ctx;
      printProfileOptimizationType(oi);
    }

    if (child) {
      RedisModule_Reply_SimpleString(reply, "Child iterator");
      printIteratorProfile(reply, child, 0, 0, depth + 1, limited, config);
    }
  RedisModule_Reply_MapEnd(reply);
}

#define PRINT_PROFILE_SINGLE_NO_CHILD(name, text)                                      \
  PRINT_PROFILE_FUNC(name) {                                                           \
    PrintIteratorChildProfile(reply, (root), counters, cpuTime, depth, limited, config, \
      NULL, (text));                                                                   \
  }

#define PRINT_PROFILE_SINGLE(name, IterType, text)                                     \
  PRINT_PROFILE_FUNC(name) {                                                           \
    PrintIteratorChildProfile(reply, (root), counters, cpuTime, depth, limited, config, \
      ((IterType *)(root))->child, (text));                                            \
  }

PRINT_PROFILE_SINGLE_NO_CHILD(printWildcardIt,          "WILDCARD");
PRINT_PROFILE_SINGLE_NO_CHILD(printIdListIt,            "ID-LIST");
PRINT_PROFILE_SINGLE_NO_CHILD(printEmptyIt,             "EMPTY");
PRINT_PROFILE_SINGLE(printNotIt, NotIterator,           "NOT");
PRINT_PROFILE_SINGLE(printOptionalIt, OptionalIterator, "OPTIONAL");
PRINT_PROFILE_SINGLE(printHybridIt, HybridIterator,     "VECTOR");
PRINT_PROFILE_SINGLE(printOptimusIt, OptimizerIterator, "OPTIMIZER");

PRINT_PROFILE_FUNC(printProfileIt) {
  ProfileIterator *pi = (ProfileIterator *)root;
  printIteratorProfile(reply, pi->child, &pi->counters,
    (double)(pi->cpuTime / CLOCKS_PER_MILLISEC), depth, limited, config);
}

void printIteratorProfile(RedisModule_Reply *reply, IndexIterator *root, ProfileCounters *counters,
                          double cpuTime, int depth, int limited, PrintProfileConfig *config) {
  if (root == NULL) return;

  // protect against limit of 7 reply layers
  if (depth == REDIS_ARRAY_LIMIT && !isFeatureSupported(NO_REPLY_DEPTH_LIMIT)) {
    RedisModule_Reply_Null(reply);
    return;
  }

  switch (root->type) {
    // Reader
    case READ_ITERATOR:       { printReadIt(reply, root, counters, cpuTime, config);                       break; }
    // Multi values
    case UNION_ITERATOR:      { printUnionIt(reply, root, counters, cpuTime, depth, limited, config);      break; }
    case INTERSECT_ITERATOR:  { printIntersectIt(reply, root, counters, cpuTime, depth, limited, config);  break; }
    // Single value
    case NOT_ITERATOR:        { printNotIt(reply, root, counters, cpuTime, depth, limited, config);        break; }
    case OPTIONAL_ITERATOR:   { printOptionalIt(reply, root, counters, cpuTime, depth, limited, config);   break; }
    case WILDCARD_ITERATOR:   { printWildcardIt(reply, root, counters, cpuTime, depth, limited, config);   break; }
    case EMPTY_ITERATOR:      { printEmptyIt(reply, root, counters, cpuTime, depth, limited, config);      break; }
    case ID_LIST_ITERATOR:    { printIdListIt(reply, root, counters, cpuTime, depth, limited, config);     break; }
    case PROFILE_ITERATOR:    { printProfileIt(reply, root, 0, 0, depth, limited, config);                break; }
    case HYBRID_ITERATOR:     { printHybridIt(reply, root, counters, cpuTime, depth, limited, config);     break; }
    case METRIC_ITERATOR:     { printMetricIt(reply, root, counters, cpuTime, depth, limited, config);     break; }
    case OPTIMUS_ITERATOR:    { printOptimusIt(reply, root, counters, cpuTime, depth, limited, config);    break; }
    case MAX_ITERATOR:        { RS_ABORT("nope");   break; }
  }
}

/** Add Profile iterator before any iterator in the tree */
void Profile_AddIters(IndexIterator **root) {
  UnionIterator *ui;
  IntersectIterator *ini;

  if (*root == NULL) return;

  // Add profile iterator before child iterators
  switch((*root)->type) {
    case NOT_ITERATOR:
      Profile_AddIters(&((NotIterator *)((*root)->ctx))->child);
      break;
    case OPTIONAL_ITERATOR:
      Profile_AddIters(&((OptionalIterator *)((*root)->ctx))->child);
      break;
    case HYBRID_ITERATOR:
      Profile_AddIters(&((HybridIterator *)((*root)->ctx))->child);
      break;
    case OPTIMUS_ITERATOR:
      Profile_AddIters(&((OptimizerIterator *)((*root)->ctx))->child);
      break;
    case UNION_ITERATOR:
      ui = (*root)->ctx;
      for (int i = 0; i < ui->norig; i++) {
        Profile_AddIters(&(ui->origits[i]));
      }
      UI_SyncIterList(ui);
      break;
    case INTERSECT_ITERATOR:
      ini = (*root)->ctx;
      for (int i = 0; i < ini->num; i++) {
        Profile_AddIters(&(ini->its[i]));
      }
      break;
    case WILDCARD_ITERATOR:
    case READ_ITERATOR:
    case EMPTY_ITERATOR:
    case ID_LIST_ITERATOR:
    case METRIC_ITERATOR:
      break;
    case PROFILE_ITERATOR:
    case MAX_ITERATOR:
      RS_ABORT("Error");
      break;
  }

  // Create a profile iterator and update outparam pointer
  *root = NewProfileIterator(*root);
}
