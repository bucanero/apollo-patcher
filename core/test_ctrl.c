/*
 * Headless checks for the shared core, so CI exercises it without a GUI.
 *
 *   test_ctrl <file.savepatch>   list its codes, like `patcher <file>`
 *   test_ctrl --db [query]       open the bundled database, report what it
 *                                holds, and read one patch back out of it
 *   test_ctrl --edit <file>      exercise the session-only code editor
 *   test_ctrl --export <file>    edit a code, write the patch back out, and
 *                                reload it to prove the round trip
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "apollo_ctrl.h"
#include "patchdb.h"

static void log_sink(void *ud, const char *line) { (void)ud; printf("- %s\n", line); }

static const char *type_name(int t) {
    switch (t) {
        case APOLLO_CODE_BSD:    return "BSD";
        case APOLLO_CODE_PYTHON: return "Python";
        case APOLLO_CODE_SAVEWIZARD: return "Save Wizard";
        default: return "Unknown";
    }
}

/* Exercises the zip reader end to end: index, entry read, parse, extraction. */
static int run_db(const char *query)
{
    patchdb_t *db = patchdb_open(NULL);
    if (!db) {
        fprintf(stderr, "patchdb: %s\n", patchdb_last_error());
        return 1;
    }

    const int n = patchdb_count(db);
    printf("database: %s\n", patchdb_path(db));
    printf("patches:  %d\n", n);

    /* Per-platform tally, so a bundle missing a whole platform is obvious. */
    for (int i = 0; i < n; i++) {
        const char *plat = patchdb_at(db, i)->platform;
        int first = 1, count = 0;
        for (int j = 0; j < n; j++) {
            if (strcmp(patchdb_at(db, j)->platform, plat) != 0) continue;
            if (j < i) { first = 0; break; }
            count++;
        }
        if (first) printf("  %-4s %d\n", plat, count);
    }

    int found = -1;
    for (int i = 0; i < n && found < 0; i++) {
        const patchdb_entry_t *e = patchdb_at(db, i);
        if (!query || strstr(e->title_id, query) || strstr(e->name, query))
            found = i;
    }
    if (found < 0) {
        fprintf(stderr, "no patch matching '%s'\n", query ? query : "");
        patchdb_close(db);
        return 1;
    }

    const patchdb_entry_t *e = patchdb_at(db, found);
    printf("\nreading %s/%s  (%s)\n", e->platform, e->title_id, e->name);

    char  *buf = NULL;
    size_t len = 0;
    if (!patchdb_read(db, found, &buf, &len)) {
        fprintf(stderr, "could not read that entry out of the archive\n");
        patchdb_close(db);
        return 1;
    }
    printf("  %zu bytes, first line: %.*s\n", len,
           (int)strcspn(buf, "\r\n"), buf);

    /* The bytes must be parseable by the engine, which is the whole point. */
    apctl_session_t *s = apctl_open_buffer(buf, len, e->title_id);
    free(buf);
    if (!s) {
        fprintf(stderr, "the engine could not parse it\n");
        patchdb_close(db);
        return 1;
    }
    printf("  parsed %d codes, game: %s\n", apctl_code_count(s), apctl_game_name(s));
    apctl_close(s);

    const char *cache = patchdb_cache_dir();
    if (cache) {
        int written = patchdb_extract_python(db, cache);
        printf("\npython modules extracted: %d -> %spython\n", written, cache);
        if (written <= 0) { patchdb_close(db); return 1; }
    }

    patchdb_close(db);
    return 0;
}

/* Patch a fresh zero-filled file with `body` and read the result back. */
static int run_one(apctl_session_t *s, apctl_code_t *c, const char *path,
                   const char *body, uint8_t *out, size_t out_len)
{
    uint8_t  zeros[32] = {0};
    uint8_t *patched = NULL;
    size_t   len = 0;

    if (!apctl_set_code_text(c, body)) return 0;
    if (write_buffer(path, zeros, sizeof zeros) != 0) return 0;
    if (!apctl_apply(s, c, path)) return 0;
    if (read_buffer(path, &patched, &len) != 0) return 0;

    memcpy(out, patched, len < out_len ? len : out_len);
    free(patched);
    return 1;
}

/*
 * The View/Edit window's contract, headless: an edit replaces the body the
 * engine will run, survives being applied, and can be taken back.
 */
