/*
 * Headless checks for the .PSV container reader (core/psvcard.c) and the
 * Shift-JIS conversion its titles need (core/shiftjis.c).
 *
 *   test_psv                  run every self-contained check
 *   test_psv --dump FILE      print what one real .PSV says about itself
 *   test_psv --icon FILE OUT  render the icon inside one .PSV and write it as
 *                             OUT-<n>.png, so it can be looked at
 *   test_psv --patch PSV SAVEPATCH FILE
 *                             extract FILE from PSV, apply every code in
 *                             SAVEPATCH to it, put it back, and check the
 *                             result -- the whole chain, end to end
 *   test_psv --corpus DIR     walk a tree of real .PSV files: verify every
 *                             signature, and round-trip every container
 *                             through apsvc_replace() to prove a rebuild
 *                             reproduces the original byte for byte
 *
 * Why a corpus mode
 * -----------------
 * The self-contained checks below say the parser holds its shape against
 * containers built to be wrong. They cannot say the FORMAT is understood --
 * only real files written by a real PS3 can do that, and the way this would
 * fail is not a crash but a save that imports as garbage.
 *
 * So --corpus does the two things that would catch it. It re-signs each
 * container and checks the signature against the one the console wrote, which
 * says the key derivation is right rather than merely self-consistent. Then it
 * rebuilds each one by replacing a file with its own contents, and requires
 * the result to be identical to the input: every position, every total and the
 * signature over all of it. A rebuild that moved a byte would show up here and
 * nowhere else until somebody's save failed to load.
 *
 * Measured over the apollo-saves database -- 2,647 containers, 2,641 PS2 and
 * 6 PS1 -- every signature verifies and every round-trip is byte-identical.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include <zlib.h>   /* writing the icon PNGs --icon produces */

#include "psvcard.h"
#include "shiftjis.h"
#include "mcicon.h"
#include "apollo_ctrl.h"

/* ---- the harness -------------------------------------------------------- */

static int g_fails;

#define CHECK(what, cond) do {                                        \
        int ok_ = (cond);                                             \
        if (!ok_) g_fails++;                                          \
        printf("  %-58s %s\n", (what), ok_ ? "ok" : "FAILED");        \
    } while (0)

#define CHECK_STR(what, got, want) do {                               \
        int ok_ = strcmp((got), (want)) == 0;                         \
        if (!ok_) g_fails++;                                          \
        printf("  %-58s %s\n", (what), ok_ ? "ok" : "FAILED");        \
        if (!ok_) printf("      got \"%s\", wanted \"%s\"\n", (got), (want)); \
    } while (0)

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---- building a container ----------------------------------------------- */

#define HDR      0x40
#define PS2_HDR  40
#define PS2_DIR  56
#define PS2_FILE 60

typedef struct {
    const char *name;
    uint32_t    size;
} entry_t;

/*
 * A PS2 container with `n` files, each filled with a recognisable byte.
 * Unsigned -- the signature is exercised separately, and a synthetic file
 * cannot have a real one.
 */
static uint8_t *make_ps2(const entry_t *files, int n, size_t *out_len)
{
    size_t dir = HDR + PS2_HDR + PS2_DIR + (size_t)n * PS2_FILE;
    size_t total = dir, pos;
    uint8_t *b;
    int i;

    for (i = 0; i < n; i++) total += files[i].size;

    b = calloc(1, total);
    if (!b) return NULL;

    b[0] = 0x00; b[1] = 'V'; b[2] = 'S'; b[3] = 'P';
    memcpy(b + 0x08, "www.bucanero.com.ar", 19);
    put32(b + 0x38, 0x2C);
    put32(b + 0x3C, 2);

    put32(b + HDR + 36, (uint32_t)n);
    memcpy(b + HDR + PS2_HDR + 24, "BASLUS-20216", 12);

    pos = dir;
    for (i = 0; i < n; i++) {
        size_t rec = HDR + PS2_HDR + PS2_DIR + (size_t)i * PS2_FILE;
        strncpy((char *)b + rec + 24, files[i].name, 31);
        put32(b + rec + 16, files[i].size);
        put32(b + rec + 20, 0x00008497);
        put32(b + rec + 56, (uint32_t)pos);
        memset(b + pos, 'A' + i, files[i].size);
        pos += files[i].size;
    }

    *out_len = total;
    return b;
}

static uint8_t *make_ps1(uint32_t size, size_t *out_len)
{
    size_t total = 0x84 + size;
    uint8_t *b = calloc(1, total);
    if (!b) return NULL;

    b[0] = 0x00; b[1] = 'V'; b[2] = 'S'; b[3] = 'P';
    put32(b + 0x38, 0x14);
    put32(b + 0x3C, 1);
    put32(b + 0x40, size);
    put32(b + 0x44, 0x84);
    b[0x49] = 2; b[0x60] = 3; b[0x61] = 0x90;
    memcpy(b + 0x64, "BASLUS-01476", 12);
    memset(b + 0x84, 'Z', size);

    *out_len = total;
    return b;
}

/* ---- the reader --------------------------------------------------------- */

