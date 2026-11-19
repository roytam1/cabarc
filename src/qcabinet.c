/*
 * Cabinet writer for Quantum-compressed folders.
 *
 * Layout: CFHEADER, CFFOLDER[], CFFILE[], CFDATA[].
 * Each Quantum CFDATA block holds one frame of at most 32768
 * uncompressed bytes.  CFFOLDER.typeCompress is packed as
 *   (window_bits << 8) | (level << 4) | 2   (2 == QUANTUM).
 *
 * C89 / MSVC6 compatible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qcabinet.h"
#include "quantum.h"
#include "mszip.h"
#include "lzxenc.h"
#include "lzxdec.h"

#define tcompNONE_CAB 0

typedef struct { const unsigned char *buf; unsigned long len, pos; }
  LZX_INPUT_STRUCT;
static int qcab_lzx_fill(void *ctx, unsigned char *buf, unsigned long want)
{
  LZX_INPUT_STRUCT *in = (LZX_INPUT_STRUCT *)ctx;
  unsigned long n = in->len - in->pos;
  if (!n) return 0;
  if (n > want) n = want;
  memcpy(buf, in->buf + in->pos, n);
  in->pos += n;
  return (int)n;
}

static void put16(unsigned char *p, unsigned short v)
{
  p[0] = (unsigned char)(v & 0xFF);
  p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(unsigned char *p, unsigned long v)
{
  p[0] = (unsigned char)(v & 0xFF);
  p[1] = (unsigned char)((v >> 8) & 0xFF);
  p[2] = (unsigned char)((v >> 16) & 0xFF);
  p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static int buf_append(unsigned char **buf, unsigned long *len,
                      unsigned long *cap, const void *data, unsigned long n)
{
  if (*len + n > *cap) {
    unsigned long newcap = *cap ? *cap : 4096;
    unsigned char *p;
    while (newcap < *len + n)
      newcap *= 2;
    p = (unsigned char *)realloc(*buf, newcap);
    if (!p)
      return 0;
    *buf = p;
    *cap = newcap;
  }
  if (n)
    memcpy(*buf + *len, data, (size_t)n);
  *len += n;
  return 1;
}

int qcab_write(const char *path, QCAB_FILE *files, int nfiles,
               int nfolders, int window_bits, int level,
               unsigned short setID, unsigned int reserve, int comp_type)
{
  unsigned char *filetab = NULL, *blocks = NULL, *image = NULL;
  unsigned char *frame = NULL, *comp = NULL, *block = NULL;
  unsigned long filelen = 0, filecap = 0;
  unsigned long blen = 0, bcap = 0;
  unsigned long *fstart = NULL, *fcount = NULL, *fend = NULL;
  unsigned long coff_files, data_start, total;
  unsigned short ctype;
  int *fidxs = NULL;
  int i, f, curfolder, ok = 0;
  FILE *fp;

  if (nfiles < 1 || nfolders < 1)
    return 0;
  if (window_bits < 10 || window_bits > 21)
    window_bits = 15;
  if (level < 1 || level > 7)
    level = 7;
  if (reserve > 65535)
    reserve = 0;

  /* Each LZX CFDATA block written here is self-contained, so it can be
     decoded independently; each file still occupies one folder. */
  fidxs = (int *)malloc((size_t)nfiles * sizeof(int));
  if (!fidxs)
    return 0;
  if (comp_type == 3) {
    for (i = 0; i < nfiles; i++)
      fidxs[i] = i;
    nfolders = nfiles;
  }
  else {
    for (i = 0; i < nfiles; i++)
      fidxs[i] = files[i].folder;
  }

  fstart = (unsigned long *)calloc((size_t)nfolders, sizeof(unsigned long));
  fend   = (unsigned long *)calloc((size_t)nfolders, sizeof(unsigned long));
  fcount = (unsigned long *)calloc((size_t)nfolders, sizeof(unsigned long));
  if (!fstart || !fend || !fcount)
    goto done;

  frame = (unsigned char *)malloc(QTM_FRAME_SIZE);
  comp  = (unsigned char *)malloc(QTM_FRAME_SIZE + 10240);
  block = (unsigned char *)malloc(8 + QTM_FRAME_SIZE + 10240);
  if (!frame || !comp || !block)
    goto done;

  /* ---- CFFILE table and per-file offsets within each folder ---- */
  curfolder = -1;
  {
    unsigned long running = 0;
    for (i = 0; i < nfiles; i++) {
      unsigned long namelen = (unsigned long)strlen(files[i].name) + 1;
      unsigned char entry[256 + 20];
      if (namelen > 256)
        goto done;
      if (fidxs[i] != curfolder) {
        curfolder = fidxs[i];
        running = 0;
      }
      memset(entry, 0, sizeof(entry));
      put32(entry, files[i].size);
      put32(entry + 4, running);
      put16(entry + 8, (unsigned short)fidxs[i]);
      put16(entry + 10, files[i].date);
      put16(entry + 12, files[i].time);
      put16(entry + 14, files[i].attribs);
      memcpy(entry + 16, files[i].name, namelen);
      if (!buf_append(&filetab, &filelen, &filecap, entry, 16 + namelen))
        goto done;
      running += files[i].size;
    }
  }

  /* ---- compress each file's data into framed CFDATA blocks ---- */
  curfolder = -1;
  {
    int folder_first = 1;
    for (i = 0; i < nfiles; i++) {
    unsigned long p = 0;
    if (fidxs[i] != curfolder) {
      curfolder = fidxs[i];
      folder_first = 1;
      if (curfolder >= 0 && curfolder < nfolders)
        fstart[curfolder] = blen;
    }
    while (p < files[i].size) {
      unsigned int chunk = (unsigned int)(files[i].size - p);
      unsigned int cbytes;
      if (chunk > QTM_FRAME_SIZE)
        chunk = QTM_FRAME_SIZE;
      memcpy(frame, files[i].data + p, chunk);
      if (comp_type == 0) {
        memcpy(comp, frame, chunk);
        cbytes = chunk;
      }
      else if (comp_type == 1) {
        cbytes = mszip_compress(frame, chunk, comp, QTM_FRAME_SIZE + 10240);
      }
      else if (comp_type == 3) {
        /* Multi-frame LZX streams do not yet interoperate with FDI; use
           self-contained uncompressed blocks for larger files so every
           cabinet stays readable.  Single-frame files get real LZX. */
        if (files[i].size > QTM_FRAME_SIZE)
          cbytes = lzx_write_uncompressed(frame, chunk, comp,
                                          QTM_FRAME_SIZE + 10240,
                                          folder_first);
        else
          cbytes = lzx_compress_frame(frame, chunk, comp,
                                      QTM_FRAME_SIZE + 10240, 1);
        folder_first = 0;
      }
      else {
        if (folder_first)
          qtm_enc_begin(window_bits, level);
        cbytes = qtm_enc_frame(frame, chunk, comp, QTM_FRAME_SIZE + 10240);
        folder_first = 0;
      }
      if (cbytes == 0)
        goto done;
      memset(block, 0, 8);
      put16(block + 4, (unsigned short)cbytes);
      put16(block + 6, (unsigned short)chunk);
      memcpy(block + 8, comp, cbytes);
      if (!buf_append(&blocks, &blen, &bcap, block, 8 + cbytes))
        goto done;
      if (curfolder >= 0 && curfolder < nfolders)
        fcount[curfolder]++;
      p += chunk;
    }
    if (curfolder >= 0 && curfolder < nfolders)
      fend[curfolder] = blen;
    }
  }

  coff_files = 36 + reserve + (unsigned long)nfolders * 8;
  data_start = coff_files + filelen;
  total = data_start + blen;

  image = (unsigned char *)malloc((size_t)total);
  if (!image)
    goto done;
  memset(image, 0, (size_t)total);

  memcpy(image, "MSCF", 4);
  put32(image + 4, 0);
  put32(image + 8, total);
  put32(image + 12, 0);
  put32(image + 16, coff_files);
  put32(image + 20, 0);
  image[24] = 3;
  image[25] = 1;
  put16(image + 26, (unsigned short)nfolders);
  put16(image + 28, (unsigned short)nfiles);
  put16(image + 30, setID);
  put16(image + 32, 0);
  put16(image + 34, (unsigned short)reserve);

  ctype = (comp_type == 0) ? tcompNONE_CAB
          : (comp_type == 1) ? (unsigned short)1
          : (comp_type == 3) ? (unsigned short)((window_bits << 8) | 3)
          : (unsigned short)((window_bits << 8) | (level << 4) | 2);
  {
    unsigned long off = 36 + reserve;
    for (f = 0; f < nfolders; f++) {
      put32(image + off, data_start + fstart[f]);
      put16(image + off + 4, (unsigned short)fcount[f]);
      put16(image + off + 6, ctype);
      off += 8;
    }
  }

  memcpy(image + coff_files, filetab, filelen);
  memcpy(image + data_start, blocks, blen);

  fp = fopen(path, "wb");
  if (!fp)
    goto done;
  if (fwrite(image, 1, (size_t)total, fp) != total) {
    fclose(fp);
    goto done;
  }
  fclose(fp);
  ok = 1;

done:
  if (filetab) free(filetab);
  if (blocks) free(blocks);
  if (image) free(image);
  if (frame) free(frame);
  if (comp) free(comp);
  if (block) free(block);
  if (fidxs) free(fidxs);
  if (fstart) free(fstart);
  if (fend) free(fend);
  if (fcount) free(fcount);
  return ok;
}
