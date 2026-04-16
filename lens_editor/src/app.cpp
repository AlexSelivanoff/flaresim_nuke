// ============================================================================
// app.cpp — FlareSim Lens Editor: window setup, ImGui loop, surface table,
//           lens diagram, and flare preview panels.
// ============================================================================

#include "app.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"   // DockBuilder API

#include <GLFW/glfw3.h>

// GL constants not in Windows' legacy gl.h
#ifndef GL_RGBA32F
#  define GL_RGBA32F 0x8814
#endif
#ifndef GL_CLAMP_TO_EDGE
#  define GL_CLAMP_TO_EDGE 0x812F
#endif

#include "ghost.h"
#include "ghost_cuda.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <locale>
#include <thread>
#include <mutex>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <commdlg.h>
#  pragma comment(lib, "comdlg32.lib")
#endif

// ---------------------------------------------------------------------------
// Native file-picker helpers (Win32; falls back to empty string on other OS)
// ---------------------------------------------------------------------------
static std::string native_open_dialog(const char* filter, const char* title)
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn     = {};
    // Convert filter string (e.g. "Lens\0*.lens\0\0") to wide
    // filter is a double-null-terminated sequence; determine its length
    const char* p = filter;
    while (*p || *(p+1)) ++p;
    int flen = (int)(p - filter) + 2;
    std::wstring wfilt(flen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, filter, flen, wfilt.data(), flen);
    wchar_t wtitle[128] = {};
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 128);

    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = nullptr;
    ofn.lpstrFilter  = wfilt.data();
    ofn.lpstrFile    = buf;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrTitle   = wtitle;
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        char utf8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
        return utf8;
    }
#else
    (void)filter; (void)title;
#endif
    return {};
}

static std::string native_save_dialog(const char* filter, const char* title,
                                      const char* default_ext)
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn     = {};
    const char* p = filter;
    while (*p || *(p+1)) ++p;
    int flen = (int)(p - filter) + 2;
    std::wstring wfilt(flen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, filter, flen, wfilt.data(), flen);
    wchar_t wtitle[128] = {}, wext[16] = {};
    MultiByteToWideChar(CP_UTF8, 0, title,       -1, wtitle, 128);
    MultiByteToWideChar(CP_UTF8, 0, default_ext, -1, wext,   16);

    ofn.lStructSize    = sizeof(ofn);
    ofn.hwndOwner      = nullptr;
    ofn.lpstrFilter    = wfilt.data();
    ofn.lpstrFile      = buf;
    ofn.nMaxFile       = MAX_PATH;
    ofn.lpstrTitle     = wtitle;
    ofn.lpstrDefExt    = wext;
    ofn.Flags          = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&ofn)) {
        char utf8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
        return utf8;
    }
#else
    (void)filter; (void)title; (void)default_ext;
#endif
    return {};
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// GLFW error callback
// ---------------------------------------------------------------------------
static void glfw_error_cb(int error, const char* desc)
{
    fprintf(stderr, "GLFW error %d: %s\n", error, desc);
}

// ---------------------------------------------------------------------------
// Undo / redo helpers
// ---------------------------------------------------------------------------
void App::push_undo()
{
    if (undo_stack_.size() >= kMaxUndo)
        undo_stack_.pop_front();
    undo_stack_.push_back(lens_);
    redo_stack_.clear();
    dirty_      = true;
    prev_dirty_ = true;  // preview needs re-render
}

void App::undo()
{
    if (undo_stack_.empty()) return;
    redo_stack_.push_back(lens_);
    lens_ = undo_stack_.back();
    undo_stack_.pop_back();
    lens_.compute_geometry();
    dirty_ = true;
    snprintf(status_msg_, sizeof(status_msg_), "Undo");
}

void App::redo()
{
    if (redo_stack_.empty()) return;
    undo_stack_.push_back(lens_);
    lens_ = redo_stack_.back();
    redo_stack_.pop_back();
    lens_.compute_geometry();
    dirty_ = true;
    snprintf(status_msg_, sizeof(status_msg_), "Redo");
}

// ---------------------------------------------------------------------------
// Save .lens file
// ---------------------------------------------------------------------------
void App::save_lens(const char* path)
{
    std::ofstream f(path);
    if (!f.is_open()) {
        snprintf(status_msg_, sizeof(status_msg_), "ERROR: cannot write %s", path);
        return;
    }
    f.imbue(std::locale::classic());

    f << "name: " << lens_.name << "\n";
    f << "focal_length: " << lens_.focal_length << "\n\n";
    f << "surfaces:\n";
    f << "# radius      thickness   ior      abbe     semi_ap  coating\n";

    for (const Surface& s : lens_.surfaces) {
        if (s.is_stop)
            f << "  stop";
        else if (s.radius == 0.0f)
            f << "  0";
        else {
            char buf[32];
            snprintf(buf, sizeof(buf), "  %.6g", s.radius);
            f << buf;
        }
        char row[128];
        snprintf(row, sizeof(row), "  %.6g  %.6g  %.4g  %.6g  %d",
                 s.thickness, s.ior, s.abbe_v, s.semi_aperture, s.coating);
        f << row << "\n";
    }

    f.close();
    lens_path_ = path;
    dirty_ = false;
    snprintf(status_msg_, sizeof(status_msg_), "Saved: %s", path);
}

// ---------------------------------------------------------------------------
// App::init
// ---------------------------------------------------------------------------
bool App::init()
{
    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialise GLFW\n");
        return false;
    }

    // Request OpenGL 3.3 Core — minimum for imgui_impl_opengl3
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    window_ = glfwCreateWindow(1280, 800, "FlareSim Lens Editor", nullptr, nullptr);
    if (!window_) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1); // vsync

    // Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    // Slightly soften the dark theme for comfortable long-session use
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 4.0f;
    style.FrameRounding    = 3.0f;
    style.GrabRounding     = 3.0f;
    style.ScrollbarRounding = 4.0f;
    style.FrameBorderSize  = 0.5f;

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    fprintf(stdout, "FlareSim Lens Editor initialised (OpenGL 3.3 + Dear ImGui)\n");
    return true;
}

