/*
 * MSZIP compression front-end.  Uses the dynamic-Huffman DEFLATE
 * compressor extracted from mscompress-expand/compress.c.
 * C89 / MSVC6 compatible.
 */
#include <string.h>
#include "mszip.h"
#include "mszip_comp.h"

unsigned int mszip_compress(const unsigned char *in, unsigned int in_bytes,
                            unsigned char *out, unsigned int out_bytes)
{
  unsigned int pos = 0;
  unsigned int chunk;
  unsigned int got;

  while (in_bytes) {
    chunk = in_bytes;
    if (chunk > 32768U) chunk = 32768U;
    got = MsZipCompressFrame(in, chunk, out + pos, out_bytes - pos);
    if (got == 0)
      return 0;
    pos += got;
    in += chunk;
    in_bytes -= chunk;
  }
  return pos;
}