static int run_edit(const char *path)
{
    apctl_session_t *s = apctl_open_file(path);
    if (!s) { fprintf(stderr, "Could not open %s\n", path); return 1; }

    apctl_code_t *c = NULL;
    for (int i = 0; i < apctl_code_count(s) && !c; i++) {
        apctl_code_t *r = apctl_code_at(s, i);
        if (apctl_code_text(r)[0]) c = r;
    }
    if (!c) { fprintf(stderr, "no code with a body in %s\n", path); apctl_close(s); return 1; }

    char *original = strdup(apctl_code_text(c));
    int   fails = 0;

    printf("editing code %d: %s (%zu bytes)\n", c->id, c->name, strlen(original));

/* One evaluation: half of these conditions call into the editor. */
#define CHECK(what, cond) do { \
        int ok_ = (cond) ? 1 : 0; \
        printf("  %-34s %s\n", what, ok_ ? "ok" : "FAILED"); \
        fails += !ok_; \
    } while (0)

    CHECK("starts unedited", !apctl_code_is_edited(c));

    CHECK("set_code_text stored", apctl_set_code_text(c, "80001000 0000FFFF\n"));
    CHECK("body is the edit", strcmp(apctl_code_text(c), "80001000 0000FFFF\n") == 0);
    CHECK("marked edited", apctl_code_is_edited(c));
    CHECK("type unchanged by an edit", c->type == c->raw->type);

    /* A second edit must free the first, not the original. */
    CHECK("second edit stored", apctl_set_code_text(c, "; nothing to do\n"));
    CHECK("body is the second edit", strcmp(apctl_code_text(c), "; nothing to do\n") == 0);

    apctl_revert_code(c);
    CHECK("revert restores the file's body", strcmp(apctl_code_text(c), original) == 0);
    CHECK("revert clears the flag", !apctl_code_is_edited(c));

    /* ---- type selector ---- */
    const int declared = c->type;
    const int other    = (declared == APOLLO_CODE_BSD) ? APOLLO_CODE_SAVEWIZARD
                                                       : APOLLO_CODE_BSD;

    CHECK("set_code_type accepted", apctl_set_code_type(c, other));
    CHECK("row type followed", c->type == other);
    CHECK("engine entry type followed", c->raw->type == other);
    CHECK("a type change counts as edited", apctl_code_is_edited(c));

    CHECK("unknown type refused", !apctl_set_code_type(c, 42));
    CHECK("refusal changed nothing", c->type == other);

    CHECK("back to the declared type", apctl_set_code_type(c, declared));
    CHECK("declared type is not an edit", !apctl_code_is_edited(c));

    apctl_set_code_type(c, other);
    apctl_revert_code(c);
    CHECK("revert restores the type", c->type == declared && c->raw->type == declared);

    /* Editing to the original text is not an edit. */
    CHECK("identical text is not an edit",
          apctl_set_code_text(c, original) && !apctl_code_is_edited(c));

    /* Emptying a body and filling it again must track the EMPTY flag, which is
     * what greys a code out in the front-ends. */
    apctl_set_code_text(c, "");
    CHECK("emptied body -> EMPTY flag", (c->flags & APOLLO_CODE_FLAG_EMPTY) != 0);
    apctl_set_code_text(c, "80001000 0000FFFF\n");
    CHECK("refilled body -> flag cleared", (c->flags & APOLLO_CODE_FLAG_EMPTY) == 0);

    /*
     * The point of the whole feature: what runs is the edited body, and it
     * still runs the second time. Save Wizard codes only, so the check does
     * not depend on which patch file was passed in — "20000004 12345678"
     * writes a 32-bit value at offset 4.
     */
    apctl_code_t *sw = NULL;
    for (int i = 0; i < apctl_code_count(s) && !sw; i++) {
        apctl_code_t *r = apctl_code_at(s, i);
        if (r->type == APOLLO_CODE_SAVEWIZARD && apctl_code_text(r)[0]) sw = r;
    }
    if (sw) {
        char path[512];
        snprintf(path, sizeof path, "%s/apollo_edit_%d.bin",
                 getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
                 (int)getpid());

        uint8_t first[32], again[32], other2[32];

        CHECK("apply edit A", run_one(s, sw, path, "20000004 12345678", first, sizeof first));
        CHECK("apply edit A again", run_one(s, sw, path, "20000004 12345678", again, sizeof again));
        CHECK("same edit -> same bytes", memcmp(first, again, sizeof first) == 0);
        CHECK("edit A changed the file", memcmp(first, "\0\0\0\0\0\0\0\0", 8) != 0);

        CHECK("apply edit B", run_one(s, sw, path, "20000004 000000FF", other2, sizeof other2));
        CHECK("a different edit -> different bytes", memcmp(first, other2, sizeof first) != 0);

        /* The type decides which interpreter reads the body: the same Save
         * Wizard line means nothing to the BSD parser, so the write goes away
         * and comes back when the type does. */
        uint8_t as_bsd[32], as_sw[32];
        apctl_set_code_type(sw, APOLLO_CODE_BSD);
        CHECK("apply as BSD", run_one(s, sw, path, "20000004 12345678", as_bsd, sizeof as_bsd));
        CHECK("a SW line means nothing as BSD", memcmp(as_bsd, first, sizeof first) != 0);

        apctl_set_code_type(sw, APOLLO_CODE_SAVEWIZARD);
        CHECK("apply as Save Wizard again", run_one(s, sw, path, "20000004 12345678", as_sw, sizeof as_sw));
        CHECK("type back -> the write is back", memcmp(as_sw, first, sizeof first) == 0);

        remove(path);
    } else {
        printf("  (no Save Wizard code here, skipped the apply checks)\n");
    }

