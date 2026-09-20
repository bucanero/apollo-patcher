/* See sfo.h. */
#include <string.h>

#include "sfo.h"

#define SFO_MAGIC    0x46535000u   /* "\0PSF", little-endian */
#define SFO_VERSION  0x00000101u
#define SFO_HDR_LEN  0x14
#define SFO_IDX_LEN  0x10

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

int asfo_valid(const uint8_t *sfo, size_t sfo_len)
{
    uint32_t keys, data, count;

    if (!sfo || sfo_len < SFO_HDR_LEN)
        return ASFO_ERR_FORMAT;
    if (le32(sfo) != SFO_MAGIC || le32(sfo + 4) != SFO_VERSION)
        return ASFO_ERR_FORMAT;

    keys  = le32(sfo + 0x08);
    data  = le32(sfo + 0x0C);
    count = le32(sfo + 0x10);

    if (keys > sfo_len || data > sfo_len)
        return ASFO_ERR_FORMAT;
    /* Written as a division so a huge count cannot overflow the multiply. */
    if (count > (sfo_len - SFO_HDR_LEN) / SFO_IDX_LEN)
        return ASFO_ERR_FORMAT;
    return ASFO_OK;
}

int asfo_find(const uint8_t *sfo, size_t sfo_len, const char *key,
              size_t *off, uint32_t *used, uint32_t *max, unsigned *fmt)
{
    uint32_t keys, data, count, i;

    if (!key || asfo_valid(sfo, sfo_len) != ASFO_OK)
        return ASFO_ERR_FORMAT;

    keys  = le32(sfo + 0x08);
    data  = le32(sfo + 0x0C);
    count = le32(sfo + 0x10);

    for (i = 0; i < count; i++) {
        const uint8_t *e = sfo + SFO_HDR_LEN + SFO_IDX_LEN * (size_t)i;
        uint32_t key_off  = le16(e);
        uint32_t val_fmt  = le16(e + 0x02);
        uint32_t val_len  = le32(e + 0x04);
        uint32_t val_max  = le32(e + 0x08);
        uint32_t val_off  = le32(e + 0x0C);
        const char *k;
        size_t room;

        if ((size_t)keys + key_off >= sfo_len)
            continue;
        /* The name has to actually terminate inside the key table, or strcmp
         * would run off the end of the buffer. */
        k    = (const char *)sfo + keys + key_off;
        room = sfo_len - (keys + key_off);
        if (!memchr(k, '\0', room) || strcmp(k, key) != 0)
            continue;

        /* A key that matches but whose value does not fit condemns the whole
         * file rather than being skipped: a reader that carried on would be
         * reporting "no such key" about a file that has one and is corrupt.
         *
         * Both subtractions are safe: data <= sfo_len was checked above, and
         * the first test leaves data + val_off <= sfo_len. */
        if (val_len > val_max)
            return ASFO_ERR_FORMAT;
        if (val_off > sfo_len - data || val_max > sfo_len - data - val_off)
            return ASFO_ERR_FORMAT;

        if (off)  *off  = (size_t)data + val_off;
        if (used) *used = val_len;
        if (max)  *max  = val_max;
        if (fmt)  *fmt  = val_fmt;
        return ASFO_OK;
    }
    return ASFO_ERR_MISSING;
}

int asfo_string(const uint8_t *sfo, size_t sfo_len, const char *key,
                char *out, size_t out_len)
{
    size_t   off, n;
    uint32_t used;
    int      rc;

    if (!out || !out_len)
        return ASFO_ERR_SPACE;
    out[0] = '\0';

    rc = asfo_find(sfo, sfo_len, key, &off, &used, NULL, NULL);
    if (rc != ASFO_OK)
        return rc;

    /* `used` counts the terminator when there is one, so stop at the first
     * NUL and let the length cap the rest. */
    n = used;
    {
        const void *nul = memchr(sfo + off, '\0', n);
        if (nul) n = (size_t)((const uint8_t *)nul - (sfo + off));
    }
    if (n >= out_len)
        return ASFO_ERR_SPACE;

    memcpy(out, sfo + off, n);
    out[n] = '\0';
    return ASFO_OK;
}

int asfo_blob(const uint8_t *sfo, size_t sfo_len, const char *key,
              size_t at, uint8_t *out, size_t len)
{
    size_t   off;
    uint32_t used;
    int      rc;

    if (!out)
        return ASFO_ERR_SPACE;

    rc = asfo_find(sfo, sfo_len, key, &off, &used, NULL, NULL);
    if (rc != ASFO_OK)
        return rc;
    if (at > used || len > (size_t)used - at)
        return ASFO_ERR_SPACE;

    memcpy(out, sfo + off + at, len);
    return ASFO_OK;
}

int asfo_u32(const uint8_t *sfo, size_t sfo_len, const char *key, uint32_t *out)
{
    size_t   off;
    uint32_t used;
    int      rc = asfo_find(sfo, sfo_len, key, &off, &used, NULL, NULL);

    if (rc != ASFO_OK)
        return rc;
    if (used != 4)
        return ASFO_ERR_FORMAT;
    if (out)
        *out = le32(sfo + off);
    return ASFO_OK;
}
