// Apollo Save Patcher (desktop) — Dear ImGui front-end over apollo_ctrl.
//
// UI parity with the `patcher` CLI:
//   - open a .savepatch  -> shows game name + code list (groups, flags)
//   - check the codes to apply
//   - per-code option dropdowns (replaces the CLI's scanf prompt)
//   - pick a target data file
//   - Apply -> runs apollo_apply_code() per selection, log panel shows progress
//
// Rendering backend: GLFW + fixed-function OpenGL2 (needs only GL 1.1, portable
// across Win/Mac/Linux and GPU-less hosts).
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>   // _putenv_s (Windows software-GL selection)
#include <cstring>   // strlen/strstr over the engine's C strings
#include <fstream>
#include <mutex>
#include <atomic>        // the save scan runs off the UI thread
#include <thread>
#include <algorithm>     // sorting the save list
#include <unordered_map> // title ID -> patch, for the save browser
#include <filesystem>    // walking a folder of saves
#include <chrono>        // --scan waits for the walk to finish
#include <sys/stat.h>   // telling a save FOLDER from a save file (see open_path)
#ifdef _WIN32
#include <direct.h>     // _mkdir, for the settings directory
#endif

#include "imgui.h"
#include "imgui_internal.h"   // PushItemFlag + ImGuiItemFlags_MixedValue (tri-state)
#include "imgui_stdlib.h"      // InputTextMultiline over std::string (misc/cpp)
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"
#include "imgui_memory_editor.h"   // vendored from ocornut/imgui_club (MIT)
#include "font10x20.h"             // the console raster font, for the hex editor
#include <GLFW/glfw3.h>

// Renderer: Dear ImGui's fixed-function OpenGL2 backend on a legacy (non-core)
// context. It needs only OpenGL 1.1 — the lowest common denominator available
// on every platform: a real GPU's compatibility profile, macOS's 2.1 legacy
// context, Mesa on Linux, and the always-present Microsoft software GL on
// RDP / VMs / old Windows. This 2D tool has no use for modern GL, so one
// backend and one code path serve all platforms.
#include "portable-file-dialogs.h"   // header-only native dialogs (osascript/zenity/Win32)
#include "apollo_ctrl.h"   // manages its own C linkage (and apollo.h is C++-safe)
#include "patchdb.h"       // bundled apollo-patches.zip (browsable database)
#include "psp_savedata.h"  // the PSP's own savedata encryption, below any patch
#include "pfd_savedata.h"  // ...and the PS3's, which works the same way
#include "saveinfo.h"     // which console wrote a PARAM.SFO, and for which game
#include "sfo.h"         // ...and reading/assigning the PS4/Vita account in one
#include "png.h"          // ...and the ICON0.PNG beside it
#include "psvcard.h"     // ...and the .PSV holding a PS1 or PS2 save
#include "mcicon.h"      // ...whose icon has to be rendered, not decoded
#include "ps2icon.h"     // ...and, for a PS2 save, animated
#include "ps2render.h"
#include "kirk_engine.h"   // KIRK_HOST_FUSE_ID, the Fuse ID default
#ifdef __APPLE__
#include "macos_open_docs.h"   // files Finder asks the app to open
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>    // MessageBox for visible startup errors (no console with -mwindows)
#include <shellapi.h>   // ShellExecuteA for opening links; WIN32_LEAN_AND_MEAN excludes it
#endif

// zlib is linked for the engine's own use either way; both the window icon and
// the code-type icons are stored deflated and inflated at startup.
#include <zlib.h>

// Window icon (Windows/Linux only): macOS takes its icon from the bundle.
#ifndef __APPLE__
#include "icon_rgba_z.h"   // 256x256 RGBA, zlib-deflated (inflated at startup)
#endif

// The code-type icons, baked by tools/make-type-icons.py. Optional: without
// them the type column falls back to the coloured letters it always had, and
// the build does not care -- the same courtesy the patch bundle gets.
// The header is always generated when Python is available, and defines
// APOLLO_HAVE_TYPE_ICONS only when there was art to bake. Keyed that way round
// on purpose: a header that only sometimes exists is one this file records no
// dependency on, so adding art later rebuilt nothing and changed nothing.
#if defined(__has_include)
#  if __has_include("type_icons.h")
#    include "type_icons.h"
#  endif
#endif

static GLFWwindow* g_window = nullptr;   // for native dialog parenting
static float       g_ui_scale = 1.0f;    // HiDPI content scale (column widths)

// The 10x20 console raster font, for the places where a proportional font
// is the wrong tool: the hex editor and the hex ID fields. Built in
// load_mono_font(); null if that failed, and every use guards for it.
static ImFont*     g_mono = nullptr;

// Whether a run of text is something that font could draw: it carries
// printable ASCII and nothing else, so anything else has to stay on the main
// font or it would come out as '?'.
//
// Deliberately a property of the TEXT and not of the font, with no reference
// to g_mono -- a file named on the command line, dropped on the Dock or opened
// from Finder is loaded before the window and its fonts exist, and asking
// about the font here latched every such patch to the wrong answer for the
// rest of the run. The callers check g_mono where they push it.
//
// This is what decides the code viewers case by case rather than wholesale.
// Their hex columns drift in a proportional font exactly as the hex editor's
// did, but 246 of the 2,247 files in the patch database are not even valid
// UTF-8 and another 82 carry curly quotes, accented Latin, katakana or
// fullwidth forms -- so a blanket switch would trade a cosmetic problem for
// rows of '?'. Per body, the 96% that are plain ASCII line up and the rest
// stay readable.
//
// Tab, newline and carriage return pass: they are layout, not glyphs.
static bool is_plain_ascii(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t' || c == '\n' || c == '\r') continue;
        if (c < FONT10X20_FIRST || c > FONT10X20_LAST) return false;
    }
    return true;
}

namespace fs = std::filesystem;

//
// One save found on disk.
//
// Defined up here rather than with the browser that fills it because the
// patcher screen holds a COPY of the one it opened. It cannot hold an index
// into the browser's list: a rescan rebuilds and re-sorts that vector, and the
// screen would go on describing whatever save had landed at that position.
//
struct SaveEntry {
    std::string path;        // the save folder, no trailing separator
    std::string dir_name;    // its last component, which is what is on disk
    std::string name;        // what the console calls the game
    std::string detail;      // ...and the slot: "AUTOSAVE", "Slot 1"
    std::string title_id;
    const char* platform = "?";
    int         platform_id = ASAVE_UNKNOWN;   // ...and the same thing to switch on
    bool        encrypted = false;   // a console layer sits under the patch
    int         patch_index = -1;    // into the database, or -1 for no codes
    std::string patch_name;          // the database's name for the game

    std::vector<std::string> files;  // relative to path, metadata excluded
    int         suggest = -1;        // the one to open, or -1 for no data file
    bool        suggest_listed = false;  // ...and whether the console named it

    // A PS1 or PS2 save, which is not a folder at all: `path` is a .PSV file
    // and `files` are the names INSIDE it, which no filesystem call will find.
    // Anything that would otherwise stat one has to ask the container instead,
    // which is what `file_sizes` is for -- see entry_file_size().
    bool        container = false;
    std::vector<uint64_t> file_sizes;
    std::string icon;                // ICON0.PNG, if the save has one
    // The PSN account this save is signed to: PS3, PS4 and Vita, the three
    // that have the concept. 16 hex digits either way -- the same 64-bit
    // number, which a PS3 stores as ASCII and a PS4 or Vita as eight raw
    // bytes. Empty when the save does not say, which covers PSP (no account
    // in its PARAM.SFO), PS1 and PS2 (no such concept), and a PS4 or Vita
    // save whose ACCOUNT_ID is zero.
    //
    // Read during the scan because the PARAM.SFO is open anyway, and because
    // the question it answers -- is this one mine? -- is asked while looking
    // at the LIST.
    std::string account;
    std::string haystack;            // lowercased, for the filter box
};

// ---- shared UI state -------------------------------------------------------
struct AppState {
    apctl_session_t*   session = nullptr;
    std::string         patch_path;
    std::string         target_path;
    std::string         game_name;
    std::string         patch_raw;        // the .savepatch as text (CR stripped)
    bool                patch_raw_ascii = false;  // ...and whether g_mono can draw it
    std::string         patch_bytes;      // ...and verbatim, for saving it back
    bool                show_patch_raw = false;

    std::vector<unsigned char> hex_data;   // the target file, for the hex editor
    std::string         hex_path;          // which file hex_data came from
    bool                show_hex = false;
    bool                hex_dirty = false; // edits not yet written to disk
    bool                hex_fit   = false; // refit the window to the grid once
    std::vector<char>   selected;         // per-row checkbox
    std::vector<char>   viewer_open;      // per-row code window open flag
    // Editable copy of a code body, one per row. `loaded` keeps unsaved typing
    // alive across closing and reopening the window; only opening a row for
    // the first time (or saving/reverting) pulls the engine's text in.
    struct CodeEdit {
        std::string text;
        bool        loaded = false;
        bool        raise  = false;   // bring the window forward next frame
        // Whether the fixed-pitch font can draw this body. Cached rather than
        // asked every frame: a single code body runs to tens of KB.
        bool        ascii  = false;
        void set(const char* t) {
            text  = t ? t : "";
            ascii = is_plain_ascii(text.data(), text.size());
        }
        void rescan() { ascii = is_plain_ascii(text.data(), text.size()); }
    };
    std::vector<CodeEdit> viewer_buf;
    std::string         log;
    std::mutex          log_mtx;
    bool                backup = true;    // copy target -> target.bak before patching

    // What apctl_is_big_endian_for() said about the patch that is open, and
    // whether anything is open for it to have said it about. The value the
    // engine actually runs with is effective_big_endian(), which is this
    // unless Settings overrides it -- keeping the two apart is what lets the
    // patcher screen say "forced, and it disagrees with this patch".
    bool                be_detected = false;
    std::string         be_platform;      // "PS3", "PS4", ... "" for a loose file
    bool                scroll_log = false;
    // Closed, and it stays closed: the log is where the low-level detail
    // goes, and an action that worked has nothing to say that the section
    // above it does not already show. Only a FAILURE opens it, and only
    // alongside a message that says to look there.
    bool                show_log = false;
    // The result box. Raised by Apply and by the whole-save decrypt passes --
    // one modal, because they are the same thing to a reader: an action
    // finished, here is what happened.
    bool                open_apply_popup = false;
    std::string         apply_msg;
    std::string         apply_title = "Apply";   // the caption, not the identity

    // The database patch this target's own location names, or -1. Every title
    // ID in the database is exactly 9 characters and save folders are named
    // <TITLEID><suffix> ("ULUS10391", "ULJM05500DATA00", "UCUS98751_DATA01"),
    // so the folder answers "which game is this" without asking. See
    // detect_patch_for_target().
    int                 match_index = -1;
    std::string         match_label;      // "PSP/ULUS10391.savepatch"
    // A title ID the save browser read out of the save's own PARAM.SFO, for
    // the saves whose folder name does not carry one (every PS4 and Vita
    // save). Empty for a target that arrived any other way.
    std::string         title_hint;

    // The save the patcher screen is showing, copied when it was opened. Not
    // set when a target arrived by hand -- the command line, a drop, or the
    // advanced file picker -- which is exactly when there is no save to show.
    SaveEntry           save;
    bool                has_save = false;

    // ---- the PSP's own savedata encryption -------------------------------
    //
    // A PSP save is wrapped twice: the console encrypts it with a per-title
    // game key, and the game encrypts what is inside that. Every .savepatch
    // addresses the INNER layer, so a file copied straight off a Memory Stick
    // goes into the engine and comes back as noise that looks like output.
    //
    // The desktop app has something the web page does not — the folder. So
    // this is DETECTED rather than asked for: choose a target, and if a
    // PARAM.SFO sits beside it that lists the file, everything below fills
    // itself in, key included. See psp_detect().
    struct Psp {
        bool        found = false;      // a PARAM.SFO beside the target lists it
        bool        wrap  = true;       // unwrap before patching, re-wrap after
        std::string sfo_path;           // the PARAM.SFO that was found
        std::string directory;          // SAVEDATA_DIRECTORY, the key lookup
        std::string listed;             // the SAVEDATA_FILE_LIST entry to use
        int         mode = 0;           // SAVEDATA_PARAMS[0]
        bool        keyed = false;      // ...does it need a game key at all
        unsigned char key[APSP_KEY_LEN] = {0};
        bool        have_key = false;
        std::string key_note;           // where the key came from, for the UI
        char        key_hex[33] = "";   // the editable field

        void clear() { *this = Psp(); }
    };
    Psp psp;

    // ---- the PS3's own savedata encryption -------------------------------
    //
    // The same story one console up, detected the same way. The differences
    // that show up below:
    //
    //   PARAM.PFD, not PARAM.SFO   It lists the files that are protected, and
    //                              it is REWRITTEN when one is re-encrypted.
    //   the folder is the lookup   PS3/games.conf files its keys under SAVE
    //                              DIRECTORY names, and PARAM.PFD carries no
    //                              such string. The desktop app has the folder
    //                              on disk, so this is one place the web page
    //                              has to ask and this does not.
    struct Ps3 {
        bool        found = false;      // a PARAM.PFD beside the target lists it
        bool        wrap  = true;       // unwrap before patching, re-wrap after
        std::string pfd_path;           // the PARAM.PFD that was found
        std::string sfo_path;           // its PARAM.SFO, when one sits beside it
        std::string folder;             // the save directory, the key lookup
        std::string listed;             // the PARAM.PFD entry to use
        int         version = 0;        // 3 or 4
        bool        trophy = false;
        unsigned char sfid[APFD_SFID_LEN] = {0};
        bool        have_key = false;
        std::string key_note;           // where the key came from, for the UI
        char        key_hex[33] = "";   // the editable field
        bool        hash_ok = true;     // does PARAM.PFD still describe the file
        bool        hash_checked = false;
        char        account[APFD_ACCT_ID_LEN + 1] = "";  // what PARAM.SFO says now

        void clear() { *this = Ps3(); }
    };
    Ps3 ps3;

    // ---- the .PSV container, which is not encryption at all ---------------
    //
    // The same unwrap/patch/re-wrap shape as the two above, for a different
    // reason. A PS1 or PS2 save has no console cipher over it -- neither
    // console encrypted one -- but it also has no files on disk: it arrives
    // inside a signed container holding its whole memory-card directory.
    //
    // So the file a code addresses is extracted to a scratch copy, patched
    // there like any other save, and put back; the container is rebuilt around
    // it and re-signed. The signature is the part that must not be forgotten,
    // because a PS3 checks it on import and a save that fails looks corrupt
    // rather than unsigned.
    struct PsvCard {
        bool        found = false;      // the target came out of a container
        bool        wrap  = true;       // put it back afterwards
        std::string path;               // the .PSV itself
        std::string inner;              // the file inside it being patched
        int         index = -1;         // ...and which one, for a repeated name
        int         type  = 0;          // APSVC_TYPE_PS1 or _PS2
        int         sig   = 0;          // what its signature said when opened

        void clear() { *this = PsvCard(); }
    };
    PsvCard psvcard;

    void append_log(const char* line) {
        std::lock_guard<std::mutex> lk(log_mtx);
        log += line;
        log += '\n';
        scroll_log = true;
    }
    void close() {
        if (session) { apctl_close(session); session = nullptr; }
        selected.clear();
        viewer_open.clear();
        viewer_buf.clear();
        game_name.clear();
        patch_path.clear();
        patch_raw.clear();
        patch_raw_ascii = false;
        patch_bytes.clear();
        show_patch_raw = false;
        // The detection belongs to the patch that is going away. Left set, a
        // save opened afterwards with no patch of its own would report the
        // previous game's byte order as if it were its own.
        be_detected = false;
        be_platform.clear();
    }
};
static AppState g_app;

//
// Which screen the window is showing.
//
// Two, because everything after "which file, and which patch" is the same
// work: the code list, the option dropdowns, Apply, the log, the hex editor.
// Advanced is not a third screen, it is a DOOR into the second one -- the same
// screen with the file pickers shown instead of filled in from a save.
//
// A screen swap rather than a modal over the list, because an ImGui modal
// "blocks every interaction behind the window" (imgui.h), and the per-code
// editors and the hex editor are windows behind it. As a modal, the patcher
// would make its own hex editor unreachable.
//
enum Screen { SCREEN_SAVES = 0, SCREEN_PATCH = 1 };
static Screen g_screen   = SCREEN_SAVES;
static bool   g_advanced = false;   // ...show the file pickers on SCREEN_PATCH

// ---- bundled patch database ------------------------------------------------
// Read straight out of apollo-patches.zip shipped next to the app (or inside
// the .app bundle). Mirrors what the web front-end offers, minus the network.
struct PatchDb {
    patchdb_t*               db = nullptr;
    std::string              error;            // why it is unavailable, if so
    std::vector<std::string> platforms;        // "All", then those present
    std::vector<std::string> haystack;         // lowercased "name titleid"
    std::vector<int>         hits;             // indices into the database
    // Lowercased title ID -> the one patch that carries it. No title ID in the
    // database appears on two platforms, so one index per ID is exact rather
    // than a first-wins approximation. Read from the save-scanning thread,
    // which is why it is built once at start-up and never touched again.
    std::unordered_map<std::string, int> by_title_id;
    // titles.tsv out of the same zip: game names by title ID, for the saves
    // that name no game themselves. Held as one buffer and scanned, rather
    // than parsed into a map, because it is consulted only for those saves --
    // in practice the Vita ones -- and never in a loop that matters.
    std::string              titles;
    char                     search[128] = "";
    int                      platform = 0;     // index into platforms
    bool                     refilter = true;
    bool                     want_open = false;    // raise the modal next frame
};
static PatchDb g_db;

static void log_sink(void* ud, const char* line) {
    static_cast<AppState*>(ud)->append_log(line);
}

// ---- small helpers ---------------------------------------------------------
static const char* type_tag(int t) {
    switch (t) {
        case APOLLO_CODE_BSD:        return "BSD";
        case APOLLO_CODE_PYTHON:     return "PY";
        case APOLLO_CODE_SAVEWIZARD: return "SW";
        default:                     return "?";
    }
}
// Spelled out for the type selector in the code window; the table shows the
// short tag above instead.
static const char* type_name(int t) {
    switch (t) {
        case APOLLO_CODE_BSD:        return "BSD";
        case APOLLO_CODE_PYTHON:     return "Python";
        case APOLLO_CODE_SAVEWIZARD: return "Save Wizard";
        default:                     return "Unknown";
    }
}
static ImVec4 type_color(int t) {
    switch (t) {
        case APOLLO_CODE_BSD:        return ImVec4(0.45f, 0.80f, 0.55f, 1.0f); // green
        case APOLLO_CODE_PYTHON:     return ImVec4(0.95f, 0.80f, 0.35f, 1.0f); // yellow
        case APOLLO_CODE_SAVEWIZARD: return ImVec4(0.45f, 0.70f, 0.95f, 1.0f); // blue
        default:                     return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    }
}

//
// The three code-type icons as GL textures, or 0 each when they were not
// baked in. Uploaded once, after the GL context exists; never freed, because
// they live exactly as long as the window does.
//
// The three code-type icons as GL textures, with the slice of each texture
// the real art occupies. Uploaded once, after the GL context exists; never
// freed, because they live exactly as long as the window does.
struct TypeIcon {
    unsigned tex  = 0;
    float    aspect = 1.0f;   // art width / art height
    float    u1 = 1.0f, v1 = 1.0f;   // where the art ends inside the texture
};
static TypeIcon g_type_icon[3];   // [0] Save Wizard, [1] Python, [2] BSD

#ifdef APOLLO_HAVE_TYPE_ICONS
static TypeIcon upload_type_icon(const type_icon_t& ic) {
    TypeIcon out;
    std::vector<unsigned char> rgba(ic.rgba_bytes);
    uLongf got = ic.rgba_bytes;

    if (uncompress(rgba.data(), &got, ic.z, (uLong)ic.z_len) != Z_OK
        || got != ic.rgba_bytes)
        return out;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    // tex_w/tex_h are powers of two; the art sits in the top-left corner and
    // the UVs below cut the padding back off. OpenGL 1.1 takes nothing else.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ic.tex_w, ic.tex_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    out.tex    = tex;
    out.aspect = float(ic.art_w) / float(ic.art_h);
    out.u1     = float(ic.art_w) / float(ic.tex_w);
    out.v1     = float(ic.art_h) / float(ic.tex_h);
    return out;
}
#endif

static void load_type_icons() {
#ifdef APOLLO_HAVE_TYPE_ICONS
    for (int i = 0; i < 3 && i < (int)(sizeof TYPE_ICONS / sizeof TYPE_ICONS[0]); i++)
        g_type_icon[i] = upload_type_icon(TYPE_ICONS[i]);
#endif
}

static const TypeIcon* type_icon_for(int t) {
    int i;
    switch (t) {
        case APOLLO_CODE_SAVEWIZARD: i = 0; break;
        case APOLLO_CODE_PYTHON:     i = 1; break;
        case APOLLO_CODE_BSD:        i = 2; break;
        default:                     return nullptr;
    }
    return g_type_icon[i].tex ? &g_type_icon[i] : nullptr;
}


//
// The interpreter a code runs under, as a small picture rather than two
// letters.
//
// The art is three PNGs baked into the binary by tools/make-type-icons.py,
// the same way src/icon_rgba_z.h carries the app icon: decoded to RGBA and
// zlib-deflated at build time, inflated and uploaded once at startup. A font
// could not do this job -- the atlas is Noto Sans JP over a fixed set of
// ranges and has no pictographs, ImWchar is 16 bits here so an emoji
// codepoint cannot even be indexed, and colour emoji would want FreeType,
// which this build does not use.
//
// Without the generated header the build still works and the column falls
// back to the coloured letters it always had, the same courtesy the patch
// bundle gets. That is also what a screen with no textures left shows.
//
static void draw_type_icon(int t, int flags) {
    const float sz = ImGui::GetFontSize();

    // A code the engine marked EMPTY is a heading or a separator in the patch
    // rather than something that runs, so it has no interpreter to name. It
    // still carries a type -- APOLLO_CODE_SAVEWIZARD is 1, which is what an
    // unset one reads as -- so without this every one of them claimed to be a
    // Save Wizard code.
    if (flags & APOLLO_CODE_FLAG_EMPTY) {
        ImGui::Dummy(ImVec2(sz, sz));
        return;
    }

    if (const TypeIcon* ic = type_icon_for(t)) {
        // Square art draws at the line height. Anything wider is fitted
        // inside that square instead of widening the row: the column is a
        // fixed 48px and art is not allowed to set its size, so a wide mark
        // would otherwise be clipped with nothing saying why.
        float w = sz * ic->aspect, h = sz;
        if (ic->aspect > 1.0f) { w = sz; h = sz / ic->aspect; }
        ImGui::Image((ImTextureID)(intptr_t)ic->tex, ImVec2(w, h),
                     ImVec2(0.0f, 0.0f), ImVec2(ic->u1, ic->v1));
    } else {
        ImGui::TextColored(type_color(t), "%s", type_tag(t));
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", type_name(t));
}

// Returns true if every option group of a code has a selection (sel >= 0).
// The engine initialises sel to -1, so an untouched required option blocks apply.
// A detected PSP save with the wrap asked for but no game key yet is the one
// state Apply must refuse. The file on disk is still the console's ciphertext,
// and running codes over it writes damage that looks exactly like a result --
// the failure mode this whole layer exists to prevent. Untick the box (for a
// file that is already plaintext) or supply a key.
static bool psp_blocked() {
    return g_app.psp.found && g_app.psp.wrap && !g_app.psp.have_key;
}

// Same rule, same reason, for the PS3.
static bool ps3_blocked() {
    return g_app.ps3.found && g_app.ps3.wrap && !g_app.ps3.have_key;
}

// Either console's layer is in the way.
static bool native_blocked() { return psp_blocked() || ps3_blocked(); }

static bool code_options_ready(apctl_code_t* c) {
    for (int g = 0; g < apctl_opt_group_count(c); ++g)
        if (apctl_opt_get_selected(c, g) < 0) return false;
    return true;
}

// True if any checked code still has an unfilled option group.
static bool has_unfilled_selection() {
    if (!g_app.session) return false;
    for (int i = 0; i < apctl_code_count(g_app.session); ++i) {
        if (!g_app.selected[i]) continue;
        if (!code_options_ready(apctl_code_at(g_app.session, i))) return true;
    }
    return false;
}

static int count_selected() {
    int n = 0;
    for (char c : g_app.selected) n += c ? 1 : 0;
    return n;
}

static void select_all(bool on) {
    for (auto& c : g_app.selected) c = on ? 1 : 0;
}

// A group parent's children are the consecutive is_child codes following it.
// Fills [begin,end) with that range (empty if the code has no children).
static void group_children(int parent, int& begin, int& end) {
    int count = apctl_code_count(g_app.session);
    begin = parent + 1;
    end = begin;
    while (end < count && apctl_code_at(g_app.session, end)->is_child) ++end;
}

// Checking any code auto-checks every [R] required code in the patch. By design
// these are prerequisite steps (e.g. decrypt/re-encrypt) that must always run.
static void auto_enable_required() {
    if (!g_app.session) return;
    for (int i = 0; i < apctl_code_count(g_app.session); ++i)
        if (apctl_code_at(g_app.session, i)->flags & APOLLO_CODE_FLAG_REQUIRED)
            g_app.selected[i] = 1;
}

static bool backup_file(const std::string& src) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(src + ".bak", std::ios::binary);
    if (!out) return false;
    out << in.rdbuf();
    return true;
}

// Most patch files use CRLF. ImGui has no glyph for a carriage return, so it
// would draw a box per line in the raw viewer.
static std::string strip_cr(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in)
        if (c != '\r') out += c;
    return out;
}
// Defined below, next to the setting that can override it.
static bool effective_big_endian();


// Shared tail of both load paths (file and database): a session exists, so set
// up the per-row UI state around it.
static void adopt_session(apctl_session_t* session,
                          const std::string& label,
                          const char* display_name,
                          const char* platform /* nullptr for a loose file */) {
    g_app.session = session;
    g_app.patch_path = label;
    g_app.game_name = display_name && *display_name ? display_name
                                                    : apctl_game_name(session);
    const int n = apctl_code_count(session);
    g_app.selected.assign(n, 0);
    g_app.viewer_open.assign(n, 0);
    g_app.viewer_buf.assign(n, AppState::CodeEdit{});
    for (int i = 0; i < n; ++i)      // pre-check [DEFAULT:] codes
        g_app.selected[i] = apctl_code_at(session, i)->activated ? 1 : 0;

    char buf[512];
    snprintf(buf, sizeof buf, "Loaded %d codes from %s", n, label.c_str());
    g_app.append_log(buf);

    // PS3 save data is big-endian, and no patch in the database declares the
    // order per code (the engine's [BE:...] header exists but goes unused), so
    // work the mode out rather than leave the user to notice. The database's own
    // platform tag decides when there is one; a loose file falls back to its
    // title ID.
    //
    // Recorded, not applied: Settings can force a mode, and keeping the
    // detection separate is what lets the patcher screen say a forced one
    // disagrees with the patch in front of it.
    g_app.be_detected = apctl_is_big_endian_for(platform, label.c_str()) != 0;
    g_app.be_platform = platform ? platform : "";
    g_app.append_log(g_app.be_detected ? "PS3 title detected - big-endian save data"
                                       : "Non-PS3 title - little-endian save data");
    apctl_set_big_endian(effective_big_endian() ? 1 : 0);
}

// ---- about box -------------------------------------------------------------

// How large a PS2 save's 3D icon is rendered. Square, because the model is
// framed square, and 128 because that is what the pane shows it at -- there is
// no source image to be faithful to, so this is simply the size worth drawing.
#define PSV_ICON_PX   128
#define PSV_ICON_SS     4

#define APP_NAME      "Apollo Save Patcher"

// This app's own version, distinct from the engine's APOLLO_LIB_VERSION below
// it in the About box -- they move independently, and "which Apollo is this"
// has two honest answers.
//
// Normally handed in by the build from project(VERSION) in the root
// CMakeLists, which is the one place to bump it. The fallback is deliberately
// NOT the current number: a copy here would be a second source of truth, and
// the way it fails is silent -- someone bumps CMake, forgets this, and every
// hand-built binary from then on reports the PREVIOUS release while looking
// perfectly plausible. A placeholder cannot be mistaken for a release.
#ifndef APOLLO_PATCHER_VERSION
#define APOLLO_PATCHER_VERSION "0.0.0-dev"
#endif

#define URL_PATCHER   "https://github.com/bucanero/apollo-patcher"
#define URL_LIB       "https://github.com/bucanero/apollo-lib"
#define URL_PATCHES   "https://github.com/bucanero/apollo-patches"

// The user guide, rendered onto the project's own site by web/Makefile from
// docs/user-guide.md. The site rather than the repo's markdown because the
// page reads better and is the same document either way -- and because the one
// failure this menu item cannot help with is the app not starting at all, for
// which the guide's troubleshooting section is also linked from the release
// notes.
#define URL_GUIDE     "https://bucanero.github.io/apollo-patcher/guide.html"

// Hand a URL to the desktop. Every caller passes a compile-time constant from
// the list above, so there is nothing to quote-escape.
static void open_url(const char* url) {
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd =
#if defined(__APPLE__)
        "open '";
#else
        "xdg-open '";
#endif
    cmd += url;
    cmd += "' >/dev/null 2>&1 &";
    if (system(cmd.c_str()) != 0)
        g_app.append_log("[!] Could not open the browser - copy the link instead");
#endif
}

// A clickable link: ImGui has no hyperlink widget, so this is a text-coloured
// button plus a copy action, for when opening a browser is not possible (a
// bare Linux session with no xdg-open, say).
static void link_row(const char* label, const char* url) {
    ImGui::Bullet();
    ImGui::TextColored(ImVec4(0.45f, 0.65f, 1.00f, 1.0f), "%s", label);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", url);
    }
    if (ImGui::IsItemClicked()) open_url(url);
    ImGui::SameLine();
    ImGui::PushID(url);
    if (ImGui::SmallButton("copy")) ImGui::SetClipboardText(url);
    ImGui::PopID();
}

static bool g_want_about = false;