#undef CHECK

    free(original);
    apctl_close(s);   /* frees both the edit and the parked original */

    printf("\nedit checks: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}

/*
 * Saving an edited patch. Two separate questions:
 *
 *   1. does the file the user gets back carry the edit AND everything the
 *      parser throws away? (checked against the real file passed in)
 *   2. what happens when the edit cannot be written down at all? (checked
 *      against a synthetic patch, so the headers are known exactly)
 */
static int g_fails;

#define CHECK(what, cond) do { \
        int ok_ = (cond) ? 1 : 0; \
        printf("  %-42s %s\n", what, ok_ ? "ok" : "FAILED"); \
        g_fails += !ok_; \
    } while (0)

/* How many codes would come back different. `text` receives the exported
 * patch when the caller wants to look at what was written. */
static int export_misses(apctl_session_t *s, const char *original, size_t len,
                         char **text)
{
    size_t out_len = 0;
    char  *out = apctl_export_patch(s, original, len, &out_len);
    int    rows[8];
    int    n = out ? apctl_export_mismatches(s, out, out_len, rows, 8) : -1;

    if (text) *text = out;
    else      free(out);
    return n;
}

/*
 * What a .savepatch can and cannot state about a type, on a patch whose
 * headers are known exactly.
 *
 * All three types have a title prefix now ([SW:], [BSD:], [PYTHON:]), so a
 * forced type survives a save -- except on a title whose single prefix slot is
 * already spent, which is the one case the caller still has to be told about.
 */
