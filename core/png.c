/* See png.h. */
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "png.h"

#define PNG_SIG_LEN 8
#define CHUNK_HDR   8   /* length (4) + type (4); the CRC is another 4 after */

static const uint8_t PNG_SIG[PNG_SIG_LEN] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

const char *apng_strerror(int rc)
{
    switch (rc) {
        case APNG_OK:          return "ok";
        case APNG_ERR_FORMAT:  return "not a PNG, or a damaged one";
        case APNG_ERR_SUPPORT: return "an interlaced PNG, which this does not read";
        case APNG_ERR_MEMORY:  return "out of memory";
        case APNG_ERR_SIZE:    return "an image far larger than any save icon";
        default:               return "unknown error";
    }
}

void apng_free(uint8_t *rgba)
{
    free(rgba);
}

/* How many channels a colour type has, or 0 for one that is not a colour
 * type at all. */
static int channels_of(int color)
{
    switch (color) {
        case 0: return 1;   /* greyscale             */
        case 2: return 3;   /* RGB                   */
        case 3: return 1;   /* palette index         */
        case 4: return 2;   /* greyscale + alpha     */
        case 6: return 4;   /* RGBA                  */
        default: return 0;
    }
}

/*
 * Inflate the concatenated IDAT data into a buffer of exactly `want` bytes.
 *
 * Written against inflate() rather than uncompress() because a PNG is allowed
 * to carry more compressed data than the image needs (and some do): stopping
 * at `want` is correct, where uncompress() would call the leftovers an error.
 */
static int inflate_exact(const uint8_t *src, size_t src_len, uint8_t *dst, size_t want)
{
    z_stream zs;
    int      rc;

    memset(&zs, 0, sizeof zs);
    if (inflateInit(&zs) != Z_OK)
        return APNG_ERR_MEMORY;

    zs.next_in  = (Bytef *)src;
    zs.avail_in = (uInt)src_len;
    zs.next_out = dst;
    zs.avail_out = (uInt)want;

    rc = inflate(&zs, Z_FINISH);
    /* Z_OK and Z_BUF_ERROR both mean "stopped early"; with avail_out at zero
     * that is exactly the wanted bytes and nothing more, which is success. */
    if (rc != Z_STREAM_END && zs.avail_out != 0) {
        inflateEnd(&zs);
        return APNG_ERR_FORMAT;
    }
    inflateEnd(&zs);
    return APNG_OK;
}

