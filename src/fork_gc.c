/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#include "fork_gc.h"
#include "triemap.h"
#include "util/arr.h"
#include "search_ctx.h"
#include "inverted_index.h"
#include "redis_index.h"
#include "numeric_index.h"
#include "tag_index.h"
#include "time_sample.h"
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <poll.h>
#include "rwlock.h"
#include "hll/hll.h"
#include <float.h>
#include "module.h"
#include "rmutil/rm_assert.h"
#include "suffix.h"
#include "resp3.h"
#include "info/global_stats.h"
#include "info/info_redis/threads/current_thread.h"
#include "obfuscation/obfuscation_api.h"
#include "obfuscation/hidden.h"

#define GC_WRITERFD 1
#define GC_READERFD 0
// Number of attempts to wait for the child to exit gracefully before trying to terminate it
#define GC_WAIT_ATTEMPTS 4

typedef enum {
  // Terms have been collected
  FGC_COLLECTED,
  // No more terms remain
  FGC_DONE,
  // Pipe error, child probably crashed
  FGC_CHILD_ERROR,
  // Error on the parent
  FGC_PARENT_ERROR,
  // The spec was deleted
  FGC_SPEC_DELETED,
} FGCError;

// Assumes the spec is locked.
static void FGC_updateStats(ForkGC *gc, RedisSearchCtx *sctx,
            size_t recordsRemoved, size_t bytesCollected, size_t bytesAdded) {
  sctx->spec->stats.numRecords -= recordsRemoved;
  sctx->spec->stats.invertedSize += bytesAdded;
  sctx->spec->stats.invertedSize -= bytesCollected;
  gc->stats.totalCollected += bytesCollected;
  gc->stats.totalCollected -= bytesAdded;
}

// Buff shouldn't be NULL.
static void FGC_sendFixed(ForkGC *fgc, const void *buff, size_t len) {
  RS_LOG_ASSERT(len > 0, "buffer length cannot be 0");
  ssize_t size = write(fgc->pipe_write_fd, buff, len);
  if (size != len) {
    perror("broken pipe, exiting GC fork: write() failed");
    // just exit, do not abort(), which will trigger a watchdog on RLEC, causing adverse effects
    RedisModule_Log(fgc->ctx, "warning", "GC fork: broken pipe, exiting");
    RedisModule_ExitFromChild(1);
  }
}

#define FGC_SEND_VAR(fgc, v) FGC_sendFixed(fgc, &v, sizeof v)

static void FGC_sendBuffer(ForkGC *fgc, const void *buff, size_t len) {
  FGC_SEND_VAR(fgc, len);
  if (len > 0) {
    FGC_sendFixed(fgc, buff, len);
  }
}

static int FGC_recvFixed(ForkGC *fgc, void *buf, size_t len);

/**
 * Send instead of a string to indicate that no more buffers are to be received
 */
static void FGC_sendTerminator(ForkGC *fgc) {
  size_t smax = SIZE_MAX;
  FGC_SEND_VAR(fgc, smax);
}

static int __attribute__((warn_unused_result)) FGC_recvFixed(ForkGC *fgc, void *buf, size_t len) {
  // poll the pipe, so that we don't block while read, with timeout of 3 minutes
  while (poll(fgc->pollfd_read, 1, 180000) == 1) {
    ssize_t nrecvd = read(fgc->pipe_read_fd, buf, len);
    if (nrecvd > 0) {
      buf += nrecvd;
      len -= nrecvd;
    } else if (nrecvd <= 0 && errno != EINTR) {
      RedisModule_Log(fgc->ctx, "warning", "ForkGC - got error while reading from pipe (%s)", strerror(errno));
      return REDISMODULE_ERR;
    }
    if (len == 0) {
      return REDISMODULE_OK;
    }
  }
  RedisModule_Log(fgc->ctx, "warning", "ForkGC - got timeout while reading from pipe (%s)", strerror(errno));
  return REDISMODULE_ERR;
}

#define TRY_RECV_FIXED(gc, obj, len)                   \
  if (FGC_recvFixed(gc, obj, len) != REDISMODULE_OK) { \
    return REDISMODULE_ERR;                            \
  }

static void *RECV_BUFFER_EMPTY = (void *)0x0deadbeef;

static int __attribute__((warn_unused_result))
FGC_recvBuffer(ForkGC *fgc, void **buf, size_t *len) {
  TRY_RECV_FIXED(fgc, len, sizeof *len);
  if (*len == SIZE_MAX) {
    *buf = RECV_BUFFER_EMPTY;
    return REDISMODULE_OK;
  }
  if (*len == 0) {
    *buf = NULL;
    return REDISMODULE_OK;
  }

  *buf = rm_malloc(*len + 1);
  ((char *)(*buf))[*len] = 0;
  if (FGC_recvFixed(fgc, *buf, *len) != REDISMODULE_OK) {
    rm_free(buf);
    return REDISMODULE_ERR;
  }
  return REDISMODULE_OK;
}

typedef struct {
  // Number of blocks prior to repair
  uint32_t nblocksOrig;
  // Number of blocks repaired
  uint32_t nblocksRepaired;
  // Number of bytes cleaned in inverted index
  uint64_t nbytesCollected;
  // Number of bytes added to inverted index
  uint64_t nbytesAdded;
  // Number of document records removed
  uint64_t ndocsCollected;
  // Number of numeric records removed
  uint64_t nentriesCollected;

  /** Specific information about the _last_ index block */
  size_t lastblkDocsRemoved;
  size_t lastblkBytesCollected;
  size_t lastblkNumEntries;
  size_t lastblkEntriesRemoved;
} MSG_IndexInfo;

/** Structure sent describing an index block */
typedef struct {
  IndexBlock blk;
  int64_t oldix;  // Old position of the block
  int64_t newix;  // New position of the block
  // the actual content of the block follows...
} MSG_RepairedBlock;

typedef struct {
  void *ptr;       // Address of the buffer to free
  uint32_t oldix;  // Old index of deleted block
  uint32_t _pad;   // Uninitialized reads, otherwise
} MSG_DeletedBlock;

/**
 * headerCallback and hdrarg are invoked before the inverted index is sent, only
 * if the inverted index was repaired.
 * This function sends the main process an info message with general info on the inverted index garbage collection.
 * In addition, for each fixed block it sends a repair message. For deleted blocks it send delete message.
 * If the index size (number of blocks) wasn't modified (no deleted blocks) we don't send a new block list.
 * In this case, the main process will get the modifications from the fix messages, that contains also a copy of the
 * repaired block.
 * RepairCallback and its argument are passed directly to IndexBlock_Repair; see
 * that function for more details.
 */
