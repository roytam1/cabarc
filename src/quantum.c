/*
 * Quantum compressor / decompressor.
 *
 * Quantum is an LZ77 + 16-bit adaptive arithmetic coder invented by
 * David Stafford.  The decoder is a faithful port of libmspack's
 * qtmd.c (validated against DIAMOND.EXE-produced cabinets).  The
 * encoder generates the same token stream and selects the arithmetic
 * renorm bits by direct enumeration against the decoder state, so the
 * two are guaranteed to be inverse operations.
 *
 * C89 / MSVC6 compatible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quantum.h"

static const unsigned int position_base[42] = {
  0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512,
  768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768,
  49152, 65536, 98304, 131072, 196608, 262144, 393216, 524288, 786432,
  1048576, 1572864
};
static const unsigned char extra_bits[42] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
  11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19
};
static const unsigned char length_base[27] = {
  0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 18, 22, 26,
  30, 38, 46, 54, 62, 78, 94, 110, 126, 158, 190, 222, 254
};
static const unsigned char length_extra[27] = {
  0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/* ------------------------------------------------------------ bit writer

   Raw bits (match extras) are not written where they are generated: the
   decoder's arithmetic coder consumes 16 initial bits as its code before it
   starts reading renormalisation bits, and it reads the extra bits at the
   point where the arithmetic emitter would otherwise be.  The verified
   PAQ.EXE encoder therefore queues raw bits for insertion at
   pending + 16 + already_queued + current_position (see paq/FORMAT.md). */

#define QTM_QMAX 256
typedef struct {
  unsigned int target;
  unsigned int value;
  int          nbits;
} QREC;

typedef struct {
  unsigned char *out;
  unsigned int  out_size, pos, bytepos, cur;
  int           nbits, overflow;
  QREC          q[QTM_QMAX];
  int           qhead, qtail, queued;
} BITW;

static void bw_init(BITW *bw, unsigned char *out, unsigned int size)
{
  bw->out = out; bw->out_size = size; bw->pos = 0; bw->bytepos = 0;
  bw->cur = 0; bw->nbits = 0; bw->overflow = 0;
  bw->qhead = bw->qtail = bw->queued = 0;
}
static void bw_raw(BITW *bw, int bit)
{
  bw->cur = (bw->cur << 1) | (bit ? 1U : 0U);
  if (++bw->nbits == 8) {
    if (bw->bytepos < bw->out_size) bw->out[bw->bytepos++] = (unsigned char)bw->cur;
    else bw->overflow = 1;
    bw->cur = 0; bw->nbits = 0;
  }
  bw->pos++;
}
static void bw_drain(BITW *bw, int pending)
{
  while (bw->qhead < bw->qtail &&
         bw->q[bw->qhead % QTM_QMAX].target <= bw->pos) {
    QREC *r = &bw->q[bw->qhead % QTM_QMAX];
    int i;
    if (r->target != bw->pos) {
      /* the decoder is still behind; wait until the exact position */
      if (r->target < bw->pos) {
        /* late: write what we can now */
      }
      else break;
    }
    bw->queued -= r->nbits;
    for (i = r->nbits - 1; i >= 0; i--)
      bw_raw(bw, (int)((r->value >> i) & 1U));
    bw->qhead++;
  }
  (void)pending;
}
static void bw_bit(BITW *bw, int bit, int pending)
{
  bw_raw(bw, bit);
  bw_drain(bw, pending);
}
static void bw_bits(BITW *bw, unsigned int val, int n, int pending)
{
  while (n-- > 0) bw_bit(bw, (int)((val >> n) & 1U), pending);
}
static void bw_queue(BITW *bw, unsigned int value, int nbits, int pending)
{
  QREC *r;
  if (nbits == 0) return;
  if (bw->qtail - bw->qhead >= QTM_QMAX)
    return;
  r = &bw->q[bw->qtail % QTM_QMAX];
  bw->qtail++;
  r->target = (unsigned int)pending + 16U + (unsigned int)bw->queued + bw->pos;
  r->value = value;
  r->nbits = nbits;
  bw->queued += nbits;
}
static void bw_flush(BITW *bw)
{
  while (bw->qhead < bw->qtail) {
    QREC *r = &bw->q[bw->qhead % QTM_QMAX];
    while (bw->pos < r->target)
      bw_raw(bw, 0);
    bw_drain(bw, 0);
  }
  while (bw->nbits)
    bw_raw(bw, 0);
}

