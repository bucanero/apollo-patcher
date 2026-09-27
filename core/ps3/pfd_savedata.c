/*
 * pfd_savedata - the PS3's own savedata encryption. See pfd_savedata.h.
 *
 * Derived from flatz's pfdtool, by way of bucanero/pfd_sfo_tools
 * (pfdtool/src/pfd.c) and bucanero/apollo-ps3 (source/pfd.c, pfd_util.c).
 * Those two are the same file: apollo-ps3's copy swaps polarSSL for mbedTLS
 * and drops every ES64() because the PS3 is big-endian and so is the format.
 * This one runs little-endian and links mbedTLS, so it takes the byte swaps
 * from the first and the crypto calls from the second.
 *
 * What is NOT taken from either is the shape. Upstream is built around a
 * directory path: pfd_init() takes one, every hash is computed by reading a
 * file off disk, and pfd_update() walks the whole folder. A browser tab has no
 * folder, and a patcher does not need one -- only the file it just changed
 * needs rehashing, and every other entry's hash is still correct. So the API
 * here is one file at a time, buffers only. The algorithms below are
 * upstream's and keep upstream's names so the two stay diffable.
 *
 * The console keys are flatz's, stored scrambled exactly as apollo-ps3 stores
 * them and unscrambled on first use.
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>        /* snprintf, for the user number's ASCII form */

#include <mbedtls/aes.h>
#include <mbedtls/md.h>

#include <apollo.h>        /* wildcard_match_icase() */
#include <dbglogger.h>

#include "pfd_savedata.h"
#include "sfo.h"          /* the PARAM.SFO container, shared with every console */

/* Same sink the rest of the engine logs through -- apollo_ctrl.c defines
 * dbglogger_log() and routes it to the front-end's log panel. */
#define LOG dbglogger_log

/* ------------------------------------------------------------------------ */
/* The on-disk layout, from upstream's pfd_internal.h.                       */
/*                                                                          */
/* Every scalar is a big-endian u64. Expressed as offsets rather than packed */
/* structs because each one is bounds-checked against a length that came     */
/* from somewhere else, and a struct pointer hides which byte is being read. */
/* ------------------------------------------------------------------------ */

#define PFD_MAGIC            0x50464442ull   /* "PFDB" */
#define PFD_VERSION_V3       3
#define PFD_VERSION_V4       4

#define PFD_HEADER_OFF       0                /* magic u64, version u64      */
#define PFD_HEADER_KEY_OFF   16               /* 16 bytes, the signature IV  */
#define PFD_SIGNATURE_OFF    32               /* 64 bytes, encrypted         */
#define PFD_SIGNATURE_SIZE   64
#define   PFD_SIG_BOTTOM     0                /*   ..offsets within it       */
#define   PFD_SIG_TOP        20
#define   PFD_SIG_HASH_KEY   40

#define PFD_HASH_TABLE_OFF   96               /* capacity, reserved, used    */
#define PFD_HASH_TABLE_HDR   24               /*   then capacity * u64       */
#define PFD_ENTRY_INDEX_SIZE 8

#define PFD_ENTRY_SIZE       272
#define   PFD_E_ADDITIONAL   0                /* u64, next in the bucket     */
#define   PFD_E_NAME         8                /* 65 bytes + 7 padding        */
#define   PFD_E_KEY          80                /* 64 bytes, encrypted        */
#define   PFD_E_HASHES       144               /* 4 * 20                     */
#define   PFD_E_SIZE         264               /* u64, logical file length   */
#define PFD_ENTRY_DATA_SIZE  192               /* KEY..end, what is hashed   */

#define PFD_HASH_SIZE        20
#define PFD_HASH_KEY_SIZE    20
#define PFD_PARAM_SFO_KEY_SIZE 20
#define PFD_KEY_SIZE         16
#define PFD_ENTRY_KEY_SIZE   64

/* The console writes a 32KB PARAM.PFD with capacity 57 and 114 reserved
 * entries. Nothing depends on those numbers -- they are read from the file --
 * but a ceiling keeps a hostile header from asking for gigabytes of table
 * before the bounds check gets a chance to fail it. */
#define PFD_MAX_CAPACITY     4096
#define PFD_MAX_RESERVED     8192

/* ------------------------------------------------------------------------ */
/* Console keys. Stored scrambled, as apollo-ps3 stores them.                */
/* ------------------------------------------------------------------------ */

static const uint8_t k_xor[8] = { 0xD4,0xD1,0x6B,0x0C,0x5D,0xB0,0x87,0x91 };

static uint8_t k_syscon_manager[16] = {
    0x00,0xC2,0xD3,0x9A,0x3E,0x51,0x79,0x0E,0xA1,0xC5,0x56,0x37,0xE9,0xE6,0xD5,0xE5 };
static uint8_t k_fallback_dhk[16] = {
    0x05,0x10,0x8A,0x07,0xC1,0xE4,0xF9,0xF9,0x4F,0x51,0x36,0xC1,0xCA,0xA0,0x49,0x1C };
static uint8_t k_authentication_id[8] = {
    0xC4,0xC1,0x6B,0x0C,0x5C,0xB0,0x87,0x92 };
static uint8_t k_keygen[20] = {
    0xBF,0xCB,0xA5,0xAE,0x1B,0x07,0xC2,0x6C,0x5B,0x42,0x1D,0x37,0xCF,0xB5,0x13,0x5C,
    0x87,0x99,0x50,0x8E };
static uint8_t k_savegame_param_sfo[20] = {
    0xD8,0xD9,0x6B,0x02,0x54,0xB5,0x83,0x95,0xD9,0xD0,0x64,0x0C,0x59,0xB6,0x85,0x93,
    0xDD,0xD7,0x66,0x0F };
static uint8_t k_trophy_param_sfo[20] = {
    0x89,0x8A,0x0F,0x75,0x4A,0xB2,0xC9,0x0A,0x6C,0x02,0x5B,0x44,0x36,0x29,0xE9,0xE8,
    0x89,0xAE,0x28,0x9E };
static uint8_t k_tropsys_dat[20] = {
    0x64,0x51,0xAF,0x03,0xAE,0xE8,0xE3,0xA7,0x5D,0xF9,0x7C,0x3A,0xFB,0x0F,0x92,0x18,
    0xF8,0x2F,0xCF,0x3A };
static uint8_t k_tropusr_dat[20] = {
    0x53,0xC0,0x84,0xF8,0x5B,0x21,0xB8,0x98,0xE3,0x20,0x7E,0xF6,0xEF,0x8D,0x66,0x38,
    0x5D,0xAB,0x13,0x96 };
static uint8_t k_troptrns_dat[20] = {
    0x45,0x3F,0xEA,0x59,0x07,0x7C,0x9B,0xDE,0x61,0x7B,0x8E,0x4A,0x71,0x4E,0x9B,0xF3,
    0x70,0x7E,0x5D,0xA9 };
