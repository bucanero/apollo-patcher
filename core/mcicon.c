/* See mcicon.h. */
#include <stdlib.h>
#include <string.h>

#include "mcicon.h"
#include "ps2icon.h"
#include "ps2render.h"

/* ---- PS1 ----------------------------------------------------------------
 *
 * The icon lives in the save's own first block, at fixed offsets:
 *
 *   byte 2         0x11, 0x12 or 0x13 -- one, two or three frames
 *   bytes 96..127  sixteen palette entries, BGR555 with a translucency bit
 *   byte 128 on    the frames, 128 bytes each, two pixels per byte
 */
#define PS1_FRAMES_OFF   2
#define PS1_PALETTE_OFF  96
#define PS1_PALETTE_LEN  32
#define PS1_FRAME_OFF    128
#define PS1_FRAME_LEN    128

int amci_ps1_frames(const uint8_t *block, size_t len)
{
    if (!block || len <= PS1_FRAMES_OFF)
        return 0;

    switch (block[PS1_FRAMES_OFF]) {
    case 0x11: return 1;
    case 0x12: return 2;
    case 0x13: return 3;
    /* Anything else is a save with no icon -- a cleared one, usually. Not an
     * error: it is simply a save that will not be showing a picture. */
    default:   return 0;
    }
}

int amci_ps1_frame(const uint8_t *block, size_t len, int frame, uint8_t *out)
{
    uint32_t palette[16];
    const uint8_t *pal, *src;
    size_t need;
    int i;

    if (!block || !out || frame < 0 || frame >= AMCI_PS1_FRAMES)
        return AMCI_ERR_DATA;
    if (frame >= amci_ps1_frames(block, len))
        return AMCI_ERR_NONE;

    need = (size_t)PS1_FRAME_OFF + (size_t)(frame + 1) * PS1_FRAME_LEN;
    if (len < need)
        return AMCI_ERR_DATA;

    /*
     * BGR555, five bits a channel, low byte first, with the top bit of the
     * high byte meaning translucent.
     *
     * An entry whose colour bits AND that flag are all clear is not black --
     * it is nothing, and drawing it as black would put a square box around
     * every icon. That rule is the console's, and it is what gives these
     * icons their shape.
     */
    pal = block + PS1_PALETTE_OFF;
    for (i = 0; i < 16; i++) {
        const uint8_t lo = pal[i * 2], hi = pal[i * 2 + 1];
        const uint8_t r = (uint8_t)((lo & 0x1F) << 3);
        const uint8_t g = (uint8_t)(((hi & 0x03) << 6) | ((lo & 0xE0) >> 2));
        const uint8_t b = (uint8_t)((hi & 0x7C) << 1);

        if (((unsigned)r | g | b | (hi & 0x80)) == 0)
            palette[i] = 0;                       /* transparent */
        else
            palette[i] = ((uint32_t)r)
                       | ((uint32_t)g << 8)
                       | ((uint32_t)b << 16)
                       | 0xFF000000u;
    }

    /* Two pixels to a byte, low nibble first. */
    src = block + PS1_FRAME_OFF + (size_t)frame * PS1_FRAME_LEN;
    for (i = 0; i < AMCI_PS1_SIZE * AMCI_PS1_SIZE; i += 2) {
        const uint8_t byte = src[i / 2];
        const uint32_t a = palette[byte & 0x0F];
        const uint32_t b = palette[byte >> 4];

        memcpy(out + (size_t)i * 4, &a, 4);
        memcpy(out + (size_t)(i + 1) * 4, &b, 4);
    }
    return AMCI_OK;
}

/* ---- PS2 ---------------------------------------------------------------- */

int amci_ps2_render(const uint8_t *ico, size_t ico_len,
                    const uint8_t *sys, size_t sys_len,
                    int size, uint8_t **out)
{
    ps2icon_t icon;
    ps2_IconSys_t sys_copy;
    const ps2_IconSys_t *lighting = NULL;
    uint8_t *rgba = NULL;
    int rc;

    if (!ico || !out || size <= 0)
        return AMCI_ERR_DATA;
    *out = NULL;

    memset(&icon, 0, sizeof icon);
    if (ps2icon_parse(ico, ico_len, &icon) != 0) {
        ps2icon_free(&icon);
        return AMCI_ERR_DATA;
    }

    /*
     * Copied rather than pointed at: the caller's icon.sys sits inside a .PSV
     * buffer at whatever offset the container put it, which need not be
     * aligned for this structure. The renderer reads it as one.
     */
    if (sys && sys_len >= sizeof sys_copy) {
        memcpy(&sys_copy, sys, sizeof sys_copy);
        lighting = &sys_copy;
    }

    /*
     * Supersampled 4x. These models are a few hundred triangles and the whole
     * render is well under a millisecond, so the cost is nothing and the
     * difference on a 16-pixel-wide list icon is the whole picture.
     */
    rc = ps2icon_render(&icon, lighting, size, 4,
                        PS2RENDER_BG_TRANSPARENT, &rgba);
    ps2icon_free(&icon);

    if (rc != 0 || !rgba) {
        free(rgba);
        return AMCI_ERR_MEMORY;
    }

    *out = rgba;
    return AMCI_OK;
}

void amci_free(uint8_t *rgba)
{
    free(rgba);
}

const char *amci_strerror(int err)
{
    switch (err) {
    case AMCI_OK:         return "ok";
    case AMCI_ERR_DATA:   return "not an icon this understands";
    case AMCI_ERR_NONE:   return "the save carries no icon";
    case AMCI_ERR_MEMORY: return "out of memory";
    default:              return "unknown error";
    }
}
