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

static apctl_session_t *g_session = NULL;

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
    if (len <= 0) return 0;

    g_session = apctl_open_buffer(buf, (size_t)len, name);
    return g_session ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void apw_close(void)
{
    if (g_session) {
        apctl_close(g_session);
        g_session = NULL;
    }
    apctl_reset_vars();
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