static void check_format_limits(void)
{
    static const char PATCH[] =
        ";TEST00001\n"
        ";Synthetic patch\n"
        ":SAVE.DAT\n"
        "[Plain code]\n"
        "20000004 12345678\n"
        "\n"
        "[DEFAULT:Preselected]\n"
        "20000008 00000001\n"
        "\n"
        "[PYTHON:A script]\n"
        "print('hi')\n";
    const size_t LEN = sizeof PATCH - 1;

    apctl_session_t *s = apctl_open_buffer(PATCH, LEN, "synthetic");
    if (!s) { printf("  synthetic patch did not parse -- FAILED\n"); g_fails++; return; }

    apctl_code_t *plain  = apctl_code_at(s, 0);
    apctl_code_t *preset = apctl_code_at(s, 1);
    apctl_code_t *script = apctl_code_at(s, 2);
    char *out = NULL;

    /* A type the body already implies needs no prefix: writing one would be
     * noise, and would display wrong on engines older than [SW:]/[BSD:]. */
    apctl_set_code_text(plain, "20000004 0000FFFF\n");
    CHECK("body-implied type survives", export_misses(s, PATCH, LEN, &out) == 0);
    CHECK("...without stating it", out && strstr(out, "[SW:") == NULL);
    free(out); out = NULL;
    apctl_revert_code(plain);

    /* Forced against the body's shape, both directions. */
    apctl_set_code_type(plain, APOLLO_CODE_BSD);
    CHECK("forced BSD on a SW body survives", export_misses(s, PATCH, LEN, &out) == 0);
    CHECK("...as [BSD:Plain code]", out && strstr(out, "[BSD:Plain code]") != NULL);
    free(out); out = NULL;
    apctl_revert_code(plain);

    apctl_set_code_text(plain, "set [x]:0\n");
    apctl_set_code_type(plain, APOLLO_CODE_SAVEWIZARD);
    CHECK("forced Save Wizard on a BSD body survives",
          export_misses(s, PATCH, LEN, &out) == 0);
    CHECK("...as [SW:Plain code]", out && strstr(out, "[SW:Plain code]") != NULL);
    free(out); out = NULL;
    apctl_revert_code(plain);

    /* Python is stated the same way, and dropping it again removes it. */
    apctl_set_code_text(plain, "print('hi')\n");
    apctl_set_code_type(plain, APOLLO_CODE_PYTHON);
    CHECK("[Plain] -> Python survives", export_misses(s, PATCH, LEN, &out) == 0);
    CHECK("...as [PYTHON:Plain code]", out && strstr(out, "[PYTHON:Plain code]") != NULL);
    free(out); out = NULL;
    apctl_revert_code(plain);

    apctl_set_code_text(script, "20000004 12345678\n");
    apctl_set_code_type(script, APOLLO_CODE_SAVEWIZARD);
    CHECK("[PYTHON:] -> Save Wizard survives", export_misses(s, PATCH, LEN, &out) == 0);
    CHECK("...by dropping the prefix", out && strstr(out, "[A script]") != NULL);
    free(out); out = NULL;
    apctl_revert_code(script);

    /* The one case left: [DEFAULT:...] has spent its prefix slot. */
    apctl_set_code_text(preset, "print('hi')\n");
    apctl_set_code_type(preset, APOLLO_CODE_PYTHON);
    CHECK("[DEFAULT:] -> Python still reported unwritable",
          export_misses(s, PATCH, LEN, NULL) == 1);
    apctl_revert_code(preset);

    CHECK("reverted session exports clean", export_misses(s, PATCH, LEN, NULL) == 0);
    apctl_close(s);
}

