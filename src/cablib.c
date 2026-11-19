/*
 * Standalone cabinet container reader.
 *
 * Parses CFHEADER / CFFOLDER / CFFILE / CFDATA and decompresses each
 * folder with the appropriate codec: NONE, MSZIP (raw DEFLATE),
 * QUANTUM (in-process arithmetic decoder) or LZX (supplied C++
 * decoder through lzxwrap).  No cabinet.dll is used.
 *
 * C89 / MSVC6 compatible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cablib.h"
#include "inflate.h"
#include "quantum.h"
#include "lzxdec.h"
#include "qcabinet.h"
#include "msinflate.h"

static unsigned short rd16(const unsigned char *p)
{
  return (unsigned short)(p[0] | ((unsigned)p[1] << 8));
}
static unsigned long rd32(const unsigned char *p)
{
  return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
         ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

const char *cablib_compname(unsigned short type)
{
  switch (type & 0x0F) {
    case CABLIB_COMP_NONE:    return "NONE";
    case CABLIB_COMP_MSZIP:   return "MSZIP";
    case CABLIB_COMP_QUANTUM: return "QUANTUM";
    case CABLIB_COMP_LZX:     return "LZX";
    default:                  return "UNKNOWN";
  }
}

void cablib_free(CABLIB *cab)
{
  int i;
  if (!cab) return;
  if (cab->files) {
    for (i = 0; i < cab->nfiles; i++)
      if (cab->files[i].data) free(cab->files[i].data);
    free(cab->files);
  }
  cab->files = NULL;
  cab->nfiles = 0;
}

/* ------------------------------------------------------------ LZX input */

typedef struct {
  const unsigned char *buf;
  unsigned long len, pos;
} LZX_INPUT;

static int lzx_fill(void *ctx, unsigned char *buf, unsigned long want)
{
  LZX_INPUT *in = (LZX_INPUT *)ctx;
  unsigned long n = in->len - in->pos;
  if (n == 0) return 0;
  if (n > want) n = want;
  memcpy(buf, in->buf + in->pos, n);
  in->pos += n;
  return (int)n;
}

/* ------------------------------------------------------------ folder decode */

/* ------------------------------------------------------------ folder decode */

static int decode_folder(const unsigned char *base, unsigned long base_len,
                         unsigned long coff, unsigned short cblocks,
                         unsigned short ctype, unsigned short cbReserveData,
                         unsigned char *out, unsigned long outlen,
                         char *err, int errsz)
{
  int method = ctype & 0x0F;
  int window_bits = (ctype >> 8) & 0x1F;
  unsigned long srcpos = coff;
  unsigned long dstpos = 0;
  unsigned int i;
  MsInflate z_mszip;
  int have_mszip = 0;
  int ok = 0;

  for (i = 0; i < cblocks; i++) {
    unsigned long p = srcpos;
    unsigned short cb, cu;
    const unsigned char *data;
    if (p + 8 + cbReserveData > base_len) goto done;
    cb = rd16(base + p + 4);
    cu = rd16(base + p + 6);
    data = base + p + 8 + cbReserveData;
    if (p + 8 + cbReserveData + cb > base_len) goto done;
    if (dstpos + cu > outlen) goto done;

    switch (method) {
      case CABLIB_COMP_NONE:
        if (cu != cb) goto done;
        memcpy(out + dstpos, data, cu);
        break;
      case CABLIB_COMP_MSZIP: {
        unsigned long wrote = 0;
        if (cb < 2 || data[0] != 'C' || data[1] != 'K') goto done;
        if (!have_mszip) {
          MsInflateInit(&z_mszip);
          z_mszip.dst = out;
          z_mszip.dstcap = outlen;
          z_mszip.dstpos = 0;
          have_mszip = 1;
        }
        if (MsInflateStream(&z_mszip, data + 2, cb - 2, NULL, &wrote) != 0 ||
            wrote != cu || z_mszip.dstpos != dstpos + cu)
          goto done;
        break;
      }
      case CABLIB_COMP_QUANTUM: {
        if (window_bits < 10 || window_bits > 21) goto done;
        if (i == 0 && !qtm_dec_begin(window_bits)) goto done;
        if (!qtm_dec_frame(data, cb, out + dstpos, cu)) goto done;
        break;
      }
      case CABLIB_COMP_LZX: {
        LZX_INPUT input;
        void *dec;
        if (window_bits < 15 || window_bits > 21) goto done;
        /* each LZX CFDATA block written by our tool is self-contained */
        input.buf = data;
        input.len = cb;
        input.pos = 0;
        dec = LzxCreate(window_bits, cu, lzx_fill, &input);
        if (!dec) goto done;
        if (!LzxDecode(dec, out + dstpos, cu)) {
          LzxDestroy(dec);
          goto done;
        }
        LzxDestroy(dec);
        break;
      }
      default:
        if (err) sprintf(err, "unsupported compression type %d", method);
        goto done;
    }
    dstpos += cu;
    srcpos = p + 8 + cbReserveData + cb;
  }
  ok = (dstpos == outlen);
done:
  return ok;
}

