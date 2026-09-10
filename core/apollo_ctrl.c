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
    /* list_free walks the engine's code_entry_t entries; the header is ours. */
    if (s->codes) list_free(s->codes);
    free(s->rows);
    free(s->game_name);
    free(s);
}
