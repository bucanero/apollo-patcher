/*
 * patchdb - read-only access to the bundled patch database. See patchdb.h.
 *
 * The zip reading here is deliberately minimal: we produce the archive
 * ourselves (tools/make-bundle.py), so only stored and deflated entries need
 * handling, and zip64 cannot occur at ~2.8MB. It is still defensive about
 * offsets and sizes — a truncated download should report an error, not crash.
 *
 * The whole archive is slurped into memory. At 2.8MB that is cheaper than
 * keeping a file handle and seeking, and it makes bounds checking a single
 * comparison against one buffer.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define PATH_SEP '\\'
#define MKDIR(p) _mkdir(p)
#else
#include <sys/types.h>
#include <unistd.h>
#define PATH_SEP '/'
#define MKDIR(p) mkdir((p), 0755)
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "patchdb.h"

/* ---------------------------------------------------------------------------
 * zip structures we care about
 * ------------------------------------------------------------------------- */

#define SIG_EOCD    0x06054b50u
#define SIG_CDIR    0x02014b50u
#define SIG_LOCAL   0x04034b50u

#define METHOD_STORED   0
#define METHOD_DEFLATE  8

/*
 * Path budget. PDB_DIR_MAX is what we accept for a *directory*; every buffer a
 * directory is joined into is declared larger, so the compiler can see that no
 * snprintf can truncate. A path longer than this simply reads as "no bundle
 * there", which is the same outcome as not finding one.
 */
#define PDB_DIR_MAX   768
#define PDB_PATH_MAX  1024

typedef struct {
    const char *name;       /* into the archive buffer, NOT NUL-terminated */
    size_t      name_len;
    unsigned    method;
    size_t      comp_size;
    size_t      uncomp_size;
    size_t      local_offset;
} zip_entry_t;

struct patchdb {
    unsigned char *raw;         /* the whole archive            */
    size_t         raw_len;

    zip_entry_t   *entries;     /* central directory, name-sorted */
    int            entry_count;

    char          *index_text;  /* index.tsv, chopped into lines  */
    patchdb_entry_t *list;      /* browsable rows                 */
    int           *list_zip;    /* row -> entries[] index         */
    int            list_count;

    char           path[PDB_PATH_MAX];
};

static const char *g_error = "";

const char *patchdb_last_error(void) { return g_error; }

/* ---------------------------------------------------------------------------
 * little-endian readers, bounds-checked against the archive
 * ------------------------------------------------------------------------- */

static int u16_at(const unsigned char *p, size_t len, size_t off, unsigned *out)
{
    if (off + 2 > len) return 0;
    *out = (unsigned)p[off] | ((unsigned)p[off + 1] << 8);
    return 1;
}

static int u32_at(const unsigned char *p, size_t len, size_t off, unsigned long *out)
{
    if (off + 4 > len) return 0;
    *out = (unsigned long)p[off] | ((unsigned long)p[off + 1] << 8) |
           ((unsigned long)p[off + 2] << 16) | ((unsigned long)p[off + 3] << 24);
    return 1;
}

/* ---------------------------------------------------------------------------
 * central directory
 * ------------------------------------------------------------------------- */

static int entry_cmp(const void *a, const void *b)
{
    const zip_entry_t *x = a, *y = b;
    size_t n = x->name_len < y->name_len ? x->name_len : y->name_len;
    int c = memcmp(x->name, y->name, n);
    if (c) return c;
    return x->name_len < y->name_len ? -1 : (x->name_len > y->name_len ? 1 : 0);
}

static const zip_entry_t *find_entry(const patchdb_t *db, const char *name)
{
    zip_entry_t key = { name, strlen(name), 0, 0, 0, 0 };
    return bsearch(&key, db->entries, (size_t)db->entry_count,
                   sizeof(*db->entries), entry_cmp);
}

/* Locate the end-of-central-directory record, scanning back from the tail. */
static int find_eocd(const unsigned char *p, size_t len, size_t *eocd)
{
    if (len < 22) return 0;

    /* 22 bytes fixed + up to 64KB of archive comment. */
    size_t max_back = len < 22 + 0xFFFF ? len : 22 + 0xFFFF;
    for (size_t back = 22; back <= max_back; back++) {
        size_t off = len - back;
        unsigned long sig;
        if (u32_at(p, len, off, &sig) && sig == SIG_EOCD) {
            *eocd = off;
            return 1;
        }
    }
    return 0;
}

