/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#ifndef __INVERTED_INDEX_H__
#define __INVERTED_INDEX_H__

#include "redisearch.h"
#include "buffer/buffer.h"
#include "doc_table.h"
#include "index_iterator.h"
#include "spec.h"
#include "numeric_filter.h"
#include <stdint.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

// The number of entries in each index block. A new block will be created after every N entries
#define INDEX_BLOCK_SIZE 100
#define INDEX_BLOCK_SIZE_DOCID_ONLY 1000

extern uint64_t TotalIIBlocks;

/* A single block of data in the index. The index is basically a list of blocks we iterate */
typedef struct {
  t_docId firstId;
  t_docId lastId;
  Buffer buf;
  uint16_t numEntries;  // Number of entries (i.e., docs)
} IndexBlock;

typedef struct InvertedIndex {
  IndexBlock *blocks; // Array containing the inverted index blocks
  uint32_t size;      // Number of blocks
  IndexFlags flags;
  t_docId lastId;
  uint32_t numDocs;   // Number of documents in the index
  uint32_t gcMarker;
  // The following union must remain at the end as memory is not allocated for it
  // if not required (see function `NewInvertedIndex`)
  union {
    t_fieldMask fieldMask;
    uint64_t numEntries;
  };
} InvertedIndex;

typedef struct IndexBlockReader {
  BufferReader buffReader;
  t_docId curBaseId; // The current value to add to the decoded delta, to get the actual docId.
} IndexBlockReader;

/**
 * This context is passed to the decoder callback, and can contain either
 * a pointer or an integer. It is intended to relay along any kind of additional
 * configuration information to help the decoder determine whether to filter
 * the entry */
typedef union {
  uint32_t mask;
  t_fieldMask wideMask;
  const NumericFilter *filter;
} IndexDecoderCtx;

typedef struct {
  size_t bytesBeforFix;
  size_t bytesAfterFix;
  size_t bytesCollected;    /** out: Number of bytes collected */
  size_t docsCollected;     /** out: Number of documents collected */
  size_t entriesCollected;  /** out: Number of entries collected */
  size_t limit;          /** in: how many index blocks to scan at once */

  /** in: Callback to invoke when a document is collected */
  void (*RepairCallback)(const RSIndexResult *, const IndexBlock *, void *);
  /** argument to pass to callback */
  void *arg;
} IndexRepairParams;

static inline size_t sizeof_InvertedIndex(IndexFlags flags) {
  bool useFieldMask = flags & Index_StoreFieldFlags;
  bool useNumEntries = flags & Index_StoreNumeric;
  RS_ASSERT(!(useFieldMask && useNumEntries));
  // Avoid some of the allocation if not needed
  const size_t base = sizeof(InvertedIndex) - sizeof(t_fieldMask); // Size without the union
  if (useFieldMask) return base + sizeof(t_fieldMask);
  if (useNumEntries) return base + sizeof(uint64_t);
  return base;
}

// Create a new inverted index object, with the given flag.
// If initBlock is 1, we create the first block.
// out parameter memsize must be not NULL, the total of allocated memory
// will be returned in it
InvertedIndex *NewInvertedIndex(IndexFlags flags, int initBlock, size_t *memsize);

/* Add a new block to the index with a given document id as the initial id
  * Returns the new block
  * in/out parameter memsize must be not NULL, because the size (bytes) of the
  * new block is added to it
*/
IndexBlock *InvertedIndex_AddBlock(InvertedIndex *idx, t_docId firstId, size_t *memsize);
size_t indexBlock_Free(IndexBlock *blk);
void InvertedIndex_Free(void *idx);

t_docId IndexBlock_FirstId(const IndexBlock *b);
t_docId IndexBlock_LastId(const IndexBlock *b);
uint16_t IndexBlock_NumEntries(const IndexBlock *b);
char *IndexBlock_Data(const IndexBlock *b);
char **IndexBlock_DataPtr(IndexBlock *b);
void IndexBlock_DataFree(const IndexBlock *b);
size_t IndexBlock_Cap(const IndexBlock *b);
void IndexBlock_SetCap(IndexBlock *b, size_t cap);
size_t IndexBlock_Len(const IndexBlock *b);
size_t *IndexBlock_LenPtr(IndexBlock *b);
Buffer *IndexBlock_Buffer(IndexBlock *b);
void IndexBlock_SetBuffer(IndexBlock *b, Buffer buf);