static int paeth(int a, int b, int c)
{
    const int p  = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/*
 * Undo the per-scanline filters, in place over the inflated data.
 *
 * `raw` holds height rows of (1 + stride) bytes: a filter byte then the row.
 * Each row is rewritten as its unfiltered self, still at the same place, so
 * the caller reads rows at 1 + row * (1 + stride).
 */
static int unfilter(uint8_t *raw, size_t stride, int height, int bpp)
{
    int row;

    for (row = 0; row < height; row++) {
        uint8_t       *line = raw + (size_t)row * (stride + 1);
        const uint8_t  type = line[0];
        uint8_t       *cur  = line + 1;
        const uint8_t *up   = row ? cur - (stride + 1) : NULL;
        size_t         i;

        switch (type) {
            case 0:  /* None */
                break;
            case 1:  /* Sub */
                for (i = (size_t)bpp; i < stride; i++)
                    cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
                break;
            case 2:  /* Up */
                if (up)
                    for (i = 0; i < stride; i++)
                        cur[i] = (uint8_t)(cur[i] + up[i]);
                break;
            case 3:  /* Average */
                for (i = 0; i < stride; i++) {
                    const int a = i >= (size_t)bpp ? cur[i - bpp] : 0;
                    const int b = up ? up[i] : 0;
                    cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
                }
                break;
            case 4:  /* Paeth */
                for (i = 0; i < stride; i++) {
                    const int a = i >= (size_t)bpp ? cur[i - bpp] : 0;
                    const int b = up ? up[i] : 0;
                    const int c = (up && i >= (size_t)bpp) ? up[i - bpp] : 0;
                    cur[i] = (uint8_t)(cur[i] + paeth(a, b, c));
                }
                break;
            default:
                return APNG_ERR_FORMAT;
        }
    }
    return APNG_OK;
}

/* One sample out of a row, for the bit depths below 8. */
static unsigned sample_at(const uint8_t *row, int depth, size_t index)
{
    switch (depth) {
        case 1:  return (row[index >> 3] >> (7 - (index & 7))) & 1u;
        case 2:  return (row[index >> 2] >> (6 - 2 * (index & 3))) & 3u;
        case 4:  return (row[index >> 1] >> (4 - 4 * (index & 1))) & 15u;
        case 8:  return row[index];
        default: return row[index * 2];   /* 16-bit: the high byte is enough */
    }
}

/* Scale a sample of `depth` bits to 0..255. */
static uint8_t to_byte(unsigned v, int depth)
{
    switch (depth) {
        case 1:  return v ? 255 : 0;
        case 2:  return (uint8_t)(v * 85);
        case 4:  return (uint8_t)(v * 17);
        default: return (uint8_t)v;
    }
}

int apng_decode(const uint8_t *data, size_t len, uint8_t **rgba_out, int *w_out, int *h_out)
{
    size_t   at = PNG_SIG_LEN;
    uint32_t width = 0, height = 0;
    int      depth = 0, color = 0, interlace = 0, channels = 0, bpp = 0;
    size_t   stride = 0, raw_len = 0;
    uint8_t *idat = NULL, *raw = NULL, *rgba = NULL;
    size_t   idat_len = 0;
    uint8_t  palette[256 * 3];
    uint8_t  pal_alpha[256];
    int      pal_count = 0, seen_ihdr = 0;
    int      rc = APNG_ERR_FORMAT;
    uint32_t y;

    if (rgba_out) *rgba_out = NULL;
    if (!data || !rgba_out || !w_out || !h_out)
        return APNG_ERR_FORMAT;
    if (len < PNG_SIG_LEN + CHUNK_HDR || memcmp(data, PNG_SIG, PNG_SIG_LEN) != 0)
        return APNG_ERR_FORMAT;

    memset(pal_alpha, 0xFF, sizeof pal_alpha);

    /* ---- the chunks -------------------------------------------------- */
    while (at + CHUNK_HDR <= len) {
        const uint32_t clen = be32(data + at);
        const uint8_t *type = data + at + 4;
        const uint8_t *body = data + at + CHUNK_HDR;

        /* The length is the file's own claim, so it is checked against what
         * is actually left -- including the 4 CRC bytes that follow. */
        if (clen > len - at - CHUNK_HDR || len - at - CHUNK_HDR - clen < 4)
            break;

        if (!memcmp(type, "IHDR", 4)) {
            if (clen < 13) goto done;
            width     = be32(body);
            height    = be32(body + 4);
            depth     = body[8];
            color     = body[9];
            interlace = body[12];

            if (!width || !height) goto done;
            if (width > APNG_MAX_DIM || height > APNG_MAX_DIM) {
                rc = APNG_ERR_SIZE;
                goto done;
            }
            if (body[10] != 0 || body[11] != 0) goto done;   /* deflate, adaptive */
            if (interlace) { rc = APNG_ERR_SUPPORT; goto done; }

            channels = channels_of(color);
            if (!channels) goto done;
            if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)
                goto done;
            /* Only a palette or greyscale may be narrower than a byte. */
            if (depth < 8 && color != 0 && color != 3) goto done;
            if (color == 3 && depth == 16) goto done;

            seen_ihdr = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            if (clen > sizeof palette || clen % 3) goto done;
            memcpy(palette, body, clen);
            pal_count = (int)(clen / 3);
        } else if (!memcmp(type, "tRNS", 4)) {
            /* Only the palette form is honoured. The other two mark a single
             * colour transparent, which no save icon uses. */
            if (color == 3 && clen <= sizeof pal_alpha)
                memcpy(pal_alpha, body, clen);
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *grown;

            if (!seen_ihdr) goto done;
            grown = realloc(idat, idat_len + clen);
            if (!grown) { rc = APNG_ERR_MEMORY; goto done; }
            idat = grown;
            memcpy(idat + idat_len, body, clen);
            idat_len += clen;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }

        at += CHUNK_HDR + clen + 4;
    }

    if (!seen_ihdr || !idat_len)
        goto done;
    if (color == 3 && pal_count == 0)
        goto done;

    /* ---- room for it -------------------------------------------------- */
    /* ceil(width * channels * depth / 8), with the multiply done in size_t
     * and bounded by APNG_MAX_DIM above, so it cannot wrap. */
    stride = ((size_t)width * (size_t)channels * (size_t)depth + 7) / 8;
    bpp    = (channels * depth + 7) / 8;
    if (bpp < 1) bpp = 1;

    raw_len = (stride + 1) * (size_t)height;
    raw = malloc(raw_len);
    rgba = malloc((size_t)width * (size_t)height * 4);
    if (!raw || !rgba) { rc = APNG_ERR_MEMORY; goto done; }

    rc = inflate_exact(idat, idat_len, raw, raw_len);
    if (rc != APNG_OK) goto done;

    rc = unfilter(raw, stride, (int)height, bpp);
    if (rc != APNG_OK) goto done;

    /* ---- to RGBA ------------------------------------------------------ */
    for (y = 0; y < height; y++) {
        const uint8_t *row = raw + (size_t)y * (stride + 1) + 1;
        uint8_t       *out = rgba + (size_t)y * width * 4;
        uint32_t       x;

        for (x = 0; x < width; x++) {
            const size_t s = (size_t)x * channels;
            uint8_t r, g, b, a = 255;

            switch (color) {
                case 0:   /* greyscale */
                    r = g = b = to_byte(sample_at(row, depth, s), depth);
                    break;
                case 2:   /* RGB */
                    r = to_byte(sample_at(row, depth, s + 0), depth);
                    g = to_byte(sample_at(row, depth, s + 1), depth);
                    b = to_byte(sample_at(row, depth, s + 2), depth);
                    break;
                case 3: { /* palette */
                    const unsigned i = sample_at(row, depth, s);
                    if ((int)i >= pal_count) { r = g = b = 0; a = 0; break; }
                    r = palette[i * 3 + 0];
                    g = palette[i * 3 + 1];
                    b = palette[i * 3 + 2];
                    a = pal_alpha[i];
                    break;
                }
                case 4:   /* greyscale + alpha */
                    r = g = b = to_byte(sample_at(row, depth, s + 0), depth);
                    a = to_byte(sample_at(row, depth, s + 1), depth);
                    break;
                default:  /* RGBA */
                    r = to_byte(sample_at(row, depth, s + 0), depth);
                    g = to_byte(sample_at(row, depth, s + 1), depth);
                    b = to_byte(sample_at(row, depth, s + 2), depth);
                    a = to_byte(sample_at(row, depth, s + 3), depth);
                    break;
            }
            out[x * 4 + 0] = r;
            out[x * 4 + 1] = g;
            out[x * 4 + 2] = b;
            out[x * 4 + 3] = a;
        }
    }

    *rgba_out = rgba;
    *w_out    = (int)width;
    *h_out    = (int)height;
    rgba      = NULL;      /* the caller's now */
    rc        = APNG_OK;

done:
    free(idat);
    free(raw);
    free(rgba);
    return rc;
}