static bool FGC_childRepairInvidx(ForkGC *gc, RedisSearchCtx *sctx, InvertedIndex *idx,
                                  void (*headerCallback)(ForkGC *, void *), void *hdrarg,
                                  IndexRepairParams *params) {
  MSG_RepairedBlock *fixed = array_new(MSG_RepairedBlock, 10);
  MSG_DeletedBlock *deleted = array_new(MSG_DeletedBlock, 10);
  IndexBlock *blocklist = array_new(IndexBlock, idx->size);
  MSG_IndexInfo ixmsg = {.nblocksOrig = idx->size};
  bool rv = false;
  IndexRepairParams params_s = {0};
  if (!params) {
    params = &params_s;
  }

  for (size_t i = 0; i < idx->size; ++i) {
    params->bytesCollected = 0;
    params->bytesBeforFix = 0;
    params->bytesAfterFix = 0;
    params->entriesCollected = 0;
    IndexBlock *blk = idx->blocks + i;
    t_docId firstId = IndexBlock_FirstId(blk);
    t_docId lastId = IndexBlock_LastId(blk);

    if (lastId - firstId > UINT32_MAX) {
      // Skip over blocks which have a wide variation. In the future we might
      // want to split a block into two (or more) on high-delta boundaries.
      // todo: is it ok??
      // The above TODO was written 5 years ago. We currently don't split blocks,
      // and it is also not clear why we care about high variations.
      array_append(blocklist, *blk);
      continue;
    }

    // Capture the pointer address before the block is cleared; otherwise
    // the pointer might be freed! (IndexBlock_Repair rewrites blk->buf if there were repairs)
    void *bufptr = IndexBlock_Data(blk);
    size_t nrepaired = IndexBlock_Repair(blk, &sctx->spec->docs, idx->flags, params);
    if (nrepaired == 0) {
      // unmodified block
      array_append(blocklist, *blk);
      continue;
    }

    uint64_t curr_bytesCollected = params->bytesBeforFix - params->bytesAfterFix;

    uint16_t numEntries = IndexBlock_NumEntries(blk);
    if (numEntries == 0) {
      // this block should be removed
      MSG_DeletedBlock *delmsg = array_ensure_tail(&deleted, MSG_DeletedBlock);
      *delmsg = (MSG_DeletedBlock){.ptr = bufptr, .oldix = i};
      curr_bytesCollected += sizeof(IndexBlock);
    } else {
      array_append(blocklist, *blk);
      MSG_RepairedBlock *fixmsg = array_ensure_tail(&fixed, MSG_RepairedBlock);
      fixmsg->newix = array_len(blocklist) - 1;
      fixmsg->oldix = i;
      fixmsg->blk = *blk; // TODO: consider sending the blocklist even if there weren't any deleted blocks instead of this redundant copy.
      ixmsg.nblocksRepaired++;
    }
    ixmsg.nbytesCollected += curr_bytesCollected;
    ixmsg.ndocsCollected += nrepaired;
    ixmsg.nentriesCollected += params->entriesCollected;
    // Save last block statistics because the main process might want to ignore the changes if
    // the block was modified while the fork was running.
    if (i == idx->size - 1) {
      ixmsg.lastblkBytesCollected = curr_bytesCollected;
      ixmsg.lastblkDocsRemoved = nrepaired;
      ixmsg.lastblkEntriesRemoved = params->entriesCollected;
      // Save the original number of entries of the last block so we can compare
      // this value to the number of entries exist in the main process, to conclude if any new entries
      // were added during the fork process was running. If there were, the main process will discard the last block
      // fixes. We rely on the assumption that a block is small enough and it will be either handled in the next iteration,
      // or it will get to its maximum capacity and will no longer be the last block.
      ixmsg.lastblkNumEntries = IndexBlock_NumEntries(blk) + params->entriesCollected;
    }
  }

  if (array_len(fixed) == 0 && array_len(deleted) == 0) {
    // No blocks were removed or repaired
    goto done;
  }

  headerCallback(gc, hdrarg);
  FGC_sendFixed(gc, &ixmsg, sizeof ixmsg);
  if (array_len(blocklist) == idx->size) {
    // no empty block, there is no need to send the blocks array. Don't send
    // any new blocks.
    size_t len = 0;
    FGC_SEND_VAR(gc, len);
  } else {
    FGC_sendBuffer(gc, blocklist, array_len(blocklist) * sizeof(*blocklist));
  }
  // TODO: can we move it inside the if?
  FGC_sendBuffer(gc, deleted, array_len(deleted) * sizeof(*deleted));

  for (size_t i = 0; i < array_len(fixed); ++i) {
    // write fix block
    const MSG_RepairedBlock *msg = fixed + i;
    const IndexBlock *blk = blocklist + msg->newix;
    FGC_sendFixed(gc, msg, sizeof(*msg));
    // TODO: check why we need to send the data if its part of the blk struct.
    FGC_sendBuffer(gc, IndexBlock_Data(blk), IndexBlock_Len(blk));
  }
  rv = true;

done:
  array_free(fixed);
  array_free(blocklist);
  array_free(deleted);
  return rv;
}

static void sendHeaderString(ForkGC *gc, void *arg) {
  struct iovec *iov = arg;
  FGC_sendBuffer(gc, iov->iov_base, iov->iov_len);
}

static void FGC_reportProgress(ForkGC *gc) {
  RedisModule_SendChildHeartbeat(gc->progress);
}

static void FGC_setProgress(ForkGC *gc, float progress) {
  gc->progress = progress;
  FGC_reportProgress(gc);
}

static void FGC_childCollectTerms(ForkGC *gc, RedisSearchCtx *sctx) {
  TrieIterator *iter = Trie_Iterate(sctx->spec->terms, "", 0, 0, 1);
  rune *rstr = NULL;
  t_len slen = 0;
  float score = 0;
  int dist = 0;
  while (TrieIterator_Next(iter, &rstr, &slen, NULL, &score, &dist)) {
    size_t termLen;
    char *term = runesToStr(rstr, slen, &termLen);
    InvertedIndex *idx = Redis_OpenInvertedIndex(sctx, term, strlen(term), DONT_CREATE_INDEX, NULL);
    if (idx) {
      struct iovec iov = {.iov_base = (void *)term, termLen};
      FGC_childRepairInvidx(gc, sctx, idx, sendHeaderString, &iov, NULL);
      FGC_reportProgress(gc);
    }
    rm_free(term);
  }
  TrieIterator_Free(iter);

  // we are done with terms
  FGC_sendTerminator(gc);
}

typedef struct {
  struct HLL majority_card;     // Holds the majority cardinality of all the blocks we've seen so far
  struct HLL last_block_card;   // Holds the cardinality of the last block we've seen
  const IndexBlock *last_block; // The last block we've seen, to know when to merge the cardinalities
} numCbCtx;

static void countRemain(const RSIndexResult *r, const IndexBlock *blk, void *arg) {
  numCbCtx *ctx = arg;

  if (ctx->last_block != blk) {
    // We are in a new block, merge the last block's cardinality into the majority, and clear the last block
    hll_merge(&ctx->majority_card, &ctx->last_block_card);
    hll_clear(&ctx->last_block_card);
    ctx->last_block = blk;
  }
  // Add the current record to the last block's cardinality
  hll_add(&ctx->last_block_card, &r->data.num.value, sizeof(r->data.num.value));
}

typedef struct {
  int type;
  const char *field;
  const void *curPtr;
  char *tagValue;
  size_t tagLen;
  uint64_t uniqueId;
  int sentFieldName;
} tagNumHeader;

static void sendNumericTagHeader(ForkGC *fgc, void *arg) {
  tagNumHeader *info = arg;
  if (!info->sentFieldName) {
    info->sentFieldName = 1;
    FGC_sendBuffer(fgc, info->field, strlen(info->field));
    FGC_sendFixed(fgc, &info->uniqueId, sizeof info->uniqueId);
  }
  FGC_SEND_VAR(fgc, info->curPtr);
  if (info->type == RSFLDTYPE_TAG) {
    FGC_sendBuffer(fgc, info->tagValue, info->tagLen);
  }
}

// If anything other than FGC_COLLECTED is returned, it is an error or done
static FGCError recvNumericTagHeader(ForkGC *fgc, char **fieldName, size_t *fieldNameLen,
                                     uint64_t *id) {
  if (FGC_recvBuffer(fgc, (void **)fieldName, fieldNameLen) != REDISMODULE_OK) {
    return FGC_PARENT_ERROR;
  }
  if (*fieldName == RECV_BUFFER_EMPTY) {
    *fieldName = NULL;
    return FGC_DONE;
  }

  if (FGC_recvFixed(fgc, id, sizeof(*id)) != REDISMODULE_OK) {
    rm_free(*fieldName);
    *fieldName = NULL;
    return FGC_PARENT_ERROR;
  }
  return FGC_COLLECTED;
}

