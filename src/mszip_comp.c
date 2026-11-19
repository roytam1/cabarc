/*
 * MSZIP (raw DEFLATE dynamic-Huffman) compressor, extracted from
 * mscompress-expand/compress.c.  C89 / MSVC6 compatible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lzencode.h"
#define C_MSZIP_BLOCK 32768

/* ------------------------------------------------------------------ */
/* bit writer                                                          */
typedef struct
{
    unsigned char *buf;
    unsigned long len, cap;
    unsigned int acc;
    int nbits;
    int msb;                   /* 1 = pack bits into byte from bit 7 down */
} BitWriter;

static int BwInit(BitWriter *bw)
{
    bw->cap = 4096;
    bw->len = 0;
    bw->acc = 0;
    bw->nbits = 0;
    bw->msb = 0;
    bw->buf = (unsigned char *)malloc((size_t)bw->cap);
    return bw->buf != NULL;
}

static int BwByte(BitWriter *bw, unsigned char c)
{
    if (bw->len >= bw->cap)
    {
        unsigned char *tmp;
        bw->cap *= 2;
        tmp = (unsigned char *)realloc(bw->buf, (size_t)bw->cap);
        if (!tmp) return -1;
        bw->buf = tmp;
    }
    bw->buf[bw->len++] = c;
    return 0;
}

static int BwBit(BitWriter *bw, int bit)
{
    bw->acc |= (unsigned int)(bit & 1) << (bw->msb ? (7 - bw->nbits)
                                                    : bw->nbits);
    bw->nbits++;
    if (bw->nbits == 8)
    {
        if (BwByte(bw, (unsigned char)bw->acc)) return -1;
        bw->acc = 0;
        bw->nbits = 0;
    }
    return 0;
}

/* append n low bits of v, least significant bit first */
static int BwLsb(BitWriter *bw, unsigned int v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (BwBit(bw, (int)((v >> i) & 1u))) return -1;
    return 0;
}

/* append an n-bit Huffman code, most significant bit first */
static int BwCode(BitWriter *bw, unsigned int code, int n)
{
    int i;
    for (i = n - 1; i >= 0; i--)
        if (BwBit(bw, (int)((code >> i) & 1u))) return -1;
    return 0;
}

static int BwFlush(BitWriter *bw)
{
    if (bw->nbits)
        if (BwByte(bw, (unsigned char)bw->acc)) return -1;
    bw->acc = 0;
    bw->nbits = 0;
    return 0;
}

static void BwFree(BitWriter *bw)
{
    free(bw->buf);
    bw->buf = NULL;
}

/* Shannon-Fano split: order[] holds symbols sorted by descending
 * frequency.  It yields a valid prefix code with depth <= log2(n). */
static void SfSplit(int *order, unsigned long *freq,
                    int lo, int hi, int depth, unsigned char *lengths)
{
    int i, split;
    unsigned long total = 0, acc = 0, half;

    if (lo > hi) return;
    if (lo == hi)
    {
        lengths[order[lo]] = (unsigned char)(depth ? depth : 1);
        return;
    }
    for (i = lo; i <= hi; i++)
        total += freq[order[i]];
    half = total / 2;
    split = lo;
    for (i = lo; i < hi; i++)
    {
        acc += freq[order[i]];
        if (acc >= half) { split = i; break; }
    }
    if (split >= hi) split = hi - 1;
    SfSplit(order, freq, lo, split, depth + 1, lengths);
    SfSplit(order, freq, split + 1, hi, depth + 1, lengths);
}

static void BuildCodes(unsigned char *lengths, int n, unsigned int *codes)
{
    int count[17] = {0}, next[17], i, l, code = 0;

    for (i = 0; i < n; i++)
        count[lengths[i]]++;
    for (l = 1; l <= 16; l++)
    {
        next[l] = code;
        code = (code + count[l]) << 1;
    }
    for (i = 0; i < n; i++)
        if (lengths[i])
            codes[i] = (unsigned int)next[lengths[i]]++;
}

