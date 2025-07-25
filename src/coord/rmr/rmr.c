/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#include "rmr.h"
#include "reply.h"
#include "reply_macros.h"
#include "redismodule.h"
#include "module.h"
#include "cluster.h"
#include "chan.h"
#include "rq.h"
#include "rmutil/rm_assert.h"
#include "resp3.h"
#include "coord/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/param.h>

#include "hiredis/hiredis.h"
#include "hiredis/async.h"
#include "io_runtime_ctx.h"

#define REFCOUNT_INCR_MSG(caller, refcount) \
  RS_DEBUG_LOG_FMT("%s: increased refCount to == %d", caller, refcount);
#define REFCOUNT_DECR_MSG(caller, refcount) \
  RS_DEBUG_LOG_FMT("%s: decreased refCount to == %d", caller, refcount);

#define CEIL_DIV(a, b) ((a + b - 1) / b)

/* A cluster is a pool of IORuntimes. It is owned by the main thread and accessed in the coordinator threads */
static MRCluster *cluster_g = NULL;

/* Coordination request timeout */
long long timeout_g = 5000; // unused value. will be set in MR_Init

/* MapReduce context for a specific command's execution */
typedef struct MRCtx {
  int numReplied;
  int numExpected;
  int numErrored;
  int repliesCap;
  MRReply **replies;
  MRReduceFunc reducer;
  void *privdata;
  RedisModuleCtx *redisCtx;
  RedisModuleBlockedClient *bc;
  bool mastersOnly;
  MRCommand cmd;
  IORuntimeCtx *ioRuntime;

  /**
   * This is a reduce function inside the MRCtx.
   * if set when replies will arrive we will not
   * unblock the client and instead the reduce function
   * will be called directly. This mechanism allows us to
   * send commands and base on the response send more commands
   * and do more aggregations. Only the last command/commands sent
   * needs to unblock the client.
   */
  MRReduceFunc fn;
} MRCtx;

void MR_SetCoordinationStrategy(MRCtx *ctx, bool mastersOnly) {
  ctx->mastersOnly = mastersOnly;
}

/* Create a new MapReduce context */
MRCtx *MR_CreateCtx(RedisModuleCtx *ctx, RedisModuleBlockedClient *bc, void *privdata, int replyCap) {
  RS_ASSERT(cluster_g);
  MRCtx *ret = rm_malloc(sizeof(MRCtx));
  ret->numReplied = 0;
  ret->numErrored = 0;
  ret->numExpected = 0;
  ret->repliesCap = replyCap;
  ret->replies = rm_calloc(ret->repliesCap, sizeof(redisReply *));
  ret->reducer = NULL;
  ret->privdata = privdata;
  ret->mastersOnly = true; // default to masters only
  ret->redisCtx = ctx;
  ret->bc = bc;
  RS_ASSERT(ctx || bc);
  ret->fn = NULL;
  ret->ioRuntime = MRCluster_GetIORuntimeCtx(cluster_g, MRCluster_AssignRoundRobinIORuntimeIdx(cluster_g));
  return ret;
}

void MRCtx_Free(MRCtx *ctx) {

  MRCommand_Free(&ctx->cmd);

  for (int i = 0; i < ctx->numReplied; i++) {
    if (ctx->replies[i] != NULL) {
      MRReply_Free(ctx->replies[i]);
      ctx->replies[i] = NULL;
    }
  }
  rm_free(ctx->replies);

  // free the context
  rm_free(ctx);
}

/* Get the user stored private data from the context */
void *MRCtx_GetPrivData(struct MRCtx *ctx) {
  return ctx->privdata;
}

int MRCtx_GetNumReplied(struct MRCtx *ctx) {
  return ctx->numReplied;
}

void MRCtx_RequestCompleted(struct MRCtx *ctx) {
  IORuntimeCtx_RequestCompleted(ctx->ioRuntime);
}

MRReply** MRCtx_GetReplies(struct MRCtx *ctx) {
  return ctx->replies;
}

RedisModuleCtx *MRCtx_GetRedisCtx(struct MRCtx *ctx) {
  return ctx->redisCtx;
}

RedisModuleBlockedClient *MRCtx_GetBlockedClient(struct MRCtx *ctx) {
  return ctx->bc;
}

void MRCtx_SetReduceFunction(struct MRCtx *ctx, MRReduceFunc fn) {
  ctx->fn = fn;
}