static uint8_t k_tropconf_sfm[20] = {
    0x36,0x3C,0x58,0xCB,0x41,0xF4,0xC9,0x7A,0x15,0x33,0x56,0x6F,0x07,0x68,0x6F,0xBE,
    0x9A,0x1B,0x25,0x98 };

/*
 * Which console the save is FOR.
 *
 * Unset by default, and that is the safe default: PARAM.SFO's entry carries
 * three hashes keyed by the console it came off, and leaving them alone keeps
 * a patched save working on that console. Setting one is how a save is re-bound
 * to a different machine -- see apfd_set_console().
 */
static apfd_console_t g_console;
static int            g_console_set = 0;

static int keys_ready = 0;

static int all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

static void unscramble(uint8_t *key, int len)
{
    for (int i = 0; i < len; i++)
        key[i] ^= k_xor[i % 8];
}

static void setup_keys(void)
{
    if (keys_ready)
        return;

    unscramble(k_syscon_manager,    sizeof(k_syscon_manager));
    unscramble(k_fallback_dhk,      sizeof(k_fallback_dhk));
    unscramble(k_authentication_id, sizeof(k_authentication_id));
    unscramble(k_keygen,            sizeof(k_keygen));
    unscramble(k_savegame_param_sfo, sizeof(k_savegame_param_sfo));
    unscramble(k_trophy_param_sfo,  sizeof(k_trophy_param_sfo));
    unscramble(k_tropsys_dat,       sizeof(k_tropsys_dat));
    unscramble(k_tropusr_dat,       sizeof(k_tropusr_dat));
    unscramble(k_troptrns_dat,      sizeof(k_troptrns_dat));
    unscramble(k_tropconf_sfm,      sizeof(k_tropconf_sfm));

    keys_ready = 1;
}

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

void apfd_set_console(const apfd_console_t *console)
{
    setup_keys();

    if (!console || all_zero(console->console_id, APFD_CONSOLE_ID_LEN)) {
        memset(&g_console, 0, sizeof g_console);
        g_console_set = 0;
        LOG("[PFD] console binding cleared; PARAM.SFO keeps the hashes it has");
        return;
    }

    g_console = *console;
    g_console_set = 1;
    LOG("[PFD] console %02X%02X..%02X%02X, user %08u",
        g_console.console_id[0], g_console.console_id[1],
        g_console.console_id[14], g_console.console_id[15],
        (unsigned)g_console.user_id);
}

int apfd_get_console(apfd_console_t *out)
{
    if (out)
        *out = g_console;
    return g_console_set;
}

const char *apfd_strerror(int err)
{
    switch (err) {
    case APFD_OK:           return "OK";
    case APFD_ERR_PFD:      return "PARAM.PFD is missing, truncated or malformed";
    case APFD_ERR_VERSION:  return "PARAM.PFD is a version this does not implement";
    case APFD_ERR_NO_ENTRY: return "that file has no entry in PARAM.PFD, so it is not protected";
    case APFD_ERR_SIZE:     return "the save file is the wrong length for its PARAM.PFD entry";
    case APFD_ERR_NO_KEY:   return "no secure file ID for this save in the Apollo database";
    case APFD_ERR_PLAIN:    return "that entry is listed in PARAM.PFD but is not encrypted";
    case APFD_ERR_ARG:      return "missing argument";
    case APFD_ERR_MEM:      return "out of memory";
    case APFD_ERR_HASH:     return "PARAM.PFD's recorded hash does not match the file";
    default:                return "unknown error";
    }
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void wr64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

static void hmac_sha1(const uint8_t *key, size_t key_len,
                      const uint8_t *data, size_t data_len,
                      uint8_t out[PFD_HASH_SIZE])
{
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
                    key, key_len, data, data_len, out);
}

static size_t align_up(size_t n, size_t a)
{
    return (n + a - 1) & ~(a - 1);
}


/* ------------------------------------------------------------------------ */
/* The validated view                                                       */
/*                                                                          */
/* Everything below works through one of these. Building it is the only      */
/* place a PFD's own numbers are trusted, and it refuses any that do not fit */
/* the buffer, so the rest of the file can index without re-checking.        */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t       *data;        /* NULL for a read-only view                */
    const uint8_t *cdata;
    size_t         len;

    uint64_t version;
    uint64_t capacity, reserved, used;

    size_t ht_size;             /* header + capacity * 8                    */
    size_t et_off;              /* entry table                              */
    size_t est_off;             /* entry signature table                    */
    size_t est_size;            /* capacity * 20                            */

    uint8_t real_hash_key[PFD_HASH_KEY_SIZE];
} pfd_view;

/* The signature block, decrypted. Upstream keeps it decrypted in its working
 * copy for the whole session; here the caller's buffer always holds what is on
 * disk, so it is decrypted into a local and re-encrypted on the way out. */
static void signature_decrypt(const uint8_t *pfd, uint8_t out[PFD_SIGNATURE_SIZE])
{
    mbedtls_aes_context aes;
    uint8_t iv[PFD_KEY_SIZE];

    memcpy(iv, pfd + PFD_HEADER_KEY_OFF, PFD_KEY_SIZE);
    memcpy(out, pfd + PFD_SIGNATURE_OFF, PFD_SIGNATURE_SIZE);

    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, k_syscon_manager, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, PFD_SIGNATURE_SIZE, iv, out, out);
    mbedtls_aes_free(&aes);
}

static void signature_encrypt(uint8_t *pfd, const uint8_t sig[PFD_SIGNATURE_SIZE])
{
    mbedtls_aes_context aes;
    uint8_t iv[PFD_KEY_SIZE];

    memcpy(iv, pfd + PFD_HEADER_KEY_OFF, PFD_KEY_SIZE);
    memcpy(pfd + PFD_SIGNATURE_OFF, sig, PFD_SIGNATURE_SIZE);

    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, k_syscon_manager, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, PFD_SIGNATURE_SIZE, iv,
                          pfd + PFD_SIGNATURE_OFF, pfd + PFD_SIGNATURE_OFF);
    mbedtls_aes_free(&aes);
}