static void draw_about() {
    if (g_want_about) { ImGui::OpenPopup("About"); g_want_about = false; }

    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::Text(APP_NAME " %s", APOLLO_PATCHER_VERSION);
    ImGui::TextDisabled("Apollo engine %s", APOLLO_LIB_VERSION);
    ImGui::Spacing();

    ImGui::TextWrapped("Applies Apollo save patches - Save Wizard codes, BSD "
                       "scripts and Python scripts - to decrypted save data.");
    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Text("Copyright (C) 2020-2026 Damian Parrino (Bucanero)");
    ImGui::TextWrapped("Licensed under the GNU General Public License v3 or "
                       "later. This program comes with no warranty, to the "
                       "extent permitted by law.");
    ImGui::Spacing();

    ImGui::TextDisabled("Project");
    link_row("User guide", URL_GUIDE);
    link_row("apollo-patcher (this app)", URL_PATCHER);
    link_row("apollo-lib (the engine)", URL_LIB);
    link_row("apollo-patches (the patch database)", URL_PATCHES);
    ImGui::Spacing();

    ImGui::TextDisabled("Third-party components");
    ImGui::BulletText("Dear ImGui and GLFW - user interface");
    ImGui::BulletText("imgui_club memory editor - the hex view (MIT)");
    ImGui::BulletText("portable-file-dialogs - native file pickers (WTFPL)");
    ImGui::BulletText("mbedTLS and zlib - crypto and compression");
    ImGui::BulletText("MicroPython - runs the Python patch scripts");
    ImGui::BulletText("Noto Sans JP - the interface font (SIL OFL)");
    ImGui::BulletText("idispatch/raster-fonts - the 10x20 fixed-pitch font");
    ImGui::Spacing();

    if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Window titles get the file name; the full path goes in the body, where it
// can wrap and be read.
// A file size for a list: three significant figures and a unit, because the
// point of the column is telling the 4 MB save from the 3 KB index beside it,
// not counting bytes.
static std::string human_size(unsigned long long bytes) {
    static const char* const unit[] = { "B", "KB", "MB", "GB" };
    double v = double(bytes);
    int    u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }

    char buf[64];
    snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : (v < 10.0 ? "%.1f %s" : "%.0f %s"),
             v, unit[u]);
    return buf;
}

static const char* base_name(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

// The hex editor works on a copy of the target file held in memory, written
// back only when asked — so a mistyped byte costs nothing until you commit it.
static bool hex_load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { g_app.append_log("[!] Could not read the target file"); return false; }
    g_app.hex_data.assign(std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>());
    // A different file wants the window refitted around it; a reload of the
    // same one must not undo a size the user chose.
    g_app.hex_fit  = (g_app.hex_path != path);
    g_app.hex_path = path;
    g_app.hex_dirty = false;
    char buf[512];
    snprintf(buf, sizeof buf, "Loaded %zu bytes of %s for editing",
             g_app.hex_data.size(), path.c_str());
    g_app.append_log(buf);
    return true;
}

static bool hex_write_back() {
    if (g_app.hex_path.empty()) return false;

    // Same courtesy the patch path gives: keep a .bak before overwriting.
    if (g_app.backup && !backup_file(g_app.hex_path))
        g_app.append_log("[!] Backup failed - writing anyway");

    std::ofstream out(g_app.hex_path, std::ios::binary | std::ios::trunc);
    if (!out) { g_app.append_log("[!] Could not write the target file"); return false; }
    out.write(reinterpret_cast<const char*>(g_app.hex_data.data()),
              (std::streamsize)g_app.hex_data.size());
    if (!out) { g_app.append_log("[!] Write failed"); return false; }

    g_app.hex_dirty = false;
    char buf[512];
    snprintf(buf, sizeof buf, "Wrote %zu bytes to %s",
             g_app.hex_data.size(), g_app.hex_path.c_str());
    g_app.append_log(buf);
    return true;
}

// ---- the consoles' own savedata encryption ---------------------------------
//
// Everything here is buffer work; core/psp and core/ps3 do the crypto. What
// the desktop app adds over the web page is that it can look around: the save
// folder is right there, so the metadata file, the file list, the folder name
// and the key are all found rather than asked for.

static bool read_all(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

static bool write_all(const std::string& path, const unsigned char* data, size_t len) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data), (std::streamsize)len);
    return (bool)out;
}

// ---- settings --------------------------------------------------------------
//
// Which console a save is written FOR. Both values change what gets WRITTEN
// and neither affects reading one, which is why both are safe to leave empty:
// blank means "keep whatever the save already says", and that is what patching
// a save in place wants.
//
//   the PSP's Fuse ID    reaches the two PARAM.SFO hashes that savedata modes
//                        4 and 6 derive from the console's own fuse. Not
//                        enforced by the console, but console-locked games
//                        check it themselves (Gran Turismo does)
//   the PS3's console ID reaches PARAM.SFO's second hash inside PARAM.PFD,
//                        which is what binds a save to one machine
//
// Kept in the user's config directory rather than beside the app, because it
// describes their console and not this copy of the program. ImGui's own ini is
// deliberately off (see main), so this is the only thing written there.
/*
 * Byte order for save DATA, the CLI's -b/--big-endian flag.
 *
 * AUTO is the default and the only value that cannot be wrong: PS3 saves are
 * big-endian and everything else Apollo covers is not, and the patch database's
 * own platform tag says which a patch is (apctl_is_big_endian_for). The other
 * two are for somebody who knows better than the tag -- a loose patch file for
 * a console that is not in the database, say.
 *
 * Forcing one is remembered across runs, which is the point of it and also its
 * only hazard: a forced big-endian left set will byte-reverse every PS4 or Vita
 * save afterwards, and the result looks like a patched save rather than an
 * error. draw_byte_order() says so in amber whenever a forced mode disagrees
 * with the patch that is open.
 */
enum ByteOrder { BYTE_ORDER_AUTO = 0, BYTE_ORDER_BIG = 1, BYTE_ORDER_LITTLE = 2 };

struct Settings {
    char fuse_hex[17]    = "";     // 16 hex digits, or empty for the default
    char console_hex[33] = "";     // 32 hex digits, or empty for "leave alone"
    char account_hex[17] = "";     // 16 hex digits, the PSN account ID
    int  user_id         = 1;
    int  byte_order      = BYTE_ORDER_AUTO;
    // Where the saves are. Not a property of a console like the two above,
    // but the same kind of thing: something the person tells the app once and
    // should not have to tell it again. Set by choosing a folder in the save
    // browser, never typed.
    std::string saves_root;
    std::string status;            // what the last save or load had to say
};
/*
 * g_settings holds the FIELDS -- ImGui edits its buffers in place, so while
 * someone is halfway through typing a console ID, g_settings.console_hex is
 * that half. g_saved is what is on disk.
 *
 * They are separate because the byte order commits the moment it is picked
 * while the two hex fields commit on Save: without the split, changing the
 * order mid-word would write the half-typed ID to the file and push it to the
 * engine, silently clearing the console binding.
 */
static Settings g_settings;
static Settings g_saved;
static bool g_want_settings = false;

static std::string config_dir() {
#ifdef _WIN32
    const char* base = getenv("APPDATA");
    return base && *base ? std::string(base) + "\\apollo-patcher\\" : std::string();
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    return home && *home
        ? std::string(home) + "/Library/Application Support/apollo-patcher/"
        : std::string();
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/apollo-patcher/";
    const char* home = getenv("HOME");
    return home && *home ? std::string(home) + "/.config/apollo-patcher/" : std::string();
#endif
}

static std::string settings_path() {
    const std::string dir = config_dir();
    return dir.empty() ? std::string() : dir + "settings.txt";
}

/*
 * The byte order actually in effect: what the patch says, unless Settings
 * overrides it. With nothing loaded there is nothing to detect, so AUTO reads
 * as little-endian -- the host's own order, and what the engine defaults to.
 */
static bool effective_big_endian() {
    switch (g_settings.byte_order) {
        case BYTE_ORDER_BIG:    return true;
        case BYTE_ORDER_LITTLE: return false;
        default:                return g_app.be_detected;
    }
}

// Hand the values to the two engines. Called after a load and after every
// edit, so what is on screen is what will be written.
static void settings_apply() {
    apctl_set_big_endian(effective_big_endian() ? 1 : 0);

    apsp_set_fuse_id(KIRK_HOST_FUSE_ID);
    if (strlen(g_settings.fuse_hex) == 16) {
        unsigned long long v = strtoull(g_settings.fuse_hex, nullptr, 16);
        apsp_set_fuse_id((uint64_t)v);
    }

    apfd_console_t console;
    memset(&console, 0, sizeof console);
    if (strlen(g_settings.console_hex) == 32 &&
        apfd_sfid_from_hex(g_settings.console_hex, console.console_id) == APFD_OK) {
        console.user_id = (uint32_t)(g_settings.user_id > 0 ? g_settings.user_id : 1);
        apfd_set_console(&console);
    } else {
        apfd_set_console(nullptr);
    }
}

static void settings_load() {
    const std::string path = settings_path();
    std::ifstream in(path);
    if (!in) { settings_apply(); return; }

    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);

        if (key == "psp_fuse_id")    snprintf(g_settings.fuse_hex, sizeof g_settings.fuse_hex, "%s", val.c_str());
        else if (key == "ps3_console_id") snprintf(g_settings.console_hex, sizeof g_settings.console_hex, "%s", val.c_str());
        else if (key == "ps3_account_id") snprintf(g_settings.account_hex, sizeof g_settings.account_hex, "%s", val.c_str());
        else if (key == "ps3_user_id")    g_settings.user_id = atoi(val.c_str());
        else if (key == "saves_root")     g_settings.saves_root = val;
        else if (key == "byte_order")
            /* Named rather than numbered, so the file stays readable and an
             * unrecognised value falls back to the safe one. */
            g_settings.byte_order = val == "big"    ? BYTE_ORDER_BIG
                                  : val == "little" ? BYTE_ORDER_LITTLE
                                                    : BYTE_ORDER_AUTO;
    }
    settings_apply();
    g_saved = g_settings;
}

// Writes g_saved, never the fields: see the note there.
static bool settings_store() {
    const std::string dir = config_dir();
    if (dir.empty()) return false;

#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0777);
#endif

    std::ofstream out(settings_path(), std::ios::trunc);
    if (!out) return false;

    out << "# Apollo Save Patcher - which console saves are written FOR.\n"
        << "# Both are optional; blank keeps whatever a save already says.\n"
        << "psp_fuse_id=" << g_saved.fuse_hex << "\n"
        << "ps3_console_id=" << g_saved.console_hex << "\n"
        << "ps3_account_id=" << g_saved.account_hex << "\n"
        << "ps3_user_id=" << g_saved.user_id << "\n"
        << "saves_root=" << g_saved.saves_root << "\n"
        << "byte_order=" << (g_saved.byte_order == BYTE_ORDER_BIG    ? "big"
                           : g_saved.byte_order == BYTE_ORDER_LITTLE ? "little"
                                                                     : "auto") << "\n";
    return (bool)out;
}

