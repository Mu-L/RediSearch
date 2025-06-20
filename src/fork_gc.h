/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/

#ifndef SRC_FORK_GC_H_
#define SRC_FORK_GC_H_

#include "redismodule.h"
#include "gc.h"
#include "VecSim/vec_sim.h"
#include <poll.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  // total bytes collected by the GC
  size_t totalCollected;
  // number of cycle ran
  size_t numCycles;

  long long totalMSRun;
  long long lastRunTimeMs;

  uint64_t gcNumericNodesMissed;
  uint64_t gcBlocksDenied;
} ForkGCStats;

/* Internal definition of the garbage collector context (each index has one) */
typedef struct ForkGC {

  // owner of the gc
  WeakRef index;

  RedisModuleCtx *ctx;

  // statistics for reporting
  ForkGCStats stats;

  int pipe_read_fd;
  int pipe_write_fd;
  struct pollfd pollfd_read[1]; // pollfd to poll the read pipe so that we don't block while read

  volatile uint32_t pauseState;
  volatile uint32_t execState;

  struct timespec retryInterval;
  volatile size_t deletedDocsFromLastRun;

  // current value of RSGlobalConfig.gcConfigParams.forkGc.forkGCCleanNumericEmptyNodes
  // This value is updated during the periodic callback execution.
  int cleanNumericEmptyNodes;
  // a variable to store a percentage of the progress of the child process, used to send heartbeats
  float progress;
} ForkGC;

ForkGC *FGC_New(StrongRef spec_ref, GCCallbacks *callbacks);

typedef enum {
  // Normal "open" state. No pausing will happen
  FGC_PAUSED_UNPAUSED = 0x00,
  // Prevent invoking the child. The child is not invoked until this flag is
  // cleared
  FGC_PAUSED_CHILD = 0x01,
  // Prevent the parent reading from the child. The results from the child are
  // not read until this flag is cleared.
  FGC_PAUSED_PARENT = 0x02
} FGCPauseFlags;

typedef enum {
  // Idle, "normal" state
  FGC_STATE_IDLE = 0,

  // Set when the PAUSED_CHILD flag is set, indicates that we are
  // awaiting this flag to be cleared.
  FGC_STATE_WAIT_FORK,

  // Set when the child has been launched, but before the first results have
  // been applied.
  FGC_STATE_SCANNING,

  // Set when the PAUSED_PARENT flag is set. The results will not be
  // scanned until the PAUSED_PARENT flag is unset
  FGC_STATE_WAIT_APPLY,

  // Set when results are being applied from the child to the parent
  FGC_STATE_APPLYING
} FGCState;

/**
 * Indicate that the gc should wait immediately prior to
 * forking. This is in order to perform some commands which
 * may not be visible by the fork gc engine.
 *
 * This function will return before the fork is performed. You
 * must call FGC_ForkAndWaitBeforeApply or FGC_Apply to allow the GC to
 * resume functioning
 */
//TODO: I'm not sure this one is necessary, we already wait before we call the callback. (in cbWrapper)
void FGC_WaitBeforeFork(ForkGC *gc);

/**
 * Indicate that the GC should continue from FGC_WaitBeforeFork, and
 * wait before the changes are applied. At this point, the child and parent process
 * no longer share the same memory, hence, the child will not be aware of any
 * changes made in the main process.
 */
void FGC_ForkAndWaitBeforeApply(ForkGC *gc);

/**
 * Apply the changes the parent received from the child.
 */
void FGC_Apply(ForkGC *gc);

typedef struct InfoGCStats {
  size_t totalCollectedBytes; // Total bytes collected by the GCs
  size_t totalCycles;         // Total number of cycles ran
  size_t totalTime;           // In ms
} InfoGCStats;

#ifdef __cplusplus
}
#endif

#endif /* SRC_FORK_GC_H_ */