static int view_open(const uint8_t *pfd, size_t len, pfd_view *v)
{
    uint8_t sig[PFD_SIGNATURE_SIZE];
    size_t need;

    if (!pfd || !v)
        return APFD_ERR_ARG;

    setup_keys();
    memset(v, 0, sizeof(*v));

    if (len < PFD_HASH_TABLE_OFF + PFD_HASH_TABLE_HDR)
        return APFD_ERR_PFD;

    if (rd64(pfd + PFD_HEADER_OFF) != PFD_MAGIC)
        return APFD_ERR_PFD;

    v->version = rd64(pfd + PFD_HEADER_OFF + 8);
    if (v->version != PFD_VERSION_V3 && v->version != PFD_VERSION_V4)
        return APFD_ERR_VERSION;

    v->capacity = rd64(pfd + PFD_HASH_TABLE_OFF);
    v->reserved = rd64(pfd + PFD_HASH_TABLE_OFF + 8);
    v->used     = rd64(pfd + PFD_HASH_TABLE_OFF + 16);

    if (v->capacity == 0 || v->capacity > PFD_MAX_CAPACITY ||
        v->reserved > PFD_MAX_RESERVED || v->used > v->reserved)
        return APFD_ERR_PFD;

    v->ht_size  = PFD_HASH_TABLE_HDR + (size_t)v->capacity * PFD_ENTRY_INDEX_SIZE;
    v->et_off   = PFD_HASH_TABLE_OFF + v->ht_size;
    v->est_off  = v->et_off + (size_t)v->reserved * PFD_ENTRY_SIZE;
    v->est_size = (size_t)v->capacity * PFD_HASH_SIZE;

    /* One check covering every table, since each offset is built from the one
     * before it and the sizes above are bounded by the ceilings. */
    need = v->est_off + v->est_size;
    if (need > len)
        return APFD_ERR_PFD;

    signature_decrypt(pfd, sig);

    /* V4 runs the stored key through the keygen key; V3 uses it as it is.
     * Everything downstream keys off the result, and nothing else about the
     * two versions differs. */
    if (v->version == PFD_VERSION_V4)
        hmac_sha1(k_keygen, sizeof(k_keygen), sig + PFD_SIG_HASH_KEY,
                  PFD_HASH_KEY_SIZE, v->real_hash_key);
    else
        memcpy(v->real_hash_key, sig + PFD_SIG_HASH_KEY, PFD_HASH_KEY_SIZE);

    v->cdata = pfd;
    v->len   = len;
    return APFD_OK;
}

static int view_open_rw(uint8_t *pfd, size_t len, pfd_view *v)
{
    int rc = view_open(pfd, len, v);
    if (rc == APFD_OK)
        v->data = pfd;
    return rc;
}

/* The entry table is `reserved` entries long; `used` of them are live. */
static const uint8_t *entry_at(const pfd_view *v, uint64_t index)
{
    if (index >= v->reserved)
        return NULL;
    return v->cdata + v->et_off + (size_t)index * PFD_ENTRY_SIZE;
}

static uint8_t *entry_at_rw(const pfd_view *v, uint64_t index)
{
    if (!v->data || index >= v->reserved)
        return NULL;
    return v->data + v->et_off + (size_t)index * PFD_ENTRY_SIZE;
}

/* A name field is 65 bytes and need not be terminated, so read it as a bounded
 * region rather than a C string. */
static void entry_name_copy(const uint8_t *entry, char out[APFD_NAME_LEN])
{
    memcpy(out, entry + PFD_E_NAME, APFD_NAME_LEN - 1);
    out[APFD_NAME_LEN - 1] = '\0';
}

/* ------------------------------------------------------------------------ */
/* Entry lookup -- upstream's bucket hash and chain walk                     */
/* ------------------------------------------------------------------------ */

static uint64_t hash_table_index(const pfd_view *v, const char *file_name)
{
    uint64_t hash = 0;

    for (const char *p = file_name; *p; ++p)
        hash = (hash << 5) - hash + (uint8_t)*p;

    return hash % v->capacity;
}

static uint64_t bucket_head(const pfd_view *v, uint64_t bucket)
{
    return rd64(v->cdata + PFD_HASH_TABLE_OFF + PFD_HASH_TABLE_HDR +
                bucket * PFD_ENTRY_INDEX_SIZE);
}

/*
 * The index of `file_name`, or APFD_ERR_NO_ENTRY.
 *
 * Entries that hash to the same bucket are chained through additional_index,
 * and the chain ends at any index >= reserved. `reserved` doubles as the step
 * limit: a PFD whose chain loops would otherwise spin here forever, and one
 * can arrive from a browser tab.
 *
 * The bucket walk is upstream's. The linear pass after it is not, and is there
 * because the bucket hash is case-SENSITIVE while the name comparison is not:
 * "data.bin" hashes to a different bucket than "DATA.BIN" and upstream simply
 * fails to find it. The console never notices, since it asks with the name it
 * wrote. A user who renamed a file on the way off the console would, so the
 * fallback scans the live entries -- at most a couple of hundred comparisons,
 * and only on a miss.
 */
static int entry_find(const pfd_view *v, const char *file_name, uint64_t *out)
{
    uint64_t cur = bucket_head(v, hash_table_index(v, file_name));

    for (uint64_t steps = 0; cur < v->reserved && steps <= v->reserved; ++steps) {
        const uint8_t *entry = entry_at(v, cur);
        char name[APFD_NAME_LEN];

        if (!entry)
            break;

        entry_name_copy(entry, name);
        if (strncasecmp(name, file_name, APFD_NAME_LEN - 1) == 0) {
            if (out)
                *out = cur;
            return APFD_OK;
        }
        cur = rd64(entry + PFD_E_ADDITIONAL);
    }

    for (uint64_t i = 0; i < v->used; ++i) {
        char name[APFD_NAME_LEN];

        entry_name_copy(entry_at(v, i), name);
        if (strncasecmp(name, file_name, APFD_NAME_LEN - 1) == 0) {
            if (out)
                *out = i;
            return APFD_OK;
        }
    }

    return APFD_ERR_NO_ENTRY;
}

/* ------------------------------------------------------------------------ */
/* Hash keys                                                                */
/* ------------------------------------------------------------------------ */

/*
 * The four files the console keys from built-in secrets rather than from a
 * per-game secure file ID. PARAM.SFO is on the list because it is the one
 * entry every save has; the rest only appear in trophy folders.
 */
int apfd_entry_has_builtin_key(const char *name)
{
    static const char *builtin[] = {
        "PARAM.SFO", "TROPSYS.DAT", "TROPUSR.DAT", "TROPTRNS.DAT", "TROPCONF.SFM",
    };

    if (!name)
        return 0;

    for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++)
        if (strncasecmp(name, builtin[i], APFD_NAME_LEN - 1) == 0)
            return 1;

    return 0;
}

static int is_param_sfo(const char *name)
{
    return strncasecmp(name, "PARAM.SFO", APFD_NAME_LEN - 1) == 0;
}

/*
 * PARAM.SFO's hash at index `which`, which is where a save's console binding
 * lives. Every other entry has only index 0.
 *
 *   0  a built-in key, the same on every console
 *   1  the CONSOLE ID -- the IDPS of the machine the save belongs to
 *   2  the disc hash key, per game, with a built-in fallback
 *   3  the authentication ID, a constant for savedata
 *
 * A trophy folder keys 2 and 3 differently -- off the console ID and the user
 * number, spliced with constants the way a secure file ID is -- and has no
 * index 1 at all, which is why the console skips it there.
 *
 * Returns the key length, or a negative error. Lengths differ per index and
 * are NOT all 20, which is the whole reason this reports one.
 */