static void FGC_childCollectNumeric(ForkGC *gc, RedisSearchCtx *sctx) {
  arrayof(FieldSpec*) numericFields = getFieldsByType(sctx->spec, INDEXFLD_T_NUMERIC | INDEXFLD_T_GEO);

  for (int i = 0; i < array_len(numericFields); ++i) {
    RedisModuleString *keyName = IndexSpec_GetFormattedKey(sctx->spec, numericFields[i], INDEXFLD_T_NUMERIC);
    NumericRangeTree *rt = openNumericKeysDict(sctx->spec, keyName, DONT_CREATE_INDEX);

    // No entries were added to the numeric field, hence the tree was not initialized
    if (!rt) {
      continue;
    }

    NumericRangeTreeIterator *gcIterator = NumericRangeTreeIterator_New(rt);

    NumericRangeNode *currNode = NULL;
    tagNumHeader header = {.type = RSFLDTYPE_NUMERIC,
                           .field = HiddenString_GetUnsafe(numericFields[i]->fieldName, NULL),
                           .uniqueId = rt->uniqueId};

    numCbCtx nctx;
    IndexRepairParams params = {.RepairCallback = countRemain, .arg = &nctx};
    hll_init(&nctx.majority_card, NR_BIT_PRECISION);
    hll_init(&nctx.last_block_card, NR_BIT_PRECISION);
    while ((currNode = NumericRangeTreeIterator_Next(gcIterator))) {
      if (!currNode->range) {
        continue;
      }
      nctx.last_block = NULL;
      hll_clear(&nctx.majority_card);
      hll_clear(&nctx.last_block_card);

      InvertedIndex *idx = currNode->range->entries;
      header.curPtr = currNode;
      bool repaired = FGC_childRepairInvidx(gc, sctx, idx, sendNumericTagHeader, &header, &params);

      if (repaired) {
        // Instead of sending the majority cardinality and the last block's cardinality, we now
        // merge the majority cardinality into the last block's cardinality, and send its registers
        // as the cardinality WITH the last block's cardinality, and then send the majority registers
        // as the cardinality WITHOUT the last block's cardinality. This way, the main process can
        // choose which registers to use without having to merge them itself.
        hll_merge(&nctx.last_block_card, &nctx.majority_card);
        FGC_sendFixed(gc, nctx.last_block_card.registers, NR_REG_SIZE);
        FGC_sendFixed(gc, nctx.majority_card.registers, NR_REG_SIZE);
      }
      FGC_reportProgress(gc);
    }
    hll_destroy(&nctx.majority_card);
    hll_destroy(&nctx.last_block_card);

    if (header.sentFieldName) {
      // If we've repaired at least one entry, send the terminator;
      // note that "terminator" just means a zero address and not the
      // "no more strings" terminator in FGC_sendTerminator
      void *pdummy = NULL;
      FGC_SEND_VAR(gc, pdummy);
    }

    NumericRangeTreeIterator_Free(gcIterator);
  }

  array_free(numericFields);
  // we are done with numeric fields
  FGC_sendTerminator(gc);
}

static void FGC_childCollectTags(ForkGC *gc, RedisSearchCtx *sctx) {
  arrayof(FieldSpec*) tagFields = getFieldsByType(sctx->spec, INDEXFLD_T_TAG);
  if (array_len(tagFields) != 0) {
    for (int i = 0; i < array_len(tagFields); ++i) {
      RedisModuleString *keyName = IndexSpec_GetFormattedKey(sctx->spec, tagFields[i], INDEXFLD_T_TAG);
      TagIndex *tagIdx = TagIndex_Open(sctx->spec, keyName, DONT_CREATE_INDEX);
      if (!tagIdx) {
        continue;
      }

      tagNumHeader header = {.type = RSFLDTYPE_TAG,
                             .field = HiddenString_GetUnsafe(tagFields[i]->fieldName, NULL),
                             .uniqueId = tagIdx->uniqueId};

      TrieMapIterator *iter = TrieMap_Iterate(tagIdx->values);
      char *ptr;
      tm_len_t len;
      InvertedIndex *value;
      while (TrieMapIterator_Next(iter, &ptr, &len, (void **)&value)) {
        header.curPtr = value;
        header.tagValue = ptr;
        header.tagLen = len;
        // send repaired data
        FGC_childRepairInvidx(gc, sctx, value, sendNumericTagHeader, &header, NULL);
        FGC_reportProgress(gc);
      }

      // we are done with the current field
      if (header.sentFieldName) {
        void *pdummy = NULL;
        FGC_SEND_VAR(gc, pdummy);
      }
    }
  }

  array_free(tagFields);
  // we are done with tag fields
  FGC_sendTerminator(gc);
}

static void FGC_childCollectMissingDocs(ForkGC *gc, RedisSearchCtx *sctx) {
  IndexSpec *spec = sctx->spec;

  dictIterator* iter = dictGetIterator(spec->missingFieldDict);
  dictEntry* entry = NULL;
  while ((entry = dictNext(iter))) {
    const HiddenString *hiddenFieldName = dictGetKey(entry);
    InvertedIndex *idx = dictGetVal(entry);
    if(idx) {
      size_t length;
      const char* fieldName = HiddenString_GetUnsafe(hiddenFieldName, &length);
      struct iovec iov = {.iov_base = (void *)fieldName, length};
      FGC_childRepairInvidx(gc, sctx, idx, sendHeaderString, &iov, NULL);
      FGC_reportProgress(gc);
    }
  }
  dictReleaseIterator(iter);

  // we are done with missing field docs inverted indexes
  FGC_sendTerminator(gc);
}

static void FGC_childCollectExistingDocs(ForkGC *gc, RedisSearchCtx *sctx) {
  IndexSpec *spec = sctx->spec;

  InvertedIndex *idx = spec->existingDocs;
  if (idx) {
    struct iovec iov = {.iov_base = (void *)"", 0};
    FGC_childRepairInvidx(gc, sctx, idx, sendHeaderString, &iov, NULL);
  }

  // we are done with existing docs inverted index
  FGC_sendTerminator(gc);
}

static void FGC_childScanIndexes(ForkGC *gc, IndexSpec *spec) {
  RedisSearchCtx sctx = SEARCH_CTX_STATIC(gc->ctx, spec);
  const char* indexName = IndexSpec_FormatName(spec, RSGlobalConfig.hideUserDataFromLog);
  RedisModule_Log(sctx.redisCtx, "debug", "ForkGC in index %s - child scanning indexes start", indexName);
  FGC_setProgress(gc, 0);
  FGC_childCollectTerms(gc, &sctx);
  FGC_setProgress(gc, 0.2);
  FGC_childCollectNumeric(gc, &sctx);
  FGC_setProgress(gc, 0.4);
  FGC_childCollectTags(gc, &sctx);
  FGC_setProgress(gc, 0.6);
  FGC_childCollectMissingDocs(gc, &sctx);
  FGC_setProgress(gc, 0.8);
  FGC_childCollectExistingDocs(gc, &sctx);
  FGC_setProgress(gc, 1);
  RedisModule_Log(sctx.redisCtx, "debug", "ForkGC in index %s - child scanning indexes end", indexName);
}

typedef struct {
  MSG_DeletedBlock *delBlocks;
  size_t numDelBlocks;

  MSG_RepairedBlock *changedBlocks;

  IndexBlock *newBlocklist;
  size_t newBlocklistSize;
  bool lastBlockIgnored;
} InvIdxBuffers;

static int __attribute__((warn_unused_result))
FGC_recvRepairedBlock(ForkGC *gc, MSG_RepairedBlock *binfo) {
  if (FGC_recvFixed(gc, binfo, sizeof(*binfo)) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }
  if (FGC_recvBuffer(gc, (void **)IndexBlock_DataPtr(&binfo->blk), IndexBlock_LenPtr(&binfo->blk)) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }
  IndexBlock_SetCap(&binfo->blk, IndexBlock_Len(&binfo->blk));
  return REDISMODULE_OK;
}