static bool is_dir(const std::string& path) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static std::string dir_of(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// The game key, from the database in the bundle. Offline by design: the same
// file the web page fetches from the CDN travels in apollo-patches.zip, so the
// desktop app needs no network to open a save. The MATCHING is C shared with
// the page (apsp_key_from_db) — prefix against the save directory, longest
// entry wins, which matters because the database holds both NPJJ30022 and
// NPJJ30022GAME1 with different keys.
static bool psp_key_from_bundle(const std::string& directory,
                                unsigned char key[APSP_KEY_LEN], std::string& note) {
    char*  text = nullptr;
    size_t len  = 0;

    if (!g_db.db || !patchdb_read_file(g_db.db, "PSP/gamekeys.txt", &text, &len)) {
        note = "no key database in the bundle";
        return false;
    }

    char entry[64] = "";
    int rc = apsp_key_from_db(text, len, directory.c_str(), key, entry, sizeof entry);
    free(text);

    if (rc != APSP_OK) { note = apsp_strerror(rc); return false; }
    note = std::string("from the Apollo database (") + entry + ")";
    return true;
}

// Is the chosen target a PSP save the console encrypted? Answered by looking
// for a PARAM.SFO beside it that LISTS it — the file list is the authoritative
// answer to what is wrapped, and a save folder holds ICON0.PNG too.
static void psp_detect() {
    g_app.psp.clear();
    if (g_app.target_path.empty()) return;

    const std::string sfo_path = dir_of(g_app.target_path) + "PARAM.SFO";
    std::vector<unsigned char> sfo;
    if (!read_all(sfo_path, sfo) || apsp_sfo_valid(sfo.data(), sfo.size()) != APSP_OK)
        return;

    const char* target = base_name(g_app.target_path);
    const int n = apsp_sfo_file_count(sfo.data(), sfo.size());
    for (int i = 0; i < n; i++) {
        char name[APSP_NAME_LEN + 1];
        if (apsp_sfo_file_name(sfo.data(), sfo.size(), i, name, sizeof name) != APSP_OK)
            continue;
        if (strcmp(name, target) == 0) { g_app.psp.listed = name; break; }
    }
    // A file the SFO does not name is not wrapped; say nothing rather than
    // offering to decrypt something that was never encrypted.
    if (g_app.psp.listed.empty()) return;

    char dir[64] = "";
    apsp_sfo_directory(sfo.data(), sfo.size(), dir, sizeof dir);

    g_app.psp.found     = true;
    g_app.psp.sfo_path  = sfo_path;
    g_app.psp.directory = dir;
    g_app.psp.mode      = apsp_sfo_mode(sfo.data(), sfo.size());
    g_app.psp.keyed     = (g_app.psp.mode & APSP_MODE_KEYED) != 0;

    if (!g_app.psp.keyed) {
        g_app.psp.have_key = true;             // the null key IS the key
        g_app.psp.key_note = "not needed - this save is unkeyed";
    } else if (psp_key_from_bundle(g_app.psp.directory, g_app.psp.key,
                                   g_app.psp.key_note)) {
        g_app.psp.have_key = true;
        for (int i = 0; i < APSP_KEY_LEN; i++)
            snprintf(g_app.psp.key_hex + i * 2, 3, "%02X", g_app.psp.key[i]);
    }

    char msg[512];
    snprintf(msg, sizeof msg, "PSP save detected: %s in %s, mode 0x%02X, key %s",
             g_app.psp.listed.c_str(), g_app.psp.directory.c_str(),
             g_app.psp.mode & 0xFF, g_app.psp.key_note.c_str());
    g_app.append_log(msg);
}

// Take the console's layer off the target file, in place. Returns false and
// logs on failure; the caller must not carry on patching a file that is still
// encrypted.
//
// ---- the .PSV container ---------------------------------------------------
//
// Opening a PS1 or PS2 save means lifting one file out of the container it
// arrived in; saving means putting it back and re-signing. Everything between
// those two points is an ordinary file on disk, so the hex editor, the code
// viewers and the patch engine need to know nothing about any of this.
//

// Where an extracted file lives while it is being patched. Beside the Python
// modules the patch engine already unpacks, for the same reason: it is a
// scratch copy of something the app can always produce again, and it must not
// be written next to somebody's save.
static std::string psvcard_scratch_path(const std::string& inner) {
    const char* cache = patchdb_cache_dir();
    if (!cache) return std::string();

    std::error_code ec;
    const fs::path dir = fs::path(cache) / "psv";
    fs::create_directories(dir, ec);
    if (ec) return std::string();

    // The inner name is used as-is for recognisability -- it is what the code
    // viewer's title bar will show -- but a memory card can hold a name with a
    // separator in it, and that would write outside the scratch directory.
    std::string leaf = inner;
    for (char& c : leaf)
        if (c == '/' || c == '\\' || c == ':') c = '_';
    if (leaf.empty()) leaf = "savedata";

    return (dir / leaf).string();
}

// Lift the chosen file out and point the app at it. The container itself is
// left untouched until the save is written back.
//
// `st` is filled rather than g_app.psvcard being set directly, because the caller
// has to call adopt_target() afterwards and that CLEARS the container state --
// deliberately, so that choosing a loose file cannot leave the app pointed at
// the last container it opened and write the wrong thing back into it.
//
static bool psvcard_extract(const SaveEntry& save, int index, std::string& out_path,
                        AppState::PsvCard& st) {
    std::vector<unsigned char> buf;

    if (!read_all(save.path, buf)) {
        g_app.append_log("[!] PSV: could not read the container");
        return false;
    }

    apsvc_info_t info;
    int rc = apsvc_info(buf.data(), buf.size(), &info);
    if (rc != APSVC_OK) {
        g_app.append_log((std::string("[!] PSV: ") + apsvc_strerror(rc)).c_str());
        return false;
    }

    std::vector<apsvc_file_t> inner(size_t(info.file_count));
    if (info.file_count <= 0 ||
        apsvc_files(buf.data(), buf.size(), inner.data(), info.file_count, nullptr) != APSVC_OK) {
        g_app.append_log("[!] PSV: the container lists no files");
        return false;
    }
    if (index < 0 || index >= info.file_count) {
        g_app.append_log("[!] PSV: no such file in the container");
        return false;
    }

    const apsvc_file_t& f = inner[size_t(index)];
    const std::string scratch = psvcard_scratch_path(f.name);
    if (scratch.empty()) {
        g_app.append_log("[!] PSV: nowhere to extract to");
        return false;
    }
    if (!write_all(scratch, buf.data() + f.off, f.size)) {
        g_app.append_log("[!] PSV: could not write the extracted file");
        return false;
    }

    // Said out loud rather than silently repaired. Several tools wrote .PSV
    // files before the signature was understood and those saves are otherwise
    // perfectly good, so this is worth knowing and not worth refusing over --
    // and re-signing on the way out fixes it either way.
    const int sig = apsvc_verify(buf.data(), buf.size());
    if (sig == APSVC_SIG_BAD)
        g_app.append_log("[!] PSV: the container's signature does not match its "
                         "contents - it will be re-signed when saved");
    else if (sig == APSVC_SIG_UNSIGNED)
        g_app.append_log("PSV: the container is unsigned - it will be signed when saved");

    st.clear();
    st.found = true;
    st.path  = save.path;
    st.inner = f.name;
    st.index = index;
    st.type  = info.type;
    st.sig   = sig;

    char msg[512];
    snprintf(msg, sizeof msg, "PSV: extracted %s (%u bytes) from %s",
             f.name, f.size, info.dir_name);
    g_app.append_log(msg);

    out_path = scratch;
    return true;
}

// ...and put it back, rebuilding the container around it and signing the
// result. Written to a temporary file and moved into place, because a partly
// written container is not a save at all -- unlike a patched data file, which
// is at worst wrong.
static bool psvcard_wrap_target() {
    std::vector<unsigned char> container, patched;

    if (!read_all(g_app.psvcard.path, container) ||
        !read_all(g_app.target_path, patched)) {
        g_app.append_log("[!] PSV: could not read the container or the patched file");
        return false;
    }

    uint8_t* out = nullptr;
    size_t   out_len = 0;
    const int rc = apsvc_replace_at(container.data(), container.size(),
                                    g_app.psvcard.index,
                                    patched.data(), uint32_t(patched.size()),
                                    &out, &out_len);
    if (rc != APSVC_OK) {
        std::string why = apsvc_strerror(rc);
        if (rc == APSVC_ERR_SPACE)
            why += " - a PS1 save occupies whole memory-card blocks, so a code "
                   "that changes its length cannot be written back";
        g_app.append_log(("[!] PSV: " + why).c_str());
        return false;
    }

    const std::string tmp = g_app.psvcard.path + ".tmp";
    const bool wrote = write_all(tmp, out, out_len);
    const size_t wrote_len = out_len;
    apsvc_free(out);

    if (!wrote) {
        g_app.append_log("[!] PSV: could not write the rebuilt container");
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }

    std::error_code ec;
    fs::rename(tmp, g_app.psvcard.path, ec);
    if (ec) {
        // Across filesystems rename fails; copy and then drop the temporary.
        fs::copy_file(tmp, g_app.psvcard.path, fs::copy_options::overwrite_existing, ec);
        std::error_code rm;
        fs::remove(tmp, rm);
        if (ec) {
            g_app.append_log("[!] PSV: could not replace the container");
            return false;
        }
    }

    char msg[512];
    snprintf(msg, sizeof msg, "PSV: %s put back and the container re-signed "
                              "(%zu bytes)", g_app.psvcard.inner.c_str(), wrote_len);
    g_app.append_log(msg);
    return true;
}

static bool psp_unwrap_file(const std::string& path, const char* listed) {
    std::vector<unsigned char> sfo, enc;
    if (!read_all(g_app.psp.sfo_path, sfo) || !read_all(path, enc)) {
        g_app.append_log((std::string("[!] PSP: could not read ") + listed
                          + " or its PARAM.SFO").c_str());
        return false;
    }

    const size_t want = apsp_decrypted_size(enc.size());
    std::vector<unsigned char> out(want ? want : 1);
    size_t got = 0;
    int rc = apsp_decrypt(sfo.data(), sfo.size(), enc.data(), enc.size(),
                          g_app.psp.key, out.data(), out.size(), &got);
    if (rc != APSP_OK) {
        g_app.append_log((std::string("[!] PSP decrypt failed: ") + apsp_strerror(rc)).c_str());
        return false;
    }
    if (!write_all(path, out.data(), got)) {
        g_app.append_log((std::string("[!] PSP: could not write ") + listed).c_str());
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof msg, "PSP layer removed from %s: %zu -> %zu bytes",
             listed, enc.size(), got);
    g_app.append_log(msg);
    return true;
}

static bool psp_unwrap_target() {
    return psp_unwrap_file(g_app.target_path, g_app.psp.listed.c_str());
}

// ...and put it back, rewriting PARAM.SFO with it. Both files are written or
// neither is: a save whose PARAM.SFO does not match its data does not load, so
// a half-done wrap is worse than none.
static bool psp_wrap_file(const std::string& path, const char* listed) {
    std::vector<unsigned char> sfo, plain;
    if (!read_all(g_app.psp.sfo_path, sfo) || !read_all(path, plain)) {
        g_app.append_log((std::string("[!] PSP: could not read ") + listed
                          + " or its PARAM.SFO").c_str());
        return false;
    }

    std::vector<unsigned char> out(apsp_encrypted_size(plain.size()));
    size_t got = 0;
    int rc = apsp_encrypt(sfo.data(), sfo.size(), listed,
                          plain.data(), plain.size(), g_app.psp.key,
                          out.data(), out.size(), &got);
    if (rc != APSP_OK) {
        g_app.append_log((std::string("[!] PSP encrypt failed: ") + apsp_strerror(rc)).c_str());
        return false;
    }

    // PARAM.SFO first. If the data write then fails the save is inconsistent
    // either way, but this order leaves the SFO describing bytes that CAN be
    // produced again by re-running the wrap, rather than data nothing
    // describes.
    if (!write_all(g_app.psp.sfo_path, sfo.data(), sfo.size())) {
        g_app.append_log("[!] PSP: could not write PARAM.SFO");
        return false;
    }
    if (!write_all(path, out.data(), got)) {
        g_app.append_log((std::string("[!] PSP: could not write ") + listed).c_str());
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof msg,
             "PSP layer restored on %s: %zu -> %zu bytes, PARAM.SFO rewritten",
             listed, plain.size(), got);
    g_app.append_log(msg);
    return true;
}

static bool psp_wrap_target() {
    return psp_wrap_file(g_app.target_path, g_app.psp.listed.c_str());
}

//
// One action, one answer.
//
// The whole-save passes below build their own counts; this is the short form
// for the single-file buttons beside them. Without it those buttons succeed in
// silence, which reads as nothing having happened -- and on an action that
// rewrites a save in place, "did that work?" is not a question to leave open.
//
// Failure also raises the log, because the box can only say that something
// went wrong; the log says what.
//
static void report_action(const char *title, bool ok, const char *done) {
    g_app.apply_msg   = ok ? std::string(done)
                           : std::string(title) + " did not work.\n"
                             "Check the log for details.";
    g_app.apply_title = title;
    g_app.open_apply_popup = true;
    if (!ok) g_app.show_log = true;
}

//
// Every file the console wrapped, in one pass.
//
// A save folder often holds more than one encrypted file -- the metadata's own
// list is the authoritative answer to which -- and taking them off one at a
// time means re-picking the target for each. This walks SAVEDATA_FILE_LIST
// instead.
//
// Deliberately NOT all-or-nothing. Each file is independent, so one that is
// missing from the folder or already plaintext should not stop the rest; the
// count at the end says what actually happened, and the log names every file
// either way. Stopping at the first failure would leave a half-done save with
// no way to tell how far it got.
//
// `wrap` says which direction. The per-file functions log their own detail.
//
static void psp_all_files(bool wrap) {
    std::vector<unsigned char> sfo;
    if (!read_all(g_app.psp.sfo_path, sfo)) {
        g_app.append_log("[!] PSP: could not read PARAM.SFO");
        g_app.show_log = true;
        g_app.apply_msg   = "PARAM.SFO could not be read, so nothing was changed.";
        g_app.apply_title = wrap ? "Re-encrypt all files" : "Decrypt all files";
        g_app.open_apply_popup = true;
        return;
    }

    const int n = apsp_sfo_file_count(sfo.data(), sfo.size());
    if (n <= 0) {
        g_app.append_log("PSP: PARAM.SFO lists no encrypted files");
        g_app.show_log = true;
        g_app.apply_msg   = "PARAM.SFO lists no encrypted files, so there was nothing to do.";
        g_app.apply_title = wrap ? "Re-encrypt all files" : "Decrypt all files";
        g_app.open_apply_popup = true;
        return;
    }

    const std::string folder = dir_of(g_app.psp.sfo_path);
    int done = 0, failed = 0, absent = 0;

    for (int i = 0; i < n; i++) {
        char name[APSP_NAME_LEN + 1];
        if (apsp_sfo_file_name(sfo.data(), sfo.size(), i, name, sizeof name) != APSP_OK)
            continue;

        const std::string path = folder + name;
        if (!std::ifstream(path)) {
            g_app.append_log((std::string("PSP: ") + name
                              + " is listed but not in the folder - skipped").c_str());
            absent++;
            continue;
        }
        if (wrap ? psp_wrap_file(path, name) : psp_unwrap_file(path, name)) done++;
        else                                                               failed++;
    }

    char msg[256];
    snprintf(msg, sizeof msg, "PSP: %s %d of %d listed file%s%s%s",
             wrap ? "re-encrypted" : "decrypted", done, n, n == 1 ? "" : "s",
             absent ? ", some not in the folder" : "",
             failed ? ", some failed - see above" : "");
    g_app.append_log(msg);

    // The log is raised only when something went wrong, like every other
    // button here. A clean run says so in the result box instead -- an action
    // the user asked for has to answer, and silence reads as "nothing
    // happened" whether or not it did.
    if (failed) g_app.show_log = true;

    char box[320];
    if (failed)
        snprintf(box, sizeof box,
                 "%s %d of %d file(s). %d failed.\nCheck the log for which, and why.",
                 wrap ? "Re-encrypted" : "Decrypted", done, n, failed);
    else
        snprintf(box, sizeof box, "All done - %s %d of %d file(s)%s.",
                 wrap ? "re-encrypted" : "decrypted", done, n,
                 absent ? ", the rest were not in the folder" : "");
    g_app.apply_msg   = box;
    g_app.apply_title = wrap ? "Re-encrypt all files" : "Decrypt all files";
    g_app.open_apply_popup = true;

    // The target is one of these, so the checkbox has to follow or Apply would
    // unwrap an already-plaintext file (or wrap one twice).
    if (done) g_app.psp.wrap = wrap;
}

// Regenerate the PARAM.SFO hashes alone — what an already-plaintext save needs
// after something edited it.
static bool psp_resign() {
    std::vector<unsigned char> sfo;
    if (!read_all(g_app.psp.sfo_path, sfo)) {
        g_app.append_log("[!] PSP: could not read PARAM.SFO");
        return false;
    }
    int rc = apsp_resign(sfo.data(), sfo.size());
    if (rc != APSP_OK) {
        g_app.append_log((std::string("[!] PSP resign failed: ") + apsp_strerror(rc)).c_str());
        return false;
    }
    if (!write_all(g_app.psp.sfo_path, sfo.data(), sfo.size())) {
        g_app.append_log("[!] PSP: could not write PARAM.SFO");
        return false;
    }
    g_app.append_log("PARAM.SFO resigned");
    return true;
}

// ---- the PS3's own savedata encryption -------------------------------------

// The secure file ID, from the database in the bundle. Offline by design, the
// same way the PSP's game keys are: PS3/games.conf travels in
// apollo-patches.zip, so the desktop app needs no network to open a save. The
// MATCHING is C shared with the page (apfd_sfid_from_conf) -- longest save
// directory prefix for the section, first pattern in file order for the file.
static bool ps3_key_from_bundle(const std::string& folder, const std::string& file,
                                unsigned char sfid[APFD_SFID_LEN], std::string& note) {
    char*  text = nullptr;
    size_t len  = 0;

    if (!g_db.db || !patchdb_read_file(g_db.db, "PS3/games.conf", &text, &len)) {
        note = "no key database in the bundle";
        return false;
    }

    char entry[80] = "";
    int rc = apfd_sfid_from_conf(text, len, folder.c_str(), file.c_str(),
                                 sfid, entry, sizeof entry);
    free(text);

    if (rc != APFD_OK) { note = apfd_strerror(rc); return false; }
    note = std::string("from the Apollo database (") + entry + ")";
    return true;
}

// The disc hash key the same section names, for re-binding. Almost every game
// names none and falls back to a built-in one, so a miss is the normal answer
// and the caller carries on.
static std::string ps3_dhk_from_bundle(const std::string& folder) {
    char*  text = nullptr;
    size_t len  = 0;
    unsigned char dhk[APFD_DHK_LEN];
    char hex[APFD_DHK_LEN * 2 + 1] = "";

    if (!g_db.db || !patchdb_read_file(g_db.db, "PS3/games.conf", &text, &len))
        return "";

    int rc = apfd_dhk_from_conf(text, len, folder.c_str(), dhk);
    free(text);
    if (rc != APFD_OK)
        return "";

    for (int i = 0; i < APFD_DHK_LEN; i++)
        snprintf(hex + i * 2, 3, "%02X", dhk[i]);
    return hex;
}

// Is the chosen target a PS3 save the console encrypted? Answered by looking
// for a PARAM.PFD beside it whose entry table LISTS it. That table is the
// authoritative answer to what is protected -- a game that encrypts nothing
// ships a PARAM.PFD holding only PARAM.SFO -- so no key database is consulted
// to find out, and a stale one cannot make the answer wrong.
static void ps3_detect() {
    g_app.ps3.clear();
    if (g_app.target_path.empty()) return;

    const std::string folder_path = dir_of(g_app.target_path);
    const std::string pfd_path = folder_path + "PARAM.PFD";
    std::vector<unsigned char> pfd;
    if (!read_all(pfd_path, pfd) || apfd_valid(pfd.data(), pfd.size()) != APFD_OK)
        return;

    const char* target = base_name(g_app.target_path);
    int index = apfd_find(pfd.data(), pfd.size(), target);
    if (index < 0) return;               // listed by nothing: not protected

    char listed[APFD_NAME_LEN] = "";
    apfd_entry_name(pfd.data(), pfd.size(), index, listed, sizeof listed);

    // PARAM.SFO is listed in every PARAM.PFD and is never encrypted, so a
    // target that IS the SFO is not something this offers to unwrap.
    if (apfd_entry_has_builtin_key(listed)) return;

    g_app.ps3.found    = true;
    g_app.ps3.pfd_path = pfd_path;
    g_app.ps3.listed   = listed;
    g_app.ps3.version  = apfd_version(pfd.data(), pfd.size());
    g_app.ps3.trophy   = apfd_is_trophy(pfd.data(), pfd.size()) != 0;

    // The save folder, which is the key lookup. The folder on disk is the
    // answer, and a PARAM.SFO beside it confirms it -- worth preferring,
    // because a folder somebody renamed on the way off the console would
    // otherwise look up nothing.
    {
        std::string dir = folder_path;
        while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\'))
            dir.pop_back();
        g_app.ps3.folder = base_name(dir);
    }

    std::vector<unsigned char> sfo;
    const std::string sfo_path = folder_path + "PARAM.SFO";
    if (read_all(sfo_path, sfo)) {
        char named[64] = "";
        g_app.ps3.sfo_path = sfo_path;
        if (apsp_sfo_directory(sfo.data(), sfo.size(), named, sizeof named) == APSP_OK
            && named[0])
            g_app.ps3.folder = named;
        // Which account it is signed to now, so that signing it to another is
        // a visible change rather than a leap of faith.
        apfd_sfo_account_id(sfo.data(), sfo.size(),
                            g_app.ps3.account, sizeof g_app.ps3.account);
    }

    if (ps3_key_from_bundle(g_app.ps3.folder, g_app.ps3.listed,
                            g_app.ps3.sfid, g_app.ps3.key_note)) {
        g_app.ps3.have_key = true;
        for (int i = 0; i < APFD_SFID_LEN; i++)
            snprintf(g_app.ps3.key_hex + i * 2, 3, "%02X", g_app.ps3.sfid[i]);
    }

    // Does PARAM.PFD still describe the file? A save that already disagrees
    // was damaged before it got here, and patching would sign the damage into
    // place. Only asked when the file is still the length the console left it:
    // one somebody already decrypted cannot match, and saying so would be
    // crying wolf.
    std::vector<unsigned char> disk;
    long long size = apfd_entry_size(pfd.data(), pfd.size(), index);
    if (g_app.ps3.have_key && size >= 0 && read_all(g_app.target_path, disk)
        && disk.size() == apfd_encrypted_size((size_t)size)) {
        g_app.ps3.hash_checked = true;
        g_app.ps3.hash_ok = apfd_verify_file(pfd.data(), pfd.size(),
                                             g_app.ps3.listed.c_str(),
                                             disk.data(), disk.size(),
                                             g_app.ps3.sfid) == APFD_OK;
    }

    char msg[512];
    snprintf(msg, sizeof msg, "PS3 save detected: %s in %s, PFD v%d, key %s",
             g_app.ps3.listed.c_str(), g_app.ps3.folder.c_str(),
             g_app.ps3.version, g_app.ps3.key_note.c_str());
    g_app.append_log(msg);
    if (g_app.ps3.hash_checked && !g_app.ps3.hash_ok)
        g_app.append_log("[!] PS3: PARAM.PFD's recorded hash does not match this file");
}

// Take the console's layer off the target file, in place.
static bool ps3_unwrap_file(const std::string& path, const char* listed,
                            const unsigned char* sfid) {
    std::vector<unsigned char> pfd, enc;
    if (!read_all(g_app.ps3.pfd_path, pfd) || !read_all(path, enc)) {
        g_app.append_log((std::string("[!] PS3: could not read ") + listed
                          + " or its PARAM.PFD").c_str());
        return false;
    }

    long long want = apfd_decrypted_size(pfd.data(), pfd.size(), listed);
    if (want < 0) {
        g_app.append_log((std::string("[!] PS3 decrypt failed: ")
                          + apfd_strerror((int)want)).c_str());
        return false;
    }

    std::vector<unsigned char> out((size_t)want ? (size_t)want : 1);
    size_t got = 0;
    int rc = apfd_decrypt(pfd.data(), pfd.size(), listed,
                          enc.data(), enc.size(), sfid,
                          out.data(), out.size(), &got);
    if (rc != APFD_OK) {
        g_app.append_log((std::string("[!] PS3 decrypt failed: ") + apfd_strerror(rc)).c_str());
        return false;
    }
    if (!write_all(path, out.data(), got)) {
        g_app.append_log((std::string("[!] PS3: could not write ") + listed).c_str());
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof msg, "PS3 layer removed from %s: %zu -> %zu bytes",
             listed, enc.size(), got);
    g_app.append_log(msg);
    return true;
}

static bool ps3_unwrap_target() {
    return ps3_unwrap_file(g_app.target_path, g_app.ps3.listed.c_str(), g_app.ps3.sfid);
}

// ...and put it back, rewriting PARAM.PFD with it. Both files are written or
// neither is: a save whose PARAM.PFD does not match its data does not load, so
// a half-done wrap is worse than none.
static bool ps3_wrap_file(const std::string& path, const char* listed,
                          const unsigned char* sfid) {
    std::vector<unsigned char> pfd, plain;
    if (!read_all(g_app.ps3.pfd_path, pfd) || !read_all(path, plain)) {
        g_app.append_log((std::string("[!] PS3: could not read ") + listed
                          + " or its PARAM.PFD").c_str());
        return false;
    }

    std::vector<unsigned char> out(apfd_encrypted_size(plain.size()));
    size_t got = 0;
    int rc = apfd_encrypt(pfd.data(), pfd.size(), listed,
                          plain.data(), plain.size(), sfid,
                          out.data(), out.size(), &got);
    if (rc != APFD_OK) {
        g_app.append_log((std::string("[!] PS3 encrypt failed: ") + apfd_strerror(rc)).c_str());
        return false;
    }

    // PARAM.PFD first, for the same reason the PSP path writes PARAM.SFO
    // first: this order leaves the metadata describing bytes that CAN be
    // produced again by re-running the wrap.
    if (!write_all(g_app.ps3.pfd_path, pfd.data(), pfd.size())) {
        g_app.append_log("[!] PS3: could not write PARAM.PFD");
        return false;
    }
    if (!write_all(path, out.data(), got)) {
        g_app.append_log((std::string("[!] PS3: could not write ") + listed).c_str());
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof msg,
             "PS3 layer restored on %s: %zu -> %zu bytes, PARAM.PFD rewritten",
             listed, plain.size(), got);
    g_app.append_log(msg);
    return true;
}

static bool ps3_wrap_target() {
    return ps3_wrap_file(g_app.target_path, g_app.ps3.listed.c_str(), g_app.ps3.sfid);
}

//
// Every protected file in the save, in one pass. See psp_all_files() above for
// why this is not all-or-nothing.
//
// Two things differ from the PSP. PARAM.SFO is listed in every PARAM.PFD and
// is never encrypted, so it is skipped by the same test ps3_detect() uses
// rather than being offered and then failing. And the secure file ID is per
// FILE here, not per title, so each one is looked up on its own -- a file the
// key database does not cover is reported and passed over, which is the right
// answer: guessing a key produces noise that looks like output.
//
static void ps3_all_files(bool wrap) {
    std::vector<unsigned char> pfd;
    if (!read_all(g_app.ps3.pfd_path, pfd)) {
        g_app.append_log("[!] PS3: could not read PARAM.PFD");
        g_app.show_log = true;
        g_app.apply_msg   = "PARAM.PFD could not be read, so nothing was changed.";
        g_app.apply_title = wrap ? "Re-encrypt all files" : "Decrypt all files";
        g_app.open_apply_popup = true;
        return;
    }

    const int n = apfd_entry_count(pfd.data(), pfd.size());
    if (n <= 0) {
        g_app.append_log("PS3: PARAM.PFD lists no files");
        g_app.show_log = true;
        g_app.apply_msg   = "PARAM.PFD lists no files, so there was nothing to do.";
        g_app.apply_title = wrap ? "Re-encrypt all files" : "Decrypt all files";
        g_app.open_apply_popup = true;
        return;
    }

    const std::string folder = dir_of(g_app.ps3.pfd_path);
    int done = 0, failed = 0, eligible = 0;

    for (int i = 0; i < n; i++) {
        char name[APFD_NAME_LEN] = "";
        if (apfd_entry_name(pfd.data(), pfd.size(), i, name, sizeof name) != APFD_OK)
            continue;
        if (apfd_entry_has_builtin_key(name))      // PARAM.SFO: listed, never encrypted
            continue;

        eligible++;
        const std::string path = folder + name;
        if (!std::ifstream(path)) {
            g_app.append_log((std::string("PS3: ") + name
                              + " is listed but not in the folder - skipped").c_str());
            continue;
        }

        unsigned char sfid[APFD_SFID_LEN];
        std::string note;
        if (!ps3_key_from_bundle(g_app.ps3.folder, name, sfid, note)) {
            g_app.append_log((std::string("PS3: no key for ") + name + " (" + note
                              + ") - skipped").c_str());
            failed++;
            continue;
        }

        if (wrap ? ps3_wrap_file(path, name, sfid) : ps3_unwrap_file(path, name, sfid)) done++;
        else                                                                            failed++;
    }

    char msg[256];
    if (eligible == 0)
        snprintf(msg, sizeof msg,
                 "PS3: this save has no encrypted files - PARAM.PFD lists %d, "
                 "all unprotected", n);
    else
        snprintf(msg, sizeof msg, "PS3: %s %d of %d protected file%s%s",
                 wrap ? "re-encrypted" : "decrypted", done, eligible,
                 eligible == 1 ? "" : "s", failed ? ", some failed - see above" : "");
    g_app.append_log(msg);
    if (failed || eligible == 0) g_app.show_log = true;

    char box[320];
    if (eligible == 0)
        snprintf(box, sizeof box,
                 "This save has no encrypted files - PARAM.PFD lists %d, and "
                 "none of them is protected.", n);
    else if (failed)
        snprintf(box, sizeof box,
                 "%s %d of %d file(s). %d could not be done.\nCheck the log "
                 "for which, and why - a missing key is the usual reason.",
                 wrap ? "Re-encrypted" : "Decrypted", done, eligible, failed);
    else
        snprintf(box, sizeof box, "All done - %s %d of %d protected file(s).",
                 wrap ? "re-encrypted" : "decrypted", done, eligible);
    g_app.apply_msg   = box;
    g_app.apply_title = wrap ? "Re-encrypt all files" : "Decrypt all files";
    g_app.open_apply_popup = true;

    if (done) g_app.ps3.wrap = wrap;
}

// Regenerate the PARAM.PFD signatures alone, leaving every entry as it is.
static bool ps3_resign() {
    std::vector<unsigned char> pfd;
    if (!read_all(g_app.ps3.pfd_path, pfd)) {
        g_app.append_log("[!] PS3: could not read PARAM.PFD");
        return false;
    }
    int rc = apfd_resign(pfd.data(), pfd.size());
    if (rc != APFD_OK) {
        g_app.append_log((std::string("[!] PS3 resign failed: ") + apfd_strerror(rc)).c_str());
        return false;
    }
    if (!write_all(g_app.ps3.pfd_path, pfd.data(), pfd.size())) {
        g_app.append_log("[!] PS3: could not write PARAM.PFD");
        return false;
    }
    g_app.append_log("PARAM.PFD resigned");
    return true;
}

// Re-bind the save to the console named in Settings: rewrite the three
// PARAM.SFO hashes that name a machine, and resign PARAM.PFD around them.
//
// Defined with the save browser, further down: the list keeps its own copy of
// every save, and the Owner column reads from that one rather than from the
// save that happens to be open.
static void note_account_change(const std::string& save_path, const char *account);

// PARAM.SFO is read, not written. Its own account fields are a separate
// binding, and this does not touch them.
//
// Re-sign this save to the PSN account named in Settings.
//
// The other half of "make this save mine", and usually the better half:
// re-binding writes the IDPS of ONE MACHINE into a PARAM.PFD hash, where an
// account ID travels with the account and the save then loads on any PS3 that
// account has signed in to.
//
// The order matters and is the whole reason this is one action rather than
// two buttons. PARAM.SFO is rewritten first; the PFD's hash of it is taken
// over those bytes, so apfd_update_file recomputes it and re-signs the
// database around it. Stop after the first step and the save no longer loads
// at all, which is worse than where it started.
//
static bool ps3_account_resign() {
    std::vector<unsigned char> pfd, sfo;

    if (strlen(g_saved.account_hex) != APFD_ACCT_ID_LEN) {
        g_app.append_log("[!] PS3: no account ID in Settings, so there is nothing "
                         "to sign to");
        return false;
    }
    if (g_app.ps3.sfo_path.empty()) {
        g_app.append_log("[!] PS3: no PARAM.SFO beside this save - the account "
                         "fields live in it");
        return false;
    }
    if (!read_all(g_app.ps3.pfd_path, pfd) || !read_all(g_app.ps3.sfo_path, sfo)) {
        g_app.append_log("[!] PS3: could not read PARAM.PFD or PARAM.SFO");
        return false;
    }

    char was[APFD_ACCT_ID_LEN + 1] = "";
    apfd_sfo_account_id(sfo.data(), sfo.size(), was, sizeof was);

    int rc = apfd_sfo_set_account_id(sfo.data(), sfo.size(), g_saved.account_hex);
    if (rc != APFD_OK) {
        g_app.append_log((std::string("[!] PS3 account re-sign failed: ")
                          + apfd_strerror(rc)).c_str());
        return false;
    }

    //
    // The PFD's record of PARAM.SFO, over the bytes just written.
    //
    // The console is cleared across the call and put back afterwards, because
    // apfd_update_file re-binds PARAM.SFO's other three hashes whenever one is
    // named -- ambient state, not an argument. Leaving it set would make this
    // button quietly do the console's job as well, which is a separate choice
    // with a separate button.
    //
    apfd_console_t saved;
    const bool had_console = apfd_get_console(&saved) != 0;
    if (had_console) apfd_set_console(nullptr);

    rc = apfd_update_file(pfd.data(), pfd.size(), "PARAM.SFO",
                          sfo.data(), sfo.size(), nullptr);

    if (had_console) apfd_set_console(&saved);
    if (rc != APFD_OK) {
        g_app.append_log((std::string("[!] PS3: PARAM.PFD would not record the new "
                                      "PARAM.SFO: ") + apfd_strerror(rc)).c_str());
        return false;
    }

    // PARAM.SFO first: if the PFD write fails after it, the save is one
    // apfd_resign away from correct rather than silently mismatched.
    if (!write_all(g_app.ps3.sfo_path, sfo.data(), sfo.size())) {
        g_app.append_log("[!] PS3: could not write PARAM.SFO");
        return false;
    }
    if (!write_all(g_app.ps3.pfd_path, pfd.data(), pfd.size())) {
        g_app.append_log("[!] PS3: PARAM.SFO was written but PARAM.PFD was not - "
                         "use \"Resign PARAM.PFD\" to finish, or the save will not load");
        return false;
    }

    g_app.ps3.hash_checked = false;   // the file changed under the check
    // Both files are on disk now, so the panel should say so. The web panel
    // deliberately does NOT do this: there the bytes are only offered for
    // download, and claiming the save had changed would be a lie until the
    // person saved them.
    snprintf(g_app.ps3.account, sizeof g_app.ps3.account, "%s", g_saved.account_hex);
    if (g_app.has_save) {
        g_app.save.account = g_saved.account_hex;
        note_account_change(g_app.save.path, g_saved.account_hex);
    }

    g_app.append_log((std::string("PARAM.SFO signed to account ")
                      + g_saved.account_hex
                      + (was[0] ? std::string(" (was ") + was + ")" : std::string())
                      + ", and PARAM.PFD updated to match").c_str());
    return true;
}

//
// The same thing for a PS4 or Vita save, which is very much simpler.
//
// There is no PARAM.PFD and no hash to keep in step -- these are decrypted
// saves with the console's own layer already off -- so this is one value in
// one file. ACCOUNT_ID is rewritten at its own eight bytes, so the file
// neither grows nor moves and nothing else in it has to be touched.
//
// Which is also why there is no .bak: an eight-byte overwrite at a known
// offset is undone by signing the save back, and a stray param.sfo.bak inside
// sce_sys is worse than the thing it guards against.
//
static bool sfo_account_resign() {
    std::vector<unsigned char> sfo;

    if (!g_app.has_save ||
        (g_app.save.platform_id != ASAVE_PS4 && g_app.save.platform_id != ASAVE_PSVITA)) {
        g_app.append_log("[!] Signing this way is for PS4 and Vita saves");
        return false;
    }
    if (strlen(g_saved.account_hex) != APFD_ACCT_ID_LEN) {
        g_app.append_log("[!] No account ID in Settings, so there is nothing to sign to");
        return false;
    }

    // The same 16 hex digits the PS3 writes as text are the PS4's number.
    const uint64_t want = strtoull(g_saved.account_hex, nullptr, 16);
    if (!want) {
        g_app.append_log("[!] An account ID of zero would take the save's owner away "
                         "rather than set one");
        return false;
    }

    const std::string path = (fs::path(g_app.save.path) / "sce_sys" / "param.sfo").string();
    if (!read_all(path, sfo)) {
        g_app.append_log(("[!] Could not read " + path).c_str());
        return false;
    }

    uint64_t was = 0;
    const bool had = asfo_account_id(sfo.data(), sfo.size(), &was) == ASFO_OK;

    int rc = asfo_set_account_id(sfo.data(), sfo.size(), want);
    if (rc != ASFO_OK) {
        g_app.append_log(rc == ASFO_ERR_MISSING
            ? "[!] This save's param.sfo carries no ACCOUNT_ID to assign"
            : "[!] This save's ACCOUNT_ID is not the eight-byte kind a PS4 or Vita writes");
        return false;
    }
    if (!write_all(path, sfo.data(), sfo.size())) {
        g_app.append_log(("[!] Could not write " + path).c_str());
        return false;
    }

    char now[17];
    snprintf(now, sizeof now, "%016llx", (unsigned long long)want);
    g_app.save.account = now;
    note_account_change(g_app.save.path, now);

    char msg[256];
    if (had && was)
        snprintf(msg, sizeof msg, "param.sfo signed to account %s (was %016llx)",
                 now, (unsigned long long)was);
    else
        snprintf(msg, sizeof msg, "param.sfo signed to account %s (it named none before)", now);
    g_app.append_log(msg);
    return true;
}

static bool ps3_rebind() {
    std::vector<unsigned char> pfd, sfo;
    apfd_console_t console, saved;

    if (!apfd_get_console(&saved)) {
        g_app.append_log("[!] PS3: no console ID in Settings, so there is nothing to bind to");
        return false;
    }
    if (!read_all(g_app.ps3.pfd_path, pfd) || !read_all(g_app.ps3.sfo_path, sfo)) {
        g_app.append_log("[!] PS3: could not read PARAM.PFD or PARAM.SFO");
        return false;
    }

    // The disc hash key keys one of the three and is per GAME, so it rides
    // with the call rather than living in Settings.
    console = saved;
    memset(console.disc_hash_key, 0, APFD_DHK_LEN);
    const std::string dhk = ps3_dhk_from_bundle(g_app.ps3.folder);
    if (dhk.size() == APFD_DHK_LEN * 2)
        apfd_sfid_from_hex(dhk.c_str(), console.disc_hash_key);
    apfd_set_console(&console);

    int rc = apfd_update_file(pfd.data(), pfd.size(), "PARAM.SFO",
                              sfo.data(), sfo.size(), nullptr);
    apfd_set_console(&saved);

    if (rc != APFD_OK) {
        g_app.append_log((std::string("[!] PS3 re-bind failed: ") + apfd_strerror(rc)).c_str());
        return false;
    }
    if (!write_all(g_app.ps3.pfd_path, pfd.data(), pfd.size())) {
        g_app.append_log("[!] PS3: could not write PARAM.PFD");
        return false;
    }

    const std::string note = "PARAM.PFD re-bound to the console in Settings ("
                           + (dhk.empty() ? std::string("fallback disc hash key")
                                          : "disc hash key " + dhk) + ")";
    g_app.append_log(note.c_str());
    return true;
}

static void load_patch(const std::string& path) {
    g_app.close();
    apctl_session_t* s = apctl_open_file(path.c_str());
    if (!s) { g_app.append_log("[!] Could not open patch file"); return; }

    // Kept as text so it can be read: parsing keeps only the codes, dropping
    // the author's comments, target-file lines and anything else the format
    // allows.
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::string raw((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
        g_app.patch_bytes = raw;              // verbatim: what a save writes back
        g_app.patch_raw = strip_cr(raw);      // CR-stripped: what ImGui draws
        g_app.patch_raw_ascii =
            is_plain_ascii(g_app.patch_raw.data(), g_app.patch_raw.size());
    }
    adopt_session(s, path, nullptr, nullptr);   // loose file: no platform tag
}

// ---- patch database --------------------------------------------------------

static std::string lowered(const std::string& in) {
    std::string out = in;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

// Does this name end in that extension, whatever case it was written in? A
// .PSV comes off a PS3 in capitals and off a repacking tool in lower case.
static bool ends_with_ci(const std::string& name, const char* ext) {
    const size_t k = strlen(ext);
    if (name.size() < k) return false;
    return lowered(name).compare(name.size() - k, k, ext) == 0;
}

// Opened once at startup. Also extracts the archive's Python modules to a cache
// directory and points the engine at it: MicroPython imports go through
// stat()/open() on real paths, so without this a Python code that imports a
// helper module only works when the app is launched from the right directory.
static void init_patchdb() {
    g_db.db = patchdb_open(nullptr);
    if (!g_db.db) {
        g_db.error = patchdb_last_error();
        g_app.append_log(("Patch database unavailable: " + g_db.error).c_str());
        return;
    }

    const int n = patchdb_count(g_db.db);
    g_db.platforms.push_back("All");
    g_db.haystack.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        const patchdb_entry_t* e = patchdb_at(g_db.db, i);
        g_db.haystack.push_back(lowered(std::string(e->name) + " " + e->title_id));
        g_db.by_title_id.emplace(lowered(e->title_id), i);
        // Platforms in database order, deduplicated — no fixed list to keep in
        // sync when the database gains one.
        bool seen = false;
        for (size_t p = 1; p < g_db.platforms.size(); ++p)
            if (g_db.platforms[p] == e->platform) { seen = true; break; }
        if (!seen) g_db.platforms.push_back(e->platform);
    }

    char buf[512];
    snprintf(buf, sizeof buf, "Patch database: %d patches from %s",
             n, patchdb_path(g_db.db));
    g_app.append_log(buf);

    // The game-name catalogue. Optional: a bundle built before it existed
    // simply has none, and every save that names itself is unaffected.
    {
        char*  text = nullptr;
        size_t len  = 0;
        if (patchdb_read_file(g_db.db, "titles.tsv", &text, &len)) {
            g_db.titles.assign(text, len);
            free(text);
            snprintf(buf, sizeof buf, "Game names: %zu KB of title IDs",
                     (g_db.titles.size() + 1023) / 1024);
            g_app.append_log(buf);
        } else {
            g_app.append_log("[!] No titles.tsv in the bundle - Vita saves will "
                             "be listed by folder unless the patch database "
                             "happens to name them.");
        }
    }

    if (const char* cache = patchdb_cache_dir()) {
        int written = patchdb_extract_python(g_db.db, cache);
        if (written > 0) {
            apctl_set_data_path(cache);
            snprintf(buf, sizeof buf, "Python modules ready (%d) in %spython",
                     written, cache);
            g_app.append_log(buf);
        } else {
            g_app.append_log("[!] Could not unpack the Python modules; Python "
                             "codes that import one will fail.");
        }
    }
}

static void refilter_db() {
    g_db.hits.clear();
    if (!g_db.db) return;

    const std::string needle = lowered(g_db.search);
    const char* platform = g_db.platform > 0 ? g_db.platforms[size_t(g_db.platform)].c_str()
                                             : nullptr;

    for (int i = 0; i < patchdb_count(g_db.db); ++i) {
        const patchdb_entry_t* e = patchdb_at(g_db.db, i);
        if (platform && strcmp(e->platform, platform) != 0) continue;
        if (!needle.empty() && g_db.haystack[size_t(i)].find(needle) == std::string::npos)
            continue;
        g_db.hits.push_back(i);
    }
    g_db.refilter = false;
}

//
// Which patch in the database this target's own location names, or -1.
//
// Every title ID in the database is exactly nine characters, on every platform,
// and a save folder is named after it with an optional suffix — "ULUS10391",
// "ULJM05500DATA00", "UCUS98751_DATA01". So the first nine characters of the
// folder are the lookup, and an EXACT match against a real title ID is the
// guard: "Downloads" and "Brave_Sto" match nothing, which is the right answer
// for a loose file and for the handful of games that name their save folder
// after themselves rather than their title.
//
// The PSP's SAVEDATA_DIRECTORY is preferred over the folder on disk when there
// is one: it is what the console recorded, and survives someone renaming the
// directory on the way off the Memory Stick.
//
// The folder is not always enough, though. A PS4 or Vita save is in a folder
// named after its SLOT ("SLOT0", "JOJOASB.S"), with the title ID only inside
// sce_sys/param.sfo — so the save browser, which has already read that, passes
// it in as g_app.title_hint and this believes it over anything on disk.
//
static int detect_patch_for_target() {
    g_app.match_index = -1;
    g_app.match_label.clear();
    if (!g_db.db || g_app.target_path.empty()) return -1;

    std::string from = g_app.title_hint;
    if (from.empty() && g_app.psp.found && !g_app.psp.directory.empty())
        from = g_app.psp.directory;
    if (from.empty()) {
        // The containing folder's own name. dir_of() keeps its trailing
        // separator, so drop that before taking the last component.
        std::string dir = dir_of(g_app.target_path);
        if (!dir.empty()) dir.erase(dir.size() - 1);
        size_t slash = dir.find_last_of("/\\");
        from = slash == std::string::npos ? dir : dir.substr(slash + 1);
    }
    if (from.size() < 9) return -1;

    auto it = g_db.by_title_id.find(lowered(from.substr(0, 9)));
    if (it == g_db.by_title_id.end()) return -1;

    const patchdb_entry_t* e = patchdb_at(g_db.db, it->second);
    if (!e) return -1;
    // A title ID belongs to one platform (the letters say which), but if the
    // PSP detection already spoke, believe it over the database.
    if (g_app.psp.found && strcmp(e->platform, "PSP") != 0) return -1;

    g_app.match_index = it->second;
    g_app.match_label = std::string(e->platform) + "/" + e->title_id + ".savepatch";
    return g_app.match_index;
}

// Defined below, next to the database browser it belongs to.
static void load_patch_from_db(int index);

// Defined below with the save browser. Reused here so that a save FOLDER
// dropped on the window, or named on the command line, goes through exactly
// the same identification as one picked from the list -- same icon, same file
// list, same name.
static bool examine(const fs::path& dir, SaveEntry& out);
static bool examine_psvcard(const fs::path& file, SaveEntry& out);
static void resolve_patches(std::vector<SaveEntry>& saves);
static void commit_open_save(const SaveEntry& save);

//
// Take `path` as the save to patch, and work out everything that follows from
// it. Shared by the "Choose target..." dialog, the command line and files
// dropped on the window, so all three behave identically.
//
static void adopt_target(const std::string& path, const std::string& title_hint = "") {
    g_app.target_path = path;
    g_app.title_hint  = title_hint;

    // Whatever .PSV was last opened is not this one. Cleared here rather than
    // at each call site so that nothing can be pointed at a loose file and
    // still be holding a container to write it back into -- which would put
    // one save's data inside another save's file. open_file_path() re-arms it
    // immediately afterwards for a target that really did come out of one.
    g_app.psvcard.clear();

    // Look around the new target for the metadata that lists it, so a save the
    // console encrypted announces itself instead of having to be declared. Only
    // one can match: the two look for different files.
    psp_detect();
    ps3_detect();

    // ...and for the patch its title ID names. Loaded outright only when
    // nothing is open: load_patch_from_db() closes the current session, and
    // silently discarding somebody's edited codes to be helpful is not a trade
    // worth making. With a patch already open this only offers (see
    // draw_main_window).
    if (detect_patch_for_target() >= 0 && !g_app.session) {
        g_app.append_log(("Save matches " + g_app.match_label
                          + " - loading it from the database").c_str());
        load_patch_from_db(g_app.match_index);
    }
}

//
// Open whatever this path is.
//
// A .savepatch is the patch; anything else is the save to patch. Decided on
// the extension rather than by sniffing, so it matches what the file dialogs
// filter on and stays predictable — a patch under another name can still be
// opened through File > Open.
//
// Used by the command line, by files dropped on the window, and so by
// whatever a desktop environment does with a file association.
//
static void open_path(const std::string& path) {
    //
    // A FOLDER, which for either console is the obvious thing to drag: the
    // whole save directory. It goes through the browser's own identification,
    // so a dropped save is the same thing as one picked from the list --
    // named, iconned, with its files listed and its codes loaded.
    //
    // The directory itself must never become the target: nothing can read it,
    // and the failure would surface much later with no explanation.
    //
    if (is_dir(path)) {
        SaveEntry found;

        if (examine(fs::path(path), found)) {
            // The one thing the scan does afterwards and examine() does not:
            // look the title ID up, which is where a Vita save's name comes
            // from and what says whether there are codes.
            std::vector<SaveEntry> one{ found };
            resolve_patches(one);
            commit_open_save(one.front());
            return;
        }

        g_app.append_log(("[!] " + path + " is a folder, and not a save one "
                          "(no PARAM.SFO, and no sce_sys/param.sfo). "
                          "Pick the save file itself.").c_str());
        return;
    }

    // Not a folder, and not there either. Worth saying now: otherwise it
    // becomes a target that nothing can read, and the only complaint arrives
    // at Apply, about a file the person thought they had opened.
    if (!std::ifstream(path)) {
        g_app.append_log(("[!] " + path + " cannot be read - is the path right, "
                          "and the drive still connected?").c_str());
        return;
    }

    const size_t dot = path.find_last_of('.');
    const std::string ext = dot == std::string::npos ? std::string()
                                                     : lowered(path.substr(dot));
    if (ext == ".savepatch") {
        load_patch(path);
        g_screen = SCREEN_PATCH;
        return;
    }

    //
    // A .PSV, which is a whole PS1 or PS2 save in one file -- the only console
    // save here that is not a folder. Taken through the same identification a
    // scanned one gets, so dropping it is the same as picking it from the
    // list: named from the catalogue, its inner files listed, its icon
    // rendered and its codes loaded.
    //
    // Sniffed rather than trusted to the extension: .PSV is also what a Vita
    // save's folder is sometimes called, and a file that merely ends in those
    // letters without the magic is not one of these.
    //
    if (ext == ".psv") {
        SaveEntry found;
        if (examine_psvcard(fs::path(path), found)) {
            std::vector<SaveEntry> one{ found };
            resolve_patches(one);
            commit_open_save(one.front());
            return;
        }
        g_app.append_log(("[!] " + path + " ends in .PSV but is not a PS1 or PS2 "
                          "save container - opening it as a plain file "
                          "instead.").c_str());
    }

    // A loose file. There is no save behind it, so the patcher screen shows
    // the pickers rather than a save header describing the previous one.
    g_app.has_save = false;
    adopt_target(path);
    g_screen = SCREEN_PATCH;
}

// ---- the save browser ------------------------------------------------------
//
// Point the app at wherever the saves are -- a memory stick, a folder pulled
// off a PS3's hard drive, a USB stick full of PS4 exports -- and it finds the
// saves in it and lists them. That is how the console apps work, and it is the
// difference between "choose target file..." (which means knowing that
// MHP2NDG.BIN inside ULUS10391DATA00 is the one) and picking a game by name.
//
// Finding them is the same question on all four consoles and has the same
// answer: a save is a FOLDER WITH A PARAM.SFO IN IT. Where that SFO sits is
// itself the first half of the identification --
//
//   <save>/PARAM.SFO           PSP or PS3, and the save is encrypted
//   <save>/sce_sys/param.sfo   PS4 or Vita, and it is not
//
// -- and core/saveinfo.c does the second half (which console, which game).
// Nothing here parses an SFO; this walks directories and nothing else, so the
// part that reads attacker-controlled offsets stays in one tested place.
//
// The walk runs on its own thread. A memory stick scans in well under a
// second, but nothing stops somebody choosing their home directory, and a UI
// that freezes for a minute looks broken rather than busy.
//
// Files a save carries that are never the target: the metadata itself, and
// the icons and jingle the console shows in its own save list.
static bool is_metadata(const std::string& name) {
    static const char* const skip[] = {
        "PARAM.SFO", "PARAM.PFD", "ICON0.PNG", "ICON1.PNG",
        "ICON1.PMF", "ICON0.PAM", "ICON1.PAM", "PIC0.PNG", "PIC1.PNG",
        "PIC1.PAM", "SND0.AT3", "KEYSTONE", "SCE_SYS",
    };
    const std::string up = [&] {
        std::string u = name;
        for (char& c : u) c = char(toupper((unsigned char)c));
        return u;
    }();
    for (const char* s : skip)
        if (up == s) return true;
    return false;
}

struct SaveBrowser {
    std::string              root;          // what was scanned
    std::vector<SaveEntry>   saves;
    std::vector<int>         hits;          // indices into saves, filtered
    char                     search[128] = "";
    // The consoles these saves are actually FROM -- "All", then one per
    // platform present, in the order they are listed here. Built from the scan
    // rather than from the patch database, which is a different question: the
    // database has no PS1 patches at all, so a PS1 save filtered against it
    // could be seen under "All" and nowhere else.
    std::vector<std::string> platforms;
    int                      platform = 0;  // 0 = all, else index into platforms
    bool                     only_coded = false;
    bool                     refilter = true;
    int                      selected = -1; // index into saves
    std::string              note;          // what the last scan had to say

    // The scan, which runs on its own thread. `done` is what the UI polls;
    // seeing it set is the point at which the thread is joined and its result
    // taken, so nothing is shared while both are running.
    std::thread              worker;
    std::atomic<bool>        running{false};
    std::atomic<bool>        done{false};
    std::atomic<bool>        cancel{false};
    std::atomic<int>         seen{0};       // directories looked at so far
    std::atomic<int>         too_deep{0};   // ...and ones the depth cap cut off
    std::vector<SaveEntry>   out;           // the thread's result
    std::string              out_note;
};
static SaveBrowser g_sb;

/*
 * A save was just re-signed, so the row in the list has to agree.
 *
 * The open save is a COPY of the list's entry -- a rescan re-sorts the list,
 * so holding a reference would dangle -- which means writing one of the two
 * leaves the other stale, and the Owner column would go on reporting the old
 * owner until the next rescan.
 */
static void note_account_change(const std::string& save_path, const char *account)
{
    for (SaveEntry& e : g_sb.saves)
        if (e.path == save_path) { e.account = account; return; }
}

/*
 * ...and the same for a container that was just rebuilt.
 *
 * A code may change the length of the file it patches, which moves everything
 * after it and changes the container. The sizes shown in the File dropdown and
 * the file list were read during the scan, so without this they go on
 * reporting what the save used to be until the next rescan.
 */
static void note_container_rebuilt(const std::string& psvcard_path)
{
    std::vector<unsigned char> buf;
    apsvc_info_t info;

    if (!read_all(psvcard_path, buf)) return;
    if (apsvc_info(buf.data(), buf.size(), &info) != APSVC_OK) return;
    if (info.file_count <= 0) return;

    std::vector<apsvc_file_t> inner(size_t(info.file_count));
    if (apsvc_files(buf.data(), buf.size(), inner.data(), info.file_count, nullptr) != APSVC_OK)
        return;

    auto refresh = [&](SaveEntry& e) {
        // Only the sizes: the file LIST is the container's own order and did
        // not change, and rebuilding it here would renumber a target the user
        // has already chosen.
        if (e.file_sizes.size() != inner.size()) return;
        for (size_t i = 0; i < inner.size(); i++)
            e.file_sizes[i] = inner[i].size;
    };

    if (g_app.has_save && g_app.save.path == psvcard_path) refresh(g_app.save);
    for (SaveEntry& e : g_sb.saves)
        if (e.path == psvcard_path) { refresh(e); break; }
}

// How deep to go looking, and how much to look at. A PS3's savedata sits five
// levels down (dev_hdd0/home/00000001/savedata/<save>), so eight is generous;
// the directory cap is only there so that pointing this at a whole disk stops
// rather than running for minutes.
static const int  SCAN_MAX_DEPTH = 8;
static const int  SCAN_MAX_DIRS  = 40000;

// Defined below, with the rest of the icon handling: a scan throws away the
// save list, and the decoded icon belongs to one of its entries.
static void icon_drop();

static bool read_meta(const fs::path& p, std::vector<unsigned char>& out) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    return read_all(p.string(), out);
}

// Every regular file in the save that could be the one a patch addresses.
// Recursive because a PS4 save can have subfolders, shallow because none of
// them nests deeply, and sce_sys is skipped outright -- it is the console's
// own metadata, never save data.
static void collect_files(const fs::path& root, const fs::path& dir,
                          int depth, std::vector<std::string>& out) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (out.size() >= 256) return;
        const std::string leaf = e.path().filename().string();
        if (leaf.empty() || leaf[0] == '.') continue;

        if (e.is_directory(ec)) {
            if (depth > 0 && !is_metadata(leaf))
                collect_files(root, e.path(), depth - 1, out);
            continue;
        }
        if (!e.is_regular_file(ec) || is_metadata(leaf)) continue;

        const std::string rel = fs::relative(e.path(), root, ec).generic_string();
        // An empty result would make the save FOLDER the target, which nothing
        // can read; the plain file name is right for everything but the
        // subfolder case, and is the better thing to lose.
        out.push_back(ec || rel.empty() ? leaf : rel);
    }
}