static void freePrivDataCB(RedisModuleCtx *ctx, void *p) {
  if (p) {
    MRCtx *mc = p;
    IORuntimeCtx_RequestCompleted(mc->ioRuntime);
    MRCtx_Free(mc);
  }
}

static int timeoutHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  RedisModule_Log(ctx, "notice", "Timed out coordination request");
  return RedisModule_ReplyWithError(ctx, "Timeout calling command");
}

/* handler for unblocking redis commands, that calls the actual reducer */
static int unblockHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  RS_AutoMemory(ctx);
  MRCtx *mc = RedisModule_GetBlockedClientPrivateData(ctx);

  mc->redisCtx = ctx;

  return mc->reducer(mc, mc->numReplied, mc->replies);
}

/* The callback called from each fanout request to aggregate their replies */
static void fanoutCallback(redisAsyncContext *c, void *r, void *privdata) {
  MRCtx *ctx = privdata;

  if (!r) {
    ctx->numErrored++;

  } else {
    /* If needed - double the capacity for replies */
    if (ctx->numReplied == ctx->repliesCap) {
      ctx->repliesCap *= 2;
      ctx->replies = rm_realloc(ctx->replies, ctx->repliesCap * sizeof(MRReply *));
    }
    ctx->replies[ctx->numReplied++] = r;
  }

  // If we've received the last reply - unblock the client
  if (ctx->numReplied + ctx->numErrored == ctx->numExpected) {
    if (ctx->fn) {
      ctx->fn(ctx, ctx->numReplied, ctx->replies);
    } else {
      RedisModuleBlockedClient *bc = ctx->bc;
      RS_ASSERT(bc);
      RedisModule_BlockedClientMeasureTimeEnd(bc);
      RedisModule_UnblockClient(bc, ctx);
    }
  }
}

/* Initialize the MapReduce engine with a node provider */
void MR_Init(size_t num_io_threads, size_t conn_pool_size, long long timeoutMS) {
  cluster_g = MR_NewCluster(NULL, conn_pool_size, num_io_threads);
  timeout_g = timeoutMS;
}

/* The fanout request received in the event loop in a thread safe manner */
static void uvFanoutRequest(void *p) {
  MRCtx *mrctx = p;
  IORuntimeCtx *ioRuntime = mrctx->ioRuntime;

  mrctx->numExpected =
      MRCluster_FanoutCommand(ioRuntime, mrctx->mastersOnly, &mrctx->cmd, fanoutCallback, mrctx);

  if (mrctx->numExpected == 0) {
    RedisModuleBlockedClient *bc = mrctx->bc;
    RS_ASSERT(bc);
    RedisModule_BlockedClientMeasureTimeEnd(bc);
    RedisModule_UnblockClient(bc, mrctx);
  }
}

// This function already runs in one of the IO threads. We need to make sure that the adequate RuntimeCtx is used. This info can be found in the MRCtx
static void uvMapRequest(void *p) {
  MRCtx *mrctx = p;
  IORuntimeCtx *ioRuntime = mrctx->ioRuntime;

  int rc = MRCluster_SendCommand(ioRuntime, mrctx->mastersOnly, &mrctx->cmd, fanoutCallback, mrctx);
  mrctx->numExpected = (rc == REDIS_OK) ? 1 : 0;

  if (mrctx->numExpected == 0) {
    RedisModuleBlockedClient *bc = mrctx->bc;
    RS_ASSERT(bc);
    RedisModule_BlockedClientMeasureTimeEnd(bc);
    RedisModule_UnblockClient(bc, mrctx);
  }
}

/* Fanout map - send the same command to all the shards, sending the collective
 * reply to the reducer callback */
int MR_Fanout(struct MRCtx *mrctx, MRReduceFunc reducer, MRCommand cmd, bool block) {
  if (block) {
    RS_ASSERT(!mrctx->bc);
    mrctx->bc = RedisModule_BlockClient(
        mrctx->redisCtx, unblockHandler, timeoutHandler, freePrivDataCB, 0); // timeout_g);
    RedisModule_BlockedClientMeasureTimeStart(mrctx->bc);
  }
  //Is possible that mrctx->fn may already be there and reducer to be null
  mrctx->reducer = reducer;
  mrctx->cmd = cmd;


  IORuntimeCtx_Schedule(mrctx->ioRuntime, uvFanoutRequest, mrctx);
  return REDIS_OK;
}