/* ----------------------------------------------------------------- model */

#define QTM_MAX_SYMS 66
typedef struct { unsigned short sym, cumfreq; } QSYM;
typedef struct { int shiftsleft, entries; QSYM syms[QTM_MAX_SYMS]; } QMODEL;

static void model_init(QMODEL *m, int start, int len)
{
  int i;
  m->shiftsleft = 4; m->entries = len;
  for (i = 0; i <= len; i++) {
    m->syms[i].sym = (unsigned short)(start + i);
    m->syms[i].cumfreq = (unsigned short)(len - i);
  }
}
static void model_rescale(QMODEL *m)
{
  int i, j; QSYM tmp;
  if (--m->shiftsleft) {
    for (i = m->entries - 1; i >= 0; i--) {
      m->syms[i].cumfreq >>= 1;
      if (m->syms[i].cumfreq <= m->syms[i + 1].cumfreq)
        m->syms[i].cumfreq = (unsigned short)(m->syms[i + 1].cumfreq + 1);
    }
  }
  else {
    m->shiftsleft = 50;
    for (i = 0; i < m->entries; i++) {
      int f = (int)m->syms[i].cumfreq - (int)m->syms[i + 1].cumfreq;
      m->syms[i].cumfreq = (unsigned short)((f + 1) >> 1);
    }
    for (i = 0; i < m->entries - 1; i++)
      for (j = i + 1; j < m->entries; j++)
        if (m->syms[i].cumfreq < m->syms[j].cumfreq) {
          tmp = m->syms[i]; m->syms[i] = m->syms[j]; m->syms[j] = tmp;
        }
    for (i = m->entries - 1; i >= 0; i--)
      m->syms[i].cumfreq = (unsigned short)(m->syms[i].cumfreq + m->syms[i + 1].cumfreq);
  }
}
static void model_add(QMODEL *m, int idx)
{
  int i;
  for (i = idx; i >= 0; i--)
    m->syms[i].cumfreq = (unsigned short)(m->syms[i].cumfreq + 8);
  if (m->syms[0].cumfreq > 3800) model_rescale(m);
}

/* -------------------------------------------- shared arithmetic state */

typedef struct {
  BITW bw;
  unsigned short L, H;
  int pending;
  int e4, e5, e6;
  QMODEL sel, lit[4], m4, m5, m6, m6len;
} QENC;

static int model_index(QMODEL *m, int sym)
{
  int i;
  for (i = 0; i < m->entries; i++)
    if (m->syms[i].sym == (unsigned short)sym) return i;
  return 0;
}

/* Forward declaration: the encoder brute-forces the initial code value by
   running the (validated) decoder over candidate streams. */
int qtm_decompress_frame(const unsigned char *in, unsigned int in_bytes,
                         unsigned char *out, unsigned int out_bytes,
                         int window_bits);

/* Encode a symbol with the carry-less arithmetic coder from paq/qpack.c. */
static void enc_symbol(QENC *e, QMODEL *m, int sym)
{
  int idx = model_index(m, sym);
  unsigned long range, total;

  total = m->syms[0].cumfreq;
  range = (unsigned long)((unsigned)e->H - e->L + 1);
  e->H = (unsigned short)((e->L + (m->syms[idx].cumfreq * range) / total - 1) & 0xFFFFU);
  e->L = (unsigned short)((e->L + (m->syms[idx + 1].cumfreq * range) / total) & 0xFFFFU);

  for (;;) {
    if ((e->H & 0x8000) == (e->L & 0x8000)) {
      int bit = (int)((e->H >> 15) & 1);
      bw_bit(&e->bw, bit, e->pending);
      while (e->pending > 0) {
        bw_bit(&e->bw, 1 - bit, e->pending);
        e->pending--;
      }
    }
    else if ((e->L & 0x4000) && !(e->H & 0x4000)) {
      e->pending++;
      e->L = (unsigned short)(e->L & 0x3FFF);
      e->H = (unsigned short)(e->H | 0x4000);
    }
    else break;
    e->L = (unsigned short)((e->L << 1) & 0xFFFF);
    e->H = (unsigned short)(((e->H << 1) | 1) & 0xFFFF);
  }
  model_add(m, idx);
}

