#ifndef LZXENC_H
#define LZXENC_H
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Compress one 32 KB frame as LZX uncompressed blocks.
   first_frame: emit the one-time stream header bit. */
unsigned int lzx_compress_frame(const unsigned char *in, unsigned int n,
                                unsigned char *out, unsigned int cap,
                                int first_frame);
/* start a folder; windowBits 15..21 selects the position-slot table */
void lzx_enc_begin(int windowBits);
unsigned int lzx_write_uncompressed(const unsigned char *in, unsigned int n,
                                    unsigned char *out, unsigned int cap,
                                    int first_frame);
#ifdef __cplusplus
}
#endif
#endif