int MR_MapSingle(struct MRCtx *ctx, MRReduceFunc reducer, MRCommand cmd) {
  ctx->reducer = reducer;
  ctx->cmd = cmd;
  RS_ASSERT(!ctx->bc);
  ctx->bc = RedisModule_BlockClient(ctx->redisCtx, unblockHandler, timeoutHandler, freePrivDataCB, 0); // timeout_g);
  RedisModule_BlockedClientMeasureTimeStart(ctx->bc);
  IORuntimeCtx_Schedule(ctx->ioRuntime, uvMapRequest, ctx);
  return REDIS_OK;
}

/* on-loop update topology request. This can't be done from the main thread */
static void uvUpdateTopologyRequest(void *p) {
  struct UpdateTopologyCtx *ctx = p;
  IORuntimeCtx *ioRuntime = ctx->ioRuntime;
  MRClusterTopology *old_topo = ioRuntime->topo;
  ioRuntime->topo = ctx->new_topo;
  IORuntimeCtx_UpdateNodesAndConnectAll(ioRuntime);
  rm_free(ctx);
  if (old_topo) {
    MRClusterTopology_Free(old_topo);
  }
}

/* Set a new topology for the cluster.*/
void MR_UpdateTopology(MRClusterTopology *newTopo) {
  for (size_t i = 0; i < cluster_g->num_io_threads; i++) {
    IORuntimeCtx_Schedule_Topology(cluster_g->io_runtimes_pool[i], uvUpdateTopologyRequest, newTopo, i == 0);
  }
}

struct UpdateConnPoolSizeCtx {
  IORuntimeCtx *ioRuntime;
  size_t conn_pool_size;
};

/* Modifying the connection pools cannot be done from the main thread */
static void uvUpdateConnPoolSize(void *p) {
  struct UpdateConnPoolSizeCtx *ctx = p;
  IORuntimeCtx *ioRuntime = ctx->ioRuntime;
  IORuntimeCtx_UpdateConnPoolSize(ioRuntime, ctx->conn_pool_size);
  size_t max_pending = ioRuntime->conn_mgr.nodeConns * PENDING_FACTOR;
  RQ_UpdateMaxPending(ioRuntime->queue, max_pending);
  IORuntimeCtx_RequestCompleted(ioRuntime);
  rm_free(ctx);
}

extern size_t NumShards;
void MR_UpdateConnPoolSize(size_t conn_pool_size) {
  if (!cluster_g) return; // not initialized yet, we have nothing to update yet.
  if (NumShards == 1) {
    // If we observe that there is only one shard from the main thread,
    // we know the uv thread is not initialized yet (and may never be).
    // We can update the connection pool size directly from the main thread.
    // This is mostly a no-op, as the connection pool is not in use (yet or at all).
    // This call should only update the connection pool `size` for when the connection pool is initialized.
    for (size_t i = 0; i < cluster_g->num_io_threads; i++) {
      IORuntimeCtx_UpdateConnPoolSize(cluster_g->io_runtimes_pool[i], conn_pool_size);
    }
  } else {
    for (size_t i = 0; i < cluster_g->num_io_threads; i++) {
      struct UpdateConnPoolSizeCtx *ctx = rm_malloc(sizeof(*ctx));
      ctx->ioRuntime = cluster_g->io_runtimes_pool[i];
      ctx->conn_pool_size = conn_pool_size;
      IORuntimeCtx_Schedule(cluster_g->io_runtimes_pool[i], uvUpdateConnPoolSize, ctx);
    }
  }
}

struct ReplyClusterInfoCtx {
  IORuntimeCtx *ioRuntime;
  RedisModuleBlockedClient *bc;
};

struct MultiThreadedRedisBlockedCtx {
  RedisModuleBlockedClient *bc;
  size_t pending_threads;
  size_t num_io_threads;
  pthread_mutex_t lock;
  // Accumulate partial replies
  dict *replyDict;
};

struct ReducedConnPoolStateCtx {
  IORuntimeCtx *ioRuntime;
  struct MultiThreadedRedisBlockedCtx *mt_ctx;
};