/**
 * Decode a single record from the buffer reader. This function is responsible for:
 * (1) Decoding the record at the given position of br
 * (2) Advancing the reader's position to the next record
 * (3) Filtering the record based on any relevant information (can be passed through `ctx`)
 * (4) Populating `res` with the information from the record.
 * (5) Setting `br->curOffset` for reading the next record
 *
 * If the record should not be processed, it should not be populated and 0 should
 * be returned. Otherwise, the function should return 1.
 */
typedef bool (*IndexDecoder)(IndexBlockReader *, const IndexDecoderCtx *, RSIndexResult *out);

struct IndexReader;
/**
 * Custom implementation of a seeking function. Seek to the specific ID within
 * the index, or at one position after it.
 *
 * The implementation of this function is optional. If this is not used, then
 * the decoder() implementation will be used instead.
 */
typedef bool (*IndexSeeker)(IndexBlockReader *, const IndexDecoderCtx *, t_docId to, RSIndexResult *out);

typedef struct {
  IndexDecoder decoder;
  IndexSeeker seeker;
} IndexDecoderProcs;

/* Get the decoder for the index based on the index flags. This is used to externally inject the
 * endoder/decoder when reading and writing */
IndexDecoderProcs InvertedIndex_GetDecoder(uint32_t flags);

/* An IndexReader wraps an inverted index record for reading and iteration */
typedef struct IndexReader {
  const RedisSearchCtx *sctx;

  // the underlying data buffer
  BufferReader br;

  InvertedIndex *idx;
  // last docId, used for delta encoding/decoding
  t_docId lastId;
  // same docId, used for detecting same doc (with multi values)
  t_docId sameId;

  union {
    struct {
      double rangeMin;
      double rangeMax;
    } numeric;
  } profileCtx;

  /* The decoder's filtering context. It may be a number or a pointer. The number is used for
   * filtering field masks, the pointer for numeric filtering */
  IndexDecoderCtx decoderCtx;
  /* The decoding function for reading the index */
  IndexDecoderProcs decoders;

  /* The number of records read */
  size_t len;

  /* The record we are decoding into */
  RSIndexResult *record;

  // If present, this pointer is updated when the end has been reached. This is
  // an optimization to avoid calling IR_HasNext() each time
  bool *isValidP;

  bool atEnd_;
  // Whether to skip multi values from the same doc
  bool skipMulti;
  uint32_t currentBlock;

  /* This marker lets us know whether the garbage collector has visited this index while the reading
   * thread was asleep, and reset the state in a deeper way
   */
  uint32_t gcMarker;

  FieldFilterContext filterCtx;
} IndexReader;

// On Reopen callback for term index
void TermReader_OnReopen(void *privdata);

// On Reopen callback for common use
void IndexReader_OnReopen(IndexReader *ir);

/* An index encoder is a callback that writes records to the index. It accepts a pre-calculated
 * delta for encoding */
typedef size_t (*IndexEncoder)(BufferWriter *bw, t_docId delta, RSIndexResult *record);

// Create a new IndexBlockReader for a buffer. This is only used by benchmark functions
// for decoders
IndexBlockReader NewIndexBlockReader(BufferReader *buff, t_docId curBaseId);

// Create a new IndexDecoderCtx with a default numeric filter. Used only benchmarks
IndexDecoderCtx NewIndexDecoderCtx_NumericFilter();

// Create a new IndexDecoderCtx with a mask filter. Used only in benchmarks.
IndexDecoderCtx NewIndexDecoderCtx_MaskFilter(uint32_t mask);

/* Wrapper around the static encodeFreqsOnly to be able to access it in the Rust benchmarks. */
size_t encode_freqs_only(BufferWriter *bw, t_docId delta, RSIndexResult *res);

/* Wrapper around the static encodeFreqsFields to be able to access it in the Rust benchmarks. */
size_t encode_freqs_fields(BufferWriter *bw, t_docId delta, RSIndexResult *res);

/* Wrapper around the static encodeFreqsFieldsWide to be able to access it in the Rust benchmarks. */
size_t encode_freqs_fields_wide(BufferWriter *bw, t_docId delta, RSIndexResult *res);

/* Wrapper around the static encodeFieldsOnly to be able to access it in the Rust benchmarks. */
size_t encode_fields_only(BufferWriter *bw, t_docId delta, RSIndexResult *res);

/* Wrapper around the static encodeFieldsOnlyWide to be able to access it in the Rust benchmarks. */
size_t encode_fields_only_wide(BufferWriter *bw, t_docId delta, RSIndexResult *res);

/* Wrapper around the static encodeNumeric to be able to access it in the Rust benchmarks */
size_t encode_numeric(BufferWriter *bw, t_docId delta, RSIndexResult *res);

