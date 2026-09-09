// Apollo Patcher GUI — Dear ImGui front-end over apollo_ctrl.
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
#include <fstream>
#include <mutex>

#include "imgui.h"
#include "imgui_internal.h"   // PushItemFlag + ImGuiItemFlags_MixedValue (tri-state)
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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>   // MessageBox for visible startup errors (no console with -mwindows)
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
    bool                show_patch_raw = false;

    std::vector<unsigned char> hex_data;   // the target file, for the hex editor
    std::string         hex_path;          // which file hex_data came from
    bool                show_hex = false;
    bool                hex_dirty = false; // edits not yet written to disk
    std::vector<char>   selected;         // per-row checkbox
    std::vector<char>   viewer_open;      // per-row raw-code window open flag
    std::string         log;
    std::mutex          log_mtx;
    bool                backup = true;    // copy target -> target.bak before patching
    bool                big_endian = false; // engine data byte order (CLI's -b flag)
    bool                scroll_log = false;
    bool                show_log = false; // log pane collapsed by default
    bool                open_apply_popup = false;
    std::string         apply_msg;

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
        game_name.clear();
        patch_path.clear();
        patch_raw.clear();
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

// Shared tail of both load paths (file and database): a session exists, so set
// up the per-row UI state around it.
static void adopt_session(apctl_session_t* session,
                          const std::string& label,
                          const char* display_name) {
    g_app.session = session;
    g_app.patch_path = label;
    g_app.game_name = display_name && *display_name ? display_name
                                                    : apctl_game_name(session);
    const int n = apctl_code_count(session);
    g_app.selected.assign(n, 0);
    g_app.viewer_open.assign(n, 0);
    for (int i = 0; i < n; ++i)      // pre-check [DEFAULT:] codes
        g_app.selected[i] = apctl_code_at(session, i)->activated ? 1 : 0;

    char buf[512];
    snprintf(buf, sizeof buf, "Loaded %d codes from %s", n, label.c_str());
    g_app.append_log(buf);

    // PS3 save data is big-endian, and no patch in the database declares the
    // order per code (the engine's [BE:...] header exists but goes unused), so
    // pick the mode up from the title ID rather than leave the user to notice.
    // Still a checkbox they can override afterwards.
    if (apctl_title_is_big_endian(label.c_str())) {
        if (!g_app.big_endian) {
            g_app.big_endian = true;
            g_app.append_log("PS3 title detected - big-endian data mode enabled");
        }
    } else if (g_app.big_endian) {
        g_app.big_endian = false;
        g_app.append_log("Non-PS3 title - big-endian data mode disabled");
    }
    apctl_set_big_endian(g_app.big_endian ? 1 : 0);
}