static void uvGetConnectionPoolState(void *p) {
  struct ReducedConnPoolStateCtx *reducedConnPoolStateCtx = p;
  IORuntimeCtx *ioRuntime = reducedConnPoolStateCtx->ioRuntime;
  struct MultiThreadedRedisBlockedCtx *mt_bc = reducedConnPoolStateCtx->mt_ctx;
  RedisModuleBlockedClient *bc = mt_bc->bc;
  RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(bc);
  pthread_mutex_lock(&mt_bc->lock);
  MRConnManager_FillStateDict(&ioRuntime->conn_mgr, mt_bc->replyDict);
  mt_bc->pending_threads--;
  if (mt_bc->pending_threads == 0) {
    // We are the last ones to reply, so we can now send the response
    MRConnManager_ReplyState(mt_bc->replyDict, ctx);
    pthread_mutex_unlock(&mt_bc->lock);
    RedisModule_FreeThreadSafeContext(ctx);
    RedisModule_BlockedClientMeasureTimeEnd(bc);
    RedisModule_UnblockClient(bc, NULL);
    IORuntimeCtx_RequestCompleted(ioRuntime);
    pthread_mutex_destroy(&mt_bc->lock);
    dictRelease(mt_bc->replyDict);
    rm_free(mt_bc);
  } else {
    pthread_mutex_unlock(&mt_bc->lock);
    RedisModule_FreeThreadSafeContext(ctx);
  }

  rm_free(reducedConnPoolStateCtx);
}

void MR_GetConnectionPoolState(RedisModuleCtx *ctx) {
  RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
  RedisModule_BlockedClientMeasureTimeStart(bc);
  struct MultiThreadedRedisBlockedCtx *mt_bc = rm_new(struct MultiThreadedRedisBlockedCtx);
  mt_bc->num_io_threads = cluster_g->num_io_threads;
  mt_bc->pending_threads = cluster_g->num_io_threads;
  mt_bc->replyDict = dictCreate(&dictTypeHeapStringsListVal, NULL);
  mt_bc->bc = bc;
  pthread_mutex_init(&mt_bc->lock, NULL);
  for (size_t i = 0; i < cluster_g->num_io_threads; i++) {
    struct ReducedConnPoolStateCtx *reducedConnPoolStateCtx = rm_new(struct ReducedConnPoolStateCtx);
    reducedConnPoolStateCtx->ioRuntime = cluster_g->io_runtimes_pool[i];
    reducedConnPoolStateCtx->mt_ctx = mt_bc;
    IORuntimeCtx_Schedule(cluster_g->io_runtimes_pool[i], uvGetConnectionPoolState, reducedConnPoolStateCtx);
  }
}

static void uvReplyClusterInfo(void *p) {
  struct ReplyClusterInfoCtx *replyClusterInfoCtx = p;
  IORuntimeCtx *ioRuntime = replyClusterInfoCtx->ioRuntime;
  RedisModuleBlockedClient *bc = replyClusterInfoCtx->bc;
  RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(bc);
  MR_ReplyClusterInfo(ctx, ioRuntime->topo);
  IORuntimeCtx_RequestCompleted(ioRuntime);
  RedisModule_FreeThreadSafeContext(ctx);
  RedisModule_BlockedClientMeasureTimeEnd(bc);
  RedisModule_UnblockClient(bc, NULL);
  rm_free(replyClusterInfoCtx);
}

void MR_uvReplyClusterInfo(RedisModuleCtx *ctx) {
  RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
  RedisModule_BlockedClientMeasureTimeStart(bc);
  struct ReplyClusterInfoCtx *replyClusterInfoCtx = rm_new(struct ReplyClusterInfoCtx);
  size_t idx = MRCluster_AssignRoundRobinIORuntimeIdx(cluster_g);
  replyClusterInfoCtx->bc = bc;
  replyClusterInfoCtx->ioRuntime = cluster_g->io_runtimes_pool[idx];
  IORuntimeCtx_Schedule(replyClusterInfoCtx->ioRuntime, uvReplyClusterInfo, replyClusterInfoCtx);
}