static int param_sfo_hash_key(int which, uint8_t key[PFD_HASH_KEY_SIZE],
                              int is_trophy)
{
    /* An all-zero disc hash key means the game named none, which is the usual
     * case: eight of games.conf's 1819 sections carry one. */
    const uint8_t *dhk = all_zero(g_console.disc_hash_key, APFD_DHK_LEN)
                       ? k_fallback_dhk : g_console.disc_hash_key;

    memset(key, 0, PFD_HASH_KEY_SIZE);

    if (!is_trophy) {
        switch (which) {
        case 0:
            memcpy(key, k_savegame_param_sfo, PFD_PARAM_SFO_KEY_SIZE);
            return PFD_PARAM_SFO_KEY_SIZE;
        case 1:
            memcpy(key, g_console.console_id, APFD_CONSOLE_ID_LEN);
            return APFD_CONSOLE_ID_LEN;
        case 2:
            memcpy(key, dhk, APFD_DHK_LEN);
            return APFD_DHK_LEN;
        case 3:
            memcpy(key, k_authentication_id, sizeof(k_authentication_id));
            return (int)sizeof(k_authentication_id);
        }
        return APFD_ERR_ARG;
    }

    switch (which) {
    case 0:
        memcpy(key, k_trophy_param_sfo, PFD_PARAM_SFO_KEY_SIZE);
        return PFD_PARAM_SFO_KEY_SIZE;
    case 2:
        for (int i = 0, j = 0; i < PFD_HASH_KEY_SIZE; ++i) {
            switch (i) {
            case 4:  key[i] = 11; break;
            case 8:  key[i] = 10; break;
            case 9:  key[i] = 14; break;
            case 10: key[i] = 15; break;
            default: key[i] = g_console.console_id[j++]; break;
            }
        }
        return PFD_HASH_KEY_SIZE;
    case 3: {
        /* The user number as its eight ASCII digits, which is the form the
         * console hashes -- "00000001", not the integer 1. Upstream reads
         * user_id[j] once before this switch overwrites it, with j running to
         * 17 over an eight-byte array; the read is dead and out of bounds, so
         * it is not reproduced. */
        char uid[9];
        snprintf(uid, sizeof uid, "%08u", (unsigned)g_console.user_id);
        for (int i = 0, j = 0; i < PFD_HASH_KEY_SIZE; ++i) {
            switch (i) {
            case 3: key[i] = 11; break;
            case 7: key[i] = 14; break;
            default: key[i] = (uint8_t)uid[j++ % 8]; break;
            }
        }
        return PFD_HASH_KEY_SIZE;
    }
    }
    return APFD_ERR_ARG;   /* index 1 does not exist for a trophy folder */
}

/*
 * The hash key for one entry: what its file hash is HMACed with, and -- taking
 * its first 16 bytes as an IV -- what unwraps its AES key.
 *
 * A secure file ID is 16 bytes and the key is 20, so four constants are spliced
 * in at fixed positions and the ID fills the rest. The values are flatz's; they
 * are not derived from anything.
 *
 * This is index 0 only. Indices 1 to 3 exist for PARAM.SFO alone and come from
 * param_sfo_hash_key() above.
 */
static int entry_hash_key(const char *name, const uint8_t *sfid,
                          uint8_t key[PFD_HASH_KEY_SIZE], int is_trophy)
{
    memset(key, 0, PFD_HASH_KEY_SIZE);

    if (is_param_sfo(name))
        memcpy(key, is_trophy ? k_trophy_param_sfo : k_savegame_param_sfo, PFD_HASH_KEY_SIZE);
    else if (strncasecmp(name, "TROPSYS.DAT", APFD_NAME_LEN - 1) == 0)
        memcpy(key, k_tropsys_dat, PFD_HASH_KEY_SIZE);
    else if (strncasecmp(name, "TROPUSR.DAT", APFD_NAME_LEN - 1) == 0)
        memcpy(key, k_tropusr_dat, PFD_HASH_KEY_SIZE);
    else if (strncasecmp(name, "TROPTRNS.DAT", APFD_NAME_LEN - 1) == 0)
        memcpy(key, k_troptrns_dat, PFD_HASH_KEY_SIZE);
    else if (strncasecmp(name, "TROPCONF.SFM", APFD_NAME_LEN - 1) == 0)
        memcpy(key, k_tropconf_sfm, PFD_HASH_KEY_SIZE);
    else if (sfid) {
        for (int i = 0, j = 0; i < PFD_HASH_KEY_SIZE; ++i) {
            switch (i) {
            case 1:  key[i] = 11; break;
            case 2:  key[i] = 15; break;
            case 5:  key[i] = 14; break;
            case 8:  key[i] = 10; break;
            default: key[i] = sfid[j++]; break;
            }
        }
    } else
        return APFD_ERR_NO_KEY;

    return APFD_OK;
}

/* The entry's AES key, unwrapped. Stored encrypted with the syscon manager key
 * under an IV that is the first 16 bytes of the hash key, so an entry is only
 * readable by something that already knows the file's secure ID. */
static void entry_key_unwrap(const uint8_t *entry,
                             const uint8_t hash_key[PFD_HASH_KEY_SIZE],
                             uint8_t out[PFD_ENTRY_KEY_SIZE])
{
    mbedtls_aes_context aes;
    uint8_t iv[PFD_KEY_SIZE];

    memcpy(iv, hash_key, PFD_KEY_SIZE);
    memcpy(out, entry + PFD_E_KEY, PFD_ENTRY_KEY_SIZE);

    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, k_syscon_manager, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, PFD_ENTRY_KEY_SIZE, iv, out, out);
    mbedtls_aes_free(&aes);
}

/* ------------------------------------------------------------------------ */
/* The file cipher                                                          */
/*                                                                          */
/* Per 16-byte block, with the block index as a big-endian counter:          */
/*                                                                          */
/*   encrypt:  c = AES-ENC(k, p XOR AES-ENC(k, i))                          */
/*   decrypt:  p = AES-DEC(k, c) XOR AES-ENC(k, i)                          */
/*                                                                          */
/* Exact inverses, so a decrypt/encrypt round trip is lossless on any buffer */
/* that is a whole number of blocks.                                        */
/* ------------------------------------------------------------------------ */

