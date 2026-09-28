/*
 * mcicon - the icon a memory-card save carries, as an image.
 *
 * Every other console in this repo ships its save's icon as a PNG sitting
 * beside the data, which core/png.c decodes and the app draws. Neither the PS1
 * nor the PS2 did anything so convenient:
 *
 *   PS1   16x16, sixteen colours, packed four bits to a pixel in the save's
 *         own first block -- palette at byte 96, then up to three animation
 *         frames of 128 bytes each. There is no file; it is part of the save.
 *   PS2   a textured 3D MODEL, one to three .ico files inside the save's
 *         directory, lit by parameters in icon.sys. There is no picture
 *         anywhere, so showing one means rendering it.
 *
 * This turns both into the same thing the PNG decoder produces -- RGBA8, top
 * row first, ready for a texture -- so a front-end can show a PS1 or PS2 save
 * in its list without knowing any of the above.
 *
 * The PS2 side is core/ps2/ps2icon.c and core/ps2/ps2render.c, ported from
 * apollo-ps4; this is the facade over them and the whole of the PS1 side,
 * which is small enough not to need one.
 *
 * Bounds. Everything here is read out of a save that arrives from wherever
 * saves arrive from, so a block too short for its palette, a frame count that
 * claims more than is there and an .ico whose vertex count does not fit are
 * each refused rather than followed.
 */
#ifndef APOLLO_MCICON_H
#define APOLLO_MCICON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AMCI_OK        =  0,
    AMCI_ERR_DATA  = -1,   /* not an icon this understands                  */
    AMCI_ERR_NONE  = -2,   /* the save carries no icon at all               */
    AMCI_ERR_MEMORY = -3,
    /*
     * The icon is DAMAGED -- it names more data than the file holds, and not
     * even a whole texture could be recovered from it.
     *
     * Distinct from AMCI_ERR_DATA on purpose. A format this does not
     * understand is this code's problem; a file whose own header does not
     * describe it is the SAVE's problem, and the person looking at it wants to
     * know that, because whatever damaged the icon was under no obligation to
     * stop there.
     */
    AMCI_ERR_CORRUPT = -4
};

/* What a PS2 icon turned out to be, for a caller that draws it. */
typedef enum {
    AMCI_PS2_MODEL = 0,   /* the 3D model, lit and posed                    */
    AMCI_PS2_FLAT         /* no usable geometry: a whole texture, drawn flat */
} amci_ps2_kind_t;

#define AMCI_PS1_SIZE   16    /* a PS1 icon is always 16x16          */
#define AMCI_PS1_FRAMES  3    /* ...and never more than three frames */

/*
 * How many animation frames a PS1 save's block holds: 0 to 3.
 *
 * Zero is an ordinary answer, not an error -- a save whose data has been
 * cleared, or one that never had an icon, says so in the same byte.
 */
int amci_ps1_frames(const uint8_t *block, size_t len);

/*
 * Decode one PS1 frame into 16x16 RGBA.
 *
 * `out` must hold AMCI_PS1_SIZE * AMCI_PS1_SIZE * 4 bytes. Palette entry zero
 * is transparent by the console's own rule -- a colour whose every channel and
 * whose translucency bit are clear means "nothing here", which is how these
 * icons get their shape.
 */
int amci_ps1_frame(const uint8_t *block, size_t len, int frame, uint8_t *out);

/*
 * Render a PS2 icon into `size` x `size` RGBA.
 *
 * `ico` is one of the save's .ico files and `sys` its icon.sys, which supplies
 * the three directional lights and the ambient term; `sys` may be NULL, and
 * then the same fallback lighting apollo-ps4's renderer uses is applied.
 * `sys_len` is checked, because an icon.sys shorter than the structure would
 * otherwise be read past.
 *
 * `*out` is malloc'd and belongs to the caller; free it with amci_free().
 * The background is transparent, so the result composites anywhere.
 */
int amci_ps2_render(const uint8_t *ico, size_t ico_len,
                    const uint8_t *sys, size_t sys_len,
                    int size, uint8_t **out);

/*
 * The same, saying which of the two it drew.
 *
 * An icon is either a whole model or, when its geometry does not fit its file,
 * a whole texture drawn flat -- and nothing in between. A partial model is
 * refused rather than drawn: the damaged icon in the save database renders
 * two thirds of its geometry into a perfectly clean silhouette, which tells
 * the person looking at it that everything is fine. It is not.
 *
 * `kind` may be NULL. AMCI_ERR_CORRUPT when neither could be had.
 */
int amci_ps2_render_kind(const uint8_t *ico, size_t ico_len,
                         const uint8_t *sys, size_t sys_len,
                         int size, uint8_t **out, amci_ps2_kind_t *kind);

void amci_free(uint8_t *rgba);

/* A human-readable reason, for the log. Never NULL. */
const char *amci_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_MCICON_H */