void MR_ReplyClusterInfo(RedisModuleCtx *ctx, MRClusterTopology *topo) {
  RedisModule_Reply _reply = RedisModule_NewReply(ctx), *reply = &_reply;

  const char *hash_func_str;
  switch (topo ? topo->hashFunc : MRHashFunc_None) {
  case MRHashFunc_CRC12:
    hash_func_str = MRHASHFUNC_CRC12_STR;
    break;
  case MRHashFunc_CRC16:
    hash_func_str = MRHASHFUNC_CRC16_STR;
    break;
  default:
    hash_func_str = "n/a";
    break;
  }
  const char *cluster_type_str = clusterConfig.type == ClusterType_RedisOSS ? CLUSTER_TYPE_OSS : CLUSTER_TYPE_RLABS;
  size_t partitions = topo ? topo->numShards : 0;

  //-------------------------------------------------------------------------------------------
  if (reply->resp3) { // RESP3 variant
    RedisModule_Reply_Map(reply); // root

    RedisModule_ReplyKV_LongLong(reply, "num_partitions", partitions);
    RedisModule_ReplyKV_SimpleString(reply, "cluster_type", cluster_type_str);

    RedisModule_ReplyKV_SimpleString(reply, "hash_func", hash_func_str);

    // Report topology
    RedisModule_ReplyKV_LongLong(reply, "num_slots", topo ? (long long)topo->numSlots : 0);

    if (!topo) {
      RedisModule_ReplyKV_Null(reply, "slots");
    } else {
      RedisModule_ReplyKV_Array(reply, "slots"); // >slots
      for (int i = 0; i < topo->numShards; i++) {
        MRClusterShard *sh = &topo->shards[i];

        RedisModule_Reply_Map(reply); // >>(shards)
        RedisModule_ReplyKV_LongLong(reply, "start", sh->startSlot);
        RedisModule_ReplyKV_LongLong(reply, "end", sh->endSlot);

        RedisModule_ReplyKV_Array(reply, "nodes"); // >>>nodes
        for (int j = 0; j < sh->numNodes; j++) {
          MRClusterNode *node = &sh->nodes[j];
          RedisModule_Reply_Map(reply); // >>>>(node)

          REPLY_KVSTR_SAFE("id", node->id);
          REPLY_KVSTR_SAFE("host", node->endpoint.host);
          RedisModule_ReplyKV_LongLong(reply, "port", node->endpoint.port);
          RedisModule_ReplyKV_SimpleStringf(reply, "role", "%s%s",                        // TODO: move the space to "self"
                                      node->flags & MRNode_Master ? "master " : "slave ", // "master" : "slave",
                                      node->flags & MRNode_Self ? "self" : "");           // " self" : ""

          RedisModule_Reply_MapEnd(reply); // >>>>(node)
        }
        RedisModule_Reply_ArrayEnd(reply); // >>>nodes

        RedisModule_Reply_MapEnd(reply); // >>(shards)
      }
      RedisModule_Reply_ArrayEnd(reply); // >slots
    }

    RedisModule_Reply_MapEnd(reply); // root
  }
  //-------------------------------------------------------------------------------------------
  else // RESP2 variant
  {
    RedisModule_Reply_Array(reply); // root

    RedisModule_ReplyKV_LongLong(reply, "num_partitions", partitions);
    RedisModule_ReplyKV_SimpleString(reply, "cluster_type", cluster_type_str);

    RedisModule_ReplyKV_SimpleString(reply, "hash_func", hash_func_str);

    // Report topology
    RedisModule_ReplyKV_LongLong(reply, "num_slots", topo ? (long long)topo->numSlots : 0);

    RedisModule_Reply_SimpleString(reply, "slots");

    if (!topo) {
      RedisModule_Reply_Null(reply);
    } else {
      for (int i = 0; i < topo->numShards; i++) {
        MRClusterShard *sh = &topo->shards[i];
        RedisModule_Reply_Array(reply); // >shards

        RedisModule_Reply_LongLong(reply, sh->startSlot);
        RedisModule_Reply_LongLong(reply, sh->endSlot);
        for (int j = 0; j < sh->numNodes; j++) {
          MRClusterNode *node = &sh->nodes[j];
          RedisModule_Reply_Array(reply); // >>node
            REPLY_SIMPLE_SAFE(node->id);
            REPLY_SIMPLE_SAFE(node->endpoint.host);
            RedisModule_Reply_LongLong(reply, node->endpoint.port);
            RedisModule_Reply_SimpleStringf(reply, "%s%s",                                // TODO: move the space to "self"
                                      node->flags & MRNode_Master ? "master " : "slave ", // "master" : "slave",
                                      node->flags & MRNode_Self ? "self" : "");           // " self" : ""
          RedisModule_Reply_ArrayEnd(reply); // >>node
        }

        RedisModule_Reply_ArrayEnd(reply); // >shards
      }
    }

    RedisModule_Reply_ArrayEnd(reply); // root
  }
  //-------------------------------------------------------------------------------------------

  RedisModule_EndReply(reply);
}

