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
#include "kirk_engine.h"   // KIRK_HOST_FUSE_ID, the Fuse ID default

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>    // MessageBox for visible startup errors (no console with -mwindows)
#include <shellapi.h>   // ShellExecuteA for opening links; WIN32_LEAN_AND_MEAN excludes it
#endif

// Window icon (Windows/Linux only). Kept fully inside the guard so macOS pulls
// in neither zlib nor the icon data.
#ifndef __APPLE__
#include <zlib.h>
#include "icon_rgba_z.h"   // 256x256 RGBA, zlib-deflated (inflated at startup)
#endif

static GLFWwindow* g_window = nullptr;   // for native dialog parenting
static float       g_ui_scale = 1.0f;    // HiDPI content scale (column widths)

// ---- shared UI state -------------------------------------------------------
struct AppState {
    apctl_session_t*   session = nullptr;
    std::string         patch_path;
    std::string         target_path;
    std::string         game_name;
    std::string         patch_raw;        // the .savepatch as text (CR stripped)
    std::string         patch_bytes;      // ...and verbatim, for saving it back
    bool                show_patch_raw = false;

    std::vector<unsigned char> hex_data;   // the target file, for the hex editor
    std::string         hex_path;          // which file hex_data came from
    bool                show_hex = false;
    bool                hex_dirty = false; // edits not yet written to disk
    std::vector<char>   selected;         // per-row checkbox
    std::vector<char>   viewer_open;      // per-row code window open flag
    // Editable copy of a code body, one per row. `loaded` keeps unsaved typing
    // alive across closing and reopening the window; only opening a row for
    // the first time (or saving/reverting) pulls the engine's text in.
    struct CodeEdit {
        std::string text;
        bool        loaded = false;
        bool        raise  = false;   // bring the window forward next frame
    };
    std::vector<CodeEdit> viewer_buf;
    std::string         log;
    std::mutex          log_mtx;
    bool                backup = true;    // copy target -> target.bak before patching

    // What apctl_is_big_endian_for() said about the patch that is open, and
    // whether anything is open for it to have said it about. The value the
    // engine actually runs with is effective_big_endian(), which is this
    // unless Settings overrides it -- keeping the two apart is what lets the
    // main window say "forced, and it disagrees with this patch".
    bool                be_detected = false;
    std::string         be_platform;      // "PS3", "PS4", ... "" for a loose file
    bool                scroll_log = false;
    bool                show_log = false; // log pane collapsed by default
    bool                open_apply_popup = false;
    std::string         apply_msg;

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

        void clear() { *this = Ps3(); }
    };
    Ps3 ps3;

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
        patch_bytes.clear();
        show_patch_raw = false;
    }
};
static AppState g_app;

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
    // detection separate is what lets the main window say a forced one
    // disagrees with the patch in front of it.
    g_app.be_detected = apctl_is_big_endian_for(platform, label.c_str()) != 0;
    g_app.be_platform = platform ? platform : "";
    g_app.append_log(g_app.be_detected ? "PS3 title detected - big-endian save data"
                                       : "Non-PS3 title - little-endian save data");
    apctl_set_big_endian(effective_big_endian() ? 1 : 0);
}

// ---- about box -------------------------------------------------------------