static int __attribute__((warn_unused_result))
FGC_recvInvIdx(ForkGC *gc, InvIdxBuffers *bufs, MSG_IndexInfo *info) {
  size_t nblocksRecvd = 0;
  if (FGC_recvFixed(gc, info, sizeof(*info)) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }
  if (FGC_recvBuffer(gc, (void **)&bufs->newBlocklist, &bufs->newBlocklistSize) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }

  if (bufs->newBlocklistSize) {
    bufs->newBlocklistSize /= sizeof(*bufs->newBlocklist);
  }
  if (FGC_recvBuffer(gc, (void **)&bufs->delBlocks, &bufs->numDelBlocks) != REDISMODULE_OK) {
    goto error;
  }
  bufs->numDelBlocks /= sizeof(*bufs->delBlocks);
  bufs->changedBlocks = rm_malloc(sizeof(*bufs->changedBlocks) * info->nblocksRepaired);
  for (size_t i = 0; i < info->nblocksRepaired; ++i) {
    if (FGC_recvRepairedBlock(gc, bufs->changedBlocks + i) != REDISMODULE_OK) {
      goto error;
    }
    nblocksRecvd++;
  }
  return REDISMODULE_OK;

error:
  rm_free(bufs->newBlocklist);
  for (size_t ii = 0; ii < nblocksRecvd; ++ii) {
    IndexBlock_DataFree(&bufs->changedBlocks[ii].blk);
  }
  rm_free(bufs->changedBlocks);
  memset(bufs, 0, sizeof(*bufs));
  return REDISMODULE_ERR;
}

static void freeInvIdx(InvIdxBuffers *bufs, MSG_IndexInfo *info) {
  rm_free(bufs->newBlocklist);
  rm_free(bufs->delBlocks);

  if (bufs->changedBlocks) {
    // could be null because of pipe error
    for (size_t ii = 0; ii < info->nblocksRepaired; ++ii) {
      IndexBlock_DataFree(&bufs->changedBlocks[ii].blk);
    }
    rm_free(bufs->changedBlocks);
  }
}

static void checkLastBlock(ForkGC *gc, InvIdxBuffers *idxData, MSG_IndexInfo *info,
                           InvertedIndex *idx) {
  IndexBlock *lastOld = idx->blocks + info->nblocksOrig - 1;
  if (info->lastblkDocsRemoved == 0) {
    // didn't touch last block in child
    return;
  }
  if (info->lastblkNumEntries == IndexBlock_NumEntries(lastOld)) {
    // didn't touch last block in parent
    return;
  }

  // Otherwise, we added new entries to the last block while the child was running. In this case we discard all
  // the child garbage collection, assuming they will take place in the next gc iteration.

  if (info->lastblkEntriesRemoved == info->lastblkNumEntries) {
    // Last block was deleted entirely while updates on the main process.
    // Remove it from delBlocks list
    idxData->numDelBlocks--;

    // If all the blocks were deleted, there is no newblocklist. Otherwise, we need to add it to the newBlocklist.
    if (idxData->newBlocklist) {
      idxData->newBlocklistSize++;
      idxData->newBlocklist = rm_realloc(idxData->newBlocklist,
                                        sizeof(*idxData->newBlocklist) * idxData->newBlocklistSize);
      idxData->newBlocklist[idxData->newBlocklistSize - 1] = *lastOld;
    }
  } else {
    // Last block was modified on the child and on the parent. (but not entirely deleted)

    // we need to remove it from changedBlocks
    MSG_RepairedBlock *rb = idxData->changedBlocks + info->nblocksRepaired - 1;
    indexBlock_Free(&rb->blk);
    info->nblocksRepaired--;

    // If newBlocklist!=NULL then the last block must be there (it was changed and not deleted),
    // prefer the parent's block.
    if (idxData->newBlocklist) {
      idxData->newBlocklist[idxData->newBlocklistSize - 1] = *lastOld;
    }
  }

  info->ndocsCollected -= info->lastblkDocsRemoved;
  info->nbytesCollected -= info->lastblkBytesCollected;
  info->nentriesCollected -= info->lastblkEntriesRemoved;
  idxData->lastBlockIgnored = true;
  gc->stats.gcBlocksDenied++;
}

static void FGC_applyInvertedIndex(ForkGC *gc, InvIdxBuffers *idxData, MSG_IndexInfo *info,
                                   InvertedIndex *idx) {
  checkLastBlock(gc, idxData, info, idx);
  for (size_t i = 0; i < info->nblocksRepaired; ++i) {
    MSG_RepairedBlock *blockModified = idxData->changedBlocks + i;
    indexBlock_Free(&idx->blocks[blockModified->oldix]);
  }
  for (size_t i = 0; i < idxData->numDelBlocks; ++i) {
    // Blocks that were deleted entirely:
    MSG_DeletedBlock *delinfo = idxData->delBlocks + i;
    rm_free(delinfo->ptr);
  }
  TotalIIBlocks -= idxData->numDelBlocks;
  rm_free(idxData->delBlocks); // Consume del block array
  idxData->delBlocks = NULL;

  // Ensure the old index is at least as big as the new index' size
  RS_LOG_ASSERT(idx->size >= info->nblocksOrig, "Current index size should be larger or equal to original index size");

  if (idxData->newBlocklist) { // the child removed some of the blocks, but not all of them
    /**
     * At this point, we check if the last block has had new data added to it,
     * but was _not_ repaired. We check for a repaired last block in
     * checkLastBlock().
     */

    if (!info->lastblkDocsRemoved) {
      /**
       * Last block was unmodified-- let's prefer the last block's pointer
       * over our own (which may be stale).
       * If the last block was repaired, this is handled above in checkLastBlock()
       */
      idxData->newBlocklist[idxData->newBlocklistSize - 1] = idx->blocks[info->nblocksOrig - 1];
    }

    // Number of blocks added in the parent process since the last scan
    size_t newAddedLen = idx->size - info->nblocksOrig; // TODO: can we just decrease by number of deleted.

    // The final size is the reordered block size, plus the number of blocks
    // which we haven't scanned yet, because they were added in the parent
    size_t totalLen = idxData->newBlocklistSize + newAddedLen;

    idxData->newBlocklist =
        rm_realloc(idxData->newBlocklist, totalLen * sizeof(*idxData->newBlocklist));
    memcpy(idxData->newBlocklist + idxData->newBlocklistSize, (idx->blocks + info->nblocksOrig),
           newAddedLen * sizeof(*idxData->newBlocklist));

    rm_free(idx->blocks);
    idx->blocks = idxData->newBlocklist; // Consume new blocks array
    idxData->newBlocklist = NULL;
    idxData->newBlocklistSize += newAddedLen;
    idx->size = idxData->newBlocklistSize;
  } else if (idxData->numDelBlocks) {
    // if idxData->newBlocklist == NULL it's either because all the blocks the child has seen are gone or we didn't change the
    // size of the index (idxData->numDelBlocks == 0).
    // So if we enter here (idxData->numDelBlocks != 0) it's the first case, all blocks the child has seen need to be deleted.
    // Note that we might want to keep the last block, although deleted by the child. In this case numDelBlocks will *not include*
    // the last block.
    idx->size -= idxData->numDelBlocks;

    // There were new blocks added to the index in the main process while the child was running,
    // and/or we decided to ignore changes made to the last block, we copy the blocks data starting from
    // the first valid block we want to keep.

    memmove(idx->blocks, idx->blocks + idxData->numDelBlocks, sizeof(*idx->blocks) * idx->size);

    if (idx->size == 0) {
      InvertedIndex_AddBlock(idx, 0, (size_t*)(&info->nbytesAdded));
    }
  }

  // TODO : can we skip if we have newBlocklist?
  for (size_t i = 0; i < info->nblocksRepaired; ++i) {
    MSG_RepairedBlock *blockModified = idxData->changedBlocks + i;
    idx->blocks[blockModified->newix] = blockModified->blk;
  }
  // Consume changed blocks array
  rm_free(idxData->changedBlocks);
  idxData->changedBlocks = NULL;

  idx->numDocs -= info->ndocsCollected;
  idx->gcMarker++;
  RS_LOG_ASSERT(idx->size, "Index should have at least one block");
  idx->lastId = IndexBlock_LastId(&idx->blocks[idx->size - 1]); // Update lastId
}