//
// The save's icon: what the console's own save list shows for it, and the
// quickest way to tell apart six folders with nearly the same name.
//
//   PSP, PS3    ICON0.PNG beside the data files
//   PS4, Vita   sce_sys/icon0.png
//
// Both spellings of each are tried. The case is per console, and only some
// filesystems care -- a save copied to a Mac and then opened on Linux is the
// case that would otherwise lose its icon.
//
static std::string find_icon(const fs::path& dir) {
    static const char* const candidates[] = {
        "ICON0.PNG", "icon0.png",
        "sce_sys/icon0.png", "sce_sys/ICON0.PNG",
    };
    std::error_code ec;

    for (const char* name : candidates) {
        const fs::path p = dir / name;
        if (fs::is_regular_file(p, ec)) return p.string();
    }
    return std::string();
}

// Which of those files the console itself says is the save -- PARAM.SFO's
// SAVEDATA_FILE_LIST on a PSP, PARAM.PFD's entry table on a PS3. Both list
// exactly the files that are encrypted, so this is also the answer to "which
// one will come out as garbage if you patch it as-is".
//
// PS4 and Vita saves have no such list, and the largest file stands in: their
// saves are one big file plus, occasionally, a small index beside it.
static int suggest_file(SaveEntry& save, const std::vector<unsigned char>& sfo,
                        const std::vector<unsigned char>& pfd) {
    char name[APFD_NAME_LEN];

    save.suggest_listed = false;

    if (strcmp(save.platform, "PSP") == 0 && !sfo.empty()) {
        const int n = apsp_sfo_file_count(sfo.data(), sfo.size());
        for (int i = 0; i < n; i++) {
            if (apsp_sfo_file_name(sfo.data(), sfo.size(), i, name, sizeof name) != APSP_OK)
                continue;
            for (size_t f = 0; f < save.files.size(); f++)
                if (save.files[f] == name) { save.suggest_listed = true; return int(f); }
        }
    } else if (!pfd.empty() && apfd_valid(pfd.data(), pfd.size()) == APFD_OK) {
        const int n = apfd_entry_count(pfd.data(), pfd.size());
        for (int i = 0; i < n; i++) {
            if (apfd_entry_name(pfd.data(), pfd.size(), i, name, sizeof name) != APFD_OK)
                continue;
            if (apfd_entry_has_builtin_key(name)) continue;   // PARAM.SFO, trophies
            for (size_t f = 0; f < save.files.size(); f++)
                if (save.files[f] == name) { save.suggest_listed = true; return int(f); }
        }
    }

    // No list, or a list naming nothing that is actually there.
    std::error_code ec;
    int    best = save.files.empty() ? -1 : 0;
    uintmax_t biggest = 0;
    for (size_t f = 0; f < save.files.size(); f++) {
        const uintmax_t n = fs::file_size(fs::path(save.path) / save.files[f], ec);
        if (!ec && n > biggest) { biggest = n; best = int(f); }
    }
    return best;
}

//
// How big is one of a save's files?
//
// For every console but two that is a question for the filesystem. A PS1 or
// PS2 save has no files on disk at all -- they live inside a .PSV, and the
// container is the only thing that knows -- so the sizes are read once during
// the scan and answered from there.
//
static uint64_t entry_file_size(const SaveEntry& save, size_t index) {
    if (index >= save.files.size()) return 0;
    if (save.container)
        return index < save.file_sizes.size() ? save.file_sizes[index] : 0;

    std::error_code ec;
    const uintmax_t n = fs::file_size(fs::path(save.path) / save.files[index], ec);
    return ec ? 0 : uint64_t(n);
}

//
// Which file inside a container is the one somebody wants to patch?
//
// A memory-card save carries its own presentation with it -- icon.sys names
// the save, and one to three .ico files are the 3D icon the console spun on
// its dashboard -- and those are never what a code addresses. Every one of
// the 2,641 real PS2 containers has an icon.sys, and most have icons besides.
//
// So the icons are set aside and the largest of what is left is offered,
// which is the same rule used for a PS4 or Vita save. A PS1 container holds
// exactly one file, its whole memory-card block, so this always answers that.
//
static bool psvcard_is_presentation(const std::string& name) {
    const std::string n = lowered(name);
    if (n == "icon.sys") return true;
    for (const char* ext : { ".ico", ".icn" }) {
        const size_t k = strlen(ext);
        if (n.size() > k && n.compare(n.size() - k, k, ext) == 0) return true;
    }
    return false;
}

static int psvcard_suggest_file(const SaveEntry& save) {
    int best = -1;
    uint64_t biggest = 0;

    for (size_t i = 0; i < save.files.size(); i++) {
        if (psvcard_is_presentation(save.files[i])) continue;
        const uint64_t n = i < save.file_sizes.size() ? save.file_sizes[i] : 0;
        if (best < 0 || n > biggest) { biggest = n; best = int(i); }
    }
    // Nothing but presentation. Unusual, but a container is somebody's file
    // and it may hold whatever it holds -- offer the first thing rather than
    // nothing, so the save can still be opened and looked at.
    if (best < 0 && !save.files.empty()) best = 0;
    return best;
}

//
// Is this .PSV a save, and if so what is in it?
//
// The PS1 and PS2 kept saves in memory-card blocks, so unlike every other
// console here there is no folder to examine and no PARAM.SFO to read. What
// there is instead is a container the PS3 wrote, and it carries everything:
// the save's own memory-card directory, the files inside it, and the name the
// console's save list showed.
//
static bool examine_psvcard(const fs::path& file, SaveEntry& out) {
    std::vector<unsigned char> buf;
    apsvc_info_t info;

    if (!read_all(file.string(), buf)) return false;
    if (apsvc_info(buf.data(), buf.size(), &info) != APSVC_OK) return false;

    char title[ASAVE_NAME_LEN] = "";
    apsvc_title(buf.data(), buf.size(), title, sizeof title);

    asave_info_t id;
    if (asave_identify_psvcard(info.dir_name, info.type, title, &id) != ASAVE_OK)
        return false;

    out.path        = file.string();
    out.dir_name    = info.dir_name;
    out.name        = id.name;          // empty: the catalogue fills it in
    out.detail      = id.detail;
    out.title_id    = id.title_id;
    out.platform    = asave_platform_name(id.platform);
    out.platform_id = id.platform;
    out.encrypted   = false;
    out.container   = true;

    std::vector<apsvc_file_t> inner(size_t(info.file_count));
    if (info.file_count > 0 &&
        apsvc_files(buf.data(), buf.size(), inner.data(), info.file_count, nullptr) != APSVC_OK)
        return false;

    // Left in the container's own order rather than sorted: the order IS the
    // layout, and a patch that names a slot by index means this one. It is
    // also what apsvc_replace_at() counts in.
    for (const apsvc_file_t& f : inner) {
        out.files.push_back(f.name);
        out.file_sizes.push_back(f.size);
    }

    out.suggest = psvcard_suggest_file(out);
    return true;
}

// Is this folder a save, and if so what is in it? Returns false for every
// other folder on the card, which is most of them.
static bool examine(const fs::path& dir, SaveEntry& out) {
    std::vector<unsigned char> sfo, pfd;
    asave_info_t info;
    asave_where_t where;

    if (read_meta(dir / "PARAM.SFO", sfo)) {
        where = ASAVE_AT_ROOT;
    } else if (read_meta(dir / "sce_sys" / "param.sfo", sfo)) {
        where = ASAVE_AT_SCE;
    } else {
        return false;
    }

    read_meta(dir / "PARAM.PFD", pfd);
    if (asave_identify(sfo.data(), sfo.size(), where, pfd.empty() ? 0 : 1, &info) != ASAVE_OK)
        return false;

    out.path      = dir.string();
    out.dir_name  = dir.filename().string();
    out.name      = info.name;
    out.detail    = info.detail;
    out.title_id  = info.title_id;
    out.platform    = asave_platform_name(info.platform);
    out.platform_id = info.platform;
    out.encrypted = info.encrypted != 0;

    collect_files(dir, dir, 2, out.files);
    std::sort(out.files.begin(), out.files.end());
    out.suggest = suggest_file(out, sfo, pfd);
    out.icon    = find_icon(dir);

    // Which PSN account the save is signed to. The same 64-bit number on
    // every console that has the concept, written two different ways:
    //
    //   PS3        ACCOUNT_ID as SIXTEEN bytes of ASCII hex, and again inside
    //              the PARAMS blob -- apfd_sfo_account_id() reads both.
    //   PS4, Vita  ACCOUNT_ID as EIGHT raw bytes, little-endian.
    //
    // Only the length tells the two apart, so each reader checks it and
    // refuses the other's file rather than reading a number that is not there.
    //
    // PSP has no account in its PARAM.SFO, and PS1 and PS2 have no such
    // concept at all, so those are simply never asked.
    if (info.platform == ASAVE_PS3) {
        char id[APFD_ACCT_ID_LEN + 1] = "";
        if (apfd_sfo_account_id(sfo.data(), sfo.size(), id, sizeof id) == APFD_OK)
            out.account = id;
    } else if (info.platform == ASAVE_PS4 || info.platform == ASAVE_PSVITA) {
        uint64_t id = 0;
        // Zero is left as "no account". It is what a decrypted or shared save
        // usually carries -- 48 of the 672 in apollo-saves -- and calling that
        // an owner would mark every one of them as somebody else's.
        if (asfo_account_id(sfo.data(), sfo.size(), &id) == ASFO_OK && id) {
            char hex[17];
            snprintf(hex, sizeof hex, "%016llx", (unsigned long long)id);
            out.account = hex;
        }
    }
    return true;
}

// The walk itself. A folder that IS a save is not descended into: a PS4
// save's sce_sys would otherwise be examined as a candidate of its own, and
// nothing below a save is a save.
static void scan_walk(const fs::path& dir, int depth, std::vector<SaveEntry>& out) {
    std::error_code ec;

    if (g_sb.cancel.load() || g_sb.seen.load() >= SCAN_MAX_DIRS) return;
    g_sb.seen.fetch_add(1);

    SaveEntry here;
    if (examine(dir, here)) {
        out.push_back(std::move(here));
        return;
    }
    if (depth <= 0) {
        // Counted, because a save below this point would otherwise be missing
        // from the list with nothing said about it -- which looks exactly like
        // "you have no saves" and is the one failure somebody cannot debug.
        std::error_code dc;
        for (const auto& e : fs::directory_iterator(dir, dc))
            if (e.is_directory(dc)) { g_sb.too_deep.fetch_add(1); break; }
        return;
    }

    for (const auto& e : fs::directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec)) {
        if (g_sb.cancel.load()) return;
        // Symlinks are not followed: a link pointing back up its own tree
        // would walk forever, and the depth cap only bounds how long.
        if (e.is_symlink(ec)) continue;
        const std::string leaf = e.path().filename().string();
        if (leaf.empty() || leaf[0] == '.') continue;

        if (e.is_directory(ec)) {
            scan_walk(e.path(), depth - 1, out);
            continue;
        }

        // A PS1 or PS2 save is a FILE, not a folder -- the only one of the six
        // consoles for which that is true. Checked by extension first so the
        // walk does not read every file on the card to find out.
        if (e.is_regular_file(ec) && ends_with_ci(leaf, ".psv")) {
            SaveEntry found;
            if (examine_psvcard(e.path(), found))
                out.push_back(std::move(found));
        }
    }
}

// Everything that needs the patch database, done on the UI thread after the
// walk: the database is read-only but the entries it hands back are the
// caller's to hold, and keeping it on one thread costs nothing here.
static void resolve_patches(std::vector<SaveEntry>& saves) {
    for (SaveEntry& s : saves) {
        if (!s.title_id.empty() && g_db.db) {
            auto it = g_db.by_title_id.find(lowered(s.title_id));
            if (it != g_db.by_title_id.end()) {
                const patchdb_entry_t* e = patchdb_at(g_db.db, it->second);
                // The database's platform has to agree with the save's own.
                // It always does -- a title ID belongs to one console -- and
                // when it does not, something has been misidentified and
                // loading that game's codes would be worse than offering none.
                if (e && strcmp(e->platform, s.platform) == 0) {
                    s.patch_index = it->second;
                    s.patch_name  = e->name;
                }
            }
        }
        //
        // A name for a save that carries none -- which is every Vita save
        // (see saveinfo.h), and the occasional one elsewhere.
        //
        // In this order, and the order is the point:
        //
        //   the save's own PARAM.SFO   what the console itself shows. Already
        //                              in s.name by now when there is one.
        //   the title catalogue        8783 games, keyed exactly by title ID
        //   the patch database         the patch author's own wording, which
        //                              carries region suffixes and varies
        //
        if (s.name.empty() && !g_db.titles.empty() && !s.title_id.empty()) {
            char named[ASAVE_NAME_LEN] = "";
            if (asave_name_from_db(g_db.titles.data(), g_db.titles.size(),
                                   s.platform, s.title_id.c_str(),
                                   named, sizeof named) == ASAVE_OK)
                s.name = named;
        }
        if (s.name.empty()) s.name = s.patch_name;

        s.haystack = lowered(s.name + " " + s.patch_name + " " + s.title_id
                             + " " + s.dir_name + " " + s.detail);
    }

    std::sort(saves.begin(), saves.end(), [](const SaveEntry& a, const SaveEntry& b) {
        const int p = strcmp(a.platform, b.platform);
        if (p != 0) return p < 0;
        if (a.name != b.name) return lowered(a.name) < lowered(b.name);
        return a.dir_name < b.dir_name;
    });
}

static void scan_join() {
    if (g_sb.worker.joinable()) g_sb.worker.join();
    g_sb.running = false;
    g_sb.done    = false;
}

static void scan_start(const std::string& root) {
    // Cancel BEFORE joining: choosing a second folder while the first is still
    // being walked would otherwise freeze the window until that walk finished
    // on its own, which is exactly what running it on a thread was for.
    g_sb.cancel = true;
    scan_join();

    g_sb.root   = root;
    g_sb.cancel   = false;
    g_sb.seen     = 0;
    g_sb.too_deep = 0;
    g_sb.out.clear();
    g_sb.out_note.clear();
    g_sb.saves.clear();
    g_sb.hits.clear();
    g_sb.selected = -1;
    icon_drop();               // it belongs to a save that is about to vanish
    g_sb.note     = "Scanning...";
    g_sb.running  = true;
    g_sb.done     = false;

    g_sb.worker = std::thread([root] {
        try {
            scan_walk(fs::path(root), SCAN_MAX_DEPTH, g_sb.out);
        } catch (const std::exception& e) {
            // A filesystem this cannot walk at all -- a disconnected volume,
            // a path the OS refuses. Reported rather than thrown away, and
            // whatever was found before it stays in the list.
            g_sb.out_note = std::string("The scan stopped early: ") + e.what();
        }
        g_sb.done = true;
    });
}

// Take the thread's result. Called once per frame from the UI, and does
// nothing until the walk has finished.
static void scan_collect() {
    if (!g_sb.running.load() || !g_sb.done.load()) return;

    scan_join();
    g_sb.saves = std::move(g_sb.out);
    g_sb.out.clear();
    resolve_patches(g_sb.saves);
    g_sb.refilter = true;
    g_sb.selected = g_sb.saves.empty() ? -1 : 0;

    // Which consoles turned up. A fixed order, so the buttons do not jump
    // about between scans of different folders.
    g_sb.platforms.clear();
    g_sb.platforms.push_back("All");
    for (const char* p : { "PS1", "PS2", "PS3", "PS4", "PSP", "PSV" })
        for (const SaveEntry& s : g_sb.saves)
            if (strcmp(s.platform, p) == 0) { g_sb.platforms.push_back(p); break; }
    if (g_sb.platform >= int(g_sb.platforms.size())) g_sb.platform = 0;

    char buf[512];
    int coded = 0;
    for (const SaveEntry& s : g_sb.saves) if (s.patch_index >= 0) coded++;
    snprintf(buf, sizeof buf, "%d save%s found in %d folder%s; %d ha%s codes.",
             int(g_sb.saves.size()), g_sb.saves.size() == 1 ? "" : "s",
             g_sb.seen.load(), g_sb.seen.load() == 1 ? "" : "s",
             coded, coded == 1 ? "s" : "ve");
    g_sb.note = buf;
    if (!g_sb.out_note.empty()) g_sb.note += "  " + g_sb.out_note;
    const int deep = g_sb.too_deep.load();
    if (deep > 0) {
        snprintf(buf, sizeof buf,
                 "  %d folder%s sit%s deeper than the scan goes (%d levels) - "
                 "choose a folder closer to the saves.",
                 deep, deep == 1 ? "" : "s", deep == 1 ? "s" : "", SCAN_MAX_DEPTH);
        g_sb.note += buf;
    }
    if (g_sb.seen.load() >= SCAN_MAX_DIRS)
        g_sb.note += "  Stopped after " + std::to_string(SCAN_MAX_DIRS)
                   + " folders - choose a folder closer to the saves.";
    if (g_sb.cancel.load()) g_sb.note += "  Stopped early.";
    g_app.append_log(("Save scan: " + g_sb.note).c_str());
}

// What a platform tag is called on screen. The tags are what the scan writes
// and what the filter compares against, so they stay "PSV"; only the caption
// is spelled out, because the abbreviation is the one of the six nobody reads
// at a glance.
static const char* platform_label(const char* tag) {
    return strcmp(tag, "PSV") == 0 ? "PS Vita" : tag;
}

static void refilter_saves() {
    g_sb.hits.clear();
    const std::string needle = lowered(g_sb.search);
    const char* platform = (g_sb.platform > 0
                            && size_t(g_sb.platform) < g_sb.platforms.size())
                         ? g_sb.platforms[size_t(g_sb.platform)].c_str() : nullptr;

    for (size_t i = 0; i < g_sb.saves.size(); ++i) {
        const SaveEntry& s = g_sb.saves[i];
        if (platform && strcmp(s.platform, platform) != 0) continue;
        if (g_sb.only_coded && s.patch_index < 0) continue;
        if (!needle.empty() && s.haystack.find(needle) == std::string::npos) continue;
        g_sb.hits.push_back(int(i));
    }
    g_sb.refilter = false;
}

//
// The one decoded icon on screen.
//
// One, not a cache of them: only ever one save is on screen at a time, and a
// folder of 176 saves would otherwise be 176 PNGs decoded and 40MB of texture
// uploaded for a list that displays them one at a time.
// Re-decoding on every selection change costs a fraction of a millisecond for
// an image this size.
//
struct SaveIcon {
    std::string path;        // what is loaded, "" for nothing
    unsigned    tex = 0;     // the GL texture name
    int         w = 0, h = 0;      // the image
    float       u = 1.0f, v = 1.0f;  // ...as a fraction of the texture
    std::string error;       // why there is no picture, when there is a file

    //
    // A PS2 icon is a MODEL, so unlike every other console's it can move. The
    // parse is kept alive for exactly that: each frame asks where in the loop
    // it is and re-renders that pose.
    //
    // Only where there is something to move between -- 1,898 of the 2,345
    // icons in the save database carry a single shape and are simply drawn
    // once, which `animated` records so the common case costs nothing.
    //
    bool          model_valid = false;
    ps2icon_t     model{};
    ps2_IconSys_t sys{};
    bool          have_sys = false;
    bool          animated = false;
    // The icon would not parse at all: worth saying, because an icon that
    // claims more than its file holds is evidence about the SAVE, not just
    // about the icon.
    bool          corrupt = false;
    // ...or it parsed only far enough for its texture, which is shown flat.
    bool          flat_texture = false;
    //
    // The frame the hover panel was last drawn on. While it is up, only the
    // big copy animates.
    //
    // A frame NUMBER rather than a flag, because the panel is only drawn on
    // the patcher screen: a flag set there would still be set after switching
    // to the saves list, and the icon in the list's own tooltip would sit
    // frozen for as long as the app stayed open. A number goes stale on its
    // own the moment the panel stops being drawn.
    //
    int           hover_frame = -1;
    float         loop = 0.0f;   // seconds
    double        began = 0.0;   // ImGui's clock when it started

    // The bigger render, made only while the pointer is over the small one.
    unsigned      big_tex = 0;
    int           big_px = 0;
};
static SaveIcon g_icon;

// How big the two renders are, and how hard each is antialiased.
//
// The small one sits in a list row and a header; the large one is a hover
// panel, where the model is finally worth looking at.
//
// Both end up rasterising the same 512x512 internally -- 128 at 4x, 256 at 2x
// -- which is deliberate. The renderer is fill-rate bound, so that number is
// what the work costs, and matching them makes the hover panel cost what the
// small icon already does. 256 at 4x would be 1024x1024: three and a half
// times the work for a difference visible only side by side, and on an Athlon
// XP 2400+ that is the difference between an animation that plays and one
// that stutters.
//
// Both are POWERS OF TWO on purpose. OpenGL 1.1 -- the floor this app targets,
// and what Microsoft's software renderer offers over Remote Desktop -- accepts
// no other texture size, and the failure is a silently invisible icon on
// exactly the machines least able to say why. The PNG path pads to a power of
// two for the same reason; this one simply renders at one.
#define PSV_ICON_BIG_PX  256
#define PSV_ICON_BIG_SS    2

static void icon_drop() {
    if (g_icon.tex)     glDeleteTextures(1, (const GLuint*)&g_icon.tex);
    if (g_icon.big_tex) glDeleteTextures(1, (const GLuint*)&g_icon.big_tex);
    if (g_icon.model_valid) ps2icon_free(&g_icon.model);
    g_icon = SaveIcon();
}

// Upload RGBA as the one icon on screen. Split out from icon_load() because a
// PS1 or PS2 icon never was a file: it is rendered from the save itself, and
// arrives here as pixels rather than as a PNG to decode.
static void icon_upload(const uint8_t* rgba, int w, int h) {
    // The one on screen replaces the one before it. This used to run once per
    // icon, straight after icon_drop(), so there was never anything to free;
    // an ANIMATED icon calls it sixty times a second, and without this each
    // frame would leave its texture behind -- 64KB a frame, which is 3.8MB a
    // second for as long as the save is on screen.
    if (g_icon.tex) {
        glDeleteTextures(1, (const GLuint*)&g_icon.tex);
        g_icon.tex = 0;
    }

    //
    // Padded to a power of two, and drawn with UVs that cut the padding back
    // off.
    //
    // OpenGL 1.1 -- which is the floor this app targets, and exactly what
    // Microsoft's software renderer offers on a GPU-less or Remote Desktop
    // host -- takes only power-of-two textures. No save icon is one: they are
    // 320x176, 228x128, 144x80. Without this the icon silently fails to
    // appear on precisely the machines least able to say why.
    //
    int pw = 1, ph = 1;
    while (pw < w) pw <<= 1;
    while (ph < h) ph <<= 1;

    std::vector<unsigned char> pot((size_t)pw * ph * 4, 0);
    for (int y = 0; y < h; y++)
        memcpy(&pot[(size_t)y * pw * 4], rgba + (size_t)y * w * 4, (size_t)w * 4);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // GL_CLAMP, not GL_CLAMP_TO_EDGE: the latter is 1.2, and the whole point
    // of the padding above is that 1.1 is the floor.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pw, ph, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pot.data());

    g_icon.tex = tex;
    g_icon.w   = w;
    g_icon.h   = h;
    g_icon.u   = float(w) / float(pw);
    g_icon.v   = float(h) / float(ph);
}

static void icon_load(const std::string& path) {
    if (path == g_icon.path) return;   // already the one on screen
    icon_drop();
    g_icon.path = path;
    if (path.empty()) return;

    std::vector<unsigned char> file;
    if (!read_all(path, file)) { g_icon.error = "could not be read"; return; }

    uint8_t* rgba = nullptr;
    int      w = 0, h = 0;
    const int rc = apng_decode(file.data(), file.size(), &rgba, &w, &h);
    if (rc != APNG_OK) { g_icon.error = apng_strerror(rc); return; }

    icon_upload(rgba, w, h);
    apng_free(rgba);
}

//
// The icon a PS1 or PS2 save carries, which is not a picture until something
// makes one: a PS1 save's is sixteen colours packed into its own first block,
// and a PS2 save's is a textured 3D model that has to be rendered.
//
// Keyed on the container's path so the "already on screen" check still works
// -- selecting the same save twice must not re-render it.
//
// Either way of getting a save's picture, chosen by the save. Every call site
// wants this rather than one of the two below it.
static void icon_load_for(const SaveEntry& save);