static void BuildTree(unsigned long *freq, int n,
                      unsigned char *lengths, unsigned int *codes)
{
    int order[288], i, j;

    for (i = 0; i < n; i++) { order[i] = i; lengths[i] = 0; }
    /* insertion sort by descending frequency */
    for (i = 1; i < n; i++)
    {
        int s = order[i];
        unsigned long f = freq[s];
        j = i - 1;
        while (j >= 0 && freq[order[j]] < f)
        {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = s;
    }
    i = 0;
    while (i < n && freq[order[i]]) i++;
    if (i > 0)
        SfSplit(order, freq, 0, i - 1, 0, lengths);
    BuildCodes(lengths, n, codes);
}

/* KWAJ method 4 writer: raw DEFLATE with fixed Huffman codes.        */
static const unsigned short C_LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char C_LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short C_DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577
};
static const unsigned char C_DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static const unsigned char C_CLORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* dynamic-Huffman DEFLATE token with per-block code tables */
static int DeflateToken(BitWriter *bw, LzToken *t,
                        unsigned char *ll, unsigned int *lc,
                        unsigned char *dl, unsigned int *dc)
{
    if (!t->match)
        return BwCode(bw, lc[t->lit], ll[t->lit]);
    else
    {
        unsigned int len = t->len, dist = t->dist, sym;
        int i, nb, extra;
        for (i = 28; i >= 0; i--)
            if (len >= C_LEN_BASE[i]) break;
        if (i < 0) return -1;
        sym = 257u + (unsigned int)i;
        if (BwCode(bw, lc[sym], ll[sym])) return -1;
        extra = (int)(len - C_LEN_BASE[i]);
        nb = C_LEN_EXTRA[i];
        if (nb && BwLsb(bw, (unsigned int)extra, nb)) return -1;

        for (i = 29; i >= 0; i--)
            if (dist >= C_DIST_BASE[i]) break;
        if (i < 0) return -1;
        if (BwCode(bw, dc[i], dl[i])) return -1;
        nb = C_DIST_EXTRA[i];
        extra = (int)(dist - C_DIST_BASE[i]);
        if (nb && BwLsb(bw, (unsigned int)extra, nb)) return -1;
        return 0;
    }
}

typedef struct
{
    int sym, extra, nbits;
} ClSym;

/* Turn a list of code lengths into the DEFLATE code-length alphabet,
 * using repeat codes 16/17/18 for long runs. */
static int MakeClSeq(unsigned char *lens, int n, ClSym *seq)
{
    int i = 0, k = 0, prev = -1;
    while (i < n)
    {
        int v = lens[i], cnt = 1, rem;
        while (i + cnt < n && lens[i + cnt] == v) cnt++;
        rem = cnt;
        if (v == 0)
        {
            while (rem >= 11)
            {
                int t = rem > 138 ? 138 : rem;
                seq[k].sym = 18; seq[k].extra = t - 11; seq[k++].nbits = 7;
                rem -= t;
            }
            while (rem >= 3)
            {
                int t = rem > 10 ? 10 : rem;
                seq[k].sym = 17; seq[k].extra = t - 3; seq[k++].nbits = 3;
                rem -= t;
            }
        }
        else if (v == prev && rem >= 3)
        {
            while (rem >= 3)
            {
                int t = rem > 6 ? 6 : rem;
                seq[k].sym = 16; seq[k].extra = t - 3; seq[k++].nbits = 2;
                rem -= t;
            }
        }
        while (rem--)
        {
            seq[k].sym = v; seq[k].extra = 0; seq[k++].nbits = 0;
        }
        prev = v;
        i += cnt;
    }
    return k;
}

static int WriteMszipBlock(BitWriter *bw, LzToken *tok,
                           unsigned long from, unsigned long to)
{
    unsigned long lf[288], df[32], clf[19];
    unsigned char ll[288], dl[32], cll[19];
    unsigned int lc[288], dc[32], clc[19];
    unsigned long i;
    int t, used_dist = 0;

    for (i = 0; i < 288; i++) lf[i] = 0;
    for (i = 0; i < 32; i++) df[i] = 0;
    lf[256] = 1;                       /* end-of-block */
    for (i = from; i < to; i++)
    {
        LzToken *p = &tok[i];
        if (!p->match)
            lf[p->lit]++;
        else
        {
            int k;
            for (k = 28; k >= 0; k--)
                if (p->len >= C_LEN_BASE[k]) break;
            if (k < 0) return -1;
            lf[257 + k]++;
            for (k = 29; k >= 0; k--)
                if (p->dist >= C_DIST_BASE[k]) break;
            if (k < 0) return -1;
            df[k]++;
            used_dist = 1;
        }
    }
    if (!used_dist) df[0] = 1;         /* keep one distance code */
    BuildTree(lf, 286, ll, lc);
    BuildTree(df, 30, dl, dc);

    {
        ClSym litseq[400], distseq[400];
        int nlit = MakeClSeq(ll, 286, litseq);
        int ndist = MakeClSeq(dl, 30, distseq);

        for (i = 0; i < 19; i++) clf[i] = 0;
        for (i = 0; i < (unsigned long)nlit; i++) clf[litseq[i].sym]++;
        for (i = 0; i < (unsigned long)ndist; i++) clf[distseq[i].sym]++;
        BuildTree(clf, 19, cll, clc);

        if (BwBit(bw, 1)) return -1;       /* BFINAL */
        if (BwLsb(bw, 2u, 2)) return -1;   /* BTYPE = 10, dynamic */
        if (BwLsb(bw, 29u, 5)) return -1;  /* HLIT  = 286 */
        if (BwLsb(bw, 29u, 5)) return -1;  /* HDIST = 30  */
        if (BwLsb(bw, 15u, 4)) return -1;  /* HCLEN = 19  */
        for (t = 0; t < 19; t++)
            if (BwLsb(bw, cll[C_CLORDER[t]], 3)) return -1;
        for (i = 0; i < (unsigned long)nlit; i++)
        {
            if (BwCode(bw, clc[litseq[i].sym], cll[litseq[i].sym])) return -1;
            if (litseq[i].nbits &&
                BwLsb(bw, (unsigned int)litseq[i].extra, litseq[i].nbits))
                return -1;
        }
        for (i = 0; i < (unsigned long)ndist; i++)
        {
            if (BwCode(bw, clc[distseq[i].sym], cll[distseq[i].sym])) return -1;
            if (distseq[i].nbits &&
                BwLsb(bw, (unsigned int)distseq[i].extra, distseq[i].nbits))
                return -1;
        }
    }
    for (i = from; i < to; i++)
        if (DeflateToken(bw, &tok[i], ll, lc, dl, dc)) return -1;
    return BwCode(bw, lc[256], ll[256]);
}

