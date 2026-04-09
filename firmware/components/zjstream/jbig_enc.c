/**
 * Minimal JBIG (ITU T.82) encoder for bi-level images.
 * Implements template 1 (2-line, 10-bit context) with QM arithmetic coder.
 * Designed for the HP LaserJet 1020 ZjStream pipeline.
 */

#include "jbig_enc.h"
#include <stdlib.h>
#include <string.h>

/* ────────── QM Arithmetic Encoder State Table (ITU T.82 Table A.1) ────────── */

typedef struct {
    uint16_t qe;
    uint8_t  nmps;
    uint8_t  nlps;
    uint8_t  sw;
} qe_entry_t;

static const qe_entry_t QE[47] = {
    {0x5601,  1,  1, 1}, {0x3401,  2,  6, 0}, {0x1801,  3,  9, 0},
    {0x0AC1,  4, 12, 0}, {0x0521,  5, 29, 0}, {0x0221, 38, 33, 0},
    {0x5601,  7,  6, 1}, {0x5401,  8, 14, 0}, {0x4801,  9, 14, 0},
    {0x3801, 10, 14, 0}, {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0},
    {0x1C01, 13, 20, 0}, {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1},
    {0x5401, 16, 14, 0}, {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0},
    {0x3801, 19, 17, 0}, {0x3401, 20, 18, 0}, {0x3001, 21, 19, 0},
    {0x2801, 22, 19, 0}, {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0},
    {0x1C01, 25, 22, 0}, {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0},
    {0x1401, 28, 25, 0}, {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0},
    {0x0AC1, 31, 28, 0}, {0x09C1, 32, 29, 0}, {0x08A1, 33, 30, 0},
    {0x0521, 34, 31, 0}, {0x0441, 35, 32, 0}, {0x02A1, 36, 33, 0},
    {0x0221, 37, 34, 0}, {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0},
    {0x0085, 40, 37, 0}, {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0},
    {0x0015, 43, 40, 0}, {0x0009, 44, 41, 0}, {0x0005, 45, 42, 0},
    {0x0001, 45, 43, 0}, {0x5601, 46, 46, 0},
};

/* ────────── Arithmetic Encoder ────────── */

typedef struct {
    uint32_t c;
    uint32_t a;
    int      ct;
    uint8_t *bp;
    uint8_t *buf;
    size_t   buf_cap;
} arith_t;

static void arith_init(arith_t *ar, uint8_t *buf, size_t cap)
{
    ar->a = 0x10000;
    ar->c = 0;
    ar->ct = 12;
    ar->buf = buf;
    ar->bp = buf;
    ar->buf_cap = cap;
    if (cap > 0) buf[0] = 0;
}

static inline void arith_byteout(arith_t *ar)
{
    uint8_t *bp = ar->bp;
    if (bp >= ar->buf && bp < ar->buf + ar->buf_cap && *bp == 0xFF) {
        bp++;
        ar->bp = bp;
        if (bp < ar->buf + ar->buf_cap)
            *bp = (uint8_t)(ar->c >> 20);
        ar->c &= 0xFFFFF;
        ar->ct = 7;
    } else {
        if (!(ar->c & 0x8000000)) {
            bp++;
            ar->bp = bp;
            if (bp < ar->buf + ar->buf_cap)
                *bp = (uint8_t)(ar->c >> 19);
            ar->c &= 0x7FFFF;
            ar->ct = 8;
        } else {
            if (bp >= ar->buf && bp < ar->buf + ar->buf_cap)
                (*bp)++;
            ar->c &= 0x7FFFFFF;
            arith_byteout(ar);
        }
    }
}

static void arith_encode(arith_t *ar, int cx, int pix, uint8_t *st)
{
    uint8_t s = st[cx];
    int mps = s & 1;
    int idx = s >> 1;
    uint16_t qe = QE[idx].qe;

    ar->a -= qe;

    if (pix == mps) {
        if (ar->a & 0xFFFF8000) {
            st[cx] = (QE[idx].nmps << 1) | mps;
            return;
        }
        if (ar->a < qe) {
            ar->c += ar->a;
            ar->a = qe;
        }
        st[cx] = (QE[idx].nmps << 1) | mps;
    } else {
        if (ar->a >= qe) {
            ar->c += ar->a;
            ar->a = qe;
        }
        st[cx] = (QE[idx].nlps << 1) | (mps ^ QE[idx].sw);
    }

    do {
        ar->a <<= 1;
        ar->c <<= 1;
        ar->ct--;
        if (ar->ct == 0)
            arith_byteout(ar);
    } while (!(ar->a & 0xFFFF8000));
}

static size_t arith_flush(arith_t *ar)
{
    uint32_t temp = (ar->a - 1 + ar->c) & 0xFFFF0000;
    if (temp < ar->c) temp += 0x8000;
    ar->c = temp;

    ar->c <<= ar->ct;
    arith_byteout(ar);
    ar->c <<= ar->ct;
    arith_byteout(ar);

    size_t len = (size_t)(ar->bp - ar->buf) + 1;
    if (ar->bp >= ar->buf && ar->bp < ar->buf + ar->buf_cap && *ar->bp != 0xFF)
        len++;
    return len > ar->buf_cap ? ar->buf_cap : len;
}

/* ────────── JBIG Encoder ────────── */

