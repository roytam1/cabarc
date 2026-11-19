/*
 * Real LZX compressor for cabarc.
 *
 * Emits LZX verbatim blocks (type 1) with canonical Huffman trees:
 *   - LZ77 tokenisation with position slots and the R0/R1/R2 repeatOffsets
 *   - a main tree (256 literals + offset-slot*8 symbols) and a length tree
 *   - trees transmitted through a 20-symbol pre-tree, delta-coded
 *   - 16-bit little-endian word bit order, MSB-first bits (as CabLzx.cpp reads)
 *   - no E8 filesize (stream header bit 0), so no x86 call translation
 *
 * Falls back to an uncompressed block (type 3) if the Huffman lengths
 * cannot be represented within the 16-bit limit.
 *
 * C89 / MSVC6 compatible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lzxenc.h"

#define LZX_NUM_CHARS  256
#define LZX_PRETREE    20
#define LZX_MAX_BITS   16
#define LZX_MAX_SLOTS  290
#define LZX_MAX_SYMS   (LZX_NUM_CHARS + LZX_MAX_SLOTS * 8)

typedef struct {
  unsigned char *out;
  unsigned int cap, pos, bytepos, cur;
  int nbits;
} BW;

static void bw_init(BW *b, unsigned char *out, unsigned int cap)
{
  b->out = out; b->cap = cap; b->pos = 0; b->bytepos = 0;
  b->cur = 0; b->nbits = 0;
}
static void bw_raw(BW *b, int bit)
{
  b->cur = (b->cur << 1) | (bit ? 1U : 0U);
  if (++b->nbits == 16) {
    if (b->bytepos + 2 <= b->cap) {
      b->out[b->bytepos++] = (unsigned char)(b->cur & 0xFF);
      b->out[b->bytepos++] = (unsigned char)((b->cur >> 8) & 0xFF);
    }
    b->cur = 0; b->nbits = 0;
  }
  b->pos++;
}
static void bw_bit(BW *b, int bit) { bw_raw(b, bit); }
static void bw_bits(BW *b, unsigned int v, int n)
{
  while (n-- > 0) bw_bit(b, (int)((v >> n) & 1U));
}
static void bw_align_word(BW *b) { while (b->nbits) bw_raw(b, 0); }

/* ---------------------------------------------------------------- tables */

static const int slot_count[11] = {
  30, 32, 34, 36, 38, 42, 50, 66, 98, 162, 290
};
static unsigned int pos_base[LZX_MAX_SLOTS];
static int bases_ready = 0;

static void make_bases(void)
{
  unsigned int base = 0;
  int i;
  if (bases_ready) return;
  for (i = 0; i < LZX_MAX_SLOTS; i++) {
    pos_base[i] = base;
    if (i < 4) base += 1;
    else if (i < 36) base += 1u << ((i >> 1) - 1);
    else base += 1u << 17;
  }
  bases_ready = 1;
}
static int slot_extra(int s)
{
  if (s < 4) return 0;
  if (s < 36) return (s >> 1) - 1;
  return 17;
}

/* Distance cost in raw extra bits, by offset (offsets under 128 KB; all
   longer offsets use 17 bits in the LZX position tables). */
static unsigned char g_dbits[1 << 17];
static int dbits_ready = 0;
static void make_dbits(void)
{
  int s;
  if (dbits_ready) return;
  memset(g_dbits, 17, sizeof(g_dbits));
  for (s = 3; s < LZX_MAX_SLOTS; s++) {
    unsigned long lo = pos_base[s] - 2;
    unsigned long hi = lo + (1UL << slot_extra(s));
    unsigned long i;
    int e = slot_extra(s);
    for (i = lo; i < hi && i < (1UL << 17); i++)
      g_dbits[i] = (unsigned char)e;
  }
  dbits_ready = 1;
}
static int dist_bits(unsigned long d)
{
  return d < (1UL << 17) ? (int)g_dbits[d] : 17;
}

/* --------------------------------------------------------- Huffman lengths */

typedef struct { int w, l, r, leaf; } HNODE;

/* Build a complete Huffman code from frequencies; 0-frequency symbols get
   length 0.  Returns 1 on success, 0 if a length exceeds maxbits. */
