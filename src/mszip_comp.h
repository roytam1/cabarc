#ifndef MSZIP_COMP_H
#define MSZIP_COMP_H
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
unsigned int MsZipCompressFrame(const unsigned char *in, unsigned long n,
                                unsigned char *out, unsigned long cap);
#ifdef __cplusplus
}
#endif
#endif
