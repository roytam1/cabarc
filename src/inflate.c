/*
 * Raw DEFLATE decoder (RFC 1951) -- stored, fixed-Huffman and
 * dynamic-Huffman blocks.  Written for the standalone cabarc
 * reconstruction; no external zlib dependency.
 *
 * C89 / MSVC6 compatible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "inflate.h"

#define INF_MAXBITS 15
#define INF_NSYM    288
#define INF_NDIST   32
#define INF_WINSIZE 32768U

typedef struct {
  const unsigned char *in;
  unsigned long inlen, inpos;
  unsigned int bitbuf;
  int bitcnt;
  unsigned char *out;
  unsigned long outlen, outpos;
  unsigned char window[INF_WINSIZE];
  unsigned int wpos;
  int eof;
} INF;

static unsigned int inf_bits(INF *s, int n)
{
  while (s->bitcnt < n) {
    if (s->inpos >= s->inlen) { s->eof = 1; return 0; }
    s->bitbuf |= (unsigned int)s->in[s->inpos++] << s->bitcnt;
    s->bitcnt += 8;
  }
  {
    unsigned int v = s->bitbuf & ((1U << n) - 1U);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
  }
}

static int inf_byte(INF *s)
{
  if (s->inpos >= s->inlen) { s->eof = 1; return 0; }
  return s->in[s->inpos++];
}

/* Build a Huffman decode table from an array of code lengths.
   Returns 0 on success, -1 on oversubscribed/incomplete code. */
static int inf_build(const unsigned char *len, int n,
                     unsigned short *counts, unsigned short *symbols)
{
  int i, bits, left;
  int next[INF_MAXBITS + 2];

  memset(counts, 0, (INF_MAXBITS + 1) * sizeof(unsigned short));
  for (i = 0; i < n; i++)
    if (len[i] > INF_MAXBITS) return -1;
  for (i = 0; i < n; i++)
    counts[len[i]]++;
  counts[0] = 0;
  left = 1;
  for (bits = 1; bits <= INF_MAXBITS; bits++) {
    left = (left << 1) - counts[bits];
    if (left < 0) return -1;
    next[bits] = 0;
  }
  {
    int offs[INF_MAXBITS + 2];
    offs[1] = 0;
    for (bits = 1; bits < INF_MAXBITS; bits++)
      offs[bits + 1] = offs[bits] + counts[bits];
    for (i = 0; i < n; i++) {
      int l = len[i];
      if (l)
        symbols[offs[l] + next[l]++] = (unsigned short)i;
    }
  }
  return 0;
}