typedef struct {
  // Node in the tree that was GC'd
  NumericRangeNode *node;
  InvIdxBuffers idxbufs;
  MSG_IndexInfo info;

  void *registersWithLastBlock;
  void *registersWithoutLastBlock; // In case the last block was modified
} NumGcInfo;

static int recvRegisters(ForkGC *fgc, NumGcInfo *ninfo) {
  if (FGC_recvFixed(fgc, ninfo->registersWithLastBlock, NR_REG_SIZE) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }
  return FGC_recvFixed(fgc, ninfo->registersWithoutLastBlock, NR_REG_SIZE);
}

static FGCError recvNumIdx(ForkGC *gc, NumGcInfo *ninfo) {
  if (FGC_recvFixed(gc, &ninfo->node, sizeof(ninfo->node)) != REDISMODULE_OK) {
    goto error;
  }
  if (ninfo->node == NULL) {
    return FGC_DONE;
  }

  if (FGC_recvInvIdx(gc, &ninfo->idxbufs, &ninfo->info) != REDISMODULE_OK) {
    goto error;
  }

  if (recvRegisters(gc, ninfo) != REDISMODULE_OK) {
    goto error;
  }
  return FGC_COLLECTED;

error:
  return FGC_CHILD_ERROR;
}

static void resetCardinality(NumGcInfo *info, NumericRange *range, size_t blocksSinceFork) {
  if (!info->idxbufs.lastBlockIgnored) {
    hll_set_registers(&range->hll, info->registersWithLastBlock, NR_REG_SIZE);
    if (blocksSinceFork == 0) {
      return; // No blocks were added since the fork. We're done
    }
  } else {
    hll_set_registers(&range->hll, info->registersWithoutLastBlock, NR_REG_SIZE);
    blocksSinceFork++; // Count the ignored block as well
  }
  // Add the entries that were added since the fork to the HLL
  RSIndexResult *cur;
  IndexReader *ir = NewMinimalNumericReader(range->entries, false);
  size_t startIdx = range->entries->size - blocksSinceFork; // Here `blocksSinceFork` > 0
  t_docId startId = IndexBlock_FirstId(&range->entries->blocks[startIdx]);
  int rc = IR_SkipTo(ir, startId, &cur);
  while (INDEXREAD_OK == rc) {
    hll_add(&range->hll, &cur->data.num.value, sizeof(cur->data.num.value));
    rc = IR_Read(ir, &cur);
  }
  IR_Free(ir);
}

static void applyNumIdx(ForkGC *gc, RedisSearchCtx *sctx, NumGcInfo *ninfo) {
  NumericRangeNode *currNode = ninfo->node;
  InvIdxBuffers *idxbufs = &ninfo->idxbufs;
  MSG_IndexInfo *info = &ninfo->info;
  size_t blocksSinceFork = currNode->range->entries->size - info->nblocksOrig; // record before applying changes
  FGC_applyInvertedIndex(gc, idxbufs, info, currNode->range->entries);
  currNode->range->entries->numEntries -= info->nentriesCollected;
  currNode->range->invertedIndexSize += info->nbytesAdded;
  currNode->range->invertedIndexSize -= info->nbytesCollected;

  FGC_updateStats(gc, sctx, info->nentriesCollected, info->nbytesCollected, info->nbytesAdded);

  resetCardinality(ninfo, currNode->range, blocksSinceFork);
}

static FGCError FGC_parentHandleTerms(ForkGC *gc) {
  FGCError status = FGC_COLLECTED;
  size_t len;
  char *term = NULL;
  if (FGC_recvBuffer(gc, (void **)&term, &len) != REDISMODULE_OK) {
    return FGC_CHILD_ERROR;
  }

  if (term == RECV_BUFFER_EMPTY) {
    return FGC_DONE;
  }

  InvIdxBuffers idxbufs = {0};
  MSG_IndexInfo info = {0};
  if (FGC_recvInvIdx(gc, &idxbufs, &info) != REDISMODULE_OK) {
    rm_free(term);
    return FGC_CHILD_ERROR;
  }

  StrongRef spec_ref = IndexSpecRef_Promote(gc->index);
  IndexSpec *sp = StrongRef_Get(spec_ref);
  if (!sp) {
    status = FGC_SPEC_DELETED;
    goto cleanup;
  }

  RedisSearchCtx sctx_ = SEARCH_CTX_STATIC(gc->ctx, sp);
  RedisSearchCtx *sctx = &sctx_;

  RedisSearchCtx_LockSpecWrite(sctx);

  InvertedIndex *idx = Redis_OpenInvertedIndex(sctx, term, len, DONT_CREATE_INDEX, NULL);

  if (idx == NULL) {
    status = FGC_PARENT_ERROR;
    goto cleanup;
  }

  FGC_applyInvertedIndex(gc, &idxbufs, &info, idx);

  if (idx->numDocs == 0) {

    // inverted index was cleaned entirely lets free it
    RedisModuleString *termKey = fmtRedisTermKey(sctx, term, len);
    size_t formatedTremLen;
    const char *formatedTrem = RedisModule_StringPtrLen(termKey, &formatedTremLen);
    if (sctx->spec->keysDict) {
      // get memory before deleting the inverted index
      size_t inv_idx_size = InvertedIndex_MemUsage(idx);
      if (dictDelete(sctx->spec->keysDict, termKey) == DICT_OK) {
        info.nbytesCollected += inv_idx_size;
      }
    }

    if (!Trie_Delete(sctx->spec->terms, term, len)) {
      const char* name = IndexSpec_FormatName(sctx->spec, RSGlobalConfig.hideUserDataFromLog);
      RedisModule_Log(sctx->redisCtx, "warning", "RedisSearch fork GC: deleting a term '%s' from"
                      " trie in index '%s' failed", RSGlobalConfig.hideUserDataFromLog ? Obfuscate_Text(term) : term, name);
    }
    sctx->spec->stats.numTerms--;
    sctx->spec->stats.termsSize -= len;
    RedisModule_FreeString(sctx->redisCtx, termKey);
    if (sctx->spec->suffix) {
      deleteSuffixTrie(sctx->spec->suffix, term, len);
    }
  }

  FGC_updateStats(gc, sctx, info.nentriesCollected, info.nbytesCollected, info.nbytesAdded);

cleanup:

  if (sp) {
    RedisSearchCtx_UnlockSpec(sctx);
    IndexSpecRef_Release(spec_ref);
  }
  rm_free(term);
  if (status != FGC_COLLECTED) {
    freeInvIdx(&idxbufs, &info);
  }
  return status;
}