// ---------------------------------------------------------------------------
// App::load_lens
// ---------------------------------------------------------------------------
void App::load_lens(const char* path)
{
    lens_path_ = path;
    std::strncpy(path_buf_, path, sizeof(path_buf_) - 1);

    if (lens_.load(path)) {
        lens_loaded_ = true;
        undo_stack_.clear();
        redo_stack_.clear();
        dirty_      = false;
        prev_dirty_ = true;  // new lens — schedule render on next frame
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Loaded: %s  (%d surfaces, fl=%.1f mm)",
                      lens_.name.c_str(), lens_.num_surfaces(), lens_.focal_length);

        // Add to recent files (deduplicated, newest first)
        std::string sp(path);
        recent_files_.erase(std::remove(recent_files_.begin(), recent_files_.end(), sp),
                            recent_files_.end());
        recent_files_.insert(recent_files_.begin(), sp);
        if ((int)recent_files_.size() > kMaxRecent)
            recent_files_.resize(kMaxRecent);
    } else {
        lens_loaded_ = false;
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "ERROR: failed to load %s", path);
    }
}

// ---------------------------------------------------------------------------
// App::shutdown
// ---------------------------------------------------------------------------
void App::shutdown()
{
    // Stop any in-flight preview render
    if (prev_rendering_.load()) {
        // Wait for thread to finish naturally — it checks prev_rendering_
        if (prev_thread_.joinable()) prev_thread_.join();
    }
    if (prev_tex_) {
        glDeleteTextures(1, &prev_tex_);
        prev_tex_ = 0;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
}

// ---------------------------------------------------------------------------
// App::run — main loop
// ---------------------------------------------------------------------------
void App::run()
{
    while (!glfwWindowShouldClose(window_))
    {
        glfwPollEvents();

        // Update window title to reflect dirty state
        {
            char title[512];
            if (lens_loaded_) {
                const char* base = lens_path_.empty() ? "untitled" : lens_path_.c_str();
                snprintf(title, sizeof(title), "FlareSim Lens Editor — %s%s",
                         base, dirty_ ? " *" : "");
            } else {
                snprintf(title, sizeof(title), "FlareSim Lens Editor");
            }
            glfwSetWindowTitle(window_, title);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Full-screen dockspace so panels can be freely rearranged
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##dockspace_root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove     |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus |
                     ImGuiWindowFlags_MenuBar);
        ImGui::PopStyleVar(3);

        draw_menu_bar();

        ImGuiID ds_id = ImGui::GetID("DockSpace");
        ImGui::DockSpace(ds_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);

        // Build the default layout exactly once (first frame after init).
        if (!dockspace_built_) {
            setup_default_dockspace(ds_id);
            dockspace_built_ = true;
        }

        ImGui::End();

        // ---- Panels ----
        draw_flare_preview();
        draw_lens_diagram();
        draw_info_panel();
        draw_surface_table();
        draw_status_bar();
        // Open/SaveAs use native OS dialogs — no ImGui popups needed.

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window_, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
        upload_preview_texture();
    }
}

// ---------------------------------------------------------------------------
// draw_menu_bar — File / Edit menus + keyboard shortcuts
// ---------------------------------------------------------------------------
void App::draw_menu_bar()
{
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Lens...", "Ctrl+O")) {
            auto p = native_open_dialog(
                "Lens files\0*.lens\0All files\0*.*\0",
                "Open Lens File");
            if (!p.empty()) load_lens(p.c_str());
        }

        ImGui::Separator();

        bool can_save = lens_loaded_ && !lens_path_.empty();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, can_save))
            save_lens(lens_path_.c_str());
        if (ImGui::MenuItem("Save As...", nullptr, false, lens_loaded_)) {
            auto p = native_save_dialog(
                "Lens files\0*.lens\0All files\0*.*\0",
                "Save Lens File", "lens");
            if (!p.empty()) save_lens(p.c_str());
        }

        // Recent files sub-menu
        if (!recent_files_.empty() && ImGui::BeginMenu("Recent Files")) {
            for (const std::string& rp : recent_files_) {
                if (ImGui::MenuItem(rp.c_str()))
                    load_lens(rp.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Recent"))
                recent_files_.clear();
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4"))
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_stack_.empty()))
            undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo_stack_.empty()))
            redo();
        ImGui::EndMenu();
    }

    // Keyboard shortcuts
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
        auto p = native_open_dialog(
            "Lens files\0*.lens\0All files\0*.*\0", "Open Lens File");
        if (!p.empty()) load_lens(p.c_str());
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && lens_loaded_) {
        if (lens_path_.empty()) {
            auto p = native_save_dialog(
                "Lens files\0*.lens\0All files\0*.*\0", "Save Lens File", "lens");
            if (!p.empty()) save_lens(p.c_str());
        } else {
            save_lens(lens_path_.c_str());
        }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) undo();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) redo();

    ImGui::EndMenuBar();
}