static void check_reader(void)
{
    static const entry_t files[] = {
        { "icon.sys",    1024 },
        { "BASLUS-20216", 260 },
        { "SaveData-00", 2416 },
    };
    apsvc_info_t info;
    apsvc_file_t f;
    uint8_t *b;
    size_t len;
    int count = 0;

    printf("\nreading a container\n");

    b = make_ps2(files, 3, &len);
    if (!b) { printf("  out of memory\n"); g_fails++; return; }

    CHECK("a PS2 container is valid", apsvc_valid(b, len) == APSVC_OK);
    CHECK("info succeeds", apsvc_info(b, len, &info) == APSVC_OK);
    CHECK("...and says PS2", info.type == APSVC_TYPE_PS2);
    CHECK("...with three files", info.file_count == 3);
    CHECK_STR("...under the save's own directory", info.dir_name, "BASLUS-20216");

    CHECK("the file count comes back alone", apsvc_files(b, len, NULL, 0, &count) == APSVC_OK);
    CHECK("...and is three", count == 3);

    CHECK("a file is found by name", apsvc_find(b, len, "SaveData-00", &f) == APSVC_OK);
    CHECK("...at its stored size", f.size == 2416);
    CHECK("...and its data is there", f.off + f.size <= len && b[f.off] == 'C');

    CHECK("a name matches case-insensitively",
          apsvc_find(b, len, "savedata-00", &f) == APSVC_OK);
    CHECK("a name that is not there is missing",
          apsvc_find(b, len, "SaveData-99", &f) == APSVC_ERR_MISSING);

    free(b);

    b = make_ps1(8192, &len);
    if (!b) { printf("  out of memory\n"); g_fails++; return; }

    CHECK("a PS1 container is valid", apsvc_valid(b, len) == APSVC_OK);
    CHECK("info says PS1", apsvc_info(b, len, &info) == APSVC_OK
                        && info.type == APSVC_TYPE_PS1);
    CHECK("...and reports its block as one file", info.file_count == 1);
    CHECK("the block is found by its own name",
          apsvc_find(b, len, "BASLUS-01476", &f) == APSVC_OK);
    CHECK("...at the fixed data offset", f.off == APSVC_PS1_DATA_OFF);
    CHECK("...for the whole block", f.size == 8192);

    free(b);
}

/*
 * Bounds. Every count and position below is read out of the file, so each is
 * given a value that would walk off the end if it were trusted.
 */
static void check_bounds(void)
{
    static const entry_t files[] = { { "icon.sys", 512 }, { "data", 512 } };
    apsvc_info_t info;
    uint8_t *b;
    size_t len;

    printf("\nrefusing a malformed container\n");

    CHECK("NULL is not a container", apsvc_valid(NULL, 100) == APSVC_ERR_FORMAT);

    b = make_ps2(files, 2, &len);
    if (!b) { printf("  out of memory\n"); g_fails++; return; }

    CHECK("a truncated header is refused", apsvc_valid(b, 0x40) == APSVC_ERR_FORMAT);

    b[1] = 'X';
    CHECK("a bad magic is refused", apsvc_valid(b, len) == APSVC_ERR_FORMAT);
    b[1] = 'V';

    put32(b + 0x3C, 7);
    CHECK("an unknown save type is refused", apsvc_valid(b, len) == APSVC_ERR_TYPE);
    put32(b + 0x3C, 2);

    put32(b + HDR + 36, 0xFFFFFFFFu);
    CHECK("an impossible file count is refused",
          apsvc_info(b, len, &info) == APSVC_ERR_FORMAT);
    put32(b + HDR + 36, 2);

    /* A position past the end, and one that would wrap if added to its size. */
    put32(b + HDR + PS2_HDR + PS2_DIR + 56, (uint32_t)len + 1000);
    CHECK("a file outside the container is refused",
          apsvc_info(b, len, &info) == APSVC_ERR_FORMAT);

    put32(b + HDR + PS2_HDR + PS2_DIR + 56, 0xFFFFFFF0u);
    put32(b + HDR + PS2_HDR + PS2_DIR + 16, 0xFFFFFFF0u);
    CHECK("a position plus size that would wrap is refused",
          apsvc_info(b, len, &info) == APSVC_ERR_FORMAT);

    free(b);

    b = make_ps1(8192, &len);
    if (!b) { printf("  out of memory\n"); g_fails++; return; }
    put32(b + 0x40, 0xFFFFFFFFu);
    CHECK("a PS1 block longer than its file is refused",
          apsvc_info(b, len, &info) == APSVC_ERR_FORMAT);
    free(b);
}

/* ---- signing ------------------------------------------------------------ */

static void check_signature(void)
{
    static const entry_t files[] = { { "icon.sys", 512 }, { "data", 256 } };
    uint8_t *b, keep[0x14];
    size_t len;

    printf("\nsigning\n");

    b = make_ps2(files, 2, &len);
    if (!b) { printf("  out of memory\n"); g_fails++; return; }

    CHECK("an all-zero signature reads as unsigned",
          apsvc_verify(b, len) == APSVC_SIG_UNSIGNED);

    CHECK("signing succeeds", apsvc_sign(b, len) == APSVC_OK);
    CHECK("...and the result verifies", apsvc_verify(b, len) == APSVC_SIG_OK);

    /* verify() has to hash the signature field as zeros, so it writes into the
     * caller's buffer. It must put it back. */
    memcpy(keep, b + 0x1C, sizeof keep);
    apsvc_verify(b, len);
    CHECK("...and verifying leaves the buffer untouched",
          memcmp(keep, b + 0x1C, sizeof keep) == 0);

    b[len - 1] ^= 0xFF;
    CHECK("a changed byte breaks the signature",
          apsvc_verify(b, len) == APSVC_SIG_BAD);
    b[len - 1] ^= 0xFF;
    CHECK("...and undoing the change restores it",
          apsvc_verify(b, len) == APSVC_SIG_OK);

    free(b);

    b = make_ps1(8192, &len);
    if (!b) { printf("  out of memory\n"); g_fails++; return; }
    CHECK("a PS1 container signs too", apsvc_sign(b, len) == APSVC_OK);
    CHECK("...and verifies", apsvc_verify(b, len) == APSVC_SIG_OK);

    /* The two consoles derive the key differently; a PS1 signature must not
     * happen to be what the PS2 schedule would produce. */
    {
        uint8_t ps1sig[0x14];
        memcpy(ps1sig, b + 0x1C, sizeof ps1sig);
        put32(b + 0x3C, 2);
        apsvc_sign(b, len);
        CHECK("...with a schedule the PS2 one does not match",
              memcmp(ps1sig, b + 0x1C, sizeof ps1sig) != 0);
        put32(b + 0x3C, 1);
    }
    free(b);
}

