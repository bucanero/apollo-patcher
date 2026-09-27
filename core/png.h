/*
 * png - decoding a save's icon.
 *
 * Every console writes one next to the save: ICON0.PNG for a PSP or PS3,
 * sce_sys/icon0.png for a PS4 or Vita. It is what the console's own save list
 * shows, and the quickest way for somebody to recognise a save that six other
 * folders are named almost the same as.
 *
 * This exists rather than a vendored decoder because the job is small and the
 * input is narrow. Measured over every .png in the apollo-saves database --
 * all 5,560 of them, entry art and everything inside the save archives,
 * across all six consoles -- every real PNG is 8 bits per channel, not one
 * 16-bit or sub-8-bit file among 5,505, and all but 12 are non-interlaced RGB,
 * RGBA or palette. So the whole non-interlaced format is supported (palettes,
 * greyscale, 1/2/4/16-bit, tRNS), each a few lines.
 *
 * Adam7 interlacing is refused: it is the one thing in the format that would
 * double the size of this file, for 12 files in 5,560.
 *
 * 5,492 of the 5,560 decode. Of the 68 refused, 55 are not PNG data at all
 * (Vita thumbnails archived without decrypting, macOS AppleDouble stubs, one
 * zero-byte file), 12 are the interlaced ones, and exactly ONE is a damaged
 * image -- an IDAT that fails its own CRC and will not inflate. An
 * independent decoder refuses the same set.
 *
 * The caller is expected to cope either way: the desktop app says
 * "(the icon is ...)" and shows the row regardless.
 *
 * Output is always RGBA8, top row first, which is what a texture wants.
 *
 * Bounds. A PNG's every length and offset is read out of the file, and a save
 * folder is something a person is handed. Chunk lengths are checked against
 * what is left of the buffer, the pixel buffer is sized from IHDR with the
 * multiplications checked for overflow, and the decoded rows are filled from
 * the inflate output rather than trusted to be there.
 */
#ifndef APOLLO_PNG_H
#define APOLLO_PNG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    APNG_OK            =  0,
    APNG_ERR_FORMAT    = -1,  /* not a PNG, or one that does not parse     */
    APNG_ERR_SUPPORT   = -2,  /* a PNG this does not do -- interlaced      */
    APNG_ERR_MEMORY    = -3,
    APNG_ERR_SIZE      = -4   /* larger than APNG_MAX_DIM                  */
};

/* An icon is at most 320x176 on any console here. The cap is far above that
 * and exists only so a file claiming 65535x65535 cannot ask for 17GB. */
#define APNG_MAX_DIM 8192

/*
 * Decode to RGBA8. On APNG_OK, *rgba is a buffer of (*w * *h * 4) bytes that
 * the caller frees with apng_free(); on anything else it is left NULL.
 */
int apng_decode(const uint8_t *data, size_t len, uint8_t **rgba, int *w, int *h);

void apng_free(uint8_t *rgba);

/* For a message: "not a PNG", "an interlaced PNG", ... */
const char *apng_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_PNG_H */