static int huffman_lengths(const unsigned long *freq, int n,
                           unsigned char *lens, int maxbits)
{
  HNODE node[2 * LZX_MAX_SYMS];
  int avail[LZX_MAX_SYMS], na = 0, next, i;
  int depth[LZX_MAX_SYMS], stack[2 * LZX_MAX_SYMS], top, root;

  for (i = 0; i < n; i++) {
    node[i].w = (int)freq[i];
    node[i].l = node[i].r = -1;
    node[i].leaf = i;
    lens[i] = 0;
    if (freq[i]) avail[na++] = i;
  }
  if (na == 0) return 1;
  if (na == 1) {
    /* a single symbol cannot form a complete tree; give a dummy symbol
       a frequency so both get one-bit codes (Kraft = 1). */
    int dummy = -1, j;
    for (j = 0; j < n; j++)
      if (j != avail[0] && !freq[j]) { dummy = j; break; }
    if (dummy < 0) return 0;
    node[dummy].w = 1;
    avail[na++] = dummy;
  }

  next = n;
  while (na > 1) {
    int a = 0, b = 1, k;
    if (node[avail[1]].w < node[avail[0]].w) { a = 1; b = 0; }
    for (k = 2; k < na; k++) {
      if (node[avail[k]].w < node[avail[a]].w) b = a, a = k;
      else if (node[avail[k]].w < node[avail[b]].w) b = k;
    }
    {
      int ia = avail[a], ib = avail[b];
      node[next].w = node[ia].w + node[ib].w;
      node[next].l = ia; node[next].r = ib; node[next].leaf = -1;
      /* remove a and b, add next */
      for (k = b; k < na - 1; k++) avail[k] = avail[k + 1];
      na--;
      if (a > b) a--;
      for (k = a; k < na - 1; k++) avail[k] = avail[k + 1];
      na--;
      avail[na++] = next;
      next++;
    }
  }
  root = avail[0];
  top = 0;
  stack[top++] = root;
  depth[root] = 0;
  while (top) {
    int nd = stack[--top];
    if (node[nd].leaf >= 0) {
      int leaf = node[nd].leaf;
      if (depth[nd] > maxbits) return 0;
      lens[leaf] = (unsigned char)depth[nd];
    }
    else {
      if (node[nd].l >= 0) {
        depth[node[nd].l] = depth[nd] + 1;
        stack[top++] = node[nd].l;
      }
      if (node[nd].r >= 0) {
        depth[node[nd].r] = depth[nd] + 1;
        stack[top++] = node[nd].r;
      }
    }
  }
  return 1;
}

/* Previous block's tree lengths, kept per folder as the cabinet LZX
   encoder does (WriteRepTree delta-codes against the previous tree). */
static unsigned char g_prev_main[LZX_MAX_SYMS];
static unsigned char g_prev_len[250];

/* repeat offsets also carry across blocks of a folder */
static unsigned int g_R0 = 1, g_R1 = 1, g_R2 = 1;
static int g_nslots = 30;       /* position slots for the current window */
static unsigned long g_e8filesize = 0;   /* 0 = no E8 translation */

/* Write canonical codes for a tree: for each length, symbols in index order.
   Delta values are relative to the previous block's tree lengths.  The
   pre-tree is Huffman-coded from the actual symbol frequencies, and runs
   of equal lengths are RLE-coded with symbols 17/18/19, as in cabinet.dll's
   WriteRepTree. */