struct MRIteratorCtx {
  MRChannel *chan;
  MRIteratorCallback cb;
  short pending;    // Number of shards with more results (not depleted)
  short inProcess;  // Number of currently running commands on shards
  bool timedOut;    // whether the coordinator experienced a timeout
  // reference counter of the iterator.
  // When it reaches 0, both readers and the writer agree that the iterator can be released
  int8_t itRefCount;
  IORuntimeCtx *ioRuntime;
};

struct MRIteratorCallbackCtx {
  MRIterator *it;
  MRCommand cmd;
};

struct MRIterator {
  MRIteratorCtx ctx;
  MRIteratorCallbackCtx *cbxs;
  size_t len;
};

static void mrIteratorRedisCB(redisAsyncContext *c, void *r, void *privdata) {
  MRIteratorCallbackCtx *ctx = privdata;
  if (!r) {
    MRIteratorCallback_Done(ctx, 1);
    // ctx->numErrored++;
    // TODO: report error
  } else {
    ctx->it->ctx.cb(ctx, r);
  }
}

int MRIteratorCallback_ResendCommand(MRIteratorCallbackCtx *ctx) {
  IORuntimeCtx *io_runtime_ctx = ctx->it->ctx.ioRuntime;
  return MRCluster_SendCommand(io_runtime_ctx, true, &ctx->cmd, mrIteratorRedisCB, ctx);
}

// Use after modifying `pending` (or any other variable of the iterator) to make sure it's visible
// to other threads
void MRIteratorCallback_ProcessDone(MRIteratorCallbackCtx *ctx) {
  short inProcess = __atomic_sub_fetch(&ctx->it->ctx.inProcess, 1, __ATOMIC_RELEASE);
  if (!inProcess) {
    MRChannel_Unblock(ctx->it->ctx.chan);
    RS_DEBUG_LOG("MRIteratorCallback_ProcessDone: calling MRIterator_Release");
    IORuntimeCtx *ioRuntime = ctx->it->ctx.ioRuntime;  // Save before potential free
    MRIterator_Release(ctx->it);
    IORuntimeCtx_RequestCompleted(ioRuntime);
  }
}

// Use before obtaining `pending` (or any other variable of the iterator) to make sure it's synchronized with other threads
static short MRIteratorCallback_GetNumInProcess(MRIterator *it) {
  return __atomic_load_n(&it->ctx.inProcess, __ATOMIC_ACQUIRE);
}

short MRIterator_GetPending(MRIterator *it) {
  return __atomic_load_n(&it->ctx.pending, __ATOMIC_ACQUIRE);
}

bool MRIteratorCallback_GetTimedOut(MRIteratorCtx *ctx) {
  return __atomic_load_n(&ctx->timedOut, __ATOMIC_ACQUIRE);
}

void MRIteratorCallback_SetTimedOut(MRIteratorCtx *ctx) {
  // Atomically set the timedOut field of the ctx
  __atomic_store_n(&ctx->timedOut, true, __ATOMIC_RELAXED);
}

void MRIteratorCallback_ResetTimedOut(MRIteratorCtx *ctx) {
  // Set the `timedOut` field to false
  __atomic_store_n(&ctx->timedOut, false, __ATOMIC_RELAXED);
}

static inline int8_t MRIterator_IncreaseRefCount(MRIterator *it) {
  return  __atomic_add_fetch(&it->ctx.itRefCount, 1, __ATOMIC_ACQUIRE);
}

static inline int8_t MRIterator_DecreaseRefCount(MRIterator *it) {
  return  __atomic_sub_fetch(&it->ctx.itRefCount, 1, __ATOMIC_ACQUIRE);
}

void MRIteratorCallback_Done(MRIteratorCallbackCtx *ctx, int error) {
  // Mark the command of the context as depleted (so we won't send another command to the shard)
  RS_DEBUG_LOG_FMT(
      "depleted(should be false): %d, Pending: (%d), inProcess: %d, itRefCount: %d, channel size: "
      "%zu, shard_slot: %d",
      ctx->cmd.depleted, ctx->it->ctx.pending, ctx->it->ctx.inProcess, ctx->it->ctx.itRefCount,
      MRChannel_Size(ctx->it->ctx.chan), ctx->cmd.targetSlot);
  ctx->cmd.depleted = true;
  short pending = --ctx->it->ctx.pending; // Decrease `pending` before decreasing `inProcess`
  RS_ASSERT(pending >= 0);
  MRIteratorCallback_ProcessDone(ctx);
}

