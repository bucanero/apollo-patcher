/*
 * Headless checks for the shared core, so CI exercises it without a GUI.
 *
 *   test_ctrl <file.savepatch>   list its codes, like `patcher <file>`
 *   test_ctrl --db [query]       open the bundled database, report what it
 *                                holds, and read one patch back out of it
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--db") == 0)
        return run_db(argc >= 3 ? argv[2] : NULL);

    if (argc < 2) {
        fprintf(stderr, "usage: %s file.savepatch\n", argv[0]);
        fprintf(stderr, "       %s --db [query]\n", argv[0]);
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
