/*
 * Quantum compressor for the cabarc reconstruction.
 *
 * Quantum is an LZ77 + 16-bit adaptive arithmetic coder invented by
 * David Stafford.  The bitstream layout implemented here is compatible
 * with the decoder in cabinet.dll (QDICreateDecompression) and with
 * libmspack's qtmd.c, which was used as the normative reference.
 *
 * C89 / MSVC6 compatible.
 */
#ifndef QUANTUM_H
#define QUANTUM_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QTM_FRAME_SIZE   32768U

/* Compress one frame of uncompressed data into out.
   Returns the number of bytes written to out, or 0 on overflow.
   window_bits: 10..21, level: 1..7.
   out must have at least QTM_FRAME_SIZE + 10240 bytes of space. */
unsigned int qtm_compress_frame(const unsigned char *in,
                                unsigned int in_bytes,
                                unsigned char *out,
                                unsigned int out_bytes,
                                int window_bits,
                                int level);

/* Stateful encoding: models and coder state carry over between the
   CFDATA frames of one cabinet folder, as DIAMOND does. */
int qtm_enc_begin(int window_bits, int level);
unsigned int qtm_enc_frame(const unsigned char *in, unsigned int in_bytes,
                           unsigned char *out, unsigned int out_bytes);

/* Decompress one Quantum frame.  Returns 1 on success, 0 on error.
   out_bytes is the number of uncompressed bytes expected. */
int qtm_decompress_frame(const unsigned char *in, unsigned int in_bytes,
                         unsigned char *out, unsigned int out_bytes,
                         int window_bits);

/* Stateful decoding: models persist across the frames of one folder. */
int qtm_dec_begin(int window_bits);
int qtm_dec_frame(const unsigned char *in, unsigned int in_bytes,
                  unsigned char *out, unsigned int out_bytes);

#ifdef __cplusplus
}
#endif

#endif /* QUANTUM_H */
