#pragma once

/* Warning, this file is auto-generated by cbindgen from `src/redisearch_rs/c_entrypoint/rlookup_ffi/build.rs. Don't modify it manually. */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum RLookupKeyFlag
#ifdef __cplusplus
  : uint32_t
#endif // __cplusplus
 {
  /**
   * This field is (or assumed to be) part of the document itself.
   * This is a basic flag for a loaded key.
   */
  DocSrc = 1,
  /**
   * This field is part of the index schema.
   */
  SchemaSrc = 2,
  /**
   * Check the sorting table, if necessary, for the index of the key.
   */
  SvSrc = 4,
  /**
   * This key was created by the query itself (not in the document)
   */
  QuerySrc = 8,
  /**
   * Copy the key string via strdup. `name` may be freed
   */
  NameAlloc = 16,
  /**
   * If the key is already present, then overwrite it (relevant only for LOAD or WRITE modes)
   */
  Override = 32,
  /**
   * Request that the key is returned for loading even if it is already loaded.
   */
  ForceLoad = 64,
  /**
   * This key is unresolved. Its source needs to be derived from elsewhere
   */
  Unresolved = 128,
  /**
   * This field is hidden within the document and is only used as a transient
   * field for another consumer. Don't output this field.
   */
  Hidden = 256,
  /**
   * The opposite of [`RLookupKeyFlag::Hidden`]. This field is specified as an explicit return in
   * the RETURN list, so ensure that this gets emitted. Only set if
   * explicitReturn is true in the aggregation request.
   */
  ExplicitReturn = 512,
  /**
   * This key's value is already available in the RLookup table,
   * if it was opened for read but the field is sortable and not normalized,
   * so the data should be exactly the same as in the doc.
   */
  ValAvailable = 1024,
  /**
   * This key's value was loaded (by a loader) from the document itself.
   */
  IsLoaded = 2048,
  /**
   * This key type is numeric
   */
  Numeric = 4096,
};
#ifndef __cplusplus
typedef uint32_t RLookupKeyFlag;
#endif // __cplusplus

enum RLookupOption
#ifdef __cplusplus
  : uint32_t
#endif // __cplusplus
 {
  /**
   * If the key cannot be found, do not mark it as an error, but create it and
   * mark it as F_UNRESOLVED
   */
  AllowUnresolved = 1,
  /**
   * If a loader was added to load the entire document, this flag will allow
   * later calls to GetKey in read mode to create a key (from the schema) even if it is not sortable
   */
  AllLoaded = 2,
};
#ifndef __cplusplus
typedef uint32_t RLookupOption;
#endif // __cplusplus
