/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/

#include "hidden.h"
#include "rmalloc.h"
#include "util/minmax.h"
#include "redis_index.h"
#include "query_node.h"
#include "reply_macros.h"

typedef struct {
  const char *user;
  size_t length;
} UserString;

HiddenString *NewHiddenString(const char* name, size_t length, bool takeOwnership) {
  UserString* value = rm_malloc(sizeof(*value));
  if (takeOwnership) {
    value->user = rm_strndup(name, length);
  } else {
    value->user = name;
  }
  value->length = length;
  return (HiddenString*)value;
};

void HiddenString_Free(const HiddenString* hn, bool tookOwnership) {
  UserString* value = (UserString*)hn;
  if (tookOwnership) {
    rm_free((void*)value->user);
  }
  rm_free(value);
};

static inline int Compare(const char *left, size_t left_length, const char *right, size_t right_length) {
  int result = strncmp(left, right, MIN(left_length, right_length));
  if (result != 0 || left_length == right_length) {
    return result;
  } else {
    return (int)(left_length - right_length);
  }
}

static inline int CaseInsensitiveCompare(const char *left, size_t left_length, const char *right, size_t right_length) {
  int result = strncasecmp(left, right, MIN(left_length, right_length));
  if (result != 0 || left_length == right_length) {
    return result;
  } else {
    return (int)(left_length - right_length);
  }
}

int HiddenString_CompareC(const HiddenString *left, const char *right, size_t right_length) {
  const UserString* l = (const UserString*)left;
  return Compare(l->user, l->length, right, right_length);
}

int HiddenString_Compare(const HiddenString* left, const HiddenString* right) {
  UserString* r = (UserString*)right;
  return HiddenString_CompareC(left, r->user, r->length);
}

int HiddenString_CaseInsensitiveCompare(const HiddenString *left, const HiddenString *right) {
  UserString* r = (UserString*)right;
  return HiddenString_CaseInsensitiveCompareC(left, r->user, r->length);
}

int HiddenString_CaseInsensitiveCompareC(const HiddenString *left, const char *right, size_t right_length) {
  UserString* l = (UserString*)left;
  return CaseInsensitiveCompare(l->user, l->length, right, right_length);
}

HiddenString *HiddenString_Duplicate(const HiddenString *value) {
  const UserString* text = (const UserString*)value;
  return NewHiddenString(text->user, text->length, true);
}

void HiddenString_TakeOwnership(HiddenString *hidden) {
  UserString* userString = (UserString*)hidden;
  userString->user = rm_strndup(userString->user, userString->length);
}

void HiddenString_Clone(const HiddenString* src, HiddenString** dst) {
  const UserString* s = (const UserString*)src;
  if (*dst == NULL) {
    *dst = NewHiddenString(s->user, s->length, true);
  } else {
    UserString* d = (UserString*)*dst;
    if (s->length > d->length) {
      d->user = rm_realloc((void*)d->user, s->length);
    }
    // strncpy will pad d->user with zeroes per documentation if there is room
    // also remember d->user[d->length] == '\0' due to rm_strdup
    strncpy((void*)d->user, s->user, d->length);
    // By setting the length we cause rm_realloc to potentially be called
    // in the future if this function is called again
    // But a reasonable allocator should do zero allocation work and identify the memory chunk is enough
    // That saves us from storing a capacity field
    d->length = s->length;
  }
}

void HiddenString_SaveToRdb(const HiddenString* value, RedisModuleIO* rdb) {
  const UserString* text = (const UserString*)value;
  RedisModule_SaveStringBuffer(rdb, text->user, text->length + 1);
}

void HiddenString_DropFromKeySpace(RedisModuleCtx* redisCtx, const char* fmt, const HiddenString* value) {
  const UserString* text = (const UserString*)value;
  RedisModuleString *str =
      RedisModule_CreateStringPrintf(redisCtx, fmt, text->user);
  Redis_DeleteKey(redisCtx, str);
  RedisModule_FreeString(redisCtx, str);
}

const char *HiddenString_GetUnsafe(const HiddenString* value, size_t* length) {
  const UserString* text = (const UserString*)value;
  if (length != NULL) {
    *length = text->length;
  }
  return text->user;
}

RedisModuleString *HiddenString_CreateRedisModuleString(const HiddenString* value, RedisModuleCtx* ctx) {
  const UserString* text = (const UserString*)value;
  return RedisModule_CreateString(ctx, text->user, text->length);
}