// ---------------------------------------------------------------------------
// draw_info_panel — lens metadata
// ---------------------------------------------------------------------------
void App::draw_info_panel()
{
    ImGui::Begin("Lens Info");

    if (!lens_loaded_) {
        ImGui::TextDisabled("No lens loaded. Use File > Open Lens...");
    } else {
        ImGui::LabelText("Name",          "%s",    lens_.name.c_str());
        ImGui::LabelText("Focal Length",  "%.2f mm", lens_.focal_length);
        ImGui::LabelText("Surfaces",      "%d",    lens_.num_surfaces());
        ImGui::LabelText("Sensor Z",      "%.3f mm", lens_.sensor_z);
        // Find the stop surface
        int stop_idx = -1;
        for (int i = 0; i < lens_.num_surfaces(); ++i)
            if (lens_.surfaces[i].is_stop) { stop_idx = i; break; }
        if (stop_idx >= 0)
            ImGui::LabelText("Aperture Stop", "Surface %d (semi-ap %.2f mm)",
                             stop_idx, lens_.surfaces[stop_idx].semi_aperture);
        else
            ImGui::LabelText("Aperture Stop", "Not marked");

        ImGui::Spacing();
        ImGui::LabelText("File", "%s", lens_path_.c_str());
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// draw_surface_table — editable prescription grid
// ---------------------------------------------------------------------------
void App::draw_surface_table()
{
    ImGui::Begin("Surfaces");

    if (!lens_loaded_) {
        ImGui::TextDisabled("Load a lens to see surfaces.");
        ImGui::End();
        return;
    }

    static ImGuiTableFlags flags =
        ImGuiTableFlags_Borders         |
        ImGuiTableFlags_RowBg           |
        ImGuiTableFlags_ScrollY         |
        ImGuiTableFlags_SizingFixedFit  |
        ImGuiTableFlags_Resizable;

    // Reserve space at bottom for buttons + count line
    const float btn_bar_h = ImGui::GetFrameHeight() + ImGui::GetTextLineHeightWithSpacing() + 8.0f;
    const float table_h   = ImGui::GetContentRegionAvail().y - btn_bar_h;

    bool geometry_dirty = false;  // set true when thickness/radius changes
    int  del_row = -1;            // row to delete after table ends
    int  ins_row = -1;            // insert AFTER this row (-1 = append)

    if (ImGui::BeginTable("surfaces_tbl", 9, flags, ImVec2(0, table_h)))
    {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("#",        ImGuiTableColumnFlags_WidthFixed, 26.0f);
        ImGui::TableSetupColumn("Radius",   ImGuiTableColumnFlags_WidthFixed, 88.0f);
        ImGui::TableSetupColumn("Thickness",ImGuiTableColumnFlags_WidthFixed, 88.0f);
        ImGui::TableSetupColumn("IOR",      ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Abbe V",   ImGuiTableColumnFlags_WidthFixed, 68.0f);
        ImGui::TableSetupColumn("Semi-AP",  ImGuiTableColumnFlags_WidthFixed, 68.0f);
        ImGui::TableSetupColumn("Coat",     ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Stop",     ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("##ops",    ImGuiTableColumnFlags_WidthFixed, 46.0f);
        ImGui::TableHeadersRow();

        // Track selection changes from diagram so we can auto-scroll
        static int prev_diag_sel = -1;
        const bool scroll_to_sel = (diag_selected_ != prev_diag_sel && diag_selected_ >= 0);
        prev_diag_sel = diag_selected_;

        const int n = lens_.num_surfaces();
        for (int i = 0; i < n; ++i)
        {
            Surface& s = lens_.surfaces[i];
            ImGui::PushID(i);

            ImGui::TableNextRow();

            // Row background: selected (from diagram) > stop > default
            if (i == diag_selected_) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(100, 140, 80, 140));
                if (scroll_to_sel)
                    ImGui::SetScrollHereY(0.5f);
            } else if (s.is_stop) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(70, 110, 70, 80));
            }

            // col 0 — index (click to select surface in diagram)
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i);
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                diag_selected_ = i;

            // col 1 — radius
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (s.is_stop) {
                ImGui::TextDisabled("stop");
            } else {
                float r = s.radius;
                if (ImGui::InputFloat("##r", &r, 0, 0, "%.4g",
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
                    push_undo();
                    s.radius = r;
                    geometry_dirty = true;
                }
            }

            // col 2 — thickness
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            {
                float t = s.thickness;
                if (ImGui::InputFloat("##t", &t, 0, 0, "%.4g",
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
                    push_undo();
                    s.thickness = t;
                    geometry_dirty = true;
                }
            }

            // col 3 — IOR
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-FLT_MIN);
            {
                float v = s.ior;
                if (ImGui::InputFloat("##ior", &v, 0, 0, "%.5g",
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
                    push_undo();
                    s.ior = std::max(1.0f, v);
                }
            }

            // col 4 — Abbe V
            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(-FLT_MIN);
            {
                float v = s.abbe_v;
                if (ImGui::InputFloat("##av", &v, 0, 0, "%.3g",
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
                    push_undo();
                    s.abbe_v = std::max(0.0f, v);
                }
            }

            // col 5 — semi aperture
            ImGui::TableSetColumnIndex(5);
            ImGui::SetNextItemWidth(-FLT_MIN);
            {
                float v = s.semi_aperture;
                if (ImGui::InputFloat("##ap", &v, 0, 0, "%.4g",
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
                    push_undo();
                    s.semi_aperture = std::max(0.01f, v);
                }
            }

            // col 6 — coating
            ImGui::TableSetColumnIndex(6);
            ImGui::SetNextItemWidth(-FLT_MIN);
            {
                int c = s.coating;
                if (ImGui::InputInt("##c", &c, 0, 0,
                                    ImGuiInputTextFlags_EnterReturnsTrue)) {
                    push_undo();
                    s.coating = std::max(0, c);
                }
            }

            // col 7 — stop checkbox
            ImGui::TableSetColumnIndex(7);
            {
                bool is_stop = s.is_stop;
                if (ImGui::Checkbox("##stop", &is_stop)) {
                    push_undo();
                    // Clear existing stop when setting a new one
                    if (is_stop)
                        for (Surface& os : lens_.surfaces) os.is_stop = false;
                    s.is_stop = is_stop;
                    if (is_stop) s.radius = 0.0f;
                }
            }

            // col 8 — row operations: insert after / delete
            ImGui::TableSetColumnIndex(8);
            if (ImGui::SmallButton("+")) ins_row = i;
            ImGui::SameLine(0, 2);
            ImGui::BeginDisabled(n <= 1);
            if (ImGui::SmallButton("x")) del_row = i;
            ImGui::EndDisabled();

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // ---- Mutations outside the table loop ----
    if (del_row >= 0) {
        push_undo();
        lens_.surfaces.erase(lens_.surfaces.begin() + del_row);
        lens_.compute_geometry();
    }
    if (ins_row >= 0) {
        push_undo();
        Surface ns{};
        ns.radius       = 0.0f;
        ns.thickness    = 1.0f;
        ns.ior          = 1.0f;
        ns.abbe_v       = 0.0f;
        ns.semi_aperture = lens_.surfaces[ins_row].semi_aperture;
        ns.coating      = 0;
        ns.is_stop      = false;
        lens_.surfaces.insert(lens_.surfaces.begin() + ins_row + 1, ns);
        lens_.compute_geometry();
    }
    if (geometry_dirty)
        lens_.compute_geometry();

    // ---- Button bar ----
    if (ImGui::Button("+ Surface")) {
        push_undo();
        Surface ns{};
        ns.radius       = 0.0f;
        ns.thickness    = 1.0f;
        ns.ior          = 1.0f;
        ns.semi_aperture = 10.0f;
        lens_.surfaces.push_back(ns);
        lens_.compute_geometry();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  %d surface(s)%s",
                        lens_.num_surfaces(), dirty_ ? "  [modified]" : "");

    ImGui::End();
}

// ---------------------------------------------------------------------------
// draw_status_bar — pinned to the bottom of the main viewport
// ---------------------------------------------------------------------------
void App::draw_status_bar()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float bar_h = ImGui::GetFrameHeight() + 4.0f;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - bar_h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, bar_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));

    ImGui::Begin("##statusbar", nullptr,
                 ImGuiWindowFlags_NoNav          | ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoInputs       | ImGuiWindowFlags_NoMove       |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextUnformatted(status_msg_);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------------------
// Preview render helpers (Phase 4)
// ---------------------------------------------------------------------------

// (Re)create the GL texture at size w×h.
void App::alloc_preview_texture(int w, int h)
{
    if (prev_tex_) {
        glDeleteTextures(1, &prev_tex_);
        prev_tex_ = 0;
    }
    glGenTextures(1, &prev_tex_);
    glBindTexture(GL_TEXTURE_2D, prev_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Allocate storage — GL_RGBA32F for HDR float data (constant = 0x8814)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    prev_tex_w_ = w;
    prev_tex_h_ = h;
}

// Upload the tone-mapped result to the GL texture (call on main thread only).
void App::upload_preview_texture()
{
    std::vector<float> local;
    int lw, lh;
    {
        std::lock_guard<std::mutex> lk(prev_mutex_);
        lw = prev_buf_w_;
        lh = prev_buf_h_;
        local = prev_rgba_buf_; // copy out of mutex
    }
    if (local.empty() || lw <= 0 || lh <= 0) return;

    if (prev_tex_w_ != lw || prev_tex_h_ != lh)
        alloc_preview_texture(lw, lh);

    glBindTexture(GL_TEXTURE_2D, prev_tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lw, lh, GL_RGBA, GL_FLOAT, local.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

// Kick off an async render.  Joins any in-progress thread first.
void App::schedule_preview_render()
{
    if (!lens_loaded_) return;

    // Wait for previous render to complete (CUDA can't be cancelled mid-kernel)
    if (prev_thread_.joinable())
        prev_thread_.join();

    // Capture render parameters on the main thread
    prev_render_lens_ = lens_;
    const int    res  = prev_res_;
    const float  gain = prev_gain_;
    const float  ev   = prev_ev_;

    // Sensor dimensions: 16:9 filmback (36×20.25 mm)
    const float shw = 18.0f;
    const float shh = shw * 9.0f / 16.0f;  // 10.125 mm

    GhostConfig cfg{};
    cfg.ray_grid            = prev_ray_grid_;
    cfg.min_intensity       = 1e-7f;
    cfg.gain                = gain;
    cfg.ghost_normalize     = true;
    cfg.max_area_boost      = 100.0f;
    cfg.aperture_blades     = prev_blades_;
    cfg.aperture_rotation_deg = prev_blade_rot_;
    cfg.spectral_samples    = prev_spec_samp_;
    cfg.pupil_jitter        = 0;
    cfg.wavelengths[0]      = 650.0f;
    cfg.wavelengths[1]      = 550.0f;
    cfg.wavelengths[2]      = 450.0f;

    const float ax = prev_angle_x_;
    const float ay = prev_angle_y_;

    prev_rendering_.store(true);
    prev_result_ready_.store(false);

    prev_thread_ = std::thread([this, res, shw, shh, cfg, ax, ay, ev]()
    {
        LensSystem lens_copy = prev_render_lens_;

        std::vector<GhostPair> pairs;
        std::vector<float>     boosts;
        filter_ghost_pairs(lens_copy, shw, shh, cfg, pairs, boosts);

        // Single point-light source at the configured angle
        BrightPixel src{};
        src.angle_x = ax;
        src.angle_y = ay;
        const float intensity = std::pow(2.0f, ev);
        src.r = intensity;
        src.g = intensity;
        src.b = intensity;

        const int W = res, H = (res * 9 + 8) / 16;  // 16:9
        std::vector<float> out_r(W * H, 0.0f);
        std::vector<float> out_g(W * H, 0.0f);
        std::vector<float> out_b(W * H, 0.0f);

        if (!pairs.empty()) {
            std::string err;
            launch_ghost_cuda(lens_copy, pairs, boosts,
                              {src}, shw, shh,
                              out_r.data(), out_g.data(), out_b.data(),
                              W, H, W, H, 0, 0,
                              cfg, prev_gpu_cache_, &err);
            if (!err.empty())
                snprintf(status_msg_, sizeof(status_msg_), "CUDA: %s", err.c_str());
        }

        // Tone-map: simple Reinhard per-channel, γ 2.2
        std::vector<float> rgba(W * H * 4);
        for (int i = 0; i < W * H; ++i)
        {
            auto tm = [](float x) -> float {
                x = std::max(x, 0.0f);
                x = x / (1.0f + x);                // Reinhard
                return std::pow(x, 1.0f / 2.2f);   // gamma
            };
            rgba[i * 4 + 0] = tm(out_r[i]);
            rgba[i * 4 + 1] = tm(out_g[i]);
            rgba[i * 4 + 2] = tm(out_b[i]);
            rgba[i * 4 + 3] = 1.0f;
        }

        {
            std::lock_guard<std::mutex> lk(prev_mutex_);
            prev_rgba_buf_ = std::move(rgba);
            prev_buf_w_    = W;
            prev_buf_h_    = H;
        }

        prev_rendering_.store(false);
        prev_result_ready_.store(true);
    });
}

// ---------------------------------------------------------------------------
// draw_flare_preview — top-right hero panel
// ---------------------------------------------------------------------------
void App::draw_flare_preview()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 1.0f));
    ImGui::Begin("Flare Preview");
    ImGui::PopStyleColor();

    const ImVec2 avail = ImGui::GetContentRegionAvail();

    if (!lens_loaded_) {
        const char* msg = "Load a lens to enable preview";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - ts.x) * 0.5f,
                                   (avail.y - ts.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
        ImGui::End();
        return;
    }

    // ---- Upload result if ready ----
    if (prev_result_ready_.exchange(false))
        upload_preview_texture();

    // ---- Controls ----
    // Reserve top bar for sliders, leave rest for the image
    const float ctrl_h = ImGui::GetFrameHeightWithSpacing() * 3.0f + 8.0f;
    const float img_h  = avail.y - ctrl_h;
    const float img_w  = avail.x;

    bool need_render = false;

    // Row 1: light angles + EV + gain
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderFloat("Angle X", &prev_angle_x_, -0.5f, 0.5f, "%.3f"))
        need_render = true;
    ImGui::SameLine(0, 14);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderFloat("Angle Y", &prev_angle_y_, -0.281f, 0.281f, "%.3f"))
        need_render = true;
    ImGui::SameLine(0, 14);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::SliderFloat("EV", &prev_ev_, 6.0f, 24.0f, "%.1f"))
        need_render = true;
    ImGui::SameLine(0, 14);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderFloat("Gain", &prev_gain_, 10.0f, 10000.0f, "%.0f",
                           ImGuiSliderFlags_Logarithmic))
        need_render = true;

    // Row 2: quality knobs
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::SliderInt("Grid", &prev_ray_grid_, 8, 5000))
        need_render = true;
    ImGui::SameLine(0, 14);
    static const char* spec_labels[] = {"3","5","7","9","11"};
    static const int   spec_vals[]   = {3,5,7,9,11};
    int spec_idx = 0;
    for (int i = 0; i < 5; ++i)
        if (spec_vals[i] == prev_spec_samp_) { spec_idx = i; break; }
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::Combo("Spectral", &spec_idx, spec_labels, 5)) {
        prev_spec_samp_ = spec_vals[spec_idx];
        need_render = true;
    }
    ImGui::SameLine(0, 14);
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::SliderInt("Blades", &prev_blades_, 0, 16))
        need_render = true;
    ImGui::SameLine(0, 14);
    static const char* res_labels[] = {"256×144","512×288","1024×576"};
    static const int   res_vals[]   = {256,512,1024};
    int res_idx = 1;    for (int i = 0; i < 3; ++i)
        if (res_vals[i] == prev_res_) { res_idx = i; break; }
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::Combo("Res", &res_idx, res_labels, 3)) {
        prev_res_ = res_vals[res_idx];
        need_render = true;
    }
    ImGui::SameLine(0, 14);
    const bool busy = prev_rendering_.load();
    if (busy) {
        ImGui::BeginDisabled();
        ImGui::Button("Rendering...");
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Render")) need_render = true;
    }

    // Also re-render when the lens prescription was edited
    if (prev_dirty_) { need_render = true; prev_dirty_ = false; }

    // NOTE: schedule_preview_render() is called after the drag block below,
    // so that drag-triggered need_render is also caught.

    // ---- Preview image + interactive light position ----
    // Compute the letterboxed display rect (1:1, centred)
    ImVec2 img_pos = ImGui::GetCursorScreenPos();
    // Letterbox: fit 16:9 into available space
    float disp_w = img_w;
    float disp_h = img_w * 9.0f / 16.0f;
    if (disp_h > img_h) { disp_h = img_h; disp_w = img_h * 16.0f / 9.0f; }
    const float ox      = (img_w - disp_w) * 0.5f;
    const float oy      = (img_h - disp_h) * 0.5f;
    const ImVec2 disp_tl = ImVec2(img_pos.x + ox, img_pos.y + oy);
    const ImVec2 disp_br = ImVec2(disp_tl.x + disp_w, disp_tl.y + disp_h);

    // InvisibleButton over the whole img_w×img_h area to capture hover/drag
    ImGui::SetCursorScreenPos(img_pos);
    ImGui::InvisibleButton("##preview_img", ImVec2(img_w, img_h));
    const bool img_hov = ImGui::IsItemHovered();
    const bool img_act = ImGui::IsItemActive();

    ImDrawList* dl         = ImGui::GetWindowDrawList();
    const ImVec2 mouse     = ImGui::GetIO().MousePos;
    // Isotropic angular scale: horizontal ±0.5 rad fills width,
    // vertical range shrinks proportionally so pixels/rad is equal in both axes.
    const float max_angle_x = 0.5f;
    const float max_angle_y = max_angle_x * 9.0f / 16.0f;  // ≈ ±0.281 rad

    // Drag: update angles from mouse position within disp rect
    if (img_act && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float nx = (mouse.x - disp_tl.x) / disp_w;  // 0..1
        float ny = (mouse.y - disp_tl.y) / disp_h;  // 0..1 (screen Y down)
        nx = std::max(0.0f, std::min(1.0f, nx));
        ny = std::max(0.0f, std::min(1.0f, ny));
        const float new_ax = (nx - 0.5f) * 2.0f * max_angle_x;
        const float new_ay = -(ny - 0.5f) * 2.0f * max_angle_y; // flip Y
        if (std::abs(new_ax - prev_angle_x_) > 1e-4f ||
            std::abs(new_ay - prev_angle_y_) > 1e-4f) {
            prev_angle_x_ = new_ax;
            prev_angle_y_ = new_ay;
            if (!busy) need_render = true;
        }
    }

    // Draw texture (or placeholder)
    if (prev_tex_) {
        dl->AddImage((ImTextureID)(uintptr_t)prev_tex_,
                     disp_tl, disp_br,
                     ImVec2(0, 1), ImVec2(1, 0)); // flip Y
    } else {
        dl->AddRectFilled(disp_tl, disp_br, IM_COL32(10, 10, 10, 255));
        const char* wm = "Drag to set light position, then press Render";
        ImVec2 wms = ImGui::CalcTextSize(wm);
        dl->AddText(ImVec2(disp_tl.x + (disp_w - wms.x) * 0.5f,
                           disp_tl.y + (disp_h - wms.y) * 0.5f),
                    IM_COL32(60, 60, 60, 255), wm);
    }

    // Light source crosshair
    {
        const float sx = disp_tl.x + (prev_angle_x_ / max_angle_x * 0.5f + 0.5f) * disp_w;
        const float sy = disp_tl.y + (-prev_angle_y_ / max_angle_y * 0.5f + 0.5f) * disp_h;
        const float r  = 8.0f;
        const ImU32 col_outer = IM_COL32(0, 0, 0, 180);
        const ImU32 col_inner = IM_COL32(255, 220, 60, 255);
        // Shadow lines
        dl->AddLine(ImVec2(sx - r - 1, sy + 1), ImVec2(sx + r + 1, sy + 1), col_outer, 1.5f);
        dl->AddLine(ImVec2(sx + 1, sy - r - 1), ImVec2(sx + 1, sy + r + 1), col_outer, 1.5f);
        // Main lines
        dl->AddLine(ImVec2(sx - r, sy), ImVec2(sx + r, sy), col_inner, 1.5f);
        dl->AddLine(ImVec2(sx, sy - r), ImVec2(sx, sy + r), col_inner, 1.5f);
        dl->AddCircle(ImVec2(sx, sy), 4.0f, col_inner, 0, 1.5f);

        // Tooltip with current angles
        if (img_hov && std::hypot(mouse.x - sx, mouse.y - sy) < r * 2.5f) {
            ImGui::BeginTooltip();
            ImGui::Text("Light  X: %.3f rad  Y: %.3f rad", prev_angle_x_, prev_angle_y_);
            ImGui::TextDisabled("drag to reposition");
            ImGui::EndTooltip();
        }
    }

    // Crosshair cursor when hovering
    if (img_hov)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None); // we draw our own

    // Fire render after all sources of need_render (controls + drag) are resolved
    if (need_render && !busy)
        schedule_preview_render();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// draw_lens_diagram — bottom-right: 2D optical cross-section
