/*
 * shiftjis - turning a PS1 or PS2 save title into something displayable.
 *
 * Both consoles stored the name the save list showed in Shift-JIS, and it
 * reaches this repo inside a .PSV (see psvcard.h). Nothing else here needs
 * the encoding, so this is deliberately one function wide.
 *
 * WHAT IS ACTUALLY IN THOSE TITLES. Measured over every .PSV in apollo-saves,
 * 2,641 of them with a title: 2,620 are ASCII written in Shift-JIS's
 * FULL-WIDTH forms -- "ＮＣＡＡ　Ｒｏｓｔｅｒ" is the letters N, C, A, A in
 * codepoints U+FF21 and friends -- and only 19 hold real kana or kanji. So
 * the common case is not translation at all, it is folding full-width back to
 * the ASCII it plainly is, which is also what reads better in a list.
 *
 * Both happen here: decode to Unicode, fold the full-width block and the
 * ideographic space back to ASCII, and emit UTF-8.
 *
 * LEAD BYTES. Shift-JIS is a variable-width encoding -- 0x81-0x9F and
 * 0xE0-0xEF introduce a two-byte character, everything else stands alone --
 * and a converter that assumes fixed two-byte pairs desynchronises on any
 * mixed string, turning the rest of the name into garbage. Four titles in the
 * database mix them ("Dati ｄｉ ｇｉｏｃｏ ＧＴ３" opens with four single-byte
 * characters), so the decode below tracks lead bytes rather than stepping two
 * at a time.
 */
#ifndef APOLLO_SHIFTJIS_H
#define APOLLO_SHIFTJIS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert `len` bytes of Shift-JIS into UTF-8, always NUL-terminating `out`.
 *
 * The input need not be NUL-terminated and any NUL inside it ends the string,
 * which is what a fixed-size title field in a save wants.
 *
 * Returns the number of bytes written, not counting the terminator. Output is
 * truncated to fit rather than refused -- a name is for reading, and half a
 * name beats none -- and never split mid-character.
 *
 * A byte that is not valid Shift-JIS is passed through when it is ASCII and
 * replaced with '?' otherwise, so a damaged title degrades into something
 * legible instead of failing.
 */
size_t asjis_to_utf8(const unsigned char *in, size_t len,
                     char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_SHIFTJIS_H */
