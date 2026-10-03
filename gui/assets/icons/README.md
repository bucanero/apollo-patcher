# Code-type icons

Three small pictures for the patcher screen's type column — one per
interpreter a `.savepatch` code can run under. Drop them here and re-run
`cmake`; `tools/make-type-icons.py` bakes them into `gui/src/type_icons.h` and
the app picks them up on the next build.

| file | code type | shown for |
|---|---|---|
| `type-savewizard.png` | `APOLLO_CODE_SAVEWIZARD` | Save Wizard codes |
| `type-python.png` | `APOLLO_CODE_PYTHON` | Python scripts |
| `type-bsd.png` | `APOLLO_CODE_BSD` | BSD (Bruteforce Save Data) scripts |

**Nothing here is required.** With no icons the column falls back to the
coloured `SW` / `PY` / `BSD` text it always had, and the build says so at
configure time rather than failing.

## What the files have to be

- **PNG, 8 bits a channel, RGB or RGBA, not interlaced.** That is the whole
  subset the baker reads, and it refuses anything else by name rather than
  guessing. It is also every PNG that turned up in a sweep of 5,560 real ones,
  so it is not a narrow bet.
- **Square**, and **at least 40x40**. The column draws at one line of text,
  which is 20px, and the art is stored at 40 so a HiDPI display has real
  pixels rather than a smeared upscale. 128x128 is a good size to author at.
- **Transparent background.** These sit on the table's own background, which
  is not a fixed colour.

Scaling happens once at build time, with a box filter over premultiplied
alpha — the floor here is OpenGL 1.1, which has no automatic mipmaps, so a
large texture handed to `GL_LINEAR` and drawn at 20px shimmers. Premultiplying
is what stops a hard alpha edge picking up a dark halo.