#define APP_NAME      "Apollo Save Patcher"
#define URL_PATCHER   "https://github.com/bucanero/apollo-patcher"
#define URL_LIB       "https://github.com/bucanero/apollo-lib"
#define URL_PATCHES   "https://github.com/bucanero/apollo-patches"

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

    ImGui::TextUnformatted(APP_NAME);
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
//                        4 and 6 derive from the console's own fuse
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
static bool psp_unwrap_target() {
    std::vector<unsigned char> sfo, enc;
    if (!read_all(g_app.psp.sfo_path, sfo) || !read_all(g_app.target_path, enc)) {
        g_app.append_log("[!] PSP: could not read the save or its PARAM.SFO");
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
    if (!write_all(g_app.target_path, out.data(), got)) {
        g_app.append_log("[!] PSP: could not write the decrypted save");
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof msg, "PSP layer removed: %zu -> %zu bytes", enc.size(), got);
    g_app.append_log(msg);
    return true;
}

// ...and put it back, rewriting PARAM.SFO with it. Both files are written or
// neither is: a save whose PARAM.SFO does not match its data does not load, so
// a half-done wrap is worse than none.
static bool psp_wrap_target() {
    std::vector<unsigned char> sfo, plain;
    if (!read_all(g_app.psp.sfo_path, sfo) || !read_all(g_app.target_path, plain)) {
        g_app.append_log("[!] PSP: could not read the save or its PARAM.SFO");
        return false;
    }

    std::vector<unsigned char> out(apsp_encrypted_size(plain.size()));
    size_t got = 0;
    int rc = apsp_encrypt(sfo.data(), sfo.size(), g_app.psp.listed.c_str(),
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
    if (!write_all(g_app.target_path, out.data(), got)) {
        g_app.append_log("[!] PSP: could not write the encrypted save");
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof msg, "PSP layer restored: %zu -> %zu bytes, PARAM.SFO rewritten",
             plain.size(), got);
    g_app.append_log(msg);
    return true;
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
static bool ps3_unwrap_target() {
    std::vector<unsigned char> pfd, enc;
    if (!read_all(g_app.ps3.pfd_path, pfd) || !read_all(g_app.target_path, enc)) {
        g_app.append_log("[!] PS3: could not read the save or its PARAM.PFD");
        return false;
    }

    long long want = apfd_decrypted_size(pfd.data(), pfd.size(), g_app.ps3.listed.c_str());
    if (want < 0) {
        g_app.append_log((std::string("[!] PS3 decrypt failed: ")
                          + apfd_strerror((int)want)).c_str());
        return false;
    }

    std::vector<unsigned char> out((size_t)want ? (size_t)want : 1);
    size_t got = 0;
    int rc = apfd_decrypt(pfd.data(), pfd.size(), g_app.ps3.listed.c_str(),
                          enc.data(), enc.size(), g_app.ps3.sfid,
                          out.data(), out.size(), &got);
    if (rc != APFD_OK) {
        g_app.append_log((std::string("[!] PS3 decrypt failed: ") + apfd_strerror(rc)).c_str());
        return false;
    }
    if (!write_all(g_app.target_path, out.data(), got)) {
        g_app.append_log("[!] PS3: could not write the decrypted save");
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof msg, "PS3 layer removed: %zu -> %zu bytes", enc.size(), got);
    g_app.append_log(msg);
    return true;
}

// ...and put it back, rewriting PARAM.PFD with it. Both files are written or
// neither is: a save whose PARAM.PFD does not match its data does not load, so
// a half-done wrap is worse than none.
static bool ps3_wrap_target() {
    std::vector<unsigned char> pfd, plain;
    if (!read_all(g_app.ps3.pfd_path, pfd) || !read_all(g_app.target_path, plain)) {
        g_app.append_log("[!] PS3: could not read the save or its PARAM.PFD");
        return false;
    }

    std::vector<unsigned char> out(apfd_encrypted_size(plain.size()));
    size_t got = 0;
    int rc = apfd_encrypt(pfd.data(), pfd.size(), g_app.ps3.listed.c_str(),
                          plain.data(), plain.size(), g_app.ps3.sfid,
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
    if (!write_all(g_app.target_path, out.data(), got)) {
        g_app.append_log("[!] PS3: could not write the encrypted save");
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof msg, "PS3 layer restored: %zu -> %zu bytes, PARAM.PFD rewritten",
             plain.size(), got);
    g_app.append_log(msg);
    return true;
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
// PARAM.SFO is read, not written. Its own account fields are a separate
// binding, and this does not touch them.
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

//
// Take `path` as the save to patch, and work out everything that follows from
// it. Shared by the "Choose target..." dialog, the command line and files
// dropped on the window, so all three behave identically.
//
static void adopt_target(const std::string& path, const std::string& title_hint = "") {
    g_app.target_path = path;
    g_app.title_hint  = title_hint;

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
    // A FOLDER, which for a PSP save is the obvious thing to drag: the whole
    // directory off the Memory Stick. Its PARAM.SFO already says which files
    // the console encrypted, and the first of those is what a patch is about,
    // so take that as the target. Anything else about the folder — the key,
    // the patch — follows from it as usual.
    //
    // The directory itself must never become the target: nothing can read it,
    // and the failure would surface much later with no explanation.
    if (is_dir(path)) {
        std::vector<unsigned char> meta;
        char name[APFD_NAME_LEN] = "";

        // A PSP save folder: PARAM.SFO names the files the console encrypted.
        if (read_all(path + "/PARAM.SFO", meta) &&
            apsp_sfo_valid(meta.data(), meta.size()) == APSP_OK &&
            apsp_sfo_file_count(meta.data(), meta.size()) > 0 &&
            apsp_sfo_file_name(meta.data(), meta.size(), 0, name, sizeof name) == APSP_OK) {
            adopt_target(path + "/" + name);
            return;
        }

        // A PS3 one: PARAM.PFD's entry table does. PARAM.SFO is listed there
        // too and is never encrypted, so it is skipped -- the first entry with
        // a key of its own is what a patch is about.
        if (read_all(path + "/PARAM.PFD", meta) &&
            apfd_valid(meta.data(), meta.size()) == APFD_OK) {
            const int n = apfd_entry_count(meta.data(), meta.size());
            for (int i = 0; i < n; i++) {
                if (apfd_entry_name(meta.data(), meta.size(), i, name, sizeof name) != APFD_OK)
                    continue;
                if (apfd_entry_has_builtin_key(name)) continue;
                adopt_target(path + "/" + name);
                return;
            }
        }

        g_app.append_log(("[!] " + path + " is a folder, and not a save one "
                          "(no PARAM.SFO or PARAM.PFD listing an encrypted file). "
                          "Pick the save file itself.").c_str());
        return;
    }

    const size_t dot = path.find_last_of('.');
    const std::string ext = dot == std::string::npos ? std::string()
                                                     : lowered(path.substr(dot));
    if (ext == ".savepatch")
        load_patch(path);
    else
        adopt_target(path);
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
namespace fs = std::filesystem;

struct SaveEntry {
    std::string path;        // the save folder, no trailing separator
    std::string dir_name;    // its last component, which is what is on disk
    std::string name;        // what the console calls the game
    std::string detail;      // ...and the slot: "AUTOSAVE", "Slot 1"
    std::string title_id;
    const char* platform = "?";
    bool        encrypted = false;   // a console layer sits under the patch
    int         patch_index = -1;    // into the database, or -1 for no codes
    std::string patch_name;          // the database's name for the game

    std::vector<std::string> files;  // relative to path, metadata excluded
    int         suggest = -1;        // the one to open, or -1 for no data file
    bool        suggest_listed = false;  // ...and whether the console named it
    std::string haystack;            // lowercased, for the filter box
};

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
    int                      platform = 0;  // 0 = all, else index into names
    bool                     only_coded = false;
    bool                     refilter = true;
    bool                     want_open = false;
    int                      selected = -1; // index into saves
    int                      pick = -1;     // index into saves[selected].files
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

// How deep to go looking, and how much to look at. A PS3's savedata sits five
// levels down (dev_hdd0/home/00000001/savedata/<save>), so eight is generous;
// the directory cap is only there so that pointing this at a whole disk stops
// rather than running for minutes.
static const int  SCAN_MAX_DEPTH = 8;
static const int  SCAN_MAX_DIRS  = 40000;

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
    out.platform  = asave_platform_name(info.platform);
    out.encrypted = info.encrypted != 0;

    collect_files(dir, dir, 2, out.files);
    std::sort(out.files.begin(), out.files.end());
    out.suggest = suggest_file(out, sfo, pfd);
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
        if (!e.is_directory(ec) || e.is_symlink(ec)) continue;
        const std::string leaf = e.path().filename().string();
        if (leaf.empty() || leaf[0] == '.') continue;
        scan_walk(e.path(), depth - 1, out);
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
        // A Vita save has no name of its own (see saveinfo.h), so the
        // database is where its name comes from when there is a patch.
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
    g_sb.pick     = -1;
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
    g_sb.pick     = -1;

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

static void refilter_saves() {
    g_sb.hits.clear();
    const std::string needle = lowered(g_sb.search);
    const char* platform = (g_sb.platform > 0
                            && size_t(g_sb.platform) < g_db.platforms.size())
                         ? g_db.platforms[size_t(g_sb.platform)].c_str() : nullptr;

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

static void open_save_file(const SaveEntry& save, int file_index) {
    if (file_index < 0 || file_index >= int(save.files.size())) return;

    const std::string name = save.files[size_t(file_index)];
    adopt_target((fs::path(save.path) / name).string(), save.title_id);

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
    adopt_target((fs::path(save.path) / want).string(), save.title_id);
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
    free(data);

    if (!s) { g_app.append_log("[!] Could not parse that patch"); return; }

    // The index's name is preferred: it was decoded at build time, where the
    // 245 Windows-1252 patch files (game names with (TM)/(R)) are handled.
    adopt_session(s, label, e->name, e->platform);
}

static void apply_selected() {
    if (!g_app.session) return;
    const char* target = g_app.target_path.empty() ? nullptr : g_app.target_path.c_str();

    if (g_app.backup && target) {
        if (backup_file(g_app.target_path))
            g_app.append_log(("Backup written: " + g_app.target_path + ".bak").c_str());
        else
            g_app.append_log("[!] Backup failed (target unreadable?) — aborting.");
        if (!std::ifstream(g_app.target_path + ".bak")) {
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

    // Result pop-up message.
    char msg[320];
    if (errors == 0) {
        char tail[160] = "";
        if (native)
            snprintf(tail, sizeof tail,
                     "\nThe %s layer was taken off and put back, and %s was "
                     "rewritten with it.", console, meta);
        snprintf(msg, sizeof msg, "All done — %d code(s) applied successfully.%s",
                 applied, tail);
    } else if (native && !rewrapped) {
        g_app.show_log = true;
        snprintf(msg, sizeof msg,
                 "The %s layer could not be put back, so the save on disk is "
                 "DECRYPTED.\nCheck the log, then use \"Re-encrypt\" below once the "
                 "cause is fixed.", console);
    } else {
        g_app.show_log = true;   // reveal the log so the user can inspect
        snprintf(msg, sizeof msg, "%d of %d code(s) failed to apply.\nCheck the log for details.",
                 errors, applied);
    }
    g_app.apply_msg = msg;
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
// [PYTHON:...]) and survives, but only one prefix fits per title, so a code
// already marked [DEFAULT:...] or [INFO:...] has no room to state one.
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
                 "[!] %d code(s) will read back differently from that file - a "
                 "title can carry only one marker, so a code that is already "
                 "[DEFAULT:...] or [INFO:...] cannot also state its type:", miss);
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
        if (!p.empty()) adopt_target(p);
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
                            g_app.viewer_buf[i].text = body ? body : "";
                            g_app.viewer_buf[i].loaded = true;
                        }
                    }
                    // A window already open may be behind another one.
                    g_app.viewer_buf[i].raise = true;
                }
            }

            // --- col 2: type badge ---
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(type_color(c->type), "%s", type_tag(c->type));

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

    char title[256];
    snprintf(title, sizeof title, "Patch file: %s##rawpatch",
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
        ImGui::TextUnformatted(g_app.patch_raw.c_str());
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

    char title[512];
    snprintf(title, sizeof title, "Save data: %s%s##hexedit",
             base_name(g_app.hex_path), g_app.hex_dirty ? " *" : "");

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(700, 520), ImGuiCond_FirstUseEver);
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
        if (!g_app.hex_data.empty())
            ed.DrawContents(g_app.hex_data.data(), g_app.hex_data.size());
        else
            ImGui::TextDisabled("(empty file)");
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

        char title[192];
        snprintf(title, sizeof title, "Code: %s%s##viewer%d",
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
                buf.text = apctl_code_text(c);
            }
            if (!unsaved) ImGui::EndDisabled();

            ImGui::SameLine();
            if (!edited && !unsaved) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Revert to file")) {
                apctl_revert_code(c);
                buf.text = apctl_code_text(c);
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
            ImGui::InputTextMultiline("##body", &buf.text, ImVec2(-FLT_MIN, -FLT_MIN),
                                      ImGuiInputTextFlags_AllowTabInput);
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
        if (p) ImGui::SameLine();
        if (ImGui::RadioButton(g_db.platforms[p].c_str(), g_db.platform == int(p))) {
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
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

//
// The save browser's window: the list on the left, the chosen save on the
// right. Two panes rather than two steps, because the question "which of
// these files is the save" only makes sense next to the answer to "which save
// is this", and a second modal on top of the first would hide it.
//
static void render_save_browser() {
    if (g_sb.want_open) {
        g_sb.want_open = false;
        g_sb.refilter = true;
        ImGui::OpenPopup("Saves");
    }

    size_modal(920, 600);
    if (!ImGui::BeginPopupModal("Saves", nullptr, ImGuiWindowFlags_NoSavedSettings))
        return;

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

    // --- filters ---
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##savesearch", "Game, title ID or folder...",
                                 g_sb.search, sizeof g_sb.search))
        g_sb.refilter = true;

    // Six consoles and a checkbox on the line below the search box, wrapped
    // rather than laid out with SameLine alone: the modal is only as wide as
    // the application window (see size_modal), so on a narrow one the tail of
    // a fixed row would simply be off the edge with no way to reach it.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float right = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
    auto same_line_if_it_fits = [&](const char* next) {
        const float w = ImGui::CalcTextSize(next).x + ImGui::GetFrameHeight()
                      + st.ItemInnerSpacing.x;
        if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + w < right)
            ImGui::SameLine();
    };

    for (size_t p = 0; p < g_db.platforms.size(); ++p) {
        if (p) same_line_if_it_fits(g_db.platforms[p].c_str());
        if (ImGui::RadioButton(g_db.platforms[p].c_str(), g_sb.platform == int(p))) {
            g_sb.platform = int(p);
            g_sb.refilter = true;
        }
    }
    if (!g_db.platforms.empty()) same_line_if_it_fits("Only saves with codes");
    if (ImGui::Checkbox("Only saves with codes", &g_sb.only_coded))
        g_sb.refilter = true;

    if (g_sb.refilter) refilter_saves();

    // --- the two panes ---
    const float footer = ImGui::GetFrameHeightWithSpacing() + st.ItemSpacing.y;
    // 55% to the list, except that the pane beside it has a file list and two
    // paragraphs in it and stops being readable below about 300px. On a
    // window wide enough the first rule wins; on a narrow one the second
    // does, down to a floor that keeps the list itself usable.
    const float avail  = ImGui::GetContentRegionAvail().x;
    const float detail = 300.0f * g_ui_scale;
    const float list_w = std::max(avail * 0.38f, std::min(avail * 0.55f, avail - detail));
    int         open_now = -1;   // index into the selected save's files

    if (ImGui::BeginTable("##saverows", 4,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                          ImVec2(list_w, -footer))) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Console", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Console ").x);
        ImGui::TableSetupColumn("Title ID", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("NPUB31842 ").x);
        ImGui::TableSetupColumn("Codes", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Codes ").x);
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
                // The slot, when the save has one, is what tells two saves of
                // the same game apart -- which is the whole reason the list
                // has more than one row per game.
                const std::string label =
                    (s.name.empty() ? s.dir_name : s.name)
                    + (s.detail.empty() ? std::string() : "  -  " + s.detail);
                if (ImGui::Selectable(label.c_str(), g_sb.selected == index,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (g_sb.selected != index) g_sb.pick = -1;
                    g_sb.selected = index;
                    if (ImGui::IsMouseDoubleClicked(0))
                        open_now = g_sb.pick >= 0 ? g_sb.pick : s.suggest;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", s.platform);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", s.title_id.empty() ? "-" : s.title_id.c_str());
                ImGui::TableSetColumnIndex(3);
                if (s.patch_index >= 0)
                    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.60f, 1.0f), "yes");
                else
                    ImGui::TextDisabled("-");
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::SameLine();

    if (ImGui::BeginChild("##savedetail", ImVec2(0, -footer), true)) {
        if (g_sb.selected < 0 || g_sb.selected >= int(g_sb.saves.size())) {
            ImGui::TextWrapped(
                g_sb.saves.empty()
                    ? "Choose a folder above. Anything underneath it that is a save - a "
                      "folder with a PARAM.SFO in it, or with sce_sys/param.sfo for PS4 "
                      "and Vita - turns up in this list."
                    : "Pick a save on the left.");
        } else {
            const SaveEntry& s = g_sb.saves[size_t(g_sb.selected)];

            ImGui::TextColored(ImVec4(0.80f, 0.80f, 0.95f, 1.0f), "%s",
                               s.name.empty() ? s.dir_name.c_str() : s.name.c_str());
            if (!s.detail.empty()) ImGui::TextDisabled("%s", s.detail.c_str());
            ImGui::Spacing();
            ImGui::Text("%s   %s", s.platform, s.title_id.empty() ? "(no title ID)"
                                                                  : s.title_id.c_str());
            ImGui::TextDisabled("%s", s.dir_name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.path.c_str());

            ImGui::Spacing();
            if (s.patch_index >= 0) {
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.60f, 1.0f),
                                   "Codes available: %s", s.patch_name.c_str());
            } else {
                ImGui::TextDisabled("No codes in the database for this game.");
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The save still opens - the hex editor works on it,\n"
                                      "and a .savepatch from anywhere else can be loaded\n"
                                      "by hand. Only the bundled database has nothing.");
            }
            if (s.encrypted)
                ImGui::TextDisabled("The %s encrypts this save; it is unwrapped on the "
                                    "way in and re-wrapped on the way out.", s.platform);

            ImGui::Spacing();
            ImGui::SeparatorText("Files in this save");

            if (s.files.empty()) {
                ImGui::TextWrapped("This save holds nothing but its own metadata - "
                                   "no data file to patch.");
            } else {
                const int shown = g_sb.pick >= 0 ? g_sb.pick : s.suggest;
                const float avail = ImGui::GetContentRegionAvail().y
                                  - ImGui::GetFrameHeightWithSpacing() * 2.0f;
                if (ImGui::BeginChild("##files", ImVec2(0, avail > 60.0f ? avail : 60.0f))) {
                    std::error_code ec;
                    for (size_t f = 0; f < s.files.size(); f++) {
                        const uintmax_t n =
                            fs::file_size(fs::path(s.path) / s.files[f], ec);
                        char row[512];
                        snprintf(row, sizeof row, "%s##f%zu", s.files[f].c_str(), f);
                        if (ImGui::Selectable(row, shown == int(f),
                                              ImGuiSelectableFlags_AllowDoubleClick)) {
                            g_sb.pick = int(f);
                            if (ImGui::IsMouseDoubleClicked(0)) open_now = int(f);
                        }
                        ImGui::SameLine(ImGui::GetContentRegionMax().x - 90.0f * g_ui_scale);
                        ImGui::TextDisabled("%s", ec ? "?" : human_size(n).c_str());
                        if (int(f) == s.suggest && shown != int(f)) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("*");
                        }
                    }
                }
                ImGui::EndChild();

                // Which of the two answers this actually is. A save whose
                // game encrypts nothing has a PARAM.PFD listing only
                // PARAM.SFO, so "the console says so" would be a claim the
                // console never made.
                ImGui::TextDisabled(
                    s.suggest_listed
                        ? "* the file the console's own metadata says is the save"
                        : "* the largest file - nothing here names one");

                ImGui::BeginDisabled(shown < 0);
                if (ImGui::Button("Open this save", ImVec2(160, 0))) open_now = shown;
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Make this file the target%s.",
                                      s.patch_index >= 0
                                          ? " and load this game's codes" : "");
            }
        }
    }
    ImGui::EndChild();

    ImGui::Text("%d save%s", int(g_sb.hits.size()), g_sb.hits.size() == 1 ? "" : "s");
    ImGui::SameLine();
    ImGui::TextDisabled("of %d", int(g_sb.saves.size()));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 110.0f);
    if (ImGui::Button("Close", ImVec2(110, 0))) ImGui::CloseCurrentPopup();

    if (open_now >= 0 && g_sb.selected >= 0) {
        open_save_file(g_sb.saves[size_t(g_sb.selected)], open_now);
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

static void draw_menu_bar(bool* want_quit) {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Browse saves...", "Ctrl+B")) g_sb.want_open = true;
            if (ImGui::MenuItem("Find a game...", "Ctrl+F")) g_db.want_open = true;
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
            ImGui::MenuItem("Legend: SW=Save Wizard  BSD  PY=Python", nullptr, false, false);
            ImGui::Separator();
            if (ImGui::MenuItem("Project on GitHub")) open_url(URL_PATCHER);
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
        if (psp_unwrap_target()) {
            g_app.psp.wrap = false;   // it is plaintext now; do not unwrap twice
            g_app.show_log = true;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Take the console's layer off and leave it off, for editing\n"
                          "the file by hand. Turns the checkbox above off.");
    ImGui::SameLine();
    if (ImGui::Button("Re-encrypt")) {
        if (psp_wrap_target()) {
            g_app.psp.wrap = true;
            g_app.show_log = true;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Put the console's layer back on, and rewrite PARAM.SFO\n"
                          "with the file's new hash.");
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Resign PARAM.SFO")) { psp_resign(); g_app.show_log = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Recompute the PARAM.SFO hashes alone. Needs no game key -\n"
                          "for a save that was never encrypted.");

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
        if (ps3_unwrap_target()) {
            g_app.ps3.wrap = false;   // it is plaintext now; do not unwrap twice
            g_app.ps3.hash_checked = false;
            g_app.show_log = true;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Take the console's layer off and leave it off, for editing\n"
                          "the file by hand. Turns the checkbox above off.");
    ImGui::SameLine();
    if (ImGui::Button("Re-encrypt##ps3")) {
        if (ps3_wrap_target()) {
            g_app.ps3.wrap = true;
            g_app.ps3.hash_checked = false;
            g_app.show_log = true;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Put the console's layer back on, and rewrite PARAM.PFD\n"
                          "with the file's new size and hash.");
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Resign PARAM.PFD")) { ps3_resign(); g_app.show_log = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Recompute the PARAM.PFD signatures alone. Needs no key -\n"
                          "for a database whose entries something else edited.");

    // Offered only once a console has been named in Settings, because without
    // one there is nothing to bind TO.
    if (apfd_get_console(nullptr)) {
        ImGui::SameLine();
        ImGui::BeginDisabled(g_app.ps3.sfo_path.empty());
        if (ImGui::Button("Re-bind to your console")) { ps3_rebind(); g_app.show_log = true; }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(g_app.ps3.sfo_path.empty()
                ? "No PARAM.SFO beside this save - the hashes are taken over it."
                : "Rewrite the PARAM.SFO hashes that name a console, so the save\n"
                  "belongs to the one in Settings instead of the one it came from.");
    }

    ImGui::Spacing();
}

// Which console saves are written FOR. A window rather than a modal: somebody
// checking a value against their console's dumper wants to see the save behind
// it.
static void draw_settings_window() {
    if (!g_want_settings) return;

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &g_want_settings, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("Which console Apollo writes saves FOR. Both are optional, "
                       "and neither changes how a save is READ - only what is "
                       "written back. Left blank, a save keeps whatever it "
                       "already says.");

    ImGui::SeparatorText("Save data");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const char* ORDERS[] = { "Auto (detect from the patch)", "Big-endian", "Little-endian" };
    bool order_changed = ImGui::Combo("Byte order", &g_settings.byte_order,
                                      ORDERS, IM_ARRAYSIZE(ORDERS));
    ImGui::TextDisabled("Auto is right for every patch in the database: PS3 saves are");
    ImGui::TextDisabled("big-endian and everything else Apollo covers is not, and the");
    ImGui::TextDisabled("database says which a patch is. Force one only for a loose patch");
    ImGui::TextDisabled("file for a console it does not cover.");
    if (g_settings.byte_order != BYTE_ORDER_AUTO) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                           "A forced order is remembered across runs, and applies to");
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                           "every save. The main window says so when it disagrees with");
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                           "the patch you have open.");
    }

    ImGui::SeparatorText("PSP");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    bool changed = order_changed;
    changed |= ImGui::InputText("Fuse ID", g_settings.fuse_hex, sizeof g_settings.fuse_hex,
                                    ImGuiInputTextFlags_CharsHexadecimal |
                                    ImGuiInputTextFlags_CharsUppercase);
    ImGui::TextDisabled("16 hex digits. Savedata modes 4 and 6 derive two PARAM.SFO");
    ImGui::TextDisabled("hashes from the console's own fuse. A PSP loads a save whose");
    ImGui::TextDisabled("values differ, so this only matters for reproducing one");
    ImGui::TextDisabled("console's output byte for byte. Blank = FFFFFFFFFFFFFFFF.");

    ImGui::SeparatorText("PS3");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
    changed |= ImGui::InputText("Console ID (IDPS)", g_settings.console_hex,
                                sizeof g_settings.console_hex,
                                ImGuiInputTextFlags_CharsHexadecimal |
                                ImGuiInputTextFlags_CharsUppercase);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    changed |= ImGui::InputInt("User number", &g_settings.user_id);
    if (g_settings.user_id < 1) g_settings.user_id = 1;

    ImGui::TextDisabled("32 hex digits. Inside PARAM.PFD, one of PARAM.SFO's four");
    ImGui::TextDisabled("hashes is keyed by the IDPS of a single machine - that is");
    ImGui::TextDisabled("what binds a save to a console. Name yours and the PS3");
    ImGui::TextDisabled("section offers to re-bind a save to it. The user number");
    ImGui::TextDisabled("reaches only a trophy folder's hashes.");
    ImGui::Spacing();
    ImGui::TextDisabled("Re-binding is the PARAM.PFD half of moving a save. A save");
    ImGui::TextDisabled("also carries account fields in its own PARAM.SFO, and those");
    ImGui::TextDisabled("are not touched here.");

    // The fields are all-or-nothing: a half-typed value is not "no value", it
    // is one that would bind a save to the wrong machine.
    const size_t fuse_len = strlen(g_settings.fuse_hex);
    const size_t cid_len  = strlen(g_settings.console_hex);
    const bool   ok = (fuse_len == 0 || fuse_len == 16) && (cid_len == 0 || cid_len == 32);

    /* The order alone: apply just the byte order, not settings_apply(), which
     * would also push whatever half-typed hex is in the fields. */
    if (order_changed) {
        apctl_set_big_endian(effective_big_endian() ? 1 : 0);
        g_saved.byte_order = g_settings.byte_order;
        settings_store();
    }
    if (changed) g_settings.status.clear();

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
                                                      : "the console ID needs 32 hex digits");
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