//
// Coordinate system: lens Z axis maps to horizontal screen X.
//   - Each surface is drawn as an arc (or vertical line for flat).
//   - The aperture stop is tinted green.
//   - Sensor plane is shown as a dashed green line.
//   - Ray fan placeholder traces straight lines for now (Phase 3).
// ---------------------------------------------------------------------------
void App::draw_lens_diagram()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.08f, 0.06f, 1.0f));
    ImGui::Begin("Lens Diagram");
    ImGui::PopStyleColor();

    const ImVec2 avail  = ImGui::GetContentRegionAvail();
    ImDrawList*  dl     = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    if (!lens_loaded_) {
        const char* msg = "Load a lens to see the diagram";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - ts.x) * 0.5f,
                                   (avail.y - ts.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
        ImGui::End();
        return;
    }

    // ---- Coordinate mapping ----
    const float total_z = lens_.sensor_z * 1.1f;   // axial extent (mm)

    float max_ap = 1.0f;
    for (const Surface& s : lens_.surfaces)
        max_ap = std::max(max_ap, s.semi_aperture);
    const float total_h = max_ap * 2.0f * 1.15f;    // full height extent (mm)

    const float legend_h = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
    const float pad_x    = 16.0f;
    const float pad_y    = 10.0f;
    const float avail_w  = avail.x - 2.0f * pad_x;
    const float avail_h  = avail.y - 2.0f * pad_y - legend_h;

    // Uniform scale: pick the axis that constrains first, centre the other
    const float scale_x = avail_w / total_z;
    const float scale_y = avail_h / total_h;
    const float scale   = std::min(scale_x, scale_y);  // px per mm

    const float draw_w  = total_z * scale;              // actual pixel extents
    const float draw_h  = total_h * scale;
    const float off_x   = (avail_w - draw_w) * 0.5f;   // centering offsets
    const float off_y   = (avail_h - draw_h) * 0.5f;

    // max_ap still used for vertical half-range
    max_ap = total_h * 0.5f / 1.15f;

    auto to_screen = [&](float lz, float ly) -> ImVec2 {
        return ImVec2(
            origin.x + pad_x + off_x + (lz / total_z) * draw_w,
            origin.y + pad_y + off_y + draw_h * 0.5f - (ly / (total_h * 0.5f)) * (draw_h * 0.5f));
    };

    // Sag at height h for a surface (parametric arc formula)
    auto surf_sag = [](const Surface& s, float h) -> float {
        if (std::abs(s.radius) < 1e-5f) return 0.0f;
        const float absR = std::abs(s.radius);
        const float sinv = std::min(h / absR, 0.9999f);
        return s.radius * (1.0f - std::cos(std::asin(sinv)));
    };

    // ---- Register interactive canvas (InvisibleButton covers full padded area) ----
    ImGui::SetCursorScreenPos(ImVec2(origin.x + pad_x, origin.y + pad_y));
    ImGui::InvisibleButton("##canvas", ImVec2(avail_w, avail_h));
    const bool c_hovered = ImGui::IsItemHovered();
    const bool c_active  = ImGui::IsItemActive();

    const int    n        = lens_.num_surfaces();
    const ImVec2 mouse    = ImGui::GetMousePos();
    const float  handle_r = 7.0f; // pixel radius for handle hit-testing

    // ---- Interaction: find and start drag on click ----
    if (c_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int   best_surf = -1, best_type = -1;
        float best_dist = handle_r + 3.0f; // minimum snap distance

        for (int i = 0; i < n; ++i) {
            const Surface& s = lens_.surfaces[i];

            // Vertex handle: drag left/right → adjusts thickness of surface[i-1]
            if (i > 0) {
                ImVec2 v = to_screen(s.z, 0.0f);
                float  d = std::hypot(mouse.x - v.x, mouse.y - v.y);
                if (d < best_dist) { best_dist = d; best_surf = i; best_type = 0; }
            }
            // Tip handle: drag up/down → adjusts semi_aperture of surface[i]
            ImVec2 tip = to_screen(s.z + surf_sag(s, s.semi_aperture), s.semi_aperture);
            float  d   = std::hypot(mouse.x - tip.x, mouse.y - tip.y);
            if (d < best_dist) { best_dist = d; best_surf = i; best_type = 1; }
        }

        if (best_surf >= 0) {
            push_undo();           // snapshot before any mutation
            diag_selected_  = best_surf;
            diag_drag_surf_ = best_surf;
            diag_drag_type_ = best_type;
        } else {
            diag_selected_  = -1; // click on empty area = deselect
            diag_drag_surf_ = -1;
        }
    }

    // ---- Apply incremental mouse delta while dragging ----
    if (c_active && diag_drag_surf_ >= 0) {
        const ImVec2 md = ImGui::GetIO().MouseDelta;
        Surface& ds     = lens_.surfaces[diag_drag_surf_];

        if (diag_drag_type_ == 0) {
            // Vertex: adjust previous surface thickness (horizontal movement)
            float& t = lens_.surfaces[diag_drag_surf_ - 1].thickness;
            t = std::max(0.01f, t + (md.x / draw_w) * total_z);
            lens_.compute_geometry();
            dirty_ = true;
        } else {
            // Tip: adjust semi_aperture (vertical movement, screen Y is flipped)
            ds.semi_aperture = std::max(0.5f,
                ds.semi_aperture - (md.y / (draw_h * 0.5f)) * max_ap);
            dirty_ = true;
        }
    }

    // Release: clear drag (undo was already pushed at drag start)
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        diag_drag_surf_ = -1;
        diag_drag_type_ = -1;
    }

    // ---- Cursor feedback when hovering handles (but not mid-drag) ----
    if (c_hovered && diag_drag_surf_ < 0) {
        for (int i = 0; i < n; ++i) {
            const Surface& s = lens_.surfaces[i];
            if (i > 0) {
                ImVec2 v = to_screen(s.z, 0.0f);
                if (std::hypot(mouse.x - v.x, mouse.y - v.y) < handle_r + 2.0f) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    break;
                }
            }
            ImVec2 tip = to_screen(s.z + surf_sag(s, s.semi_aperture), s.semi_aperture);
            if (std::hypot(mouse.x - tip.x, mouse.y - tip.y) < handle_r + 2.0f) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                break;
            }
        }
    }

    // ---- Drawing ----
    // Background — outer available area
    dl->AddRectFilled(
        ImVec2(origin.x + pad_x, origin.y + pad_y),
        ImVec2(origin.x + pad_x + avail_w, origin.y + pad_y + avail_h),
        IM_COL32(8, 10, 8, 255));
    // Inner lens sub-rect (actual proportional drawing area)
    dl->AddRectFilled(
        ImVec2(origin.x + pad_x + off_x, origin.y + pad_y + off_y),
        ImVec2(origin.x + pad_x + off_x + draw_w, origin.y + pad_y + off_y + draw_h),
        IM_COL32(12, 16, 12, 255));

    // Optical axis
    dl->AddLine(to_screen(0.0f, 0.0f),
                to_screen(lens_.sensor_z * 1.05f, 0.0f),
                IM_COL32(40, 80, 40, 180), 1.0f);

    // Pass 1 — glass fills (drawn behind arcs)
    for (int i = 0; i < n; ++i) {
        const Surface& s = lens_.surfaces[i];
        if (i + 1 < n && s.ior > 1.001f) {
            const Surface& s2 = lens_.surfaces[i + 1];
            const float ap    = std::max(s.semi_aperture, s2.semi_aperture);
            ImVec2 a = to_screen(s.z,  ap);   // upper-left in screen space
            ImVec2 b = to_screen(s2.z, -ap);  // lower-right in screen space
            dl->AddRectFilled(
                ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)),
                ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)),
                IM_COL32(60, 100, 160, 30));
        }
    }

    // Pass 2 — surface arcs / lines
    for (int i = 0; i < n; ++i) {
        const Surface& s        = lens_.surfaces[i];
        const bool     selected = (i == diag_selected_);
        const ImU32    col      = selected  ? IM_COL32(255, 200,  50, 255)
                                : s.is_stop ? IM_COL32( 80, 220,  80, 255)
                                            : IM_COL32(100, 160, 220, 255);
        const float    lw       = selected ? 2.5f : 1.5f;

        if (std::abs(s.radius) < 1e-5f) {
            dl->AddLine(to_screen(s.z, +s.semi_aperture),
                        to_screen(s.z, -s.semi_aperture),
                        col, s.is_stop ? 2.5f : lw);
        } else {
            const float absR      = std::abs(s.radius);
            const float sin_max   = std::min(s.semi_aperture / absR, 0.9999f);
            const float theta_max = std::asin(sin_max);
            const int   segs      = 48;
            std::vector<ImVec2> pts;
            pts.reserve(segs + 1);
            for (int k = 0; k <= segs; ++k) {
                float theta = -theta_max + 2.0f * theta_max * k / (float)segs;
                pts.push_back(to_screen(
                    s.z + s.radius * (1.0f - std::cos(theta)),
                    std::abs(s.radius) * std::sin(theta)));
            }
            dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, lw);
        }
    }

    // Pass 3 — labels, handles, tooltips
    for (int i = 0; i < n; ++i) {
        const Surface& s        = lens_.surfaces[i];
        const bool     selected = (i == diag_selected_);
        const float    sag_top  = surf_sag(s, s.semi_aperture);
        const ImVec2   tip      = to_screen(s.z + sag_top, s.semi_aperture);
        const ImVec2   vertex   = to_screen(s.z, 0.0f);

        // Surface index label
        {
            char lbl[8];
            snprintf(lbl, sizeof(lbl), "%d", i);
            ImVec2 lbl_pos = { tip.x - 4.0f,
                               tip.y - ImGui::GetTextLineHeight() - 3.0f };
            dl->AddText(lbl_pos,
                selected ? IM_COL32(255, 200, 50, 255) : IM_COL32(130, 130, 130, 200),
                lbl);
        }

        // Vertex handle — drag left/right to adjust spacing (shown for i > 0)
        if (i > 0) {
            bool hov = c_hovered &&
                       std::hypot(mouse.x - vertex.x, mouse.y - vertex.y) < handle_r + 2.0f;
            ImU32 hc = hov || (diag_drag_surf_ == i && diag_drag_type_ == 0)
                       ? IM_COL32(255, 220, 80, 255) : IM_COL32(140, 170, 210, 180);
            dl->AddCircle(vertex, 4.5f, hc, 0, 1.5f);
        }

        // Tip handle — drag up/down to adjust semi_aperture
        {
            bool hov = c_hovered &&
                       std::hypot(mouse.x - tip.x, mouse.y - tip.y) < handle_r + 2.0f;
            ImU32 tc = hov || (diag_drag_surf_ == i && diag_drag_type_ == 1)
                       ? IM_COL32(255, 220, 80, 255) : IM_COL32(200, 140, 60, 200);
            dl->AddCircleFilled(tip, 4.0f, tc);
        }

        // Tooltip when hovering near any handle
        if (c_hovered) {
            bool near_v = (i > 0) &&
                          std::hypot(mouse.x - vertex.x, mouse.y - vertex.y) < handle_r + 8.0f;
            bool near_t = std::hypot(mouse.x - tip.x, mouse.y - tip.y) < handle_r + 8.0f;
            if (near_v || near_t) {
                ImGui::BeginTooltip();
                ImGui::Text("Surface %d", i);
                ImGui::Separator();
                if (s.is_stop) {
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Aperture Stop");
                } else {
                    ImGui::Text("R = %.4g mm", s.radius);
                }
                ImGui::Text("t = %.4g mm", s.thickness);
                ImGui::Text("n = %.5g", s.ior);
                if (s.abbe_v > 0.1f) ImGui::Text("V = %.2f", s.abbe_v);
                ImGui::Text("Semi-AP = %.4g mm", s.semi_aperture);
                ImGui::Separator();
                if (near_v && i > 0)
                    ImGui::TextDisabled("drag \xe2\x86\x94 to adjust spacing");
                else
                    ImGui::TextDisabled("drag \xe2\x86\x95 to adjust aperture");
                ImGui::EndTooltip();
            }
        }
    }

    // Sensor plane — dashed green vertical line
    {
        const float sz    = lens_.sensor_z;
        const float dash  = 6.0f;
        ImVec2      top   = to_screen(sz, +max_ap * 0.8f);
        ImVec2      bot   = to_screen(sz, -max_ap * 0.8f);
        float       total = bot.y - top.y;
        int         steps = (int)(total / (dash * 2.0f));
        for (int k = 0; k < steps; ++k) {
            float y0 = top.y + k * dash * 2.0f;
            dl->AddLine(ImVec2(top.x, y0), ImVec2(top.x, y0 + dash),
                        IM_COL32(80, 220, 80, 160), 1.5f);
        }
        dl->AddText(ImVec2(top.x + 3.0f, top.y), IM_COL32(80, 220, 80, 180), "sensor");
    }

    // ---- Legend ----
    ImGui::SetCursorScreenPos(
        ImVec2(origin.x + pad_x, origin.y + pad_y + avail_h + 4.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.6f, 0.9f, 1.0f));
    ImGui::Text("lens surfaces");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 20);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.85f, 0.3f, 1.0f));
    ImGui::Text("stop / sensor");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 20);
    ImGui::TextDisabled("\xe2\x97\x8b drag to space  \xe2\x97\x8f drag to aperture");

    ImGui::End();
}