static int read_central_directory(patchdb_t *db)
{
    size_t eocd;
    if (!find_eocd(db->raw, db->raw_len, &eocd)) {
        g_error = "not a zip archive (no end-of-central-directory record)";
        return 0;
    }

    unsigned count;
    unsigned long cd_size, cd_off;
    if (!u16_at(db->raw, db->raw_len, eocd + 10, &count) ||
        !u32_at(db->raw, db->raw_len, eocd + 12, &cd_size) ||
        !u32_at(db->raw, db->raw_len, eocd + 16, &cd_off)) {
        g_error = "truncated end-of-central-directory record";
        return 0;
    }
    if (cd_off > db->raw_len || cd_off + cd_size > db->raw_len) {
        g_error = "central directory lies outside the archive (truncated file?)";
        return 0;
    }
    if (count == 0) {
        g_error = "archive is empty";
        return 0;
    }

    db->entries = calloc(count, sizeof(*db->entries));
    if (!db->entries) { g_error = "out of memory"; return 0; }

    size_t off = cd_off;
    for (unsigned i = 0; i < count; i++) {
        unsigned long sig, comp, uncomp, local;
        unsigned method, name_len, extra_len, comment_len;

        if (!u32_at(db->raw, db->raw_len, off, &sig) || sig != SIG_CDIR) {
            g_error = "malformed central directory";
            return 0;
        }
        if (!u16_at(db->raw, db->raw_len, off + 10, &method) ||
            !u32_at(db->raw, db->raw_len, off + 20, &comp) ||
            !u32_at(db->raw, db->raw_len, off + 24, &uncomp) ||
            !u16_at(db->raw, db->raw_len, off + 28, &name_len) ||
            !u16_at(db->raw, db->raw_len, off + 30, &extra_len) ||
            !u16_at(db->raw, db->raw_len, off + 32, &comment_len) ||
            !u32_at(db->raw, db->raw_len, off + 42, &local)) {
            g_error = "truncated central directory entry";
            return 0;
        }
        if (off + 46 + name_len > db->raw_len || local >= db->raw_len) {
            g_error = "central directory entry points outside the archive";
            return 0;
        }

        zip_entry_t *e = &db->entries[db->entry_count++];
        e->name         = (const char *)db->raw + off + 46;
        e->name_len     = name_len;
        e->method       = method;
        e->comp_size    = (size_t)comp;
        e->uncomp_size  = (size_t)uncomp;
        e->local_offset = (size_t)local;

        off += 46 + name_len + extra_len + comment_len;
    }

    qsort(db->entries, (size_t)db->entry_count, sizeof(*db->entries), entry_cmp);
    return 1;
}

/* ---------------------------------------------------------------------------
 * reading one entry
 * ------------------------------------------------------------------------- */

/* Inflate (or copy) an entry into a fresh NUL-terminated buffer. */
static int read_zip_entry(const patchdb_t *db, const zip_entry_t *e,
                          char **out, size_t *out_len)
{
    unsigned long sig;
    unsigned name_len, extra_len;

    if (!u32_at(db->raw, db->raw_len, e->local_offset, &sig) || sig != SIG_LOCAL ||
        !u16_at(db->raw, db->raw_len, e->local_offset + 26, &name_len) ||
        !u16_at(db->raw, db->raw_len, e->local_offset + 28, &extra_len))
        return 0;

    size_t data = e->local_offset + 30 + name_len + extra_len;
    if (data + e->comp_size > db->raw_len)
        return 0;

    char *buf = malloc(e->uncomp_size + 1);
    if (!buf) return 0;

    if (e->method == METHOD_STORED) {
        if (e->comp_size != e->uncomp_size) { free(buf); return 0; }
        memcpy(buf, db->raw + data, e->uncomp_size);
    } else if (e->method == METHOD_DEFLATE) {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        /* Negative window bits: raw deflate, no zlib header — what zip uses. */
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) { free(buf); return 0; }

        zs.next_in   = (Bytef *)(db->raw + data);
        zs.avail_in  = (uInt)e->comp_size;
        zs.next_out  = (Bytef *)buf;
        zs.avail_out = (uInt)e->uncomp_size;

        int rc = inflate(&zs, Z_FINISH);
        size_t got = zs.total_out;
        inflateEnd(&zs);

        if ((rc != Z_STREAM_END && rc != Z_OK) || got != e->uncomp_size) {
            free(buf);
            return 0;
        }
    } else {
        free(buf);     /* encrypted or an exotic method: not something we make */
        return 0;
    }

    buf[e->uncomp_size] = '\0';
    *out = buf;
    if (out_len) *out_len = e->uncomp_size;
    return 1;
}

/* ---------------------------------------------------------------------------
 * index.tsv -> browsable rows
 * ------------------------------------------------------------------------- */

