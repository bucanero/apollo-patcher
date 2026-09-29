# The front-ends

Two front-ends, one engine. Neither reimplements any patch logic: both drive
the same `libapollo` calls the CLI does, through `core/apollo_ctrl.[ch]`.

What differs between them is **what each one can see**. The desktop app has the
save folder, so it detects the console's encryption layer, the game key and the
byte order from the target you pick. The web page has one file at a time and
has to ask. That single difference explains most of what follows.

## What both do

### The engine facade

`core/apollo_ctrl.c` is the only code that adapts libapollo's data model for a
UI. It replaces the CLI's three interactive pieces with callbacks and data:

| CLI (`patcher.c` in apollo-lib) | Front-end equivalent               |
|---------------------------------|------------------------------------|
| `printf` / `dbglogger_log`      | `apctl_set_log_sink()` → log panel |
| `scanf` in `get_user_options`   | `apctl_opt_set_selected()` ← dropdowns |
| `is_active_code` arg parsing    | per-row checkboxes                 |

MicroPython `print()` output is routed to the same sink — the engine is built
**without** `-DAPOLLO_CLI`, so `dbglogger_printf` goes there too.

### Byte order

PS3 save data is big-endian; nothing else Apollo covers is. The engine accepts
a per-code `[BE:...]` header, but no patch in the database uses one — so before
this, every PS3 patch relied on the user knowing to tick a box.

`apctl_is_big_endian_for()` decides, and is shared by both front-ends. A patch
picked from the database carries its **platform tag** — simply the directory it
lives in — so that is authoritative and needs no guessing; a PS3 title with a
prefix nobody has catalogued yet still comes out right. A loose file has no
directory, so it falls back to matching the known PS3 title-ID prefixes against
the file name, and then against the patch's own first lines for a file that has
been renamed.

That answer is the default, not the last word. Settings can force big- or
little-endian for every save and the choice is remembered, so **both front-ends
say which order is in effect whenever one is forced**, and say so in amber or
red when a forced order contradicts the patch that is open.

### Interactive `{TAG}` options

Options start **unset** — the engine loads them as `-1`, meaning "not chosen" —
so Apply stays blocked until every option group on a ticked code has a value.
Both front-ends enforce it. Defaulting to the first value would silently choose
a user profile or character slot on the player's behalf.

The trap worth the extra code: an option's value is written **over** its
`{TAG}`, in place and at the tag's own length (`apply_tag_opts`), so a tag that
has been retyped or deleted stops resolving and its dropdown quietly does
nothing. Both front-ends watch for that and say so, since nothing else would
until the patch misbehaved.

### Editing a code, and saving the patch

The per-code **View** window is a text editor in both. *Save changes* replaces
the body the engine runs, *Revert to file* restores the patch's own, and the
row is marked while the two differ. Applying works on a copy of the body, so an
edit is not consumed by applying it and Apply stays repeatable.

**Runs as** picks the interpreter — Save Wizard, BSD or Python — and takes
effect immediately rather than waiting for *Save changes*: the type and the
text are separate things, and a wrongly-typed code often has nothing to type.
The loader takes the type from a `[SW:…]` / `[BSD:…]` / `[PYTHON:…]` title
prefix when there is one, and otherwise from the shape of the body (Save Wizard
only when *every* line is exactly `XXXXXXXX YYYYYYYY`) — so one mistyped line
is enough to land a code on the wrong interpreter, and this is how it gets
corrected.

**Saving the patch** writes it back out with the session's edits in it. The
engine splices the edits into the **original bytes** rather than regenerating
the file from its parse, so comments, credits, `:file` lines and option blocks
come through untouched — the parse keeps codes and drops all of that. It then
re-reads what it built and reports any code that would come back different.

One case always remains: a title carries a single marker, so a code already
flagged `[DEFAULT:…]` or `[INFO:…]` has no room to state a type, and that is
what gets reported.

## The desktop app

### Two screens

The app opens on **your saves**, not on a file picker.

