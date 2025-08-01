/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#include "buffer.h"
#include "rmalloc.h"
#include "rmutil/rm_assert.h"
#include <sys/param.h>

size_t Buffer_Grow(Buffer *buf, size_t extraLen) {
  size_t originalCap = buf->cap;
  do {
    buf->cap += MIN(1 + buf->cap / 5, 1024 * 1024);
  } while (buf->offset + extraLen > buf->cap);

  RS_LOG_ASSERT_FMT(extraLen <= UINT32_MAX && buf->cap > originalCap, "Buffer_Grow: cap is not growing, extraLen: %zu, originalCap: %zu, buf->cap: %zu", extraLen, originalCap, buf->cap);

  buf->data = rm_realloc(buf->data, buf->cap);
  return (buf->cap - originalCap);
}

/**
Truncate the buffer to newlen. If newlen is 0 - truncate capacity
*/
size_t Buffer_Truncate(Buffer *b, size_t newlen) {
  if (newlen == 0) {
    newlen = Buffer_Offset(b);
  }

  // we might have an empty buffer, in this case we set the data to NULL and free it
  if (newlen == 0) {
    rm_free(b->data);
    b->data = NULL;
  } else {
    b->data = rm_realloc(b->data, newlen);
  }
  b->cap = newlen;
  return newlen;
}

BufferWriter NewBufferWriter(Buffer *b) {
  BufferWriter ret = {.buf = b, .pos = b->offset};
  return ret;
}

BufferReader NewBufferReader(Buffer *b) {
  BufferReader ret = {.buf = b, .pos = 0};
  return ret;
}

/* Initialize a static buffer and fill its data */
void Buffer_Init(Buffer *b, size_t cap) {
  b->cap = cap;
  b->offset = 0;
  b->data = rm_malloc(cap);
}

Buffer *Buffer_Wrap(char *data, size_t len) {
  Buffer *buf = rm_malloc(sizeof(Buffer));
  buf->cap = len;
  buf->offset = 0;
  buf->data = data;
  return buf;
}

size_t Buffer_Free(Buffer *buf) {
  // buf->cap is the number of bytes allocated,
  // buf->offset is the number of bytes used
  size_t bytes_to_release = buf->cap;
  buf->offset = 0;
  buf->cap = 0;
  rm_free(buf->data);
  return bytes_to_release;
}

/**
Consme one byte from the buffer
@return 0 if at end, 1 if consumed
*/
inline size_t Buffer_ReadByte(BufferReader *br, char *c) {
  // if (BufferAtEnd(b)) {
  //     return 0;
  // }
  *c = br->buf->data[br->pos++];
  //++b->buf->offset;
  return 1;
}

size_t BufferWriter_Seek(BufferWriter *b, size_t offset) {
  if (offset > b->buf->cap) {
    return b->buf->offset;
  }
  b->pos = offset;
  b->buf->offset = offset;

  return offset;
}

size_t Buffer_WriteAt(BufferWriter *b, size_t offset, void *data, size_t len) {
  size_t pos = b->buf->offset;
  BufferWriter_Seek(b, offset);

  size_t sz = Buffer_Write(b, data, len);
  BufferWriter_Seek(b, pos);
  return sz;
}
/**
Seek to a specific offset. If offset is out of bounds we seek to the end.
@return the effective seek position
*/
inline size_t Buffer_Seek(BufferReader *br, size_t where) {
  Buffer *b = br->buf;

  br->pos = MIN(where, b->cap);

  return where;
}