static int parse_index(patchdb_t *db)
{
    const zip_entry_t *e = find_entry(db, "index.tsv");
    if (!e) {
        g_error = "archive has no index.tsv (built with an old make-bundle.sh?)";
        return 0;
    }
    if (!read_zip_entry(db, e, &db->index_text, NULL)) {
        g_error = "could not read index.tsv from the archive";
        return 0;
    }

    /* One row per line at most; the header line and any blanks go unused. */
    int lines = 1;
    for (const char *p = db->index_text; *p; p++)
        if (*p == '\n') lines++;

    db->list     = calloc((size_t)lines, sizeof(*db->list));
    db->list_zip = calloc((size_t)lines, sizeof(*db->list_zip));
    if (!db->list || !db->list_zip) { g_error = "out of memory"; return 0; }

    char *save = NULL;
    for (char *line = db->index_text; line && *line; line = save) {
        save = strchr(line, '\n');
        if (save) *save++ = '\0';

        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[n - 1] = '\0';
        if (!*line || *line == '#') continue;      /* header / blank */

        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        char *tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) continue;
        *tab2 = '\0';

        const char *platform = line;
        const char *title_id = tab1 + 1;
        const char *name     = tab2 + 1;
        if (!*platform || !*title_id) continue;

        /* Only list rows whose patch is actually in this archive, so a click
         * can never fail to find its file. */
        char want[512];
        snprintf(want, sizeof(want), "%s/%s.savepatch", platform, title_id);
        const zip_entry_t *patch = find_entry(db, want);
        if (!patch) continue;

        db->list[db->list_count].platform = platform;
        db->list[db->list_count].title_id = title_id;
        db->list[db->list_count].name     = *name ? name : title_id;
        db->list_zip[db->list_count] = (int)(patch - db->entries);
        db->list_count++;
    }

    if (db->list_count == 0) {
        g_error = "index.tsv listed no patches that are present in the archive";
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * locating the archive
 * ------------------------------------------------------------------------- */

static int exe_dir(char *out, size_t cap)
{
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) return 0;
#elif defined(__APPLE__)
    uint32_t n = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &n) != 0) return 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1) return 0;
    out[n] = '\0';
#endif

    char *slash = strrchr(out, PATH_SEP);
#if defined(_WIN32)
    char *alt = strrchr(out, '/');
    if (alt > slash) slash = alt;
#endif
    if (!slash) return 0;
    *slash = '\0';
    return 1;
}