#define JBIG_NUM_CONTEXTS 1024
#define JBIG_ORDER  0x03   /* ILEAVE | SMID */
#define JBIG_OPT    0x40   /* LRLTWO (template 1) */

struct jbig_enc {
    uint32_t xd, yd, l0;
    uint32_t bpl;           /* bytes per line = ceil(xd / 8) */
    jbig_output_cb_t cb;
    void *user;
};

static inline int get_pixel(const uint8_t *line, int x, int max_x)
{
    if (x < 0 || x >= max_x || !line) return 0;
    return (line[x >> 3] >> (7 - (x & 7))) & 1;
}

jbig_enc_t *jbig_enc_create(uint32_t width, uint32_t height, uint32_t l0,
                             jbig_output_cb_t cb, void *user)
{
    jbig_enc_t *e = calloc(1, sizeof(jbig_enc_t));
    if (!e) return NULL;
    e->xd = width;
    e->yd = height;
    e->l0 = l0 ? l0 : height;
    e->bpl = (width + 7) / 8;
    e->cb = cb;
    e->user = user;
    return e;
}

void jbig_enc_get_bih(jbig_enc_t *enc, uint8_t bih[20])
{
    memset(bih, 0, 20);
    /* byte 0: DL, byte 1: D, byte 2: P */
    bih[0] = 0;
    bih[1] = 0;
    bih[2] = 1;
    /* bytes 4-7: Xd */
    bih[4] = (enc->xd >> 24) & 0xFF;
    bih[5] = (enc->xd >> 16) & 0xFF;
    bih[6] = (enc->xd >> 8)  & 0xFF;
    bih[7] = enc->xd & 0xFF;
    /* bytes 8-11: Yd */
    bih[8]  = (enc->yd >> 24) & 0xFF;
    bih[9]  = (enc->yd >> 16) & 0xFF;
    bih[10] = (enc->yd >> 8)  & 0xFF;
    bih[11] = enc->yd & 0xFF;
    /* bytes 12-15: L0 */
    bih[12] = (enc->l0 >> 24) & 0xFF;
    bih[13] = (enc->l0 >> 16) & 0xFF;
    bih[14] = (enc->l0 >> 8)  & 0xFF;
    bih[15] = enc->l0 & 0xFF;
    /* byte 16: Mx */
    bih[16] = 8;
    /* byte 17: My */
    bih[17] = 0;
    /* byte 18: order */
    bih[18] = JBIG_ORDER;
    /* byte 19: options */
    bih[19] = JBIG_OPT;
}

void jbig_enc_encode(jbig_enc_t *enc, const uint8_t *bitmap)
{
    uint32_t w = enc->xd;
    uint32_t h = enc->yd;
    uint32_t bpl = enc->bpl;
    uint32_t l0 = enc->l0;

    size_t out_cap = bpl * l0 + 4096;
    uint8_t *out_buf = malloc(out_cap);
    uint8_t *cx_st = calloc(JBIG_NUM_CONTEXTS, 1);
    if (!out_buf || !cx_st) {
        free(out_buf);
        free(cx_st);
        return;
    }

    uint32_t y = 0;
    while (y < h) {
        uint32_t stripe_h = l0;
        if (y + stripe_h > h) stripe_h = h - y;

        arith_t ar;
        arith_init(&ar, out_buf, out_cap);
        memset(cx_st, 0, JBIG_NUM_CONTEXTS);

        for (uint32_t ly = 0; ly < stripe_h; ly++) {
            const uint8_t *cur  = bitmap + (y + ly) * bpl;
            const uint8_t *prev = (y + ly > 0) ? bitmap + (y + ly - 1) * bpl : NULL;

            /*
             * Template 1 (2-line) context, 10-bit:
             * bit 9: prev_line[x+3]   bit 8: prev_line[x+2]
             * bit 7: prev_line[x+1]   bit 6: prev_line[x]
             * bit 5: prev_line[x-1]   bit 4: prev_line[x-2]
             * bit 3: prev_line[x-3]   (AT[0] default)
             * bit 2: cur_line[x-1]    bit 1: cur_line[x-2]
             * bit 0: cur_line[x-3]    (AT[1] default)
             */
            for (uint32_t x = 0; x < w; x++) {
                int cx = 0;
                cx |= get_pixel(prev, x + 3, w) << 9;
                cx |= get_pixel(prev, x + 2, w) << 8;
                cx |= get_pixel(prev, x + 1, w) << 7;
                cx |= get_pixel(prev, x,     w) << 6;
                cx |= get_pixel(prev, x - 1, w) << 5;
                cx |= get_pixel(prev, x - 2, w) << 4;
                cx |= get_pixel(prev, x - 3, w) << 3;
                cx |= get_pixel(cur,  x - 1, w) << 2;
                cx |= get_pixel(cur,  x - 2, w) << 1;
                cx |= get_pixel(cur,  x - 3, w) << 0;

                int pixel = get_pixel(cur, x, w);
                arith_encode(&ar, cx, pixel, cx_st);
            }
        }

        size_t comp_len = arith_flush(&ar);
        if (enc->cb && comp_len > 0) {
            enc->cb(out_buf, comp_len, enc->user);
        }

        y += stripe_h;
    }

    free(out_buf);
    free(cx_st);
}

void jbig_enc_destroy(jbig_enc_t *enc)
{
    free(enc);
}
