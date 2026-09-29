# Text: the fonts, and the encodings underneath them

Game names are whatever the game wrote, and they are not ASCII. This is the
font that draws them, the second font the hex editor needs instead, and the
Shift-JIS decoder that a PS2 save's own title arrives in.

## The UI font

Game names are whatever the game wrote, and they are not ASCII. Measured over
every name and slot label in the [apollo-saves](https://github.com/bucanero/apollo-saves)
database — **6,243 strings** out of 2,648 `PARAM.SFO` files — **1,616 of them
(26%) contain a character outside ASCII**: 877 distinct codepoints, 14,768
occurrences. The distribution is not what a Latin-1 font would suggest:

| | occurrences |
|---|---|
| Kana | **7,627** |
| CJK ideographs | 3,066 |
| Fullwidth forms | 1,615 |
| Ideographic space and CJK punctuation | 1,246 |
| Latin-1 supplement (`®`, accented Latin) | 429 |
| `™` and other letterlike | 284 |
| Cyrillic, Greek, symbols (`★ ♪ ♂ ⚙ ↑`), Arabic | ~500 |

Japanese is not a footnote here, it is the bulk of it — a quarter of every
name in the database needs more than Latin-1.

ImGui's built-in ProggyClean covers U+0020–00FF and nothing else, so `®` drew
fine and `™` drew as `?`. The app therefore **ships a font**:

1. **`$APOLLO_FONT`** — a `.ttf` or `.otf` named outright, for anyone who
   wants a different face.
2. **`NotoSansJP-Medium.otf` beside the app** — `Contents/Resources` for a
   macOS `.app`, next to the executable elsewhere. The same two places the
   patch database is looked for, and what happens in practice.
3. **ImGui's own**, as a safety net that should never fire. Reaching it means
   the copy beside the app is missing, so the log says so rather than quietly
   looking wrong.

There is deliberately **no system-font tier**. Falling back to a per-OS list
(Arial, Segoe UI, DejaVu) would work, but the app would then look different on
every machine — and since every column width comes from `CalcTextSize`, even
its proportions would move. It would also hide a broken build behind something
that looked almost right. One vendored font is one appearance and one tested
path.

Asked of the built atlas, one codepoint at a time, Noto Sans JP with the ranges
the app bakes has a glyph for **827 of the 877** — and weighted by how often
each actually turns up, **14,596 of 14,768 characters, or 98.84%**.

The 50 it misses are worth naming, because each is a deliberate limit rather
than an oversight:

| missing | codepoints | occurrences | why |
|---|---|---|---|
| rare CJK ideographs | 28 | 109 | outside ImGui's common-use Japanese set; the full range would balloon the atlas |
| Arabic | 11 | 12 | see below |
| enclosed alphanumerics (`ⓒ ⑴`), geometric shapes (`■`) | 4 | 21 | ranges not baked — two lines would close it |
| an emoji, a variation selector, four others | 7 | 30 | not UI-font material |

**Arabic is left out on purpose.** ImGui does no bidirectional layout and no
contextual shaping, so baking the glyphs would draw Arabic left-to-right in
isolated letterforms — confidently wrong rather than visibly missing. Twelve
characters in the whole database is not worth a wrong answer.

The Japanese range is requested whichever font is found, because asking costs
nothing when the font has no such glyphs — Arial comes out at the same 1015
glyphs and 0.2MB either way. So there is no need to ask a font what it
contains before asking it for something.

The font is **vendored** at `gui/assets/fonts/`, with its SIL Open Font
License beside it (redistribution requires it). That is a deliberate exception
to this repo's habit of finding siblings rather than copying them: `apollo-lib`
and `apollo-patches` are code and data you want to update independently, where
this is a frozen asset the app needs in order to render correctly. It is the
same file `apollo-psp` ships.

`-DAPOLLO_FONT_FILE=` ships a different face instead. A build with no font
still succeeds — with a CMake warning, because the checkout is then incomplete
— and CI treats a missing font in the artifact as an error.

Text is drawn at **20px** rather than ProggyClean's 13: a bitmap font at its
design size is crisp where an outline font at 13 is muddy, and this app is read
more than it is clicked. Every column width comes from `CalcTextSize`, so the
layout follows the size rather than having to be retuned for it.

Size is what drives the atlas, not the glyph count, and it is a step rather
than a slope: the same ~4000 glyphs fit 1024×1024 up to and including 16px and
need 1024×2048 from 17 — 1MB against 2MB of alpha texture. Having paid that,
20px costs no more than 18.

## The second font, for the hex editor

Noto is the right font for names and the wrong one for a hex dump. The memory
editor sizes its entire grid from `CalcTextSize("F").x` and then draws every
other character in that one cell, which only holds if the advances match. In
Noto at 20px they do not:

| | advance |
|---|---|
| `0`–`9` | 7.87 — tabular, so decimal lines up |
| `A`–`F` | 7.82 (`F`) to 9.65 (`D`) |
| `i` / `W` | 3.96 / 12.35 |

So hex columns drift and the ASCII pane stops lining up with the bytes above
it. The fix is a fixed-pitch font, and the one already in the family is the
**10×20 console raster font** that `apollo-ps3`, `apollo-ps4` and
`apollo-vita` draw with, from
[idispatch/raster-fonts](https://github.com/idispatch/raster-fonts).

It was picked over a monospace outline face (Cousine, which ImGui ships, comes
out at 10.59×20 — near enough the same box) for three reasons. A bitmap is
drawn rather than rasterised, so it is crisp at exactly the small dense sizes
where antialiasing does the most damage. Its advance is an integer 10, so 16
columns of hex land on integer pixels instead of accumulating a fraction. And
the app already renders at a 20px line height, so a 20px cell sits in the same
rhythm as everything around it.

`tools/make-font.py` converts the upstream file — 180KB of commented C
for 10KB of bitmap — into `gui/src/font10x20.h`, keeping only the 95 printable
ASCII glyphs at 3,800 bytes. The parser matches each byte pair together with
the bit-pattern comment beside it and fails if the two disagree, so a parse
that drifted would have to drift in both at once. The codes above `0x7E` are
**deliberately dropped**: they are CP437 box drawing, which is not what
Unicode U+0080–00FF means, so registering them would draw the wrong character
rather than none.

ImGui has no notion of a bitmap font, so `load_mono_font()` goes the long way
round: a font whose own glyph range is a single character nobody draws
(U+0020, from ProggyClean), with the 95 glyphs added as **custom atlas
rectangles** and their pixels written in by hand after packing. Custom rects
are registered last and the lookup table takes the last glyph for a codepoint,
so nothing of ProggyClean shows. A custom-rect glyph sits at its
`GlyphOffset`, which defaults to `(0,0)` — the top-left of the text line — so
a full-height 20px cell lands on a 20px line with no nudging. `Build()` is
called explicitly rather than left to the backend, because the pixels have to
be written before anything reads the texture; the GL backend then asks for
RGBA32, which converts from that same alpha buffer and only rebuilds if it is
absent.

The whole thing costs 95 glyphs, 19,000 atlas pixels and no change to the
atlas size, which stays 1024×2048.

## Where the fixed-pitch font is used, and where it isn't

Being ASCII-only, the font cannot simply replace Noto everywhere a fixed pitch
would help. The savepatch code viewers and the raw patch view have the same
column drift as the hex editor, but of the 2,247 files in the patch database
**246 are not valid UTF-8 at all** (CP1252 quotes and dashes, which ImGui
already draws as `?` whatever the font) and another 82 carry curly quotes,
accented Latin, `™`, katakana, CJK or fullwidth forms. A blanket switch would
trade a cosmetic problem for rows of `?`.

So the choice is made per piece of text, by `is_plain_ascii()` — every byte
printable ASCII, with tab, newline and carriage return allowed through as
layout. Measured over the database:

| | fixed pitch | falls back to Noto |
|---|---|---|
| code bodies | **80,092 of 80,094** (100.0%) | 2 |
| whole patch files | 1,919 of 2,247 (85.4%) | 328 |

The split is not a coincidence: non-ASCII in a `.savepatch` lives almost
entirely in **names and author comments**, which the code viewer does not
show. So in practice every code body gets the fixed-pitch font, the raw view
gets it for six files in seven, and nothing anywhere degrades to `?`.

The answer is **cached**, not asked per frame — a patch file runs to 430KB and
a single body to tens of KB. `AppState::CodeEdit::set()` recomputes it on
every assignment, and an edit re-asks, because pasted text can bring in a
character the font has no glyph for. The push and the pop read one local so
the font stack stays balanced even when an edit flips the answer mid-widget.

`is_plain_ascii()` deliberately asks about the **text and not the font**, and
never looks at `g_mono`. An earlier version checked the font there and got it
wrong in a way worth recording: a patch named on the command line, dropped on
the Dock or opened from Finder is loaded by the argument loop, which runs
*before* the window and its fonts exist — so `g_mono` was still null, every
such patch latched to "cannot draw", and the raw view stayed proportional for
the rest of the run. The font is checked where it is pushed instead.

The hex editor's own window is the one place the font is pushed around part of
a window rather than all of it: the path and file name above the grid can hold
anything, so only the grid gets it. All three of these windows also had their
identities fixed while this was going in — see
[One window, one identity](frontends.md#one-window-one-identity).

## Fitting the hex window

The hex window used to open at a hard-coded 700×520 and cut off the right-hand
ASCII pane. Under Noto the editor was laying its grid out on
`CalcTextSize("F").x + 1` = 8.82px cells while a `W` drew 12.35px wide, so the
text spilled past the width the editor thought it needed — 700 looked like
plenty and wasn't.

With a fixed pitch the arithmetic is exact, so the window is now sized from
`MemoryEditor::CalcSizes()` rather than guessed at. Measured, with the address
column growing as the file does:

| file | address digits | width |
|---|---|---|
| 64 B | 2 | 716 |
| 2 KB | 3 | 727 |
| 64 KB | 4 | 738 |
| 4 MB | 6 | 760 |

The width has to be measured **with the fixed-pitch font pushed and before
`Begin()`**, since `SetNextWindowSize` applies to the next window rather than
the current one. Height follows the file, clamped to 8–24 rows so a 64-byte
`PARAM.SFO` does not get the same window as a 4MB save, and both dimensions
are clamped to the viewport.

Refitting happens on `ImGuiCond_FirstUseEver`, plus once more whenever a
**different** file is loaded — `hex_load()` compares the path and sets
`hex_fit`. Reloading the same file from disk deliberately does not refit, so a
size the user chose survives.

## Shift-JIS, which is what a PS1 or PS2 title arrives in

Both consoles stored the name their save list showed in **Shift-JIS**, and it
reaches this repo inside a `.PSV` — a PS2's in `icon.sys` at offset `0xC0`, 68
bytes; a PS1's in the save's own block. Nothing else here needs the encoding,
so `core/shiftjis.c` is deliberately one function wide.

**What is actually in those titles** is not what the encoding suggests.
Measured over every `.PSV` in apollo-saves, 2,641 of them with a title:

| | titles |
|---|---|
| ASCII written in Shift-JIS's **full-width** forms | 2,620 |
| real kana or kanji | 19 |

`ＮＣＡＡ　Ｒｏｓｔｅｒ` is the letters N, C, A, A at U+FF21 and friends. So the
common case is not translation at all — it is folding full-width back to the
ASCII it plainly is, which is also what reads better in a list. The decoder
does both: decode to Unicode, fold the full-width block (U+FF01–FF5E) and the
ideographic space (U+3000) back to ASCII, and emit UTF-8.

**Lead bytes matter.** Shift-JIS is variable width — `0x81`–`0x9F` and
`0xE0`–`0xEF` introduce a two-byte character, everything else stands alone — so
a converter that steps two bytes at a time desynchronises on any mixed string
and turns the rest of the name into garbage. Four titles in the database mix
them (`Dati ｄｉ ｇｉｏｃｏ ＧＴ３` opens with four single-byte characters), which
is why the decoder tracks lead bytes rather than assuming pairs. The reference
implementation this was derived from assumes pairs; those four are what it
would have broken.

The full table is vendored at `core/shiftjis_table.h` — 157KB, `static`, and
included by exactly one translation unit so it is never linked twice. The whole
decoder was cross-checked against Python's own `shift_jis` codec over every
title in the corpus: **2,639 of 2,639 agree**.

### One character is folded that is not full-width

`0x817C` decodes to **U+2212 MINUS SIGN**, and 118 of the 2,641 titles use it
as an ordinary hyphen — *I-Ninja*, *FFX-2*, *Xenosaga EPISODE1-06*. It is
folded to ASCII `-` for the same reason the full-width letters are: it is
punctuation wearing a wide glyph, and apollo-ps4's own conversion table maps
that byte to `-` as well, so this agrees with the console apps rather than
departing from them.

It is also the **only character in all 2,647 container titles the font atlas
does not cover**. U+2212 sits in Mathematical Operators, which neither the
extra ranges nor ImGui's Japanese set includes, so before the fold it drew as
`?`. Folding fixes that without carrying 256 more glyphs for one character.
Every other non-ASCII character these titles use — curly quotes, katakana, a
black star, tortoise-shell brackets — is already in the atlas and is left
alone.