/* ---- rebuilding --------------------------------------------------------- */

static void check_replace(void)
{
    static const entry_t files[] = {
        { "icon.sys",    512 },
        { "SaveData-00", 256 },
        { "SaveData-01", 128 },
    };
    uint8_t *b, *out = NULL;
    uint8_t bigger[400], smaller[64];
    size_t len, out_len = 0;
    apsvc_file_t f;

    printf("\nrebuilding a container\n");

    b = make_ps2(files, 3, &len);
    if (!b) { printf("  out of memory\n"); g_fails++; return; }
    apsvc_sign(b, len);

    /* Same size: everything should land exactly where it was. */
    {
        apsvc_file_t orig;
        apsvc_find(b, len, "SaveData-00", &orig);
        CHECK("replacing a file with its own contents succeeds",
              apsvc_replace(b, len, "SaveData-00", b + orig.off, orig.size,
                            &out, &out_len) == APSVC_OK);
        CHECK("...and reproduces the container byte for byte",
              out && out_len == len && memcmp(out, b, len) == 0);
        apsvc_free(out); out = NULL;
    }

    memset(bigger, 'Q', sizeof bigger);
    CHECK("a file may grow",
          apsvc_replace(b, len, "SaveData-00", bigger, sizeof bigger,
                        &out, &out_len) == APSVC_OK);
    CHECK("...and the container grows with it",
          out_len == len - 256 + sizeof bigger);
    CHECK("...the new contents are in place",
          apsvc_find(out, out_len, "SaveData-00", &f) == APSVC_OK
          && f.size == sizeof bigger && out[f.off] == 'Q');
    CHECK("...the file after it moved with everything intact",
          apsvc_find(out, out_len, "SaveData-01", &f) == APSVC_OK
          && f.size == 128 && out[f.off] == 'C');
    CHECK("...icon.sys still sits before them",
          apsvc_find(out, out_len, "icon.sys", &f) == APSVC_OK
          && f.off < (size_t)out_len && out[f.off] == 'A');
    CHECK("...and the rebuilt container is signed",
          apsvc_verify(out, out_len) == APSVC_SIG_OK);
    apsvc_free(out); out = NULL;

    memset(smaller, 'q', sizeof smaller);
    CHECK("a file may shrink",
          apsvc_replace(b, len, "SaveData-00", smaller, sizeof smaller,
                        &out, &out_len) == APSVC_OK);
    CHECK("...and the container shrinks with it",
          out_len == len - 256 + sizeof smaller);
    CHECK("...and is still signed", apsvc_verify(out, out_len) == APSVC_SIG_OK);
    apsvc_free(out); out = NULL;

    CHECK("replacing a file that is not there is missing",
          apsvc_replace(b, len, "nope", smaller, sizeof smaller,
                        &out, &out_len) == APSVC_ERR_MISSING);

    free(b);

    /* PS1 is one block that the memory card sized; it cannot change length. */
    b = make_ps1(8192, &len);
    if (!b) { printf("  out of memory\n"); g_fails++; return; }
    apsvc_sign(b, len);

    CHECK("a PS1 block may be rewritten at its own size",
          apsvc_replace(b, len, "BASLUS-01476", b + 0x84, 8192,
                        &out, &out_len) == APSVC_OK);
    CHECK("...reproducing the container exactly",
          out && out_len == len && memcmp(out, b, len) == 0);
    apsvc_free(out); out = NULL;

    CHECK("...but not at a different one",
          apsvc_replace(b, len, "BASLUS-01476", smaller, sizeof smaller,
                        &out, &out_len) == APSVC_ERR_SPACE);
    free(b);
}

/* ---- title IDs ---------------------------------------------------------- */

static void check_title_id(void)
{
    char id[10];

    printf("\nderiving a title ID\n");

    apsvc_title_id("BASLUS-20216", id, sizeof id);
    CHECK_STR("an ordinary PS2 save name", id, "SLUS20216");

    apsvc_title_id("BESLES-50003", id, sizeof id);
    CHECK_STR("a European one", id, "SLES50003");

    apsvc_title_id("BISLPM-87053", id, sizeof id);
    CHECK_STR("a Japanese one", id, "SLPM87053");

    /* Several saves run the slot's own name straight on after the ID. */
    apsvc_title_id("BASLUS-21361DMC3SE", id, sizeof id);
    CHECK_STR("a name with the slot run on after it", id, "SLUS21361");

    /* Final Fantasy Chronicles: the container says SLUS-01360 while the save
     * is filed under SLUS01363. The container is what patches match against. */
    apsvc_title_id("BASLUS-01360FF4", id, sizeof id);
    CHECK_STR("a two-in-one disc's other half", id, "SLUS01360");

    /* Some discs carry a P after the code, with or without the dash. */
    apsvc_title_id("BASLUSP20074vol", id, sizeof id);
    CHECK_STR("a name with a P and no dash", id, "SLUS20074");

    apsvc_title_id("BESLESP-50136robot", id, sizeof id);
    CHECK_STR("a name with a P and a dash", id, "SLES50136");

    CHECK("a name that is not one is refused",
          apsvc_title_id("SAVEDATA", id, sizeof id) == APSVC_ERR_FORMAT);
    CHECK("...as is an empty one",
          apsvc_title_id("", id, sizeof id) == APSVC_ERR_FORMAT);
    CHECK("...and one whose digits are missing",
          apsvc_title_id("BASLUS-201", id, sizeof id) == APSVC_ERR_FORMAT);
}

/* ---- Shift-JIS ---------------------------------------------------------- */

