/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#include <redisearch.h>
#include <result_processor.h>
#include <util/block_alloc.h>
#include <util/khash.h>
#include "reducer.h"

/**
 * A group represents the allocated context of all reducers in a group, and the
 * selected values of that group.
 *
 * Because one of these is created for every single group (i.e. every single
 * unique key) we want to keep this quite small!
 */
typedef struct {
  /** Contains the selected 'out' values used by the reducers output functions */
  RLookupRow rowdata;

  /**
   * Contains the actual per-reducer data for the group, in an accumulating
   * fashion (e.g. how many records seen, and so on). This is created by
   * Reducer::NewInstance()
   */
  void *accumdata[0];
} Group;

#define kh_set(kname, hash, key, val)   \
  ({                                    \
    int ret;                            \
    k = kh_put(kname, hash, key, &ret); \
    kh_value(hash, k) = val;            \
    ret;                                \
  })

static const int khid = 33;
KHASH_MAP_INIT_INT64(khid, Group *);

#define GROUPER_NREDUCERS(g) (array_len((g)->reducers))
#define GROUP_BYTESIZE(parent) (sizeof(Group) + (sizeof(void *) * GROUPER_NREDUCERS(parent)))
#define GROUPS_PER_BLOCK 1024
#define GROUPER_NSRCKEYS(g) ((g)->nkeys)

typedef struct Grouper {
  // Result processor base, for use in row processing
  ResultProcessor base;

  // Map of group_name => `Group` structure
  khash_t(khid) * groups;

  // Backing store for the groups themselves
  BlkAlloc groupsAlloc;

  /**
   * Keys to group by. Both srckeys and dstkeys are used because different lookups
   * are employed. The srckeys are the lookup keys for the properties as they
   * appear in the row received from the upstream processor, and the dstkeys are
   * the keys as they are expected in the output row.
   */
  const RLookupKey **srckeys;
  const RLookupKey **dstkeys;
  size_t nkeys;

  // array of reducers
  Reducer **reducers;

  // Used for maintaining state when yielding groups
  khiter_t iter;
} Grouper;

/**
 * Create a new group. groupvals is the key of the group. This will be the
 * number of field arguments passed to GROUPBY, e.g.
 * GROUPBY 2 @foo @bar will have a `groupvals` of `{"foo", "bar"}`.
 *
 * These will be placed in the output row.
 */
static Group *createGroup(Grouper *g, const RSValue **groupvals, size_t ngrpvals) {
  size_t numReducers = array_len(g->reducers);
  size_t elemSize = GROUP_BYTESIZE(g);
  Group *group = BlkAlloc_Alloc(&g->groupsAlloc, elemSize, GROUPS_PER_BLOCK * elemSize);
  memset(group, 0, elemSize);

  for (size_t ii = 0; ii < numReducers; ++ii) {
    group->accumdata[ii] = g->reducers[ii]->NewInstance(g->reducers[ii]);
  }

  /** Initialize the row data! */
  for (size_t ii = 0; ii < ngrpvals; ++ii) {
    const RLookupKey *dstkey = g->dstkeys[ii];
    RLookup_WriteKey(dstkey, &group->rowdata, (RSValue *)groupvals[ii]);
  }
  return group;
}

static void writeGroupValues(const Grouper *g, const Group *gr, SearchResult *r) {
  for (size_t ii = 0; ii < g->nkeys; ++ii) {
    const RLookupKey *dstkey = g->dstkeys[ii];
    RSValue *groupval = RLookup_GetItem(dstkey, &gr->rowdata);
    if (groupval) {
      RLookup_WriteKey(dstkey, &r->rowdata, groupval);
    }
  }
}

static int Grouper_rpYield(ResultProcessor *base, SearchResult *r) {
  Grouper *g = (Grouper *)base;

  while (g->iter != kh_end(g->groups)) {
    if (!kh_exist(g->groups, g->iter)) {
      g->iter++;
      continue;
    }

    Group *gr = kh_value(g->groups, g->iter);
    writeGroupValues(g, gr, r);
    for (size_t ii = 0; ii < GROUPER_NREDUCERS(g); ++ii) {
      Reducer *rd = g->reducers[ii];
      RSValue *v = rd->Finalize(rd, gr->accumdata[ii]);
      RLookup_WriteOwnKey(rd->dstkey, &r->rowdata, v);
    }
    ++g->iter;
    return RS_RESULT_OK;
  }

  return RS_RESULT_EOF;
}

static void invokeReducers(Grouper *g, Group *gr, RLookupRow *srcrow) {
  size_t nreducers = GROUPER_NREDUCERS(g);
  for (size_t ii = 0; ii < nreducers; ii++) {
    g->reducers[ii]->Add(g->reducers[ii], gr->accumdata[ii], srcrow);
  }
}

/**
 * This function recursively descends into each value within a group and invokes
 * Add() for each cartesian product of the current row.
 *
 * @param g the grouper
 * @param xarr the array of 'x' values - i.e. the raw results received from the
 *  upstream result processor. The number of results can be found via
 *  the `GROUPER_NSRCKEYS(g)` macro
 * @param xpos the current position in xarr
 * @param xlen cached value of GROUPER_NSRCKEYS
 * @param hval current X-wise hash value. Note that members of the same Y array
 *  are not hashed together.
 * @param res the row is passed to each reducer
 */