//
// What a container's icon turns out to be, in words. For the headless report
// and for anything else that wants the answer without a GL context.
//
static const char* describe_container_icon(const SaveEntry& save) {
    std::vector<unsigned char> buf;
    apsvc_info_t info;

    if (!read_all(save.path, buf)) return "the container could not be read";
    if (apsvc_info(buf.data(), buf.size(), &info) != APSVC_OK)
        return "the container could not be read";

    if (info.type == APSVC_TYPE_PS1) {
        apsvc_file_t f;
        if (apsvc_files(buf.data(), buf.size(), &f, 1, nullptr) != APSVC_OK)
            return "none";
        return amci_ps1_frames(buf.data() + f.off, f.size) > 0
             ? "rendered from the save" : "none";
    }

    std::vector<apsvc_file_t> all(size_t(info.file_count));
    if (info.file_count <= 0 ||
        apsvc_files(buf.data(), buf.size(), all.data(), info.file_count, nullptr) != APSVC_OK)
        return "none";

    const char* worst = "none";
    for (const apsvc_file_t& f : all) {
        if (!ends_with_ci(f.name, ".ico") && !ends_with_ci(f.name, ".icn")) continue;

        uint8_t* rgba = nullptr;
        amci_ps2_kind_t kind = AMCI_PS2_MODEL;
        const int rc = amci_ps2_render_kind(buf.data() + f.off, f.size,
                                            info.sys_off ? buf.data() + info.sys_off : nullptr,
                                            info.sys_size, 32, &rgba, &kind);
        if (rc == AMCI_OK) {
            amci_free(rgba);
            // The first readable icon is the one shown, so it settles it.
            return kind == AMCI_PS2_FLAT
                 ? "texture only - the 3D model would not read"
                 : "rendered from the save";
        }
        // Remembered, but keep looking: icon.sys names up to three and a
        // later one may be intact.
        if (rc == AMCI_ERR_CORRUPT) worst = "DAMAGED - the icon claims more data than the file holds";
    }
    return worst;
}

//
// Draw the model at wherever it is in its loop, into `tex`.
//
// The whole render happens on the CPU (core/ps2/ps2render.c) and the result is
// uploaded as an ordinary texture, so this needs no depth buffer, no shaders
// and no second GL context -- which matters, because the app's floor is
// OpenGL 1.1 and one of the platforms it has to work on is a software
// rasteriser over Remote Desktop.
//
// Affordable at this size: 128px at 4x supersampling measures 1.4ms on this
// machine, against a 16ms frame. The hover panel is bigger and rendered only
// while the pointer is on the icon.
//
static void icon_render_pose(int px, int ss, unsigned* tex) {
    if (!g_icon.model_valid) return;

    // One clock drives both, exactly as it does in the reference renderer:
    // the morph walks the shape list, the yaw turns the model on the spot.
    const float t = float(ImGui::GetTime() - g_icon.began);

    int a = g_icon.model.still_shape, b = a;
    float morph = 0.0f;
    if (g_icon.animated)
        ps2icon_morph_at(&g_icon.model, t, &a, &b, &morph);

    uint8_t* rgba = nullptr;
    if (ps2icon_render_at(&g_icon.model, g_icon.have_sys ? &g_icon.sys : nullptr,
                          px, ss, PS2RENDER_BG_TRANSPARENT, a, b, morph,
                          ps2icon_yaw_at(t), &rgba) != 0
        || !rgba)
        return;

    // icon_upload() owns the ONE icon on screen; the hover panel's texture is
    // a second one, so it is uploaded here rather than through that.
    if (tex == &g_icon.tex) {
        icon_upload(rgba, px, px);
    } else {
        if (*tex) glDeleteTextures(1, (const GLuint*)tex);
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // A power of two already: both sizes are chosen that way, because
        // OpenGL 1.1 takes nothing else.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, px, px, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        *tex = t;
    }
    free(rgba);
}

static void icon_load_container(const SaveEntry& save) {
    if (save.path == g_icon.path) return;
    icon_drop();
    g_icon.path = save.path;

    std::vector<unsigned char> buf;
    if (!read_all(save.path, buf)) { g_icon.error = "could not be read"; return; }

    apsvc_info_t info;
    if (apsvc_info(buf.data(), buf.size(), &info) != APSVC_OK) {
        g_icon.error = "the container could not be read";
        return;
    }

    if (info.type == APSVC_TYPE_PS1) {
        apsvc_file_t f;
        if (apsvc_files(buf.data(), buf.size(), &f, 1, nullptr) != APSVC_OK) {
            g_icon.error = "the container lists no save";
            return;
        }
        // Frame zero. A PS1 icon can carry two or three frames, but not one
        // of the six in the save database does -- so the animation that would
        // go here has no save to play it on, and a still is what these are.
        std::vector<uint8_t> rgba(AMCI_PS1_SIZE * AMCI_PS1_SIZE * 4);
        const int rc = amci_ps1_frame(buf.data() + f.off, f.size, 0, rgba.data());
        if (rc != AMCI_OK) { g_icon.error = amci_strerror(rc); return; }
        icon_upload(rgba.data(), AMCI_PS1_SIZE, AMCI_PS1_SIZE);
        return;
    }

    // PS2: the first .ico in the container. icon.sys names three -- the
    // normal, copying and deleting icons -- and the first is the one the save
    // list showed.
    std::vector<apsvc_file_t> all(size_t(info.file_count));
    if (info.file_count <= 0 ||
        apsvc_files(buf.data(), buf.size(), all.data(), info.file_count, nullptr) != APSVC_OK) {
        g_icon.error = "the container lists no files";
        return;
    }

    if (info.sys_off && info.sys_size >= sizeof g_icon.sys) {
        // Copied out of the container, which does not align it for this
        // structure and is about to go out of scope besides.
        memcpy(&g_icon.sys, buf.data() + info.sys_off, sizeof g_icon.sys);
        g_icon.have_sys = true;
    }

    for (const apsvc_file_t& f : all) {
        if (!ends_with_ci(f.name, ".ico") && !ends_with_ci(f.name, ".icn")) continue;

        ps2icon_t model{};
        const int rc = ps2icon_parse(buf.data() + f.off, f.size, &model);

        if (rc < 0) {
            //
            // Damaged: the file names more geometry than it holds, and no
            // whole texture could be had either. Said out loud rather than
            // shrugged off as an unsupported format.
            //
            // Drawing the part that parsed was tried and is WRONG. The one
            // such icon in the save database has two thirds of its model
            // present and renders into a clean silhouette -- a picture that
            // states, convincingly, that nothing is the matter. Whatever
            // truncated the icon was under no obligation to stop there, and
            // the person looking at the save should hear about it.
            //
            ps2icon_free(&model);
            g_icon.error = "damaged - it claims more data than the file holds";
            g_icon.corrupt = true;
            continue;   // icon.sys names up to three; another may be intact
        }

        if (rc == 1) {
            // No usable geometry, but the whole texture survived. Shown flat,
            // because there is no model to wrap it around.
            std::vector<uint8_t> flat(size_t(PSV_ICON_PX) * PSV_ICON_PX * 4);
            uint8_t* rgba = nullptr;
            if (amci_ps2_render_kind(buf.data() + f.off, f.size, nullptr, 0,
                                     PSV_ICON_PX, &rgba, nullptr) == AMCI_OK && rgba) {
                icon_upload(rgba, PSV_ICON_PX, PSV_ICON_PX);
                amci_free(rgba);
                g_icon.flat_texture = true;
                g_icon.error.clear();
                ps2icon_free(&model);
                return;
            }
            ps2icon_free(&model);
            continue;
        }

        // Kept rather than rendered and thrown away: an animated icon is
        // re-posed every frame from this, and the hover panel renders the same
        // model larger.
        g_icon.model       = model;
        g_icon.model_valid = true;
        g_icon.loop        = ps2icon_loop_seconds(&g_icon.model);
        g_icon.animated    = g_icon.loop > 0.0f;
        g_icon.began       = ImGui::GetTime();
        g_icon.error.clear();

        icon_render_pose(PSV_ICON_PX, PSV_ICON_SS, &g_icon.tex);
        return;
    }
    if (g_icon.error.empty()) g_icon.error = "the save carries no icon";
}

static void icon_load_for(const SaveEntry& save) {
    if (save.container) icon_load_container(save);
    else                icon_load(save.icon);

    //
    // A model is re-posed every frame, whether or not it morphs: EVERY icon
    // turns on its axis, and the ones with a single shape are exactly the
    // ones a still frame misrepresents. A PNG cannot move and costs nothing.
    //
    // ...but NOT while the hover panel is up. Both copies are the same model
    // and drawing both means rasterising it twice a frame, which is real work
    // on a slow machine -- an Athlon XP 2400+ shows it. The big one is what
    // the pointer is pointed at, so it keeps moving and the small one holds
    // its pose until the pointer leaves. That cap is why this stayed
    // affordable when the turn made every icon a moving one.
    //
    // Keyed on LAST frame's hover, because the panel is drawn after this
    // runs. One frame of lag at this rate is not something an eye can find.
    //
    if (g_icon.hover_frame < ImGui::GetFrameCount() - 1)
        icon_render_pose(PSV_ICON_PX, PSV_ICON_SS, &g_icon.tex);
}

//
// The icon, bigger, while the pointer is on it.
//
// A PS2 save's icon is a 3D model the console spun on its dashboard, and at
// the size a header shows it that is mostly wasted. Hovering renders the same
// model at PSV_ICON_BIG_PX, still animating -- which is the closest this gets
// to what the save actually looked like on a television.
//
// Only for a model: a PNG has no more detail to give, so enlarging one would
// just be a blurrier copy of what is already on screen.
//
static void icon_hover_panel() {
    if (!g_icon.model_valid || !ImGui::IsItemHovered())
        return;
    g_icon.hover_frame = ImGui::GetFrameCount();

    // Rendered while hovered and dropped when the pointer leaves, so the cost
    // is paid only while somebody is looking -- and re-posed every frame,
    // since it is turning even when it has no shapes to morph between.
    icon_render_pose(PSV_ICON_BIG_PX, PSV_ICON_BIG_SS, &g_icon.big_tex);
    if (!g_icon.big_tex) return;

    ImGui::BeginTooltip();
    const float side = float(PSV_ICON_BIG_PX) * g_ui_scale;
    ImGui::Image((ImTextureID)(intptr_t)g_icon.big_tex, ImVec2(side, side));
    // About the MORPH, not the turn: every icon turns, so saying so of each
    // one would be noise. What differs between icons is whether they also
    // change shape.
    if (g_icon.animated)
        ImGui::TextDisabled("%d shapes, %.1fs loop",
                            g_icon.model.shape_count, g_icon.loop);
    else
        ImGui::TextDisabled("one shape - this one turns but does not animate");
    ImGui::EndTooltip();
}

// Draw it at up to `box_w` x `box_h`, keeping its shape. A PS3 icon is
// 320x176 and a Vita one is square, so a fixed width would make the Vita
// tower over the rest of the pane.
static void icon_draw(float box_w, float box_h) {
    if (!g_icon.tex) return;

    const float scale = std::min(box_w / float(g_icon.w), box_h / float(g_icon.h));
    ImGui::Image((ImTextureID)(intptr_t)g_icon.tex,
                 ImVec2(float(g_icon.w) * scale, float(g_icon.h) * scale),
                 ImVec2(0, 0), ImVec2(g_icon.u, g_icon.v));
}

//
// Open one file out of one save: make it the target, and load the game's
// codes if the database has them.
//
// The title ID goes along with it, because for PS4 and Vita the folder does
// not carry one -- see detect_patch_for_target().
//
//
// Does the patch now open name this file? A `.savepatch` carries target-file
// lines (":OPTIONS.DAT"), and they are the last word on which file it
// addresses -- more authoritative than anything guessed from the save.
//
// Returns the name it does want, or "" when it names the file already open, or
// names nothing, or names several. Only a single unambiguous answer is worth
// acting on.
//
static std::string patch_wants_other_file(const SaveEntry& save, const std::string& open_now) {
    if (!g_app.session) return "";

    std::vector<std::string> named;
    const int n = apctl_code_count(g_app.session);
    for (int i = 0; i < n; i++) {
        const apctl_code_t* c = apctl_code_at(g_app.session, i);
        if (!c || !c->file || !c->file[0]) continue;
        const std::string f = lowered(c->file);
        if (f == lowered(open_now)) return "";   // it means this one
        if (std::find(named.begin(), named.end(), f) == named.end()) named.push_back(f);
    }
    if (named.size() != 1) return "";

    // ...and it has to actually be in the save. A patch naming a file that is
    // not here says the save is a different slot, not that it is the wrong
    // file, and re-pointing at something absent would only break what works.
    for (const std::string& f : save.files)
        if (lowered(f) == named[0]) return f;
    return "";
}

//
// The on-disk path for one of a save's files, opening the container first when
// there is one. Empty when the file could not be produced, which is the only
// way this fails -- an ordinary save's file is simply the path joined.
//
static std::string open_file_path(const SaveEntry& save, int file_index,
                                  AppState::PsvCard& st) {
    st.clear();
    if (file_index < 0 || file_index >= int(save.files.size()))
        return std::string();

    if (!save.container)
        return (fs::path(save.path) / save.files[size_t(file_index)]).string();

    std::string extracted;
    if (!psvcard_extract(save, file_index, extracted, st))
        return std::string();
    return extracted;
}

// The two steps that always go together: point the app at one of a save's
// files, and remember the container it came out of if it came out of one.
static bool adopt_save_file(const SaveEntry& save, int file_index) {
    AppState::PsvCard st;
    const std::string path = open_file_path(save, file_index, st);
    if (path.empty()) return false;

    adopt_target(path, save.title_id);   // clears g_app.psvcard
    g_app.psvcard = st;                      // ...and this puts back what applies
    return true;
}

static void open_save_file(const SaveEntry& save, int file_index) {
    if (file_index < 0 || file_index >= int(save.files.size())) return;

    const std::string name = save.files[size_t(file_index)];

    if (!adopt_save_file(save, file_index)) {
        g_app.show_log = true;
        return;
    }

    // adopt_target loads the matching patch only when nothing is open, so that
    // choosing a target never discards somebody's edited codes behind their
    // back. Here the GAME is what was chosen, out of a list of games, so a
    // patch already open is the stale one and swapping it is the whole point.
    if (g_app.match_index >= 0 && g_app.patch_path != g_app.match_label)
        load_patch_from_db(g_app.match_index);

    // Now that the patch is open it can be asked. This matters where the
    // console's own list and the patch disagree: Assassin's Creed encrypts
    // ICON1.PAM as well as its save, so PARAM.PFD's first entry is the
    // ANIMATED ICON, while the patch says ":OPTIONS.DAT". Only ever taken
    // when the file opened was the suggestion rather than one picked by hand.
    if (file_index != save.suggest) return;

    const std::string want = patch_wants_other_file(save, name);
    if (want.empty() || want == name) return;

    g_app.append_log(("The patch targets " + want + " - opening that instead of "
                      + name).c_str());

    // By index rather than by name: four real containers hold two files called
    // settings.dat, and writing back to the wrong one would corrupt the save.
    int want_index = -1;
    for (size_t i = 0; i < save.files.size(); i++)
        if (save.files[i] == want) { want_index = int(i); break; }
    if (want_index < 0) return;

    adopt_save_file(save, want_index);
}

static void load_patch_from_db(int index) {
    const patchdb_entry_t* e = patchdb_at(g_db.db, index);
    if (!e) return;

    char*  data = nullptr;
    size_t len  = 0;
    if (!patchdb_read(g_db.db, index, &data, &len)) {
        g_app.append_log("[!] Could not read that patch out of the database");
        return;
    }

    g_app.close();
    std::string label = std::string(e->platform) + "/" + e->title_id + ".savepatch";
    apctl_session_t* s = apctl_open_buffer(data, len, label.c_str());
    g_app.patch_bytes.assign(data, len);
    g_app.patch_raw = strip_cr(g_app.patch_bytes);
    g_app.patch_raw_ascii =
        is_plain_ascii(g_app.patch_raw.data(), g_app.patch_raw.size());
    free(data);

    if (!s) { g_app.append_log("[!] Could not parse that patch"); return; }

    // The index's name is preferred: it was decoded at build time, where the
    // 245 Windows-1252 patch files (game names with (TM)/(R)) are handled.
    adopt_session(s, label, e->name, e->platform);
}

static void apply_selected() {
    if (!g_app.session) return;
    const char* target = g_app.target_path.empty() ? nullptr : g_app.target_path.c_str();

    //
    // What the backup has to protect is whatever Apply will OVERWRITE.
    //
    // Usually that is the target itself. For a PS1 or PS2 save it is not: the
    // target is a scratch copy under the cache directory, and the file that
    // gets rewritten is the .PSV it came out of. Backing up the scratch copy
    // would leave the actual save unprotected while the checkbox said
    // otherwise, which is worse than not offering a backup at all.
    //
    const std::string backup_of = g_app.psvcard.found ? g_app.psvcard.path : g_app.target_path;

    if (g_app.backup && target) {
        if (backup_file(backup_of))
            g_app.append_log(("Backup written: " + backup_of + ".bak").c_str());
        else
            g_app.append_log("[!] Backup failed (target unreadable?) — aborting.");
        if (!std::ifstream(backup_of + ".bak")) {
            g_app.show_log = true;
            g_app.apply_msg = "Could not back up the target file, so nothing was patched.\n"
                              "Check the log for details.";
            g_app.open_apply_popup = true;
            return;
        }
    }

    // Byte order for save data — the engine's global setting, re-asserted by
    // apctl_apply() for every code (apollo_free_var_list() clears it).
    const bool be = effective_big_endian();
    apctl_set_big_endian(be ? 1 : 0);
    g_app.log.clear();
    g_app.append_log(be ? "=== Using big-endian data mode"
                        : "=== Using host (little-endian) data mode");
    if (g_settings.byte_order != BYTE_ORDER_AUTO && be != g_app.be_detected)
        g_app.append_log("[!] That byte order is forced in Settings and disagrees "
                         "with this patch");

    // The console's own layer comes OFF before any code runs and goes back ON
    // after — the reverse order, which is not negotiable: wrapping first would
    // encrypt the ciphertext. If it will not come off, stop: patching a file
    // that is still encrypted writes plausible-looking damage.
    if (native_blocked()) {
        g_app.apply_msg = psp_blocked()
            ? "This is a PSP save and its game key is not known yet, so nothing was "
              "patched.\nSupply the key, or untick the unwrap box if the file is "
              "already decrypted."
            : "This is a PS3 save and its secure file ID is not known yet, so nothing "
              "was patched.\nSupply the ID, or untick the unwrap box if the file is "
              "already decrypted.";
        g_app.open_apply_popup = true;
        return;
    }

    // Which console's layer is in play, if either. The two cannot both be: one
    // looks for a PARAM.SFO that lists the target, the other for a PARAM.PFD.
    const bool psp = g_app.psp.found && g_app.psp.wrap && g_app.psp.have_key && target;
    const bool ps3 = g_app.ps3.found && g_app.ps3.wrap && g_app.ps3.have_key && target;
    const bool native = psp || ps3;
    const char* console = psp ? "PSP" : "PS3";
    const char* meta    = psp ? "PARAM.SFO" : "PARAM.PFD";

    // A PS1 or PS2 save, whose target is a scratch copy lifted out of a .PSV.
    // Not mutually exclusive with the above in principle, but no console ever
    // encrypted a memory-card save, so in practice it never coincides.
    const bool container = g_app.psvcard.found && g_app.psvcard.wrap && target;

    if (native) {
        g_app.append_log((std::string("=== Removing the ") + console
                          + "'s own encryption").c_str());
        if (!(psp ? psp_unwrap_target() : ps3_unwrap_target())) {
            g_app.show_log = true;
            g_app.apply_msg = std::string("The ") + console + " layer would not come off, "
                              "so nothing was patched.\nCheck the log for details.";
            g_app.open_apply_popup = true;
            return;
        }
    }

    int applied = 0, errors = 0;
    for (int i = 0; i < apctl_code_count(g_app.session); ++i) {
        if (!g_app.selected[i]) continue;
        apctl_code_t* c = apctl_code_at(g_app.session, i);
        char hdr[256];
        snprintf(hdr, sizeof hdr, "=== Applying code #%d: %s", c->id, c->name);
        g_app.append_log(hdr);
        bool ok = apctl_apply(g_app.session, c, target);
        g_app.append_log(ok ? "- OK" : "- ERROR!");
        ++applied;
        if (!ok) ++errors;
    }
    apctl_reset_vars();
    char buf[80];
    snprintf(buf, sizeof buf, "Patching completed: %d codes applied, %d error(s)", applied, errors);
    g_app.append_log(buf);

    // Re-wrap even when a code failed: the file on disk is the decrypted save
    // either way, and leaving it that way would be leaving the user with
    // something the console cannot read and no obvious way back.
    bool rewrapped = true;
    if (native) {
        g_app.append_log((std::string("=== Restoring the ") + console
                          + "'s own encryption").c_str());
        rewrapped = psp ? psp_wrap_target() : ps3_wrap_target();
        if (!rewrapped) errors++;
    }

    // ...and for the same reason, put a container's file back. This is the
    // step that actually writes the save: everything up to here has been
    // editing a scratch copy, and without it the .PSV on disk is untouched and
    // the patching appears to have done nothing at all.
    bool reboxed = true;
    if (container) {
        g_app.append_log("=== Putting the file back into the .PSV container");
        reboxed = psvcard_wrap_target();
        if (!reboxed) errors++;
        else note_container_rebuilt(g_app.psvcard.path);
    }

    // Result pop-up message.
    char msg[320];
    if (errors == 0) {
        char tail[224] = "";
        if (native)
            snprintf(tail, sizeof tail,
                     "\nThe %s layer was taken off and put back, and %s was "
                     "rewritten with it.", console, meta);
        else if (container)
            snprintf(tail, sizeof tail,
                     "\n%s was put back into the .PSV container, which was "
                     "rebuilt and re-signed.", g_app.psvcard.inner.c_str());
        snprintf(msg, sizeof msg, "All done — %d code(s) applied successfully.%s",
                 applied, tail);
    } else if (container && !reboxed) {
        g_app.show_log = true;
        snprintf(msg, sizeof msg,
                 "The patched file could not be put back into the .PSV, so the "
                 "container on disk is UNCHANGED.\nCheck the log for details.");
    } else if (native && !rewrapped) {
        g_app.show_log = true;
        snprintf(msg, sizeof msg,
                 "The %s layer could not be put back, so the save on disk is "
                 "DECRYPTED.\nCheck the log, then use \"Re-encrypt\" below once the "
                 "cause is fixed.", console);
    } else {
        g_app.show_log = true;   // a failure: the message says to look there
        snprintf(msg, sizeof msg, "%d of %d code(s) failed to apply.\nCheck the log for details.",
                 errors, applied);
    }
    g_app.apply_msg = msg;
    g_app.apply_title = "Apply";
    g_app.open_apply_popup = true;
}

// ---- native file dialogs ---------------------------------------------------
static std::string pick_file(const char* filter_ext) {
    // portable-file-dialogs shells out to osascript/zenity (or Win32), so the
    // dialog runs outside this process — avoiding GLFW's in-process Cocoa
    // breakage on macOS. Called after the frame is rendered (see main loop).
    std::vector<std::string> filters;
    if (filter_ext) {
        filters = { std::string("Apollo patch (*.") + filter_ext + ")",
                    std::string("*.") + filter_ext,
                    "All files", "*" };
    } else {
        filters = { "All files", "*" };
    }
    auto sel = pfd::open_file("Select file", "", filters).result();
    return sel.empty() ? std::string() : sel[0];
}

static std::string pick_save_path(const std::string& suggested) {
    auto p = pfd::save_file("Save patch file", suggested,
                            { "Apollo patch (*.savepatch)", "*.savepatch",
                              "All files", "*" }).result();
    return p;
}

// Native modals are opened AFTER the ImGui frame is rendered (see the main
// loop), not from inside a widget callback — opening a modal mid-frame is the
// second macOS pitfall. Widgets just raise these intents.
static bool g_pending_open   = false;
static bool g_pending_target = false;
static bool g_pending_save_patch = false;
static bool g_pending_psp_key = false;
static bool g_pending_saves_root = false;
static void do_open_patch()    { g_pending_open = true; }
static void do_choose_target() { g_pending_target = true; }
static void do_save_patch()    { g_pending_save_patch = true; }

//
// Write the .savepatch back out, with any code edits in it.
//
// The engine splices the edits into the ORIGINAL bytes rather than
// regenerating the file from its parse, so comments, credits, target-file
// lines and option blocks all survive (see apctl_export_patch). It then
// re-reads what it built and reports codes that would come back different.
// A forced type is written as a title prefix ([SW:...], [BSD:...],
// [PYTHON:...]) and survives. Prefixes compose, so a code already marked
// [DEFAULT:...] or [INFO:...] states its type alongside that; a group heading
// is the one title that cannot carry one.
//
static void save_patch_file(const std::string& path) {
    size_t len = 0;
    char*  text = apctl_export_patch(g_app.session, g_app.patch_bytes.data(),
                                     g_app.patch_bytes.size(), &len);
    if (!text) { g_app.append_log("[!] Could not rebuild the patch file"); return; }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        g_app.append_log("[!] Could not write that file");
        free(text);
        return;
    }
    out.write(text, (std::streamsize)len);
    out.close();

    char msg[512];
    snprintf(msg, sizeof msg, "Saved %s (%zu bytes)", base_name(path), len);
    g_app.append_log(msg);

    int rows[32];
    int miss = apctl_export_mismatches(g_app.session, text, len, rows, 32);
    free(text);

    if (miss > 0) {
        snprintf(msg, sizeof msg,
                 "[!] %d code(s) will read back differently from that file - "
                 "the .savepatch format cannot express every edit, and a group "
                 "heading cannot state a type at all:", miss);
        g_app.append_log(msg);

        for (int i = 0; i < miss && i < 32; i++) {
            apctl_code_t* c = apctl_code_at(g_app.session, rows[i]);
            snprintf(msg, sizeof msg, "      #%d %s", c ? c->id : rows[i],
                     (c && c->name) ? c->name : "");
            g_app.append_log(msg);
        }
        g_app.show_log = true;
    }
}

static void process_pending_dialogs() {
    if (g_pending_open) {
        g_pending_open = false;
        std::string p = pick_file("savepatch");
        if (!p.empty()) load_patch(p);
    }
    if (g_pending_target) {
        g_pending_target = false;
        std::string p = pick_file(nullptr);
        if (!p.empty()) {
            // Chosen by hand, so there is no save behind it: the patcher
            // screen must stop showing a header describing the last one.
            g_app.has_save = false;
            adopt_target(p);
            g_screen = SCREEN_PATCH;
        }
    }
    if (g_pending_psp_key) {
        g_pending_psp_key = false;
        std::string p = pick_file(nullptr);
        if (!p.empty()) {
            std::vector<unsigned char> buf;
            int rc = read_all(p, buf)
                ? apsp_key_from_buffer(buf.data(), buf.size(), g_app.psp.key)
                : APSP_ERR_ARG;
            if (rc == APSP_OK) {
                g_app.psp.have_key = true;
                g_app.psp.key_note = std::string("read from ") + base_name(p);
                for (int i = 0; i < APSP_KEY_LEN; i++)
                    snprintf(g_app.psp.key_hex + i * 2, 3, "%02X", g_app.psp.key[i]);
            } else {
                g_app.psp.key_note = apsp_strerror(rc);
                g_app.append_log((std::string("[!] PSP key file: ") + apsp_strerror(rc)
                                  + " (expected SGKeyDumper's 16 bytes or SGDeemer's 1536)").c_str());
            }
        }
    }
    if (g_pending_saves_root) {
        g_pending_saves_root = false;
        // A FOLDER, not a file: what is being chosen is where to look, and
        // the saves underneath it may be five levels down.
        std::string p = pfd::select_folder("Where your saves are",
                                           g_saved.saves_root).result();
        if (!p.empty()) {
            // Remembered straight away rather than on some later Save: this
            // is the one setting somebody changes by USING the app, and
            // having to re-find a memory stick every launch is the thing the
            // browser exists to stop.
            g_settings.saves_root = g_saved.saves_root = p;
            settings_store();
            scan_start(p);
        }
    }
    if (g_pending_save_patch) {
        g_pending_save_patch = false;
        if (g_app.session && !g_app.patch_bytes.empty()) {
            // Suggest a new name: overwriting the file they opened (or a
            // database patch's own name) is rarely what an edit wants. Only
            // once, though, or a file saved from here twice ends up
            // "-edited-edited".
            std::string base = base_name(g_app.patch_path);
            size_t dot = base.rfind(".savepatch");
            if (dot != std::string::npos) base.erase(dot);

            const std::string tag = "-edited";
            bool tagged = base.size() >= tag.size() &&
                          base.compare(base.size() - tag.size(), tag.size(), tag) == 0;

            std::string p = pick_save_path(base + (tagged ? "" : tag) + ".savepatch");
            if (!p.empty()) save_patch_file(p);
        }
    }
}