static void file_crypt(uint8_t *data, size_t aligned_len,
                       const uint8_t key[PFD_KEY_SIZE], int encrypting)
{
    mbedtls_aes_context counter_aes, data_aes;

    mbedtls_aes_init(&counter_aes);
    mbedtls_aes_init(&data_aes);
    mbedtls_aes_setkey_enc(&counter_aes, key, 128);
    if (encrypting)
        mbedtls_aes_setkey_enc(&data_aes, key, 128);
    else
        mbedtls_aes_setkey_dec(&data_aes, key, 128);

    for (size_t i = 0; i < aligned_len / PFD_KEY_SIZE; ++i) {
        uint8_t counter[PFD_KEY_SIZE];
        uint8_t *block = data + i * PFD_KEY_SIZE;

        memset(counter, 0, sizeof(counter));
        wr64(counter, (uint64_t)i);
        mbedtls_aes_crypt_ecb(&counter_aes, MBEDTLS_AES_ENCRYPT, counter, counter);

        if (encrypting) {
            for (int j = 0; j < PFD_KEY_SIZE; ++j)
                block[j] ^= counter[j];
            mbedtls_aes_crypt_ecb(&data_aes, MBEDTLS_AES_ENCRYPT, block, block);
        } else {
            mbedtls_aes_crypt_ecb(&data_aes, MBEDTLS_AES_DECRYPT, block, block);
            for (int j = 0; j < PFD_KEY_SIZE; ++j)
                block[j] ^= counter[j];
        }
    }

    mbedtls_aes_free(&counter_aes);
    mbedtls_aes_free(&data_aes);
}

/* ------------------------------------------------------------------------ */
/* Reading a PARAM.PFD                                                      */
/* ------------------------------------------------------------------------ */

int apfd_valid(const uint8_t *pfd, size_t len)
{
    pfd_view v;
    return view_open(pfd, len, &v);
}

int apfd_version(const uint8_t *pfd, size_t len)
{
    pfd_view v;
    int rc = view_open(pfd, len, &v);
    return (rc == APFD_OK) ? (int)v.version : rc;
}

int apfd_entry_count(const uint8_t *pfd, size_t len)
{
    pfd_view v;
    int rc = view_open(pfd, len, &v);
    return (rc == APFD_OK) ? (int)v.used : rc;
}

int apfd_entry_name(const uint8_t *pfd, size_t len, int index,
                    char *out, size_t out_len)
{
    pfd_view v;
    const uint8_t *entry;
    char name[APFD_NAME_LEN];
    int rc;

    if (!out || out_len == 0)
        return APFD_ERR_ARG;

    rc = view_open(pfd, len, &v);
    if (rc != APFD_OK)
        return rc;

    if (index < 0 || (uint64_t)index >= v.used)
        return APFD_ERR_NO_ENTRY;

    entry = entry_at(&v, (uint64_t)index);
    entry_name_copy(entry, name);

    if (strlen(name) >= out_len)
        return APFD_ERR_SIZE;
    strcpy(out, name);
    return APFD_OK;
}

long long apfd_entry_size(const uint8_t *pfd, size_t len, int index)
{
    pfd_view v;
    int rc = view_open(pfd, len, &v);

    if (rc != APFD_OK)
        return rc;
    if (index < 0 || (uint64_t)index >= v.used)
        return APFD_ERR_NO_ENTRY;

    return (long long)rd64(entry_at(&v, (uint64_t)index) + PFD_E_SIZE);
}

int apfd_find(const uint8_t *pfd, size_t len, const char *name)
{
    pfd_view v;
    uint64_t index;
    int rc;

    if (!name)
        return APFD_ERR_ARG;

    rc = view_open(pfd, len, &v);
    if (rc != APFD_OK)
        return rc;

    rc = entry_find(&v, name, &index);
    if (rc != APFD_OK)
        return rc;

    /* A chain can reach an entry past `used` in a PFD that was edited badly.
     * Such an entry is not live, so report it as absent rather than hand back
     * an index the enumeration will not produce. */
    if (index >= v.used)
        return APFD_ERR_NO_ENTRY;

    return (int)index;
}

int apfd_is_trophy(const uint8_t *pfd, size_t len)
{
    static const char *trophy[] = {
        "TROPSYS.DAT", "TROPUSR.DAT", "TROPTRNS.DAT", "TROPCONF.SFM",
    };
    pfd_view v;

    if (view_open(pfd, len, &v) != APFD_OK)
        return 0;

    for (size_t i = 0; i < sizeof(trophy) / sizeof(trophy[0]); i++) {
        uint64_t index;
        if (entry_find(&v, trophy[i], &index) == APFD_OK && index < v.used)
            return 1;
    }

    return 0;
}

long long apfd_decrypted_size(const uint8_t *pfd, size_t len, const char *name)
{
    pfd_view v;
    uint64_t index;
    int rc;

    if (!name)
        return APFD_ERR_ARG;

    rc = view_open(pfd, len, &v);
    if (rc != APFD_OK)
        return rc;

    rc = entry_find(&v, name, &index);
    if (rc != APFD_OK || index >= v.used)
        return APFD_ERR_NO_ENTRY;

    return (long long)rd64(entry_at(&v, index) + PFD_E_SIZE);
}

size_t apfd_encrypted_size(size_t plain_len)
{
    return align_up(plain_len, APFD_ALIGN);
}

/* ------------------------------------------------------------------------ */
/* Resigning                                                                */
/* ------------------------------------------------------------------------ */

/*
 * One entry's signature: an HMAC over its whole bucket chain, name and entry
 * data for each link. Every entry in a bucket therefore signs the same value,
 * and changing any one of them invalidates all of them -- which is why
 * resigning walks buckets rather than entries.
 */
static int calculate_entry_hash(const pfd_view *v, const char *file_name,
                                uint8_t hash[PFD_HASH_SIZE])
{
    mbedtls_md_context_t md;
    uint64_t cur = bucket_head(v, hash_table_index(v, file_name));
    uint64_t steps = 0;
    int rc = APFD_ERR_NO_ENTRY;

    if (cur >= v->reserved)
        return rc;

    mbedtls_md_init(&md);
    if (mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1) != 0) {
        mbedtls_md_free(&md);
        return APFD_ERR_MEM;
    }
    mbedtls_md_hmac_starts(&md, v->real_hash_key, PFD_HASH_KEY_SIZE);

    while (cur < v->reserved && steps++ <= v->reserved) {
        const uint8_t *entry = entry_at(v, cur);
        if (!entry)
            break;
        mbedtls_md_hmac_update(&md, entry + PFD_E_NAME, APFD_NAME_LEN);
        mbedtls_md_hmac_update(&md, entry + PFD_E_KEY, PFD_ENTRY_DATA_SIZE);
        cur = rd64(entry + PFD_E_ADDITIONAL);
        rc = APFD_OK;
    }

    mbedtls_md_hmac_finish(&md, hash);
    mbedtls_md_free(&md);
    return rc;
}

/*
 * Rebuild the entry signature table and the two hashes over it, exactly as
 * pfd_update() does once it has finished with the file hashes.
 *
 * Buckets nothing hashes into carry the HMAC of the empty message, so the
 * table is fully determined and a reader cannot tell used slots from padding.
 */