static void check_shiftjis(void)
{
    char out[256];

    printf("\nreading a Shift-JIS title\n");

    /* Full-width ASCII, which is what 2,620 of the 2,641 real titles are. */
    {
        static const unsigned char fw[] = {
            0x82,0x60, 0x82,0x61, 0x82,0x62, 0x00   /* A B C, full-width */
        };
        asjis_to_utf8(fw, sizeof fw, out, sizeof out);
        CHECK_STR("full-width letters fold to ASCII", out, "ABC");
    }

    {
        static const unsigned char sp[] = { 0x82,0x60, 0x81,0x40, 0x82,0x61, 0x00 };
        asjis_to_utf8(sp, sizeof sp, out, sizeof out);
        CHECK_STR("the ideographic space folds to a space", out, "A B");
    }

    /* Plain ASCII passes through untouched. */
    {
        static const unsigned char a[] = "Dati";
        asjis_to_utf8(a, 4, out, sizeof out);
        CHECK_STR("single-byte ASCII survives", out, "Dati");
    }

    /*
     * Mixed widths. A converter that stepped two bytes at a time would pair
     * "Da" and "ti" as lead/trail and turn the rest of the name to noise --
     * four real titles do exactly this ("Dati di gioco GT3").
     */
    {
        static const unsigned char mixed[] = {
            'D','a','t','i', 0x81,0x40, 0x82,0x84, 0x82,0x89, 0x00
        };
        asjis_to_utf8(mixed, sizeof mixed, out, sizeof out);
        CHECK_STR("single and double byte characters may be mixed", out, "Dati di");
    }

    /* Real kana, which is the whole reason the table is here. */
    {
        static const unsigned char kana[] = { 0x83,0x47, 0x81,0x5B, 0x83,0x58, 0x00 };
        asjis_to_utf8(kana, sizeof kana, out, sizeof out);
        CHECK_STR("kana become UTF-8", out, "\xE3\x82\xA8\xE3\x83\xBC\xE3\x82\xB9");
    }

    /* A field that ends in half a character -- two real saves do. */
    {
        static const unsigned char cut[] = { 0x82,0x60, 0x81 };
        asjis_to_utf8(cut, sizeof cut, out, sizeof out);
        CHECK_STR("a dangling lead byte is dropped, not guessed", out, "A");
    }

    CHECK("a NUL ends the string", (asjis_to_utf8((const unsigned char *)"AB\0CD", 5,
                                                  out, sizeof out), strcmp(out, "AB") == 0));

    /* Truncation must not split a UTF-8 sequence. */
    {
        static const unsigned char kana[] = { 0x83,0x47, 0x83,0x47, 0x00 };
        char small[4];
        size_t n = asjis_to_utf8(kana, sizeof kana, small, sizeof small);
        CHECK("a short buffer stops on a character boundary", n == 3
              && (unsigned char)small[0] == 0xE3 && small[3] == '\0');
    }

    CHECK("a zero-length buffer is handled",
          asjis_to_utf8((const unsigned char *)"A", 1, out, 0) == 0);
}

/* ---- icons -------------------------------------------------------------- */

/*
 * A .ico built to order.
 *
 * `verts` is what the header CLAIMS; `pad` is how much vertex data actually
 * follows. Making the two disagree is the whole point: it is what the damaged
 * icon in the save database does, and the only way to reach the paths below
 * without one.
 */
static uint8_t *make_ico(uint32_t verts, uint32_t pad, int texture_type,
                         int with_texture, size_t *out_len)
{
    size_t len = 20 + pad + (with_texture ? (size_t)128 * 128 * 2 : 0);
    uint8_t *b = calloc(1, len);
    size_t at;
    int i;

    if (!b) return NULL;

    put32(b,      0x00010000);        /* file_id          */
    put32(b + 4,  1);                 /* animation_shapes */
    put32(b + 8,  (uint32_t)texture_type);
    put32(b + 12, 0x3F800000);        /* reserved         */
    put32(b + 16, verts);

    /* A recognisable texture: full red everywhere, with a green ramp over it.
     * Red is held at maximum so that EVERY pixel is plainly non-black -- the
     * check below is "is there a picture here", and a ramp alone would start
     * at black and prove nothing about its first pixel. */
    if (with_texture) {
        at = 20 + pad;
        for (i = 0; i < 128 * 128; i++, at += 2) {
            uint16_t t = (uint16_t)(0x1F | ((i % 32) << 5));
            b[at] = (uint8_t)t;
            b[at + 1] = (uint8_t)(t >> 8);
        }
    }

    *out_len = len;
    return b;
}