static FGCError FGC_parentHandleNumeric(ForkGC *gc) {
  size_t fieldNameLen;
  char *fieldName = NULL;
  const FieldSpec *fs = NULL;
  RedisModuleString *keyName = NULL;
  uint64_t rtUniqueId;
  NumericRangeTree *rt = NULL;
  FGCError status = recvNumericTagHeader(gc, &fieldName, &fieldNameLen, &rtUniqueId);
  bool initialized = false;
  if (status == FGC_DONE) {
    return FGC_DONE;
  }

  NumGcInfo ninfo = {
    .registersWithLastBlock = rm_malloc(NR_REG_SIZE),
    .registersWithoutLastBlock = rm_malloc(NR_REG_SIZE),
  };
  while (status == FGC_COLLECTED) {
    // Read from GC process
    FGCError status2 = recvNumIdx(gc, &ninfo);
    if (status2 == FGC_DONE) {
      break;
    } else if (status2 != FGC_COLLECTED) {
      status = status2;
      break;
    }

    StrongRef spec_ref = IndexSpecRef_Promote(gc->index);
    IndexSpec *sp = StrongRef_Get(spec_ref);
    if (!sp) {
      status = FGC_SPEC_DELETED;
      goto loop_cleanup;
    }
    RedisSearchCtx _sctx = SEARCH_CTX_STATIC(gc->ctx, sp);
    RedisSearchCtx *sctx = &_sctx;

    RedisSearchCtx_LockSpecWrite(sctx);

    if (!initialized) {
      fs = IndexSpec_GetFieldWithLength(sctx->spec, fieldName, fieldNameLen);
      keyName = IndexSpec_GetFormattedKey(sctx->spec, fs, fs->types);
      rt = openNumericKeysDict(sctx->spec, keyName, DONT_CREATE_INDEX);
      initialized = true;
    }

    if (rt->uniqueId != rtUniqueId) {
      status = FGC_PARENT_ERROR;
      goto loop_cleanup;
    }

    if (!ninfo.node->range) {
      gc->stats.gcNumericNodesMissed++;
      goto loop_cleanup;
    }

    applyNumIdx(gc, sctx, &ninfo);
    rt->numEntries -= ninfo.info.nentriesCollected;
    rt->invertedIndexesSize -= ninfo.info.nbytesCollected;
    rt->invertedIndexesSize += ninfo.info.nbytesAdded;

    if (ninfo.node->range->entries->numDocs == 0) {
      rt->emptyLeaves++;
    }

  loop_cleanup:
    if (status != FGC_COLLECTED) {
      freeInvIdx(&ninfo.idxbufs, &ninfo.info);
    }
    if (sp) {
      RedisSearchCtx_UnlockSpec(sctx);
      IndexSpecRef_Release(spec_ref);
    }
  }

  rm_free(ninfo.registersWithLastBlock);
  rm_free(ninfo.registersWithoutLastBlock);
  rm_free(fieldName);

  if (status == FGC_COLLECTED && rt && gc->cleanNumericEmptyNodes) {
    // We need to have a valid strong reference to the spec in order to dereference rt
    StrongRef spec_ref = IndexSpecRef_Promote(gc->index);
    IndexSpec *sp = StrongRef_Get(spec_ref);
    if (!sp) return FGC_SPEC_DELETED;
    RedisSearchCtx sctx = SEARCH_CTX_STATIC(gc->ctx, sp);
    RedisSearchCtx_LockSpecWrite(&sctx);
    if (rt->emptyLeaves >= rt->numLeaves / 2) {
      NRN_AddRv rv = NumericRangeTree_TrimEmptyLeaves(rt);
      // rv.sz is the number of bytes added. Since we are cleaning empty leaves, it should be negative
      FGC_updateStats(gc, &sctx, 0, -rv.sz, 0);
    }
    RedisSearchCtx_UnlockSpec(&sctx);
    IndexSpecRef_Release(spec_ref);
  }

  return status;
}

static FGCError FGC_parentHandleTags(ForkGC *gc) {
  size_t fieldNameLen;
  char *fieldName;
  uint64_t tagUniqueId;
  InvertedIndex *value = NULL;
  FGCError status = recvNumericTagHeader(gc, &fieldName, &fieldNameLen, &tagUniqueId);

  while (status == FGC_COLLECTED) {
    RedisModuleString *keyName = NULL;
    MSG_IndexInfo info = {0};
    InvIdxBuffers idxbufs = {0};
    TagIndex *tagIdx = NULL;
    char *tagVal = NULL;
    size_t tagValLen;

    if (FGC_recvFixed(gc, &value, sizeof value) != REDISMODULE_OK) {
      status = FGC_CHILD_ERROR;
      break;
    }

    // No more tags values in tag field
    if (value == NULL) {
      RS_LOG_ASSERT(status == FGC_COLLECTED, "GC status is COLLECTED");
      break;
    }

    StrongRef spec_ref = IndexSpecRef_Promote(gc->index);
    IndexSpec *sp = StrongRef_Get(spec_ref);
    if (!sp) {
      status = FGC_SPEC_DELETED;
      break;
    }
    RedisSearchCtx _sctx = SEARCH_CTX_STATIC(gc->ctx, sp);
    RedisSearchCtx *sctx = &_sctx;

    if (FGC_recvBuffer(gc, (void **)&tagVal, &tagValLen) != REDISMODULE_OK) {
      status = FGC_CHILD_ERROR;
      goto loop_cleanup;
    }

    if (FGC_recvInvIdx(gc, &idxbufs, &info) != REDISMODULE_OK) {
      status = FGC_CHILD_ERROR;
      goto loop_cleanup;
    }

    RedisSearchCtx_LockSpecWrite(sctx);

    keyName = IndexSpec_GetFormattedKeyByName(sctx->spec, fieldName, INDEXFLD_T_TAG);
    tagIdx = TagIndex_Open(sctx->spec, keyName, DONT_CREATE_INDEX);

    if (tagIdx->uniqueId != tagUniqueId) {
      status = FGC_CHILD_ERROR;
      goto loop_cleanup;
    }

    size_t dummy_size;
    InvertedIndex *idx = TagIndex_OpenIndex(tagIdx, tagVal, tagValLen, DONT_CREATE_INDEX, &dummy_size);
    if (idx == TRIEMAP_NOTFOUND || idx != value) {
      status = FGC_PARENT_ERROR;
      goto loop_cleanup;
    }

    FGC_applyInvertedIndex(gc, &idxbufs, &info, idx);

    // if tag value is empty, let's remove it.
    if (idx->numDocs == 0) {
      // get memory before deleting the inverted index
      info.nbytesCollected += InvertedIndex_MemUsage(idx);
      TrieMap_Delete(tagIdx->values, tagVal, tagValLen, InvertedIndex_Free);

      if (tagIdx->suffix) {
        deleteSuffixTrieMap(tagIdx->suffix, tagVal, tagValLen);
      }
    }

    FGC_updateStats(gc, sctx, info.nentriesCollected, info.nbytesCollected, info.nbytesAdded);

  loop_cleanup:
    RedisSearchCtx_UnlockSpec(sctx);
    IndexSpecRef_Release(spec_ref);
    if (status != FGC_COLLECTED) {
      freeInvIdx(&idxbufs, &info);
    }
    if (tagVal) {
      rm_free(tagVal);
    }
  }

  rm_free(fieldName);
  return status;
}

static FGCError FGC_parentHandleMissingDocs(ForkGC *gc) {
  FGCError status = FGC_COLLECTED;
  size_t fieldNameLen;
  char *rawFieldName = NULL;

  if (FGC_recvBuffer(gc, (void **)&rawFieldName, &fieldNameLen) != REDISMODULE_OK) {
    return FGC_CHILD_ERROR;
  }

  if (rawFieldName == RECV_BUFFER_EMPTY) {
    return FGC_DONE;
  }

  InvIdxBuffers idxbufs = {0};
  MSG_IndexInfo info = {0};
  if (FGC_recvInvIdx(gc, &idxbufs, &info) != REDISMODULE_OK) {
    rm_free(rawFieldName);
    return FGC_CHILD_ERROR;
  }

  HiddenString *fieldName = NewHiddenString(rawFieldName, fieldNameLen, false);
  StrongRef spec_ref = IndexSpecRef_Promote(gc->index);
  IndexSpec *sp = StrongRef_Get(spec_ref);
  if (!sp) {
    status = FGC_SPEC_DELETED;
    goto cleanup;
  }

  RedisSearchCtx sctx_ = SEARCH_CTX_STATIC(gc->ctx, sp);
  RedisSearchCtx *sctx = &sctx_;

  RedisSearchCtx_LockSpecWrite(sctx);
  InvertedIndex *idx = dictFetchValue(sctx->spec->missingFieldDict, fieldName);

  if (idx == NULL) {
    status = FGC_PARENT_ERROR;
    goto cleanup;
  }

  FGC_applyInvertedIndex(gc, &idxbufs, &info, idx);

  if (idx->numDocs == 0) {
    // inverted index was cleaned entirely lets free it
    if (sctx->spec->missingFieldDict) {
      info.nbytesCollected += InvertedIndex_MemUsage(idx);
      dictDelete(sctx->spec->missingFieldDict, fieldName);
    }
  }
  FGC_updateStats(gc, sctx, info.nentriesCollected, info.nbytesCollected, info.nbytesAdded);

cleanup:

  if (sp) {
    RedisSearchCtx_UnlockSpec(sctx);
    IndexSpecRef_Release(spec_ref);
  }
  HiddenString_Free(fieldName, false);
  rm_free(rawFieldName);
  if (status != FGC_COLLECTED) {
    freeInvIdx(&idxbufs, &info);
  }
  return status;
}