int apfd_resign(uint8_t *pfd, size_t pfd_len)
{
    pfd_view v;
    uint8_t sig[PFD_SIGNATURE_SIZE];
    uint8_t hash[PFD_HASH_SIZE];
    int rc;

    rc = view_open_rw(pfd, pfd_len, &v);
    if (rc != APFD_OK)
        return rc;

    hmac_sha1(v.real_hash_key, PFD_HASH_KEY_SIZE, NULL, 0, hash);
    for (uint64_t i = 0; i < v.capacity; ++i)
        if (bucket_head(&v, i) >= v.capacity)
            memcpy(pfd + v.est_off + i * PFD_HASH_SIZE, hash, PFD_HASH_SIZE);

    for (uint64_t i = 0; i < v.used; ++i) {
        char name[APFD_NAME_LEN];

        entry_name_copy(entry_at(&v, i), name);
        if (calculate_entry_hash(&v, name, hash) != APFD_OK)
            return APFD_ERR_PFD;

        memcpy(pfd + v.est_off + hash_table_index(&v, name) * PFD_HASH_SIZE,
               hash, PFD_HASH_SIZE);
    }

    signature_decrypt(pfd, sig);
    hmac_sha1(v.real_hash_key, PFD_HASH_KEY_SIZE,
              pfd + v.est_off, v.est_size, sig + PFD_SIG_BOTTOM);
    hmac_sha1(v.real_hash_key, PFD_HASH_KEY_SIZE,
              pfd + PFD_HASH_TABLE_OFF, v.ht_size, sig + PFD_SIG_TOP);
    signature_encrypt(pfd, sig);

    return APFD_OK;
}

/* ------------------------------------------------------------------------ */
/* Decrypt / encrypt                                                        */
/* ------------------------------------------------------------------------ */

int apfd_decrypt(const uint8_t *pfd, size_t pfd_len, const char *name,
                 const uint8_t *in, size_t in_len,
                 const uint8_t sfid[APFD_SFID_LEN],
                 uint8_t *out, size_t out_cap, size_t *out_len)
{
    pfd_view v;
    uint64_t index;
    const uint8_t *entry;
    uint8_t hash_key[PFD_HASH_KEY_SIZE], entry_key[PFD_ENTRY_KEY_SIZE];
    uint8_t *work;
    size_t file_size, aligned;
    int rc;

    if (!name || !in || !out)
        return APFD_ERR_ARG;

    rc = view_open(pfd, pfd_len, &v);
    if (rc != APFD_OK)
        return rc;

    if (strncasecmp(name, "PARAM.SFO", APFD_NAME_LEN - 1) == 0)
        return APFD_ERR_PLAIN;

    rc = entry_find(&v, name, &index);
    if (rc != APFD_OK || index >= v.used)
        return APFD_ERR_NO_ENTRY;

    entry     = entry_at(&v, index);
    file_size = (size_t)rd64(entry + PFD_E_SIZE);
    aligned   = align_up(file_size, APFD_ALIGN);

    if (out_cap < file_size)
        return APFD_ERR_SIZE;

    rc = entry_hash_key(name, sfid, hash_key, apfd_is_trophy(pfd, pfd_len));
    if (rc != APFD_OK)
        return rc;

    /* The file has to carry every block the entry claims. A short one is not
     * salvageable: the cipher works on whole blocks, so missing bytes in the
     * last one corrupt the plaintext bytes BEFORE them too, and those are
     * inside the file. Better to say so than to hand back a quiet 12 bytes of
     * garbage at the end. */
    if (in_len < aligned) {
        LOG("[PFD] %s is %zu bytes; PARAM.PFD says %zu (%zu on disk)",
            name, in_len, file_size, aligned);
        return APFD_ERR_SIZE;
    }

    work = malloc(aligned ? aligned : APFD_ALIGN);
    if (!work)
        return APFD_ERR_MEM;
    memcpy(work, in, aligned);

    entry_key_unwrap(entry, hash_key, entry_key);
    file_crypt(work, aligned, entry_key, 0);

    memcpy(out, work, file_size);
    if (out_len)
        *out_len = file_size;

    LOG("[PFD] decrypted %s (%zu bytes)", name, file_size);

    free(work);
    return APFD_OK;
}

int apfd_encrypt(uint8_t *pfd, size_t pfd_len, const char *name,
                 const uint8_t *in, size_t in_len,
                 const uint8_t sfid[APFD_SFID_LEN],
                 uint8_t *out, size_t out_cap, size_t *out_len)
{
    pfd_view v;
    uint64_t index;
    uint8_t *entry;
    uint8_t hash_key[PFD_HASH_KEY_SIZE], entry_key[PFD_ENTRY_KEY_SIZE];
    size_t aligned;
    int rc;

    if (!name || !in || !out)
        return APFD_ERR_ARG;

    rc = view_open_rw(pfd, pfd_len, &v);
    if (rc != APFD_OK)
        return rc;

    if (strncasecmp(name, "PARAM.SFO", APFD_NAME_LEN - 1) == 0)
        return APFD_ERR_PLAIN;

    rc = entry_find(&v, name, &index);
    if (rc != APFD_OK || index >= v.used)
        return APFD_ERR_NO_ENTRY;

    aligned = align_up(in_len, APFD_ALIGN);
    if (out_cap < aligned)
        return APFD_ERR_SIZE;

    rc = entry_hash_key(name, sfid, hash_key, apfd_is_trophy(pfd, pfd_len));
    if (rc != APFD_OK)
        return rc;

    entry = entry_at_rw(&v, index);
    entry_key_unwrap(entry, hash_key, entry_key);

    memset(out, 0, aligned);
    memcpy(out, in, in_len);
    file_crypt(out, aligned, entry_key, 1);

    if (out_len)
        *out_len = aligned;

    /* The entry records the LOGICAL size and hashes the ALIGNED bytes: the
     * console reads the file whole and only the game knows where it ends. */
    wr64(entry + PFD_E_SIZE, (uint64_t)in_len);
    hmac_sha1(hash_key, PFD_HASH_KEY_SIZE, out, aligned, entry + PFD_E_HASHES);

    LOG("[PFD] encrypted %s (%zu bytes, %zu on disk)", name, in_len, aligned);

    return apfd_resign(pfd, pfd_len);
}

