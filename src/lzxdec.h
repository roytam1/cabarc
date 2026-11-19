/*
 * LZX decoder for Microsoft Cabinet files.
 *
 * Plain C89 port of CabLzx.cpp, written from the published format
 * description.  No C++ runtime is needed.
 */
#ifndef LZXDEC_H
#define LZXDEC_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*LzxFillFn)(void *ctx, unsigned char *buf, unsigned long want);

typedef struct LzxDecoder LzxDecoder;

LzxDecoder *LzxCreate(int windowBits, unsigned long totalLength,
                      LzxFillFn fill, void *fillCtx);
void LzxDestroy(LzxDecoder *d);
int LzxDecode(LzxDecoder *d, unsigned char *out, unsigned long len);

#ifdef __cplusplus
}
#endif

#endif