static int WriteMszip(LzToken *tok, unsigned long n, FILE *out)
{
    unsigned long i = 0;
    unsigned long bytes = 0;
    unsigned long from = 0;
    int rc = -1;

    for (;;)
    {
        BitWriter bw;
        unsigned long step;
        unsigned int clen;

        while (i < n && bytes < C_MSZIP_BLOCK)
        {
            bytes += tok[i].len;
            i++;
        }
        if (!BwInit(&bw)) return -1;
        if (WriteMszipBlock(&bw, tok, from, i))
        {
            BwFree(&bw);
            return -1;
        }
        if (BwFlush(&bw))
        {
            BwFree(&bw);
            return -1;
        }
        if (bw.len + 2u > 0xFFFFu)
        {
            BwFree(&bw);
            return -1;
        }
        clen = (unsigned int)bw.len + 2u;
        if (fputc((int)(clen & 0xFF), out) == EOF ||
            fputc((int)((clen >> 8) & 0xFF), out) == EOF ||
            fputc('C', out) == EOF || fputc('K', out) == EOF ||
            fwrite(bw.buf, 1, bw.len, out) != bw.len)
        {
            BwFree(&bw);
            return -1;
        }
        BwFree(&bw);
        step = i;
        if (i >= n)
        {
            (void)step;
            break;
        }
        from = i;
        bytes = 0;
    }
    if (fputc(0, out) == EOF || fputc(0, out) == EOF)
        return -1;
    rc = 0;
    return rc;
}

/* Public API: compress one 32 KB frame into an MSZIP CFDATA payload.
   Returns the number of bytes written (including the "CK" signature). */
unsigned int MsZipCompressFrame(const unsigned char *in, unsigned long n,
                                unsigned char *out, unsigned long cap)
{
    LzToken *tok = NULL;
    unsigned long ntok = 0, i, pos = 0, inoff = 0;
    int rc;

    if (n == 0 || n > C_MSZIP_BLOCK) return 0;
    rc = LzParse(in, n, 32768, 0, 0, 3, 258, 32768, &tok, &ntok);
    if (rc) return 0;

    pos = 0;
    i = 0;
    inoff = 0;
    while (i < ntok)
    {
        BitWriter bw;
        unsigned long from = i, bytes = 0;
        if (!BwInit(&bw)) { free(tok); return 0; }
        while (i < ntok && bytes < C_MSZIP_BLOCK) { bytes += tok[i].len; i++; }
        if (WriteMszipBlock(&bw, tok, from, i) || BwFlush(&bw))
        {
            BwFree(&bw); free(tok); return 0;
        }
        /* A stored DEFLATE block costs 7 bytes of overhead; for
           incompressible data the dynamic tree makes the block larger
           than that, and cabinet.dll's MSZIP decoder rejects it. */
        if (2UL + bw.len > 7UL + bytes) {
            unsigned long j;
            unsigned long len = bytes;
            if (pos + 7 + len > cap) { BwFree(&bw); free(tok); return 0; }
            out[pos++] = 'C';
            out[pos++] = 'K';
            out[pos++] = 0x01;                    /* BFINAL=1, BTYPE=00 */
            out[pos++] = (unsigned char)(len & 0xFF);
            out[pos++] = (unsigned char)((len >> 8) & 0xFF);
            out[pos++] = (unsigned char)(~len & 0xFF);
            out[pos++] = (unsigned char)((~len >> 8) & 0xFF);
            memcpy(out + pos, in + inoff, (size_t)len);
            pos += len;
        }
        else {
            if (pos + 2 + bw.len > cap) { BwFree(&bw); free(tok); return 0; }
            out[pos++] = 'C';
            out[pos++] = 'K';
            memcpy(out + pos, bw.buf, bw.len);
            pos += bw.len;
        }
        BwFree(&bw);
        inoff += bytes;
    }
    free(tok);
    return pos;
}