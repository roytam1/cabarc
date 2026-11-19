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

/* Write canonical codes for a tree: for each length, symbols in index order. */
static void write_tree(BW *b, const unsigned char *lens, int first, int last)
{
  int n = last - first;
  unsigned char pre[LZX_PRETREE];
  int counts[LZX_MAX_BITS + 1], firstcode[LZX_MAX_BITS + 2];
  int precode[LZX_PRETREE];
  int i, len, x, prev = 0, code = 0;
  unsigned int startpos;

  startpos = b->pos;
  /* pre-tree: 12 codes of length 4, 8 codes of length 5 (Kraft = 1) */
  for (i = 0; i < LZX_PRETREE; i++)
    pre[i] = (unsigned char)(i < 12 ? 4 : 5);
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

  /* delta-code the real tree lengths through the pre-tree */
  for (x = first; x < last; x++) {
    int L = lens[x];
    /* The decoder computes new_lens[x] = old_lens[x] - symbol (mod 17).
       For a fresh block old_lens[x] is zero for every index. */
    int sym = (17 - L) % 17;
    int l = pre[sym];
    unsigned int c = (unsigned int)precode[sym];
    if (getenv("LZX_DEBUG") && x < first + 4)
      fprintf(stderr, "ENC delta x=%d L=%d sym=%d l=%d code=%u\n", x, L, sym, l, c);
    bw_bits(b, c, l);
  }
  (void)prev;
  (void)startpos;
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
static int m_head[HSIZE];
static int m_prev[32768];

static unsigned int mhash(const unsigned char *p)
{
  return (((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^ p[2]) & HMASK;
}
static void matcher_init(void)
{
  int i;
  for (i = 0; i < HSIZE; i++) m_head[i] = -1;
}
static void matcher_insert(const unsigned char *in, int pos, int n)
{
  unsigned int h;
  if (pos + 2 >= n) return;
  h = mhash(in + pos);
  m_prev[pos] = m_head[h];
  m_head[h] = pos;
}
static int find_match(const unsigned char *in, int pos, int n, int maxchain,
                      int *off)
{
  unsigned int h;
  int cand, best = 0, boff = 0, chain, lim;
  if (pos + 2 >= n) return 0;
  h = mhash(in + pos);
  cand = m_head[h];
  lim = pos > 32768 ? pos - 32768 : 0;
  for (chain = 0; cand >= 0 && chain < maxchain; chain++) {
    int l = 0, d = pos - cand;
    const unsigned char *a = in + pos, *b = in + cand;
    if (d <= 0 || d > 32768 || cand < lim) break;
    while (l < 258 && pos + l < n && a[l] == b[l]) l++;
    if (l > best) {
      best = l; boff = d;
      if (l >= 258) break;
    }
    cand = m_prev[cand];
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
  if (first_frame) bw_bit(&b, 0);
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

unsigned int lzx_compress_frame(const unsigned char *in, unsigned int in_n,
                                unsigned char *out, unsigned int cap,
                                int first_frame)
{
  BW b;
  LZTOK *tok;
  unsigned int n = 0;
  unsigned long *mainfreq, *lenfreq;
  unsigned char *mainlens, *lenlens;
  int ntok = 0, i, pos = 0, nslots;
  unsigned int R0 = 1, R1 = 1, R2 = 1;
  int maxsyms, ok = 1;

  make_bases();
  nslots = slot_count[0];           /* window 32 KB */
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

  matcher_init();
  while (pos < (int)in_n) {
    int off = 0, len = find_match(in, pos, in_n, 64, &off);
    LZTOK *t = &tok[ntok++];
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
        while (len-- > 0) { matcher_insert(in, pos, in_n); pos++; }
        continue;
      }
    }
    /* literal */
    t->lit = 1;
    t->sym = in[pos];
    mainfreq[t->sym]++;
    matcher_insert(in, pos, in_n);
    pos++;
  }

  if (!huffman_lengths(mainfreq, maxsyms, mainlens, LZX_MAX_BITS) ||
      !huffman_lengths(lenfreq, 250, lenlens, LZX_MAX_BITS)) {
    n = lzx_write_uncompressed(in, in_n, out, cap, first_frame);
    goto done2;
  }

  bw_init(&b, out, cap);
  if (first_frame) bw_bit(&b, 0);
  bw_bits(&b, 1u, 3);               /* BLOCK_VERBATIM */
  bw_bits(&b, in_n, 24);
  write_tree(&b, mainlens, 0, LZX_NUM_CHARS);
  if (getenv("LZX_DEBUG")) fprintf(stderr, "ENC tree2 from %u\n", b.bytepos);
  write_tree(&b, mainlens, LZX_NUM_CHARS, maxsyms);
  if (getenv("LZX_DEBUG")) fprintf(stderr, "ENC tree3 from %u\n", b.bytepos);
  write_tree(&b, lenlens, 0, 249);
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
          if (t->nbits) bw_bits(&b, t->extra, t->nbits);
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
        }
      }
      free(mcodes);
    }
  }
  bw_align_word(&b);
  {
    unsigned int ret = b.bytepos;
    free(tok); free(mainfreq); free(lenfreq); free(mainlens); free(lenlens);
    return ret;
  }

done2:
  if (getenv("LZX_DEBUG")) fprintf(stderr, "ENC done2 n=%u\n", n);
  if (tok) free(tok);
  if (mainfreq) free(mainfreq);
  if (lenfreq) free(lenfreq);
  if (mainlens) free(mainlens);
  if (lenlens) free(lenlens);
  return n;
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