/* ------------------------------------------------------------ write cabinet */

int cablib_write(const char *path, CABLIB_FILE *files, int nfiles,
                 int nfolders, int comp_type, int window_bits, int level,
                 unsigned short setID, unsigned int reserve)
{
  QCAB_FILE *qf;
  int i, ok;
  if (nfiles < 1 || nfolders < 1)
    return 0;
  if (comp_type != CABLIB_COMP_NONE && comp_type != CABLIB_COMP_MSZIP &&
      comp_type != CABLIB_COMP_QUANTUM && comp_type != CABLIB_COMP_LZX)
    return 0;
  qf = (QCAB_FILE *)calloc((size_t)nfiles, sizeof(QCAB_FILE));
  if (!qf) return 0;
  for (i = 0; i < nfiles; i++) {
    qf[i].name = files[i].name;
    qf[i].data = files[i].data;
    qf[i].size = files[i].size;
    qf[i].date = files[i].date;
    qf[i].time = files[i].time;
    qf[i].attribs = files[i].attribs;
    qf[i].folder = files[i].folder;
  }
  ok = qcab_write(path, qf, nfiles, nfolders, window_bits, level,
                  setID, reserve, comp_type);
  free(qf);
  return ok;
}

/* ------------------------------------------------------------ read cabinet */

