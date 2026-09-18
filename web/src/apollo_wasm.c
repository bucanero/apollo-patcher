/*
 * apollo_wasm - Emscripten binding over the apollo_ctrl facade.
 *
 * The whole point of this layer is to keep the JS/C boundary narrow: instead of
 * one export per field, the code list crosses once as JSON. Everything else is
 * a handful of calls that take/return scalars or a single string.
 *
 * Log output does NOT come back through a return value. libapollo emits
 * progress through dbglogger_log() and MicroPython print() through
 * dbglogger_printf(); apollo_ctrl routes both to a sink, and the sink here
 * appends to a JS array that the worker drains after each call. This matters
 * for Python codes, which can print a lot before returning.
 *
 * One session at a time, held in a static: the UI only ever has one patch file
 * open, and libapollo's patch-variable state is process-global anyway.
 */
#include <emscripten.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apollo.h"
#include "apollo_ctrl.h"
#include "psp_savedata.h"

static apctl_session_t *g_session = NULL;

/* The patch file's own bytes, kept for export: the engine's parse mutates and
 * discards its input, and a saved patch has to carry everything the parse
 * drops (comments, credits, `:file` lines, option blocks). */
static char  *g_source = NULL;
static size_t g_source_len = 0;

/* ---------------------------------------------------------------------------
 * Log sink -> JS
 * ------------------------------------------------------------------------- */

static void log_sink(void *ud, const char *line)
{
    (void)ud;
    /* MicroPython's print() emits each argument as its own call, so lines
     * arrive fragmented; the worker reassembles. Pushing raw keeps that
     * decision on the JS side. */
    EM_ASM({ (globalThis.apolloLog ||= []).push(UTF8ToString($0)); }, line);
}

/* ---------------------------------------------------------------------------
 * Growable string buffer for the JSON payload
 * ------------------------------------------------------------------------- */

typedef struct {
    char  *p;
    size_t len, cap;
} sbuf_t;

static int sb_grow(sbuf_t *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + need + 1) cap *= 2;
    char *np = realloc(b->p, cap);
    if (!np) return 0;
    b->p = np;
    b->cap = cap;
    return 1;
}

