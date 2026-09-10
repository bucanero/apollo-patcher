#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "apollo.h"        /* full engine types (code_entry_t, list_t, ...) */
#include "apollo_ctrl.h"

/* ---------------------------------------------------------------------------
 * Log plumbing.
 * libapollo calls dbglogger_log() for progress (see source/loader.c: #define
 * LOG dbglogger_log). The CLI defined it to printf; we route it to a sink.
 * ------------------------------------------------------------------------- */
static apctl_log_fn g_log_fn = NULL;
static void         *g_log_ud = NULL;

void apctl_set_log_sink(apctl_log_fn fn, void *ud)
{
    g_log_fn = fn;
    g_log_ud = ud;
}

/* Symbol required by libapollo. Kept variadic to match its call sites. */
void dbglogger_log(const char *fmt, ...)
{
    if (!g_log_fn)
        return;

    char buffer[0x800];
    va_list arg;
    va_start(arg, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, arg);
    va_end(arg);

    g_log_fn(g_log_ud, buffer);
}

/* MicroPython print() sink (used when the engine is built WITHOUT -DAPOLLO_CLI,
 * which is what we want for a UI: Python output lands in the log panel, not on
 * stdout).
 *
 * Unlike dbglogger_log(), this arrives in fragments: MicroPython emits every
 * print() argument, every separator and the trailing newline as its own call,
 * so `print("size:", 483)` would reach a sink as four entries with the number
 * detached from its label. Buffer here and emit whole lines, so the sink
 * contract is identical for both functions: one complete line, no trailing
 * newline. */
static char   g_pbuf[0x800];
static size_t g_plen = 0;

static void print_flush(void)
{
    if (!g_plen)
        return;

    g_pbuf[g_plen] = '\0';
    g_plen = 0;
    if (g_log_fn)
        g_log_fn(g_log_ud, g_pbuf);
}

void dbglogger_printf(const char *fmt, ...)
{
    if (!g_log_fn)
        return;

    char buffer[0x800];
    va_list arg;
    va_start(arg, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, arg);
    va_end(arg);

    for (const char *c = buffer; *c; c++) {
        if (*c == '\n')
            print_flush();
        else if (g_plen < sizeof(g_pbuf) - 1)
            g_pbuf[g_plen++] = *c;
        else                        /* pathological line: flush and keep going */
            print_flush();
    }
}

void apctl_log_flush(void)
{
    print_flush();
}

/* ---------------------------------------------------------------------------
 * Host callback
 *
 * libapollo asks the host for a few values through this; the only one that
 * matters to a desktop front-end is the data path, which is where Python codes
 * import their helper modules from. Everything else mirrors what libapollo's
 * own dummy callback returns, so behaviour is unchanged when no path is set.
 * ------------------------------------------------------------------------- */
static char g_data_path[1024] = "";

void apctl_set_data_path(const char *dir)
{
    if (dir && *dir)
        snprintf(g_data_path, sizeof(g_data_path), "%s", dir);
    else
        g_data_path[0] = '\0';
}

const char *apctl_get_data_path(void) { return g_data_path; }

static void *apctl_host_cb(int info, uint32_t *size)
{
    switch (info) {
    case APOLLO_HOST_TEMP_PATH:
    case APOLLO_HOST_DATA_PATH:
        if (size) *size = (uint32_t)strlen(g_data_path);
        return g_data_path;

    case APOLLO_HOST_USERNAME:
    case APOLLO_HOST_SYS_NAME:
    case APOLLO_HOST_LAN_ADDR:
    case APOLLO_HOST_WLAN_ADDR:
    case APOLLO_HOST_ACCOUNT_ID:
        if (size) *size = 6;
        return (void *)"APOLLO";
    }

    if (size) *size = 1;
    return (void *)"";
}

/* ---------------------------------------------------------------------------
 * Byte order for a patch (see apctl_is_big_endian_for)
 *
 * The PS3 title-ID prefixes, taken from the platform directories of
 * bucanero/apollo-patches. No prefix there is shared with PS2, PS4, PSP or PS
 * Vita, so a four-letter match is unambiguous. PSP's NPJH/NPUH are close to
 * PS3's NPJA/NPJB/NPJD/NPUA/NPUB, which is why the whole four letters matter.
 * ------------------------------------------------------------------------- */
