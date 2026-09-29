# Save icons

Four of the six consoles ship the picture their own save list showed as a PNG
sitting beside the data. Two of them shipped no picture at all, so showing one
means decoding a palette or rendering a model.

| | where the icon is | what it takes |
|---|---|---|
| PSP, PS3 | `ICON0.PNG` beside the data files | decode |
| PS4, Vita | `sce_sys/icon0.png` | decode |
| PS1 | sixteen colours inside the save's own first block | decode a palette |
| PS2 | a textured **3D model**, one to three `.ico` files | render it |

## A PNG beside the data

The picture the console's own save list shows — `ICON0.PNG` beside the data
files on a PSP or PS3, `sce_sys/icon0.png` on a PS4 or Vita — appears beside
the game's name on the patcher screen, and in the hover panel on the list.
Both spellings of each are tried, because the case is per console and only
some filesystems care: a save copied to a Mac and then opened on Linux is the
one that would otherwise lose it.

It is decoded when the selection changes, not during the scan. A folder of 176
saves is 176 PNGs, and uploading all of them would be 40MB of texture for a
list that shows one at a time; re-decoding a 320x176 image on each click costs
a fraction of a millisecond.

### Decoding it here rather than vendoring a library

`core/png.c` does the decoding rather than a vendored library, because the
job is small and the input is narrow. Measured over **every `.png` in the
[apollo-saves](https://github.com/bucanero/apollo-saves) database** — all
5,560 of them, entry art and everything inside the archives, across all six
consoles:

| | files | |
|---|---|---|
| 8-bit RGB | 4,316 | |
| 8-bit RGBA | 940 | |
| 8-bit palette | 237 | |
| 8-bit RGB, **interlaced** | 12 | refused |
| not a PNG at all | 55 | |

Every real PNG among them is **8 bits per channel** — not one 16-bit, 1-, 2-
or 4-bit file in 5,505 — so the whole non-interlaced format is handled anyway
(palettes, greyscale, 1/2/4/16-bit, `tRNS`), each a few lines. Adam7
interlacing is refused outright, being the one thing in the format that would
double the size of that file.

**5,492 of the 5,560 decode.** The 68 refused break down as:

| | files | |
|---|---|---|
| not PNG data | 41 | Vita thumbnails archived without decrypting — entropy 8.00 |
| macOS AppleDouble stubs | 13 | `__MACOSX/._ICON0.PNG`, not icons at all |
| interlaced | 12 | valid; see above |
| `IDAT` fails its CRC and will not inflate | 1 | `PS3/NPUB30720/00000001.zip`'s copy |
| zero-byte | 1 | |

So **three are genuinely lost images** and the rest are either not pictures or
not supported. An independent decoder refuses the same ones. Six are an
`ICON0.PNG` the app would actually show — which is why a failed icon is a
caption and not an error: the row still lists, and says *"(the icon is …)"*.

Two near-misses worth recording, because both looked worse than they were.
Five files have a complete, CRC-clean `IDAT` and a mangled 12-byte tail
reading `HEND` rather than `IEND`; they decode everywhere, since `IEND` carries
no pixels. And 544 have bytes appended *after* `IEND`, which is what a
conformant decoder is supposed to ignore. Neither is damage, and an audit that
checks the chunk walk before the image data will report both as truncation.

The inflating is done by zlib, which the engine already links.

### Padding to a power of two

The texture is **padded to a power of two** and drawn with UVs that cut the
padding back off. OpenGL 1.1 is the floor this app targets — it is exactly what
Microsoft's software renderer offers on a GPU-less or Remote Desktop host — and
1.1 takes only power-of-two textures. No save icon is one: they are 320x176,
228x128, 144x80. Without the padding the icon would silently fail to appear on
precisely the machines least able to explain why.

## PS1 — sixteen colours in the save's own block

A PS1 save's icon is not a file. It is packed into the save's own first block,
four bits to a pixel against a sixteen-entry palette, 16x16, with up to three
animation frames — a frame count byte, the palette at byte 96, then 128 bytes
per frame. `core/mcicon.c` reads it.

Palette entry zero is **transparent by the console's own rule**: an entry whose
colour bits and whose translucency bit are all clear means "nothing here"
rather than black, and that rule is what gives these icons their shape. Drawing
it as black would put a square box around every one of them.

**They do not animate here**, though the format allows it: not one of the six
PS1 saves in the database uses more than a single frame, so there is nothing to
play.

## PS2 — a textured 3D model

A PS2 save's icon is one to three `.ico` files named by `icon.sys`, which also
supplies the three directional lights and the ambient term. `core/ps2/` is
apollo-ps4's parser and software rasteriser, ported — so it needs no GL context
of its own and works in the wasm build too.

### It animates

It is a model with up to eight morph targets, and the app plays them: the pose
is re-rendered every frame, interpolating between shapes on the loop the file
itself states. The morph is positions only, matching
[ps2vmc-tool](https://github.com/bucanero/ps2vmc-tool)'s WebGL renderer so the
two agree about what a save looks like moving.

Most icons do not move — 1,898 of the 2,345 in the save database carry a single
shape — so `animated` is checked first and the common case costs nothing.

### Hovering shows it bigger

Hovering the icon on the patcher screen shows it bigger, still animating, at
256px against a transparent background. That is the closest this gets to
what the save looked like on a television, and it is rendered only while the
pointer is actually on it.

While the panel is up the **small copy holds its pose**. Both are the same
model, and animating both means rasterising it twice a frame — which a Windows
7 machine on an Athlon XP 2400+ notices. The one being pointed at gets the
animation.

### What it costs

All of it happens on the CPU and arrives as an ordinary texture, so it needs no
depth buffer, no shaders and no second GL context — which matters, because the
floor here is OpenGL 1.1 and one supported configuration is a software
rasteriser over Remote Desktop. Both render sizes are powers of two for the
same reason.

It is affordable, and the two sizes are chosen to cost the same: 128px at 4x
supersampling and 256px at 2x both rasterise 512x512, which measures about
1.3ms against a 16ms frame. The renderer is **fill-rate bound**, so that
internal size is the cost — vertex count barely registers, and a 1,194-vertex model can
be cheaper than a 552-vertex one that covers more of the frame. 256px at 4x was
tried and is 3.5x dearer for a difference visible only in a side-by-side.

### Whole, or not at all

Either the 3D model reads, or — when
its geometry does not fit its file but the texture survived intact — that
texture is shown flat. There is no third option, and in particular no partial
model.

That was tried, and it was the wrong answer. The one damaged icon in the save
database declares 1,770 vertices in a file with room for 1,159; drawing the
two thirds that survive produces a clean, confident silhouette that tells the
person looking at it that nothing is the matter. Something truncated that file,
and it was under no obligation to stop at the icon. So the app says so instead,
in colour, on the patcher screen and in the list's hover panel: *this save's
icon is damaged, and the save data may be too*.

Measured over the 2,647 real containers: **2,563 icons render** as models, 69
saves carry none at all, and **15 are damaged** — all the same Action Replay
MAX file. None fall back to a flat texture, because that file's texture is in
the missing third; the fallback is covered by a built fixture in
`core/test_psv.c` rather than by anything real, which is worth knowing.