static FGCError FGC_parentHandleExistingDocs(ForkGC *gc) {
  FGCError status = FGC_COLLECTED;

  size_t ei_len;
  char *empty_indicator = NULL;

  if (FGC_recvBuffer(gc, (void **)&empty_indicator, &ei_len) != REDISMODULE_OK) {
    return FGC_CHILD_ERROR;
  }

  if (empty_indicator == RECV_BUFFER_EMPTY) {
    return FGC_DONE;
  }

  InvIdxBuffers idxbufs = {0};
  MSG_IndexInfo info = {0};
  if (FGC_recvInvIdx(gc, &idxbufs, &info) != REDISMODULE_OK) {
    rm_free(empty_indicator);
    return FGC_CHILD_ERROR;
  }

  StrongRef spec_ref = IndexSpecRef_Promote(gc->index);
  IndexSpec *sp = StrongRef_Get(spec_ref);
  if (!sp) {
    status = FGC_SPEC_DELETED;
    goto cleanup;
  }

  RedisSearchCtx sctx_ = SEARCH_CTX_STATIC(gc->ctx, sp);
  RedisSearchCtx *sctx = &sctx_;

  RedisSearchCtx_LockSpecWrite(sctx);

  InvertedIndex *idx = sp->existingDocs;

  FGC_applyInvertedIndex(gc, &idxbufs, &info, idx);
  // We don't count the records that we removed, because we also don't count
  // their addition (they are duplications so we have no such desire).

  if (idx->numDocs == 0) {
    // inverted index was cleaned entirely, let's free it
    info.nbytesCollected += InvertedIndex_MemUsage(idx);
    InvertedIndex_Free(idx);
    sp->existingDocs = NULL;
  }
  FGC_updateStats(gc, sctx, 0, info.nbytesCollected, info.nbytesAdded);

cleanup:
  rm_free(empty_indicator);
  if (sp) {
    RedisSearchCtx_UnlockSpec(sctx);
    IndexSpecRef_Release(spec_ref);
  }
  if (status != FGC_COLLECTED)  {
    freeInvIdx(&idxbufs, &info);
  }
  return status;
}

FGCError FGC_parentHandleFromChild(ForkGC *gc) {
  FGCError status = FGC_COLLECTED;
  RedisModule_Log(gc->ctx, "debug", "ForkGC - parent start applying changes");

#define COLLECT_FROM_CHILD(e)               \
  while ((status = (e)) == FGC_COLLECTED) { \
  }                                         \
  if (status != FGC_DONE) {                 \
    return status;                          \
  }

  COLLECT_FROM_CHILD(FGC_parentHandleTerms(gc));
  COLLECT_FROM_CHILD(FGC_parentHandleNumeric(gc));
  COLLECT_FROM_CHILD(FGC_parentHandleTags(gc));
  COLLECT_FROM_CHILD(FGC_parentHandleMissingDocs(gc));
  COLLECT_FROM_CHILD(FGC_parentHandleExistingDocs(gc));
  RedisModule_Log(gc->ctx, "debug", "ForkGC - parent ends applying changes");

  return status;
}

// GIL must be held before calling this function
static inline bool isOutOfMemory(RedisModuleCtx *ctx) {
  #define MIN_NOT_0(a,b) (((a)&&(b))?MIN((a),(b)):MAX((a),(b)))
  RedisModuleServerInfoData *info = RedisModule_GetServerInfo(ctx, "memory");

  size_t maxmemory = RedisModule_ServerInfoGetFieldUnsigned(info, "maxmemory", NULL);
  size_t max_process_mem = RedisModule_ServerInfoGetFieldUnsigned(info, "max_process_mem", NULL); // Enterprise limit
  maxmemory = MIN_NOT_0(maxmemory, max_process_mem);

  size_t total_system_memory = RedisModule_ServerInfoGetFieldUnsigned(info, "total_system_memory", NULL);
  maxmemory = MIN_NOT_0(maxmemory, total_system_memory);

  size_t used_memory = RedisModule_ServerInfoGetFieldUnsigned(info, "used_memory", NULL);

  RedisModule_FreeServerInfo(ctx, info);

  RedisModule_Log(ctx, "debug", "ForkGC - checking memory: maxmemory=%zu, used_memory=%zu", maxmemory, used_memory);
  return used_memory > maxmemory;
}

static int periodicCb(void *privdata) {
  ForkGC *gc = privdata;
  RedisModuleCtx *ctx = gc->ctx;

  // This check must be done first, because some values (like `deletedDocsFromLastRun`) that are used for
  // early termination might never change after index deletion and will cause periodicCb to always return 1,
  // which will cause the GC to never stop rescheduling itself.
  // If the index was deleted, we don't want to reschedule the GC, so we return 0.
  // If the index is still valid, we MUST hold the strong reference to it until after the fork, to make sure
  // the child process has a valid reference to the index.
  // If we were to try and revalidate the index after the fork, it might already be dropped and the child
  // will exit before sending any data, and might left the parent waiting for data that will never arrive.
  // Attempting to revalidate the index after the fork is also problematic because the parent and child are
  // not synchronized, and the parent might see the index alive while the child sees it as deleted.
  StrongRef early_check = IndexSpecRef_Promote(gc->index);
  if (!StrongRef_Get(early_check)) {
    // Index was deleted
    return 0;
  }

  if (gc->deletedDocsFromLastRun < RSGlobalConfig.gcConfigParams.forkGc.forkGcCleanThreshold) {
    IndexSpecRef_Release(early_check);
    return 1;
  }

  int gcrv = 1;
  pid_t cpid;
  TimeSample ts;

  while (gc->pauseState == FGC_PAUSED_CHILD) {
    gc->execState = FGC_STATE_WAIT_FORK;
    // spin or sleep
    usleep(500);
  }

  pid_t ppid_before_fork = getpid();

  TimeSampler_Start(&ts);
  int pipefd[2];
  int rc = pipe(pipefd);  // create the pipe
  if (rc == -1) {
    RedisModule_Log(ctx, "warning", "Couldn't create pipe - got errno %d, aborting fork GC", errno);
    IndexSpecRef_Release(early_check);
    return 1;
  }
  gc->pipe_read_fd = pipefd[GC_READERFD];
  gc->pipe_write_fd = pipefd[GC_WRITERFD];
  // initialize the pollfd for the read pipe
  gc->pollfd_read[0].fd = gc->pipe_read_fd;
  gc->pollfd_read[0].events = POLLIN;

  // We need to acquire the GIL to use the fork api
  RedisModule_ThreadSafeContextLock(ctx);

  // Check if we are out of memory before even trying to fork
  if (isOutOfMemory(ctx)) {
    RedisModule_Log(ctx, "warning", "Not enough memory for GC fork, skipping GC job");
    gc->retryInterval.tv_sec = RSGlobalConfig.gcConfigParams.forkGc.forkGcRetryInterval;
    IndexSpecRef_Release(early_check);
    RedisModule_ThreadSafeContextUnlock(ctx);
    return 1;
  }

  gc->execState = FGC_STATE_SCANNING;

  cpid = RedisModule_Fork(NULL, NULL);  // duplicate the current process

  if (cpid == -1) {
    RedisModule_Log(ctx, "warning", "fork failed - got errno %d, aborting fork GC", errno);
    gc->retryInterval.tv_sec = RSGlobalConfig.gcConfigParams.forkGc.forkGcRetryInterval;
    IndexSpecRef_Release(early_check);

    RedisModule_ThreadSafeContextUnlock(ctx);

    close(gc->pipe_read_fd);
    close(gc->pipe_write_fd);

    return 1;
  }

  // Now that we hold the GIL, we can cache this value knowing it won't change by the main thread
  // upon deleting a document (this is the actual number of documents to be cleaned by the fork).
  size_t num_docs_to_clean = gc->deletedDocsFromLastRun;
  gc->deletedDocsFromLastRun = 0;

  gc->retryInterval.tv_sec = RSGlobalConfig.gcConfigParams.forkGc.forkGcRunIntervalSec;

  RedisModule_ThreadSafeContextUnlock(ctx);


  if (cpid == 0) {
    // fork process
    setpriority(PRIO_PROCESS, getpid(), 19);
    close(gc->pipe_read_fd);
    // Pass the index to the child process
    FGC_childScanIndexes(gc, StrongRef_Get(early_check));
    close(gc->pipe_write_fd);
    sleep(RSGlobalConfig.gcConfigParams.forkGc.forkGcSleepBeforeExit);
    RedisModule_ExitFromChild(EXIT_SUCCESS);
  } else {
    // main process
    // release the strong reference to the index for the main process (see comment above)
    IndexSpecRef_Release(early_check);
    close(gc->pipe_write_fd);
    while (gc->pauseState == FGC_PAUSED_PARENT) {
      gc->execState = FGC_STATE_WAIT_APPLY;
      // spin
      usleep(500);
    }

    gc->execState = FGC_STATE_APPLYING;
    gc->cleanNumericEmptyNodes = RSGlobalConfig.gcConfigParams.forkGc.forkGCCleanNumericEmptyNodes;
    if (FGC_parentHandleFromChild(gc) == FGC_SPEC_DELETED) {
      gcrv = 0;
    }
    close(gc->pipe_read_fd);
    // give the child some time to exit gracefully
    for (int attempt = 0; attempt < GC_WAIT_ATTEMPTS; ++attempt) {
      if (waitpid(cpid, NULL, WNOHANG) == 0) {
        usleep(500);
      }
    }
    // KillForkChild must be called when holding the GIL
    // otherwise it might cause a pipe leak and eventually run
    // out of file descriptor
    RedisModule_ThreadSafeContextLock(ctx);
    RedisModule_KillForkChild(cpid);
    RedisModule_ThreadSafeContextUnlock(ctx);

    if (gcrv) {
      gcrv = VecSim_CallTieredIndexesGC(gc->index);
    }
  }

  IndexsGlobalStats_UpdateLogicallyDeleted(-num_docs_to_clean);
  gc->execState = FGC_STATE_IDLE;
  TimeSampler_End(&ts);
  long long msRun = TimeSampler_DurationMS(&ts);

  gc->stats.numCycles++;
  gc->stats.totalMSRun += msRun;
  gc->stats.lastRunTimeMs = msRun;

  return gcrv;
}

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define NO_TSAN_CHECK __attribute__((no_sanitize("thread")))
#endif
#endif
#ifndef NO_TSAN_CHECK
#define NO_TSAN_CHECK
#endif