MRCommand *MRIteratorCallback_GetCommand(MRIteratorCallbackCtx *ctx) {
  return &ctx->cmd;
}

MRIteratorCtx *MRIteratorCallback_GetCtx(MRIteratorCallbackCtx *ctx) {
  return &ctx->it->ctx;
}

void MRIteratorCallback_AddReply(MRIteratorCallbackCtx *ctx, MRReply *rep) {
  MRChannel_Push(ctx->it->ctx.chan, rep);
}

// This function already runs in one of the IO threads. We need to make sure that the adequate RuntimeCtx is used. This info can be found in the MRIterator ctx
void iterStartCb(void *p) {
  MRIterator *it = p;
  IORuntimeCtx *io_runtime_ctx = it->ctx.ioRuntime;
  size_t numShards = io_runtime_ctx->topo->numShards;
  it->len = numShards;
  it->ctx.pending = numShards;
  it->ctx.inProcess = numShards; // Initially all commands are in process

  it->cbxs = rm_realloc(it->cbxs, numShards * sizeof(*it->cbxs));
  MRCommand *cmd = &it->cbxs->cmd;
  cmd->targetSlot = io_runtime_ctx->topo->shards[0].startSlot; // Set the first command to target the first shard
  for (size_t i = 1; i < numShards; i++) {
    it->cbxs[i].it = it;
    it->cbxs[i].cmd = MRCommand_Copy(cmd);
    // Set each command to target a different shard
    it->cbxs[i].cmd.targetSlot = io_runtime_ctx->topo->shards[i].startSlot;
  }

  // This implies that every connection to each shard will work inside a single IO thread
  for (size_t i = 0; i < it->len; i++) {
    if (MRCluster_SendCommand(io_runtime_ctx, true, &it->cbxs[i].cmd,
                              mrIteratorRedisCB, &it->cbxs[i]) == REDIS_ERR) {
      MRIteratorCallback_Done(&it->cbxs[i], 1);
    }
  }
}

// This function already runs in one of the IO threads. We need to make sure that the adequate RuntimeCtx is used. This info can be found in the MRIterator ctx
void iterManualNextCb(void *p) {
  MRIterator *it = p;
  IORuntimeCtx *io_runtime_ctx = it->ctx.ioRuntime;
  for (size_t i = 0; i < it->len; i++) {
    if (!it->cbxs[i].cmd.depleted) {
      if (MRCluster_SendCommand(io_runtime_ctx, true, &it->cbxs[i].cmd,
                                mrIteratorRedisCB, &it->cbxs[i]) == REDIS_ERR) {
        MRIteratorCallback_Done(&it->cbxs[i], 1);
      }
    }
  }
}

bool MR_ManuallyTriggerNextIfNeeded(MRIterator *it, size_t channelThreshold) {
  // We currently trigger the next batch of commands only when no commands are in process,
  // regardless of the number of replies we have in the channel.
  // Since we push the triggering job to a single-threaded queue (currently), we can modify the logic here
  // to trigger the next batch when we have no commands in process and no more than channelThreshold replies to process.
  if (MRIteratorCallback_GetNumInProcess(it)) {
    // We have more replies to wait for
    return true;
  }
  size_t channelSize = MRChannel_Size(it->ctx.chan);
  if (channelSize > channelThreshold) {
    // We have more replies to process
    return true;
  }
  // We have <= channelThreshold replies to process, so if there are pending commands we want to trigger them.
  if (it->ctx.pending) {
    // We have more commands to send
    it->ctx.inProcess = it->ctx.pending;
    // All reader have marked that they are done with the current command batch (decreased inProcess)
    // However, they may still hold the iterator reference.
    // We need to take a reference to the iterator for the next batch of commands.
    int8_t refCount = MRIterator_IncreaseRefCount(it);
    REFCOUNT_INCR_MSG("MR_ManuallyTriggerNextIfNeeded", refCount);
    IORuntimeCtx_Schedule(it->ctx.ioRuntime, iterManualNextCb, it);
    return true; // We may have more replies (and we surely will)
  }
  // We have no pending commands and no more than channelThreshold replies to process.
  // If we have more replies we will process them, otherwise we are done.
  return channelSize > 0;
}