static const char *const PS3_PREFIXES[] = {
    "BCAS", "BCES", "BCJS", "BCUS", "BLAS", "BLES", "BLET", "BLJM", "BLJS",
    "BLKS", "BLUS", "CPCS", "GUST", "MRTC", "NPEA", "NPEB", "NPEJ", "NPEL",
    "NPHB", "NPJA", "NPJB", "NPJD", "NPUA", "NPUB",
};

static int is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* The database's platform tag decides on its own: it is the directory name, so
 * a patch filed under PS3/ is a PS3 patch by definition, whatever its title ID
 * looks like. That also means a title prefix nobody has seen yet needs no code
 * change to be handled correctly. */
int apctl_is_big_endian_for(const char *platform, const char *title_text)
{
    if (platform && *platform)
        return strcmp(platform, "PS3") == 0;

    return apctl_title_is_big_endian(title_text);
}

int apctl_title_is_big_endian(const char *text)
{
    if (!text) return 0;

    /* Walk every position and test for a CCCCNNNNN token. Cheap, and it does
     * not care whether the caller passed a bare ID, a file name or a line of
     * patch text. */
    for (const char *p = text; *p; p++) {
        int shaped = 1;
        for (int i = 0; i < 4 && shaped; i++)
            if (!is_upper(p[i])) shaped = 0;
        for (int i = 4; i < 9 && shaped; i++)
            if (!is_digit(p[i])) shaped = 0;
        if (!shaped) continue;

        for (size_t i = 0; i < sizeof(PS3_PREFIXES) / sizeof(*PS3_PREFIXES); i++)
            if (memcmp(p, PS3_PREFIXES[i], 4) == 0)
                return 1;

        /* A recognisable ID that is not PS3: little-endian, and no point
         * scanning on. */
        return 0;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------- */
struct apctl_session {
    list_t         *codes;      /* head node is the synthetic header entry */
    char           *game_name;
    code_entry_t   *header;
    apctl_code_t  *rows;       /* flattened view, count == n_rows          */
    int             n_rows;
};

/* Build the synthetic header entry + flattened rows, mirroring patcher.c. */
static apctl_session_t *build_session(char *data, size_t len, const char *name)
{
    apctl_session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    /* NUL-terminate the working buffer (apollo_load_code_list mutates it). */
    data = realloc(data, len + 1);
    data[len] = 0;

    code_entry_t *header = calloc(1, sizeof(code_entry_t));
    header->name = (char *)name;
    header->file = (char *)name;

    /* Game name lives after the first ';' up to newline (same as CLI). */
    char *tmp = strchr(data + 1, ';');
    if (tmp) {
        tmp++;
        size_t nlen = strcspn(tmp, "\n");
        s->game_name = malloc(nlen + 1);
        memcpy(s->game_name, tmp, nlen);
        s->game_name[nlen] = '\0';
        for (char *p = s->game_name; p[0]; p++)
            if (*p < ' ') p[0] = ' ';
        header->name = s->game_name;
    }

    s->header = header;
    s->codes  = list_alloc();
    list_append(s->codes, header);
    apollo_load_code_list(data, s->codes, NULL, NULL);
    free(data);

    /* Flatten: skip the header node, number from 1 (CLI-compatible). */
    int total = (int)list_count(s->codes) - 1;
    if (total < 0) total = 0;
    s->rows   = calloc(total ? total : 1, sizeof(apctl_code_t));
    s->n_rows = 0;

    int pos = 1;
    for (list_node_t *node = list_next(list_head(s->codes)); node; node = list_next(node), pos++) {
        code_entry_t *code = list_get(node);
        apctl_code_t *r = &s->rows[s->n_rows++];
        r->id            = pos;
        r->type          = code->type;
        r->flags         = code->flags;
        r->is_parent     = (code->flags & APOLLO_CODE_FLAG_PARENT) ? 1 : 0;
        r->is_child      = (code->flags & APOLLO_CODE_FLAG_CHILD) ? 1 : 0;
        r->activated     = code->activated;
        r->options_count = code->options_count;
        r->name          = code->name;
        r->file          = code->file;
        r->raw           = code;
    }
    return s;
}

apctl_session_t *apctl_open_buffer(const char *buf, size_t len, const char *name)
{
    if (!buf || !len) return NULL;
    char *data = malloc(len);
    if (!data) return NULL;
    memcpy(data, buf, len);
    return build_session(data, len, name ? name : "buffer");
}

apctl_session_t *apctl_open_file(const char *path)
{
    uint8_t *data = NULL;
    size_t   len  = 0;
    if (read_buffer(path, &data, &len) != 0)
        return NULL;
    return build_session((char *)data, len, path);
}

const char *apctl_game_name(apctl_session_t *s)
{
    return s ? (s->game_name ? s->game_name : s->header->name) : "";
}

int apctl_code_count(apctl_session_t *s) { return s ? s->n_rows : 0; }

apctl_code_t *apctl_code_at(apctl_session_t *s, int index)
{
    if (!s || index < 0 || index >= s->n_rows) return NULL;
    return &s->rows[index];
}

const char *apctl_code_text(const apctl_code_t *c)
{
    if (!c || !c->raw || !c->raw->codes) return "";
    return c->raw->codes;
}

/* ---- editing a code body ---- */

/* The loader flags a code EMPTY when its body has no characters, and
 * front-ends use that to grey it out. An edit can cross that line in either
 * direction, so re-derive it with the loader's own rule. */
static void refresh_empty_flag(apctl_code_t *c)
{
    const char *body = c->raw->codes;

    if (body && body[0]) c->raw->flags &= ~APOLLO_CODE_FLAG_EMPTY;
    else                 c->raw->flags |= APOLLO_CODE_FLAG_EMPTY;

    c->flags = c->raw->flags;
}

int apctl_set_code_text(apctl_code_t *c, const char *text)
{
    char *copy;

    if (!c || !c->raw || !text) return 0;

    /* Nothing actually changed: a front-end hands us the whole buffer on every
     * save, so this is the common case when someone opens the window, looks,
     * and saves anyway. Leaving the flag alone keeps it honest. */
    if (c->raw->codes && strcmp(text, c->raw->codes) == 0) return 1;

    /* Back to what the file said: drop the edit rather than store an identical
     * copy, so `edited` keeps meaning "differs from the patch file". */
    if (c->edited && c->orig_text && strcmp(text, c->orig_text) == 0) {
        apctl_revert_code(c);
        return 1;
    }

    copy = strdup(text);
    if (!copy) return 0;

    if (c->edited) {
        free(c->raw->codes);        /* a previous edit; the original is safe */
    } else {
        c->orig_text = c->raw->codes;   /* first edit: take ownership of it */
        c->edited = 1;
    }

    c->raw->codes = copy;
    refresh_empty_flag(c);
    return 1;
}

int apctl_set_code_type(apctl_code_t *c, int type)
{
    if (!c || !c->raw) return 0;
    if (type != APOLLO_CODE_SAVEWIZARD && type != APOLLO_CODE_BSD &&
        type != APOLLO_CODE_PYTHON)
        return 0;

    if (!c->type_edited) {
        c->orig_type   = c->raw->type;
        c->type_edited = 1;
    }

    c->raw->type = (uint8_t)type;
    c->type      = type;

    /* Back to what the patch declared: stop calling it a change, so the
     * edited flag keeps meaning "differs from the patch file". */
    if (type == c->orig_type) c->type_edited = 0;
    return 1;
}

int apctl_code_is_edited(const apctl_code_t *c)
{
    return (c && (c->edited || c->type_edited)) ? 1 : 0;
}

void apctl_revert_code(apctl_code_t *c)
{
    if (!c) return;

    if (c->edited) {
        free(c->raw->codes);
        c->raw->codes = c->orig_text;
        c->orig_text  = NULL;
        c->edited     = 0;
        refresh_empty_flag(c);
    }

    if (c->type_edited) {
        c->raw->type   = (uint8_t)c->orig_type;
        c->type        = c->orig_type;
        c->type_edited = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Exporting an edited patch
 *
 * The engine's parse is lossy on purpose -- it keeps codes and drops comments,
 * credits, `:file` lines and the {TAG} option blocks -- so writing a patch
 * from the parsed model would hand the user back a poorer file than they
 * opened. Instead the ORIGINAL text is copied through verbatim and only the
 * edited codes' bodies are spliced in.
 *
 * Line roles come from the loader itself (loader.c), using the same
 * wildcard_match() calls, so what counts as a header or as the end of a body
 * cannot drift from what the parser does:
 *
 *   header:      "[*]"  |  "; --- * ---"  |  "GROUP:*"
 *   ends a body: any header, plus a ":file" line, a "PATH:" line, or an
 *                option block (see ends_body() for the patterns themselves)
 *
 * A body is therefore the run of lines between a header and the first of
 * those; comment lines inside it are not part of the code, which is why they
 * survive a splice.
 * ------------------------------------------------------------------------- */

typedef struct {
    char  *p;
    size_t len, cap;
} ebuf_t;

static int eb_room(ebuf_t *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return 1;

    size_t cap = b->cap ? b->cap : 8192;
    while (cap < b->len + need + 1) cap *= 2;

    char *np = realloc(b->p, cap);
    if (!np) return 0;
    b->p = np;
    b->cap = cap;
    return 1;
}

static int eb_add(ebuf_t *b, const char *s, size_t n)
{
    if (!eb_room(b, n)) return 0;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 1;
}

static int eb_str(ebuf_t *b, const char *s) { return eb_add(b, s, strlen(s)); }

/*
 * A line as the parser sees it: trailing blanks and the CR of a CRLF removed.
 *
 * Matching str_rtrim() exactly is what keeps the splice aligned with the
 * parse, so this function follows it rather than doing its own idea of
 * trimming: spaces and tabs, plus the CR that clean_eol() rewrites as a space
 * on a CRLF file. The NULs str_rtrim leaves are turned back into newlines
 * before the bodies are read (loader.c: remove_char), so both of the loader's
 * passes see lines trimmed this way -- and a difference of one character here
 * means counting codes the parser does not have.
 */
static char *line_copy(const char *start, size_t n)
{
    char *s = malloc(n + 1);
    if (!s) return NULL;

    memcpy(s, start, n);
    s[n] = '\0';
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = '\0';
    return s;
}

static int is_code_header(const char *line)
{
    return wildcard_match(line, "[*]") ||
           wildcard_match(line, "; --- * ---") ||
           wildcard_match_icase(line, "GROUP:*");
}

static int ends_body(const char *line)
{
    return is_code_header(line) ||
           wildcard_match(line, ":*") ||
           wildcard_match(line, "{*}*{/*}") ||
           wildcard_match_icase(line, "PATH:*");
}

/* The prefixes the loader accepts inside a "[...]" header, in ITS order. Only
 * the first one is consumed, so "[DEFAULT:PYTHON:x]" is a DEFAULT code named
 * "PYTHON:x" -- which is why a type still cannot always be written down. */
/* Arrays, not pointers: their addresses are compile-time constants, so the
 * table below can be initialised with them and identity comparisons on the
 * returned prefix are meaningful. */
static const char P_DEFAULT[] = "DEFAULT:";
static const char P_INFO[]    = "INFO:";
static const char P_PYTHON[]  = "PYTHON:";
static const char P_SW[]      = "SW:";
static const char P_BSD[]     = "BSD:";
static const char P_LE[]      = "LE:";
static const char P_BE[]      = "BE:";
static const char P_GROUP[]   = "GROUP:";

static const char *const HEADER_PREFIXES[] = {
    P_DEFAULT, P_INFO, P_PYTHON, P_SW, P_BSD, P_LE, P_BE, P_GROUP
};

static const char *header_prefix(const char *line)
{
    if (line[0] != '[') return NULL;

    for (size_t i = 0; i < sizeof(HEADER_PREFIXES) / sizeof(*HEADER_PREFIXES); i++) {
        size_t n = strlen(HEADER_PREFIXES[i]);
        if (strncasecmp(line + 1, HEADER_PREFIXES[i], n) == 0)
            return HEADER_PREFIXES[i];
    }
    return NULL;
}

/* The three prefixes that state a type, as opposed to marking a code default,
 * informational, grouped or byte-ordered. */
static const char *type_prefix(int type)
{
    switch (type) {
        case APOLLO_CODE_PYTHON:     return P_PYTHON;
        case APOLLO_CODE_SAVEWIZARD: return P_SW;
        case APOLLO_CODE_BSD:        return P_BSD;
        default:                     return NULL;
    }
}

static int is_type_prefix(const char *prefix)
{
    return prefix == P_PYTHON || prefix == P_SW || prefix == P_BSD;
}

/*
 * What the loader would make of this body on its own (loader.c,
 * get_patch_code): Save Wizard while every code line is exactly
 * "XXXXXXXX YYYYYYYY", BSD from the first line that is not, and Save Wizard
 * for a body with no code lines at all.
 *
 * Comment lines and blank lines are not code lines -- the parser skips the
 * former and strtok() never yields the latter -- so an edited body's own
 * comments do not decide its type.
 */
static int inferred_type(const char *body)
{
    for (const char *p = body ? body : ""; *p; ) {
        size_t n = strcspn(p, "\n");
        char  *line = line_copy(p, n);
        int    decides = 0;

        if (!line) return APOLLO_CODE_SAVEWIZARD;
        if (line[0] && line[0] != ';')
            decides = !wildcard_match(line, "\?\?\?\?\?\?\?\? \?\?\?\?\?\?\?\?");

        free(line);
        if (decides) return APOLLO_CODE_BSD;
        p += n + (p[n] == '\n' ? 1 : 0);
    }
    return APOLLO_CODE_SAVEWIZARD;
}

/*
 * Write the header for a code whose type was changed.
 *
 * All three types are declarable ("[SW:...]", "[BSD:...]", "[PYTHON:...]"),
 * but the prefix is only written when it is actually needed -- when the body
 * alone would be read as something else. Stating a type that the body already
 * implies would add noise to the file and, worse, make it display wrong on
 * console builds whose engine predates these two prefixes: they would keep the
 * code but call it "SW:Max Money".
 *
 * A title whose single prefix slot is already spent on something else
 * ("[DEFAULT:...]") still cannot say a type; apctl_export_mismatches() is what
 * reports those.
 */
static int emit_header(ebuf_t *b, const char *line, int type, const char *body)
{
    const char *had  = header_prefix(line);
    const char *want = (type == inferred_type(body)) ? NULL : type_prefix(type);

    if (line[0] != '[' || (had && !is_type_prefix(had)))
        return eb_str(b, line);

    /* Skip whatever type prefix is there now; `want` replaces it, or nothing
     * does when the body speaks for itself. */
    const char *rest = line + 1 + (is_type_prefix(had) ? strlen(had) : 0);

    return eb_str(b, "[") && (want ? eb_str(b, want) : 1) && eb_str(b, rest);
}

/* Does the file use CRLF? Length-bounded: patch bytes come straight off disk
 * or out of the zip, with no terminator to stop a strstr(). */
static int uses_crlf(const char *p, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++)
        if (p[i] == '\r' && p[i + 1] == '\n') return 1;

    return 0;
}

char *apctl_export_patch(apctl_session_t *s, const char *original, size_t original_len,
                         size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!s || !original) return NULL;

    /* Emitted lines follow the file's own convention rather than the host's. */
    const char *eol = uses_crlf(original, original_len) ? "\r\n" : "\n";

    ebuf_t b = {0};
    int    row = -1;        /* header occurrence == apctl row index */
    int    in_edit = 0;     /* inside an edited code's body region  */
    int    placed = 0;      /* its replacement body already written  */
    int    ok = 1;

    const char *end = original + original_len;

    for (const char *p = original; p < end && ok; ) {
        const char *nl  = memchr(p, '\n', (size_t)(end - p));
        size_t      raw = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
        size_t      txt = nl ? (size_t)(nl - p) : raw;

        char *line = line_copy(p, txt);
        if (!line) { ok = 0; break; }

        if (is_code_header(line)) {
            /* An edited body with nothing to anchor to (the code was empty, or
             * held only comments) still has to be written before we leave. */
            if (in_edit && !placed) {
                apctl_code_t *prev = apctl_code_at(s, row);
                ok = eb_str(&b, apctl_code_text(prev)) && eb_str(&b, eol);
            }

            row++;
            apctl_code_t *c = apctl_code_at(s, row);
            in_edit = (c && apctl_code_is_edited(c));
            placed  = 0;

            if (ok) ok = in_edit
                       ? (emit_header(&b, line, c->type, apctl_code_text(c)) && eb_str(&b, eol))
                       : eb_add(&b, p, raw);
        }
        else if (in_edit && ends_body(line)) {
            if (!placed) {
                apctl_code_t *c = apctl_code_at(s, row);
                ok = eb_str(&b, apctl_code_text(c)) && eb_str(&b, eol);
            }
            in_edit = 0;
            placed  = 0;
            if (ok) ok = eb_add(&b, p, raw);
        }
        else if (in_edit && !wildcard_match(line, ";*")) {
            /* One of the lines being replaced. The body goes in where the
             * first of them was, so it keeps its place among the comments. */
            if (!placed) {
                apctl_code_t *c = apctl_code_at(s, row);
                const char   *body = apctl_code_text(c);
                size_t        n = strlen(body);

                ok = eb_str(&b, body);
                if (ok && (n == 0 || body[n - 1] != '\n')) ok = eb_str(&b, eol);
                placed = 1;
            }
        }
        else {
            ok = eb_add(&b, p, raw);
        }

        free(line);
        p += raw;
    }

    /* A file that ends inside an edited code. */
    if (ok && in_edit && !placed) {
        apctl_code_t *c = apctl_code_at(s, row);
        ok = eb_str(&b, apctl_code_text(c));
    }

    if (!ok) {
        free(b.p);
        return NULL;
    }
    if (out_len) *out_len = b.len;
    return b.p ? b.p : calloc(1, 1);
}

/*
 * Do these two bodies run the same way?
 *
 * Not a memcmp, for two reasons that are both the parser's doing rather than
 * the exporter's:
 *
 *   - it terminates every line it keeps with a newline, so a body typed
 *     without a final one comes back with it;
 *   - it drops ';' comment lines. Someone who types a comment into a code
 *     gets it written to the file (the splice is verbatim) but not back into
 *     the body, and that is the right outcome -- a comment is not code -- so
 *     it must not be reported as an edit the file failed to carry.
 */
static const char *next_code_line(const char *p, size_t *len)
{
    while (*p) {
        size_t n = strcspn(p, "\n");

        if (p[0] != ';') { *len = n; return p; }
        p += n + (p[n] == '\n' ? 1 : 0);
    }
    return NULL;
}

static int same_body(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";

    for (;;) {
        size_t la = 0, lb = 0;
        const char *ra = next_code_line(a, &la);
        const char *rb = next_code_line(b, &lb);

        if (!ra || !rb) return ra == rb;
        if (la != lb || memcmp(ra, rb, la) != 0) return 0;

        a = ra + la + (ra[la] == '\n' ? 1 : 0);
        b = rb + lb + (rb[lb] == '\n' ? 1 : 0);
    }
}

int apctl_export_mismatches(apctl_session_t *s, const char *exported, size_t len,
                            int *rows, int max)
{
    if (!s || !exported || !len) return 0;

    /* Re-parsing is loud (every option, every code) and this is a background
     * check, so mute the sink for the duration. */
    apctl_log_fn  fn = g_log_fn;
    void         *ud = g_log_ud;
    g_log_fn = NULL;

    apctl_session_t *check = apctl_open_buffer(exported, len, "export-check");

    g_log_fn = fn;
    g_log_ud = ud;

    if (!check) return -1;

    int found = 0;
    int n = apctl_code_count(s);

    for (int i = 0; i < n; i++) {
        apctl_code_t *mine  = apctl_code_at(s, i);
        apctl_code_t *round = apctl_code_at(check, i);

        if (round && mine->type == round->type &&
            same_body(apctl_code_text(mine), apctl_code_text(round)))
            continue;

        if (rows && found < max) rows[found] = i;
        found++;
    }

    apctl_close(check);
    return found;
}

/* ---- options ---- */
int apctl_opt_group_count(const apctl_code_t *c)
{
    return c ? c->raw->options_count : 0;
}

const char *apctl_opt_tag(const apctl_code_t *c, int group)
{
    if (!c || group < 0 || group >= c->raw->options_count) return "";
    const char *line = c->raw->options[group].line;
    return line ? line : "";
}

int apctl_opt_value_count(const apctl_code_t *c, int group)
{
    if (!c || group < 0 || group >= c->raw->options_count) return 0;
    return (int)list_count(c->raw->options[group].opts);
}

const char *apctl_opt_value_name(const apctl_code_t *c, int group, int idx)
{
    if (!c || group < 0 || group >= c->raw->options_count) return "";
    option_value_t *v = list_get_item(c->raw->options[group].opts, idx);
    return (v && v->name) ? v->name : "";
}

int apctl_opt_get_selected(const apctl_code_t *c, int group)
{
    if (!c || group < 0 || group >= c->raw->options_count) return -1;
    return c->raw->options[group].sel;
}

void apctl_opt_set_selected(apctl_code_t *c, int group, int idx)
{
    if (!c || group < 0 || group >= c->raw->options_count) return;
    c->raw->options[group].sel = idx;
}

/* ---- data endianness ---- */
static int g_big_endian = 0;

void apctl_set_big_endian(int enabled)
{
    g_big_endian = enabled ? 1 : 0;
    apollo_set_endianness(g_big_endian ? APOLLO_DATA_MODE_BIG : APOLLO_DATA_MODE_DEFAULT);
}

int apctl_get_big_endian(void) { return g_big_endian; }

/* ---- apply ---- */
int apctl_apply(apctl_session_t *s, apctl_code_t *c, const char *target_file)
{
    (void)s;
    if (!c) return 0;
    const char *target = target_file ? target_file : c->raw->file;
    /* apollo_free_var_list() resets the engine to the host's byte order, and it
     * can run between codes, so re-assert the mode on every apply. */
    apollo_set_endianness(g_big_endian ? APOLLO_DATA_MODE_BIG : APOLLO_DATA_MODE_DEFAULT);

    /* Only take over the host callback once a data path is set, so an
     * embedder that never calls apctl_set_data_path() keeps libapollo's own
     * defaults exactly. */
    int ok = apollo_apply_code(target, c->raw,
                               g_data_path[0] ? apctl_host_cb : NULL) ? 1 : 0;

    /* A Python script's final print() need not end in a newline; emit whatever
     * is still buffered so the caller sees the code's complete output. */
    print_flush();
    return ok;
}

void apctl_reset_vars(void) { apollo_free_var_list(); }

void apctl_close(apctl_session_t *s)
{
    if (!s) return;

    /* The parsed entries and the list go back to the library, which is the
     * only side that knows what apollo_load_code_list() allocated. The header
     * node is ours and stops there: its name/file alias game_name and the
     * caller's label rather than being allocations of their own. */
    if (s->codes)
        apollo_free_code_list(s->codes, list_next(list_head(s->codes)));
    free(s->header);
    /* An edited code parked its original body here; the edit itself is in the
     * entry the library just freed. */
    for (int i = 0; i < s->n_rows; i++)
        free(s->rows[i].orig_text);
    free(s->rows);
    free(s->game_name);
    free(s);
}
