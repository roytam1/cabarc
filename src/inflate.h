/*
 * Minimal raw-DEFLATE (RFC 1951) inflater for MSZIP cabinet folders.
 * C89 / MSVC6 compatible.
 */
#ifndef INFLATE_H
#define INFLATE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inflate a raw DEFLATE stream into out (at most out_bytes).
   Returns the number of bytes produced, or 0 on error. */
unsigned long inflate_raw(const unsigned char *in, unsigned long in_bytes,
                          unsigned char *out, unsigned long out_bytes);

#ifdef __cplusplus
}
#endif

#endif