int cablib_read(const char *path, CABLIB *cab, char *err, int errsz)
{
  FILE *fp;
  long flen;
  unsigned long flen_image;
  unsigned char *image = NULL;
  unsigned long base = 0, p, cbCabinet, coffFiles;
  unsigned short cFolders, cFiles, flags, setID, iCab;
  unsigned short cbCFHeader = 0, cbCFFolder = 0, cbCFData = 0;
  unsigned char *folders = NULL;
  unsigned long *folder_len = NULL;
  unsigned short *folder_type = NULL;
  int i, ok = 0;
  memset(cab, 0, sizeof(*cab));
  if (err && errsz > 0) err[0] = '\0';

  fp = fopen(path, "rb");
  if (!fp) {
    if (err) _snprintf(err, errsz, "Unable to open '%s' for input", path);
    return 0;
  }
  fseek(fp, 0, SEEK_END);
  flen = ftell(fp);
  flen_image = (unsigned long)flen;
  fseek(fp, 0, SEEK_SET);
  if (flen < 36) { fclose(fp); return 0; }
  image = (unsigned char *)malloc((size_t)flen);
  if (!image) { fclose(fp); return 0; }
  if (fread(image, 1, (size_t)flen, fp) != (size_t)flen) {
    free(image); fclose(fp); return 0;
  }
  fclose(fp);

  /* Locate the MSCF signature (allows a self-extracting stub). */
  for (p = 0; p + 4 <= (unsigned long)flen; p++) {
    if (memcmp(image + p, "MSCF", 4) == 0) { base = p; break; }
  }
  if (p + 4 > (unsigned long)flen) {
    if (err) _snprintf(err, errsz, "'%s' is not a cabinet", path);
    goto done;
  }

  p = base;
  cbCabinet = rd32(image + p + 8);
  coffFiles = rd32(image + p + 16);
  cFolders = rd16(image + p + 26);
  cFiles = rd16(image + p + 28);
  flags = rd16(image + p + 30);
  setID = rd16(image + p + 32);
  iCab = rd16(image + p + 34);

  if (flags & 0x0004) {
    unsigned long q = p + 36;
    /* the byte at 36 is the reserved-field presence byte (0x00) */
    cbCFHeader = rd16(image + q + 1);
    cbCFFolder = rd16(image + q + 3);
    cbCFData = rd16(image + q + 5);
    (void)cbCFHeader;
  }

  if (cFolders == 0 || cFiles == 0) goto done;
  if (cFolders > 4096 || cFiles > 65535) goto done;
  cab->files = (CABLIB_FILE *)calloc(cFiles, sizeof(CABLIB_FILE));
  folders = (unsigned char *)malloc((size_t)cFolders * 8);
  folder_len = (unsigned long *)calloc(cFolders, sizeof(unsigned long));
  folder_type = (unsigned short *)calloc(cFolders, sizeof(unsigned short));
  if (!cab->files || !folders || !folder_len || !folder_type) goto done;
  cab->nfiles = cFiles;
  cab->setID = setID;
  cab->iCab = iCab;

  /* CFFOLDER entries start at 36 (+ reserve header area). */
  {
    unsigned long q = base + 36 + (flags & 0x0004 ? 7 + cbCFHeader : 0);
    int k;
    for (k = 0; k < cFolders; k++) {
      unsigned short nb = rd16(folders + k * 8 + 4);
      unsigned long coff = 0, r;
      unsigned short ct = rd16(image + q + 6);
      unsigned long fp = q;
      (void)coff; (void)nb; (void)ct; (void)fp; (void)r;
      memcpy(folders + k * 8, image + q, 8);
      q += 8 + cbCFFolder;
    }
  }

  /* Pre-scan CFDATA to learn each folder's uncompressed length. */
  {
    int k;
    for (k = 0; k < cFolders; k++) {
      unsigned long fp = rd32(folders + k * 8);
      unsigned short nb = rd16(folders + k * 8 + 4);
      unsigned int b;
      folder_type[k] = rd16(folders + k * 8 + 6);
      for (b = 0; b < nb; b++) {
        unsigned short cu;
        if (fp + 8 + cbCFData > (unsigned long)flen) goto done;
        cu = rd16(image + fp + 6);
        folder_len[k] += cu;
        fp += 8 + cbCFData + rd16(image + fp + 4);
      }
    }
  }

  /* CFFILE entries. */
  {
    unsigned long q = base + coffFiles;
    int k;
    for (k = 0; k < cFiles; k++) {
      CABLIB_FILE *f = &cab->files[k];
      unsigned long nameoff;
      unsigned short ifolder;
      if (q + 16 > (unsigned long)flen) goto done;
      f->size = rd32(image + q);
      f->uoff = rd32(image + q + 4);
      ifolder = rd16(image + q + 8);
      f->folder = (ifolder == 0xFFFF) ? 0 : ifolder;
      f->date = rd16(image + q + 10);
      f->time = rd16(image + q + 12);
      f->attribs = rd16(image + q + 14);
      nameoff = q + 16;
      while (nameoff < (unsigned long)flen && image[nameoff] != 0)
        nameoff++;
      if (nameoff >= (unsigned long)flen) goto done;
      {
        unsigned long n = nameoff - (q + 16);
        if (n >= sizeof(f->name)) n = sizeof(f->name) - 1;
        memcpy(f->name, image + q + 16, n);
        f->name[n] = '\0';
      }
      q = nameoff + 1;
    }
  }

  /* Decompress folders, then carve out each file's data. */
  {
    int k;
    for (k = 0; k < cFolders; k++) {
      unsigned long coff = rd32(folders + k * 8);
      unsigned short nb = rd16(folders + k * 8 + 4);
      unsigned long flen = folder_len[k];
      unsigned char *fdata;
      if (flen == 0) continue;
      fdata = (unsigned char *)malloc((size_t)flen);
      if (!fdata) goto done;
      if (!decode_folder(image, (unsigned long)flen_image, coff, nb,
                         folder_type[k], cbCFData, fdata, flen, err, errsz)) {
        free(fdata);
        goto done;
      }
      for (i = 0; i < cFiles; i++) {
        CABLIB_FILE *f = &cab->files[i];
        if (f->folder != (unsigned short)k) continue;
        if (f->uoff + f->size > flen) continue;
        f->data = (unsigned char *)malloc((size_t)f->size + 1);
        if (!f->data) continue;
        if (f->size)
          memcpy(f->data, fdata + f->uoff, f->size);
      }
      free(fdata);
    }
  }

  (void)cbCabinet;
  ok = 1;
done:
  free(image);
  free(folders);
  if (folder_len) free(folder_len);
  if (folder_type) free(folder_type);
  if (!ok)
    cablib_free(cab);
  return ok;
}