// Window titles get the file name; the full path goes in the body, where it
// can wrap and be read.
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
        g_app.patch_raw = strip_cr(raw);
    }
    adopt_session(s, path, nullptr);
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
    g_app.patch_raw = strip_cr(std::string(data, len));
    free(data);

    if (!s) { g_app.append_log("[!] Could not parse that patch"); return; }

    // The index's name is preferred: it was decoded at build time, where the
    // 245 Windows-1252 patch files (game names with (TM)/(R)) are handled.
    adopt_session(s, label, e->name);
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
    apctl_set_big_endian(g_app.big_endian ? 1 : 0);
    g_app.log.clear();
    g_app.append_log(g_app.big_endian ? "=== Using big-endian data mode"
                                      : "=== Using host (little-endian) data mode");

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

    // Result pop-up message.
    char msg[160];
    if (errors == 0) {
        snprintf(msg, sizeof msg, "All done — %d code(s) applied successfully.", applied);
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

// Native modals are opened AFTER the ImGui frame is rendered (see the main
// loop), not from inside a widget callback — opening a modal mid-frame is the
// second macOS pitfall. Widgets just raise these intents.
static bool g_pending_open   = false;
static bool g_pending_target = false;
static void do_open_patch()    { g_pending_open = true; }
static void do_choose_target() { g_pending_target = true; }

static void process_pending_dialogs() {
    if (g_pending_open) {
        g_pending_open = false;
        std::string p = pick_file("savepatch");
        if (!p.empty()) load_patch(p);
    }
    if (g_pending_target) {
        g_pending_target = false;
        std::string p = pick_file(nullptr);
        if (!p.empty()) g_app.target_path = p;
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

            if (parent || disabled) ImGui::PopStyleColor();
            if (indent) ImGui::Unindent(indent);

            // --- col 1: View button opens a raw-code window ---
            ImGui::TableSetColumnIndex(1);
            const char* body = apctl_code_text(c);
            if (body && body[0]) {
                if (ImGui::SmallButton("View")) g_app.viewer_open[i] = 1;
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

static void draw_code_viewers() {
    if (!g_app.session) return;
    for (int i = 0; i < apctl_code_count(g_app.session); ++i) {
        if (!g_app.viewer_open[i]) continue;
        apctl_code_t* c = apctl_code_at(g_app.session, i);
        char title[160];
        snprintf(title, sizeof title, "Code: %s##viewer%d",
                 (c->name && c->name[0]) ? c->name : "(unnamed)", i);
        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(560, 400), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title, &open)) {
            ImGui::Text("Target file: %s", (c->file && c->file[0]) ? c->file : "(none)");
            ImGui::Separator();
            ImGui::BeginChild("body", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(apctl_code_text(c));
            ImGui::EndChild();
        }
        ImGui::End();
        if (!open) g_app.viewer_open[i] = 0;
    }
}

// The database browser. 2200+ rows, so the list is clipped rather than emitted
// in full every frame.
static void render_db_browser() {
    if (g_db.want_open) {
        g_db.want_open = false;
        g_db.refilter = true;
        ImGui::OpenPopup("Patch database");
    }

    ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_Appearing);
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

static void draw_menu_bar(bool* want_quit) {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Find a game...", "Ctrl+F")) g_db.want_open = true;
            if (ImGui::MenuItem("Open .savepatch...", "Ctrl+O")) do_open_patch();
            if (ImGui::MenuItem("Choose target file...")) do_choose_target();
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Q")) *want_quit = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("Apollo Patcher GUI", nullptr, false, false);
            ImGui::MenuItem("Legend: SW=Save Wizard  BSD  PY=Python", nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

static void draw_main_window(bool* want_quit) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Apollo Patcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);

    draw_menu_bar(want_quit);

    // --- file rows ---
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

    ImGui::Checkbox("Back up target (.bak) before patching", &g_app.backup);

    // Data byte order — equivalent of the CLI's -b/--big-endian flag. Applied
    // to the engine right before patching (see apply_selected).
    if (ImGui::Checkbox("Big-endian mode", &g_app.big_endian))
        apctl_set_big_endian(g_app.big_endian ? 1 : 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Read/write save data as big-endian (PS3, Xbox 360, Wii, ...).\n"
                          "Leave off for little-endian saves (PS4, PS Vita, PC).\n"
                          "Same as the patcher CLI's -b/--big-endian flag.");

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
    ImGui::BeginDisabled(blocked || !g_app.session || count_selected() == 0);
    if (ImGui::Button("Apply selected", ImVec2(140, 0))) apply_selected();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear log")) { std::lock_guard<std::mutex> lk(g_app.log_mtx); g_app.log.clear(); }
    if (blocked) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                           "Fill the required options on the highlighted codes first.");
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

    render_db_browser();

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
    MessageBoxA(nullptr, msg.c_str(), "Apollo Patcher — startup error", MB_ICONERROR | MB_OK);
#else
    fprintf(stderr, "%s\n", msg.c_str());
#endif
}

int main(int, char**) {
    apctl_set_log_sink(log_sink, &g_app);
    init_patchdb();

#ifdef _WIN32
    // The app ships a Mesa software opengl32.dll in a "softgl" subfolder. If the
    // user copies it next to the .exe, GLFW loads it (the exe's own directory is
    // searched first) and this forces its software renderer. Ignored when the
    // system GPU driver is used.
    _putenv_s("GALLIUM_DRIVER", "llvmpipe");
#endif

    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) { fatal("Failed to initialize GLFW.\n\n" + g_glfw_error); return 1; }
    // No context hints: GLFW's default legacy/compatibility context is what the
    // fixed-function opengl2 backend needs, on every platform.
    GLFWwindow* window = glfwCreateWindow(860, 820, "Apollo Patcher", nullptr, nullptr);
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

    bool want_quit = false;
    while (!glfwWindowShouldClose(window) && !want_quit) {
        glfwPollEvents();
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // keyboard shortcuts
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_O, false)) do_open_patch();
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_F, false)) g_db.want_open = true;
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_Q, false)) want_quit = true;

        draw_main_window(&want_quit);
        draw_code_viewers();

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

    g_app.close();
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