static void draw_main_window(bool* want_quit) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Apollo Save Patcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);

    draw_menu_bar(want_quit);

    // --- file rows ---
    //
    // Two ways in, and the first is the one to reach for: start from YOUR
    // SAVES and the app works out the game, the codes and the encryption.
    // "Find a game" starts from the patch database instead, which is what you
    // want when the save is not on this machine yet, or to read the codes.
    if (ImGui::Button("Browse saves...")) g_sb.want_open = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Point at a folder of saves - a memory stick, a PS3's\n"
                          "savedata folder, PS4 or Vita exports - and pick one by\n"
                          "name. PSP and PS3 saves are unwrapped for you.");
    ImGui::SameLine();
    if (ImGui::Button("Find a game...")) g_db.want_open = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Search the bundled patch database (%d patches).",
                          patchdb_count(g_db.db));
    ImGui::SameLine();
    if (ImGui::Button("Open .savepatch...")) do_open_patch();
    ImGui::SameLine();
    ImGui::TextUnformatted(g_app.patch_path.empty() ? "(no patch loaded)" : g_app.patch_path.c_str());

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

    if (ImGui::Button("Choose target...")) do_choose_target();
    ImGui::SameLine();
    ImGui::TextUnformatted(g_app.target_path.empty() ? "(script uses patch's own target)"
                                                     : g_app.target_path.c_str());
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

    // The patch this save's title ID names, when it is not the one already
    // open. Never swapped in silently from here: the session being replaced
    // may carry edited code bodies and ticked rows, and this is a one-click
    // offer rather than a surprise.
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
    if (g_app.open_apply_popup) { ImGui::OpenPopup("Apply"); g_app.open_apply_popup = false; }
    if (ImGui::BeginPopupModal("Apply", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(g_app.apply_msg.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    scan_collect();
    render_save_browser();
    render_db_browser();
    draw_about();

    ImGui::End();

    draw_patch_raw();
    draw_hex_editor();
}

static void apply_style() {
    // Cosmetic rounding only — no size scaling, so everything stays at
    // ImGui's default dimensions.
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 6.0f;
    s.FrameRounding     = 4.0f;
    s.GrabRounding      = 4.0f;
    s.ScrollbarRounding = 4.0f;
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
// This is what "drag the app a file" means on macOS: a .app launched from
// Finder is handed documents through Apple Events, not argv, so the command
// line below covers a terminal, a script and a Windows/Linux file association,
// and this covers the gesture people actually reach for.
static void drop_cb(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; i++)
        if (paths[i] && *paths[i]) open_path(paths[i]);
}

static void print_usage(const char* argv0) {
    fprintf(stderr,
        "Apollo Save Patcher\n"
        "\n"
        "  %s [FILE...]\n"
        "  %s --scan DIR [N]\n"
        "\n"
        "A .savepatch is opened as the patch; anything else is opened as the\n"
        "save to patch, which also looks up its game key and its patch in the\n"
        "bundled database. Files can be dropped on the window instead.\n"
        "\n"
        "--scan prints the saves under DIR and exits, without opening a window:\n"
        "the same walk \"Browse saves...\" does, for checking what a folder holds\n"
        "from a terminal or a script. Add a save's number to have it opened as\n"
        "well, which reports the target, the patch and the encryption layer --\n"
        "everything the main window would be showing had you clicked it.\n"
        "\n"
        "  $APOLLO_PATCHES_ZIP   where to find apollo-patches.zip\n",
        argv0 && *argv0 ? argv0 : "apollo_patcher_gui",
        argv0 && *argv0 ? argv0 : "apollo_patcher_gui");
}

//
// The save scan on its own, with no window. This is the browser's whole brain
// -- the same scan_start/scan_collect pair the modal drives -- so what it
// prints is exactly what the list would show, which is the only way to check
// the walk without a person looking at it.
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

static int run_scan(const char* root, int pick) {
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
    }
    printf("\n%s\n", g_sb.note.c_str());

    if (pick < 0)
        return g_sb.saves.empty() ? 1 : 0;

    // ...and what happens when one of them is chosen: the same call the
    // browser's "Open this save" makes, and then everything that followed
    // from it. What this prints is what the main window would be showing.
    if (pick >= int(g_sb.saves.size())) {
        fprintf(stderr, "no save %d in that folder\n", pick);
        return 2;
    }
    const SaveEntry& s = g_sb.saves[size_t(pick)];
    open_save_file(s, s.suggest);

    printf("\nopening save %d\n", pick);
    printf("  target      %s\n", g_app.target_path.c_str());
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
    if (!g_app.psp.found && !g_app.ps3.found)
        printf("  no console encryption layer\n");

    return g_app.session ? 0 : 1;
}

//
// The window's starting size.
//
// Wider than it used to be because the save browser is the way into the app
// now, and a modal cannot be wider than the window around it (see
// size_modal) -- so the window's width is what decides whether the file list
// beside the save list is readable. At 860 the browser lost its right-hand
// edge outright.
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
            return run_scan(argv[i + 1], i + 2 < argc ? atoi(argv[i + 2]) : -1);
        }
        open_path(argv[i]);
    }

#ifdef _WIN32
    // The app ships a Mesa software opengl32.dll in a "softgl" subfolder. If the
    // user copies it next to the .exe, GLFW loads it (the exe's own directory is
    // searched first) and this forces its software renderer. Ignored when the
    // system GPU driver is used.
    _putenv_s("GALLIUM_DRIVER", "llvmpipe");
#endif

    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) { fatal("Failed to initialize GLFW.\n\n" + g_glfw_error); return 1; }
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

    io.Fonts->AddFontDefault();   // ImGui's default 13px font, no HiDPI upscaling

    ImGui::StyleColorsDark();
    apply_style();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    // Which console saves are written FOR, from the user's config directory.
    // Applied before anything can use it -- a save patched in the first second
    // has to be written for the same console as one patched in the tenth.
    settings_load();

    // The folder from last time, scanned in the background while the window
    // comes up. Costs nothing when there is none, and means "Browse saves..."
    // shows a list rather than an empty pane on every launch.
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
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_B, false)) g_sb.want_open = true;
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

    g_app.close();
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