static void check_icons(void)
{
    uint8_t *ico, *rgba = NULL;
    size_t len;
    amci_ps2_kind_t kind;

    printf("\nreading a PS2 icon\n");

    /*
     * Geometry that does not fit, with a whole texture behind it. There is no
     * model to draw, but the picture is all there, so it comes back flat.
     */
    ico = make_ico(9999, 64, 7 /* uncompressed */, 1, &len);
    if (!ico) { printf("  out of memory\n"); g_fails++; return; }

    CHECK("an icon whose geometry does not fit falls back to its texture",
          amci_ps2_render_kind(ico, len, NULL, 0, 64, &rgba, &kind) == AMCI_OK);
    CHECK("...and says it drew the texture, not a model", kind == AMCI_PS2_FLAT);
    CHECK("...with the picture actually in it",
          rgba && (rgba[0] || rgba[1] || rgba[2]));
    amci_free(rgba); rgba = NULL;
    free(ico);

    /*
     * The same, with no texture either -- which is the Action Replay MAX icon
     * in the save database: truncated mid-geometry, so the animation block and
     * the texture are both simply absent.
     *
     * This must be reported as DAMAGE. Drawing the two thirds of the model
     * that survived produces a clean silhouette that looks entirely fine, and
     * a save whose icon is truncated is a save worth being suspicious of.
     */
    ico = make_ico(9999, 64, 7, 0, &len);
    if (!ico) { printf("  out of memory\n"); g_fails++; return; }
    CHECK("...and with no texture either, it is reported as damaged",
          amci_ps2_render_kind(ico, len, NULL, 0, 64, &rgba, &kind)
              == AMCI_ERR_CORRUPT);
    CHECK("...drawing nothing", rgba == NULL);
    free(ico);

    /*
     * An RLE texture is not hunted for when the geometry is unusable: its
     * position depends on where the geometry ended, which is exactly what is
     * wrong. A search finds convincing-looking noise, and a wrong picture
     * would stop the caller reporting the damage.
     */
    ico = make_ico(9999, 64, 0x0F /* RLE */, 1, &len);
    if (!ico) { printf("  out of memory\n"); g_fails++; return; }
    CHECK("an RLE texture is not guessed at, so the icon reads as damaged",
          amci_ps2_render_kind(ico, len, NULL, 0, 64, &rgba, &kind)
              == AMCI_ERR_CORRUPT);
    free(ico);

    CHECK("a zero-length icon is refused",
          amci_ps2_render_kind((const uint8_t *)"", 0, NULL, 0, 64, &rgba, &kind) < 0);
    CHECK("...as is a NULL one",
          amci_ps2_render_kind(NULL, 100, NULL, 0, 64, &rgba, &kind) < 0);
}

/* ---- real files --------------------------------------------------------- */

static int read_file(const char *path, uint8_t **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;

    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }

    *buf = malloc((size_t)n);
    if (!*buf) { fclose(f); return 0; }
    if (fread(*buf, 1, (size_t)n, f) != (size_t)n) {
        free(*buf); fclose(f); return 0;
    }
    fclose(f);
    *len = (size_t)n;
    return 1;
}

static int run_dump(const char *path)
{
    uint8_t *b;
    size_t len;
    apsvc_info_t info;
    apsvc_file_t *files;
    char id[10] = "", title[256] = "";
    int rc, i, sig;

    if (!read_file(path, &b, &len)) {
        printf("cannot read %s\n", path);
        return 1;
    }

    rc = apsvc_info(b, len, &info);
    if (rc != APSVC_OK) {
        printf("%s: %s\n", path, apsvc_strerror(rc));
        free(b);
        return 1;
    }

    apsvc_title_id(info.dir_name, id, sizeof id);
    apsvc_title(b, len, title, sizeof title);
    sig = apsvc_verify(b, len);

    printf("%s\n", path);
    printf("  %zu bytes, %s\n", len,
           info.type == APSVC_TYPE_PS1 ? "PS1" : "PS2");
    printf("  directory   %s\n", info.dir_name);
    printf("  title ID    %s\n", id[0] ? id : "(not derivable)");
    printf("  title       %s\n", title[0] ? title : "(none)");
    printf("  signature   %s\n",
           sig == APSVC_SIG_OK       ? "ok" :
           sig == APSVC_SIG_BAD      ? "DOES NOT MATCH" :
           sig == APSVC_SIG_UNSIGNED ? "unsigned" : "cannot be checked");
    printf("  %d file(s)\n", info.file_count);

    files = calloc((size_t)info.file_count, sizeof *files);
    if (files) {
        apsvc_files(b, len, files, info.file_count, NULL);
        for (i = 0; i < info.file_count; i++)
            printf("    %-24s %8u bytes at 0x%06zX\n",
                   files[i].name, files[i].size, files[i].off);
        free(files);
    }
    free(b);
    return 0;
}

static long g_total, g_sig_ok, g_sig_bad, g_sig_unsigned, g_sig_unknown;
static long g_parse_bad, g_roundtrip_ok, g_roundtrip_bad, g_no_id, g_no_title;
static long g_edit_ok, g_edit_bad;
static long g_icon_ok, g_icon_none, g_icon_bad;
static long g_icon_flat, g_icon_corrupt;