static int inf_decode(INF *s, const unsigned short *counts,
                      const unsigned short *symbols)
{
  int code = 0, first = 0, index = 0, bits, count;
  for (bits = 1; bits <= INF_MAXBITS; bits++) {
    code |= (int)inf_bits(s, 1);
    count = counts[bits];
    if (code - first < count)
      return symbols[index + code - first];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

static const unsigned short len_base[29] = {
  3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const unsigned char len_extra[29] = {
  0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const unsigned short dist_base[30] = {
  1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
  1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const unsigned char dist_extra[30] = {
  0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static int inf_put(INF *s, unsigned char b)
{
  if (s->outpos >= s->outlen) return -1;
  s->out[s->outpos++] = b;
  s->window[s->wpos] = b;
  s->wpos = (s->wpos + 1) & (INF_WINSIZE - 1);
  return 0;
}

static int inf_copy(INF *s, unsigned int dist, unsigned int len)
{
  unsigned int i;
  if (dist == 0 || dist > INF_WINSIZE || dist > s->wpos) return -1;
  for (i = 0; i < len; i++) {
    unsigned int p = (s->wpos - dist) & (INF_WINSIZE - 1);
    unsigned char b = s->window[p];
    if (inf_put(s, b) < 0) return -1;
  }
  return 0;
}

static int inf_codes(INF *s, const unsigned short *lc,
                     const unsigned short *ls,
                     const unsigned short *dc,
                     const unsigned short *ds)
{
  for (;;) {
    int sym = inf_decode(s, lc, ls);
    if (sym < 0 || s->eof) return -1;
    if (sym < 256) {
      if (inf_put(s, (unsigned char)sym) < 0) return -1;
    }
    else if (sym == 256) {
      return 0;
    }
    else {
      unsigned int len, dist;
      int dsym;
      sym -= 257;
      if (sym >= 29) return -1;
      len = len_base[sym] + inf_bits(s, len_extra[sym]);
      dsym = inf_decode(s, dc, ds);
      if (dsym < 0 || dsym >= 30) return -1;
      dist = dist_base[dsym] + inf_bits(s, dist_extra[dsym]);
      if (inf_copy(s, dist, len) < 0) return -1;
    }
  }
}

static int inf_dynamic(INF *s)
{
  static const unsigned char order[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
  };
  unsigned char clen[19], llen[INF_NSYM], dlen[INF_NDIST];
  unsigned short ccount[INF_MAXBITS + 1], csym[19];
  unsigned short lcount[INF_MAXBITS + 1], lsym[INF_NSYM];
  unsigned short dcount[INF_MAXBITS + 1], dsym[INF_NDIST];
  int nlen, ndist, ncode, i, total;

  nlen = (int)inf_bits(s, 5) + 257;
  ndist = (int)inf_bits(s, 5) + 1;
  ncode = (int)inf_bits(s, 4) + 4;
  if (nlen > INF_NSYM || ndist > INF_NDIST)
    return -1;
  memset(clen, 0, sizeof(clen));
  for (i = 0; i < ncode; i++)
    clen[order[i]] = (unsigned char)inf_bits(s, 3);
  if (inf_build(clen, 19, ccount, csym) < 0) { if(1)fprintf(stderr,"INF bad clen tree\n"); return -1; }

  memset(llen, 0, sizeof(llen));
  memset(dlen, 0, sizeof(dlen));
  total = nlen + ndist;
  i = 0;
  while (i < total) {
    int sym = inf_decode(s, ccount, csym);
    int n, v;
    unsigned char *dest;
    if (sym < 0 || s->eof) return -1;
    if (sym < 16) {
      if (i >= total) return -1;
      dest = (i < nlen) ? llen + i : dlen + (i - nlen);
      *dest = (unsigned char)sym;
      i++;
    }
    else if (sym == 16) {
      if (i == 0) return -1;
      v = (i < nlen) ? llen[i - 1] : dlen[i - nlen - 1];
      n = (int)inf_bits(s, 2) + 3;
      if (i + n > total) return -1;
      while (n--) {
        dest = (i < nlen) ? llen + i : dlen + (i - nlen);
        *dest = (unsigned char)v;
        i++;
      }
    }
    else if (sym == 17) {
      n = (int)inf_bits(s, 3) + 3;
      if (i + n > total) return -1;
      i += n;
    }
    else {
      n = (int)inf_bits(s, 7) + 11;
      if (i + n > total) return -1;
      i += n;
    }
  }
  if (llen[256] == 0) return -1;
  if (inf_build(llen, nlen, lcount, lsym) < 0) { if(1)fprintf(stderr,"INF bad lit tree\n"); return -1; }
  if (inf_build(dlen, ndist, dcount, dsym) < 0) { if(1)fprintf(stderr,"INF bad dist tree\n"); return -1; }
  return inf_codes(s, lcount, lsym, dcount, dsym);
}

static int inf_fixed(INF *s)
{
  unsigned char llen[INF_NSYM], dlen[INF_NDIST];
  unsigned short lcount[INF_MAXBITS + 1], lsym[INF_NSYM];
  unsigned short dcount[INF_MAXBITS + 1], dsym[INF_NDIST];
  int i;
  for (i = 0; i < 144; i++) llen[i] = 8;
  for (i = 144; i < 256; i++) llen[i] = 9;
  for (i = 256; i < 280; i++) llen[i] = 7;
  for (i = 280; i < INF_NSYM; i++) llen[i] = 8;
  for (i = 0; i < INF_NDIST; i++) dlen[i] = 5;
  if (inf_build(llen, INF_NSYM, lcount, lsym) < 0) { fprintf(stderr,"fixed lit build fail\n"); return -1; }
  if (inf_build(dlen, INF_NDIST, dcount, dsym) < 0) { fprintf(stderr,"fixed dist build fail\n"); return -1; }
  {
    int rr = inf_codes(s, lcount, lsym, dcount, dsym);
    fprintf(stderr, "fixed codes rr=%d pos=%lu\n", rr, s->inpos);
    return rr;
  }
}

unsigned long inflate_raw(const unsigned char *in, unsigned long in_bytes,
                          unsigned char *out, unsigned long out_bytes)
{
  INF s;
  int last, type, ok = 1;

    memset(&s, 0, sizeof(s));
  s.in = in;
  s.inlen = in_bytes;
  s.out = out;
  s.outlen = out_bytes;

  do {
    last = (int)inf_bits(&s, 1);
    type = (int)inf_bits(&s, 2);
        if (s.eof) { ok = 0; break; }
    if (type == 0) {
      unsigned int len, nlen;
      s.bitbuf = 0; s.bitcnt = 0;
      len = (unsigned int)inf_byte(&s) | ((unsigned int)inf_byte(&s) << 8);
      nlen = (unsigned int)inf_byte(&s) | ((unsigned int)inf_byte(&s) << 8);
      if ((len ^ 0xFFFFU) != nlen) { ok = 0; break; }
      while (len--) {
        if (inf_put(&s, (unsigned char)inf_byte(&s)) < 0) { ok = 0; break; }
      }
    }
    else if (type == 1) {
      if (inf_fixed(&s) < 0) { if(1)fprintf(stderr,"INF fixed fail\n"); ok = 0; break; }
    }
    else if (type == 2) {
      if (inf_dynamic(&s) < 0) { if(1)fprintf(stderr,"INF dynamic fail\n"); ok = 0; break; }
    }
    else { ok = 0; break; }
    if (s.eof && !last) { /* MSZIP blocks are self-terminating */ break; }
  } while (!last && !s.eof);

  return ok ? s.outpos : 0;
}
