/**
 * ZjStream encoder for HP LaserJet 1020.
 * Wraps JBIG-compressed bands in the Zenographics ZjStream chunk format.
 */

#include "zjstream.h"
#include "jbig_enc.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "zjstream";

/* ────────── ZjStream chunk types ────────── */

typedef enum {
    ZJT_START_DOC  = 0,
    ZJT_END_DOC    = 1,
    ZJT_START_PAGE = 2,
    ZJT_END_PAGE   = 3,
    ZJT_JBIG_BIH   = 4,
    ZJT_JBIG_BID   = 5,
    ZJT_END_JBIG   = 6,
} zj_chunk_type_t;

/* ────────── ZjStream item IDs ────────── */

#define ZJI_PAGECOUNT       0
#define ZJI_DMCOLLATE       1
#define ZJI_DMDUPLEX        2
#define ZJI_DMPAPER         3
#define ZJI_DMCOPIES        4
#define ZJI_DMDEFAULTSOURCE 5
#define ZJI_DMMEDIATYPE     6
#define ZJI_NBIE            7
#define ZJI_RESOLUTION_X    8
#define ZJI_RESOLUTION_Y    9
#define ZJI_OFFSET_X        10
#define ZJI_OFFSET_Y        11
#define ZJI_RASTER_X        12
#define ZJI_RASTER_Y        13

#define ZJ_MAGIC 0x5A4A5A4AUL

/* ────────── Internal types ────────── */

#pragma pack(push, 1)
typedef struct {
    uint32_t size;
    uint32_t type;
    uint32_t items;
} zj_header_t;

typedef struct {
    uint32_t size;
    uint32_t item;
    uint32_t type;      /* 0 = ZJIT_UINT32 */
    uint32_t value;
} zj_item_t;
#pragma pack(pop)

struct zjstream {
    zjstream_config_t cfg;
    uint32_t band_count;
};

/* ────────── Helpers ────────── */

static inline uint32_t le32(uint32_t v) { return v; } /* x86/ARM = little-endian, same as ZjStream */

static void emit(zjstream_t *z, const void *data, size_t len)
{
    if (z->cfg.cb)
        z->cfg.cb((const uint8_t *)data, len, z->cfg.user);
}

static void write_chunk(zjstream_t *z, zj_chunk_type_t type, int nitems,
                        const zj_item_t *items, const uint8_t *payload, size_t payload_len)
{
    uint32_t total = sizeof(zj_header_t) + nitems * sizeof(zj_item_t) + payload_len;
    zj_header_t hdr = {
        .size  = le32(total),
        .type  = le32((uint32_t)type),
        .items = le32((uint32_t)nitems),
    };
    emit(z, &hdr, sizeof(hdr));
    for (int i = 0; i < nitems; i++) {
        emit(z, &items[i], sizeof(zj_item_t));
    }
    if (payload && payload_len > 0) {
        emit(z, payload, payload_len);
    }
}

static zj_item_t make_item(uint32_t id, uint32_t val)
{
    zj_item_t it = {
        .size  = le32(sizeof(zj_item_t)),
        .item  = le32(id),
        .type  = le32(0),   /* ZJIT_UINT32 */
        .value = le32(val),
    };
    return it;
}

/* ────────── Public API ────────── */

zjstream_t *zjstream_create(const zjstream_config_t *cfg)
{
    zjstream_t *z = calloc(1, sizeof(zjstream_t));
    if (!z) return NULL;
    z->cfg = *cfg;
    return z;
}

void zjstream_start_doc(zjstream_t *z)
{
    uint32_t magic = le32(ZJ_MAGIC);
    emit(z, &magic, 4);

    zj_item_t items[] = {
        make_item(ZJI_DMCOLLATE, 1),
        make_item(ZJI_PAGECOUNT, 0),
    };
    write_chunk(z, ZJT_START_DOC, 2, items, NULL, 0);
    ESP_LOGI(TAG, "start_doc");
}

void zjstream_start_page(zjstream_t *z, uint32_t page_w, uint32_t page_h)
{
    z->band_count = 0;
    z->cfg.width_px = page_w;
    z->cfg.height_px = page_h;

    zj_item_t items[] = {
        make_item(ZJI_DMPAPER,         z->cfg.paper),
        make_item(ZJI_DMCOPIES,        z->cfg.copies ? z->cfg.copies : 1),
        make_item(ZJI_DMDEFAULTSOURCE, 7),
        make_item(ZJI_DMMEDIATYPE,     0),
        make_item(ZJI_NBIE,            1),
        make_item(ZJI_RESOLUTION_X,    z->cfg.dpi),
        make_item(ZJI_RESOLUTION_Y,    z->cfg.dpi),
        make_item(ZJI_RASTER_X,        page_w),
        make_item(ZJI_RASTER_Y,        page_h),
        make_item(ZJI_OFFSET_X,        0),
        make_item(ZJI_OFFSET_Y,        0),
    };
    write_chunk(z, ZJT_START_PAGE, 11, items, NULL, 0);
    ESP_LOGI(TAG, "start_page %lux%lu", (unsigned long)page_w, (unsigned long)page_h);
}

typedef struct {
    zjstream_t *zjs;
    bool bih_sent;
} band_ctx_t;

static void jbig_output_cb(const uint8_t *data, size_t len, void *user_data)
{
    band_ctx_t *bc = (band_ctx_t *)user_data;
    zjstream_t *z = bc->zjs;

    if (!bc->bih_sent) {
        ESP_LOGW(TAG, "unexpected jbig data before BIH");
    }
    write_chunk(z, ZJT_JBIG_BID, 0, NULL, data, len);
}

void zjstream_write_band(zjstream_t *z, const uint8_t *bitmap, uint32_t lines)
{
    uint32_t w = z->cfg.width_px;

    band_ctx_t bc = { .zjs = z, .bih_sent = false };

    jbig_enc_t *enc = jbig_enc_create(w, lines, lines, jbig_output_cb, &bc);
    if (!enc) {
        ESP_LOGE(TAG, "jbig_enc_create failed");
        return;
    }

    uint8_t bih[20];
    jbig_enc_get_bih(enc, bih);
    write_chunk(z, ZJT_JBIG_BIH, 0, NULL, bih, 20);
    bc.bih_sent = true;

    jbig_enc_encode(enc, bitmap);
    jbig_enc_destroy(enc);

    write_chunk(z, ZJT_END_JBIG, 0, NULL, NULL, 0);
    z->band_count++;
}

void zjstream_end_page(zjstream_t *z)
{
    write_chunk(z, ZJT_END_PAGE, 0, NULL, NULL, 0);
    ESP_LOGI(TAG, "end_page (%lu bands)", (unsigned long)z->band_count);
}

void zjstream_end_doc(zjstream_t *z)
{
    write_chunk(z, ZJT_END_DOC, 0, NULL, NULL, 0);
    ESP_LOGI(TAG, "end_doc");
}

void zjstream_destroy(zjstream_t *z)
{
    free(z);
}
