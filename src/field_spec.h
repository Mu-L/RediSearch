/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#ifndef SRC_FIELD_SPEC_H_
#define SRC_FIELD_SPEC_H_

#include "redisearch.h"
#include "value.h"
#include "VecSim/vec_sim.h"
#include "geometry/geometry_types.h"
#include "info/index_error.h"
#include "obfuscation/hidden.h"

#ifdef __cplusplus
#define RS_ENUM_BITWISE_HELPER(T)   \
  inline T operator|=(T a, int b) { \
    return (T)((int)a | b);         \
  }
#else
#define RS_ENUM_BITWISE_HELPER(T)
#endif

typedef enum {
  // Newline
  INDEXFLD_T_FULLTEXT = 0x01,
  INDEXFLD_T_NUMERIC = 0x02,
  INDEXFLD_T_GEO = 0x04,
  INDEXFLD_T_TAG = 0x08,
  INDEXFLD_T_VECTOR = 0x10,
  INDEXFLD_T_GEOMETRY = 0x20,
} FieldType;

#define INDEXFLD_NUM_TYPES 6

// clang-format off
// otherwise, it looks h o r r i b l e
#define INDEXTYPE_TO_POS(T)         \
  (T == INDEXFLD_T_FULLTEXT   ? 0 : \
  (T == INDEXFLD_T_NUMERIC    ? 1 : \
  (T == INDEXFLD_T_GEO        ? 2 : \
  (T == INDEXFLD_T_TAG        ? 3 : \
  (T == INDEXFLD_T_VECTOR     ? 4 : \
  (T == INDEXFLD_T_GEOMETRY   ? 5 : -1))))))

#define INDEXTYPE_FROM_POS(P) (1<<(P))
// clang-format on

#define IXFLDPOS_FULLTEXT INDEXTYPE_TO_POS(INDEXFLD_T_FULLTEXT)
#define IXFLDPOS_NUMERIC INDEXTYPE_TO_POS(INDEXFLD_T_NUMERIC)
#define IXFLDPOS_GEO INDEXTYPE_TO_POS(INDEXFLD_T_GEO)
#define IXFLDPOS_TAG INDEXTYPE_TO_POS(INDEXFLD_T_TAG)
#define IXFLDPOS_VECTOR INDEXTYPE_TO_POS(INDEXFLD_T_VECTOR)
#define IXFLDPOS_GEOMETRY INDEXTYPE_TO_POS(INDEXFLD_T_GEOMETRY)

RS_ENUM_BITWISE_HELPER(FieldType)

typedef enum {
  FieldSpec_Sortable = 0x01,
  FieldSpec_NoStemming = 0x02,
  FieldSpec_NotIndexable = 0x04,
  FieldSpec_Phonetics = 0x08,
  FieldSpec_Dynamic = 0x10,
  FieldSpec_UNF = 0x20,
  FieldSpec_WithSuffixTrie = 0x40,
  FieldSpec_UndefinedOrder = 0x80,
  FieldSpec_IndexEmpty = 0x100,       // Index empty values (i.e., empty strings)
  FieldSpec_IndexMissing = 0x200,     // Index missing values (non-existing field)
} FieldSpecOptions;

RS_ENUM_BITWISE_HELPER(FieldSpecOptions)

// Flags for tag fields
typedef enum {
  TagField_CaseSensitive = 0x01,
  TagField_TrimSpace = 0x02,
  TagField_RemoveAccents = 0x04,
} TagFieldFlags;

#define TAG_FIELD_IS(f, t) (FIELD_IS((f), INDEXFLD_T_TAG) && (((f)->tagOpts.tagFlags) & (t)))

RS_ENUM_BITWISE_HELPER(TagFieldFlags)

/*
The fieldSpec represents a single field in the document's field spec.
Each field has a unique id that's a power of two, so we can filter fields
by a bit mask.
*/
typedef struct FieldSpec {
  HiddenString *fieldName;
  HiddenString *fieldPath;
  FieldType types : 8;
  FieldSpecOptions options : 16;

  /** If this field is sortable, the sortable index. Otherwise -1 */
  int16_t sortIdx;

  /** Unique field index. Each field has a unique index regardless of its type */
  // We rely on the index starting from 0 and being sequential
  t_fieldIndex index;

  union {
    struct {
      // Flags for tag options
      TagFieldFlags tagFlags : 16;
      char tagSep;
    } tagOpts;
    struct {
      // Vector similarity index parameters.
      VecSimParams vecSimParams;
      // expected size of vector blob.
      size_t expBlobSize;
    } vectorOpts;
    struct {
      // Geometry index parameters
      GEOMETRY_COORDS geometryCoords;
    } geometryOpts;
  };

  // weight in frequency calculations
  double ftWeight;
  // ID used to identify the field within the field mask
  t_fieldId ftId;

  // The index error for this field
  IndexError indexError;
} FieldSpec;

#define FIELD_IS(f, t) (((f)->types) & (t))
#define FIELD_CHKIDX(fmask, ix) (fmask & ix)

#define TAG_FIELD_DEFAULT_FLAGS (TagFieldFlags)(TagField_TrimSpace | TagField_RemoveAccents);
#define TAG_FIELD_DEFAULT_HASH_SEP ','
#define TAG_FIELD_DEFAULT_JSON_SEP '\0' // by default, JSON fields have no separator

#define FieldSpec_IsSortable(fs) ((fs)->options & FieldSpec_Sortable)
#define FieldSpec_IsNoStem(fs) ((fs)->options & FieldSpec_NoStemming)
#define FieldSpec_IsPhonetics(fs) ((fs)->options & FieldSpec_Phonetics)
#define FieldSpec_IsIndexable(fs) (0 == ((fs)->options & FieldSpec_NotIndexable))
#define FieldSpec_HasSuffixTrie(fs) ((fs)->options & FieldSpec_WithSuffixTrie)
#define FieldSpec_IsUndefinedOrder(fs) ((fs)->options & FieldSpec_UndefinedOrder)
#define FieldSpec_IndexesEmpty(fs) ((fs)->options & FieldSpec_IndexEmpty)
#define FieldSpec_IndexesMissing(fs) ((fs)->options & FieldSpec_IndexMissing)
#define FieldSpec_IsUnf(fs) ((fs)->options & FieldSpec_UNF)

void FieldSpec_SetSortable(FieldSpec* fs);
void FieldSpec_Cleanup(FieldSpec* fs);
/**
 * Convert field type given by integer to the name type in string form.
 */
const char *FieldSpec_GetTypeNames(int idx);

char *FieldSpec_FormatName(const FieldSpec *fs, bool obfuscate);
char *FieldSpec_FormatPath(const FieldSpec *fs, bool obfuscate);

/**Adds an error message to the IndexError of the FieldSpec.
 * This function also updates the global field's type index error counter.
 */
void FieldSpec_AddError(FieldSpec *, ConstErrorMessage withoutUserData, ConstErrorMessage withUserData, RedisModuleString *key);

static inline void FieldSpec_AddQueryError(FieldSpec *fs, const QueryError *queryError, RedisModuleString *key) {
  FieldSpec_AddError(fs, QueryError_GetDisplayableError(queryError, true), QueryError_GetDisplayableError(queryError, false), key);
}

size_t FieldSpec_GetIndexErrorCount(const FieldSpec *);

#endif /* SRC_FIELD_SPEC_H_ */
