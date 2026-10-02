# The corpus

Most of the numbers in these documents — how many saves carry an account, what
a `CATEGORY` may be, which PNG shapes a decoder has to handle — come from one
place: the **[apollo-saves](https://github.com/bucanero/apollo-saves)
database**, checked out beside this repo. At the time of measuring that is
**4,834 archives** across PS1, PS2, PS3, PS4, PSP and Vita, holding **2,648
`PARAM.SFO` files**, **2,566 identified saves**, **2,647 `.PSV` containers**
and **5,047 PNGs**.

Two things make it usable as a test corpus rather than a pile of zips:

- The saves are **real**, written by real consoles for real games, so they
  carry the cases nobody thinks to write a fixture for — a zero-byte
  `param.sfo`, a 14-byte file beginning `LOCA`, an icon whose `IDAT` fails its
  own CRC, 41 `.png` files that are encrypted Vita thumbnails, macOS
  `__MACOSX/._ICON0.PNG` stubs, PS3 saves filed under a Vita title, 80 DLC
  folders shaped exactly like PS4 saves.
- It is **big enough for a claim to fail**. Several claims in these docs did:
  "all real icons are non-interlaced" (12 are not), "the two PS3 account fields
  always agree" (42 do not), "a `.PSV`'s files are contiguous" (two leave a
  gap) and "its `displaySize` is the sum of them" (nine are not). All had been
  measured honestly against a smaller sample and were simply wrong at scale.

Reading it needs one thing beyond Python's `zipfile`: two archives use
**Deflate64**, which it does not implement, so any sweep should fall back to
`unzip` for those. With that in place nothing in the database fails to open.

A corpus this size also punishes a sloppy audit. Checking a PNG's chunk walk
before its image data reports five files as truncated when their pixels are
perfectly good and only the `IEND` marker is mangled; and 544 files carry bytes
after `IEND`, which a conformant decoder ignores. Measure the thing that
matters — can the image be recovered — before the thing that does not.

## Pointing something at it

Nothing here is checked in, and nothing in the build depends on it — it is a
measuring stick, not a dependency.

| | what it walks |
|---|---|
| `apollo_save_test --scan DIR` | every save under a tree: console, title ID, game |
| `apollo_save_test --accounts DIR` | the account each save names, instead of its title |
| `apollo_save_test --icon FILE` | one save icon, decoded — size and checksum |
| `apollo_ps3_test --corpus DIR PS3/games.conf` | every PS3 hash a console wrote, against one recomputed here |
| `apollo_psv_test --corpus DIR` | every `.PSV`: parse, verify, rebuild byte for byte |
| `apollo_psv_test --patch DIR` | ...and the whole extract / patch / re-sign chain |
| `apollo_patcher_gui --scan DIR` | the save list exactly as the app would show it |

The two `--corpus` modes and `--accounts` exist so a whole tree can be diffed
against an independent reader written from the format spec, which is how the
claims above were tested rather than assumed.
