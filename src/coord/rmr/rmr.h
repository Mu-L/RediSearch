/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/

#pragma once

#include <stdbool.h>

#include "reply.h"
#include "cluster.h"
#include "command.h"

struct MRCtx;
struct RedisModuleCtx;

/* Prototype for all reduce functions */
typedef int (*MRReduceFunc)(struct MRCtx *ctx, int count, MRReply **replies);

/* Fanout map - send the same command to all the shards, sending the collective
 * reply to the reducer callback */
int MR_Fanout(struct MRCtx *ctx, MRReduceFunc reducer, MRCommand cmd, bool block);

int MR_MapSingle(struct MRCtx *ctx, MRReduceFunc reducer, MRCommand cmd);

void MR_SetCoordinationStrategy(struct MRCtx *ctx, bool mastersOnly);

/* Initialize the MapReduce engine with a given number of I/O threads and connections per each node in the Cluster */
void MR_Init(size_t num_io_threads, size_t conn_pool_size, long long timeoutMS);

/* Set a new topology for the cluster */
void MR_UpdateTopology(MRClusterTopology *newTopology);

void MR_ReplyClusterInfo(RedisModuleCtx *ctx, MRClusterTopology *topo);

void MR_GetConnectionPoolState(RedisModuleCtx *ctx);

void MR_uvReplyClusterInfo(RedisModuleCtx *ctx);

void MR_UpdateConnPoolSize(size_t conn_pool_size);

void MR_Debug_ClearPendingTopo();

void MR_FreeCluster();

/* Get the user stored private data from the context */
void *MRCtx_GetPrivData(struct MRCtx *ctx);

struct RedisModuleCtx *MRCtx_GetRedisCtx(struct MRCtx *ctx);
int MRCtx_GetNumReplied(struct MRCtx *ctx);
void MRCtx_RequestCompleted(struct MRCtx *ctx);
MRReply** MRCtx_GetReplies(struct MRCtx *ctx);
RedisModuleBlockedClient *MRCtx_GetBlockedClient(struct MRCtx *ctx);
void MRCtx_SetReduceFunction(struct MRCtx *ctx, MRReduceFunc fn);


/* Free the MapReduce context */
void MRCtx_Free(struct MRCtx *ctx);

/* Create a new MapReduce context with a given private data. In a redis module
 * this should be the RedisModuleCtx */
struct MRCtx *MR_CreateCtx(struct RedisModuleCtx *ctx, struct RedisModuleBlockedClient *bc, void *privdata, int replyCap);

typedef struct MRIteratorCallbackCtx MRIteratorCallbackCtx;
typedef struct MRIteratorCtx MRIteratorCtx;
typedef struct MRIterator MRIterator;

typedef void (*MRIteratorCallback)(MRIteratorCallbackCtx *ctx, MRReply *rep);

// Trigger all the commands in the iterator to be sent.
// Returns true if there may be more replies to come, false if we are done.
bool MR_ManuallyTriggerNextIfNeeded(MRIterator *it, size_t channelThreshold);

MRReply *MRIterator_Next(MRIterator *it);

MRIterator *MR_Iterate(const MRCommand *cmd, MRIteratorCallback cb);

MRCommand *MRIteratorCallback_GetCommand(MRIteratorCallbackCtx *ctx);

MRIteratorCtx *MRIteratorCallback_GetCtx(MRIteratorCallbackCtx *ctx);

void MRIteratorCallback_AddReply(MRIteratorCallbackCtx *ctx, MRReply *rep);

bool MRIteratorCallback_GetTimedOut(MRIteratorCtx *ctx);

void MRIteratorCallback_SetTimedOut(MRIteratorCtx *ctx);

void MRIteratorCallback_ResetTimedOut(MRIteratorCtx *ctx);

void MRIteratorCallback_Done(MRIteratorCallbackCtx *ctx, int error);

void MRIteratorCallback_ProcessDone(MRIteratorCallbackCtx *ctx);

int MRIteratorCallback_ResendCommand(MRIteratorCallbackCtx *ctx);

MRIteratorCtx *MRIterator_GetCtx(MRIterator *it);

short MRIterator_GetPending(MRIterator *it);

void MRIterator_Release(MRIterator *it);