static void corpus_one(const char *path)
{
    uint8_t *b, *out = NULL;
    size_t len, out_len = 0;
    apsvc_info_t info;
    apsvc_file_t f;
    char id[10], title[256];

    if (!read_file(path, &b, &len)) return;
    g_total++;

    if (apsvc_info(b, len, &info) != APSVC_OK) {
        g_parse_bad++;
        printf("  cannot parse: %s\n", path);
        free(b);
        return;
    }

    switch (apsvc_verify(b, len)) {
    case APSVC_SIG_OK:       g_sig_ok++; break;
    case APSVC_SIG_BAD:      g_sig_bad++;
                             printf("  signature does not match: %s\n", path); break;
    case APSVC_SIG_UNSIGNED: g_sig_unsigned++; break;
    default:                 g_sig_unknown++; break;
    }

    if (apsvc_title_id(info.dir_name, id, sizeof id) != APSVC_OK) {
        g_no_id++;
        printf("  no title ID from \"%s\": %s\n", info.dir_name, path);
    }
    if (apsvc_title(b, len, title, sizeof title) != APSVC_OK)
        g_no_title++;

    /*
     * The icon, which for these two consoles is not a file to decode but
     * something to BUILD: a PS1 save's is sixteen colours inside its own
     * block, a PS2 save's a textured 3D model that has to be rendered.
     */
    if (info.type == APSVC_TYPE_PS1) {
        apsvc_file_t one;
        if (apsvc_files(b, len, &one, 1, NULL) == APSVC_OK) {
            uint8_t rgba[AMCI_PS1_SIZE * AMCI_PS1_SIZE * 4];
            int rc = amci_ps1_frame(b + one.off, one.size, 0, rgba);
            if (rc == AMCI_OK)            g_icon_ok++;
            else if (rc == AMCI_ERR_NONE) g_icon_none++;
            else                          g_icon_bad++;
        }
    } else {
        apsvc_file_t *all = calloc((size_t)info.file_count, sizeof *all);
        int i, found = 0;

        if (all && apsvc_files(b, len, all, info.file_count, NULL) == APSVC_OK) {
            for (i = 0; i < info.file_count && !found; i++) {
                const char *dot = strrchr(all[i].name, '.');
                uint8_t *rgba = NULL;
                int rc;

                if (!dot || (strcasecmp(dot, ".ico") && strcasecmp(dot, ".icn")))
                    continue;
                found = 1;
                amci_ps2_kind_t kind = AMCI_PS2_MODEL;
                rc = amci_ps2_render_kind(b + all[i].off, all[i].size,
                                          info.sys_off ? b + info.sys_off : NULL,
                                          info.sys_size, 64, &rgba, &kind);
                if (rc == AMCI_OK) {
                    if (kind == AMCI_PS2_FLAT) g_icon_flat++;
                    else                       g_icon_ok++;
                    amci_free(rgba);
                } else if (rc == AMCI_ERR_CORRUPT) {
                    g_icon_corrupt++;
                } else {
                    g_icon_bad++;
                }
            }
            if (!found) g_icon_none++;
        }
        free(all);
    }

    /*
     * Rebuild by replacing the last file with its own contents. The last one
     * because it is the one whose position the most arithmetic has to agree
     * on, and its own bytes because then the answer is known: anything other
     * than the input means the rebuild moved something.
     */
    if (info.file_count > 0) {
        apsvc_file_t *all = calloc((size_t)info.file_count, sizeof *all);
        if (all && apsvc_files(b, len, all, info.file_count, NULL) == APSVC_OK) {
            f = all[info.file_count - 1];
            /* By index, not by name: four containers hold two files called
             * settings.dat, and a name would find the wrong one. */
            if (apsvc_replace_at(b, len, info.file_count - 1, b + f.off, f.size,
                                 &out, &out_len) == APSVC_OK
                && out_len == len && memcmp(out, b, len) == 0) {
                g_roundtrip_ok++;
            } else {
                g_roundtrip_bad++;
                printf("  rebuild differs (%s in %s)\n", f.name, path);
            }
            apsvc_free(out);
            out = NULL;

            /*
             * ...and the same rebuild with the file actually CHANGED, which is
             * what patching one does. The container must come back with that
             * byte different, everything else where it was, and a signature
             * that verifies -- a PS3 checks it on import, and a save that
             * fails looks corrupt rather than merely unsigned.
             */
            if (f.size > 0) {
                uint8_t *edit = malloc(f.size);
                if (edit) {
                    memcpy(edit, b + f.off, f.size);
                    edit[0] ^= 0xFF;

                    if (apsvc_replace_at(b, len, info.file_count - 1,
                                         edit, f.size, &out, &out_len) == APSVC_OK
                        && out_len == len
                        && apsvc_verify(out, out_len) == APSVC_SIG_OK
                        && memcmp(out + f.off, edit, f.size) == 0
                        /* Everything before the edit, EXCEPT the signature --
                         * which is the one field that is supposed to change
                         * when the contents do. */
                        && memcmp(out, b, 0x1C) == 0
                        && memcmp(out + 0x30, b + 0x30, f.off - 0x30) == 0) {
                        g_edit_ok++;
                    } else {
                        g_edit_bad++;
                        printf("  edited rebuild wrong (%s in %s)\n", f.name, path);
                    }
                    apsvc_free(out);
                    free(edit);
                }
            }
        }
        free(all);
    }
    free(b);
}

static void corpus_walk(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[4096];

    if (!d) return;
    while ((e = readdir(d))) {
        struct stat st;

        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            corpus_walk(path);
        } else {
            const char *dot = strrchr(e->d_name, '.');
            if (dot && (strcmp(dot, ".PSV") == 0 || strcmp(dot, ".psv") == 0))
                corpus_one(path);
        }
    }
    closedir(d);
}

static int run_corpus(const char *dir)
{
    printf("walking %s\n", dir);
    corpus_walk(dir);

    printf("\n  containers            %ld\n", g_total);
    printf("  parsed                %ld  (%ld refused)\n",
           g_total - g_parse_bad, g_parse_bad);
    printf("  signature ok          %ld\n", g_sig_ok);
    printf("  signature bad         %ld\n", g_sig_bad);
    printf("  unsigned              %ld\n", g_sig_unsigned);
    printf("  signature unknown     %ld\n", g_sig_unknown);
    printf("  rebuilt identically   %ld\n", g_roundtrip_ok);
    printf("  rebuilt DIFFERENTLY   %ld\n", g_roundtrip_bad);
    printf("  edited and re-signed  %ld\n", g_edit_ok);
    printf("  edited WRONGLY        %ld\n", g_edit_bad);
    printf("  icon rendered         %ld\n", g_icon_ok);
    printf("  icon absent           %ld\n", g_icon_none);
    printf("  icon flat (texture)   %ld\n", g_icon_flat);
    /*
     * Neither of these fails the run: they are facts about the SAVES, not
     * about this code.
     *
     * "damaged" is an icon whose header claims more than its file holds and
     * from which not even a whole texture could be recovered. The fifteen in
     * apollo-saves are all the same Action Replay MAX file, truncated
     * mid-geometry. Reporting it is the point -- a front-end says so, rather
     * than drawing the two thirds that survived and implying all is well.
     */
    printf("  icon DAMAGED          %ld\n", g_icon_corrupt);
    printf("  icon unusable         %ld\n", g_icon_bad);
    printf("  no title ID           %ld\n", g_no_id);
    printf("  no title              %ld\n", g_no_title);

    if (g_sig_bad || g_roundtrip_bad || g_edit_bad || g_parse_bad) {
        printf("\nFAILED\n");
        return 1;
    }
    printf("\nPASS\n");
    return 0;
}

/* ---- icons -------------------------------------------------------------- */