int apfd_update_file(uint8_t *pfd, size_t pfd_len, const char *name,
                     const uint8_t *plain, size_t plain_len,
                     const uint8_t sfid[APFD_SFID_LEN])
{
    pfd_view v;
    uint64_t index;
    uint8_t *entry;
    uint8_t hash_key[PFD_HASH_KEY_SIZE];
    int rc;

    if (!name || (!plain && plain_len))
        return APFD_ERR_ARG;

    rc = view_open_rw(pfd, pfd_len, &v);
    if (rc != APFD_OK)
        return rc;

    rc = entry_find(&v, name, &index);
    if (rc != APFD_OK || index >= v.used)
        return APFD_ERR_NO_ENTRY;

    rc = entry_hash_key(name, sfid, hash_key, apfd_is_trophy(pfd, pfd_len));
    if (rc != APFD_OK)
        return rc;

    entry = entry_at_rw(&v, index);
    wr64(entry + PFD_E_SIZE, (uint64_t)plain_len);
    hmac_sha1(hash_key, PFD_HASH_KEY_SIZE, plain, plain_len, entry + PFD_E_HASHES);

    /*
     * PARAM.SFO's other three hashes, which is where a save's console binding
     * lives. Written only when a console has been named, because the default
     * is to LEAVE THEM ALONE: they already describe the machine the save came
     * off, and rewriting them with a guess would unbind a working save.
     *
     * A trophy folder has no index 1, which is why a negative length is a skip
     * rather than a failure.
     */
    if (g_console_set && is_param_sfo(name)) {
        int trophy = apfd_is_trophy(pfd, pfd_len);

        for (int which = 1; which <= 3; which++) {
            int key_len = param_sfo_hash_key(which, hash_key, trophy);

            if (key_len < 0)
                continue;
            hmac_sha1(hash_key, (size_t)key_len, plain, plain_len,
                      entry + PFD_E_HASHES + which * PFD_HASH_SIZE);
        }
        LOG("[PFD] re-bound PARAM.SFO to the console in Settings");
    }

    LOG("[PFD] recorded %s (%zu bytes)", name, plain_len);

    return apfd_resign(pfd, pfd_len);
}

/* ---- the account a save is signed to ------------------------------------ */

/*
 * The account ID lives twice in a savedata PARAM.SFO: in the ACCOUNT_ID field
 * and again inside the binary PARAMS blob. This is where it sits in the blob.
 *
 *   0x00  12  unknown
 *   0x0C   4  unknown
 *   0x10   4  unknown
 *   0x14   4  unknown
 *   0x18   4  user id
 *   0x1C  16  PSID
 *   0x2C   4  user id, again
 *   0x30  16  ACCOUNT ID, as ASCII hex
 *
 * Taken from apollo-ps3's sfo_param_params_t and checked against 172 real
 * saves, where the two copies agreed every time.
 */
#define SFO_PARAMS_ACCOUNT_OFF 0x30

static int is_hex16(const char *s)
{
    int i;

    if (!s)
        return 0;
    for (i = 0; i < APFD_ACCT_ID_LEN; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return 0;
    }
    return s[APFD_ACCT_ID_LEN] == '\0';
}

int apfd_sfo_set_account_id(uint8_t *sfo, size_t sfo_len, const char *account)
{
    size_t   off;
    uint32_t used;
    int      written = 0;

    if (!sfo || !is_hex16(account))
        return APFD_ERR_ARG;

    /* The field. Written only at its declared length: the value is a fixed
     * 16 characters and a shorter one would be a different account. */
    if (asfo_find(sfo, sfo_len, "ACCOUNT_ID", &off, &used, NULL, NULL) == ASFO_OK
        && used >= APFD_ACCT_ID_LEN) {
        memcpy(sfo + off, account, APFD_ACCT_ID_LEN);
        written++;
    }

    /* ...and the copy inside PARAMS. */
    if (asfo_find(sfo, sfo_len, "PARAMS", &off, &used, NULL, NULL) == ASFO_OK
        && used >= SFO_PARAMS_ACCOUNT_OFF + APFD_ACCT_ID_LEN) {
        memcpy(sfo + off + SFO_PARAMS_ACCOUNT_OFF, account, APFD_ACCT_ID_LEN);
        written++;
    }

    if (!written)
        return APFD_ERR_NO_ENTRY;

    dbglogger_log("PARAM.SFO: account ID set to %s (%d field%s)",
                  account, written, written == 1 ? "" : "s");
    return APFD_OK;
}

int apfd_sfo_account_id(const uint8_t *sfo, size_t sfo_len, char *out, size_t out_cap)
{
    size_t   off;
    uint32_t used;

    if (!out || out_cap < APFD_ACCT_ID_LEN + 1)
        return APFD_ERR_SIZE;
    out[0] = '\0';
    if (!sfo)
        return APFD_ERR_ARG;

    if (asfo_find(sfo, sfo_len, "ACCOUNT_ID", &off, &used, NULL, NULL) != ASFO_OK
        || used < APFD_ACCT_ID_LEN) {
        /* No field of its own; the blob still has one. */
        if (asfo_find(sfo, sfo_len, "PARAMS", &off, &used, NULL, NULL) != ASFO_OK
            || used < SFO_PARAMS_ACCOUNT_OFF + APFD_ACCT_ID_LEN)
            return APFD_ERR_NO_ENTRY;
        off += SFO_PARAMS_ACCOUNT_OFF;
    }

    memcpy(out, sfo + off, APFD_ACCT_ID_LEN);
    out[APFD_ACCT_ID_LEN] = '\0';
    return APFD_OK;
}

int apfd_verify_file(const uint8_t *pfd, size_t pfd_len, const char *name,
                     const uint8_t *disk, size_t disk_len,
                     const uint8_t sfid[APFD_SFID_LEN])
{
    pfd_view v;
    uint64_t index;
    const uint8_t *entry;
    uint8_t hash_key[PFD_HASH_KEY_SIZE], hash[PFD_HASH_SIZE];
    int rc;

    if (!name || (!disk && disk_len))
        return APFD_ERR_ARG;

    rc = view_open(pfd, pfd_len, &v);
    if (rc != APFD_OK)
        return rc;

    rc = entry_find(&v, name, &index);
    if (rc != APFD_OK || index >= v.used)
        return APFD_ERR_NO_ENTRY;

    rc = entry_hash_key(name, sfid, hash_key, apfd_is_trophy(pfd, pfd_len));
    if (rc != APFD_OK)
        return rc;

    entry = entry_at(&v, index);
    hmac_sha1(hash_key, PFD_HASH_KEY_SIZE, disk, disk_len, hash);

    return memcmp(hash, entry + PFD_E_HASHES, PFD_HASH_SIZE) == 0
         ? APFD_OK : APFD_ERR_HASH;
}

/* ------------------------------------------------------------------------ */
/* The secure file ID database (apollo-patches PS3/games.conf)               */
/* ------------------------------------------------------------------------ */

int apfd_sfid_from_hex(const char *hex, uint8_t sfid[APFD_SFID_LEN])
{
    if (!hex || !sfid)
        return APFD_ERR_ARG;

    for (int i = 0; i < APFD_SFID_LEN; i++) {
        int hi = 0, lo = 0;

        for (int half = 0; half < 2; half++) {
            char c = hex[i * 2 + half];
            int  n;

            if      (c >= '0' && c <= '9') n = c - '0';
            else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
            else return APFD_ERR_ARG;

            if (half == 0) hi = n; else lo = n;
        }
        sfid[i] = (uint8_t)((hi << 4) | lo);
    }

    return hex[APFD_SFID_LEN * 2] == '\0' ? APFD_OK : APFD_ERR_ARG;
}

/* One line of the file, trimmed of the CR that the file's CRLF endings leave
 * and of surrounding blanks. Advances *pos past the newline. */