| Screen | | |
|---|---|---|
| **Saves** | Ctrl+B | The list. Point it at a folder and every save underneath turns up, by game. |
| **Patcher** | Ctrl+P | One save: its icon, which of its files is the target, the console's encryption, the codes, Apply. |

Picking a save goes to the patcher; **< Saves** comes back. Going back closes
nothing — the patch stays open, so you can look at the list and return.

There is no third screen for driving files by hand, because everything after
*"which file, and which patch"* is the same work: the code list, the option
dropdowns, Apply, the log, the hex editor. **File ▸ Advanced** is a *door* into
the patcher screen rather than a room of its own — it swaps the save header for
**Open .savepatch...** and **Choose target...** so a loose file or a patch of
your own can be driven by hand. Everything below is identical either way.

A screen swap rather than a pop-up, for a concrete reason: an ImGui modal
*"blocks every interaction behind the window"*, and the hex editor and the
per-code editors are windows behind it. As a modal, the patcher would have made
its own hex editor unreachable.

### The patch finds itself

Every title ID in the database is exactly nine characters, on every platform,
and a save folder is named after it with an optional suffix — `ULUS10391`,
`ULJM05500DATA00`, `UCUS98751_DATA01`. So **choosing a target usually identifies
the game**: the first nine characters of the folder are looked up in the
database, and for a PSP save the `SAVEDATA_DIRECTORY` out of `PARAM.SFO` is
preferred over the folder on disk, since that is what the console recorded and
it survives a rename. A PS4 or Vita save's folder is named after the *slot*
and carries no title ID at all, so the saves screen passes the one it read
from `sce_sys/param.sfo` along with the file — see
[save data](savedata.md#recognising-one).

With no patch open it loads outright. With one already open it only *offers* —
a line naming the game and a button — because loading closes the current
session along with any edited code bodies in it, and discarding somebody's work
to be helpful is not a trade worth making.

An exact match against a real title ID is the whole guard, which is what keeps
it quiet: across the 792 real PSP save folders in
[apollo-saves](https://github.com/bucanero/apollo-saves) it matches 23 and
nothing else — `Brave_Story_New_Traveler` and a folder called `Downloads` name
no game, and neither does `Copy of ULUS101890001`, since the id has to be at
the front. Those open a patch by hand as before.

Patches are read straight out of the zip, so there is nothing to unpack. The
Python helper modules are the exception — MicroPython's `import` goes through
`stat()`/`open()` on real paths — so on startup they are extracted to the
per-user cache directory and the engine is pointed at it with
`apctl_set_data_path()`. Without that call `DATA_PATH` is empty and the engine
resolves `python/` against the process's working directory, so a Python code
that imports a helper module would work only when the app happened to be
launched from the right place.

`index.tsv` inside the zip carries the game names, generated by
`tools/build-index.py` — the same script that builds the web front-end's index,
including the Windows-1252 fallback that 245 of the patch files need. Without it
the app would have to inflate 2247 entries at startup just to read their second
line.

### What the save walk does and does not do

- **Depth 8, 40 000 folders.** A PS3's savedata sits five levels down
  (`dev_hdd0/home/00000001/savedata/<save>`), so eight is generous for anything
  pointed at a card or a drive. Both limits are *reported* when they are hit —
  "3 folders sit deeper than the scan goes" — because a save missing from the
  list with nothing said about it looks exactly like "you have no saves", and
  that is the one failure nobody can debug.
- **A folder that is a save is not descended into.** Nothing below a save is a
  save, and a PS4 save's `sce_sys` would otherwise be examined in its own right.
- **Symlinked folders are skipped.** A link pointing back up its own tree would
  otherwise be walked until the depth cap stopped it, listing the same saves
  several times over.
- **A game's own `PARAM.SFO` is not a save.** Discs and homebrew EBOOTs carry
  one and they are all over a memory card; requiring `SAVEDATA_DIRECTORY` is
  what keeps them out of the list.
- **Neither is DLC.** `CATEGORY` says what an SFO *describes*, and exactly one
  value per console means savedata — `MS` on a PSP, `SD` on a PS3, `sd` on a
  PS4 or Vita. Everything else is something else: `ac` is add-on content, `gd`
  is game data.

  This matters most for the Vita, whose DLC folders carry their own
  `sce_sys/param.sfo` **with a `TITLE_ID` in it** — and a `TITLE_ID` beside a
  Vita-shaped SFO is the single thing that otherwise distinguishes a PS4 save
  from a Vita one. Run over the
  [apollo-saves](https://github.com/bucanero/apollo-saves) database, 80 DLC
  folders across 7 archives were being listed as PS4 saves. All 2,566 real
  saves in it carry one of the three savedata categories and nothing else
  does.

  The comparison ignores case, since the same two letters are upper on a PS3
  and lower on a PS4. A file with **no** `CATEGORY` falls through to the
  key-set tests rather than being refused — every real save measured has one,
  but refusing a save over a key it merely omits would be a worse failure than
  the one this fixes.
- **It runs on its own thread.** A memory stick scans in well under a second,
  but nothing stops somebody choosing their home directory, and a window that
  freezes for a minute looks broken rather than busy. There is a Stop button,
  and quitting mid-scan cancels rather than waiting for the disk.

The folder is remembered in the settings file and re-scanned in the background
at startup, so the list is there the next time rather than asking again. With no
folder set, the saves screen says what to do instead of showing an empty table.

Going back to the list never loses anything, and opening a *different* save is
the one place work could go — it closes the patch that holds any edited code
bodies. `apctl_code_is_edited()` makes that detectable, so it is asked rather
than assumed.

The window opens at 1180x820, clamped to the monitor's work area so the
default cannot land partly off a smaller screen. The width matters because a
modal cannot be wider than the window around it — ImGui clips it rather than
growing the window — so the patch database browser and the rest size to what
the window gives them, and shrink with it rather than losing an edge.

### Settings

**File ▸ Settings…** holds how saves are read and written. Everything in it is
optional, and every default is the safe one.

- **Byte order** — *Auto*, *Big-endian* or *Little-endian*; see
  [Byte order](#byte-order) above. Force one only for a loose patch file for a
  console the database does not cover. The hazard is that a forced order is
  remembered across runs and applies to every save, so a forced big-endian left
  set will byte-reverse a PS4 or Vita save and hand back something that looks
  patched. The patcher screen therefore always says which order is in effect
  and why, in amber when a forced one disagrees with the open patch, and the
  log repeats it at Apply.

The other two name the console a save is written *for*. Both change only what
is WRITTEN, and leaving them blank keeps whatever a save already says — which
is what patching one in place wants.

- **PSP, Fuse ID** (16 hex digits). Savedata modes 4 and 6 derive two
  `PARAM.SFO` hashes from the console's own fuse. A PSP loads a save whose
  values differ, so this only matters for reproducing one console's output byte
  for byte.
- **PS3, account ID** (16 hex digits) — your PSN account, and **usually the
  one to reach for**. It is written into the save's own `PARAM.SFO`, so the
  save loads on *any* PS3 that account has signed in to rather than on one
  machine. Name one and the PS3 section offers **Sign to your account**.
- **PS3, console ID / IDPS** (32 hex digits, plus a user number). Inside
  `PARAM.PFD`, one of `PARAM.SFO`'s four hashes is keyed by the IDPS of a single
  machine — that is what binds a save to a console. Name one and the PS3 section
  offers **Re-bind to your console**, which rewrites that hash and re-signs the
  database around it. The user number reaches only a trophy folder's hashes.

Both hex fields are all-or-nothing: a half-typed value is not "no value", it is
one that would bind a save to the wrong machine, so Save stays disabled until
each is empty or complete. The byte order has nothing to validate, so it takes
effect and is saved the moment it is picked.

**Which account a save is signed to** is read during the scan, for every save
on a console that has the concept, and shown in three places: an **Owner**
column in the save list, the hover panel, and the patcher screen's save
header. The column appears only once an account is named in Settings — which
is exactly when the question has an answer worth a column.

Its cells carry a **check mark** when the save is yours and nothing when it is
not — the same tick the View menu puts beside *Saves* and *Patcher*, drawn by
`draw_check_mark()` with `MenuItemEx()`'s own sizing copied so the two cannot
drift apart. Marking only the matches keeps the column scannable: what the eye
runs down it for is the saves that **are** yours, and a word in every row would
bury those among the rest. Whose a save is instead — and whether it names an
account at all — is in the hover panel, which reads *"Signed to account … —
yours"* or *"— not yours"*.

It is read during the scan rather than when a save is opened because that is
when the question gets asked: *which of these are mine?* Reading it at open
time would have missed every save whose game encrypts nothing, since the PS3
section that would have shown it is hidden for those.

`--scan` and `--open` report it too, as `account: … (yours)`. Getting that
working turned up a separate bug: both modes returned from the argument loop
**before** `settings_load()` ran, so they answered every question against a
blank configuration — no account, so never "yours", and no console ID either,
so the console layer looked unavailable when it was configured. A file named
on the command line or handed over by Finder is opened in that same loop, so
it had the same problem, and the same save opened a second later through the
browser would behave differently. Settings are now loaded before the loop.

The saves folder is in that file too, though nothing in this dialog sets it:
choosing one in the save browser writes it there straight away. It is the one
setting somebody changes by *using* the app, and having to re-find a memory
stick every launch is the thing the browser exists to stop.

Settings live in the user's config directory —
`~/Library/Application Support/apollo-patcher/settings.txt` on macOS,
`$XDG_CONFIG_HOME/apollo-patcher/` on Linux, `%APPDATA%\apollo-patcher\` on
Windows — because they describe the person's console rather than this copy of
the program. It is the only file the app writes there; ImGui's own `.ini` is
deliberately off.

### Opening things

```bash
apollo_patcher_gui [FILE...]
```

Files on the command line are the other way in, alongside the
[saves screen](#two-screens), and they land on the patcher screen. A
`.savepatch` is opened as the patch; anything else is opened as the save to
patch, which pulls in everything that follows — the PSP game key, and that
game's patch from the database. Arguments are taken in the order given, so an
explicit patch beats the one a save's title ID would have auto-loaded,
whichever way round they are written. `--help` prints this and exits;
`--scan` and `--open` are described in
[Checking a folder from a terminal](#checking-a-folder-from-a-terminal).

**A folder works too**, which is the obvious thing to drag for a console save:

```bash
apollo_patcher_gui /Volumes/PSP/PSP/SAVEDATA/ULUS10391
```

It goes through the same identification the save list uses, so it arrives
**named, iconned, with its files listed** and Monster Hunter Freedom Unite's
patch loaded — the same thing as picking it from the list. A folder that is not
a save says so rather than becoming a target nothing can read, and neither does
a path that is not there.

Files can also be **dropped on the window**, which takes the same route.

### From Finder, on macOS

Double-clicking a `.savepatch`, dropping a save on the **Dock icon**, and
**Open With** are all the same mechanism — `kAEOpenDocuments` — and all three
work. A `.app` launched from Finder is handed its documents through Apple
Events rather than `argv`, so this takes two halves, and neither is any use
alone:

- **`Info.plist.in`** claims the types. macOS sends these events only for
  types an app has claimed, so without this the Dock icon would not even
  highlight when a save was dragged over it. `.savepatch` is claimed at
  `Owner` rank, since nothing else opens those; `public.data` and
  `public.folder` — a save is any file, or a folder — at **`Alternate`**,
  deliberately, so the app turns up in *Open With* and accepts Dock drops
  without volunteering to become the default application for every file on
  the machine.
- **`src/macos_open_docs.mm`** receives them. GLFW installs its own
  `NSApplication` delegate and does not implement `application:openFiles:`,
  so rather than subclass or swizzle it this registers with the Apple Event
  manager directly, which GLFW leaves alone. It is registered before the
  window exists, because a double-click launch sends the event almost
  immediately.

Each path goes to the same `open_path()` the drop handler and the command line
use, so a save folder opened from Finder arrives identified — named, iconned,
files listed, codes loaded — exactly as one picked from the list.

Windows and Linux file associations go through `argv`, which already worked.

### Viewing and editing data

- **View / edit data** (next to the target picker) opens the save file in a hex
  editor — `src/imgui_memory_editor.h`, vendored from
  [imgui_club](https://github.com/ocornut/imgui_club). It works on a copy held
  in memory and writes back only when asked, so a mistyped byte costs nothing
  until committed; a `.bak` is kept first, like the patch path does. The buffer
  is re-read every time the window is opened, because applying codes rewrites
  the file underneath it.
- **View** (per code row) opens that code's body, editable — see
  [Editing a code](#editing-a-code-and-saving-the-patch) for the rules both
  front-ends share.
- **View patch file** shows the `.savepatch` as text. Worth having: parsing
  keeps only the codes, so author comments, credits, `[INFO:]` notes and the
  target-file lines are invisible otherwise. Carriage returns are stripped for
  display — most patches are CRLF and ImGui has no glyph for CR.
- **Big-endian mode** is set on load, by the shared
  [`apctl_is_big_endian_for()`](#byte-order). Still a checkbox, so it can be
  overridden.

The window sizing for all three is in
[Fitting the hex window](text.md#fitting-the-hex-window), since it falls out of
the fixed-pitch font rather than out of anything here.

### One window, one identity

ImGui hashes a window's whole name, so `"Save data: foo *##hexedit"` and
`"Save data: foo##hexedit"` were two different windows: the hex editor jumped
back to its default position and size the moment a byte was edited, and the
code viewers did the same on their first edit. `###` restarts the hash, so the
varying part — file name, code name, the `*` dirty marker — stays out of the
identity. The three windows this affects are the hex editor, the raw patch
view and the per-code viewers (`###viewer%d`, one identity each).

### Checking a folder from a terminal

`--scan` runs the same walk with no window, which is how the browser's
behaviour is tested — and is useful on its own for "what does this card
actually hold":

```bash
apollo_patcher_gui --scan /Volumes/PSP
```

Add a save's number to open it as well — the same call the list makes — which
reports the save, its files, the target, the patch, the code count, the byte
order and the state of the encryption layer. What it prints is what the patcher
screen would be showing had you clicked it:

```bash
apollo_patcher_gui --scan /Volumes/PSP 2
```

`--open` takes one path the way a **dropped file** does and reports the same,
which is how that path is tested:

```bash
apollo_patcher_gui --open /Volumes/PS3/PS3/SAVEDATA/BLUS30917-AUTOSAVE
```

A dropped save **folder** goes through the browser's own identification, so it
arrives named, iconned, with its files listed and its codes loaded — the same
thing as one picked from the list. A loose file has no save behind it, so the
patcher shows the pickers instead of a save header.

## The web front-end

### Two pages

`index.html` is the patcher: every patch in the database, the whole code list,
tick what you want. It assumes you know which codes you need, which is also why
the two **console savedata panels** and **Settings** live there — neither is
per-game, both want the save folder's metadata alongside the file, and a save
still wrapped in its console's own encryption is not something the two-click
page can sensibly offer.

`tools.html` is narrower on purpose. Most people arrive wanting one of two
things — open this save so an editor can read it, or put the edited one back —
and the patcher makes them assemble that themselves from a list where getting
the order wrong produces a save the game rejects. The tools page offers the two
buttons instead, and `toolkit.js` decides which codes each runs:

- **Decrypt** — the required codes up to the first checksum-or-encrypt step.
- **Re-encrypt** (or **Fix checksum**, when the patch has no crypto) — the rest,
  in file order. The order is load-bearing: Silent Hill 3 is decrypt, update
  the DWADD checksum, encrypt, and skipping the middle gives a save the game
  refuses. Crisis Core computes its checksum over the *ciphertext*, which is
  why its checksum code sits after the encrypt.

It lists only patches `make verify` has vouched for — see
[web/README.md](../../web/README.md#verifying-the-tools). Everything
else about it is the patcher's machinery: same worker, same wasm, same CDN
fetch.

One card is one TOOL, not one patch. A game ships a patch per region and they
normally carry identical codes, so listing them separately buried the same tool
five times and left people unsure whether their region was covered. Patches are
folded by the chain group the verifier assigns — identical codes, so any member
can be the one the card loads — and the dialog names every title ID in the
group. Deliberately not folded by game name: Metal Gear Solid V keys per
region, so its PS3 releases are different tools and keep separate cards.

Card art comes from [apollo-saves](https://github.com/bucanero/apollo-saves) at
run time, by title ID. Coverage is partial, so the image removes itself when it
404s and the layout closes up; a group whose first region has no art is retried
against the others before giving up.

### The console panels

The PSP and PS3 savedata panels sit on the patcher page, below the pickers.
They are the outer layer described in
[save data](savedata.md#the-consoles-own-savedata-encryption); what differs
here is that the page cannot see a save folder, so each one asks for two files
— the console's metadata and the save itself — and works out the rest.

Two things follow from having no folder:

- **The PS3 needs a save folder NAME**, because `games.conf` files its sections
  under save directories and `PARAM.PFD` carries no such string. Dropping the
  folder's `PARAM.SFO` in alongside fills the field; otherwise you type it.
- **The PS3 panel checks the file against the database.** `PARAM.PFD` records a
  hash per protected file, so the panel can say whether the one you loaded is
  still the one it describes — worth knowing before patching, since a save that
  already disagrees would have the damage signed into place.

Both key databases are fetched from the CDN like the patches are; `games.conf`
is 23 times the size of the PSP's, so it is fetched when the panel opens
rather than at page load.

They are deliberately not cards in the grid: they are not per-game, and they
work for any save at all, including titles the patch database covers but the
catalog does not, and saves with no patch.

Re-encrypting hands back **two** files, and both have to go into the save
folder: the data file, and the rewritten metadata. The file's own hash lives in
the metadata and the signatures over it are regenerated, so a save put back with
the old `PARAM.SFO` or `PARAM.PFD` does not load.

The engines are `core/psp/` and `core/ps3/`, compiled into the same wasm module
— between them they need only mbedTLS's AES and SHA1, which this module already
links, so they cost **23KB gzipped** rather than separate modules with their own
copies of the crypto, their own heaps and their own instantiates. Measured
against builds with each layer removed: 317KB with neither, 331KB with the
PSP's, 341KB with both.

### …and inside the game cards

The PSP and PS3 tools in the grid address the *inner* layer, so each of their
dialogs also carries the console one, as an optional stage: add the save
folder's `PARAM.SFO` or `PARAM.PFD` and Decrypt takes both layers off in one
press, while Re-encrypt puts both back and hands over the rewritten metadata
alongside the save. Leave it out and the tool behaves exactly as it always did,
which is right for a file that is already unwrapped.

The stage is deliberately not one of the patch's file slots. Those slots are the
patch's own targets and their indices are what `routeChain()` assigns codes to,
so a row for the metadata would shift every route by one; it is a stage wrapped
around the run, not another target. Only files the metadata names are wrapped —
a save folder holds `ICON0.PNG` too.

For a PS3 tool the save folder defaults to the tool's own title ID, which is
right for 8480 of `games.conf`'s 8510 section IDs. When it is not — DiRT 3 files
`BLUS30724` and `BLUS30724PROFILE` with different keys — the stage says so
rather than decrypting to noise: it verifies the loaded file against the hash
`PARAM.PFD` recorded, and warns when they disagree.

The order is the part worth stating, because it is not symmetric and getting it
backwards produces a file that looks plausible and that the game refuses:

```
opening a save    unwrap the console's layer, THEN run the patch's decrypt
putting it back   run the patch's encrypt, THEN wrap the console's layer
```

The `NATIVE` table in `tools.js` owns that rule, and the per-console differences
under it, so the panels, the game cards and `verify-tools.mjs` cannot drift
apart on it.

### How it fits together

The engine is synchronous and some of it is slow — a Python code allocates a
64MB heap and runs a MicroPython interpreter — so all of it happens in a Web
Worker and the page stays responsive.

Files never touch a real filesystem. The `.savepatch` is parsed straight from
memory (`apctl_open_buffer`), and the save file is written into Emscripten's
in-memory filesystem so that `apollo_apply_code()` can read and write it the
same way it does on a console. Nothing in the engine needed changing.

`Apply` always starts from the bytes you loaded, so it is idempotent: what you
download reflects exactly the codes currently ticked, not an accumulation of
previous runs.

### The patch database browser

Users should not have to go and find a `.savepatch` first, so the page can
search the ~2250 patches in
[apollo-patches](https://github.com/bucanero/apollo-patches) by game name or
title ID and fetch the one they pick.

The split between build time and run time is deliberate:

- **The index is built in.** `../tools/build-index.py` reads the second line of
  every patch file (where the game name lives) and writes `dist/patches.json` —
  2247 rows, 24KB gzipped, fetched the first time the dialog opens. Reading
  2200 files is trivial here and impossible from a browser, and the GitHub API
  would neither give game names nor survive the rate limit.
- **The patches are fetched live**, from
  `cdn.jsdelivr.net/gh/bucanero/apollo-patches@main`, so a patch fixed upstream
  reaches users without redeploying this site. jsDelivr rather than
  `raw.githubusercontent.com`, which answers 503 to cross-origin requests from
  the Pages origin.

The consequence to keep in mind: a patch added upstream is not listed until this
site is rebuilt. A listed patch that has since been renamed 404s, which the
dialog reports while pointing at the drop zone as the fallback.

The same script builds the desktop app's index (as TSV, inside
`apollo-patches.zip`), so the parsing quirks below are handled once for both.

Two details in `build-index.py` that came from the real data: 245 patch files
are Windows-1252 rather than UTF-8 (game names with ™ / ®), so a strict decode
would drop them; and the leading `;` on the name line is a convention, not a
guarantee. The index's decoded name is also preferred over the engine's for
display, since the engine hands back raw bytes.

### Python helper modules

Fetched from the CDN on demand and written into `/python` in the in-memory
filesystem. libapollo resolves imports against `python` relative to the
filesystem root when no host callback is set, and MicroPython's import
`stat()`s real paths, so files written after startup work fine.

They were embedded with `--embed-file` at first. Measured, that cost **133KB
gzipped — 31% of the whole page** — while only 35 of 2247 patches import one. It
also left the page internally inconsistent: patches came live from the CDN while
the modules they import were frozen at build time, so a patch updated upstream
to use a new module raised a bare `ImportError`. Upstream history shows that is
a real pattern — new modules arrive together with new patches ("Add JoJo ASB
decrypter", "add jump force decrypter").

How it works now:

- `dist/python-modules.json` (300 bytes, generated) lists the module names, so
  nothing has to crawl a directory.
- The fetch starts when a patch containing **any** Python code is loaded, so it
  overlaps with the user reading the code list. `Apply` awaits it, and only when
  a ticked code is actually Python.
- The whole set is fetched, not the imports a code names, because the modules
  import each other (`umsgpack` pulls in `datetime`). Resolving that from
  outside would break the first time someone adds an import upstream. It costs
  little: 51 patches contain Python at all, and 16 of those import only
  built-ins.
- Nothing is written until every file has arrived. A partial set on disk would
  surface as an `ImportError` from inside a patch, which looks like a broken
  patch rather than a failed download. A failure is reported as one, and retried
  on the next Apply.

The desktop GUI deliberately does the opposite and bundles everything offline —
it is a download you keep, not one you make every visit.

### The hex editor

`public/hexedit.js` is vendored **unmodified** from
[ps2vmc-tool](https://github.com/bucanero/ps2vmc-tool) (`web-ps2/hexedit.js`),
so it can be refreshed from there. It ships its own dark stylesheet, injected
into `<head>` at run time; rather than fork the file, `style.css` maps its
palette onto this app's tokens with `.hx-bg`-prefixed rules — prefixed because
the injected `<style>` lands after our stylesheet and would otherwise win.

Loaded as a classic `<script>` (it is UMD, not an ES module) so it registers
`window.HexEdit` before the deferred module runs.

The loaded save is editable: committing edits replaces the bytes in memory and
retracts any previous result, since that output was produced from the bytes as
they were. The patched result is offered read-only — editing it would produce a
file no patch chain accounts for, and it is one Download away.

### Settings, and where they live

The **Settings** button is on the patcher page. The tools page has no such
button: the only setting that reaches it is the byte order, which it honours
and reports but never needs to change, since every patch it runs comes from
the database and carries its own platform. The patcher page also carries the
byte-order control inline, next to Apply.

The panel shows **which account the save is signed to now**, read from the
`PARAM.SFO` you supply, and marks it as yours when it matches Settings. It is
not updated after signing, on purpose: the page hands back bytes rather than
writing files, so the save has not changed until you save them.

The console is cleared across that update and put back afterwards, because
`apfd_update_file` re-binds `PARAM.SFO`'s console-keyed hashes whenever one is
named — ambient state rather than an argument — and signing to an account has
no business doing the console's job as well.

The Fuse ID and console ID live in the wasm module, which belongs to the
worker, so the page pushes them there on every change and once at start-up.
The account ID does not: it is passed with the call that uses it. `localStorage`
keeps all of them across visits, since they describe your console or account
rather than the save you happen to be holding.

### Two details the worker forces

Switching a code **to** Python has to tell the worker, because the Python
helper modules are fetched on demand and that decision is made from the
parsed types at load time. `codeState()` refreshes `codeTypes` and starts the
fetch, so a patch that contained no Python when it loaded still gets the
modules. The wire argument is `codeType`, not `type`: the worker envelope
already spends that name on the message kind.

The engine, not the page, decides whether a code counts as edited — saving the
patch file's own text back is not an edit — so `setCodeText` answers with what
the engine holds afterwards and the marker follows that.

Saving the patch, the bytes **never become a JS string** on the way out: 245
of the database's patches are Windows-1252, and decoding plus re-encoding
would corrupt the ™/® in their names, so the worker copies the range straight
out of the wasm heap into the Blob.

### Known limitation: the largest saves

Spilling makes the conservative scan retain aggressively, so a Python patch
working on a very large save can exhaust the MicroPython heap and raise
`MemoryError`. Monster Hunter World's 8MB save does, at `PY_HEAP_SIZE` and at
four times it; every other Python patch with a sample in
[save-decrypters](https://github.com/bucanero/save-decrypters) applies
correctly. It fails cleanly rather than producing a bad save.

## The tool catalog

`build-index.py --format=tools` emits a second, smaller index describing what
each patch can *do* rather than what codes it contains — the input for a
front-end that offers "decrypt this save" / "put it back" / "fix the checksum"
per game instead of a code list.

```bash
python3 tools/build-index.py /path/to/apollo-patches tools.json --format=tools
```

1156 of the 2247 patches qualify, covering 509 distinct games once a game's
regions are folded together: 913 are checksum-only, 243 can decrypt and
re-encrypt, and 43 of those go through offzip. 15KB gzipped.
`--verified=FILE` marks the ones `verify-tools.mjs` has proved against a real
save — 187 today, across 79 distinct code chains — and `--verified-only` emits
nothing else, which is what the web tools page ships, so it promises only what
has been run. Each row is `[platform, title_id, name, kinds, files]`, where
`kinds` holds `d` decrypt, `e` re-encrypt, `c` checksum, `z` offzip, and
`files` names what the user should supply.

Two things it deliberately does not do:

- **It does not split by the patch's `:file` targets.** Those markers are not
  dependable — Metal Gear Solid 2 HD files its "Encrypt DATA.BIN" code under
  `:MASTER.BIN`, Call of Duty: Black Ops puts "Encrypt GPAD0_CM.PRF" under
  `:GPAD0_SP.PRF` — and splitting on them invents 17 entries that can decrypt
  but never re-encrypt. It would also be splitting on something the front-end
  overrides anyway, since it applies every selected code to the single file the
  user supplied. Taken per patch, every decrypt in the database has a matching
  encrypt.
- **It does not decide which codes to run.** The engine stays the source of
  truth for that: the page opens the patch, reads the real code list back, and
  applies the required ones in file order. The catalog only answers "which
  games have a tool, and what kind" — the question you cannot ask 2247 files
  from a browser.