MRIterator *MR_Iterate(const MRCommand *cmd, MRIteratorCallback cb) {

  MRIterator *ret = rm_new(MRIterator);
  // Initial initialization of the iterator.
  // The rest of the initialization is done in the iterator start callback.
  // We set `pending` and `inProcess` to 1 so we won't get the impression that we are done
  // before the first command is sent. This is also technically correct since we know that we have
  // at least ourselves to wait for.
  // The reference count is set to 2:
  // - one ref for the writers (shards)
  // - one for the reader (the coord)
  *ret = (MRIterator){
    .ctx = {
      .chan = MR_NewChannel(),
      .cb = cb,
      .pending = 1,
      .inProcess = 1,
      .timedOut = false,
      .itRefCount = 2,
      .ioRuntime = MRCluster_GetIORuntimeCtx(cluster_g, MRCluster_AssignRoundRobinIORuntimeIdx(cluster_g)),
    },
    .cbxs = rm_new(MRIteratorCallbackCtx),
  };
  // Initialize the first command
  *ret->cbxs = (MRIteratorCallbackCtx){
    .cmd = MRCommand_Copy(cmd),
    .it = ret,
  };
  IORuntimeCtx_Schedule(ret->ctx.ioRuntime, iterStartCb, ret);
  return ret;
}

MRIteratorCtx *MRIterator_GetCtx(MRIterator *it) {
  return &it->ctx;
}

MRReply *MRIterator_Next(MRIterator *it) {
  return MRChannel_Pop(it->ctx.chan);
}

// Assumes no other thread is using the iterator, the channel, or any of the commands and contexts
static void MRIterator_Free(MRIterator *it) {
  for (size_t i = 0; i < it->len; i++) {
    MRCommand_Free(&it->cbxs[i].cmd);
  }
  MRReply *reply;
  while ((reply = MRChannel_UnsafeForcePop(it->ctx.chan))) {
    MRReply_Free(reply);
  }
  MRChannel_Free(it->ctx.chan);
  rm_free(it->cbxs);
  rm_free(it);
}

void MRIterator_Release(MRIterator *it) {
  int8_t refcount = MRIterator_DecreaseRefCount(it);
  REFCOUNT_DECR_MSG("MRIterator_Release", refcount);
  RS_ASSERT(refcount >= 0);
  if (refcount > 0) return;

  // Both reader and writers are done with the iterator. No writer is in process.
  if (it->ctx.pending) {
    // If we have pending (not depleted) shards, trigger `FT.CURSOR DEL` on them
    it->ctx.inProcess = it->ctx.pending;
    // Change the root command to DEL for each pending shard
    for (size_t i = 0; i < it->len; i++) {
      MRCommand *cmd = &it->cbxs[i].cmd;
      if (!cmd->depleted) {
        RS_DEBUG_LOG_FMT("changing command from %s to DEL for shard: %d", cmd->strs[1], cmd->targetSlot);
        RS_LOG_ASSERT_FMT(cmd->rootCommand != C_DEL, "DEL command should be sent only once to a shard. pending = %d", it->ctx.pending);
        cmd->rootCommand = C_DEL;
        strcpy(cmd->strs[1], "DEL");
        cmd->lens[1] = 3;
      }
    }
    // Take a reference to the iterator for the next batch of commands.
    // The iterator will be released when DEL commands are done.
    refcount = MRIterator_IncreaseRefCount(it);
    REFCOUNT_INCR_MSG("MRIterator_Release: triggering DEL on the shards' cursors", refcount);
    IORuntimeCtx_Schedule(it->ctx.ioRuntime, iterManualNextCb, it);
  } else {
    // No pending shards, so no remote resources to free.
    // Free the iterator and we are done.
    RS_DEBUG_LOG("MRIterator_Release: calling MRIterator_Free");
    MRIterator_Free(it);
  }
}

void MR_Debug_ClearPendingTopo() {
  for (size_t i = 0; i < cluster_g->num_io_threads; i++) {
    IORuntimeCtx_Debug_ClearPendingTopo(cluster_g->io_runtimes_pool[i]);
  }
}

void MR_FreeCluster() {
  if (!cluster_g) return;
  RedisModule_ThreadSafeContextUnlock(RSDummyContext);
  MRCluster_Free(cluster_g);
  cluster_g = NULL;
  RedisModule_ThreadSafeContextLock(RSDummyContext);
}
