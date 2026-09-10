/*
 * apollo_ctrl - stdio-free controller wrapper around libapollo.
 *
 * This is the shared "engine facade" used by both the GUI and (optionally)
 * a rewritten CLI. It contains NO printf/scanf: all output goes through a
 * log callback, and interactive code "options" are resolved by the caller
 * setting a selection index before applying.
 *
 * The heavy lifting (parsing, patch application, crypto) stays in libapollo;
 * this file only adapts its data model into a UI-friendly, callback-driven API.
 *
 * Everything this facade owns is prefixed apctl_. The apollo_ prefix belongs
 * to libapollo alone, so that a call site reading apctl_apply() vs
 * apollo_apply_code() says which layer it is talking to.
 */
#ifndef APCTL_H
#define APCTL_H

#include <stddef.h>
#include "apollo.h"   /* C++-safe: the code_entry_t type and APOLLO_CODE_* /
                         APOLLO_CODE_FLAG_* constants come straight from here. */

#ifdef __cplusplus
extern "C" {
#endif

/* Log sink. `line` is a single, already-formatted message (no trailing newline
 * guaranteed). `ud` is the opaque pointer passed to apctl_set_log_sink(). */
typedef void (*apctl_log_fn)(void *ud, const char *line);

/* Install a process-wide log sink. libapollo emits progress through the global
 * dbglogger_log() symbol (defined in apollo_ctrl.c); this routes it to `fn`.
 * Pass (NULL, NULL) to silence. Not thread-safe; set it before applying.
 *
 * The sink always receives one complete line with no trailing newline —
 * including MicroPython print() output, which apollo_ctrl.c reassembles from
 * the fragments the interpreter emits. */
void apctl_set_log_sink(apctl_log_fn fn, void *ud);

/* Emit any partial MicroPython print() line still buffered. apctl_apply()
 * already does this when a code finishes, so front-ends rarely need it. */
void apctl_log_flush(void);

/* One code row, flattened for display. Pointers are owned by the session and
 * remain valid until apctl_close(). `raw` is the underlying engine entry,
 * needed for option queries and apply. */
typedef struct {
    int            id;             /* 1-based position, matches CLI numbering  */
    int            type;           /* APOLLO_CODE_* (SaveWizard/BSD/Python)    */
    int            flags;          /* APOLLO_CODE_FLAG_* bitmask               */
    int            is_parent;      /* group header                            */
    int            is_child;       /* member of the group above               */
    int            activated;      /* [DEFAULT:] codes start pre-selected     */
    int            options_count;  /* >0 => needs option selection before apply*/
    const char    *name;
    const char    *file;           /* target-file hint from the patch         */
    code_entry_t  *raw;
} apctl_code_t;

typedef struct apctl_session apctl_session_t;

/* Parse a .savepatch. Returns NULL on read/parse failure. */
apctl_session_t *apctl_open_file(const char *path);
apctl_session_t *apctl_open_buffer(const char *buf, size_t len, const char *name);

const char      *apctl_game_name(apctl_session_t *s);
int              apctl_code_count(apctl_session_t *s);          /* excludes header */
apctl_code_t    *apctl_code_at(apctl_session_t *s, int index);  /* 0-based         */

/* Raw code/script body (Save Wizard lines, BSD commands, or Python source).
 * Owned by the session; may be empty for header/group entries. */
const char *apctl_code_text(const apctl_code_t *c);

/* ---- Interactive options (dropdowns) ---- */
int          apctl_opt_group_count(const apctl_code_t *c);
const char  *apctl_opt_tag(const apctl_code_t *c, int group);
int          apctl_opt_value_count(const apctl_code_t *c, int group);
const char  *apctl_opt_value_name(const apctl_code_t *c, int group, int idx);
int          apctl_opt_get_selected(const apctl_code_t *c, int group);
void         apctl_opt_set_selected(apctl_code_t *c, int group, int idx);

/* ---- Host data path ---- */
/* Directory libapollo resolves host paths against, WITH a trailing separator:
 * it builds the MicroPython import path as this + "python". Without it, the
 * engine looks for "python" relative to the process's working directory, so a
 * Python code that imports a helper module only works when the app happens to
 * be launched from the right place.
 *
 * Pass NULL or "" to restore that default. */
void        apctl_set_data_path(const char *dir);
const char *apctl_get_data_path(void);

/* ---- Byte order for a patch ---- */
/*
 * PS3 is the only big-endian platform Apollo covers. The engine does accept a
 * per-code [BE:...] / [LE:...] header (see loader.c), but no patch in the
 * database uses one, so EVERY PS3 patch depends on the caller selecting the
 * mode. A front-end that loads a PS3 patch should therefore turn it on rather
 * than let someone silently patch a save with the wrong byte order.
 *
 * Two sources, in order of trust:
 *
 *   platform    the patch database's own tag ("PS3", "PS4", ...). Authoritative
 *               when the patch came from there: it is the directory the file
 *               lives in, which is how the database is organised and where the
 *               index gets it. Pass NULL when there is none.
 *   title_text  a title ID, file name ("BLUS30279.savepatch") or header line
 *               ("; BLUS30279"). The fallback for a loose file, where there is
 *               no directory to consult — matched against the known PS3 title
 *               prefixes.
 *
 * Returns 0 when neither says anything recognisable, so an unknown patch keeps
 * the little-endian default.
 */
int apctl_is_big_endian_for(const char *platform, const char *title_text);

/* The prefix match on its own. Exposed mainly for testing; callers that have a
 * platform tag should use apctl_is_big_endian_for(), which prefers it. */
int apctl_title_is_big_endian(const char *text);

/* ---- Data endianness ---- */
/* Select the byte order the engine uses for save DATA (the CLI's -b/-l flags).
 * Non-zero selects big-endian (PS3/PPU saves), zero returns to the host's
 * native order. The engine setting is global and is cleared by
 * apollo_free_var_list(), so apctl_apply() re-asserts it before every code —
 * set this once and it stays in effect for the whole session. */
void apctl_set_big_endian(int enabled);
int  apctl_get_big_endian(void);

/* Apply one code to `target_file`. Returns 1 on success, 0 on error.
 * Progress is emitted through the installed log sink. If the code has options,
 * set them via apctl_opt_set_selected() first. */
int  apctl_apply(apctl_session_t *s, apctl_code_t *c, const char *target_file);

/* Release engine patch-variable state accumulated across apply() calls.
 * Call once after a batch of applies (mirrors apollo_free_var_list()). */
void apctl_reset_vars(void);

void apctl_close(apctl_session_t *s);

#ifdef __cplusplus
}
#endif

#endif /* APCTL_H */