static void sb_puts(sbuf_t *b, const char *s)
{
    size_t n = strlen(s);
    if (!sb_grow(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_printf(sbuf_t *b, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_puts(b, tmp);
}

/* JSON string literal, including the surrounding quotes. Escapes what RFC 8259
 * requires: quote, backslash and everything below 0x20. Bytes >= 0x80 are
 * passed through — patch files are effectively UTF-8/Latin-1 and the JS side
 * decodes them, so re-encoding here would only lose information. */
static void sb_json_str(sbuf_t *b, const char *s)
{
    sb_puts(b, "\"");
    if (!s) { sb_puts(b, "\""); return; }

    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        switch (*c) {
            case '"':  sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n");  break;
            case '\r': sb_puts(b, "\\r");  break;
            case '\t': sb_puts(b, "\\t");  break;
            case '\b': sb_puts(b, "\\b");  break;
            case '\f': sb_puts(b, "\\f");  break;
            default:
                if (*c < 0x20)
                    sb_printf(b, "\\u%04x", *c);
                else {
                    if (!sb_grow(b, 1)) return;
                    b->p[b->len++] = (char)*c;
                    b->p[b->len] = 0;
                }
        }
    }
    sb_puts(b, "\"");
}

/* Returned strings are owned here and stay valid until the next call that
 * returns a string. The JS side copies immediately (UTF8ToString). */
static char *g_out = NULL;

static const char *publish(sbuf_t *b)
{
    free(g_out);
    g_out = b->p;
    return g_out ? g_out : "";
}

/* ---------------------------------------------------------------------------
 * Exports
 * ------------------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
const char *apw_version(void)
{
    return APOLLO_LIB_VERSION;
}

/* Whether this patch's save data is big-endian. `platform` is the database's
 * tag ("PS3", ...) and wins when present; `text` is the title-ID fallback for a
 * loose file. Pass an empty platform for the latter. See
 * apctl_is_big_endian_for. */
EMSCRIPTEN_KEEPALIVE
int apw_is_be(const char *platform, const char *text)
{
    return apctl_is_big_endian_for(platform && *platform ? platform : NULL, text);
}

/* Parse a .savepatch from memory. Returns 1 on success, 0 on failure. */
EMSCRIPTEN_KEEPALIVE
int apw_open(const char *buf, int len, const char *name)
{
    apctl_set_log_sink(log_sink, NULL);

    if (g_session) {
        apctl_close(g_session);
        g_session = NULL;
    }
    free(g_source);
    g_source = NULL;
    g_source_len = 0;

    if (len <= 0) return 0;

    g_session = apctl_open_buffer(buf, (size_t)len, name);
    if (!g_session) return 0;

    g_source = malloc((size_t)len);
    if (g_source) {
        memcpy(g_source, buf, (size_t)len);
        g_source_len = (size_t)len;
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void apw_close(void)
{
    if (g_session) {
        apctl_close(g_session);
        g_session = NULL;
    }
    free(g_source);
    g_source = NULL;
    g_source_len = 0;
    apctl_reset_vars();
}

/* ---------------------------------------------------------------------------
 * Saving the edited patch
 *
 * Two calls rather than one: the text is bytes, not a JS string. Patch files
 * are not all UTF-8 (245 of the database's are Windows-1252), so the worker
 * copies the range out of the heap instead of letting UTF8ToString() decode
 * and mangle it. The buffer stays valid until the next export or close.
 * ------------------------------------------------------------------------- */

static char  *g_export = NULL;
static size_t g_export_len = 0;

/* Returns a pointer into the wasm heap, or 0 on failure. Length via
 * apw_export_size(). */
EMSCRIPTEN_KEEPALIVE
const char *apw_export_patch(void)
{
    free(g_export);
    g_export = NULL;
    g_export_len = 0;

    if (!g_session || !g_source) return NULL;

    g_export = apctl_export_patch(g_session, g_source, g_source_len, &g_export_len);
    return g_export;
}

EMSCRIPTEN_KEEPALIVE
int apw_export_size(void)
{
    return (int)g_export_len;
}

/* How many codes would read back differently from the text apw_export_patch()
 * just produced, with their row indices written into `rows` (an Int32 view of
 * `max` entries). See apctl_export_mismatches: the format cannot say
 * "Save Wizard" or "BSD", so a forced type is the usual answer. */
EMSCRIPTEN_KEEPALIVE
int apw_export_mismatches(int *rows, int max)
{
    if (!g_session || !g_export || !g_export_len) return 0;
    return apctl_export_mismatches(g_session, g_export, g_export_len, rows, max);
}

/* The whole code list, as JSON. Shape:
 *
 *   { "game": "...", "codes": [ { "id":1, "type":2, "flags":0,
 *       "parent":0, "child":0, "activated":0, "name":"...", "file":"...",
 *       "options":[ {"tag":"...","sel":0,"values":["..."]} ] } ] }
 */
EMSCRIPTEN_KEEPALIVE
const char *apw_codes_json(void)
{
    sbuf_t b = {0};

    if (!g_session) {
        sb_puts(&b, "{\"game\":\"\",\"codes\":[]}");
        return publish(&b);
    }

    sb_puts(&b, "{\"game\":");
    sb_json_str(&b, apctl_game_name(g_session));
    sb_puts(&b, ",\"codes\":[");

    int n = apctl_code_count(g_session);
    for (int i = 0; i < n; i++) {
        apctl_code_t *c = apctl_code_at(g_session, i);
        if (i) sb_puts(&b, ",");

        sb_printf(&b, "{\"id\":%d,\"type\":%d,\"flags\":%d,\"parent\":%d,"
                      "\"child\":%d,\"activated\":%d,\"name\":",
                  c->id, c->type, c->flags, c->is_parent, c->is_child, c->activated);
        sb_json_str(&b, c->name);
        sb_puts(&b, ",\"file\":");
        sb_json_str(&b, c->file);

        sb_puts(&b, ",\"options\":[");
        int groups = apctl_opt_group_count(c);
        for (int g = 0; g < groups; g++) {
            if (g) sb_puts(&b, ",");
            sb_puts(&b, "{\"tag\":");
            sb_json_str(&b, apctl_opt_tag(c, g));
            sb_printf(&b, ",\"sel\":%d,\"values\":[", apctl_opt_get_selected(c, g));

            int vals = apctl_opt_value_count(c, g);
            for (int v = 0; v < vals; v++) {
                if (v) sb_puts(&b, ",");
                sb_json_str(&b, apctl_opt_value_name(c, g, v));
            }
            sb_puts(&b, "]}");
        }
        sb_puts(&b, "]}");
    }

    sb_puts(&b, "]}");
    return publish(&b);
}

/* Raw code body (Save Wizard lines, BSD commands or Python source). */
EMSCRIPTEN_KEEPALIVE
const char *apw_code_text(int index)
{
    sbuf_t b = {0};
    if (!g_session || index < 0 || index >= apctl_code_count(g_session)) {
        sb_puts(&b, "");
        return publish(&b);
    }
    const char *t = apctl_code_text(apctl_code_at(g_session, index));
    sb_puts(&b, t ? t : "");
    return publish(&b);
}

/* ---------------------------------------------------------------------------
 * Editing a code body
 *
 * Session-only: the .savepatch itself is never rewritten, and closing the
 * patch drops every edit. The engine applies from a copy of the body, so an
 * edit survives being applied and Apply stays repeatable.
 * ------------------------------------------------------------------------- */

/* Returns 1 if the body is now `text`, 0 if the index is bad or the copy
 * failed. Setting the original text back counts as success and clears the
 * edited flag. */
EMSCRIPTEN_KEEPALIVE
int apw_set_code_text(int index, const char *text)
{
    if (!g_session || index < 0 || index >= apctl_code_count(g_session)) return 0;
    return apctl_set_code_text(apctl_code_at(g_session, index), text ? text : "");
}

/* Reinterpret the body as another kind of code (APOLLO_CODE_* : 1 Save
 * Wizard, 2 BSD, 3 Python). Returns 1 on success, 0 for a bad index or an
 * unknown type. See apctl_set_code_type. */
EMSCRIPTEN_KEEPALIVE
int apw_set_code_type(int index, int type)
{
    if (!g_session || index < 0 || index >= apctl_code_count(g_session)) return 0;
    return apctl_set_code_type(apctl_code_at(g_session, index), type);
}

/* The code's current type: it moves when the caller sets it, and back again on
 * revert, so the page re-reads it rather than assuming. */
EMSCRIPTEN_KEEPALIVE
int apw_code_type(int index)
{
    if (!g_session || index < 0 || index >= apctl_code_count(g_session)) return 0;
    return apctl_code_at(g_session, index)->type;
}

/* The code's current flag word. Worth re-reading after an edit: emptying a
 * body (or filling an empty one) moves APOLLO_CODE_FLAG_EMPTY, which is what
 * decides whether a row can be ticked at all. */
EMSCRIPTEN_KEEPALIVE
int apw_code_flags(int index)
{
    if (!g_session || index < 0 || index >= apctl_code_count(g_session)) return 0;
    return apctl_code_at(g_session, index)->flags;
}

EMSCRIPTEN_KEEPALIVE
int apw_code_is_edited(int index)
{
    if (!g_session || index < 0 || index >= apctl_code_count(g_session)) return 0;
    return apctl_code_is_edited(apctl_code_at(g_session, index));
}

EMSCRIPTEN_KEEPALIVE
void apw_revert_code(int index)
{
    if (!g_session || index < 0 || index >= apctl_code_count(g_session)) return;
    apctl_revert_code(apctl_code_at(g_session, index));
}

EMSCRIPTEN_KEEPALIVE
int apw_set_option(int index, int group, int value)
{
    if (!g_session || index < 0 || index >= apctl_code_count(g_session)) return 0;
    apctl_code_t *c = apctl_code_at(g_session, index);
    if (group < 0 || group >= apctl_opt_group_count(c)) return 0;
    if (value < 0 || value >= apctl_opt_value_count(c, group)) return 0;

    apctl_opt_set_selected(c, group, value);
    return 1;
}

/* Apply one code to `path` (a file the caller has already written into the
 * virtual filesystem). Returns 1 on success, 0 on failure. */
EMSCRIPTEN_KEEPALIVE
int apw_apply(int index, const char *path, int big_endian)
{
    if (!g_session || index < 0 || index >= apctl_code_count(g_session)) return 0;

    /* apctl re-asserts the mode on every apply (apollo_free_var_list() can run
     * between codes and resets it), so setting it here per code is correct. */
    apctl_set_big_endian(big_endian ? 1 : 0);
    return apctl_apply(g_session, apctl_code_at(g_session, index), path);
}

/* Drops the patch-variable state libapollo accumulates across applies. Call
 * once after a batch, exactly like the GUI does. */
EMSCRIPTEN_KEEPALIVE
void apw_reset_vars(void)
{
    apctl_reset_vars();
}

/* ---------------------------------------------------------------------------
 * PSP savedata
 *
 * The PSP's own encryption, which wraps a save BELOW anything a .savepatch
 * touches -- see core/psp/psp_savedata.h. Separate from everything above: it
 * holds no session, needs no patch, and a page can drive it for any PSP save
 * at all, including the ~60 PSP titles the patch database covers but the tool
 * catalog does not.
 *
 * Buffers cross the boundary the way the worker's withBytes() already does:
 * JS mallocs, copies in, calls, reads back out. Output lands in a buffer owned
 * here and read through apw_psp_out()/apw_psp_out_size(), the same shape as
 * apw_export_patch(). Encryption additionally rewrites the PARAM.SFO IN PLACE,
 * at the pointer JS passed in, so the caller reads its updated bytes back from
 * where it put them and there is no second output buffer to manage.
 * ------------------------------------------------------------------------- */


static unsigned char *g_psp_out = NULL;
static size_t         g_psp_out_len = 0;

static void psp_out_free(void)
{
    free(g_psp_out);
    g_psp_out = NULL;
    g_psp_out_len = 0;
}

/* Allocate the result buffer for a call about to run. Returns NULL on OOM,
 * having already cleared any previous result. */
static unsigned char *psp_out_alloc(size_t len)
{
    psp_out_free();
    if (!len) return NULL;
    g_psp_out = malloc(len);
    if (g_psp_out) g_psp_out_len = len;
    return g_psp_out;
}

EMSCRIPTEN_KEEPALIVE
const char *apw_psp_out(void)
{
    return (const char *)g_psp_out;
}

EMSCRIPTEN_KEEPALIVE
int apw_psp_out_size(void)
{
    return (int)g_psp_out_len;
}

/* A result code as something a user can read. */
EMSCRIPTEN_KEEPALIVE
const char *apw_psp_error(int rc)
{
    return apsp_strerror(rc);
}

/*
 * What a PARAM.SFO says about the save it belongs to, as one JSON crossing:
 *
 *   {"ok":true,"directory":"ULUS10391","mode":65,"files":["MHP2NDG.BIN"]}
 *   {"ok":false,"error":"PARAM.SFO is missing, truncated or malformed"}
 *
 * `directory` is the game-key lookup: apollo-patches' gamekeys.txt matches
 * against the start of it. `files` is the authoritative list of what in the
 * folder is encrypted at all -- the page offers exactly those and no others,
 * rather than letting someone feed it an ICON0.PNG.
 */
EMSCRIPTEN_KEEPALIVE
const char *apw_psp_sfo_json(const char *sfo, int sfo_len)
{
    sbuf_t b = {0};
    char name[APSP_NAME_LEN + 1];
    char dir[64];
    int rc, n, i;

    apctl_set_log_sink(log_sink, NULL);

    if (!sfo || sfo_len <= 0) rc = APSP_ERR_ARG;
    else rc = apsp_sfo_valid((const unsigned char *)sfo, (size_t)sfo_len);

    if (rc != APSP_OK) {
        sb_puts(&b, "{\"ok\":false,\"error\":");
        sb_json_str(&b, apsp_strerror(rc));
        sb_puts(&b, "}");
        return publish(&b);
    }

    sb_puts(&b, "{\"ok\":true,\"directory\":");
    if (apsp_sfo_directory((const unsigned char *)sfo, (size_t)sfo_len,
                           dir, sizeof dir) != APSP_OK)
        dir[0] = 0;
    sb_json_str(&b, dir);

    /* The raw SAVEDATA_PARAMS mode byte, so the page can say WHY a save needs
     * a key (0x01 is the unkeyed form and does not). */
    sb_printf(&b, ",\"mode\":%d,\"files\":[",
              apsp_sfo_mode((const unsigned char *)sfo, (size_t)sfo_len));

    n = apsp_sfo_file_count((const unsigned char *)sfo, (size_t)sfo_len);
    for (i = 0; i < n; i++) {
        if (apsp_sfo_file_name((const unsigned char *)sfo, (size_t)sfo_len,
                               i, name, sizeof name) != APSP_OK)
            continue;
        if (i) sb_puts(&b, ",");
        sb_json_str(&b, name);
    }
    sb_puts(&b, "]}");
    return publish(&b);
}

/*
 * A game key out of a dumper's file: SGKeyDumper's bare 0x10 bytes, or
 * SGDeemer's 0x600. Writes 16 bytes to `out16` and returns 0, or a negative
 * result code. Kept in C rather than reimplemented in JS so the page and the
 * desktop app recognise exactly the same files.
 */
EMSCRIPTEN_KEEPALIVE
int apw_psp_key_from_file(const char *buf, int len, char *out16)
{
    if (!buf || len < 0 || !out16) return APSP_ERR_ARG;
    return apsp_key_from_buffer((const unsigned char *)buf, (size_t)len,
                                (unsigned char *)out16);
}

/*
 * A game key out of apollo-patches' PSP/gamekeys.txt, by save directory.
 *
 * The page fetches the file and hands the text straight over rather than
 * parsing it in JS, so the prefix rule -- and the longest-match tie-break that
 * the NPJJ30022 / NPJJ30022GAME1 pair depends on -- lives in exactly one
 * place, shared with the desktop app.
 *
 * Writes 16 bytes to `out16` and returns 0, or a negative result code. `id`,
 * when given, receives the entry that matched.
 */
EMSCRIPTEN_KEEPALIVE
int apw_psp_key_from_db(const char *text, int len, const char *directory,
                        char *out16, char *id, int id_cap)
{
    if (!text || len < 0 || !directory || !out16) return APSP_ERR_ARG;
    return apsp_key_from_db(text, (size_t)len, directory,
                            (unsigned char *)out16, id, (size_t)(id_cap > 0 ? id_cap : 0));
}

/* Unwrap one savedata file. The plaintext is in apw_psp_out(). */
EMSCRIPTEN_KEEPALIVE
int apw_psp_decrypt(const char *sfo, int sfo_len,
                    const char *in, int in_len, const char *key)
{
    unsigned char *out;
    size_t want, got = 0;
    int rc;

    apctl_set_log_sink(log_sink, NULL);
    psp_out_free();

    if (!sfo || sfo_len <= 0 || !in || in_len <= 0 || !key) return APSP_ERR_ARG;

    want = apsp_decrypted_size((size_t)in_len);
    if (!want) return APSP_ERR_SIZE;

    out = psp_out_alloc(want);
    if (!out) return APSP_ERR_MEM;

    rc = apsp_decrypt((const unsigned char *)sfo, (size_t)sfo_len,
                      (const unsigned char *)in, (size_t)in_len,
                      (const unsigned char *)key, out, want, &got);
    if (rc != APSP_OK) psp_out_free();
    else g_psp_out_len = got;
    return rc;
}

/*
 * Wrap one back up. The ciphertext is in apw_psp_out(); the PARAM.SFO at `sfo`
 * has been REWRITTEN in place and the caller must keep both -- a save put back
 * with a stale PARAM.SFO does not load.
 */
EMSCRIPTEN_KEEPALIVE
int apw_psp_encrypt(char *sfo, int sfo_len, const char *name,
                    const char *in, int in_len, const char *key)
{
    unsigned char *out;
    size_t want, got = 0;
    int rc;

    apctl_set_log_sink(log_sink, NULL);
    psp_out_free();

    if (!sfo || sfo_len <= 0 || !name || !in || in_len <= 0 || !key)
        return APSP_ERR_ARG;

    want = apsp_encrypted_size((size_t)in_len);
    if (!want) return APSP_ERR_SIZE;

    out = psp_out_alloc(want);
    if (!out) return APSP_ERR_MEM;

    rc = apsp_encrypt((unsigned char *)sfo, (size_t)sfo_len, name,
                      (const unsigned char *)in, (size_t)in_len,
                      (const unsigned char *)key, out, want, &got);
    if (rc != APSP_OK) psp_out_free();
    else g_psp_out_len = got;
    return rc;
}

/* Regenerate the PARAM.SFO hashes in place, touching no data file. */
EMSCRIPTEN_KEEPALIVE
int apw_psp_resign(char *sfo, int sfo_len)
{
    apctl_set_log_sink(log_sink, NULL);
    psp_out_free();

    if (!sfo || sfo_len <= 0) return APSP_ERR_ARG;
    return apsp_resign((unsigned char *)sfo, (size_t)sfo_len);
}
