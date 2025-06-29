/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#include "index_result.h"
#include "index_iterator.h"
#include "rmalloc.h"

typedef struct {
  IndexIterator base;
  t_docId *docIds;
  t_docId lastDocId;
  t_offset size;
  t_offset offset;
} IdListIterator;

static inline void setEof(IdListIterator *it, int value) {
  it->base.isValid = !value;
}

static inline int isEof(const IdListIterator *it) {
  return !it->base.isValid;
}

size_t IL_NumEstimated(void *ctx) {
  IdListIterator *it = ctx;
  return (size_t)it->size;
}

/* Read the next entry from the iterator, into hit *e.
 *  Returns INDEXREAD_EOF if at the end */
int IL_Read(void *ctx, RSIndexResult **r) {
  IdListIterator *it = ctx;
  if (isEof(it) || it->offset >= it->size) {
    setEof(it, 1);
    return INDEXREAD_EOF;
  }

  it->lastDocId = it->docIds[it->offset++];

  // TODO: Filter here
  it->base.current->docId = it->lastDocId;
  *r = it->base.current;
  return INDEXREAD_OK;
}

void IL_Abort(void *ctx) {
  ((IdListIterator *)ctx)->base.isValid = 0;
}

/* Skip to a docid, potentially reading the entry into hit, if the docId
 * matches */
int IL_SkipTo(void *ctx, t_docId docId, RSIndexResult **r) {
  IdListIterator *it = ctx;
  if (isEof(it) || it->offset >= it->size) {
    return INDEXREAD_EOF;
  }

  if (docId > it->docIds[it->size - 1]) {
    it->base.isValid = 0;
    return INDEXREAD_EOF;
  }

  t_offset top = it->size - 1, bottom = it->offset;
  t_offset i = 0;

  while (bottom <= top) {
    i = (bottom + top) / 2;
    t_docId did = it->docIds[i];

    if (did == docId) {
      break;
    }
    if (docId < did) {
      if (i == 0) break;
      top = i - 1;
    } else {
      bottom = i + 1;
    }
  }
  it->offset = i + 1;
  if (it->offset >= it->size) {
    setEof(it, 1);
  }

  it->lastDocId = it->docIds[i];
  it->base.current->docId = it->lastDocId;

  *r = it->base.current;

  if (it->lastDocId == docId) {
    return INDEXREAD_OK;
  }
  return INDEXREAD_NOTFOUND;
}

/* the last docId read */
t_docId IL_LastDocId(void *ctx) {
  return ((IdListIterator *)ctx)->lastDocId;
}

/* release the iterator's context and free everything needed */
void IL_Free(struct indexIterator *self) {
  IdListIterator *it = self->ctx;
  IndexResult_Free(it->base.current);
  if (it->docIds) {
    rm_free(it->docIds);
  }
  rm_free(self);
}

/* Return the number of results in this iterator. Used by the query execution
 * on the top iterator */
size_t IL_Len(void *ctx) {
  return (size_t)((IdListIterator *)ctx)->size;
}

void IL_Rewind(void *p) {
  IdListIterator *il = p;
  setEof(il, 0);
  il->lastDocId = 0;
  il->base.current->docId = 0;
  il->offset = 0;
}

IndexIterator *NewIdListIterator(t_docId *ids, t_offset num, double weight) {
  IdListIterator *it = rm_new(IdListIterator);

  it->size = num;
  it->docIds = ids;
  setEof(it, 0);
  it->lastDocId = 0;
  it->base.current = NewVirtualResult(weight, RS_FIELDMASK_ALL);

  it->offset = 0;

  IndexIterator *ret = &it->base;
  ret->ctx = it;
  ret->type = ID_LIST_ITERATOR;
  ret->NumEstimated = IL_NumEstimated;
  ret->Free = IL_Free;
  ret->LastDocId = IL_LastDocId;
  ret->Len = IL_Len;
  ret->Read = IL_Read;
  ret->SkipTo = IL_SkipTo;
  ret->Abort = IL_Abort;
  ret->Rewind = IL_Rewind;

  ret->HasNext = NULL;
  return ret;
}