// ---- widgets ---------------------------------------------------------------
static void draw_code_list() {
    if (!g_app.session) {
        ImGui::TextDisabled("Open a .savepatch file to begin (File ▸ Open, or the button above).");
        return;
    }

    // Toolbar
    if (ImGui::SmallButton("Select all"))  select_all(true);
    ImGui::SameLine();
    if (ImGui::SmallButton("Select none")) select_all(false);
    ImGui::SameLine();
    ImGui::TextDisabled("|  %d selected", count_selected());

    // Columns: Code | View | Type  (mirrors the Qt tree).
    ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("codes", 3, flags, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthFixed, 48 * g_ui_scale);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 48 * g_ui_scale);
        ImGui::TableHeadersRow();

        for (int i = 0; i < apctl_code_count(g_app.session); ++i) {
            apctl_code_t* c = apctl_code_at(g_app.session, i);
            // Scope each row by the code POINTER, not the row index. ImGui's
            // TableHeadersRow() wraps every header in PushID(column_index), so a
            // header shares an ID scope with a same-index row: the "View" column
            // is index 1, so a row-index-1 (row #2) PushID(1) put its "View"
            // button in the same scope as the "View" header -> identical ID ->
            // conflict, always on row #2, regardless of code names. A pointer
            // can never equal a small column index, so scopes never coincide.
            ImGui::PushID(c);
            ImGui::TableNextRow();

            // --- col 0: checkbox + name ---
            ImGui::TableSetColumnIndex(0);
            float indent = c->is_child ? 18.0f : 0.0f;
            if (indent) ImGui::Indent(indent);
            const char* label = (c->name && c->name[0]) ? c->name : "(unnamed)";
            bool parent = c->is_parent;
            bool disabled = (c->flags & APOLLO_CODE_FLAG_DISABLED) != 0;
            if (parent || disabled)
                ImGui::PushStyleColor(ImGuiCol_Text, parent ? ImVec4(0.80f,0.80f,0.95f,1.0f)
                                                            : ImVec4(0.55f,0.55f,0.55f,1.0f));

            // Parent shows the aggregate of its children (checked / mixed /
            // unchecked) and toggling it propagates to every child — matching
            // the Qt tree's auto-tristate behaviour.
            int cb = 0, ce = 0;
            if (parent) group_children(i, cb, ce);
            int nchild = ce - cb, nchecked = 0;
            for (int j = cb; j < ce; ++j) nchecked += g_app.selected[j] ? 1 : 0;
            bool mixed = parent && nchild > 0 && nchecked > 0 && nchecked < nchild;
            bool chk = (parent && nchild > 0) ? (nchecked == nchild)
                                              : (g_app.selected[i] != 0);

            if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            if (ImGui::Checkbox(label, &chk)) {
                g_app.selected[i] = chk ? 1 : 0;
                for (int j = cb; j < ce; ++j) g_app.selected[j] = chk ? 1 : 0;  // parent -> children
                if (chk) auto_enable_required();   // pull in all [R] codes
            }
            if (mixed) ImGui::PopItemFlag();

            // A hand-edited body is worth seeing from the list: if an apply
            // then misbehaves, this is the first thing to suspect.
            if (apctl_code_is_edited(c)) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "*");
                ImGui::SetItemTooltip("Body edited in this session");
            }

            if (parent || disabled) ImGui::PopStyleColor();
            if (indent) ImGui::Unindent(indent);

            // --- col 1: View/Edit button opens a code window ---
            // Also offered when the body is empty but edited, so a body that
            // was emptied by an edit can still be reached and reverted.
            ImGui::TableSetColumnIndex(1);
            const char* body = apctl_code_text(c);
            if ((body && body[0]) || apctl_code_is_edited(c)) {
                if (ImGui::SmallButton("View")) {
                    if (!g_app.viewer_open[i]) {
                        g_app.viewer_open[i] = 1;
                        // Only on the way in: reopening keeps whatever was
                        // typed and not saved.
                        if (!g_app.viewer_buf[i].loaded) {
                            g_app.viewer_buf[i].set(body);
                            g_app.viewer_buf[i].loaded = true;
                        }
                    }
                    // A window already open may be behind another one.
                    g_app.viewer_buf[i].raise = true;
                }
            }

            // --- col 2: type badge ---
            ImGui::TableSetColumnIndex(2);
            draw_type_icon(c->type, c->flags);

            // --- option dropdown rows (under the Code column) ---
            for (int g = 0; g < apctl_opt_group_count(c); ++g) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Indent(indent + 18.0f);
                bool unfilled = apctl_opt_get_selected(c, g) < 0;
                const char* cur = unfilled ? "<choose a value>"
                    : apctl_opt_value_name(c, g, apctl_opt_get_selected(c, g));
                bool warn = unfilled && chk;
                if (warn) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
                if (ImGui::BeginCombo(apctl_opt_tag(c, g), cur)) {
                    for (int v = 0; v < apctl_opt_value_count(c, g); ++v) {
                        bool sel = (apctl_opt_get_selected(c, g) == v);
                        if (ImGui::Selectable(apctl_opt_value_name(c, g, v), sel))
                            apctl_opt_set_selected(c, g, v);
                    }
                    ImGui::EndCombo();
                }
                if (warn) {
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "(required)");
                }
                ImGui::Unindent(indent + 18.0f);
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// Modeless raw-code windows (one per code whose View button was clicked).
// The .savepatch as text. Worth having: the parser keeps only the codes, so
// author comments, credits and the target-file lines are invisible otherwise.
static void draw_patch_raw() {
    if (!g_app.show_patch_raw) return;

    // ### rather than ##: ImGui hashes a window's whole name, so with ## the
    // file name would be part of the identity and every patch would open at
    // the default position instead of where the last one was left. There is
    // only ever one of these windows.
    char title[256];
    snprintf(title, sizeof title, "Patch file: %s###rawpatch",
             g_app.patch_path.empty() ? "(none)" : base_name(g_app.patch_path));

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title, &open)) {
        ImGui::Text("%zu bytes", g_app.patch_raw.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(g_app.patch_raw.c_str());
        ImGui::Separator();
        ImGui::BeginChild("raw", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        // TextUnformatted skips lines outside the clip rect, so even the
        // largest patches in the database (~430KB) stay cheap to draw.
        const bool mono = g_mono && g_app.patch_raw_ascii;
        if (mono) ImGui::PushFont(g_mono);
        ImGui::TextUnformatted(g_app.patch_raw.c_str());
        if (mono) ImGui::PopFont();
        ImGui::EndChild();
    }
    ImGui::End();
    if (!open) g_app.show_patch_raw = false;
}

// Hex view/edit of the target save file.
static void draw_hex_editor() {
    if (!g_app.show_hex) return;

    static MemoryEditor ed;
    static bool wired = false;
    if (!wired) {
        // Route writes through us so an edit marks the buffer dirty; the
        // editor otherwise pokes the bytes silently.
        ed.WriteFn = [](ImU8* mem, size_t off, ImU8 d, void*) {
            mem[off] = d;
            g_app.hex_dirty = true;
        };
        wired = true;
    }

    // ### so that neither the file name nor the " *" is part of the window's
    // identity -- with ## it was, and the window jumped back to its default
    // place and size the moment a byte was edited.
    char title[512];
    snprintf(title, sizeof title, "Save data: %s%s###hexedit",
             base_name(g_app.hex_path), g_app.hex_dirty ? " *" : "");

    // Fit the window to the grid instead of guessing at it. The ASCII pane is
    // the rightmost column, so a window even slightly too narrow hides part of
    // it -- which 700px did: 16 columns of 10px glyphs want 760. The editor
    // will size itself from CalcTextSize("F"), so this has to be measured
    // with the same font pushed, and before Begin, because SetNextWindowSize
    // applies to the next window rather than the current one.
    if (g_mono) ImGui::PushFont(g_mono);
    MemoryEditor::Sizes sz;
    ed.CalcSizes(sz, g_app.hex_data.size(), 0);
    const float line_h = ImGui::GetTextLineHeight();
    if (g_mono) ImGui::PopFont();

    // Tall enough for the file, within reason: a 64-byte PARAM.SFO does not
    // want the same window as a 4MB save.
    const ImGuiStyle& st = ImGui::GetStyle();
    int rows = (int)((g_app.hex_data.size() + ed.Cols - 1) / (size_t)ed.Cols);
    rows = rows < 8 ? 8 : (rows > 24 ? 24 : rows);
    const float want_h = ImGui::GetFrameHeight()                  // title bar
                       + st.WindowPadding.y * 2
                       + ImGui::GetTextLineHeightWithSpacing() * 2  // path, byte count
                       + st.ItemSpacing.y * 2                       // the separator
                       + rows * line_h
                       + st.ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();  // editor footer

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(sz.WindowWidth, want_h),
                             g_app.hex_fit ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    g_app.hex_fit = false;
    if (ImGui::Begin(title, &open)) {
        ImGui::TextWrapped("%s", g_app.hex_path.c_str());
        ImGui::Text("%zu bytes", g_app.hex_data.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload from disk")) hex_load(g_app.hex_path);
        ImGui::SameLine();
        if (g_app.hex_dirty) {
            if (ImGui::SmallButton("Write changes")) hex_write_back();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "unsaved edits");
        } else {
            ImGui::TextDisabled("no unsaved edits");
        }
        ImGui::Separator();
        if (!g_app.hex_data.empty()) {
            // Only the grid: the lines above carry a file path and a name,
            // which can hold anything, and this font is ASCII.
            if (g_mono) ImGui::PushFont(g_mono);
            ed.DrawContents(g_app.hex_data.data(), g_app.hex_data.size());
            if (g_mono) ImGui::PopFont();
        } else {
            ImGui::TextDisabled("(empty file)");
        }
    }
    ImGui::End();
    if (!open) g_app.show_hex = false;
}

// Modeless code windows: the body as text, and editable.
//
// Editing is session-only — nothing is written back to the .savepatch file, so
// closing the patch drops it. The engine applies from a COPY of the body
// (patches.c strdup()s it), so an edit is not consumed by applying it and
// Apply stays repeatable. Two things an edit cannot do, both fixed at parse
// time: change the code's type, and rename a {TAG} — see apctl_set_code_text().
static void draw_code_viewers() {
    if (!g_app.session) return;
    for (int i = 0; i < apctl_code_count(g_app.session); ++i) {
        if (!g_app.viewer_open[i]) continue;

        apctl_code_t*      c    = apctl_code_at(g_app.session, i);
        AppState::CodeEdit& buf = g_app.viewer_buf[i];
        const char* live    = apctl_code_text(c);
        const bool  unsaved = (buf.text != live);
        const bool  edited  = apctl_code_is_edited(c) != 0;

        // ### so the code's name and the " *" stay out of the identity; with
        // ## the window moved back to its default place on the first edit.
        char title[192];
        snprintf(title, sizeof title, "Code: %s%s###viewer%d",
                 (c->name && c->name[0]) ? c->name : "(unnamed)",
                 unsaved ? " *" : "", i);

        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(560, 400), ImGuiCond_FirstUseEver);
        if (buf.raise) { ImGui::SetNextWindowFocus(); buf.raise = false; }
        if (ImGui::Begin(title, &open)) {
            ImGui::Text("Target file: %s", (c->file && c->file[0]) ? c->file : "(none)");

            // Which interpreter reads the body. The loader guesses it from the
            // [...] header and the shape of the body -- Save Wizard only when
            // every line is exactly "XXXXXXXX YYYYYYYY" -- so one mistyped
            // line, or a missing [PYTHON:] header, lands a code on the wrong
            // interpreter with no way to fix it. It is also the other half of
            // editing: Save Wizard lines rewritten as BSD commands only mean
            // something once the type follows.
            static const int TYPES[] = { APOLLO_CODE_SAVEWIZARD, APOLLO_CODE_BSD,
                                         APOLLO_CODE_PYTHON };
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            if (ImGui::BeginCombo("Runs as", type_name(c->type))) {
                for (int t : TYPES)
                    if (ImGui::Selectable(type_name(t), c->type == t) && c->type != t) {
                        apctl_set_code_type(c, t);
                        char msg[256];
                        snprintf(msg, sizeof msg, "Code #%d \"%s\" now runs as %s",
                                 c->id, c->name ? c->name : "", type_name(t));
                        g_app.append_log(msg);
                    }
                ImGui::EndCombo();
            }

            if (!unsaved) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Save changes")) {
                const size_t was = strlen(live);
                if (apctl_set_code_text(c, buf.text.c_str())) {
                    char msg[256];
                    snprintf(msg, sizeof msg, "Code #%d \"%s\" edited (%zu -> %zu bytes)%s",
                             c->id, c->name ? c->name : "", was, buf.text.size(),
                             apctl_code_is_edited(c) ? "" : " - back to the original");
                    g_app.append_log(msg);
                } else {
                    g_app.append_log("Could not store the edit (out of memory)");
                }
                // set_code_text drops an edit that matches the original, so
                // read the body back rather than assume it took the text.
                buf.set(apctl_code_text(c));
            }
            if (!unsaved) ImGui::EndDisabled();

            ImGui::SameLine();
            if (!edited && !unsaved) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Revert to file")) {
                apctl_revert_code(c);
                buf.set(apctl_code_text(c));
            }
            if (!edited && !unsaved) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(buf.text.c_str());

            ImGui::SameLine();
            if (unsaved)     ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "unsaved edits");
            else if (edited) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "edited");
            else             ImGui::TextDisabled("unchanged");

            // An option's value is written OVER its tag, in place and at the
            // tag's own length, so a tag that has been retyped or deleted stops
            // resolving and its dropdown quietly does nothing. Cheap to catch
            // here, and invisible otherwise until the patch misbehaves.
            int missing = 0;
            for (int g = 0; g < apctl_opt_group_count(c); ++g)
                if (!strstr(buf.text.c_str(), apctl_opt_tag(c, g))) missing++;
            if (missing) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
                ImGui::TextWrapped("%d option placeholder%s missing from the text - "
                                   "that dropdown has nothing left to fill in. Keep the "
                                   "{TAG} exactly as the patch wrote it.",
                                   missing, missing == 1 ? " is" : "s are");
                ImGui::PopStyleColor();
            }

            ImGui::Separator();
            // AllowTabInput: patch bodies (Python especially) are indented, and
            // the default would move focus out of the box instead.
            // One local for both the push and the pop, so the stack stays
            // balanced even when the edit below flips the answer.
            const bool mono = g_mono && buf.ascii;
            if (mono) ImGui::PushFont(g_mono);
            const bool typed =
                ImGui::InputTextMultiline("##body", &buf.text, ImVec2(-FLT_MIN, -FLT_MIN),
                                          ImGuiInputTextFlags_AllowTabInput);
            if (mono) ImGui::PopFont();
            // An edit can bring in a character this font has no glyph for --
            // pasted text especially -- so the answer is re-asked on a change
            // rather than kept from load.
            if (typed) buf.rescan();
        }
        ImGui::End();
        if (!open) g_app.viewer_open[i] = 0;
    }
}

// The database browser. 2200+ rows, so the list is clipped rather than emitted
// in full every frame.
//
// Ask for a modal this big, and take whatever fits.
//
// A popup is clipped to the application window rather than growing it, so a
// size in fixed pixels loses its right-hand edge the moment somebody's window
// is narrower -- which the save browser was, at 920 wide against a default
// window of 860. The constraint is re-applied every frame rather than only on
// open, so dragging the window smaller shrinks the modal with it instead of
// cutting it off.
//
static void size_modal(float want_w, float want_h) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // A margin, so the modal reads as a window on top of the app rather than
    // as the app's own contents.
    const float room_w = vp->WorkSize.x - 40.0f;
    const float room_h = vp->WorkSize.y - 40.0f;
    const ImVec2 want(std::min(want_w, room_w), std::min(want_h, room_h));

    ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 240.0f), ImVec2(room_w, room_h));
    ImGui::SetNextWindowSize(want, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

static void render_db_browser() {
    if (g_db.want_open) {
        g_db.want_open = false;
        g_db.refilter = true;
        ImGui::OpenPopup("Patch database");
    }

    size_modal(620, 520);
    if (!ImGui::BeginPopupModal("Patch database", nullptr,
                                ImGuiWindowFlags_NoSavedSettings))
        return;

    if (!g_db.db) {
        ImGui::TextWrapped("No patch database found: %s", g_db.error.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Put apollo-patches.zip next to the application, or set "
                           "APOLLO_PATCHES_ZIP to its path, then restart. Opening a "
                           ".savepatch file by hand still works without it.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputTextWithHint("##dbsearch", "Game name or title ID...",
                                 g_db.search, sizeof(g_db.search)))
        g_db.refilter = true;

    for (size_t p = 0; p < g_db.platforms.size(); ++p) {
        const char* label = platform_label(g_db.platforms[p].c_str());
        if (p) ImGui::SameLine();
        if (ImGui::RadioButton(label, g_db.platform == int(p))) {
            g_db.platform = int(p);
            g_db.refilter = true;
        }
    }

    if (g_db.refilter) refilter_db();

    ImGui::Separator();

    int chosen = -1;
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();

    // A table, not hand-placed columns: game names run to 60+ characters, and
    // manual right-alignment made the longest ones collide with the title ID.
    // Columns clip instead, and the same clipper keeps 2200 rows cheap.
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;
    if (ImGui::BeginTable("##dbrows", 3, flags, ImVec2(0, -footer))) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##plat", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("PSV ").x);
        ImGui::TableSetupColumn("##id", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("NPUB31842 ").x);

        ImGuiListClipper clipper;
        clipper.Begin(int(g_db.hits.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const int index = g_db.hits[size_t(row)];
                const patchdb_entry_t* e = patchdb_at(g_db.db, index);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(index);
                if (ImGui::Selectable(e->name, false, ImGuiSelectableFlags_SpanAllColumns))
                    chosen = index;
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", e->platform);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", e->title_id);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Text("%d match%s", int(g_db.hits.size()), g_db.hits.size() == 1 ? "" : "es");
    ImGui::SameLine();
    ImGui::TextDisabled("of %d", patchdb_count(g_db.db));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 110.0f);
    if (ImGui::Button("Close", ImVec2(110, 0))) ImGui::CloseCurrentPopup();

    // Enter takes the top hit, matching the web front-end.
    if (chosen < 0 && !g_db.hits.empty() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
        chosen = g_db.hits.front();

    if (chosen >= 0) {
        load_patch_from_db(chosen);
        // Reachable from either screen; picking a game is a request to work on
        // it, so it goes where the codes are.
        g_screen = SCREEN_PATCH;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

//
// Is this save signed to the account in Settings?
//
//   0  no account set in Settings, or the save does not say -- no opinion
//   1  yours
//  -1  somebody else's
//
static int account_verdict(const std::string& save_account) {
    if (save_account.empty() || strlen(g_saved.account_hex) != APFD_ACCT_ID_LEN)
        return 0;
    return lowered(save_account) == lowered(g_saved.account_hex) ? 1 : -1;
}

// The same tick the View menu puts beside Saves and Patcher. ImGui has no
// widget for a check mark on its own, so this is the call MenuItemEx() makes,
// with its sizing and offset copied verbatim so the two cannot drift apart.
static void draw_check_mark() {
    const float  sz = ImGui::GetFontSize();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    ImGui::RenderCheckMark(ImGui::GetWindowDrawList(),
                           ImVec2(p.x, p.y + sz * 0.134f * 0.5f),
                           ImGui::GetColorU32(ImGuiCol_Text), sz * 0.866f);
    ImGui::Dummy(ImVec2(sz, sz));   // so the row is as tall as a line of text
}

//
// Everything about one save, on hover.
//
// The icon the console shows, the slot, whether there are codes, and which
// files are in there with the one the patch will address starred. Enough to
// tell two saves of the same game apart without opening either, and it keeps
// the list itself down to the columns worth scanning.
//
static void draw_save_tooltip(const SaveEntry& s) {
    // At most this many files listed. A save with 40 of them (DiRT 3 ships
    // 17) would otherwise make a tooltip taller than the window.
    static const size_t MAX_FILES = 12;

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);

    icon_load_for(s);
    if (g_icon.tex) {
        icon_draw(112.0f * g_ui_scale, 78.0f * g_ui_scale);
        ImGui::SameLine();
    }

    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.80f, 0.80f, 0.95f, 1.0f), "%s",
                       s.name.empty() ? s.dir_name.c_str() : s.name.c_str());
    if (!s.detail.empty()) ImGui::TextDisabled("%s", s.detail.c_str());
    ImGui::Text("%s   %s", s.platform,
                s.title_id.empty() ? "(no title ID)" : s.title_id.c_str());
    ImGui::EndGroup();

    if (g_icon.corrupt)
        ImGui::TextColored(ImVec4(1.00f, 0.70f, 0.25f, 1.0f),
                           "This save's icon is damaged.");

    ImGui::Separator();
    ImGui::TextDisabled("%s", s.path.c_str());

    if (!s.account.empty()) {
        const int mine = account_verdict(s.account);
        ImGui::TextDisabled("Signed to account %s", s.account.c_str());
        if (mine) {
            ImGui::SameLine();
            ImGui::TextColored(mine > 0 ? ImVec4(0.55f, 0.85f, 0.60f, 1.0f)
                                        : ImVec4(0.95f, 0.75f, 0.45f, 1.0f), "%s",
                                                  mine > 0 ? "- yours" : "- not yours");
        }
    }

    if (s.patch_index >= 0)
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.60f, 1.0f),
                           "Codes: %s", s.patch_name.c_str());
    else
        ImGui::TextDisabled("No codes in the database for this game.");

    if (s.encrypted)
        ImGui::TextDisabled("The %s encrypts this save.", s.platform);

    if (s.files.empty()) {
        ImGui::TextDisabled("Nothing but metadata - no data file to patch.");
    } else {
        std::error_code ec;
        ImGui::Spacing();
        for (size_t f = 0; f < s.files.size() && f < MAX_FILES; f++) {
            const uint64_t n = entry_file_size(s, f);
            // Not %-28.28s: a width cut at a byte boundary would split a
            // multi-byte character in a PS4 or Vita file name. SameLine puts
            // the size in a column where it fits and after the name where it
            // does not, which is the right way round.
            ImGui::Text("%s %s", int(f) == s.suggest ? "*" : " ", s.files[f].c_str());
            ImGui::SameLine(ImGui::GetFontSize() * 20.0f);
            ImGui::TextDisabled("%s", human_size(n).c_str());
        }
        if (s.files.size() > MAX_FILES)
            ImGui::TextDisabled("   ...and %d more",
                                int(s.files.size() - MAX_FILES));
        ImGui::TextDisabled(s.suggest_listed
            ? "* the file the console's own metadata says is the save"
            : "* the largest file - nothing here names one");
    }

    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

//
// Opening a save, with the one guard that matters.
//
// Going BACK to the list keeps the session, so navigation on its own can never
// lose anything. The only point where work can actually go is opening a
// DIFFERENT save, which closes the session that holds the edited code bodies.
// apctl_code_is_edited() makes that detectable, so it is asked rather than
// assumed.
//
static SaveEntry g_pending_save;
static bool      g_pending_save_valid = false;
static bool      g_confirm_discard    = false;

static int count_edited() {
    if (!g_app.session) return 0;

    int n = 0;
    for (int i = 0; i < apctl_code_count(g_app.session); i++)
        if (apctl_code_is_edited(apctl_code_at(g_app.session, i))) n++;
    return n;
}

static void commit_open_save(const SaveEntry& save) {
    const bool same = g_app.has_save && g_app.save.path == save.path;

    // Copied, not referenced: a rescan rebuilds and re-sorts the browser's
    // list, and the patcher screen has to go on describing the save it was
    // given rather than whatever lands at that index afterwards.
    g_app.save     = save;
    g_app.has_save = true;

    if (g_app.save.suggest >= 0) {
        open_save_file(g_app.save, g_app.save.suggest);
    } else {
        // A save holding nothing but its own metadata. Carrying the previous
        // target over would show this save's name above another save's file.
        g_app.target_path.clear();
        g_app.title_hint.clear();
        g_app.psp.clear();
        g_app.ps3.clear();
        g_app.psvcard.clear();
        // ...and the match belongs to that other file. Re-running the
        // detection against an empty target is what clears it.
        detect_patch_for_target();
    }

    //
    // A different save with no patch of its own starts with NO codes.
    //
    // open_save_file loads a patch when the database has one, but nothing
    // closed the last one when it does not -- so the previous game's codes
    // stayed on screen underneath the new save's name and icon, looking for
    // all the world like they belonged to it.
    //
    // Guarded on the save actually changing, so that re-opening the one
    // already in front of you does not throw away a patch loaded by hand.
    //
    if (!same && g_app.match_index < 0)
        g_app.close();

    g_screen = SCREEN_PATCH;
}

static void request_open_save(const SaveEntry& save) {
    const bool same = g_app.has_save && g_app.save.path == save.path;

    if (!same && count_edited() > 0) {
        g_pending_save       = save;
        g_pending_save_valid = true;
        g_confirm_discard    = true;
        return;
    }
    commit_open_save(save);
}

// Which of the open save's files is the target right now. Derived rather than
// remembered, so it stays right when the patch's own target line re-points it
// (see patch_wants_other_file).
static int current_target_index() {
    if (!g_app.has_save || g_app.target_path.empty()) return -1;

    // A container's files are not on disk, so the target is a scratch copy and
    // the path does not contain the save's own. What identifies it there is
    // which file was lifted out, which psvcard_extract() recorded.
    if (g_app.save.container)
        return g_app.psvcard.found ? g_app.psvcard.index : -1;

    for (size_t f = 0; f < g_app.save.files.size(); f++)
        if ((fs::path(g_app.save.path) / g_app.save.files[f]).string() == g_app.target_path)
            return int(f);
    return -1;
}

//
// The saves screen: the list, and nothing else.
//
// Everything ABOUT one save -- its icon, its files, which of them is the
// target -- lives on the patcher screen, where the target actually belongs.
// That is what lets this be a plain full-width table, and what removes the
// old pick-a-file-then-press-Open two-step.
//
static void draw_saves_screen() {
    // --- the folder row ---
    if (ImGui::Button("Choose folder...")) g_pending_saves_root = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pick the folder your saves are in - a memory stick, a\n"
                          "PS3's savedata folder, a USB stick of PS4 exports. Every\n"
                          "save underneath it is found, whatever the layout.");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_sb.root.empty() || g_sb.running.load());
    if (ImGui::Button("Rescan")) scan_start(g_sb.root);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_sb.root.empty() ? "(no folder chosen)" : g_sb.root.c_str());

    // Straight to the file-by-file flow, for somebody who already knows which
    // patch and which file they want and has no use for the list.
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 120.0f * g_ui_scale);
    if (ImGui::Button("Advanced", ImVec2(120.0f * g_ui_scale, 0))) {
        g_advanced = true;
        g_screen   = SCREEN_PATCH;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Skip the list: open any .savepatch and any target file\n"
                          "by hand. Everything below is the same either way.");

    if (g_sb.running.load()) {
        ImGui::Text("Scanning... %d folders", g_sb.seen.load());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) g_sb.cancel = true;
    } else if (!g_sb.note.empty()) {
        ImGui::TextDisabled("%s", g_sb.note.c_str());
    } else {
        ImGui::TextDisabled("Nothing scanned yet.");
    }

    ImGui::Separator();

    //
    // Nothing to list yet -- which is the FIRST THING anybody sees, so it
    // says what to do rather than showing an empty table with headings.
    //
    if (g_sb.root.empty() && !g_sb.running.load()) {
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Point the app at wherever your saves are and it will list them by game.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "A memory stick, a folder pulled off a PS3's hard drive, a USB stick of\n"
            "PS4 or Vita exports - whatever the layout underneath. PSP and PS3 saves\n"
            "are unwrapped for you, and each game's codes are loaded when you open it.");
        ImGui::Spacing();
        if (ImGui::Button("Choose folder...##empty", ImVec2(180, 0)))
            g_pending_saves_root = true;
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("Have a loose save file, or a .savepatch of your own?");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open the patcher directly")) {
            g_advanced = true;
            g_screen   = SCREEN_PATCH;
        }
        return;
    }

    // --- filters ---
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##savesearch", "Game, title ID or folder...",
                                 g_sb.search, sizeof g_sb.search))
        g_sb.refilter = true;

    // Wrapped rather than laid out with SameLine alone, so the tail of the row
    // cannot end up off the edge of a narrow window with no way to reach it.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float right = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
    auto same_line_if_it_fits = [&](const char* next) {
        const float w = ImGui::CalcTextSize(next).x + ImGui::GetFrameHeight()
                      + st.ItemInnerSpacing.x;
        if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + w < right)
            ImGui::SameLine();
    };

    for (size_t p = 0; p < g_sb.platforms.size(); ++p) {
        const char* label = platform_label(g_sb.platforms[p].c_str());
        if (p) same_line_if_it_fits(label);
        if (ImGui::RadioButton(label, g_sb.platform == int(p))) {
            g_sb.platform = int(p);
            g_sb.refilter = true;
        }
    }
    if (!g_sb.platforms.empty()) same_line_if_it_fits("Only saves with codes");
    if (ImGui::Checkbox("Only saves with codes", &g_sb.only_coded))
        g_sb.refilter = true;

    if (g_sb.refilter) refilter_saves();

    // --- the list ---
    bool        open_now = false;
    const float footer = ImGui::GetFrameHeightWithSpacing() + st.ItemSpacing.y;

    const bool know_account = strlen(g_saved.account_hex) == APFD_ACCT_ID_LEN;

    if (ImGui::BeginTable("##saverows", know_account ? 6 : 5,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                          ImVec2(0, -footer))) {
        // Both stretch, and the weights are the point: the slot is what tells
        // two saves of the same game apart, so it earns real width rather
        // than just enough for its longest value. A game name that does not
        // fit is clipped, and the full one is in the tooltip.
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("Console", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Console ").x);
        ImGui::TableSetupColumn("Title ID", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("NPUB31842 ").x);
        ImGui::TableSetupColumn("Codes", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Codes ").x);
        // Shown only once an account is named in Settings, which is exactly
        // when "is this one mine?" has an answer worth a column.
        if (know_account)
            ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("Owner ").x);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(int(g_sb.hits.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const int index = g_sb.hits[size_t(row)];
                const SaveEntry& s = g_sb.saves[size_t(index)];

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(index);
                if (ImGui::Selectable(s.name.empty() ? s.dir_name.c_str() : s.name.c_str(),
                                      g_sb.selected == index,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    g_sb.selected = index;
                    if (ImGui::IsMouseDoubleClicked(0)) open_now = true;
                }
                // Everything about the save, on hover -- without spending
                // a pane's worth of width on it. The list stays scannable
                // and the answers are a pointer-rest away.
                //
                // NoSharedDelay: the wait re-arms on every row. Without it
                // ImGui carries the timer over to the next item, so once one
                // panel had appeared the rest would follow instantly -- and
                // the delay is doing real work here, since the panel decodes
                // the save's icon. Scrubbing down 176 rows decodes nothing.
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal |
                                         ImGuiHoveredFlags_NoSharedDelay))
                    draw_save_tooltip(s);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", s.detail.empty() ? "-" : s.detail.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", s.platform);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextDisabled("%s", s.title_id.empty() ? "-" : s.title_id.c_str());
                ImGui::TableSetColumnIndex(4);
                if (s.patch_index >= 0)
                    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.60f, 1.0f), "yes");
                else
                    ImGui::TextDisabled("-");
                if (know_account) {
                    ImGui::TableSetColumnIndex(5);
                    // A tick when the save is yours and nothing when it is
                    // not. Leaving the other cases blank rather than marking
                    // them keeps the column scannable -- what the eye is
                    // looking for down it is the saves that ARE yours, and a
                    // word in every row would bury those among the rest.
                    // Whose it is instead, and whether it says at all, is in
                    // the hover panel.
                    if (account_verdict(s.account) > 0) draw_check_mark();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    // --- footer ---
    ImGui::Text("%d save%s", int(g_sb.hits.size()), g_sb.hits.size() == 1 ? "" : "s");
    ImGui::SameLine();
    ImGui::TextDisabled("of %d", int(g_sb.saves.size()));

    const bool have = g_sb.selected >= 0 && g_sb.selected < int(g_sb.saves.size());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 150.0f);
    ImGui::BeginDisabled(!have);
    if (ImGui::Button("Open save", ImVec2(150, 0))) open_now = true;
    ImGui::EndDisabled();
    if (have && ImGui::IsItemHovered())
        ImGui::SetTooltip("Open this save's files and codes.");

    // Enter takes the selection, matching the patch database browser.
    if (!open_now && have && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
        open_now = true;

    if (open_now && have)
        request_open_save(g_sb.saves[size_t(g_sb.selected)]);
}

