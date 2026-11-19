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
/* A split point in a multi-frame block: number of compressed bytes and
   uncompressed bytes up to the end of a CFDATA chunk. */
typedef struct {
  unsigned long comp_off;
  unsigned long uncomp;
} LZX_SPLIT;
/* Compress `n` bytes as one LZX block, returning the compressed stream and
   the CFDATA split points (word-aligned token boundaries).  frame_size is
   the nominal uncompressed bytes per split (32768 for CAB). */
unsigned int lzx_compress_frames(const unsigned char *in, unsigned int n,
                                 unsigned char *out, unsigned int cap,
                                 int first_frame, unsigned int frame_size,
                                 LZX_SPLIT *splits, int maxsplits,
                                 int *nsplits);
/* start a folder; windowBits 15..21 selects the position-slot table.
   e8FileSize != 0 enables x86 E8 call translation for this folder. */
void lzx_enc_begin(int windowBits, unsigned long e8FileSize);
/* Translate E8 call targets to absolute file offsets in one 32 KB frame.
   The decoder undoes this; leave the last 10 bytes of the frame alone. */
unsigned int lzx_translate_e8(unsigned char *buf, unsigned long n,
                              unsigned long filepos, unsigned long filesize);
/* release the folder window and matcher (safe to call more than once) */
void lzx_enc_end(void);
unsigned int lzx_write_uncompressed(const unsigned char *in, unsigned int n,
                                    unsigned char *out, unsigned int cap,
                                    int first_frame);
#ifdef __cplusplus
}
#endif
#endif
