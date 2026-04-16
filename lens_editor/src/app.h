// ============================================================================
// app.h — FlareSim Lens Editor application state
// ============================================================================
#pragma once

#include "imgui.h"      // ImGuiID and all public ImGui types
#include "lens.h"
#include "ghost.h"      // GhostConfig, GhostPair, BrightPixel
#include "ghost_cuda.h" // GpuBufferCache, launch_ghost_cuda

#include <string>
#include <vector>
#include <deque>
#include <atomic>
#include <mutex>
#include <thread>

struct GLFWwindow;

class App {
public:
    bool init();
    void run();
    void shutdown();
    void load_lens(const char* path);

private:
    // ---- UI panels ----
    void draw_menu_bar();
    void draw_info_panel();
    void draw_surface_table();
    void draw_flare_preview();
    void draw_lens_diagram();
    void draw_status_bar();

    void setup_default_dockspace(ImGuiID ds_id);

    // ---- File helpers ----
    bool pick_lens_file_dialog();
    void save_lens(const char* path);

    // ---- Undo/redo ----
    void push_undo();   // snapshot current lens_ before a mutation
    void undo();
    void redo();

    // ---- Preview render ----
    void schedule_preview_render();  // kick off async GPU render
    void upload_preview_texture();   // upload pixel_ buf to GL (main thread only)
    void alloc_preview_texture(int w, int h); // (re)create GL texture

    // ---- State ----
    GLFWwindow* window_    = nullptr;
    LensSystem  lens_;
    std::string lens_path_;

    // Undo/redo stacks (full LensSystem snapshots — cheap for small prescriptions)
    static constexpr int kMaxUndo = 64;
    std::deque<LensSystem> undo_stack_;
    std::deque<LensSystem> redo_stack_;
    bool   dirty_          = false;   // unsaved changes

    // Recent files
    static constexpr int kMaxRecent = 10;
    std::vector<std::string> recent_files_;

    // UI buffers
    char   path_buf_[2048] = {};
    bool   lens_loaded_    = false;
    bool   show_open_      = false;
    bool   show_save_as_   = false;
    bool   dockspace_built_ = false;
    char   status_msg_[256] = "No lens loaded.";

    // Diagram interaction (Phase 3)
    int    diag_selected_  = -1;   // highlighted surface index (-1 = none)
    int    diag_drag_surf_ = -1;   // surface currently being dragged
    int    diag_drag_type_ = -1;   // 0 = vertex (thickness), 1 = tip (semi_aperture)

    // ---- Preview (Phase 4) ----
    // Controls (UI-editable)
    float  prev_angle_x_   = 0.10f;  // horizontal light angle (radians)
    float  prev_angle_y_   = 0.06f;  // vertical light angle (radians)
    float  prev_gain_      = 800.0f; // ghost gain multiplier
    int    prev_ray_grid_  = 128;     // ray grid size (samples per dim)
    int    prev_spec_samp_ = 5;      // spectral sample count (3/5/7/9/11)
    int    prev_blades_    = 0;      // aperture blades (0=circular)
    float  prev_blade_rot_ = 0.0f;   // aperture rotation degrees
    int    prev_res_       = 512;    // render resolution (square)
    float  prev_ev_        = 14.0f;  // source EV (log2 of HDR intensity)
    bool   prev_dirty_     = false;  // needs re-render

    // GL texture
    unsigned int prev_tex_   = 0;
    int          prev_tex_w_ = 0;
    int          prev_tex_h_ = 0;

    // Async render thread
    std::thread              prev_thread_;
    std::atomic<bool>        prev_rendering_{false};
    std::atomic<bool>        prev_result_ready_{false};
    std::mutex               prev_mutex_;
    std::vector<float>       prev_rgba_buf_;  // w×h×4 RGBA float (protected by prev_mutex_)
    int                      prev_buf_w_ = 0, prev_buf_h_ = 0;

    // Persistent GPU buffer cache (reused across renders)
    GpuBufferCache           prev_gpu_cache_;

    // Copy of lens + config sent to the render thread (captured at schedule time)
    LensSystem               prev_render_lens_;
    GhostConfig              prev_render_cfg_;
    float                    prev_render_shw_ = 0.0f; // sensor half-width (mm)
    float                    prev_render_shh_ = 0.0f; // sensor half-height (mm)
    int                      prev_render_res_ = 0;
    float                    prev_render_gain_= 0.0f;
    float                    prev_render_ev_  = 0.0f;
};
