# Save data: what a save is, and the layers under it

Six consoles, and almost nothing in common between them. This is what the code
has to know: how to recognise a save, how to tell which game wrote it, what the
console wrapped around it, and — for the two that wrote no files at all — the
container a save has to arrive in before a computer can see it.

## Recognising one

Finding them is the same question on four of the six consoles and has the same
answer: **a save is a folder with a `PARAM.SFO` in it**. Where that SFO sits is
itself the first half of the identification:

| Found at | Console | Encrypted by the console |
|----------|---------|--------------------------|
| `<save>/PARAM.SFO`, with `SAVEDATA_PARAMS` | PSP | yes, per-title game key |
| `<save>/PARAM.SFO`, without | PS3 | yes, `PARAM.PFD` |
| `<save>/sce_sys/param.sfo`, with `TITLE_ID` | PS4 | no |
| `<save>/sce_sys/param.sfo`, without | Vita | no |

**PS1 and PS2 are neither a folder nor an SFO.** Those consoles kept saves in
memory-card blocks and wrote no `PARAM.SFO` at all — the format postdates them —
so a save only becomes a file once it is exported as a signed `.PSV`
container. The walk therefore looks at files as well as folders, and a `.PSV` is
identified from what is inside it: the save's own memory-card directory name,
its file list, and the name the console's save list showed. See
[the container](#ps1-and-ps2--the-psv-container) below.

The second half — which game — is where the consoles stop agreeing, and is
the reason `core/saveinfo.c` exists rather than the app reading three keys and
guessing:

- **PSP and PS3** name no title ID at all. It is the first nine characters of
  `SAVEDATA_DIRECTORY` (`ULUS10391DATA00`, `BLUS30917-AUTOSAVE`), and the name
  is `TITLE`.
- **PS4** says `TITLE_ID` outright, and `MAINTITLE` is the game's real name —
  the only one of the four that stores it plainly.
- **Vita** says neither. `TITLE` is usually empty and there is no `TITLE_ID`
  key; the title ID is nine bytes at `0x28` inside the binary `PARAMS` blob,
  with `PARENT_DIRECTORY` (`/PCSE00608`) as the fallback. So a Vita save is
  **nameless** and has to be looked up by title ID.

For that last case the name comes from elsewhere, in this order:

1. **the save's own `PARAM.SFO`** — what the console itself shows, so it wins
   whenever there is one.
2. **`titles.tsv`** in the bundle, which `make-bundle.py` normalises from
   apollo-patches' `psptitleid.txt` and `psvtitleid.txt`: 8783 games by title
   ID, 4581 of them Vita.
3. **the patch database**, which names only the games it has patches for — 123
   Vita titles.

The catalogue sits above the patch database on purpose. It is a catalogue,
keyed exactly by title ID, where a patch's name is whatever its author wrote on
the file's second line and carries region suffixes and inconsistencies. More to
the point it covers 37× as many Vita games, so a save for a game nobody has
written codes for is still listed under its real name instead of `SLOT0`.

A title ID is believed only when it is exactly nine characters of upper-case
letters and digits. A folder somebody renamed still gets listed under its own
name — it just offers no codes, which is the right answer, because the wrong
game's codes would be worse than none.

This is also what makes PS4 and Vita saves work at all: their folder is named
after the *slot* (`SLOT0`, `JOJOASB.S`), so the folder-name lookup described
above finds nothing. The browser passes the title ID it read from the SFO
along with the file, and that is what the database is asked about.

## The consoles' own savedata encryption

A PSP or PS3 save is encrypted **twice**. The console wraps it with a key of its
own before the game's encryption is anywhere in the picture, and every patch in
the database addresses only the inner layer. So the console's wrapper has to
come off before any patch means anything: feed the engine a file copied straight
off a Memory Stick or a hard drive and it returns noise that looks like output.

Both layers live under `core/`, reworked into buffer APIs with no stdio because
there is no filesystem in a browser tab, and both parse their metadata
bounds-checked against the length they are handed rather than trusting the
offsets inside the file — the difference between a save off your own console and
one a stranger put on the web. Between them they need only mbedTLS's AES and
SHA1, which both front-ends already link, so they ride in the existing wasm
module rather than separate ones. Measured: the module is 317KB gzipped with
neither, 331KB with the PSP's, 341KB with both.

Both front-ends reach both layers, and they reach them differently because one
of them can look around. The web page asks for the metadata file and fetches the
key database from the CDN; the desktop app has the save folder, so it *detects*
the whole thing from the target you pick and reads the same database out of
`apollo-patches.zip`, offline. The lookup rules live in one C function each
front-end calls, because they are load-bearing in ways a second implementation
would get wrong.

The layering, on a real save:

```
MHP2NDG.BIN (1,483,024 bytes)   PSP savedata encryption (KIRK + the game key)
  └─ MHP2NDG.BIN (1,483,008)    Monster Hunter's own — what a .savepatch undoes
       └─ plaintext
```

Every patch in the database operates on the **middle** layer.

### PSP — `core/psp/`

Vendored from [apollo-psp](https://github.com/bucanero/apollo-psp)
(`kirk_engine.c` and `psp_decrypter.c`, at `17cb5ea`). `PARAM.SFO` says which
files are wrapped and which of the console's modes was used; the per-title game
key comes from apollo-patches' `PSP/gamekeys.txt`, matched by prefix against the
save directory with the **longest entry winning** — the database holds both
`NPJJ30022` and `NPJJ30022GAME1`, with different keys.

`core/test_psp.c` pins it to the unmodified upstream implementation: the
known-answer digests come from apollo-psp's own code compiled for the host, so a
pass says the vendored copy is byte-faithful to what ships on the console rather
than merely self-consistent. The whole chain has been checked against a real
console save — the plaintext it produces matches what the reference Monster
Hunter decrypter validates by the game's own stored SHA-1.

### PS3 — `core/ps3/`

Derived from flatz's `pfdtool`, by way of
[pfd_sfo_tools](https://github.com/bucanero/pfd_sfo_tools) and
[apollo-ps3](https://github.com/bucanero/apollo-ps3). Those two are the same
file: apollo-ps3's copy swaps polarSSL for mbedTLS and drops every byte swap,
because the PS3 is big-endian and so is the format. This one runs little-endian
and links mbedTLS, so it takes the swaps from the first and the crypto calls
from the second.

The shape is not upstream's. `pfd_init()` takes a directory and rehashes every
file in it; a browser has no directory, and a patcher does not need one, because
only the file it just changed needs rehashing. So the API here is one file at a
time.

`PARAM.PFD` lists the protected files — and that list is the **authoritative**
answer to what is encrypted, better than any key database: a game that encrypts
nothing ships a PFD holding only `PARAM.SFO`. The per-file secure ID comes from
apollo-patches' `PS3/games.conf`, whose sections are keyed by **save directory**
names rather than title IDs, with the longest prefix winning and the first file
pattern in file order taking precedence. Both rules matter: DiRT 3 files
`BLUS30724` and `BLUS30724PROFILE` separately with different keys, and Devil May
Cry lists `DATA` before `*`.

`core/test_ps3.c` cross-checks against pfdtool itself — a separate
implementation with its own polarSSL, which reads the synthetic save this test
writes, reports every hash OK, and re-encrypts it to byte-identical output
including the `PARAM.PFD`. Its `--corpus` mode then walks a tree of real saves:
over 180 of them, every one parses, every one **re-signs to itself byte for
byte**, and every protected file's ciphertext round-trips.

### Detecting it from the save folder

The desktop app has something the web page does not: the folder. So none of this
is asked for. **Choose a target, and if the console's metadata sits beside it
and lists that file, a section appears** with everything already filled in.

|             | PSP                        | PS3                          |
|-------------|----------------------------|------------------------------|
| metadata    | `PARAM.SFO`                | `PARAM.PFD`                  |
| what is wrapped | `SAVEDATA_FILE_LIST`   | the PFD's entry table        |
| the key     | per title, `PSP/gamekeys.txt` | per file, `PS3/games.conf` |
| keyed by    | the save directory         | the save directory *and* the file name |

Both key databases travel in `apollo-patches.zip`. No network: the web page
fetches the same files from a CDN, the app carries them.

The metadata is what decides, not the file name. A save folder holds
`ICON0.PNG` too, and a target the metadata does not name gets no section at all
rather than an offer to decrypt something that was never encrypted. On the PS3
that answer is especially reliable — a game that encrypts nothing ships a
`PARAM.PFD` listing only `PARAM.SFO`, so no key database has to be consulted to
find out.

- **The checkbox** — *unwrap before patching, and put it back after* — is the
  main event, and defaults to on. Apply then takes the console's layer off, runs
  the codes, puts it back, and rewrites the metadata with the file's new hash.
  The order is not symmetric and not negotiable: wrapping first would encrypt
  the ciphertext.
- **Decrypt only / Re-encrypt** are for the other case — opening a save in the
  hex editor, or repairing one that a failed run left decrypted. Decrypt only
  turns the checkbox off, so a file that is already plaintext is not unwrapped
  twice.
- **Resign** regenerates the metadata's own hashes alone, leaving every file as
  it is. It needs no key.
- **Decrypt all files / Re-encrypt all files** walk the console's own list --
  `SAVEDATA_FILE_LIST`, or the PFD's entry table -- and do the same to each.
  Deliberately not all-or-nothing: the files are independent, so stopping at
  the first failure would leave a half-done save with nothing saying how far it
  got. The PS3 pass looks a key up per file, because the secure file ID is
  keyed by save directory *and* file name, and skips `PARAM.SFO` by the same
  test that keeps it out of the single-file path.

  Both reuse the per-file functions the single-file buttons call, so there is
  one implementation of each direction rather than two that can drift.
- **Re-bind to your console** (PS3) appears once a console ID is named in
  Settings. See below.

Without a key nothing is guessed: the buttons and the checkbox stay disabled
rather than handing back noise. For a game the database does not cover, type 32
hex digits — or, on the PSP, load a dumper's file (SGKeyDumper's 16 bytes, or
SGDeemer's 1536).

A PS3 save also gets a check the PSP cannot offer: whether `PARAM.PFD`'s
recorded hash still matches the file on disk. A mismatch says so in the section,
because a save that already disagrees was damaged before it got here and
patching would sign the damage into place. It is only asked when the file is
still the length the console left it — one you already decrypted cannot match,
and flagging that would be crying wolf.

If Apply cannot put the layer back, it says so and names the state the file is
actually in — decrypted on disk — rather than reporting a generic failure. The
re-wrap runs even when a code failed, because the alternative is leaving the
user with something the console cannot read and no obvious way back.

The crypto itself is `core/psp/` and `core/ps3/`, described above.

## PS1 and PS2 — the `.PSV` container

Not encryption. The opposite problem: neither console encrypted a save, and
neither produced a **file**. A PS1 or PS2 save lived in blocks on a memory card,
so there was nothing to copy off — which is why a scanner looking for a folder
with a `PARAM.SFO` in it has never seen one.

A save becomes a file when something exports it as a signed `.PSV` carrying
the save's whole memory-card directory — the PS3's own export format, also
written by [apollo-ps2](https://github.com/bucanero/apollo-ps2) on the console
and by [ps2vmc-tool](https://github.com/bucanero/ps2vmc-tool) from a virtual
memory card. That container is what this reads and writes, derived from
[apollo-ps4](https://github.com/bucanero/apollo-ps4)'s `psv_resign.c` and
`psv_ps2.c` — themselves `ps3-psvresigner` by @dots_tb, with the CBPS group.

The container is signed with HMAC-SHA1 under a key derived from the file's own
salt by AES-128, on a different schedule per console. So patching one means
extracting the file a code addresses, patching that, putting it back and
**re-signing** — a PS3 checks the signature on import, and a save that fails it
looks corrupt rather than merely unsigned.

The rebuild moves what comes after the edited file rather than laying the
container out afresh, which is not pedantry: of 2,641 real PS2 containers, two
leave a gap between files and nine carry a `displaySize` that is not the sum of
their contents. Recomputing both would quietly rewrite eleven saves it was only
asked to patch.

Two things that follow from the format and cost more than they look:

- **The title ID comes out of the container, and the container is right.** It
  can disagree with the folder a save was filed under: 2,646 of 2,647 agree, and
  the one that does not is a Final Fantasy Chronicles save, a two-in-one disc
  whose halves carry different IDs (`SLUS-01360` and `SLUS-01363`).

`core/test_psv.c` walks a tree of real containers. Over the 2,647 in
apollo-saves every one parses, every signature verifies, every one **rebuilds
byte for byte**, and every one rebuilds correctly with a file actually changed.
Its `--patch` mode runs the whole chain — extract, apply real codes from the
database, put back, re-sign — and checks that every *other* file in the
container came through untouched.

### `.PSV` the file format, `PSV` the platform tag

Note that `.PSV` here is a **file format holding a PS1 or PS2 save**, and has
nothing to do with `PSV` the platform tag, which is the PS Vita. The two share
four letters and nothing else, and the ambiguity is the kind that compiles, so
the code refuses to rely on context:

- the console's enumerator is **`ASAVE_PSVITA`**, spelled out, never `ASAVE_PSV`
- everything about the container is **`apsvc_`/`psvcard`**, never plain `psv`

Two things keep the bare spelling because they name the file format itself
rather than either concept: the `.psv` extension, and `asave_platform_name()`,
which still answers `"PSV"` for the Vita — that string is the key the patch
database is sorted by, so it lives on disk and cannot be renamed.

## Which file inside a save a patch addresses

A save is a folder, and a patch addresses one file in it. The **File** dropdown
on the patcher screen is where that is chosen; the right one is starred, and for
the two encrypted consoles it is not a guess: the console's
own metadata says which files it wrapped — `SAVEDATA_FILE_LIST` in a PSP's
`PARAM.SFO`, the entry table in a PS3's `PARAM.PFD` — and that is the same list
that answers "which file comes out as garbage if you patch it as-is". PS4 and
Vita saves have no such list, so the largest file stands in; their saves are one
big file plus, occasionally, a small index beside it.

Everything else in the folder is listed too, with its size, so an unusual save
can still be opened by hand. The console's own metadata and artwork —
`PARAM.SFO`, `PARAM.PFD`, `ICON0.PNG`, `ICON1.PAM`, `PIC1.PNG`, `SND0.AT3`,
`sce_sys/` — is left out: none of it is ever the target, and on a PS3 the
artwork is *inside* the encrypted list, so leaving it in would have
Assassin's Creed suggesting its animated icon.

The last word belongs to the patch. A `.savepatch` carries target-file lines
(`:OPTIONS.DAT`), and once the game's codes are loaded they are asked: if the
patch names exactly one file, that file is in this save, and it is not the one
that was opened, the target moves to it and the log says so. Only when the
suggestion was taken — a file picked by hand is left alone — and only when the
answer is unambiguous, since plenty of patches address two files and have no
opinion about which to start with.

Across 176 real PS3 saves, 119 of which have codes: 101 suggestions already
matched the patch's own target line, 3 were corrected by it, and 15 were left
alone because the patch named several files or named one this save does not
have.

A PS1 or PS2 save's files are not on disk — they are inside the `.PSV`, and the
dropdown lists them from the container with their sizes exactly as it lists a
folder's. Choosing one **extracts** it to a scratch copy under the app's cache
directory, which is what the hex editor, the code viewers and the patch engine
then see; applying puts it back and re-signs the container. Nothing above that
line knows a container is involved, which is the point.

The starred suggestion skips a memory-card save's own presentation — `icon.sys`
and the `.ico` files, which every one of the 2,641 real PS2 containers carries
and which no code has ever addressed — and offers the largest of what is left.
A PS1 container holds exactly one file, its whole memory-card block, so there is
nothing to choose.

## Accounts, and which console a save is written for

Both front-ends have a **Settings** panel naming the console a save is written
back for. Both values are optional, both change only what is WRITTEN, and
leaving them blank keeps whatever a save already says — which is what patching
one in place wants.

- **PSP, Fuse ID.** Savedata modes 4 and 6 derive two `PARAM.SFO` hashes from
  the console's own fuse. The PSP's own loader does not enforce them, so most
  saves move between consoles regardless — but **the check is available to the
  game**, and some titles make it: they verify the fuse-derived hashes and flag
  the savedata when the signature does not match. *Gran Turismo* does. So this
  is not only for reproducing one console's output byte for byte; for a
  console-locked title it is what makes a re-signed save load at all. Which
  titles enforce it is not catalogued anywhere.

  **Per-game locking is out of scope, deliberately.** Some PSP titles bind a
  save to one console by means of their own rather than through anything the
  format offers — writing the system's Wi-Fi MAC address into the save data and
  checking it on load is a known pattern. There is no general handling to
  write: each scheme is undocumented and particular to its game, so undoing one
  belongs in that game's `.savepatch`, not here.

PS1 and PS2 have no such setting, and no account fields either — neither
console had the concept.
- **PS3, console ID (IDPS).** Inside `PARAM.PFD`, one of `PARAM.SFO`'s four
  hashes is keyed by the IDPS of a single machine — that is what binds a save to
  a console. Name one and both front-ends offer to **re-bind** a save to it.

Re-binding is the `PARAM.PFD` half of moving a save between consoles. A save
also carries account fields in its own `PARAM.SFO`, and those are not touched.

### Signing to an account

The two are independent and both can be used. Re-binding is the `PARAM.PFD`
half of moving a save; signing to an account is the `PARAM.SFO` half.

**Signing to an account** writes the ID into *both* places `PARAM.SFO` keeps
it — the `ACCOUNT_ID` field and the copy at offset `0x30` inside the binary
`PARAMS` blob. Writing one and not the other would leave the save disagreeing
with itself, and saves in that state are not hypothetical: across the **632
PS3 saves** in apollo-saves the two fields agree 587 times and **disagree 42**.

| | |
|---|---|
| `ACCOUNT_ID` zeroed, `PARAMS` still names one | 28 |
| `PARAMS` zeroed, `ACCOUNT_ID` still names one | 13 |
| two genuinely different accounts | 1 |

Forty-one of the 42 are half-unsigned — a tool cleared one field and left the
other. Which is exactly what writing both prevents.

**Reading** them is the other way round, and deliberately asymmetric:
`apfd_sfo_account_id()` takes `ACCOUNT_ID` whenever the key is present, and
falls back to the `PARAMS` copy only when it is missing or too short — *not*
when it is present and zeroed. So a save whose `ACCOUNT_ID` has been cleared
reports no owner even though `PARAMS` still holds the old one. That is the
right answer: `ACCOUNT_ID` is the documented field, `PARAMS+0x30` is a blob
offset recovered by reverse engineering, and on a save somebody unsigned for
sharing the leftover in `PARAMS` is a remnant rather than a claim of
ownership. Reporting it as the owner would put a stranger's name against 28 of
these saves. The PFD's hash of `PARAM.SFO` is taken over those bytes, so it is
recomputed and the database re-signed in the same action — stopping half way
leaves a save that will not load at all.

`PARAM.SFO` is written before `PARAM.PFD` deliberately: if the second write
fails, the save is one **Resign PARAM.PFD** away from correct rather than
silently mismatched, and the log says so.

### The same number, written two ways

The PSN account ID is one 64-bit number and it is the same number on every
console that carries it, which is why **one** field in Settings serves all of
them. What differs is how a save writes it down:

| | key | stored as | read by |
|---|---|---|---|
| **PS3** | `ACCOUNT_ID`, and again inside `PARAMS` at `+0x30` | **16 bytes** of ASCII hex | `apfd_sfo_account_id()` |
| **PS4** | `ACCOUNT_ID` in `sce_sys/param.sfo` | **8 raw bytes**, little-endian | `asfo_account_id()` |
| **Vita** | the same | the same | the same |
| **PSP** | — | no account in `PARAM.SFO` | never asked |
| **PS1, PS2** | — | no such concept | never asked |

The trap is that a PS3 and a PS4 use the **same key with the same binary
format code** and mean entirely different things by it. Only the length tells
them apart, so each reader checks it and returns an error on the other's file
rather than reading a number that is not there — `asfo_account_id()` on a PS3
save gives `ASFO_ERR_FORMAT`, not a plausible-looking 64 bits of ASCII. Both
directions are covered in `test_save.c`.

An `ACCOUNT_ID` of **zero** is left as "no account" rather than reported as an
owner. It is what a decrypted or shared save usually carries, and treating it
as an owner would mark every one of those as somebody else's.

### Measured against the save database

The whole of [apollo-saves](https://github.com/bucanero/apollo-saves) —
**4,834 archives** across PS1, PS2, PS3, PS4, PSP and Vita — with every
`PARAM.SFO` read straight out of its zip, giving **2,648** of them. Each was
classified by this reader and by an independent one written from the format
spec. The two agree on **all 2,648**:

| | files | |
|---|---|---|
| 16-byte ASCII (PS3) | **632** | 321 name an account, 311 are all-zero |
| 8-byte binary (PS4, Vita) | **672** | 624 name an account across 155 distinct ones, 48 are zero |
| no `ACCOUNT_ID` | 1,342 | PSP saves, and application/DLC SFOs |
| rejected as malformed | 2 | |

Reading the archives needs one thing beyond Python's `zipfile`: two use
**Deflate64**, which it does not implement, so the sweep falls back to `unzip`
for those. With that in place nothing in the database failed to open.

Three rows are worth drawing out, none of which was in the synthetic tests:

**Ten of the 16-byte ones were PS3 saves living inside a Vita archive** — a
PS1 Classic (`NPEB01899`) filed under `PSV/PCSB00560`, carrying a PS3-format
`PARAM.SFO` at its root. Precisely the collision the length check exists for,
sitting in the real corpus rather than only in a test. (They have since been
moved to `PS3/NPEB01899`, but the case they proved stands: nothing about a
directory's name can be trusted to say which console wrote what is in it.)

**Zero is common and means nobody** — 311 PS3 and 48 PS4/Vita saves carry it.
A save decrypted or unsigned for sharing usually does. Treating it as an owner
would mark every one of them as somebody else's, so it reads as "no account",
and the writer refuses to store it.

**The two rejected files are not PARAM.SFOs at all**: a zero-byte one
(`PSV/PCSE00638`), and a 14-byte file beginning `LOCA` rather than `\0PSF`
(`PSP/UCUS98640`). Both are refused on the magic and the length before any
offset in them is followed.

Run through the app itself rather than the reader alone, the same tree gives
2,566 saves — 1,262 PSP, 632 PS3, 339 PS4, 333 Vita — and the accounts
reconcile exactly with the table above.

### Assigning one

**PS3** goes through `ps3_account_resign()`, which must also rewrite
`PARAM.PFD` so its hash of `PARAM.SFO` still matches — see above for why that
is one action and not two buttons.

**PS4 and Vita** go through `sfo_account_resign()`, which is very much
simpler: these are decrypted saves with the console's own layer already off,
so nothing has to be re-wrapped around the change.
`asfo_set_account_id()` rewrites the value at its own eight bytes, so the file
neither grows nor moves. Verified on real saves: the file stays 2,728 bytes and
**exactly eight contiguous bytes change**, the `ACCOUNT_ID` and nothing else.

#### What is deliberately not rewritten

A PS4 `param.sfo` *does* carry a keyed hash, and it is worth being exact about
why this leaves it alone. Inside the `PARAMS` blob sits `psid_hmac`:
HMAC-SHA256 of the console's 16-byte OpenPSID under a published key, which is
what apollo-ps4's `sfo_patch_psid()` computes. It binds the save to one
console — and the console checks it **only for a save that names no account**.

So the two are alternatives, the same way they are on a PS3:

| the save | what binds it |
|---|---|
| names an account | `ACCOUNT_ID` |
| names none | `PARAMS.psid_hmac`, to one console |

Writing an account is therefore both the better fix and the one that takes the
hash out of the picture, which is also why `asfo_set_account_id()` refuses an
ID of zero: clearing the account would hand the save back to a console hash
belonging to someone else's machine.

The hash is written too, when Settings names an OpenPSID — the PS4 tab — so a
save satisfies both checks and travels to a console that has not signed in to
the account either. `asfo_ps4_set_psid_hmac()` is the same computation
apollo-ps4's `sfo_patch_psid()` performs, HMAC-SHA256 under the published key
over all sixteen bytes. Two more fields go with it, both of them what
`patch_sfo()` writes: `PARAMS.user_id` from the PS4 tab, and `title_id_1`
copied over `title_id_2`.

All three are best-effort and all three are **PS4 only**. A save carrying no
`PARAMS` is ordinary rather than a failure, an OpenPSID nobody has supplied is
simply not written, and the account above is the part that matters. What is
*not* optional is the console: the offsets below belong to the PS4 and land on
other things elsewhere.

| console | what lives at `PARAMS` + | |
|---|---|---|
| **PS4** | `0x04` `user_id`, `0x08` `psid_hmac`, `0x2C`/`0x3C` the title IDs | written here |
| **PS3** | `0x30` the account ID | never written here |
| **Vita** | `0x28` the title ID | never written here |

Both of the others sit inside what is `psid_hmac` on a PS4, so a misrouted
write would not fail — it would quietly replace a title ID or an account with
32 bytes of hash. `sfo_account_resign()` gates on `ASAVE_PS4` for that reason,
and `asfo_ps4_*` cannot do the check itself: nothing in the blob says which
console wrote it.

On the Vita the question does not arise anyway. apollo-vita's `patch_sfo()`
has its `sfo_patch_psid()` and `sfo_patch_user_id()` calls commented out, so
the account is all it writes either — which is what this matches.

`core/test_save.c`'s `check_ps4_params()` covers the three writers: each
writes its own field and leaves every other byte alone, a `PARAMS` shorter
than `0x50` is refused outright rather than half-written, and the HMAC is
checked against a vector computed with Python's `hmac`/`hashlib` rather than
against mbedTLS agreeing with itself.

That is also why there is no `.bak` for it — an eight-byte overwrite at a
known offset is undone by signing the save back, and a stray `param.sfo.bak`
inside `sce_sys` is worse than the thing it guards against.

The button lives in a different place per console, and deliberately: a PS3
save has a whole section below the header for its encryption layer, and the
button belongs there with the rest of it. A PS4 or Vita save has no such
section, because nothing about it is encrypted, so its one button sits in the
header beside the account line.

Re-signing updates **both** copies of the save — the open one and the list's,
which are separate because a rescan re-sorts the list and a reference into it
would dangle. `note_account_change()` does that, so the Owner column ticks at
once instead of waiting for a rescan.

The desktop updates the account line after signing, because the files really
are on disk by then. The web front-end's PS3 panel deliberately does not:
there the bytes are only offered for download, and the save has not changed
until you save them.

That panel is the whole of the web front-end's involvement. It handles one
file at a time and only for PSP and PS3 — the two consoles with an encryption
layer worth a panel — so there is no PS4 or Vita side of it to extend.

## Where this belongs

The better long-term home for both is apollo-lib, shared with apollo-psp,
apollo-ps3 and apollo-vita instead of copied a third time. That waits on
apollo-vita finishing its migration to mbedTLS, since its copy is still on
polarSSL and that is the only substantive difference between the upstream
versions.