/*
 * An RGBA buffer written out as a PNG, so the icons this renders can be looked
 * at rather than merely counted. Small and deliberately unclever: one IDAT, no
 * filtering, which zlib compresses perfectly well for images this size.
 */
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static size_t chunk(uint8_t *out, const char *type, const uint8_t *body, size_t len)
{
    uLong crc;
    put_be32(out, (uint32_t)len);
    memcpy(out + 4, type, 4);
    if (len) memcpy(out + 8, body, len);
    crc = crc32(0, out + 4, (uInt)(4 + len));
    put_be32(out + 8 + len, (uint32_t)crc);
    return 12 + len;
}

static int write_png(const char *path, const uint8_t *rgba, int w, int h)
{
    static const uint8_t sig[8] = { 137, 'P','N','G', 13, 10, 26, 10 };
    uint8_t ihdr[13];
    uint8_t *raw, *png, *packed;
    uLongf packed_len;
    size_t raw_len, at = 0;
    FILE *f;
    int y;

    raw_len = (size_t)h * (1 + (size_t)w * 4);
    raw = malloc(raw_len);
    if (!raw) return 0;
    for (y = 0; y < h; y++) {
        raw[(size_t)y * (1 + (size_t)w * 4)] = 0;   /* filter: None */
        memcpy(raw + (size_t)y * (1 + (size_t)w * 4) + 1,
               rgba + (size_t)y * (size_t)w * 4, (size_t)w * 4);
    }

    packed_len = compressBound((uLong)raw_len);
    packed = malloc(packed_len);
    png = malloc(packed_len + 1024);
    if (!packed || !png) { free(raw); free(packed); free(png); return 0; }

    if (compress2(packed, &packed_len, raw, (uLong)raw_len, 9) != Z_OK) {
        free(raw); free(packed); free(png); return 0;
    }
    free(raw);

    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;

    memcpy(png, sig, sizeof sig); at = sizeof sig;
    at += chunk(png + at, "IHDR", ihdr, sizeof ihdr);
    at += chunk(png + at, "IDAT", packed, packed_len);
    at += chunk(png + at, "IEND", NULL, 0);
    free(packed);

    f = fopen(path, "wb");
    if (!f) { free(png); return 0; }
    fwrite(png, 1, at, f);
    fclose(f);
    free(png);
    return 1;
}

/*
 * Render whatever icon a container carries and write it beside `out_prefix`.
 *
 * A PS1 save's icon is in its block and comes out at 16x16, up to three
 * frames. A PS2 save's is one to three 3D models named by icon.sys, each
 * rendered at 128x128.
 */
static int run_icon(const char *path, const char *out_prefix)
{
    uint8_t *b;
    size_t len;
    apsvc_info_t info;
    char out[1024];
    int made = 0;

    if (!read_file(path, &b, &len)) {
        printf("cannot read %s\n", path);
        return 1;
    }
    if (apsvc_info(b, len, &info) != APSVC_OK) {
        printf("%s: not a container\n", path);
        free(b);
        return 1;
    }

    if (info.type == APSVC_TYPE_PS1) {
        apsvc_file_t f;
        int frames, i;

        if (apsvc_files(b, len, &f, 1, NULL) != APSVC_OK) { free(b); return 1; }
        frames = amci_ps1_frames(b + f.off, f.size);
        printf("%s: PS1, %d icon frame(s)\n", info.dir_name, frames);

        for (i = 0; i < frames; i++) {
            uint8_t rgba[AMCI_PS1_SIZE * AMCI_PS1_SIZE * 4];
            int rc = amci_ps1_frame(b + f.off, f.size, i, rgba);
            if (rc != AMCI_OK) {
                printf("  frame %d: %s\n", i, amci_strerror(rc));
                continue;
            }
            snprintf(out, sizeof out, "%s-%d.png", out_prefix, i);
            if (write_png(out, rgba, AMCI_PS1_SIZE, AMCI_PS1_SIZE)) {
                printf("  frame %d -> %s\n", i, out);
                made++;
            }
        }
    } else {
        apsvc_file_t *all;
        int i;

        all = calloc((size_t)info.file_count, sizeof *all);
        if (!all || apsvc_files(b, len, all, info.file_count, NULL) != APSVC_OK) {
            free(all); free(b); return 1;
        }
        printf("%s: PS2, %d file(s)\n", info.dir_name, info.file_count);

        for (i = 0; i < info.file_count; i++) {
            const char *dot = strrchr(all[i].name, '.');
            uint8_t *rgba = NULL;
            int rc;

            if (!dot || (strcasecmp(dot, ".ico") != 0 && strcasecmp(dot, ".icn") != 0))
                continue;

            rc = amci_ps2_render(b + all[i].off, all[i].size,
                                 info.sys_off ? b + info.sys_off : NULL,
                                 info.sys_size, 128, &rgba);
            if (rc != AMCI_OK) {
                printf("  %-20s %s\n", all[i].name, amci_strerror(rc));
                continue;
            }
            snprintf(out, sizeof out, "%s-%s.png", out_prefix, all[i].name);
            if (write_png(out, rgba, 128, 128)) {
                printf("  %-20s -> %s\n", all[i].name, out);
                made++;
            }
            amci_free(rgba);
        }
        free(all);
    }

    free(b);
    return made ? 0 : 1;
}

/* ---- the whole chain ---------------------------------------------------- */

/*
 * Extract, patch, put back -- the three steps the desktop app performs when
 * somebody applies a code to a PS1 or PS2 save, run here without a window.
 *
 * Worth doing end to end rather than trusting the parts. Each half is checked
 * above: the container rebuilds byte-identically, and the patch engine has its
 * own tests. What neither says is that the file the engine WROTE is the file
 * that goes back into the container -- and getting that wrong produces a .PSV
 * that is structurally perfect, correctly signed, and does not contain the
 * change. Nothing short of this notices.
 */