/* Wrapper around the static encodeDocIdsOnly to be able to access it in the Rust benchmarks */
size_t encode_docs_ids_only(BufferWriter *bw, t_docId delta, RSIndexResult *res);

/* Wrapper around the static readFreqs to be able to access it in the Rust benchmarks */
bool read_freqs(IndexBlockReader *blockReader, const IndexDecoderCtx *ctx, RSIndexResult *res);

/* Wrapper around the static readFreqsFlags to be able to access it in the Rust benchmarks */
bool read_freqs_flags(IndexBlockReader *blockReader, const IndexDecoderCtx *ctx, RSIndexResult *res);

/* Wrapper around the static readFreqsFlagsWide to be able to access it in the Rust benchmarks */
bool read_freqs_flags_wide(IndexBlockReader *blockReader, const IndexDecoderCtx *ctx, RSIndexResult *res);

/* Wrapper around the static readFlags to be able to access it in the Rust benchmarks */
bool read_flags(IndexBlockReader *blockReader, const IndexDecoderCtx *ctx, RSIndexResult *res);

/* Wrapper around the static readFlagsWide to be able to access it in the Rust benchmarks */
bool read_flags_wide(IndexBlockReader *blockReader, const IndexDecoderCtx *ctx, RSIndexResult *res);

/* Wrapper around the static readNumeric to be able to access it in the Rust benchmarks */
bool read_numeric(IndexBlockReader *blockReader, const IndexDecoderCtx *ctx, RSIndexResult *res);

/* Wrapper around the static readDocIdsOnly to be able to access it in the Rust benchmarks */
bool read_doc_ids_only(IndexBlockReader *blockReader, const IndexDecoderCtx *ctx, RSIndexResult *res);

/* Write a numeric index entry to the index. it includes only a float value and docId. Returns the
 * number of bytes written */
size_t InvertedIndex_WriteNumericEntry(InvertedIndex *idx, t_docId docId, double value);

size_t InvertedIndex_WriteEntryGeneric(InvertedIndex *idx, IndexEncoder encoder,
                                       RSIndexResult *entry);
/* Create a new index reader for numeric records, optionally using a given filter. If the filter
 * is
 * NULL we will return all the records in the index */
IndexReader *NewNumericReader(const RedisSearchCtx *sctx, InvertedIndex *idx, const NumericFilter *flt,
                              double rangeMin, double rangeMax, bool skipMulti,
                              const FieldFilterContext* filterCtx);

IndexReader *NewMinimalNumericReader(InvertedIndex *idx, bool skipMulti);

/* Get the appropriate encoder for an inverted index given its flags. Returns NULL on invalid flags
 */
IndexEncoder InvertedIndex_GetEncoder(IndexFlags flags);

/* Create a new index reader on an inverted index buffer,
 * optionally with a skip index, docTable and scoreIndex.
 * If singleWordMode is set to 1, we ignore the skip index and use the score
 * index.
 */
IndexReader *NewTermIndexReaderEx(InvertedIndex *idx, const RedisSearchCtx *sctx, FieldMaskOrIndex fieldMaskOrIndex,
                                RSQueryTerm *term, double weight);

IndexReader *NewTermIndexReader(InvertedIndex *idx);

/* Create a new index reader on an inverted index of "missing values". */
IndexReader *NewGenericIndexReader(InvertedIndex *idx, const RedisSearchCtx *sctx, double weight, uint32_t freq,
                                   t_fieldIndex fieldIndex, enum FieldExpirationPredicate predicate);

void IR_Abort(void *ctx);

/* free an index reader */
void IR_Free(IndexReader *ir);

/* Read an entry from an inverted index into RSIndexResult */
int IR_Read(void *ctx, RSIndexResult **e);

/**
 * Skip to a specific document ID in the index, or one position after it
 * @param ctx the index reader
 * @param docId the document ID to search for
 * @param hit where to store the result pointer
 *
 * @return:
 *  - INDEXREAD_OK if the id was found
 *  - INDEXREAD_NOTFOUND if the reader is at the next position
 *  - INDEXREAD_EOF if the ID is out of the upper range
 */
int IR_SkipTo(void *ctx, t_docId docId, RSIndexResult **hit);

void IR_Rewind(void *ctx);

/* LastDocId of an inverted index stateful reader */
t_docId IR_LastDocId(void *ctx);

/* Create a reader iterator that iterates an inverted index record */
IndexIterator *NewReadIterator(IndexReader *ir);

size_t IndexBlock_Repair(IndexBlock *blk, DocTable *dt, IndexFlags flags, IndexRepairParams *params);

#ifdef __cplusplus
}
#endif
#endif
