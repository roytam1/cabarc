/*
 * MSZIP writer: raw DEFLATE stored blocks plus the empty-block flush
 * marker that terminates each MSZIP CFDATA block.
 * C89 / MSVC6 compatible.
 */
#ifndef MSZIP_H
#define MSZIP_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned int mszip_compress(const unsigned char *in, unsigned int in_bytes,
                            unsigned char *out, unsigned int out_bytes);

#ifdef __cplusplus
}
#endif

#endif