static void write_tree(BW *b, const unsigned char *lens,
                       const unsigned char *prev_lens, int first, int last)
{
  int n = last - first;
  unsigned char pre[LZX_PRETREE];
  unsigned long freq[LZX_PRETREE];
  int counts[LZX_MAX_BITS + 1], firstcode[LZX_MAX_BITS + 2];
  int precode[LZX_PRETREE];
  int i, len, code;
  int ok;

  /* pass 1: count pre-tree symbol frequencies */
  memset(freq, 0, sizeof(freq));
  for (i = 0; i < n; ) {
    unsigned char v = lens[first + i];
    int run = 1;
    while (i + run < n && lens[first + i + run] == v) run++;
    if (run < 5) {
      freq[(prev_lens[first + i] - v + 17) % 17]++;
      i++;
    }
    else if (v != 0) {
      int r = run - 1;
      if (r > 5) r = 5;
      freq[(prev_lens[first + i] - v + 17) % 17]++;
      freq[19]++;
      i += r;
    }
    else {
      int r = run - 1;
      if (r > 51) r = 51;
      freq[r > 19 ? 18 : 17]++;
      i += r;
    }
  }

  /* pass 1b: choose the pre-tree; fall back to the fixed 4/5-bit code */
  ok = huffman_lengths(freq, LZX_PRETREE, pre, 15);
  if (!ok) {
    for (i = 0; i < LZX_PRETREE; i++)
      pre[i] = (unsigned char)(i < 12 ? 4 : 5);
  }
  for (i = 0; i < LZX_PRETREE; i++)
    bw_bits(b, pre[i], 4);

  memset(counts, 0, sizeof(counts));
  for (i = 0; i < LZX_PRETREE; i++) counts[pre[i]]++;
  code = 0;
  for (len = 1; len <= LZX_MAX_BITS; len++) {
    firstcode[len] = code;
    code = (code + counts[len]) << 1;
  }
  for (i = 0; i < LZX_PRETREE; i++) precode[i] = firstcode[pre[i]]++;

  /* pass 2: emit delta symbols and run markers */
  for (i = 0; i < n; ) {
    unsigned char v = lens[first + i];
    int run = 1, d;
    while (i + run < n && lens[first + i + run] == v) run++;
    d = (prev_lens[first + i] - v + 17) % 17;
    if (run < 5) {
      bw_bits(b, (unsigned int)precode[d], pre[d]);
      i++;
    }
    else if (v != 0) {
      int r = run - 1;
      if (r > 5) r = 5;
      bw_bits(b, (unsigned int)precode[19], pre[19]);
      bw_bits(b, (unsigned int)(r - 4), 1);
      bw_bits(b, (unsigned int)precode[d], pre[d]);
      i += r;
    }
    else {
      int sym;
      int r = run - 1;
      if (r > 51) r = 51;
      sym = r > 19 ? 18 : 17;
      bw_bits(b, (unsigned int)precode[sym], pre[sym]);
      if (sym == 18) bw_bits(b, (unsigned int)(r - 20), 5);
      else           bw_bits(b, (unsigned int)(r - 4), 4);
      i += r;
    }
  }
  /* this tree becomes the "previous" tree for the next block */
  memcpy((void *)(prev_lens + first), lens + first, (size_t)n);
}

/* ------------------------------------------------------------- token writer */

typedef struct {
  int lit;          /* 1 = literal, 0 = match */
  int sym;          /* literal byte (if lit) */
  int len;          /* match length */
  int slot;         /* position slot */
  int lenidx;       /* 0..7 */
  int footer;       /* length-tree symbol if lenidx == 7 */
  int msym;         /* main-tree symbol index */
  unsigned int extra; int nbits;   /* distance extra raw bits */
} LZTOK;

/* ------------------------------------------------------------- LZ matcher */

#define HSHIFT 15
#define HSIZE  (1 << HSHIFT)
#define HMASK  (HSIZE - 1)
/* LZX matches are 3..257 bytes (the length tree has 249 footers, and
   length = 9 + footer at most).  Returning 258 made every maximal match
   fail the length check and fall back to a literal. */
#define MAX_MATCH 257
#define SCORE_BYTE 3            /* large windows: favour cheap matches */
#define SCORE_BYTE_SMALL 4      /* small windows: distant matches cost more */
#define REP_BONUS 6             /* repeat offsets have the cheapest slots */
#define FOOTER_PENALTY 0        /* extra cost of a length-tree footer */
#define LAZY_LIMIT 64           /* only look ahead for short matches */
#define MATCH_CHAIN 2048
#define MATCH_BONUS 0           /* accept a match unless it is clearly worse */

static int m_head[HSIZE];
static int *m_prev;             /* chain link per absolute position & mask */
static unsigned int m_mask;

/* History of the current folder, so matches may reach back into earlier
   CFDATA blocks when the window is larger than one 32 KB frame. */
static unsigned char *g_hist;
static unsigned long g_hist_cap, g_hist_len, g_base;
static unsigned long g_folder_start;   /* absolute position of folder byte 0 */
static unsigned long g_ins;     /* next absolute position to hash-insert */
static unsigned int g_W;        /* window size in bytes */