static int readable(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

#define BUNDLE_NAME "apollo-patches.zip"

static int locate(char *out, size_t cap)
{
    const char *env = getenv("APOLLO_PATCHES_ZIP");
    if (env && *env) {
        snprintf(out, cap, "%s", env);
        if (readable(out)) return 1;
        g_error = "$APOLLO_PATCHES_ZIP is set but that file cannot be read";
        return 0;
    }

    char dir[PDB_DIR_MAX];
    if (exe_dir(dir, sizeof(dir))) {
        snprintf(out, cap, "%s%c%s", dir, PATH_SEP, BUNDLE_NAME);
        if (readable(out)) return 1;

        /* macOS .app: the binary sits in Contents/MacOS, resources next door. */
        snprintf(out, cap, "%s%c..%cResources%c%s", dir, PATH_SEP, PATH_SEP,
                 PATH_SEP, BUNDLE_NAME);
        if (readable(out)) return 1;
    }

    snprintf(out, cap, "%s", BUNDLE_NAME);
    if (readable(out)) return 1;

    g_error = "no " BUNDLE_NAME " found next to the application";
    return 0;
}

/* ---------------------------------------------------------------------------
 * public entry points
 * ------------------------------------------------------------------------- */

patchdb_t *patchdb_open(const char *path)
{
    g_error = "";

    patchdb_t *db = calloc(1, sizeof(*db));
    if (!db) { g_error = "out of memory"; return NULL; }

    if (path && *path) {
        snprintf(db->path, sizeof(db->path), "%s", path);
        if (!readable(db->path)) {
            g_error = "cannot read that file";
            patchdb_close(db);
            return NULL;
        }
    } else if (!locate(db->path, sizeof(db->path))) {
        patchdb_close(db);
        return NULL;
    }

    FILE *fp = fopen(db->path, "rb");
    if (!fp) { g_error = "cannot open the archive"; patchdb_close(db); return NULL; }

    if (fseek(fp, 0, SEEK_END) != 0) {
        g_error = "cannot size the archive";
        fclose(fp);
        patchdb_close(db);
        return NULL;
    }
    long size = ftell(fp);
    rewind(fp);
    if (size <= 0) {
        g_error = "archive is empty";
        fclose(fp);
        patchdb_close(db);
        return NULL;
    }

    db->raw_len = (size_t)size;
    db->raw = malloc(db->raw_len);
    if (!db->raw || fread(db->raw, 1, db->raw_len, fp) != db->raw_len) {
        g_error = "could not read the archive";
        fclose(fp);
        patchdb_close(db);
        return NULL;
    }
    fclose(fp);

    if (!read_central_directory(db) || !parse_index(db)) {
        patchdb_close(db);
        return NULL;
    }
    return db;
}

void patchdb_close(patchdb_t *db)
{
    if (!db) return;
    free(db->raw);
    free(db->entries);
    free(db->index_text);
    free(db->list);
    free(db->list_zip);
    free(db);
}

const char *patchdb_path(const patchdb_t *db) { return db ? db->path : ""; }
int patchdb_count(const patchdb_t *db) { return db ? db->list_count : 0; }

const patchdb_entry_t *patchdb_at(const patchdb_t *db, int index)
{
    if (!db || index < 0 || index >= db->list_count) return NULL;
    return &db->list[index];
}

int patchdb_read(const patchdb_t *db, int index, char **buf, size_t *len)
{
    if (!db || !buf || index < 0 || index >= db->list_count) return 0;
    return read_zip_entry(db, &db->entries[db->list_zip[index]], buf, len);
}

/*
 * mkdir -p. The cache directory is several levels deep and its parents are not
 * guaranteed to exist — a fresh Linux account has no ~/.cache at all, which is
 * how this first showed up: every module failed to open, and extraction
 * reported -1.
 *
 * Intermediate failures are ignored: they are usually EEXIST, and on Windows
 * the first component is a drive root that cannot be created. Only the final
 * component's result decides.
 */
static int make_dirs(const char *path)
{
    char tmp[PDB_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t n = strlen(tmp);
    if (n == 0) return 0;

    /* Without this the last component would be treated as a parent and never
     * created — patchdb_cache_dir() deliberately ends with a separator. */
    while (n > 1 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\'))
        tmp[--n] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char sep = *p;
        *p = '\0';
        MKDIR(tmp);
        *p = sep;
    }

    if (MKDIR(tmp) != 0 && errno != EEXIST)
        return 0;
    return 1;
}

int patchdb_extract_python(const patchdb_t *db, const char *dir)
{
    if (!db || !dir || !*dir) return -1;

    /* Trim any trailing separator so the join below does not double it. */
    char base[PDB_DIR_MAX];
    snprintf(base, sizeof(base), "%s", dir);
    size_t bn = strlen(base);
    while (bn > 1 && (base[bn - 1] == '/' || base[bn - 1] == '\\'))
        base[--bn] = '\0';

    char py_dir[sizeof(base) + 16];
    snprintf(py_dir, sizeof(py_dir), "%s%cpython", base, PATH_SEP);

    /* Creates the whole chain, including <cache>/python itself. */
    if (!make_dirs(py_dir)) {
        g_error = "could not create the cache directory for the Python modules";
        return -1;
    }

    int written = 0;
    for (int i = 0; i < db->entry_count; i++) {
        const zip_entry_t *e = &db->entries[i];

        /* python/<name>, no nested directories in the bundle. */
        if (e->name_len <= 7 || memcmp(e->name, "python/", 7) != 0) continue;
        size_t base_len = e->name_len - 7;
        if (base_len == 0 || base_len >= 256) continue;
        if (memchr(e->name + 7, '/', base_len)) continue;

        char base_name[256];
        memcpy(base_name, e->name + 7, base_len);
        base_name[base_len] = '\0';

        char *data = NULL;
        size_t data_len = 0;
        if (!read_zip_entry(db, e, &data, &data_len)) continue;

        char out_path[sizeof(py_dir) + sizeof(base_name) + 2];
        snprintf(out_path, sizeof(out_path), "%s%c%s", py_dir, PATH_SEP, base_name);

        FILE *fp = fopen(out_path, "wb");
        if (fp) {
            if (fwrite(data, 1, data_len, fp) == data_len) written++;
            fclose(fp);
        }
        free(data);
    }

    if (!written)
        g_error = "no Python modules could be written to the cache directory";
    return written ? written : -1;
}

const char *patchdb_cache_dir(void)
{
    static char dir[PDB_DIR_MAX];

#if defined(_WIN32)
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !*base) base = getenv("APPDATA");
    if (!base || !*base) return NULL;
    snprintf(dir, sizeof(dir), "%s\\apollo-patcher\\", base);
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    snprintf(dir, sizeof(dir), "%s/Library/Caches/apollo-patcher/", home);
#else
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        snprintf(dir, sizeof(dir), "%s/apollo-patcher/", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return NULL;
        snprintf(dir, sizeof(dir), "%s/.cache/apollo-patcher/", home);
    }
#endif
    return dir;
}