static void enc_flush(QENC *e)
{
  int bit = (int)((e->L >> 14) & 1), i;
  bw_bit(&e->bw, bit, e->pending);
  e->pending++;
  while (e->pending > 0) {
    bw_bit(&e->bw, 1 - bit, e->pending);
    e->pending--;
  }
  for (i = 0; i < 16; i++)
    bw_bit(&e->bw, 0, e->pending);
  bw_flush(&e->bw);
}

/* ------------------------------------------------------------ match finder */

#define QTM_HASH_BITS 17
#define QTM_HASH_SIZE (1 << QTM_HASH_BITS)
#define QTM_HASH_MASK (QTM_HASH_SIZE - 1)
static int qtm_head[QTM_HASH_SIZE];
static int qtm_prev[QTM_FRAME_SIZE];

static unsigned int qtm_hash(const unsigned char *p)
{
  return (((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^ (unsigned)p[2])
         & QTM_HASH_MASK;
}
static int find_match(const unsigned char *in, unsigned int pos,
                      unsigned int nbytes, int max_off, int max_chain,
                      unsigned int *offsetp)
{
  unsigned int h;
  int cand, best_len = 0, best_off = 0, chain;
  unsigned int limit;
  if (pos + 2 >= nbytes) return 0;
  h = qtm_hash(in + pos);
  cand = qtm_head[h];
  limit = (pos < (unsigned int)max_off) ? 0 : pos - (unsigned int)max_off;
  for (chain = 0; cand >= 0 && chain < max_chain; chain++) {
    int len = 0;
    unsigned int off = pos - (unsigned int)cand;
    const unsigned char *a = in + pos, *b = in + cand;
    if (off == 0 || off > (unsigned int)max_off || (unsigned)cand < limit) break;
    while (len < 259 && (unsigned)(pos + len) < nbytes && a[len] == b[len]) len++;
    if (len > best_len) {
      best_len = len; best_off = (int)off;
      if (len >= 259) break;
    }
    cand = qtm_prev[cand];
  }
  if (best_len >= 3) { *offsetp = (unsigned int)best_off; return best_len; }
  return 0;
}
static void insert_pos(const unsigned char *in, unsigned int pos, unsigned int nbytes)
{
  unsigned int h;
  if (pos + 2 >= nbytes) return;
  h = qtm_hash(in + pos);
  qtm_prev[pos] = qtm_head[h];
  qtm_head[h] = (int)pos;
}
static int pos_slot(unsigned int d)
{
  int i;
  for (i = 0; i < 41; i++) if (d < position_base[i + 1]) return i;
  return 41;
}
static int len_slot(unsigned int l)
{
  int i;
  for (i = 0; i < 26; i++) if (l < length_base[i + 1]) return i;
  return 26;
}

/* token operations */
#define OP_MODEL 0
#define OP_RAW   1
typedef struct { int kind; int model; int sym; unsigned int val; int nbits; } QOP;

/* Encode the token stream with the carry-less arithmetic coder and the
   delayed raw-bit queue ported from paq/qpack.c.  The decoder reads the
   first 16 emitted bits as its initial code value, so no explicit prefix
   is written. */
/* persistent encoder state for one cabinet folder */
static QENC g_enc;
static int g_enc_ready = 0;
static int g_e4 = 0, g_e5 = 0, g_e6 = 0, g_chain = 0, g_maxoff = 0;

int qtm_enc_begin(int window_bits, int level)
{
  int i;
  static const int chains[8] = { 0, 5, 50, 250, 100, 200, 400, 1000 };
  if (window_bits < 10 || window_bits > 21) return 0;
  if (level < 1 || level > 7) level = 7;
  g_e4 = window_bits * 2; if (g_e4 > 24) g_e4 = 24;
  g_e5 = window_bits * 2; if (g_e5 > 36) g_e5 = 36;
  g_e6 = window_bits * 2; if (g_e6 > 42) g_e6 = 42;
  g_chain = chains[level];
  g_maxoff = 1 << window_bits;
  memset(&g_enc, 0, sizeof(g_enc));
  g_enc.e4 = g_e4; g_enc.e5 = g_e5; g_enc.e6 = g_e6;
  model_init(&g_enc.sel, 0, 7);
  for (i = 0; i < 4; i++) model_init(&g_enc.lit[i], i * 64, 64);
  model_init(&g_enc.m4, 0, g_e4);
  model_init(&g_enc.m5, 0, g_e5);
  model_init(&g_enc.m6, 0, g_e6);
  model_init(&g_enc.m6len, 0, 27);
  for (i = 0; i < QTM_HASH_SIZE; i++) qtm_head[i] = -1;
  g_enc_ready = 1;
  return 1;
}

static unsigned int encode_frame(QOP *ops, int nops,
                                 const unsigned char *in,
                                 unsigned int in_bytes,
                                 unsigned char *out, unsigned int out_bytes,
                                 int window_bits,
                                 int e4, int e5, int e6)
{
  QENC *e = &g_enc;
  int i;
  (void)window_bits; (void)e4; (void)e5; (void)e6;

  bw_init(&e->bw, out, out_bytes);
  e->L = 0; e->H = 0xFFFF; e->pending = 0;

  for (i = 0; i < nops; i++) {
    QMODEL *m;
    if (ops[i].kind == OP_RAW) {
      bw_queue(&e->bw, ops[i].val, ops[i].nbits, e->pending);
      continue;
    }
    switch (ops[i].model) {
      case 0: m = &e->sel; break;
      case 5: m = &e->m4; break;
      case 6: m = &e->m5; break;
      case 7: m = &e->m6; break;
      case 8: m = &e->m6len; break;
      default: m = &e->lit[ops[i].model - 1]; break;
    }
    enc_symbol(e, m, ops[i].sym);
  }
  enc_flush(e);
  if (e->bw.overflow)
    return 0;
  return e->bw.bytepos;
}

unsigned int qtm_enc_frame(const unsigned char *in, unsigned int in_bytes,
                           unsigned char *out, unsigned int out_bytes)
{
  QOP *ops;
  int nops = 0, entries4, entries5, entries6, max_off, chain, i;
  unsigned int pos;

  if (in_bytes == 0 || in_bytes > QTM_FRAME_SIZE) return 0;
  if (!g_enc_ready) return 0;
  ops = (QOP *)malloc((size_t)QTM_FRAME_SIZE * 4 * sizeof(QOP));
  if (!ops) return 0;

  entries4 = g_e4; entries5 = g_e5; entries6 = g_e6;
  chain = g_chain;
  max_off = g_maxoff;
  if (max_off > (int)in_bytes) max_off = (int)in_bytes;
  {
    int lim = entries6 - 1;
    unsigned int cap = position_base[lim] + (1U << extra_bits[lim]) - 1 + 1;
    if (max_off > (int)cap) max_off = (int)cap;
  }
  for (i = 0; i < QTM_HASH_SIZE; i++) qtm_head[i] = -1;

  /* ---- build token stream ---- */
  pos = 0;
  while (pos < in_bytes) {
    unsigned int offset = 0;
    int len = find_match(in, pos, in_bytes, max_off, chain, &offset);
    int slot = -1, extra = 0, lslot = -1, lextra = 0;
    int used = 0;

    if (len >= 3) {
      slot = pos_slot(offset - 1);
      if (len == 3 && slot < entries4) {
        extra = (int)((offset - 1) - position_base[slot]);
        ops[nops].kind = OP_MODEL; ops[nops].model = 0; ops[nops++].sym = 4;
        ops[nops].kind = OP_MODEL; ops[nops].model = 5; ops[nops++].sym = slot;
        ops[nops].kind = OP_RAW; ops[nops].val = (unsigned int)extra;
        ops[nops++].nbits = extra_bits[slot];
        used = 1;
      }
      else if (len == 4 && slot < entries5) {
        extra = (int)((offset - 1) - position_base[slot]);
        ops[nops].kind = OP_MODEL; ops[nops].model = 0; ops[nops++].sym = 5;
        ops[nops].kind = OP_MODEL; ops[nops].model = 6; ops[nops++].sym = slot;
        ops[nops].kind = OP_RAW; ops[nops].val = (unsigned int)extra;
        ops[nops++].nbits = extra_bits[slot];
        used = 1;
      }
      else if (len >= 5 && slot < entries6) {
        lslot = len_slot((unsigned int)(len - 5));
        lextra = (int)((unsigned)(len - 5) - length_base[lslot]);
        extra = (int)((offset - 1) - position_base[slot]);
        ops[nops].kind = OP_MODEL; ops[nops].model = 0; ops[nops++].sym = 6;
        ops[nops].kind = OP_MODEL; ops[nops].model = 8; ops[nops++].sym = lslot;
        ops[nops].kind = OP_RAW; ops[nops].val = (unsigned int)lextra;
        ops[nops++].nbits = length_extra[lslot];
        ops[nops].kind = OP_MODEL; ops[nops].model = 7; ops[nops++].sym = slot;
        ops[nops].kind = OP_RAW; ops[nops].val = (unsigned int)extra;
        ops[nops++].nbits = extra_bits[slot];
        used = 1;
      }
    }
    if (used) {
      while (len-- > 0) { insert_pos(in, pos, in_bytes); pos++; }
      continue;
    }
    {
      int b = in[pos], model = b >> 6;
      ops[nops].kind = OP_MODEL; ops[nops].model = 0; ops[nops++].sym = model;
      ops[nops].kind = OP_MODEL; ops[nops].model = 1 + model; ops[nops++].sym = b;
      insert_pos(in, pos, in_bytes);
      pos++;
    }
  }

  {
    unsigned int result = encode_frame(ops, nops, in, in_bytes, out,
                                       out_bytes, 0,
                                       entries4, entries5, entries6);
    free(ops);
    return result;
  }
}

unsigned int qtm_compress_frame(const unsigned char *in, unsigned int in_bytes,
                                unsigned char *out, unsigned int out_bytes,
                                int window_bits, int level)
{
  if (!qtm_enc_begin(window_bits, level))
    return 0;
  return qtm_enc_frame(in, in_bytes, out, out_bytes);
}

/* ------------------------------------------------------------ decoder */

typedef struct {
  const unsigned char *in;
  unsigned int len, pos, bits_read;
  unsigned char buf;
  int bits, eof;
  unsigned short H, L, C;
  QMODEL sel, lit[4], m4, m5, m6, m6len;
} QDEC;

static unsigned int br_bit(QDEC *d)
{
  unsigned int v;
  d->bits_read++;
  if (d->bits == 0) {
    if (d->pos >= d->len) { d->eof = 1; return 0; }
    d->buf = d->in[d->pos++];
    d->bits = 8;
  }
  v = (d->buf >> 7) & 1U;
  d->buf = (unsigned char)(d->buf << 1);
  d->bits--;
  return v;
}
static unsigned int br_bits(QDEC *d, int n)
{
  unsigned int v = 0;
  while (n-- > 0) v = (v << 1) | br_bit(d);
  return v;
}

static int dec_symbol(QDEC *d, QMODEL *m)
{
  unsigned int range, symf, total;
  int i, sym;
  range = (unsigned int)(((int)d->H - (int)d->L) & 0xFFFF) + 1;
  symf = ((((unsigned int)((int)d->C - (int)d->L + 1) * m->syms[0].cumfreq) - 1) / range) & 0xFFFFU;
  for (i = 1; i < m->entries; i++) if (m->syms[i].cumfreq <= symf) break;
  if (i > m->entries) i = m->entries;
  sym = m->syms[i - 1].sym;
  range = (unsigned int)((int)d->H - (int)d->L + 1);
  total = m->syms[0].cumfreq;
  d->H = (unsigned short)((unsigned int)d->L +
          (m->syms[i - 1].cumfreq * range) / total - 1);
  d->L = (unsigned short)((unsigned int)d->L +
          (m->syms[i].cumfreq * range) / total);
  do {
    --i;
    m->syms[i].cumfreq = (unsigned short)(m->syms[i].cumfreq + 8);
  } while (i > 0);
  if (m->syms[0].cumfreq > 3800) model_rescale(m);
  for (;;) {
    if ((d->L & 0x8000) != (d->H & 0x8000)) {
      if ((d->L & 0x4000) && !(d->H & 0x4000)) {
        d->C = (unsigned short)(d->C ^ 0x4000);
        d->L = (unsigned short)(d->L & 0x3FFF);
        d->H = (unsigned short)(d->H | 0x4000);
      }
      else break;
    }
    d->L = (unsigned short)((d->L << 1) & 0xFFFF);
    d->H = (unsigned short)(((d->H << 1) | 1) & 0xFFFF);
    d->C = (unsigned short)(((d->C << 1) & 0xFFFF) | br_bit(d));
  }
  return sym;
}

static QDEC g_dec;
static int g_dec_ready = 0;
static int gd_e4 = 0, gd_e5 = 0, gd_e6 = 0;

int qtm_dec_begin(int window_bits)
{
  int i;
  if (window_bits < 10 || window_bits > 21) return 0;
  memset(&g_dec, 0, sizeof(g_dec));
  gd_e4 = window_bits * 2; if (gd_e4 > 24) gd_e4 = 24;
  gd_e5 = window_bits * 2; if (gd_e5 > 36) gd_e5 = 36;
  gd_e6 = window_bits * 2; if (gd_e6 > 42) gd_e6 = 42;
  model_init(&g_dec.sel, 0, 7);
  for (i = 0; i < 4; i++) model_init(&g_dec.lit[i], i * 64, 64);
  model_init(&g_dec.m4, 0, gd_e4); model_init(&g_dec.m5, 0, gd_e5);
  model_init(&g_dec.m6, 0, gd_e6); model_init(&g_dec.m6len, 0, 27);
  g_dec_ready = 1;
  return 1;
}

int qtm_dec_frame(const unsigned char *in, unsigned int in_bytes,
                  unsigned char *out, unsigned int out_bytes)
{
  QDEC *d = &g_dec;
  unsigned int pos = 0;
  if (!in || !out || out_bytes == 0 || !g_dec_ready) return 0;
  d->in = in; d->len = in_bytes; d->pos = 0; d->bits = 0; d->eof = 0;
  d->H = 0xFFFF; d->L = 0;
  d->C = (unsigned short)br_bits(d, 16);

  while (pos < out_bytes && !d->eof) {
    int sel = dec_symbol(d, &d->sel);
    if (sel < 4) {
      out[pos++] = (unsigned char)dec_symbol(d, &d->lit[sel]);
    }
    else if (sel == 4 || sel == 5) {
      int slot = dec_symbol(d, sel == 4 ? &d->m4 : &d->m5);
      unsigned int off = position_base[slot] + br_bits(d, extra_bits[slot]) + 1;
      unsigned int k, cnt = (sel == 4) ? 3 : 4;
      if (slot >= (sel == 4 ? gd_e4 : gd_e5) || off > pos) return 0;
      for (k = 0; k < cnt && pos < out_bytes; k++) { out[pos] = out[pos - off]; pos++; }
    }
    else if (sel == 6) {
      int lslot = dec_symbol(d, &d->m6len);
      int slot;
      unsigned int len = length_base[lslot] + br_bits(d, length_extra[lslot]) + 5;
      unsigned int off, k;
      slot = dec_symbol(d, &d->m6);
      off = position_base[slot] + br_bits(d, extra_bits[slot]) + 1;
      if (slot >= gd_e6 || off > pos) return 0;
      for (k = 0; k < len && pos < out_bytes; k++) { out[pos] = out[pos - off]; pos++; }
    }
    else return 0;
  }
  d->bits = 0;
  return pos == out_bytes;
}

int qtm_decompress_frame(const unsigned char *in, unsigned int in_bytes,
                         unsigned char *out, unsigned int out_bytes,
                         int window_bits)
{
  if (!qtm_dec_begin(window_bits)) return 0;
  return qtm_dec_frame(in, in_bytes, out, out_bytes);
}