static void draw_menu_bar(bool* want_quit) {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Saves", "Ctrl+B", g_screen == SCREEN_SAVES))
                g_screen = SCREEN_SAVES;
            if (ImGui::MenuItem("Patcher", "Ctrl+P", g_screen == SCREEN_PATCH))
                g_screen = SCREEN_PATCH;
            ImGui::Separator();
            if (ImGui::MenuItem("Find a game...", "Ctrl+F")) g_db.want_open = true;
            // Advanced is a DOOR into the patcher screen, not a screen of its
            // own: it shows the file pickers there instead of a save's own
            // files. Ticking it goes straight there, because the two controls
            // it reveals are the only reason to tick it.
            if (ImGui::MenuItem("Advanced: pick files by hand", nullptr, g_advanced)) {
                g_advanced = !g_advanced;
                if (g_advanced) g_screen = SCREEN_PATCH;
            }
            if (ImGui::MenuItem("Open .savepatch...", "Ctrl+O")) do_open_patch();
            if (ImGui::MenuItem("Save .savepatch as...", "Ctrl+S",
                                false, g_app.session != nullptr)) do_save_patch();
            if (ImGui::MenuItem("Choose target file...")) do_choose_target();
            ImGui::Separator();
            if (ImGui::MenuItem("Settings...")) g_want_settings = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Q")) *want_quit = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("User guide")) open_url(URL_GUIDE);
            if (ImGui::MenuItem("Project on GitHub")) open_url(URL_PATCHER);
            ImGui::Separator();
            if (ImGui::MenuItem("About " APP_NAME "...")) g_want_about = true;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

//
// The PSP savedata section. Shown only when psp_detect() found a PARAM.SFO
// beside the target that lists it, so it never appears for the 99% of saves it
// has nothing to say about.
//
// The checkbox is the main event and defaults to ON: a detected PSP save is
// almost always one somebody wants patched, and doing it by hand means three
// steps in the right order. The three buttons below are for the other case —
// opening a save in the hex editor, or repairing one that a failed run left
// decrypted.
//
static void draw_psp_section() {
    if (!g_app.psp.found) return;

    ImGui::Spacing();
    ImGui::SeparatorText("PSP save");

    ImGui::Checkbox("Unwrap the console's encryption before patching, and put it back after",
                    &g_app.psp.wrap);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The PSP encrypts saves itself, underneath whatever the game does,\n"
                          "and a .savepatch only ever addresses the inner layer.\n"
                          "Leave this on unless the file is already decrypted.");

    ImGui::TextDisabled("%s in %s - mode 0x%02X, %s", g_app.psp.listed.c_str(),
                        g_app.psp.directory.c_str(), g_app.psp.mode & 0xFF,
                        g_app.psp.keyed ? "keyed" : "unkeyed");

    // The game key. Found in the bundled database for a game Apollo knows;
    // otherwise typed, or read from a dumper's file. Never guessed: without
    // one, everything here stays disabled rather than quietly decrypting in
    // the unkeyed mode and handing back noise.
    if (g_app.psp.keyed) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        if (ImGui::InputText("Game key", g_app.psp.key_hex, sizeof g_app.psp.key_hex,
                             ImGuiInputTextFlags_CharsHexadecimal |
                             ImGuiInputTextFlags_CharsUppercase)) {
            g_app.psp.have_key =
                apsp_key_from_hex(g_app.psp.key_hex, g_app.psp.key) == APSP_OK;
            g_app.psp.key_note = g_app.psp.have_key
                ? "entered by hand"
                : (g_app.psp.key_hex[0] ? "needs 32 hex digits" : "");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Load key file...")) g_pending_psp_key = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("SGKeyDumper's 16 bytes, or SGDeemer's 1536.");

        ImGui::SameLine();
        if (g_app.psp.have_key)
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "%s", g_app.psp.key_note.c_str());
        else
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "%s",
                               g_app.psp.key_note.empty() ? "no key yet"
                                                          : g_app.psp.key_note.c_str());
    }

    ImGui::BeginDisabled(!g_app.psp.have_key);
    if (ImGui::Button("Decrypt only")) {
        const bool ok = psp_unwrap_target();
        if (ok) g_app.psp.wrap = false;   // plaintext now; do not unwrap twice
        report_action("Decrypt only", ok,
                      "The console's layer is off, so the file on disk is now "
                      "plaintext.\nThe unwrap box above has been turned off to "
                      "match.");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Take the console's layer off and leave it off, for editing\n"
                          "the file by hand. Turns the checkbox above off.");
    ImGui::SameLine();
    if (ImGui::Button("Re-encrypt")) {
        const bool ok = psp_wrap_target();
        if (ok) g_app.psp.wrap = true;
        report_action("Re-encrypt", ok,
                      "The console's layer is back on, and PARAM.SFO has been "
                      "rewritten\nwith the file's new hash.");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Put the console's layer back on, and rewrite PARAM.SFO\n"
                          "with the file's new hash.");
    ImGui::EndDisabled();

    ImGui::Spacing();
}

// The PS3 savedata section, shown on the same terms as the PSP one above: only
// when ps3_detect() found a PARAM.PFD beside the target that lists it.
static void draw_ps3_section() {
    if (!g_app.ps3.found) return;

    ImGui::Spacing();
    ImGui::SeparatorText("PS3 save");

    ImGui::Checkbox("Unwrap the console's encryption before patching, and put it back after##ps3",
                    &g_app.ps3.wrap);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The PS3 encrypts the files it lists in PARAM.PFD, underneath\n"
                          "whatever the game does, and a .savepatch only ever addresses\n"
                          "the inner layer. Leave this on unless the file is already\n"
                          "decrypted.");

    ImGui::TextDisabled("%s in %s - PARAM.PFD v%d%s", g_app.ps3.listed.c_str(),
                        g_app.ps3.folder.c_str(), g_app.ps3.version,
                        g_app.ps3.trophy ? ", trophy folder" : "");

    // The save header above already says this for a save opened from the
    // list. Repeated here only for a target picked by hand, where there is
    // no header to say it.
    if (g_app.ps3.account[0] && (!g_app.has_save || g_advanced)) {
        const int mine = account_verdict(g_app.ps3.account);
        ImGui::TextDisabled("Signed to account %s", g_app.ps3.account);
        if (mine) {
            ImGui::SameLine();
            ImGui::TextColored(mine > 0 ? ImVec4(0.55f, 0.85f, 0.60f, 1.0f)
                                        : ImVec4(0.95f, 0.75f, 0.45f, 1.0f), "%s",
                                                  mine > 0 ? "- yours" : "- not yours");
        }
    }

    // The secure file ID. Found in the bundled database for a game Apollo
    // knows; otherwise typed. Never guessed: without one, everything here
    // stays disabled rather than handing back noise.
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
    if (ImGui::InputText("Secure file ID", g_app.ps3.key_hex, sizeof g_app.ps3.key_hex,
                         ImGuiInputTextFlags_CharsHexadecimal |
                         ImGuiInputTextFlags_CharsUppercase)) {
        g_app.ps3.have_key =
            apfd_sfid_from_hex(g_app.ps3.key_hex, g_app.ps3.sfid) == APFD_OK;
        g_app.ps3.key_note = g_app.ps3.have_key
            ? "entered by hand"
            : (g_app.ps3.key_hex[0] ? "needs 32 hex digits" : "");
        g_app.ps3.hash_checked = false;      // a new key, an unanswered question
    }
    ImGui::SameLine();
    if (g_app.ps3.have_key)
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "%s", g_app.ps3.key_note.c_str());
    else
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "%s",
                           g_app.ps3.key_note.empty() ? "no key yet"
                                                      : g_app.ps3.key_note.c_str());

    // Whether PARAM.PFD still describes the file on disk. Worth saying plainly:
    // a save that already disagrees was damaged before it got here, and
    // patching it would sign the damage into place.
    if (g_app.ps3.hash_checked && !g_app.ps3.hash_ok) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
                           "PARAM.PFD's recorded hash does not match this file.");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Either the secure file ID is wrong for this game, or the\n"
                              "save was edited without its PARAM.PFD being updated.\n"
                              "Patching would re-sign it as it is.");
    }

    ImGui::BeginDisabled(!g_app.ps3.have_key);
    if (ImGui::Button("Decrypt only##ps3")) {
        const bool ok = ps3_unwrap_target();
        if (ok) {
            g_app.ps3.wrap = false;   // it is plaintext now; do not unwrap twice
            g_app.ps3.hash_checked = false;
        }
        report_action("Decrypt only", ok,
                      "The console's layer is off, so the file on disk is now "
                      "plaintext.\nThe unwrap box above has been turned off to "
                      "match.");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Take the console's layer off and leave it off, for editing\n"
                          "the file by hand. Turns the checkbox above off.");
    ImGui::SameLine();
    if (ImGui::Button("Re-encrypt##ps3")) {
        const bool ok = ps3_wrap_target();
        if (ok) {
            g_app.ps3.wrap = true;
            g_app.ps3.hash_checked = false;
        }
        report_action("Re-encrypt", ok,
                      "The console's layer is back on, and PARAM.PFD has been "
                      "rewritten\nwith the file's new size and hash.");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Put the console's layer back on, and rewrite PARAM.PFD\n"
                          "with the file's new size and hash.");

    ImGui::EndDisabled();

    ImGui::Spacing();
}

// Which console saves are written FOR. A window rather than a modal: somebody
// checking a value against their console's dumper wants to see the save behind
// it.
// Explanatory text under a setting. Dimmed, and WRAPPED to the window rather
// than hard-wrapped at a column that was chosen for a smaller font.
static void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

static void draw_settings_window() {
    if (!g_want_settings) return;

    //
    // Sized to its own contents, measured rather than guessed.
    //
    // AlwaysAutoResize would fit exactly but has no upper bound, and a window
    // taller than the display puts Save out of reach entirely. A fixed size
    // has the opposite fault: a line too many and the body scrolls for the
    // sake of one row. So the body reports what it needed last frame and the
    // window asks for precisely that, clamped to the screen -- an exact fit
    // in the ordinary case, and the buttons still pinned when a small display
    // forces a clamp.
    //
    static float body_h = 0.0f;   // what the settings needed, measured below

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiStyle&    st = ImGui::GetStyle();
    const float room_w = vp->WorkSize.x - 80.0f;
    const float room_h = vp->WorkSize.y - 80.0f;
    const float footer = ImGui::GetFrameHeightWithSpacing()
                       + ImGui::GetTextLineHeightWithSpacing()
                       + st.ItemSpacing.y;
    // Title bar, both window paddings, and a couple of pixels of slack --
    // being a shade too tall costs a sliver of empty space, where being a
    // shade too short costs the scrollbar this exists to avoid.
    const float chrome = ImGui::GetFrameHeight() + st.WindowPadding.y * 2.0f + 4.0f;
    const float want_h = body_h > 0.0f ? body_h + footer + chrome
                                       : ImGui::GetFontSize() * 28.0f;

    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 240.0f), ImVec2(room_w, room_h));
    ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetFontSize() * 34.0f, room_w),
                                    std::min(want_h, room_h)),
                             ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin("Settings", &g_want_settings)) {
        ImGui::End();
        return;
    }

    // Everything above the action row lives here; the row is drawn after this
    // child and so is always on screen, whatever happens to the body.
    ImGui::BeginChild("##setbody", ImVec2(0, -footer));

    hint("Which console Apollo writes saves FOR. Every field is optional, and "
         "none of them changes how a save is READ - only what is written back. "
         "Left blank, a save keeps whatever it already says.");

    ImGui::SeparatorText("Save data");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const char* ORDERS[] = { "Auto (detect from the patch)", "Big-endian", "Little-endian" };
    bool order_changed = ImGui::Combo("Byte order", &g_settings.byte_order,
                                      ORDERS, IM_ARRAYSIZE(ORDERS));
    hint("Auto is right for every patch in the database. Force one only for a "
         "loose patch file for a console the database does not cover.");
    if (g_settings.byte_order != BYTE_ORDER_AUTO) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.35f, 1.0f));
        ImGui::TextWrapped("Remembered across runs and applied to every save. The "
                           "patcher screen says so when it disagrees with the patch "
                           "you have open.");
        ImGui::PopStyleColor();
    }

    ImGui::SeparatorText("PSP");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    bool changed = order_changed;
    changed |= ImGui::InputText("Fuse ID", g_settings.fuse_hex, sizeof g_settings.fuse_hex,
                                    ImGuiInputTextFlags_CharsHexadecimal |
                                    ImGuiInputTextFlags_CharsUppercase);
    hint("16 hex digits, identifying one specific PSP. Most games load a save "
         "whose value differs, but console-locked titles check the hashes it "
         "derives and flag the save when they do not match - Gran Turismo does "
         "- so those need the fuse ID of the console that will play the save. "
         "Blank = FFFFFFFFFFFFFFFF.");

    ImGui::SeparatorText("PS3");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
    changed |= ImGui::InputText("Console ID (IDPS)", g_settings.console_hex,
                                sizeof g_settings.console_hex,
                                ImGuiInputTextFlags_CharsHexadecimal |
                                ImGuiInputTextFlags_CharsUppercase);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    changed |= ImGui::InputInt("User number", &g_settings.user_id);
    if (g_settings.user_id < 1) g_settings.user_id = 1;

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
    changed |= ImGui::InputText("Account ID (PSN)", g_settings.account_hex,
                                sizeof g_settings.account_hex,
                                ImGuiInputTextFlags_CharsHexadecimal);

    hint("Two ways to re-sign a save, and the account is usually the one to "
         "reach for: 16 hex digits, written into the save's own PARAM.SFO, so it "
         "loads on ANY PS3 that account has signed in to. The console ID is 32 "
         "hex digits and binds a save to one machine; the user number reaches "
         "only a trophy folder's hashes. Name either and the PS3 section offers "
         "the matching action.");

    // The fields are all-or-nothing: a half-typed value is not "no value", it
    // is one that would bind a save to the wrong machine, or the wrong account.
    const size_t fuse_len = strlen(g_settings.fuse_hex);
    const size_t cid_len  = strlen(g_settings.console_hex);
    const size_t acct_len = strlen(g_settings.account_hex);
    const bool   ok = (fuse_len == 0 || fuse_len == 16)
                   && (cid_len  == 0 || cid_len  == 32)
                   && (acct_len == 0 || acct_len == APFD_ACCT_ID_LEN);

    /* The order alone: apply just the byte order, not settings_apply(), which
     * would also push whatever half-typed hex is in the fields. */
    if (order_changed) {
        apctl_set_big_endian(effective_big_endian() ? 1 : 0);
        g_saved.byte_order = g_settings.byte_order;
        settings_store();
    }
    if (changed) g_settings.status.clear();

    // What all of the above came to, for the window size next frame.
    body_h = ImGui::GetCursorPosY();

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::BeginDisabled(!ok);
    if (ImGui::Button("Save")) {
        settings_apply();
        g_saved = g_settings;
        g_settings.status = settings_store()
            ? (cid_len == 32 ? "Saved. PS3 saves will be re-bound to this console."
                             : "Saved. Saves keep whatever console they are bound to.")
            : "Applied, but the settings file could not be written.";
        g_app.append_log(("Settings: " + g_settings.status).c_str());
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Clear all")) {
        g_settings.fuse_hex[0] = g_settings.console_hex[0] = '\0';
        g_settings.account_hex[0] = '\0';
        g_settings.user_id = 1;
        g_settings.byte_order = BYTE_ORDER_AUTO;
        settings_apply();
        g_saved = g_settings;
        settings_store();
        g_settings.status = "Cleared. Saves keep whatever console they are bound to.";
    }

    if (!ok) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "%s",
                           fuse_len && fuse_len != 16 ? "the Fuse ID needs 16 hex digits"
                           : cid_len && cid_len != 32  ? "the console ID needs 32 hex digits"
                                                       : "the account ID needs 16 hex digits");
    } else if (!g_settings.status.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "%s", g_settings.status.c_str());
    }

    ImGui::End();
}

/*
 * What byte order the engine will run with, and why.
 *
 * A line rather than a control: the choice lives in Settings now, and what is
 * worth having next to Apply is the ANSWER. The amber case is the one this
 * exists for -- a mode forced in Settings that contradicts the patch in front
 * of you writes byte-reversed values and produces a save that looks patched, so
 * it has to be visible from where the patching happens rather than two menus
 * away.
 */
static void draw_byte_order() {
    const bool be     = effective_big_endian();
    const bool forced = g_settings.byte_order != BYTE_ORDER_AUTO;
    const char* order = be ? "big-endian" : "little-endian";

    ImGui::TextDisabled("Byte order:");
    ImGui::SameLine();

    if (!g_app.session) {
        ImGui::TextDisabled(forced ? "%s, forced in Settings"
                                   : "%s until a patch says otherwise", order);
    } else if (forced && be != g_app.be_detected) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                           "%s, forced in Settings - but this is a %s patch",
                           order,
                           g_app.be_platform.empty()
                               ? (g_app.be_detected ? "big-endian" : "little-endian")
                               : g_app.be_platform.c_str());
    } else if (forced) {
        ImGui::Text("%s, forced in Settings", order);
    } else {
        ImGui::Text("%s%s", order,
                    g_app.be_detected ? " (PS3 title)" : " (not a PS3 title)");
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Read/write save data as big-endian (PS3) or little-endian\n"
                          "(PS4, PS Vita, PSP, PC). Same as the patcher CLI's\n"
                          "-b/--big-endian flag. Change it in File > Settings.");
}

//
// The save this screen is working on: what it is, and which of its files is
// the target -- next to the controls that act on it, rather than back on the
// list where nothing could be done about it.
//
//
// What applies to the WHOLE save, as a column on the right of the header.
//
// Kept apart from the PSP/PS3 sections below on purpose, because they answer
// different questions. Everything down there is about the one file the File
// dropdown names: unwrap it, put it back, the key it needs. These are about
// the save -- every encrypted file in it, the metadata covering all of them,
// whose it is. Side by side, "Re-bind to your console" sat a few pixels from
// "Decrypt only" and the two read as the same kind of thing.
//
// Drawn only for the two consoles that wrap saves themselves; nothing else
// here has a whole-save action to offer.
//
static void draw_save_actions() {
    const bool psp = g_app.psp.found, ps3 = g_app.ps3.found;
    if (!psp && !ps3) return;

    const bool can_sign   = ps3 && strlen(g_saved.account_hex) == APFD_ACCT_ID_LEN;
    const bool can_rebind = ps3 && apfd_get_console(nullptr) != 0;
    const char* resign    = psp ? "Resign PARAM.SFO" : "Resign PARAM.PFD";

    // Width from the widest label actually on show, the way every column in
    // this app is sized -- a fixed one would either clip "Re-bind to your
    // console" or leave the short labels swimming.
    const ImGuiStyle& st = ImGui::GetStyle();
    float bw = 0.0f;
    auto widen = [&](const char* s) {
        const float w = ImGui::CalcTextSize(s).x;
        if (w > bw) bw = w;
    };
    widen("Decrypt all files");
    widen("Re-encrypt all files");
    widen(resign);
    if (can_sign)   widen("Sign to your account");
    if (can_rebind) widen("Re-bind to your console");
    bw += st.FramePadding.x * 2.0f + 8.0f;

    const ImVec2 sz(bw, 0.0f);
    const float  total = bw * 2.0f + st.ItemSpacing.x;
    const float  room  = ImGui::GetWindowContentRegionMax().x
                       - ImGui::GetWindowContentRegionMin().x;

    // Only if there is somewhere to put it. On a narrow window the buttons
    // fall below the header rather than overlapping the save's name.
    if (room > total + bw) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - total);
    } else {
        ImGui::Spacing();
    }

    // Two across, filling left to right. Called immediately before each
    // button, so only the ones actually drawn count towards the wrap -- the
    // last two appear only once Settings names an account or a console.
    int n = 0;
    auto place = [&] { if (n++ & 1) ImGui::SameLine(); };

    ImGui::BeginGroup();

    ImGui::BeginDisabled(psp ? !g_app.psp.have_key : !g_app.ps3.have_key);
    place();
    if (ImGui::Button("Decrypt all files##wsa", sz)) {
        if (psp) psp_all_files(false);
        else   { ps3_all_files(false); g_app.ps3.hash_checked = false; }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every file the console wrapped, not just the target.%s",
                          ps3 ? "\nPARAM.SFO is listed but never encrypted, so it is\n"
                                "left alone. Needs a key for each file." : "");
    place();
    if (ImGui::Button("Re-encrypt all files##wsa", sz)) {
        if (psp) psp_all_files(true);
        else   { ps3_all_files(true); g_app.ps3.hash_checked = false; }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Put the layer back on every file, and rewrite the\n"
                          "metadata. The other half of \"Decrypt all files\".");
    ImGui::EndDisabled();

    place();
    if (psp) {
        if (ImGui::Button("Resign PARAM.SFO##wsa", sz))
            report_action("Resign PARAM.SFO", psp_resign(),
                          "PARAM.SFO's own hashes have been recomputed.\n"
                          "No file in the save was touched.");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Recompute the PARAM.SFO hashes alone. Needs no game key -\n"
                              "for a save that was never encrypted.");
    } else {
        if (ImGui::Button("Resign PARAM.PFD##wsa", sz))
            report_action("Resign PARAM.PFD", ps3_resign(),
                          "PARAM.PFD's signatures have been recomputed.\n"
                          "Every entry in it was left as it was.");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Recompute the PARAM.PFD signatures alone. Needs no key -\n"
                              "for a database whose entries something else edited.");
    }

    // Before the console button because it is the one to reach for: an account
    // ID travels between machines where an IDPS does not.
    if (can_sign) {
        place();
        ImGui::BeginDisabled(g_app.ps3.sfo_path.empty());
        if (ImGui::Button("Sign to your account##wsa", sz))
            report_action("Sign to your account", ps3_account_resign(),
                          "This save is now signed to your account, in both\n"
                          "places PARAM.SFO keeps it, and PARAM.PFD has been\n"
                          "updated to match.");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(g_app.ps3.sfo_path.empty()
                ? "No PARAM.SFO beside this save - the account fields live in it."
                : "Write account %s into this save's PARAM.SFO and update\n"
                  "PARAM.PFD to match, so it loads on any PS3 signed in to\n"
                  "that account.", g_saved.account_hex);
    }

    if (can_rebind) {
        place();
        ImGui::BeginDisabled(g_app.ps3.sfo_path.empty());
        if (ImGui::Button("Re-bind to your console##wsa", sz))
            report_action("Re-bind to your console", ps3_rebind(),
                          "This save is now bound to the console in Settings,\n"
                          "and PARAM.PFD has been re-signed around it.");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(g_app.ps3.sfo_path.empty()
                ? "No PARAM.SFO beside this save - the hashes are taken over it."
                : "Rewrite the PARAM.SFO hashes that name a console, so the save\n"
                  "belongs to the one in Settings instead of the one it came from.");
    }

    ImGui::EndGroup();
}

static void draw_save_header() {
    const SaveEntry& s = g_app.save;

    // The picture the console itself shows. Decoded -- or, for a PS1 or PS2
    // save, rendered -- on demand, so only the save actually open has a
    // texture.
    icon_load_for(s);
    if (g_icon.tex) {
        icon_draw(128.0f * g_ui_scale, 88.0f * g_ui_scale);
        icon_hover_panel();
        ImGui::SameLine();
    }

    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.80f, 0.80f, 0.95f, 1.0f), "%s",
                       s.name.empty() ? s.dir_name.c_str() : s.name.c_str());
    if (!s.detail.empty()) ImGui::TextDisabled("%s", s.detail.c_str());
    ImGui::Text("%s   %s", s.platform,
                s.title_id.empty() ? "(no title ID)" : s.title_id.c_str());
    ImGui::TextDisabled("%s", s.dir_name.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.path.c_str());
    ImGui::EndGroup();

    draw_save_actions();

    //
    // Only when there IS an icon and it would not read. A save with no
    // ICON0.PNG at all is ordinary and says nothing.
    //
    // A DAMAGED one is not an aside. An icon whose header claims more than its
    // file holds was truncated by something, and that something had no reason
    // to stop at the icon -- so this is the one case that gets a colour and
    // says what it implies about the rest of the save, rather than being
    // greyed out next to the file list.
    //
    if (g_icon.corrupt) {
        ImGui::TextColored(ImVec4(1.00f, 0.70f, 0.25f, 1.0f),
                           "This save's icon is DAMAGED (%s).",
                           g_icon.error.c_str());
        ImGui::TextDisabled("Whatever truncated it may have damaged the save "
                            "data too - check the file before trusting it.");
    } else if (!g_icon.tex && !g_icon.error.empty()) {
        ImGui::TextDisabled("(the icon is %s)", g_icon.error.c_str());
    } else if (g_icon.flat_texture) {
        ImGui::TextDisabled("(the icon's 3D model would not read - showing its "
                            "texture instead)");
    }

    // Where signing happens differs by console: a PS3 save has a whole
    // section below for its encryption layer and the button belongs there
    // with the rest of it, while a PS4 or Vita save has no such section --
    // nothing is encrypted -- so its one button belongs here.
    const bool sfo_account = (s.platform_id == ASAVE_PS4 || s.platform_id == ASAVE_PSVITA);
    const bool know_account = strlen(g_saved.account_hex) == APFD_ACCT_ID_LEN;

    if (!s.account.empty() || (sfo_account && know_account)) {
        const int mine = account_verdict(s.account);
        if (s.account.empty())
            ImGui::TextDisabled("This save names no account");
        else
            ImGui::TextDisabled("Signed to account %s", s.account.c_str());
        if (mine) {
            ImGui::SameLine();
            ImGui::TextColored(mine > 0 ? ImVec4(0.55f, 0.85f, 0.60f, 1.0f)
                                        : ImVec4(0.95f, 0.75f, 0.45f, 1.0f), "%s",
                               mine > 0 ? "- yours"
                                        : (sfo_account ? "- not yours"
                                                       : "- not yours, sign it below"));
        }
        if (sfo_account && know_account && mine <= 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Sign to your account"))
                report_action("Sign to your account", sfo_account_resign(),
                              "This save is now signed to your account. Exactly the\n"
                              "eight bytes of ACCOUNT_ID changed - there is no\n"
                              "signature here to keep in step.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Write account %s into this save's sce_sys/param.sfo,\n"
                                  "so the console treats it as yours. Nothing else in the\n"
                                  "save changes - there is no signature to keep in step.",
                                  g_saved.account_hex);
        }
    }

    if (s.encrypted)
        ImGui::TextDisabled("The %s encrypts this save; it is unwrapped on the way in "
                            "and re-wrapped on the way out.", s.platform);

    // --- which file ---
    if (s.files.empty()) {
        ImGui::TextWrapped("This save holds nothing but its own metadata - no data "
                           "file to patch.");
        return;
    }

    const int current = current_target_index();
    std::error_code ec;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("File");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-160.0f * g_ui_scale);
    if (ImGui::BeginCombo("##targetfile",
                          current >= 0 ? s.files[size_t(current)].c_str()
                                       : "(none - choose one)")) {
        for (size_t f = 0; f < s.files.size(); f++) {
            const uint64_t n = entry_file_size(s, f);
            char row[640];
            // The star is the console's own answer, or the fallback. Kept in
            // the row rather than a legend, because a combo shows one line at
            // a time and a legend below it would describe nothing visible.
            snprintf(row, sizeof row, "%s%s   %s",
                     int(f) == s.suggest ? "* " : "   ",
                     s.files[f].c_str(), human_size(n).c_str());
            if (ImGui::Selectable(row, int(f) == current))
                adopt_save_file(s, int(f));
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", s.suggest_listed
            ? "* the file the console's own metadata says is the save"
            : "* the largest file - nothing here names one");

    if (!g_app.target_path.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("View / edit data")) {
            // Read fresh every time: applying codes rewrites the file, so a
            // buffer from before would be stale.
            if (hex_load(g_app.target_path)) g_app.show_hex = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Open the target file in a hex editor.\n"
                              "Edits are written only when you ask.");
    }
}

//
// The same rows by hand: any .savepatch, any target file. Shown on the
// patcher screen in advanced mode, and whenever a target arrived without a
// save behind it -- the command line, a dropped file.
//
static void draw_manual_pickers() {
    // Advanced hides the save header, so a line here keeps the answer to
    // "which save am I in" on screen rather than only in the target path.
    if (g_app.has_save) {
        ImGui::TextDisabled("from %s", g_app.save.name.empty() ? g_app.save.dir_name.c_str()
                                                               : g_app.save.name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_app.save.path.c_str());
    }
    if (ImGui::Button("Open .savepatch...")) do_open_patch();
    ImGui::SameLine();
    if (ImGui::Button("Choose target...")) do_choose_target();
    ImGui::SameLine();
    ImGui::TextUnformatted(g_app.target_path.empty()
                               ? "(script uses patch's own target)"
                               : g_app.target_path.c_str());
    if (!g_app.target_path.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("View / edit data")) {
            if (hex_load(g_app.target_path)) g_app.show_hex = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Open the target file in a hex editor.\n"
                              "Edits are written only when you ask.");
    }
}