static int run_patch(const char *psv_path, const char *patch_path, const char *inner)
{
    uint8_t *b = NULL, *patch = NULL, *out = NULL;
    size_t len = 0, patch_len = 0, out_len = 0;
    apctl_session_t *sess;
    apsvc_file_t f, after;
    uint8_t *before = NULL;
    int applied = 0, changed = 0, i, rc;

    if (!read_file(psv_path, &b, &len)) {
        printf("cannot read %s\n", psv_path);
        return 1;
    }
    if (!read_file(patch_path, &patch, &patch_len)) {
        printf("cannot read %s\n", patch_path);
        free(b);
        return 1;
    }

    if (apsvc_find(b, len, inner, &f) != APSVC_OK) {
        printf("no \"%s\" inside %s\n", inner, psv_path);
        free(b); free(patch);
        return 1;
    }
    printf("container  %s\n", psv_path);
    printf("  patching %s (%u bytes)\n", f.name, f.size);

    /* The extracted file, written where the app would put it. */
    {
        FILE *fp = fopen("psv-inner.bin", "wb");
        if (!fp) { free(b); free(patch); return 1; }
        fwrite(b + f.off, 1, f.size, fp);
        fclose(fp);
    }
    before = malloc(f.size);
    if (before) memcpy(before, b + f.off, f.size);

    sess = apctl_open_buffer((const char *)patch, patch_len, patch_path);
    if (!sess) {
        printf("  the patch would not parse\n");
        free(b); free(patch); free(before);
        return 1;
    }
    printf("  patch    %s, %d code(s)\n", apctl_game_name(sess), apctl_code_count(sess));

    apctl_set_big_endian(0);   /* neither console is big-endian */
    for (i = 0; i < apctl_code_count(sess); i++) {
        apctl_code_t *c = apctl_code_at(sess, i);
        int g;

        if (!c) continue;

        /*
         * Take the first of every choice. Several of these patches are built
         * around a slot selector -- Devil May Cry 3's five codes all address
         * whichever of ten save slots a {SF} group picks -- so a harness that
         * skipped them would apply nothing and report success for it.
         */
        for (g = 0; g < apctl_opt_group_count(c); g++)
            apctl_opt_set_selected(c, g, 0);

        if (apctl_apply(sess, c, "psv-inner.bin")) {
            applied++;
            printf("    applied: %s\n", c->name);
        }
    }
    apctl_reset_vars();
    apctl_close(sess);
    free(patch);

    if (!applied) {
        printf("  no code applied, so nothing to check\n");
        free(b); free(before);
        return 1;
    }

    /* ...and back in. */
    {
        uint8_t *patched = NULL;
        size_t patched_len = 0;

        if (!read_file("psv-inner.bin", &patched, &patched_len)) {
            free(b); free(before); return 1;
        }
        changed = before && (patched_len != f.size ||
                             memcmp(before, patched, f.size) != 0);

        rc = apsvc_replace(b, len, inner, patched, (uint32_t)patched_len,
                           &out, &out_len);
        free(patched);
        if (rc != APSVC_OK) {
            printf("  putting it back failed: %s\n", apsvc_strerror(rc));
            free(b); free(before);
            return 1;
        }
    }

    printf("\n");
    CHECK("the code changed the extracted file", changed);
    CHECK("the rebuilt container still parses",
          apsvc_valid(out, out_len) == APSVC_OK);
    CHECK("...and its signature verifies",
          apsvc_verify(out, out_len) == APSVC_SIG_OK);
    CHECK("...and still holds the file", apsvc_find(out, out_len, inner, &after) == APSVC_OK);
    CHECK("...carrying what the patch wrote",
          before && memcmp(out + after.off, before, after.size) != 0);

    /* Every OTHER file has to be exactly as it was: a code addresses one file,
     * and a rebuild that disturbed its neighbours would corrupt the save. */
    {
        int count = 0, untouched = 1;
        apsvc_file_t *was, *now;

        apsvc_files(b, len, NULL, 0, &count);
        was = calloc((size_t)count, sizeof *was);
        now = calloc((size_t)count, sizeof *now);
        if (was && now) {
            apsvc_files(b, len, was, count, NULL);
            apsvc_files(out, out_len, now, count, NULL);
            for (i = 0; i < count; i++) {
                if (strcmp(was[i].name, inner) == 0) continue;
                if (was[i].size != now[i].size ||
                    memcmp(b + was[i].off, out + now[i].off, was[i].size) != 0) {
                    untouched = 0;
                    printf("      %s differs\n", was[i].name);
                }
            }
        }
        free(was); free(now);
        CHECK("...and every other file untouched", untouched);
    }

    {
        FILE *fp = fopen("psv-patched.PSV", "wb");
        if (fp) {
            fwrite(out, 1, out_len, fp);
            fclose(fp);
            printf("\n  wrote psv-patched.PSV (%zu bytes)\n", out_len);
        }
    }

    apsvc_free(out);
    free(before);
    free(b);
    printf("\n%s\n", g_fails ? "FAILED" : "PASS");
    return g_fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "--dump") == 0)
        return run_dump(argv[2]);

    if (argc > 2 && strcmp(argv[1], "--corpus") == 0)
        return run_corpus(argv[2]);

    if (argc > 3 && strcmp(argv[1], "--icon") == 0)
        return run_icon(argv[2], argv[3]);

    if (argc > 4 && strcmp(argv[1], "--patch") == 0)
        return run_patch(argv[2], argv[3], argv[4]);

    printf(".PSV container reader\n");

    check_reader();
    check_bounds();
    check_signature();
    check_replace();
    check_title_id();
    check_shiftjis();
    check_icons();

    printf("\n%s\n", g_fails ? "FAILED" : "PASS");
    return g_fails ? 1 : 0;
}