static unsigned int mhash(const unsigned char *p)
{
  return (((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^ p[2]) & HMASK;
}

/* Keep buffers from the previous folder when the window did not change,
   but never let a match cross from one folder into another. */
static int matcher_reset(unsigned int window, unsigned long cap)
{
  int i;
  if (!m_prev || !g_hist || window != g_W || cap > g_hist_cap) {
    if (m_prev) { free(m_prev); m_prev = NULL; }
    if (g_hist) { free(g_hist); g_hist = NULL; }
    m_prev = (int *)malloc((size_t)window * sizeof(int));
    g_hist = (unsigned char *)malloc((size_t)cap);
    g_hist_cap = g_hist ? cap : 0;
  }
  g_W = window;
  m_mask = window - 1;
  for (i = 0; i < HSIZE; i++) m_head[i] = -1;
  g_hist_len = 0;
  g_base = 0;
  g_folder_start = 0;
  g_ins = 0;
  return m_prev != NULL && g_hist != NULL;
}

void lzx_enc_begin(int windowBits, unsigned long e8FileSize)
{
  unsigned int window;
  if (windowBits < 15 || windowBits > 21)
    windowBits = 15;
  g_e8filesize = e8FileSize;
  g_nslots = slot_count[windowBits - 15];
  window = 1u << windowBits;
  memset(g_prev_main, 0, sizeof(g_prev_main));
  memset(g_prev_len, 0, sizeof(g_prev_len));
  g_R0 = g_R1 = g_R2 = 1;
  matcher_reset(window, (unsigned long)window + 2 * 32768 + 512 * 1024);
}

void lzx_enc_end(void)
{
  if (m_prev) { free(m_prev); m_prev = NULL; }
  if (g_hist) { free(g_hist); g_hist = NULL; }
  g_hist_cap = g_hist_len = g_base = g_ins = 0;
  g_W = 0;
  g_e8filesize = 0;
}

/* One-time LZX stream header: bit 1 followed by the file size enables x86
   E8 call translation, which the decoder reverses. */
static void bw_stream_header(BW *b)
{
  if (!g_e8filesize) { bw_bit(b, 0); return; }
  bw_bit(b, 1);
  bw_bits(b, (unsigned int)((g_e8filesize >> 16) & 0xFFFF), 16);
  bw_bits(b, (unsigned int)(g_e8filesize & 0xFFFF), 16);
}

/* Inverse of lzxIntelUntranslate(): write the absolute target of each E8
   call so repeated calls to the same address encode identically. */
unsigned int lzx_translate_e8(unsigned char *buf, unsigned long n,
                              unsigned long filepos, unsigned long filesize)
{
  unsigned long i, limit, count = 0;
  long F, P;
  if (!filesize || n < 15) return 0;
  F = (long)filesize;
  limit = n - 10;
  for (i = 0; i < limit; ) {
    long rel, t, s;
    if (buf[i] != 0xE8) { i++; continue; }
    rel = (long)(signed int)((unsigned long)buf[i + 1] |
                             ((unsigned long)buf[i + 2] << 8) |
                             ((unsigned long)buf[i + 3] << 16) |
                             ((unsigned long)buf[i + 4] << 24));
    P = (long)(filepos + i);
    t = rel + P;
    if (t >= 0 && t < F)
      s = t;                  /* decoder: s - P = rel */
    else if (t >= F && t - F < P)
      s = rel - F;            /* decoder: s + F = rel */
    else
      s = rel;                /* decoder leaves it alone */
    buf[i + 1] = (unsigned char)s;
    buf[i + 2] = (unsigned char)(s >> 8);
    buf[i + 3] = (unsigned char)(s >> 16);
    buf[i + 4] = (unsigned char)(s >> 24);
    count++;
    i += 5;
  }
  return (unsigned int)count;
}

/* Append a frame to the history window, dropping the oldest bytes when
   the buffer is full.  Absolute positions stay valid across the move. */
static int hist_append(const unsigned char *in, unsigned long n)
{
  unsigned long need = g_hist_len + n;
  if (need > g_hist_cap) {
    unsigned long drop = need - g_hist_cap;
    if (drop > g_hist_len) return 0;
    memmove(g_hist, g_hist + drop, (size_t)(g_hist_len - drop));
    g_hist_len -= drop;
    g_base += drop;
    g_folder_start += drop;
  }
  if (n)
    memcpy(g_hist + g_hist_len, in, (size_t)n);
  g_hist_len += n;
  return 1;
}

static int matcher_insert(unsigned long p)
{
  unsigned int h;
  if (p + 2 >= g_base + g_hist_len) return 0;   /* need three bytes */
  h = mhash(g_hist + (p - g_base));
  m_prev[p & m_mask] = m_head[h];
  m_head[h] = (int)p;
  return 1;
}

/* Insert positions below `upto` whose hash bytes are now in history. */
static void matcher_sync(unsigned long upto)
{
  while (g_ins < upto) {
    if (!matcher_insert(g_ins)) break;
    g_ins++;
  }
}

static int find_match(unsigned long pos, unsigned long fend, int maxchain,
                      int *off, unsigned int R0, unsigned int R1,
                      unsigned int R2)
{
  int best = 0, boff = 0, chain, bestscore = 0;
  long cand;
  unsigned long maxl;
  if (pos + 2 >= fend) return 0;
  cand = m_head[mhash(g_hist + (pos - g_base))];
  maxl = MAX_MATCH;
  /* the CAB decoder emits output in 32768-byte frames; a match may not
     straddle one of those frame boundaries */
  {
    unsigned long rel = pos - g_folder_start;
    unsigned long to_edge = 32768UL - (rel & 32767UL);
    if (to_edge < maxl) maxl = to_edge;
  }
  if (pos + maxl > fend) maxl = fend - pos;
  /* A repeat offset may be too deep in a hash chain, or the chain may be
     truncated, so consider R0/R1/R2 explicitly as candidates. */
#define TRY_REP(DD) do {                                                      \
    unsigned long dd_ = (unsigned long)(DD);                                  \
    if (dd_ > 0 && dd_ <= g_W && dd_ <= pos && pos - dd_ >= g_base) {          \
      const unsigned char *a_ = g_hist + (pos - g_base);                      \
      const unsigned char *b_ = g_hist + (pos - dd_ - g_base);                \
      unsigned long l_ = 0;                                                   \
      int sc_;                                                                \
      while (l_ < maxl && a_[l_] == b_[l_]) l_++;                             \
      if (l_ >= 3) {                                                          \
        sc_ = (int)l_ * (g_nslots >= 34 ? SCORE_BYTE : SCORE_BYTE_SMALL)       \
              + MATCH_BONUS + REP_BONUS - dist_bits(dd_);                     \
        if (l_ >= 9) sc_ -= FOOTER_PENALTY;                                   \
        if (sc_ > bestscore) {                                                \
          bestscore = sc_; best = (int)l_; boff = (int)dd_;                   \
        }                                                                     \
      }                                                                       \
    }                                                                         \
  } while (0)
  TRY_REP(R0);
  TRY_REP(R1);
  TRY_REP(R2);
#undef TRY_REP
  for (chain = 0; cand >= 0 && chain < maxchain; chain++) {
    unsigned long d = pos - (unsigned long)cand;
    if ((long)d <= 0 || d > g_W) break;
    {
      const unsigned char *a = g_hist + (pos - g_base);
      const unsigned char *b = g_hist + ((unsigned long)cand - g_base);
      unsigned long l = 0;
      int score;
      while (l < maxl && a[l] == b[l]) l++;
      /* Prefer the match that saves the most literal bits after paying
         for its distance, and give repeat offsets a small bonus because
         their slot codes are the most frequent. */
      score = (int)l * (g_nslots >= 34 ? SCORE_BYTE : SCORE_BYTE_SMALL)
              + MATCH_BONUS - dist_bits(d);
      if (l >= 9) score -= FOOTER_PENALTY;
      if (d == R0 || d == R1 || d == R2) score += REP_BONUS;
      if (score > bestscore) {
        bestscore = score; best = (int)l; boff = (int)d;
        if (l >= maxl) break;
      }
    }
    cand = m_prev[(unsigned long)cand & m_mask];
  }
  if (best >= 3) { *off = boff; return best; }
  return 0;
}

/* ----------------------------------------------------------- frame encoder */

unsigned int lzx_write_uncompressed(const unsigned char *in, unsigned int n,
                                    unsigned char *out, unsigned int cap,
                                    int first_frame)
{
  BW b;
  unsigned int k;
  bw_init(&b, out, cap);
  if (first_frame) bw_stream_header(&b);
  bw_bits(&b, 3u, 3);              /* BLOCK_UNCOMPRESSED */
  bw_bits(&b, n, 24);
  for (k = 0; k < 4; k++) bw_bits(&b, 0xAAu, 8);   /* R0 marker */
  for (k = 4; k < 12; k++) bw_bits(&b, 0u, 8);
  bw_align_word(&b);
  /* raw block payload is stored as ordinary bytes, not through the
     16-bit word bit writer */
  if (b.bytepos + n > cap) return 0;
  if (n) memcpy(out + b.bytepos, in, n);
  b.bytepos += n;
  if (getenv("LZX_DEBUG")) fprintf(stderr, "ENC uncompressed n=%u cap=%u ret=%u\n", n, cap, b.bytepos);
  return b.bytepos;
}

/* Emit one uncompressed block per nominal frame, with CFDATA splits at
   each block end.  Used both as a Huffman fallback and when a verbatim
   block would be larger than the raw data. */
static unsigned int write_uncompressed_group(const unsigned char *in,
                          unsigned int in_n, unsigned char *out,
                          unsigned int cap, int first_frame,
                          unsigned int frame_size, LZX_SPLIT *splits,
                          int maxsplits, int *nsplits)
{
  unsigned long src = 0;
  unsigned int dst = 0;
  int f = first_frame, ns = 0;
  while (src < in_n) {
    unsigned int flen = in_n - (unsigned int)src;
    unsigned int c;
    if (flen > frame_size) flen = frame_size;
    if (cap <= dst) return 0;
    c = lzx_write_uncompressed(in + src, flen, out + dst, cap - dst, f);
    if (!c) return 0;
    dst += c; src += flen; f = 0;
    if (ns < maxsplits) {
      splits[ns].comp_off = dst;
      splits[ns].uncomp = src;
      ns++;
    }
  }
  /* the decoder loads these from the uncompressed block header */
  g_R0 = 0xAA; g_R1 = 0; g_R2 = 0;
  *nsplits = ns;
  return dst;
}

unsigned int lzx_compress_frames(const unsigned char *in, unsigned int in_n,
                                 unsigned char *out, unsigned int cap,
                                 int first_frame, unsigned int frame_size,
                                 LZX_SPLIT *splits, int maxsplits,
                                 int *nsplits)
{
  BW b;
  LZTOK *tok;
  unsigned long *mainfreq, *lenfreq;
  unsigned char *mainlens, *lenlens;
  int ntok = 0, i, pos = 0, nslots;
  unsigned int R0 = g_R0, R1 = g_R1, R2 = g_R2;
  unsigned long fstart, fend, matched = 0;
  int maxsyms, ok = 1;
  unsigned long target, last_unc = 0, aligned_unc = 0, upos = 0;
  unsigned int aligned_off = 0;
  int nsplit = 0;

  *nsplits = 0;
  if (frame_size == 0) frame_size = 32768;
  target = frame_size;

  make_bases();
  make_dbits();
  nslots = g_nslots;                /* window size selected by lzx_enc_begin */
  maxsyms = LZX_NUM_CHARS + nslots * 8;
  if (maxsyms > LZX_MAX_SYMS) maxsyms = LZX_MAX_SYMS;

  tok = (LZTOK *)malloc((size_t)in_n * sizeof(LZTOK) + 1);
  mainfreq = (unsigned long *)calloc((size_t)maxsyms, sizeof(unsigned long));
  lenfreq = (unsigned long *)calloc(250, sizeof(unsigned long));
  mainlens = (unsigned char *)malloc((size_t)maxsyms);
  lenlens = (unsigned char *)malloc(250);
  if (!tok || !mainfreq || !lenfreq || !mainlens || !lenlens) {
    ok = 0;
    goto done;
  }

  if (!m_prev || !g_hist) {
    ok = 0;
    goto done;
  }
  if (!hist_append(in, in_n)) {
    ok = 0;
    goto done;
  }
  fstart = g_base + g_hist_len - in_n;
  fend = g_base + g_hist_len;
  matcher_sync(fstart);

  while (pos < (int)in_n) {
    unsigned long P = fstart + (unsigned long)pos;
    int off = 0, len = find_match(P, fend, MATCH_CHAIN, &off, R0, R1, R2);
    LZTOK *t = &tok[ntok++];

    /* lazy matching: prefer a literal when the next position starts a
       clearly longer match */
    if (len >= 3 && len < LAZY_LIMIT && pos + 1 < (int)in_n) {
      int off1 = 0, len1;
      matcher_insert(P);
      g_ins = P + 1;
      len1 = find_match(P + 1, fend, MATCH_CHAIN, &off1, R0, R1, R2);
      if (len1 > len + 1) len = 0;
    }

    memset(t, 0, sizeof(*t));
    if (len >= 3) {
      int slot, primary;
      t->lit = 0;
      t->len = len;
      if (off == (int)R0) slot = 0;
      else if (off == (int)R1) slot = 1;
      else if (off == (int)R2) slot = 2;
      else {
        int s;
        for (s = 3; s < nslots; s++) {
          int e = slot_extra(s);
          unsigned int lo = pos_base[s] - 2;
          if ((unsigned)off >= lo && (unsigned)off < lo + (1u << e)) break;
        }
        if (s >= nslots) { t->lit = 1; t->sym = in[pos]; }
        else {
          slot = s;
          t->extra = (unsigned int)(off - (int)(pos_base[s] - 2));
          t->nbits = slot_extra(s);
        }
      }
      if (!t->lit) {
        primary = len - 2;
        if (primary < 7) t->lenidx = primary;
        else {
          t->lenidx = 7;
          t->footer = primary - 7;
          if (t->footer >= 249) t->lit = 1, t->sym = in[pos];
        }
      }
      if (!t->lit) {
        int ms = LZX_NUM_CHARS + slot * 8 + t->lenidx;
        if (ms >= maxsyms) t->lit = 1, t->sym = in[pos];
        else {
          t->msym = ms;
          mainfreq[ms]++;
          if (t->lenidx == 7) lenfreq[t->footer]++;
        }
      }
      if (!t->lit) {
        /* update repeat offsets like the decoder */
        if (slot == 1) { unsigned int t = R0; R0 = R1; R1 = t; }
        else if (slot == 2) { unsigned int t = R0; R0 = R2; R2 = t; }
        else if (slot >= 3) { R2 = R1; R1 = R0; R0 = off; }
        matched += (unsigned long)len;
        pos += len;
        matcher_sync(P + (unsigned long)len);
        continue;
      }
    }
    /* literal */
    t->lit = 1;
    t->sym = in[pos];
    mainfreq[t->sym]++;
    pos++;
    matcher_sync(P + 1);
  }
  if (getenv("LZX_DEBUG"))
    fprintf(stderr, "ENC in=%u tokens=%d matched=%lu\n", in_n, ntok, matched);

  if (!huffman_lengths(mainfreq, maxsyms, mainlens, LZX_MAX_BITS) ||
      !huffman_lengths(lenfreq, 250, lenlens, LZX_MAX_BITS)) {
    unsigned int alt;
    free(tok); free(mainfreq); free(lenfreq); free(mainlens); free(lenlens);
    alt = write_uncompressed_group(in, in_n, out, cap, first_frame,
                                   frame_size, splits, maxsplits, nsplits);
    return alt;
  }

  /* keep the trees the decoder actually knows, in case we decide to write
     uncompressed blocks instead of this verbatim block */
  {
    unsigned char save_main[LZX_MAX_SYMS];
    unsigned char save_len[250];
    memcpy(save_main, g_prev_main, sizeof(save_main));
    memcpy(save_len, g_prev_len, sizeof(save_len));

  bw_init(&b, out, cap);
  if (first_frame) bw_stream_header(&b);
  bw_bits(&b, 1u, 3);               /* BLOCK_VERBATIM */
  bw_bits(&b, in_n, 24);
  write_tree(&b, mainlens, g_prev_main, 0, LZX_NUM_CHARS);
  if (getenv("LZX_DEBUG")) fprintf(stderr, "ENC tree2 from %u\n", b.bytepos);
  write_tree(&b, mainlens, g_prev_main, LZX_NUM_CHARS, maxsyms);
  if (getenv("LZX_DEBUG")) fprintf(stderr, "ENC tree3 from %u\n", b.bytepos);
  write_tree(&b, lenlens, g_prev_len, 0, 249);
  if (getenv("LZX_DEBUG")) fprintf(stderr, "ENC data from %u\n", b.bytepos);

  /* canonical codes for the main tree */
  {
    int counts[LZX_MAX_BITS + 2], firstcode[LZX_MAX_BITS + 2];
    int nextcode[LZX_MAX_BITS + 2], code = 0, len, s;
    memset(counts, 0, sizeof(counts));
    for (s = 0; s < maxsyms; s++) counts[mainlens[s]]++;
    for (len = 1; len <= LZX_MAX_BITS; len++) {
      firstcode[len] = code;
      code = (code + counts[len]) << 1;
    }
    for (len = 0; len <= LZX_MAX_BITS; len++) nextcode[len] = firstcode[len];
    {
      int *mcodes = (int *)malloc((size_t)maxsyms * sizeof(int));
      if (!mcodes) { ok = 0; goto done; }
      for (s = 0; s < maxsyms; s++)
        mcodes[s] = mainlens[s] ? nextcode[mainlens[s]]++ : 0;

      for (i = 0; i < ntok; i++) {
        LZTOK *t = &tok[i];
        if (t->lit) {
          int l = mainlens[t->sym];
          bw_bits(&b, (unsigned int)mcodes[t->sym], l);
        }
        else {
          int symidx = t->msym;
          int l = mainlens[symidx];
          bw_bits(&b, (unsigned int)mcodes[symidx], l);
          /* the decoder reads the length-tree footer (when the length
             primary is 7) BEFORE the raw distance extra bits */
          if (t->lenidx == 7) {
            int fl = lenlens[t->footer];
            /* length-tree canonical codes */
            int lcounts[LZX_MAX_BITS + 2], lfirst[LZX_MAX_BITS + 2];
            int lnext[LZX_MAX_BITS + 2], lc = 0, ll, q;
            memset(lcounts, 0, sizeof(lcounts));
            for (q = 0; q < 250; q++) lcounts[lenlens[q]]++;
            for (ll = 1; ll <= LZX_MAX_BITS; ll++) {
              lfirst[ll] = lc;
              lc = (lc + lcounts[ll]) << 1;
            }
            for (ll = 0; ll <= LZX_MAX_BITS; ll++) lnext[ll] = lfirst[ll];
            for (q = 0; q < t->footer; q++)
              if (lenlens[q]) lnext[lenlens[q]]++;
            bw_bits(&b, (unsigned int)lnext[fl], fl);
          }
          if (t->nbits) bw_bits(&b, t->extra, t->nbits);
        }
        /* CFDATA chunks must start on a 16-bit word boundary of the LZX
           stream, so only record splits at word-aligned token ends. */
        upos += (unsigned long)(t->lit ? 1 : t->len);
        /* CabLzx pads to a 16-bit boundary at the end of every 32768-byte
           output frame, even when several frames share one LZX block. */
        if ((upos & 32767UL) == 0UL && upos < in_n)
          bw_align_word(&b);
        if (b.nbits == 0 && upos > last_unc) {
          if (upos >= target) {
            unsigned long coff, cunc;
            if (upos == target) { coff = b.bytepos; cunc = upos; }
            else if (aligned_unc > last_unc) {
              coff = aligned_off; cunc = aligned_unc;
            }
            else { coff = b.bytepos; cunc = upos; }
            if (cunc > last_unc && nsplit < maxsplits) {
              splits[nsplit].comp_off = coff;
              splits[nsplit].uncomp = cunc;
              nsplit++;
              last_unc = cunc;
              target = last_unc + frame_size;
            }
          }
          aligned_off = b.bytepos;
          aligned_unc = upos;
        }
      }
      free(mcodes);
    }
  }
  bw_align_word(&b);
  if (in_n > last_unc && nsplit < maxsplits) {
    splits[nsplit].comp_off = b.bytepos;
    splits[nsplit].uncomp = in_n;
    nsplit++;
    last_unc = in_n;
  }
  *nsplits = nsplit;
  {
    unsigned int ret = b.bytepos;
    unsigned long frames = (unsigned long)(in_n + frame_size - 1) / frame_size;
    unsigned int alt;
    {
      unsigned long thr = (unsigned long)in_n + 16UL * frames
                          + (g_e8filesize ? 4UL : 0UL);
      if (frames && ret >= thr) {
      /* the original LZX encoder also picks an uncompressed block when the
         verbatim estimate is no better than the raw bytes */
      memcpy(g_prev_main, save_main, sizeof(save_main));
      memcpy(g_prev_len, save_len, sizeof(save_len));
      alt = write_uncompressed_group(in, in_n, out, cap, first_frame,
                                     frame_size, splits, maxsplits, nsplits);
      if (alt) {
        free(tok); free(mainfreq); free(lenfreq); free(mainlens); free(lenlens);
        return alt;
      }
      }
    }
    g_R0 = R0; g_R1 = R1; g_R2 = R2;
    free(tok); free(mainfreq); free(lenfreq); free(mainlens); free(lenlens);
    return ret;
  }
  }

done:
  if (getenv("LZX_DEBUG")) fprintf(stderr, "ENC done ok=%d bytepos=%u\n", ok, b.bytepos);
  if (tok) free(tok);
  if (mainfreq) free(mainfreq);
  if (lenfreq) free(lenfreq);
  if (mainlens) free(mainlens);
  if (lenlens) free(lenlens);
  if (!ok) return 0;
  return b.bytepos;
}

unsigned int lzx_compress_frame(const unsigned char *in, unsigned int in_n,
                                unsigned char *out, unsigned int cap,
                                int first_frame)
{
  LZX_SPLIT sp[2];
  int ns;
  return lzx_compress_frames(in, in_n, out, cap, first_frame,
                             in_n ? in_n : 1, sp, 2, &ns);
}