//
// The patcher screen: one save (or one hand-picked target), its console
// encryption, its codes, and Apply.
//
static void draw_patch_screen() {
    // --- where you are, and the way back ---
    if (ImGui::Button("< Saves")) g_screen = SCREEN_SAVES;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back to the save list. Nothing here is closed or lost.");
    ImGui::SameLine();
    if (ImGui::Button("Find a game...")) g_db.want_open = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Search the bundled patch database (%d patches).",
                          patchdb_count(g_db.db));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_app.patch_path.empty() ? "(no patch loaded)"
                                                       : g_app.patch_path.c_str());
    if (!g_app.patch_raw.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("View patch file")) g_app.show_patch_raw = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show the .savepatch as text, including comments\n"
                              "and target lines that parsing leaves out.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Save patch file...")) do_save_patch();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Write this .savepatch out, with your code edits in it,\n"
                              "so they can be kept or shared. Everything the parser\n"
                              "leaves out is carried over untouched.");
    }

    ImGui::Separator();

    // --- the save, or the pickers ---
    if (g_app.has_save && !g_advanced) draw_save_header();
    else                               draw_manual_pickers();

    // The patch this save's title ID names, when it is not the one already
    // open. Never swapped in silently: the session being replaced may carry
    // edited code bodies and ticked rows, so this is a one-click offer.
    if (g_app.match_index >= 0 && g_app.patch_path != g_app.match_label) {
        const patchdb_entry_t* e = patchdb_at(g_db.db, g_app.match_index);
        ImGui::TextColored(ImVec4(0.80f, 0.80f, 0.95f, 1.0f),
                           "This save is %s%s%s",
                           g_app.match_label.c_str(),
                           (e && e->name && *e->name) ? " - " : "",
                           (e && e->name) ? e->name : "");
        ImGui::SameLine();
        if (ImGui::SmallButton(g_app.session ? "Load its patch instead"
                                             : "Load its patch"))
            load_patch_from_db(g_app.match_index);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Open this game's patch from the bundled database.%s",
                              g_app.session ? "\nThe patch open now is closed, "
                                              "along with any code edits in it." : "");
    }

    draw_psp_section();
    draw_ps3_section();

    ImGui::Checkbox("Back up target (.bak) before patching", &g_app.backup);

    draw_byte_order();

    ImGui::Spacing();
    if (g_app.session)
        ImGui::TextColored(ImVec4(0.80f,0.80f,0.95f,1.0f), "Game: %s", g_app.game_name.c_str());
    ImGui::Spacing();

    // --- code list (fills remaining space above the footer) ---
    float log_h  = ImGui::GetFontSize() * 9.0f;
    float footer = ImGui::GetFrameHeightWithSpacing()          // action row
                 + ImGui::GetFrameHeightWithSpacing()          // log toggle
                 + (g_app.show_log ? log_h + ImGui::GetStyle().ItemSpacing.y : 0.0f);
    ImGui::BeginChild("list_region", ImVec2(0, -footer));
    draw_code_list();
    ImGui::EndChild();

    // --- action row ---
    bool blocked = has_unfilled_selection();
    bool key_wait = native_blocked();
    ImGui::BeginDisabled(blocked || key_wait || !g_app.session || count_selected() == 0);
    if (ImGui::Button("Apply selected", ImVec2(140, 0))) apply_selected();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear log")) { std::lock_guard<std::mutex> lk(g_app.log_mtx); g_app.log.clear(); }
    if (blocked) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                           "Fill the required options on the highlighted codes first.");
    } else if (key_wait) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s",
                           psp_blocked()
                               ? "This PSP save needs its game key, or untick the unwrap box."
                               : "This PS3 save needs its secure file ID, or untick the unwrap box.");
    }

    // --- collapsible log ---
    if (ImGui::Button(g_app.show_log ? "- Hide log" : "+ Show log")) g_app.show_log = !g_app.show_log;
    if (g_app.show_log) {
        if (ImGui::BeginChild("log", ImVec2(0, log_h), true,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            std::lock_guard<std::mutex> lk(g_app.log_mtx);
            ImGui::TextUnformatted(g_app.log.c_str());
            if (g_app.scroll_log) { ImGui::SetScrollHereY(1.0f); g_app.scroll_log = false; }
        }
        ImGui::EndChild();
    }

    // --- apply result popup ---
    // ImGui hashes a window's whole name, so the caption is kept out of the
    // identity with ### -- see "One window, one identity" in the docs. Without
    // it, changing the title would make this a different popup and the open
    // request would be raised against the wrong one.
    if (g_app.open_apply_popup) { ImGui::OpenPopup("###result"); g_app.open_apply_popup = false; }
    if (ImGui::BeginPopupModal((g_app.apply_title + "###result").c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(g_app.apply_msg.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

}

//
// Discarding code edits, asked rather than assumed. Raised only by
// request_open_save(); see the note there for why that is the one place.
//
static void draw_discard_popup() {
    if (g_confirm_discard) {
        ImGui::OpenPopup("Unsaved code edits");
        g_confirm_discard = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved code edits", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const int n = count_edited();
    ImGui::Text("%d code%s in the patch open now %s edits that are not in any file.",
                n, n == 1 ? "" : "s", n == 1 ? "has" : "have");
    ImGui::TextDisabled("Opening another save closes this patch and the edits go with it.");
    ImGui::Spacing();

    // The file dialog opens after this frame, so this cannot then go on to
    // open the save -- and pretending otherwise would be worse than saying so.
    if (ImGui::Button("Save the patch first", ImVec2(170, 0))) {
        do_save_patch();
        ImGui::CloseCurrentPopup();
        g_pending_save_valid = false;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Write the edits out to a .savepatch.\n"
                          "Then choose the save again.");
    ImGui::SameLine();
    if (ImGui::Button("Discard and open", ImVec2(150, 0))) {
        if (g_pending_save_valid) commit_open_save(g_pending_save);
        g_pending_save_valid = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_pending_save_valid = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

//
// The window itself: a menu bar, one of the two screens, and the modals that
// belong to neither.
//
static void draw_main_window(bool* want_quit) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Apollo Save Patcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);

    draw_menu_bar(want_quit);

    scan_collect();

    if (g_screen == SCREEN_SAVES) draw_saves_screen();
    else                          draw_patch_screen();

    draw_discard_popup();
    render_db_browser();
    draw_about();

    ImGui::End();

    draw_patch_raw();
    draw_hex_editor();
}

//
// The font.
//
// ImGui's built-in ProggyClean covers U+0020-00FF and nothing else, so a game
// called "Monster Hunter Freedom Unite(TM)" drew a '?' where the symbol
// should be. That is not a rare corner: across the 16,055 names in the patch
// database, the title catalogue and a corpus of real saves there are 592
// characters outside ASCII, and 357 of them -- 60% -- are that one symbol.
// The registered sign is another 171, and those DID draw, because they fall
// inside Latin-1. Two of 176 saves are titled in kana and kanji.
//
// So the app ships a font rather than hunting for one on each platform. A
// system-font search would work, but the app would then look different on
// every machine -- and since every column width comes from CalcTextSize, even
// its proportions would move. One vendored font is one appearance and one
// tested path.
//
// $APOLLO_FONT overrides it, for somebody who wants another face.
//

//
// The font that ships with the app.
//
// Noto Sans JP, the same one the console apps use, vendored at
// gui/assets/fonts. It covers every character the databases and a corpus of
// real saves contain -- 31 of 31, Japanese included -- which no system font
// can be relied on for.
//
#define APP_FONT_NAME "NotoSansJP-Medium.otf"

//
// What to bake beyond Latin-1.
//
// Whole blocks rather than the exact characters the databases contain,
// because the saves on somebody's memory card are not in those databases and
// a name is only ever read at run time.
//
// The Japanese range is always requested, whichever font is found. That costs
// nothing when the font has no such glyphs -- measured, Arial comes out at the
// same 1015 glyphs and 0.2MB either way -- so there is no need to ask what a
// font contains before asking it for something.
//
static const ImWchar FONT_EXTRA_RANGES[] = {
    0x0100, 0x024F,   // Latin Extended-A and B
    0x0370, 0x03FF,   // Greek -- "Sigma" turns up in a title
    0x0400, 0x04FF,   // Cyrillic
    0x2000, 0x206F,   // General punctuation: curly quotes, dashes, bullet
    0x20A0, 0x20BF,   // Currency
    0x2100, 0x214F,   // Letterlike: the trade mark sign lives here
    0x2190, 0x21FF,   // Arrows
    0x2600, 0x26FF,   // Miscellaneous symbols
    0,
};

static void load_font(ImGuiIO& io) {
    // Static: ImGui keeps the pointer until the atlas is built, which happens
    // after this returns.
    static ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder builder;

    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(FONT_EXTRA_RANGES);
    builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
    builder.BuildRanges(&ranges);

    // What the user asked for, then what shipped with the app. The same two
    // places the patch database is looked for, for the same reason -- a macOS
    // .app keeps its files in Contents/Resources, everything else keeps them
    // beside the executable.
    std::vector<std::string> tries;
    const char* forced = getenv("APOLLO_FONT");
    if (forced && *forced) tries.push_back(forced);

    char dir[768];
    if (patchdb_exe_dir(dir, sizeof dir)) {
        tries.push_back(std::string(dir) + "/" APP_FONT_NAME);
        tries.push_back(std::string(dir) + "/../Resources/" APP_FONT_NAME);
    }
    tries.push_back(APP_FONT_NAME);

    char note[1024];
    for (const std::string& path : tries) {
        if (!std::ifstream(path)) continue;
        // 20px, against ProggyClean's 13. A bitmap font drawn at its design
        // size is crisp where an outline font at 13 is muddy, and this app is
        // read more than it is clicked -- game names, slots, file names.
        //
        // The one cost is the atlas, and it is a step rather than a slope:
        // the same ~4000 glyphs fit 1024x1024 up to and including 16px and
        // need 1024x2048 from 17 (1MB to 2MB of alpha texture). Having paid
        // that, 20 costs no more than 18.
        //
        // Every column width in the app comes from CalcTextSize, so the
        // layout follows the size rather than having to be retuned.
        if (io.Fonts->AddFontFromFileTTF(path.c_str(), 20.0f, nullptr, ranges.Data)) {
            snprintf(note, sizeof note, "Font: %s", path.c_str());
            g_app.append_log(note);
            return;
        }
        // Found but unusable -- a truncated file, or a format stb_truetype
        // will not read. Worth saying, because the next one along will look
        // like a silent downgrade otherwise.
        snprintf(note, sizeof note, "[!] Font %s could not be parsed; trying the next",
                 path.c_str());
        g_app.append_log(note);
    }

    // A safety net that should never fire: the font is vendored and the build
    // copies it in. Reaching here means the copy beside the app is missing or
    // unreadable, so it says so rather than quietly looking wrong.
    io.Fonts->AddFontDefault();
    g_app.append_log("[!] Font: " APP_FONT_NAME " was not found next to the application, "
                     "so the built-in one is in use and anything outside Latin-1 (the "
                     "trade mark sign, curly quotes, Japanese titles) draws as '?'. "
                     "Set $APOLLO_FONT to a .ttf or .otf, or reinstall.");
}

//
// The second font: 10x20, fixed pitch, one bit per pixel.
//
// Noto is the right font for names and prose and the wrong one for a hex
// dump. The memory editor sizes its whole grid from CalcTextSize("F").x and
// then draws every other character in that cell, which only works if the
// advances match; in Noto they run from 3.96px for 'i' to 12.35px for 'W', so
// the columns drift and the ASCII pane does not line up with the bytes.
//
// The fix is a fixed-pitch font, and the one already in the family is the
// 10x20 console font apollo-ps3, apollo-ps4 and apollo-vita draw with. At an
// exact 10 x 20 cell it matches the 20px body line height, so the hex window
// keeps the same rhythm as the rest of the app, and being a bitmap it is
// drawn rather than rasterised -- crisp at the small sizes where
// antialiasing does the most damage.
//
// ImGui has no notion of a bitmap font, so this goes in the long way round:
// a font whose own glyph range is a single character nobody draws, with the
// 95 printable ASCII glyphs added as custom atlas rectangles and their pixels
// written in by hand after the atlas is packed. A custom-rect glyph is placed
// at GlyphOffset, which defaults to (0,0) -- the top-left of the text line --
// so a full-height 20px cell lands exactly on a 20px line with no nudging.
//
// ASCII only, deliberately: see font10x20.h. That is why this font is used
// for the hex editor and the hex ID fields and nothing else -- those are hex
// digits and ASCII bytes by construction, whereas a file path, a game name or
// a savepatch comment can hold anything, and a missing glyph would draw as
// '?' all over again.
//
static void load_mono_font(ImGuiIO& io) {
    enum { GLYPHS = FONT10X20_LAST - FONT10X20_FIRST + 1 };

    // ImFontAtlas::AddFont insists on a real font source, so ProggyClean
    // supplies one -- asked for a single glyph, U+0020, which a custom rect
    // then overrides. Custom rects are registered last and the lookup table
    // takes the last glyph for a codepoint, so nothing of ProggyClean shows.
    ImFontConfig cfg;
    cfg.SizePixels = (float)FONT10X20_H;
    static const ImWchar seed[] = { 0x0020, 0x0020, 0 };
    cfg.GlyphRanges = seed;
    ImFont* font = io.Fonts->AddFontDefault(&cfg);
    if (!font) {
        g_app.append_log("[!] Font: the fixed-pitch font could not be created; "
                         "the hex editor will use the main font and its columns "
                         "will not line up.");
        return;
    }

    static int rects[GLYPHS];
    for (int i = 0; i < GLYPHS; ++i)
        rects[i] = io.Fonts->AddCustomRectFontGlyph(
            font, (ImWchar)(FONT10X20_FIRST + i),
            FONT10X20_W, FONT10X20_H, (float)FONT10X20_W);

    // Pack now rather than letting the backend trigger it, because the pixels
    // have to be written before anything reads the texture. The GL backend
    // asks for RGBA32, which converts from this same alpha buffer and only
    // rebuilds if it is absent, so the blit below is what ends up on the GPU.
    io.Fonts->Build();

    unsigned char* pixels = nullptr;
    int tex_w = 0, tex_h = 0;
    io.Fonts->GetTexDataAsAlpha8(&pixels, &tex_w, &tex_h);
    if (!pixels) return;

    for (int i = 0; i < GLYPHS; ++i) {
        const ImFontAtlasCustomRect* r = io.Fonts->GetCustomRectByIndex(rects[i]);
        if (!r->IsPacked()) continue;
        const unsigned char* src = kFont10x20 + (size_t)i * FONT10X20_H * 2;
        for (int y = 0; y < FONT10X20_H; ++y) {
            // 10 bits of the 16, most significant first.
            unsigned row  = ((unsigned)src[y * 2] << 8) | src[y * 2 + 1];
            unsigned char* dst = pixels + (size_t)(r->Y + y) * tex_w + r->X;
            for (int x = 0; x < FONT10X20_W; ++x)
                dst[x] = (row >> (15 - x)) & 1 ? 255 : 0;
        }
    }

    g_mono = font;
    char note[128];
    snprintf(note, sizeof note, "Font: %dx%d console raster, %d glyphs (fixed pitch)",
             FONT10X20_W, FONT10X20_H, GLYPHS);
    g_app.append_log(note);
}

static void apply_style() {
    // Cosmetic rounding only — no size scaling, so everything stays at
    // ImGui's default dimensions.
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 6.0f;
    s.FrameRounding     = 4.0f;
    s.GrabRounding      = 4.0f;
    s.ScrollbarRounding = 4.0f;

    // How long the pointer has to rest on a save before its details appear.
    // A shade above ImGui's 0.40s default for DelayNormal, which is tuned for
    // a one-line hint where this is a panel with a picture in it. The only
    // thing in the app that asks for DelayNormal is that tooltip, so this is
    // its setting in all but name.
    //
    // What made the difference was not the length: the call site pairs this
    // with ImGuiHoveredFlags_NoSharedDelay, so the wait applies to every row
    // rather than only the first of a run. With the timer shared, any delay
    // feels absent after the first panel.
    s.HoverDelayNormal  = 0.5f;
}

// Title-bar / taskbar icon for Windows & Linux. On macOS glfwSetWindowIcon is
// unsupported (the Dock icon comes from the .app bundle's .icns instead).
static void set_window_icon(GLFWwindow* window) {
#ifndef __APPLE__
    std::vector<unsigned char> rgba(apollo_icon_rgba_size);
    uLongf out = apollo_icon_rgba_size;
    if (uncompress(rgba.data(), &out, apollo_icon_rgba_z, apollo_icon_rgba_z_len) == Z_OK
        && out == apollo_icon_rgba_size) {
        GLFWimage img;
        img.width  = apollo_icon_w;
        img.height = apollo_icon_h;
        img.pixels = rgba.data();          // GLFW copies the pixels
        glfwSetWindowIcon(window, 1, &img);
    }
#else
    (void)window;
#endif
}

// Last message from GLFW, shown to the user if startup fails (no console with
// -mwindows, so failures would otherwise be silent).
static std::string g_glfw_error;
static void glfw_error_cb(int code, const char* desc) {
    char buf[512];
    snprintf(buf, sizeof buf, "GLFW error %d: %s", code, desc ? desc : "(unknown)");
    g_glfw_error = buf;
    fprintf(stderr, "%s\n", buf);
}
static void fatal(const std::string& msg) {
#ifdef _WIN32
    MessageBoxA(nullptr, msg.c_str(), "Apollo Save Patcher — startup error", MB_ICONERROR | MB_OK);
#else
    fprintf(stderr, "%s\n", msg.c_str());
#endif
}

// Files dragged onto the window. GLFW delivers these on the main thread from
// glfwPollEvents(), so touching app state here is safe.
//
// One of three routes into open_path(), which is the point: the command line
// covers a terminal, a script and a Windows/Linux file association, this
// covers the window, and macos_open_docs.mm covers Finder on a Mac -- where a
// .app is handed its documents through Apple Events rather than argv.
static void drop_cb(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; i++)
        if (paths[i] && *paths[i]) open_path(paths[i]);
}

static void print_usage(const char* argv0) {
    fprintf(stderr,
        "Apollo Save Patcher\n"
        "\n"
        "  %s [FILE...]\n"
        "  %s --scan DIR [N...]\n"
        "  %s --open PATH\n"
        "\n"
        "A .savepatch is opened as the patch; anything else is opened as the\n"
        "save to patch, which also looks up its game key and its patch in the\n"
        "bundled database. Files can be dropped on the window instead.\n"
        "\n"
        "--scan prints the saves under DIR and exits, without opening a window:\n"
        "the same walk the saves screen does, for checking what a folder holds\n"
        "from a terminal or a script. Add save numbers to have them opened in\n"
        "turn, each reporting the target, the patch and the encryption layer --\n"
        "everything the patcher screen would be showing had you clicked it.\n"
        "\n"
        "--open takes one path the way a dropped file does -- a save folder, a\n"
        "loose save file or a .savepatch -- and reports the same, also without a\n"
        "window.\n"
        "\n"
        "  $APOLLO_PATCHES_ZIP   where to find apollo-patches.zip\n",
        argv0 && *argv0 ? argv0 : "apollo_patcher_gui",
        argv0 && *argv0 ? argv0 : "apollo_patcher_gui",
        argv0 && *argv0 ? argv0 : "apollo_patcher_gui");
}

//
// The save scan on its own, with no window. This is the browser's whole brain
// -- the same scan_start/scan_collect pair the saves screen drives -- so what
// it prints is exactly what the list would show, which is the only way to
// check the walk without a person looking at it.
//
// A column-width string for --scan's table. Truncating at a byte boundary
// would cut a multi-byte character in half and emit invalid UTF-8 -- these are
// real game names, and plenty of them carry a (TM) or are Japanese outright.
static std::string clip(const std::string& in, size_t width) {
    size_t n = in.size();

    if (n > width) {
        n = width;
        while (n > 0 && (unsigned char)in[n] >= 0x80 && (unsigned char)in[n] < 0xC0)
            n--;   // back off onto the start of the character that got cut
    }
    std::string out = in.substr(0, n);
    // Padded here rather than by printf, whose width counts bytes and so would
    // undo the point of the above.
    size_t cols = 0;
    for (char c : out)
        if (((unsigned char)c & 0xC0) != 0x80) cols++;
    out.append(width > cols ? width - cols : 0, ' ');
    return out;
}

// What the patcher screen would be showing. Shared by --scan and --open so
// both report the same thing about the same state.
static void print_open_state() {
    printf("  screen      %s\n", g_screen == SCREEN_PATCH ? "patcher" : "saves");
    printf("  save        %s\n", g_app.has_save
                                      ? (g_app.save.name.empty() ? g_app.save.dir_name.c_str()
                                                                 : g_app.save.name.c_str())
                                      : "(none - target picked by hand)");
    printf("  files       %d\n", g_app.has_save ? int(g_app.save.files.size()) : 0);
    //
    // A PS1 or PS2 save has no icon FILE -- there is one, but it has to be
    // built from the save itself, so "is there a path" is the wrong question.
    //
    // Actually built here rather than assumed, because one of the answers is
    // "this save is damaged" and a diagnostic that cannot report that is not
    // worth printing. It costs one parse of one .ico, on a path that already
    // re-reads the whole container.
    //
    if (!g_app.has_save) {
        printf("  icon        none\n");
    } else if (!g_app.save.container) {
        printf("  icon        %s\n", g_app.save.icon.empty() ? "none" : "yes");
    } else {
        printf("  icon        %s\n", describe_container_icon(g_app.save));
    }
    printf("  target      %s\n", g_app.target_path.empty() ? "(none)"
                                                            : g_app.target_path.c_str());
    printf("  title hint  %s\n", g_app.title_hint.empty() ? "(none)"
                                                           : g_app.title_hint.c_str());
    printf("  patch       %s\n", g_app.patch_path.empty() ? "(none)"
                                                           : g_app.patch_path.c_str());
    printf("  codes       %d\n", g_app.session ? apctl_code_count(g_app.session) : 0);
    printf("  byte order  %s\n", effective_big_endian() ? "big-endian" : "little-endian");
    if (g_app.psp.found)
        printf("  PSP layer   %s, key %s (%s)\n", g_app.psp.listed.c_str(),
               g_app.psp.have_key ? "found" : "MISSING", g_app.psp.key_note.c_str());
    if (g_app.ps3.found)
        printf("  PS3 layer   %s, key %s (%s)\n", g_app.ps3.listed.c_str(),
               g_app.ps3.have_key ? "found" : "MISSING", g_app.ps3.key_note.c_str());
    // The save's own reading, which the scan takes for every PS3 save. The
    // ps3_detect copy is a fallback for a target picked by hand, where there
    // is no scanned save behind it.
    {
        const std::string acct = g_app.has_save && !g_app.save.account.empty()
                               ? g_app.save.account
                               : std::string(g_app.ps3.account);
        if (!acct.empty())
            printf("  account     %s%s\n", acct.c_str(),
                   account_verdict(acct) > 0 ? "  (yours)"
                   : account_verdict(acct) < 0 ? "  (not yours)" : "");
    }
    if (!g_app.psp.found && !g_app.ps3.found)
        printf("  no console encryption layer\n");
}

static int run_scan(const char* root, const std::vector<int>& picks) {
    if (!is_dir(root)) {
        fprintf(stderr, "%s is not a folder\n", root);
        return 2;
    }

    scan_start(root);
    while (g_sb.running.load() && !g_sb.done.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    scan_collect();

    for (size_t i = 0; i < g_sb.saves.size(); i++) {
        const SaveEntry& s = g_sb.saves[i];
        printf("%3zu  %-3s  %-9s  %s  %s  %s\n",
               i, s.platform,
               s.title_id.empty() ? "-" : s.title_id.c_str(),
               clip(s.name.empty() ? "(unnamed)" : s.name, 34).c_str(),
               clip(s.detail.empty() ? "-" : s.detail, 16).c_str(),
               s.dir_name.c_str());
        for (size_t f = 0; f < s.files.size(); f++)
            printf("          %s%s\n", int(f) == s.suggest ? "* " : "  ", s.files[f].c_str());
        printf("          %s\n", s.patch_index >= 0
                                     ? ("codes: " + s.patch_name).c_str()
                                     : "no codes");
        if (!s.icon.empty())
            printf("          icon: %s\n",
                   s.icon.c_str() + (s.icon.size() > s.path.size() ? s.path.size() + 1 : 0));
        if (!s.account.empty()) {
            const int mine = account_verdict(s.account);
            printf("          account: %s%s\n", s.account.c_str(),
                   mine > 0 ? "  (yours)" : mine < 0 ? "  (not yours)" : "");
        }
    }
    printf("\n%s\n", g_sb.note.c_str());

    if (picks.empty())
        return g_sb.saves.empty() ? 1 : 0;

    //
    // ...and what happens when they are chosen, IN ORDER. Several rather than
    // one because opening saves in sequence is where state leaks between
    // them: a save with no codes of its own inheriting the previous save's,
    // or its byte order, or a patch its own title ID never matched.
    //
    for (int pick : picks) {
        if (pick < 0 || pick >= int(g_sb.saves.size())) {
            fprintf(stderr, "no save %d in that folder\n", pick);
            return 2;
        }
        // commit_open_save, not open_save_file: the same call the list makes,
        // so what this prints is what the patcher screen would be showing.
        commit_open_save(g_sb.saves[size_t(pick)]);

        printf("\nopening save %d\n", pick);
        print_open_state();
    }
    return 0;
}

//
// Open one path the way a dropped file or a command-line argument does, and
// report where it landed. The folder case is the interesting one: it goes
// through the save browser's own identification, so a dropped save folder is
// the same thing as one picked from the list.
//
static int run_open(const char *path) {
    open_path(path);
    printf("opening %s\n", path);
    print_open_state();

    // open_path's complaints go to the log panel, which there is none of here.
    // Printed rather than dropped: "nothing happened" with no reason is the
    // least useful thing a diagnostic can say.
    {
        std::lock_guard<std::mutex> lk(g_app.log_mtx);
        if (!g_app.log.empty()) printf("\n%s", g_app.log.c_str());
    }
    return (g_app.session || !g_app.target_path.empty()) ? 0 : 1;
}

//
// The window's starting size.
//
// Wide because the save list is the way into the app, and because a modal
// cannot be wider than the window around it (see size_modal) -- so the
// window's width is what decides how much of a game's name and slot a row can
// show.
//
// Clamped to the monitor, because a default that does not fit is worse than a
// small one: a window taller than the screen hides its own buttons behind the
// taskbar, and there is no reason to assume a 1080p desktop. glfwGetMonitor-
// Workarea already excludes the taskbar and the macOS menu bar.
//
static void default_window_size(int* w, int* h) {
    *w = 1180;
    *h = 820;

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (!monitor) return;   // a headless or unusual setup; take the numbers above

    int mx = 0, my = 0, mw = 0, mh = 0;
    glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
    if (mw > 0 && *w > mw - 80) *w = mw - 80;
    if (mh > 0 && *h > mh - 80) *h = mh - 80;

    // ...but not so small that the app is unusable on a tiny display. Below
    // this the window is scrollable rather than cropped, which is the better
    // failure.
    if (*w < 720) *w = 720;
    if (*h < 540) *h = 540;
}

int main(int argc, char** argv) {
    apctl_set_log_sink(log_sink, &g_app);
    init_patchdb();

    // Opened before the window exists: everything here only touches app state
    // and the log, which the panel picks up when it first draws.
    //
    // In the order given, so an explicit patch beats the one a save's title ID
    // Settings first, because everything below this line depends on them: the
    // diagnostics report whether a save is yours and whether its console
    // layer can be opened, and a file named on the command line or handed
    // over by Finder is opened right here. Loading them afterwards -- which
    // is what used to happen -- meant all of that ran against a blank
    // configuration and answered differently from the same save opened a
    // second later through the browser.
    //
    // Nothing in here touches GLFW or ImGui, so it is safe this early.
    settings_load();

    // would have auto-loaded, whichever way round they are written.
    for (int i = 1; i < argc; i++) {
        if (!argv[i] || !*argv[i]) continue;
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        // macOS hands a Finder-launched .app a -psn_... process serial number.
        // It is not a file and predates modern launch services; skip it rather
        // than report it as an unreadable save.
        if (strncmp(argv[i], "-psn_", 5) == 0) continue;
        if (strcmp(argv[i], "--scan") == 0) {
            if (i + 1 >= argc) { print_usage(argv[0]); return 2; }
            std::vector<int> picks;
            for (int j = i + 2; j < argc; j++) picks.push_back(atoi(argv[j]));
            return run_scan(argv[i + 1], picks);
        }
        if (strcmp(argv[i], "--open") == 0) {
            if (i + 1 >= argc) { print_usage(argv[0]); return 2; }
            return run_open(argv[i + 1]);
        }
        open_path(argv[i]);
    }

#ifdef _WIN32
    //
    // The app ships a Mesa software opengl32.dll in a "softgl" subfolder. If
    // the user copies it next to the .exe, GLFW loads it (the exe's own
    // directory is searched first) and this picks its renderer. Ignored when
    // the system GPU driver is used.
    //
    // llvmpipe is the fast one, and the default for that reason -- but it
    // JITs with LLVM for whatever CPU it finds, and that is exactly what
    // fails on some hosts: a JIT that enables AVX on a Windows 7 install
    // whose OS support for it is missing produces STATUS_ILLEGAL_INSTRUCTION
    // (0xC000001D) the moment the first generated code runs, which looks like
    // the app refusing to start. softpipe has no JIT at all and is the escape
    // hatch; it is far slower, which for a static 2D interface matters less
    // than starting.
    //
    // So this only ever SUGGESTS: an existing setting is left alone, because
    // somebody setting it is somebody working around this.
    //
    if (!getenv("GALLIUM_DRIVER"))
        _putenv_s("GALLIUM_DRIVER", "llvmpipe");
#endif

#ifdef __APPLE__
    // BEFORE glfwInit, which calls [NSApp run] itself and so lets AppKit
    // finish launching -- and finish handling the document a double-click
    // launched us with. The hook has to exist by then or that first file is
    // answered with "cannot open files in the ... format" before the app has
    // drawn a frame. See macos_open_docs.mm.
    //
    // open_path is the same entry point the drop callback and the command
    // line use, so all three behave identically: a save folder arrives
    // identified, a .savepatch loads, and either way the patcher screen
    // comes up.
    const auto macos_open = [](const char* path) {
        if (path && *path) open_path(path);
    };
    const bool macos_hooked = apollo_macos_watch_open_documents(macos_open) != 0;
#endif

    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) { fatal("Failed to initialize GLFW.\n\n" + g_glfw_error); return 1; }

#ifdef __APPLE__
    // Again, now that the delegate is a real object: the call above finds the
    // class by name, and this covers that name ever changing.
    if (!apollo_macos_watch_open_documents(macos_open) && !macos_hooked)
        g_app.append_log("[!] Files opened from Finder will not reach the app "
                         "(the application delegate could not be hooked). "
                         "Dropping them on the window still works.");
#endif
    int win_w = 0, win_h = 0;
    default_window_size(&win_w, &win_h);   // needs glfwInit, for the monitor

    // No context hints: GLFW's default legacy/compatibility context is what the
    // fixed-function opengl2 backend needs, on every platform.
    GLFWwindow* window = glfwCreateWindow(win_w, win_h, "Apollo Save Patcher", nullptr, nullptr);
    if (!window) {
        fatal("Could not create an OpenGL context.\n\n"
              "This machine's graphics driver may not support OpenGL — this is "
              "common over Remote Desktop and in some virtual machines.\n\n"
              "Fix: copy opengl32.dll from the \"softgl\" folder (shipped next to "
              "this app) into the same folder as the .exe, then relaunch. See the "
              "README for details.\n\n" + g_glfw_error);
        glfwTerminate();
        return 1;
    }
    g_window = window;
    set_window_icon(window);   // Windows/Linux title-bar & taskbar icon
    glfwSetDropCallback(window, drop_cb);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // don't litter the CWD with imgui.ini

    load_font(io);        // the vendored Noto Sans JP; see APP_FONT_NAME
    load_mono_font(io);   // the 10x20 console raster font, for the hex editor

    ImGui::StyleColorsDark();
    apply_style();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();
    load_type_icons();   // needs the GL context, so not with the fonts above

    // The folder from last time, scanned in the background while the window
    // comes up. Costs nothing when there is none, and means the saves screen
    // shows a list rather than an empty one on every launch.
    if (!g_saved.saves_root.empty() && is_dir(g_saved.saves_root))
        scan_start(g_saved.saves_root);

    bool want_quit = false;
    while (!glfwWindowShouldClose(window) && !want_quit) {
        glfwPollEvents();
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        // keyboard shortcuts
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_O, false)) do_open_patch();
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_S, false) &&
            g_app.session) do_save_patch();
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_B, false)) g_screen = SCREEN_SAVES;
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_P, false)) g_screen = SCREEN_PATCH;
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_F, false)) g_db.want_open = true;
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_Q, false)) want_quit = true;

        draw_main_window(&want_quit);
        draw_code_viewers();
        draw_settings_window();

        ImGui::Render();
        int w, h; glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        // Open any requested native dialog now — outside the ImGui frame.
        process_pending_dialogs();
    }

    // Stop the save scan before anything it touches goes away. cancel makes
    // the walk return at its next directory rather than at the end of the
    // disk, so quitting mid-scan closes the window now and not in a minute.
    g_sb.cancel = true;
    scan_join();
    icon_drop();               // while there is still a GL context to drop it in

    g_app.close();
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
