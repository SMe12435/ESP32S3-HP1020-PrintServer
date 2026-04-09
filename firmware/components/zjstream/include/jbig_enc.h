#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef void (*jbig_output_cb_t)(const uint8_t *data, size_t len, void *user_data);

typedef struct jbig_enc jbig_enc_t;

/**
 * Create a JBIG encoder for one image (or one band).
 * Uses ITU T.82 template 1 (2-line, 10-bit context).
 *
 * @param width   Image width in pixels
 * @param height  Image height in pixels (can be stripe height for band-by-band)
 * @param l0      Lines per stripe (0 = entire height)
 * @param cb      Output callback for BIE data (called multiple times)
 * @param user    User data for callback
 */
jbig_enc_t *jbig_enc_create(uint32_t width, uint32_t height, uint32_t l0,
                             jbig_output_cb_t cb, void *user);

/**
 * Write the 20-byte BIH (Bi-level Image Header) into buf.
 */
void jbig_enc_get_bih(jbig_enc_t *enc, uint8_t bih[20]);

/**
 * Encode a full image. bitmap must be packed 1-bit-per-pixel, MSB first,
 * rows padded to byte boundary. Total size = ceil(width/8) * height bytes.
 * Calls the output callback with compressed BIE data blocks.
 */
void jbig_enc_encode(jbig_enc_t *enc, const uint8_t *bitmap);

/**
 * Free encoder resources.
 */
void jbig_enc_destroy(jbig_enc_t *enc);
