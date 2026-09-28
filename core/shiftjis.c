#include <string.h>

#include "shiftjis.h"
#include "shiftjis_table.h"   /* the 157KB table, included here and nowhere else */

/*
 * The table is indexed the way the original conversion does it: a single-byte
 * character indexes straight by its own value, and a two-byte one by its lead
 * byte's low nibble plus a per-range base. Each entry is two bytes of
 * big-endian UTF-16.
 *
 * Returns the Unicode codepoint, or 0 when the pair is outside the table.
 */
static unsigned sjis_lookup(unsigned char lead, unsigned char trail, int two_byte)
{
    size_t off;

    if (!two_byte) {
        off = lead;
    } else {
        switch (lead >> 4) {
        case 0x8: off = 0x0100; break;
        case 0x9: off = 0x1100; break;
        case 0xE: off = 0x2100; break;
        default:  return 0;
        }
        off += (size_t)(lead & 0x0F) << 8;
        off += trail;
    }

    off <<= 1;
    if (off + 1 >= sizeof(shiftJIS_convTable))
        return 0;

    return ((unsigned)shiftJIS_convTable[off] << 8) | shiftJIS_convTable[off + 1];
}

/*
 * Full-width back to ASCII.
 *
 * U+FF01-FF5E is the ASCII range shifted up by 0xFEE0 -- the "ＮＣＡＡ" case,
 * which is 2,620 of the 2,641 real titles -- and U+3000 is the ideographic
 * space. Folding them is what makes a save list readable; everything else is
 * left exactly as the console wrote it.
 */
static unsigned fold_fullwidth(unsigned cp)
{
    if (cp >= 0xFF01 && cp <= 0xFF5E) return cp - 0xFEE0;
    if (cp == 0x3000)                 return ' ';
    return cp;
}

/* Append one codepoint as UTF-8 if the whole sequence fits. Returns how many
 * bytes it took, or 0 when it would not fit -- the caller stops there rather
 * than emitting half a character. */
static size_t put_utf8(unsigned cp, char *out, size_t room)
{
    if (cp < 0x80) {
        if (room < 1) return 0;
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (room < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (room < 3) return 0;
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

size_t asjis_to_utf8(const unsigned char *in, size_t len,
                     char *out, size_t out_len)
{
    size_t i = 0, w = 0;

    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!in) return 0;

    while (i < len) {
        unsigned char lead = in[i];
        unsigned cp;
        int two_byte;
        size_t n;

        if (lead == 0x00) break;   /* a fixed-size field ends at its NUL */

        /* 0x81-0x9F and 0xE0-0xEF introduce a two-byte character. A lead byte
         * with nothing after it is a truncated title, not a character. */
        two_byte = (lead >= 0x81 && lead <= 0x9F) || (lead >= 0xE0 && lead <= 0xEF);
        if (two_byte && i + 1 >= len) break;

        cp = sjis_lookup(lead, two_byte ? in[i + 1] : 0, two_byte);
        if (!cp) {
            /* Not in the table. ASCII is meaningful as itself; anything else
             * would be mojibake, so say so with a character that cannot be
             * mistaken for content. */
            cp = (!two_byte && lead < 0x80) ? lead : '?';
        }

        cp = fold_fullwidth(cp);

        n = put_utf8(cp, out + w, out_len - 1 - w);
        if (!n) break;
        w += n;
        i += two_byte ? 2 : 1;
    }

    out[w] = '\0';
    return w;
}
