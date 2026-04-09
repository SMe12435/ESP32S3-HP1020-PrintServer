#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef void (*zjs_output_cb_t)(const uint8_t *data, size_t len, void *user_data);

typedef struct {
    uint32_t width_px;
    uint32_t height_px;
    uint32_t dpi;
    uint32_t paper;       // 1=letter, 9=A4
    uint32_t copies;
    zjs_output_cb_t cb;
    void *user;
} zjstream_config_t;

typedef struct zjstream zjstream_t;

zjstream_t *zjstream_create(const zjstream_config_t *cfg);

/** Emit the document-start header. Call once. */
void zjstream_start_doc(zjstream_t *zjs);

/** Emit page-start with params. Call once per page. */
void zjstream_start_page(zjstream_t *zjs, uint32_t page_width_px, uint32_t page_height_px);

/**
 * Encode and emit one band of bitmap data.
 * bitmap: 1bpp packed, MSB-first, ceil(width/8) * lines bytes.
 * lines: number of lines in this band (last band may be smaller).
 */
void zjstream_write_band(zjstream_t *zjs, const uint8_t *bitmap, uint32_t lines);

/** Emit page-end. */
void zjstream_end_page(zjstream_t *zjs);

/** Emit document-end. */
void zjstream_end_doc(zjstream_t *zjs);

void zjstream_destroy(zjstream_t *zjs);
