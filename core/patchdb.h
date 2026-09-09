/*
 * patchdb - read-only access to the bundled patch database.
 *
 * The database is apollo-patches.zip (see tools/make-bundle.sh): ~2240
 * .savepatch files, the Python helper modules, and a generated index.tsv.
 * Patches are read straight out of the archive, so there is one file to ship
 * and nothing for the user to unpack.
 *
 * The Python modules are the exception: MicroPython's import goes through
 * stat()/open() on real paths, so patchdb_extract_python() writes them to a
 * cache directory that the engine is then pointed at.
 *
 * Nothing here is thread-safe; drive it from the UI thread.
 */
#ifndef APOLLO_PATCHDB_H
#define APOLLO_PATCHDB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct patchdb patchdb_t;

typedef struct {
    const char *platform;   /* "PS3", "PS4", "PSV", "PSP", "PS2"  */
    const char *title_id;   /* "BLUS30490"                        */
    const char *name;       /* "3D Dot Game Heroes"               */
} patchdb_entry_t;

/*
 * Open the database. Pass NULL to search the usual places, in order:
 *
 *   $APOLLO_PATCHES_ZIP           explicit override (dev, CI)
 *   <exe dir>/apollo-patches.zip  alongside the binary
 *   <exe dir>/../Resources/...    inside a macOS .app bundle
 *   ./apollo-patches.zip          the working directory
 *
 * Returns NULL when no readable archive is found, or when one is found but is
 * not a usable database; patchdb_last_error() then says which.
 */
patchdb_t  *patchdb_open(const char *path);
void        patchdb_close(patchdb_t *db);

/* Where the open archive was found, for the UI to show. */
const char *patchdb_path(const patchdb_t *db);

/* Why the last patchdb_open() failed. Static string, never NULL. */
const char *patchdb_last_error(void);

int                    patchdb_count(const patchdb_t *db);
const patchdb_entry_t *patchdb_at(const patchdb_t *db, int index);

/*
 * The .savepatch bytes for one entry. On success *buf is a NUL-terminated
 * malloc'd buffer the caller owns (*len excludes the terminator), suitable for
 * handing straight to apctl_open_buffer(). Returns 1 on success, 0 on failure.
 */
int patchdb_read(const patchdb_t *db, int index, char **buf, size_t *len);

/*
 * Write the archive's python/ modules into <dir>/python, creating both
 * directories. Existing files are overwritten, so a newer bundle wins.
 * Returns the number of modules written, or -1 on failure.
 */
int patchdb_extract_python(const patchdb_t *db, const char *dir);

/*
 * Per-user cache directory for this application, with a trailing separator
 * (%LOCALAPPDATA%, ~/Library/Caches, or $XDG_CACHE_HOME). Returns a pointer to
 * a static buffer, or NULL if no home directory could be determined.
 *
 * The trailing separator matters: libapollo builds its Python import path as
 * APOLLO_HOST_DATA_PATH + "python".
 */
const char *patchdb_cache_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_PATCHDB_H */