// ---------------------------------------------------------------------------
// setup_default_dockspace — called once on first frame.
//
// Layout (matching the reference screenshot):
//
//   ┌──────────┬───────────────────────────────┐
//   │          │   Flare Preview               │
//   │  Surfaces│                               │
//   │  + Info  ├───────────────────────────────┤
//   │          │   Lens Diagram                │
//   └──────────┴───────────────────────────────┘
//
//  Left column  : ~28 % width
//  Right-top    : ~45 % height  → Flare Preview
//  Right-bottom : ~55 % height  → Lens Diagram
// ---------------------------------------------------------------------------
void App::setup_default_dockspace(ImGuiID ds_id)
{
    // Only build if no imgui.ini layout has been saved yet
    if (ImGui::DockBuilderGetNode(ds_id) != nullptr &&
        ImGui::DockBuilderGetNode(ds_id)->IsLeafNode() == false)
        return; // already has a layout from ini

    ImGui::DockBuilderRemoveNode(ds_id);
    ImGui::DockBuilderAddNode(ds_id, ImGuiDockNodeFlags_DockSpace);

    const ImVec2 sz = ImGui::GetMainViewport()->WorkSize;
    ImGui::DockBuilderSetNodeSize(ds_id, sz);

    // Split into left (surfaces/info) and right (preview + diagram)
    ImGuiID right_id, left_id;
    ImGui::DockBuilderSplitNode(ds_id, ImGuiDir_Left, 0.28f, &left_id, &right_id);

    // Split right into top (preview) and bottom (diagram)
    ImGuiID preview_id, diagram_id;
    ImGui::DockBuilderSplitNode(right_id, ImGuiDir_Up, 0.45f, &preview_id, &diagram_id);

    // Split left into top (info) and bottom (surfaces table)
    ImGuiID info_id, table_id;
    ImGui::DockBuilderSplitNode(left_id, ImGuiDir_Up, 0.25f, &info_id, &table_id);

    ImGui::DockBuilderDockWindow("Flare Preview", preview_id);
    ImGui::DockBuilderDockWindow("Lens Diagram",  diagram_id);
    ImGui::DockBuilderDockWindow("Lens Info",     info_id);
    ImGui::DockBuilderDockWindow("Surfaces",      table_id);

    ImGui::DockBuilderFinish(ds_id);
}
