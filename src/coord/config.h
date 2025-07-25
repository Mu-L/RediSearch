/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/

#pragma once

#include "redismodule.h"
#include "rmr/endpoint.h"
#include "../../src/config.h"

#include <string.h>

typedef enum { ClusterType_RedisOSS = 0, ClusterType_RedisLabs = 1 } MRClusterType;

typedef struct {
  MRClusterType type;
  int timeoutMS;
  size_t connPerShard;
  size_t cursorReplyThreshold;
  size_t coordinatorPoolSize; // number of threads in the coordinator thread pool
  size_t coordinatorIOThreads; // number of I/O threads in the coordinator
  size_t topologyValidationTimeoutMS;
} SearchClusterConfig;

extern SearchClusterConfig clusterConfig;
extern RedisModuleString *config_dummy_password;

#define CLUSTER_TYPE_OSS "redis_oss"
#define CLUSTER_TYPE_RLABS "redislabs"

#define COORDINATOR_POOL_DEFAULT_SIZE 20
#define COORDINATOR_IO_THREADS_DEFAULT_SIZE 1
#define DEFAULT_TOPOLOGY_VALIDATION_TIMEOUT 30000
#define DEFAULT_CURSOR_REPLY_THRESHOLD 1
#define DEFAULT_CONN_PER_SHARD 0

#define DEFAULT_CLUSTER_CONFIG                                                 \
  (SearchClusterConfig) {                                                      \
    .connPerShard = DEFAULT_CONN_PER_SHARD,                                    \
    .type = DetectClusterType(),                                               \
    .timeoutMS = 0,                                                            \
    .cursorReplyThreshold = DEFAULT_CURSOR_REPLY_THRESHOLD,                    \
    .coordinatorPoolSize = COORDINATOR_POOL_DEFAULT_SIZE,                      \
    .coordinatorIOThreads = COORDINATOR_IO_THREADS_DEFAULT_SIZE,               \
    .topologyValidationTimeoutMS = DEFAULT_TOPOLOGY_VALIDATION_TIMEOUT,        \
  }

/* Detect the cluster type, by trying to see if we are running inside RLEC.
 * If we cannot determine, we return OSS type anyway
 */
MRClusterType DetectClusterType();

RSConfigOptions *GetClusterConfigOptions(void);
void ClusterConfig_RegisterTriggers(void);

int RegisterClusterModuleConfig(RedisModuleCtx *ctx);