static void extractGroups(Grouper *g, const RSValue **xarr, size_t xpos, size_t xlen, uint64_t hval, RLookupRow *res) {
  // end of the line - create/add to group
  if (xpos == xlen) {
    Group *group = NULL;

    // Get or create the group
    khiter_t k = kh_get(khid, g->groups, hval);  // first have to get ieter
    if (k == kh_end(g->groups)) {                // k will be equal to kh_end if key not present
      group = createGroup(g, xarr, xlen);
      kh_set(khid, g->groups, hval, group);
    } else {
      group = kh_value(g->groups, k);
    }

    // send the result to the group and its reducers
    invokeReducers(g, group, res);
    return;
  }

  // get the value
  const RSValue *v = RSValue_Dereference(xarr[xpos]);
  // regular value - just move one step -- increment XPOS
  if (v->t != RSValue_Array) {
    hval = RSValue_Hash(v, hval);
    extractGroups(g, xarr, xpos + 1, xlen, hval, res);
  } else if (RSValue_ArrayLen(v) == 0) {
    // Empty array - hash as null
    hval = RSValue_Hash(RS_NullVal(), hval);
    const RSValue *array = xarr[xpos];
    xarr[xpos] = RS_NullVal();
    extractGroups(g, xarr, xpos + 1, xlen, hval, res);
    xarr[xpos] = array;
  } else {
    // Array value. Replace current XPOS with child temporarily.
    // Each value in the array will be a separate group
    const RSValue *array = xarr[xpos];
    for (size_t i = 0; i < RSValue_ArrayLen(v); i++) {
      const RSValue *elem = RSValue_ArrayItem(v, i);
      // hash the element, even if it's an array
      uint64_t hh = RSValue_Hash(elem, hval);
      xarr[xpos] = elem;
      extractGroups(g, xarr, xpos + 1, xlen, hh, res);
    }
    xarr[xpos] = array;
  }
}

static void invokeGroupReducers(Grouper *g, RLookupRow *srcrow) {
  uint64_t hval = 0;
  size_t nkeys = GROUPER_NSRCKEYS(g);
  const RSValue *groupvals[nkeys];

  for (size_t ii = 0; ii < nkeys; ++ii) {
    const RLookupKey *srckey = g->srckeys[ii];
    RSValue *v = RLookup_GetItem(srckey, srcrow);
    if (v == NULL) {
      v = RS_NullVal();
    }
    groupvals[ii] = v;
  }
  extractGroups(g, groupvals, 0, nkeys, 0, srcrow);
}

static int Grouper_rpAccum(ResultProcessor *base, SearchResult *res) {
  Grouper *g = (Grouper *)base;
  uint32_t chunkLimit = base->parent->resultLimit;
  base->parent->resultLimit = UINT32_MAX; // we want to accumulate all the results
  int rc;

  while ((rc = base->upstream->Next(base->upstream, res)) == RS_RESULT_OK) {
    invokeGroupReducers(g, &res->rowdata);
    SearchResult_Clear(res);
  }
  base->parent->resultLimit = chunkLimit; // restore the limit
  if (rc == RS_RESULT_EOF) {
    base->Next = Grouper_rpYield;
    base->parent->totalResults = kh_size(g->groups);
    g->iter = kh_begin(khid);
    return Grouper_rpYield(base, res);
  } else {
    return rc;
  }
}

static void cleanCallback(void *ptr, void *arg) {
  Group *group = ptr;
  Grouper *parent = arg;
  // Call the reducer's FreeInstance
  for (size_t ii = 0; ii < GROUPER_NREDUCERS(parent); ++ii) {
    Reducer *rr = parent->reducers[ii];
    if (rr->FreeInstance) {
      rr->FreeInstance(rr, group->accumdata[ii]);
    }
  }
}

static void Grouper_rpFree(ResultProcessor *grrp) {
  Grouper *g = (Grouper *)grrp;
  for (khiter_t it = kh_begin(g->groups); it != kh_end(g->groups); ++it) {
    if (!kh_exist(g->groups, it)) {
      continue;
    }
    Group *gr = kh_value(g->groups, it);
    RLookupRow_Cleanup(&gr->rowdata);
  }
  kh_destroy(khid, g->groups);
  BlkAlloc_FreeAll(&g->groupsAlloc, cleanCallback, g, GROUP_BYTESIZE(g));

  for (size_t i = 0; i < GROUPER_NREDUCERS(g); i++) {
    g->reducers[i]->Free(g->reducers[i]);
  }
  if (g->reducers) {
    array_free(g->reducers);
  }
  rm_free(g->srckeys);
  rm_free(g->dstkeys);
  rm_free(g);
}

void Grouper_Free(Grouper *g) {
  g->base.Free(&g->base);
}

Grouper *Grouper_New(const RLookupKey **srckeys, const RLookupKey **dstkeys, size_t nkeys) {
  Grouper *g = rm_calloc(1, sizeof(*g));
  BlkAlloc_Init(&g->groupsAlloc);
  g->groups = kh_init(khid);

  g->nkeys = nkeys;
  if (nkeys) {
    g->srckeys = rm_malloc(nkeys * sizeof(*g->srckeys));
    g->dstkeys = rm_malloc(nkeys * sizeof(*g->dstkeys));
    memcpy(g->srckeys, srckeys, nkeys * sizeof(*g->srckeys));
    memcpy(g->dstkeys, dstkeys, nkeys * sizeof(*g->dstkeys));
  }

  g->base.type = RP_GROUP;
  g->base.Next = Grouper_rpAccum;
  g->base.Free = Grouper_rpFree;
  return g;
}

void Grouper_AddReducer(Grouper *g, Reducer *r, RLookupKey *dstkey) {
  Reducer **rpp = array_ensure_tail(&g->reducers, Reducer *);
  *rpp = r;
  r->dstkey = dstkey;
}

ResultProcessor *Grouper_GetRP(Grouper *g) {
  return &g->base;
}