static int run_export(const char *path, const char *save_to)
{
    uint8_t *raw = NULL;
    size_t   raw_len = 0;
    if (read_buffer(path, &raw, &raw_len) != 0) {
        fprintf(stderr, "Could not read %s\n", path);
        return 1;
    }

    apctl_session_t *s = apctl_open_buffer((char *)raw, raw_len, path);
    if (!s) { fprintf(stderr, "Could not parse %s\n", path); free(raw); return 1; }

    /* Edit the first code that has a body. The replacement keeps the shape
     * the loader reads its type from, so this measures the exporter rather
     * than the format limit checked separately above. */
    apctl_code_t *c = NULL;
    for (int i = 0; i < apctl_code_count(s) && !c; i++)
        if (apctl_code_text(apctl_code_at(s, i))[0]) c = apctl_code_at(s, i);

    if (!c) { fprintf(stderr, "no code with a body\n"); apctl_close(s); free(raw); return 1; }

    const char *body = (c->type == APOLLO_CODE_SAVEWIZARD) ? "80001000 0000FFFF\n"
                                                           : "set [edited]:0\n";
    char edit[128];
    snprintf(edit, sizeof edit, "%s; a comment the user typed\n", body);

    const int n = apctl_code_count(s);
    char **before = calloc(n, sizeof(char *));
    for (int i = 0; i < n; i++) before[i] = strdup(apctl_code_text(apctl_code_at(s, i)));

    apctl_set_code_text(c, edit);

    size_t len = 0;
    char  *out = apctl_export_patch(s, (const char *)raw, raw_len, &len);

    printf("exporting %s\n  %zu bytes in, %zu out, edited code %d (%s)\n",
           path, raw_len, len, c->id, type_name(c->type));

    CHECK("export produced text", out && len);
    if (!out) { apctl_close(s); free(raw); return 1; }

    /* What the parser drops has to survive. The first line is the title ID
     * comment every patch in the database starts with. */
    size_t k = strcspn((const char *)raw, "\r\n");
    CHECK("first line preserved", len > k && memcmp(out, raw, k) == 0);
    CHECK("typed comment kept in the file", strstr(out, "a comment the user typed") != NULL);

    apctl_session_t *back = apctl_open_buffer(out, len, "exported");
    CHECK("exported text parses", back != NULL);

    if (back) {
        CHECK("same number of codes", apctl_code_count(back) == n);

        int diffs = 0, names = 0;
        for (int i = 0; i < n && i < apctl_code_count(back); i++) {
            apctl_code_t *r = apctl_code_at(back, i);
            const char *want = (apctl_code_at(s, i) == c) ? body : before[i];
            const char *got  = apctl_code_text(r);
            size_t a = strlen(want), bl = strlen(got);

            if (strcmp(apctl_code_at(s, i)->name, r->name) != 0) names++;
            while (a && want[a - 1] == '\n') a--;
            while (bl && got[bl - 1] == '\n') bl--;
            if (a != bl || memcmp(want, got, a) != 0) diffs++;
        }
        CHECK("every code name unchanged", names == 0);
        CHECK("every body round-trips (edit included)", diffs == 0);
        apctl_close(back);
    }

    int rows[8];
    int miss = apctl_export_mismatches(s, out, len, rows, 8);
    CHECK("nothing reported as lost", miss == 0);
    for (int i = 0; i < miss && i < 8; i++) {
        apctl_code_t *m = apctl_code_at(s, rows[i]);
        printf("    row %d \"%s\" (%s) reads back differently\n",
               rows[i], m->name, type_name(m->type));
    }

    /* Somewhere to look when a patch does not round-trip. */
    if (save_to) {
        if (write_buffer(save_to, (const uint8_t *)out, len) == 0)
            printf("  wrote the exported patch to %s\n", save_to);
        else
            printf("  could not write %s\n", save_to);
    }

    check_format_limits();

    for (int i = 0; i < n; i++) free(before[i]);
    free(before);
    free(out);
    apctl_close(s);
    free(raw);

    printf("\nexport checks: %s\n", g_fails ? "FAILED" : "all passed");
    return g_fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--db") == 0)
        return run_db(argc >= 3 ? argv[2] : NULL);

    if (argc >= 3 && strcmp(argv[1], "--export") == 0)
        return run_export(argv[2], argc >= 4 ? argv[3] : NULL);

    if (argc >= 3 && strcmp(argv[1], "--edit") == 0)
        return run_edit(argv[2]);

    if (argc < 2) {
        fprintf(stderr, "usage: %s file.savepatch\n", argv[0]);
        fprintf(stderr, "       %s --db [query]\n", argv[0]);
        fprintf(stderr, "       %s --edit file.savepatch\n", argv[0]);
        fprintf(stderr, "       %s --export file.savepatch\n", argv[0]);
        return 2;
    }

    /* Quiet by default: the listing is meant to be diffed against
     * `patcher <file>`, and the engine's log would pollute that. Set
     * APOLLO_TEST_VERBOSE=1 to route it through the sink instead -- same
     * variable the tests/ suite uses. */
    apctl_set_log_sink(getenv("APOLLO_TEST_VERBOSE") ? log_sink : NULL, NULL);

    apctl_session_t *s = apctl_open_file(argv[1]);
    if (!s) { fprintf(stderr, "Could not open %s\n", argv[1]); return 1; }

    printf("Game: %s\n\n", apctl_game_name(s));
    int n = apctl_code_count(s);
    for (int i = 0; i < n; i++) {
        apctl_code_t *c = apctl_code_at(s, i);
        const char *grp = c->is_parent ? "# " : (c->is_child ? "+--- " : "");
        char info[8] = "";
        if (c->flags & APOLLO_CODE_FLAG_ALERT)    snprintf(info, sizeof info, "[!] ");
        else if (c->flags & APOLLO_CODE_FLAG_EMPTY)    snprintf(info, sizeof info, "[E] ");
        else if (c->flags & APOLLO_CODE_FLAG_DISABLED) snprintf(info, sizeof info, "[D] ");
        else if (c->flags & APOLLO_CODE_FLAG_REQUIRED) snprintf(info, sizeof info, "[R] ");
        printf("%4d. %s%s%s  (%s%s)\n", c->id, grp, info, c->name,
               type_name(c->type), c->options_count ? ", has options" : "");
    }
    printf("\nParse completed: %d codes\n", n);
    apctl_close(s);
    return 0;
}