static size_t conf_line(const char *text, size_t len, size_t *pos,
                        const char **start)
{
    size_t begin = *pos, end;

    for (end = begin; end < len && text[end] != '\n'; end++)
        ;
    *pos = (end < len) ? end + 1 : len;

    while (end > begin && (text[end - 1] == '\r' || text[end - 1] == ' ' ||
                           text[end - 1] == '\t'))
        end--;
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
        begin++;

    *start = text + begin;
    return end - begin;
}

/* Does `id` (an id from a section header) prefix `directory`, ignoring case? */
static int id_prefixes(const char *id, size_t id_len, const char *directory)
{
    size_t i;

    for (i = 0; i < id_len; i++) {
        if (!directory[i])
            return 0;
        if (tolower((unsigned char)id[i]) != tolower((unsigned char)directory[i]))
            return 0;
    }
    return 1;
}

/*
 * How well a section header matches a save directory: the length of its
 * longest id that prefixes `directory`, or 0 for no match.
 *
 * Empty ids are skipped. Six sections have one, left by a stray separator, and
 * an empty prefix matches every directory in the file.
 */
static size_t section_score(const char *ids, size_t ids_len, const char *directory,
                            const char **best, size_t *best_len)
{
    size_t score = 0, i = 0;

    while (i < ids_len) {
        size_t start = i;

        while (i < ids_len && ids[i] != '/')
            i++;

        size_t id_len = i - start;
        while (id_len && (ids[start + id_len - 1] == ' ' || ids[start + id_len - 1] == '\t'))
            id_len--;

        if (id_len > score && id_prefixes(ids + start, id_len, directory)) {
            score = id_len;
            if (best)     *best     = ids + start;
            if (best_len) *best_len = id_len;
        }

        i++;  /* past the '/' */
    }

    return score;
}

/*
 * The body of the section that best matches `directory`.
 *
 * Returns its start offset and, through `id_out`, the id that matched, or
 * APFD_ERR_NO_KEY when nothing does. Shared by the two lookups below, which
 * differ only in what they then read out of that body.
 */
static int conf_find_section(const char *text, size_t len, const char *directory,
                             size_t *body_out, char *id_out, size_t id_cap)
{
    size_t pos = 0, best_score = 0, best_body = 0, best_id_len = 0;
    const char *best_id = NULL;
    const char *line;
    size_t line_len;

    while (pos < len) {
        const char *id = NULL;
        size_t id_len = 0, score;

        line_len = conf_line(text, len, &pos, &line);
        if (line_len < 2 || line[0] != '[' || line[line_len - 1] != ']')
            continue;

        score = section_score(line + 1, line_len - 2, directory, &id, &id_len);
        if (score > best_score) {
            best_score  = score;
            best_id     = id;
            best_id_len = id_len;
            best_body   = pos;   /* conf_line() left us past the header */
        }
    }

    if (!best_score)
        return APFD_ERR_NO_KEY;

    if (id_out && id_cap) {
        size_t n = best_id_len < id_cap - 1 ? best_id_len : id_cap - 1;
        memcpy(id_out, best_id, n);
        id_out[n] = '\0';
    }
    *body_out = best_body;
    return APFD_OK;
}

/*
 * The disc hash key a section names, which keys PARAM.SFO's hash 2.
 *
 * Almost every section leaves it commented out -- eight of 1819 carry one --
 * and a game with none uses a built-in fallback, so APFD_ERR_NO_KEY here is
 * the ordinary answer and not a problem.
 */
int apfd_dhk_from_conf(const char *text, size_t len, const char *directory,
                       uint8_t dhk[APFD_DHK_LEN])
{
    static const char PREFIX[] = "disc_hash_key=";
    size_t pos = 0;
    const char *line;
    size_t line_len;
    int rc;

    if (!text || !directory || !dhk)
        return APFD_ERR_ARG;

    rc = conf_find_section(text, len, directory, &pos, NULL, 0);
    if (rc != APFD_OK)
        return rc;

    while (pos < len) {
        char value[APFD_DHK_LEN * 2 + 1];

        line_len = conf_line(text, len, &pos, &line);
        if (line_len && line[0] == '[')
            break;
        /* A leading ';' comments the line out, which is how most sections
         * carry the key's NAME without a value. */
        if (line_len != sizeof(PREFIX) - 1 + sizeof(value) - 1 ||
            strncmp(line, PREFIX, sizeof(PREFIX) - 1) != 0)
            continue;

        memcpy(value, line + sizeof(PREFIX) - 1, sizeof(value) - 1);
        value[sizeof(value) - 1] = '\0';
        if (apfd_sfid_from_hex(value, dhk) == APFD_OK)
            return APFD_OK;
    }

    return APFD_ERR_NO_KEY;
}

int apfd_sfid_from_conf(const char *text, size_t len,
                        const char *directory, const char *file_name,
                        uint8_t sfid[APFD_SFID_LEN], char *id_out, size_t id_cap)
{
    static const char PREFIX[] = "secure_file_id:";
    size_t pos = 0;
    const char *line;
    size_t line_len;
    int rc;

    if (!text || !directory || !file_name || !sfid)
        return APFD_ERR_ARG;

    rc = conf_find_section(text, len, directory, &pos, id_out, id_cap);
    if (rc != APFD_OK)
        return rc;

    /* The first pattern in that section's body matching the file name. File
     * order is the tie-break, so stop at the first hit. The body runs to the
     * next header. */
    while (pos < len) {
        char pattern[APFD_NAME_LEN];
        char value[APFD_SFID_LEN * 2 + 1];
        const char *eq;
        size_t pattern_len, value_len;

        line_len = conf_line(text, len, &pos, &line);

        if (line_len && line[0] == '[')
            break;
        if (line_len <= sizeof(PREFIX) - 1 ||
            strncmp(line, PREFIX, sizeof(PREFIX) - 1) != 0)
            continue;

        line     += sizeof(PREFIX) - 1;
        line_len -= sizeof(PREFIX) - 1;

        eq = memchr(line, '=', line_len);
        if (!eq)
            continue;

        pattern_len = (size_t)(eq - line);
        value_len   = line_len - pattern_len - 1;

        if (pattern_len == 0 || pattern_len >= sizeof(pattern))
            continue;
        memcpy(pattern, line, pattern_len);
        pattern[pattern_len] = '\0';

        if (!wildcard_match_icase(file_name, pattern))
            continue;

        /* The value is a slice of the caller's buffer, not a string, so copy
         * it out before the parser goes looking for a terminator. */
        if (value_len != sizeof(value) - 1) {
            LOG("[PFD] games.conf: '%s' key is %zu digits, expected %zu",
                pattern, value_len, sizeof(value) - 1);
            continue;
        }
        memcpy(value, eq + 1, value_len);
        value[value_len] = '\0';

        if (apfd_sfid_from_hex(value, sfid) != APFD_OK) {
            LOG("[PFD] games.conf: '%s' key is not hex", pattern);
            continue;
        }

        return APFD_OK;
    }

    return APFD_ERR_NO_KEY;
}
