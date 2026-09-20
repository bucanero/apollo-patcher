/*
 * png - decoding a save's icon.
 *
 * Every console writes one next to the save: ICON0.PNG for a PSP or PS3,
 * sce_sys/icon0.png for a PS4 or Vita. It is what the console's own save list
 * shows, and the quickest way for somebody to recognise a save that six other
 * folders are named almost the same as.
 *
 * This exists rather than a vendored decoder because the job is small and the
 * input is narrow. Across 185 real icons -- the PS3, PSP, PS4 and Vita saves
 * this was checked against -- every single one is 8-bit, non-interlaced, and
 * either RGB or RGBA. The rest of the non-interlaced format is supported
 * anyway (palettes, greyscale, 1/2/4/16-bit, tRNS) because it is a few lines
 * each; Adam7 interlacing is refused, which is the one thing in the format
 * that would double the size of this file.
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