void FGC_WaitBeforeFork(ForkGC *gc) NO_TSAN_CHECK {
  RS_LOG_ASSERT(gc->pauseState == 0, "FGC pause state should be 0");
  gc->pauseState = FGC_PAUSED_CHILD;

  while (gc->execState != FGC_STATE_WAIT_FORK) {
    usleep(500);
  }
}

void FGC_ForkAndWaitBeforeApply(ForkGC *gc) NO_TSAN_CHECK {
  // Ensure that we're waiting for the child to begin
  RS_LOG_ASSERT(gc->pauseState == FGC_PAUSED_CHILD, "FGC pause state should be CHILD");
  RS_LOG_ASSERT(gc->execState == FGC_STATE_WAIT_FORK, "FGC exec state should be WAIT_FORK");

  gc->pauseState = FGC_PAUSED_PARENT;
  while (gc->execState != FGC_STATE_WAIT_APPLY) {
    usleep(500);
  }
}

void FGC_Apply(ForkGC *gc) NO_TSAN_CHECK {
  gc->pauseState = FGC_PAUSED_UNPAUSED;
  while (gc->execState != FGC_STATE_IDLE) {
    usleep(500);
  }
}

static void onTerminateCb(void *privdata) {
  ForkGC *gc = privdata;
  IndexsGlobalStats_UpdateLogicallyDeleted(-gc->deletedDocsFromLastRun);
  WeakRef_Release(gc->index);
  RedisModule_FreeThreadSafeContext(gc->ctx);
  rm_free(gc);
}

static void statsCb(RedisModule_Reply *reply, void *gcCtx) {
#define REPLY_KVNUM(k, v) RedisModule_ReplyKV_Double(reply, (k), (v))
  ForkGC *gc = gcCtx;
  if (!gc) return;
  REPLY_KVNUM("bytes_collected", gc->stats.totalCollected);
  REPLY_KVNUM("total_ms_run", gc->stats.totalMSRun);
  REPLY_KVNUM("total_cycles", gc->stats.numCycles);
  REPLY_KVNUM("average_cycle_time_ms", (double)gc->stats.totalMSRun / gc->stats.numCycles);
  REPLY_KVNUM("last_run_time_ms", (double)gc->stats.lastRunTimeMs);
  REPLY_KVNUM("gc_numeric_trees_missed", (double)gc->stats.gcNumericNodesMissed);
  REPLY_KVNUM("gc_blocks_denied", (double)gc->stats.gcBlocksDenied);
}

#ifdef FTINFO_FOR_INFO_MODULES
static void statsForInfoCb(RedisModuleInfoCtx *ctx, void *gcCtx) {
  ForkGC *gc = gcCtx;
  RedisModule_InfoBeginDictField(ctx, "gc_stats");
  RedisModule_InfoAddFieldLongLong(ctx, "bytes_collected", gc->stats.totalCollected);
  RedisModule_InfoAddFieldLongLong(ctx, "total_ms_run", gc->stats.totalMSRun);
  RedisModule_InfoAddFieldLongLong(ctx, "total_cycles", gc->stats.numCycles);
  RedisModule_InfoAddFieldDouble(ctx, "average_cycle_time_ms", (double)gc->stats.totalMSRun / gc->stats.numCycles);
  RedisModule_InfoAddFieldDouble(ctx, "last_run_time_ms", (double)gc->stats.lastRunTimeMs);
  RedisModule_InfoAddFieldDouble(ctx, "gc_numeric_trees_missed", (double)gc->stats.gcNumericNodesMissed);
  RedisModule_InfoAddFieldDouble(ctx, "gc_blocks_denied", (double)gc->stats.gcBlocksDenied);
  RedisModule_InfoEndDictField(ctx);
}
#endif

static void deleteCb(void *ctx) {
  ForkGC *gc = ctx;
  ++gc->deletedDocsFromLastRun;
  IndexsGlobalStats_UpdateLogicallyDeleted(1);
}

static struct timespec getIntervalCb(void *ctx) {
  ForkGC *gc = ctx;
  return gc->retryInterval;
}

ForkGC *FGC_New(StrongRef spec_ref, GCCallbacks *callbacks) {
  ForkGC *forkGc = rm_calloc(1, sizeof(*forkGc));
  *forkGc = (ForkGC){
      .index = StrongRef_Demote(spec_ref),
      .deletedDocsFromLastRun = 0,
  };
  forkGc->retryInterval.tv_sec = RSGlobalConfig.gcConfigParams.forkGc.forkGcRunIntervalSec;
  forkGc->retryInterval.tv_nsec = 0;

  forkGc->cleanNumericEmptyNodes = RSGlobalConfig.gcConfigParams.forkGc.forkGCCleanNumericEmptyNodes;
  forkGc->ctx = RedisModule_GetDetachedThreadSafeContext(RSDummyContext);

  callbacks->onTerm = onTerminateCb;
  callbacks->periodicCallback = periodicCb;
  callbacks->renderStats = statsCb;
  #ifdef FTINFO_FOR_INFO_MODULES
  callbacks->renderStatsForInfo = statsForInfoCb;
  #endif
  callbacks->getInterval = getIntervalCb;
  callbacks->onDelete = deleteCb;

  return forkGc;
}
