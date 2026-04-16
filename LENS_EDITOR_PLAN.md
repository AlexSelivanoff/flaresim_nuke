# Plan: FlareSim Lens Editor

## Summary
Standalone visual editor for .lens files with real-time CUDA raytraced flare preview.

## GUI Library Decision
**Recommended: Dear ImGui + GLFW + OpenGL**

Rationale:
- Immediate-mode GUI ideal for real-time parameter editing
- GLFW: single small DLL, cross-platform (Win/Linux/macOS)
- OpenGL: mature CUDA interop (cudaGraphicsGLRegisterImage)
- MIT licensed, open source friendly
- Minimal dependency footprint
- Large community, well-documented

Alternative considered:
- Qt: heavy, licensing complexity (LGPL dynamic linking requirements)
- SDL2: viable but heavier than GLFW for this use case

## Architecture

```
lens_editor/
├── CMakeLists.txt
├── src/
│   ├── main.cpp              # Entry point, window setup
│   ├── app.cpp/h             # Application state, main loop
│   ├── ui/
│   │   ├── lens_editor.cpp/h    # Surface table UI, parameter sliders
│   │   ├── lens_diagram.cpp/h   # 2D cross-section view
│   │   ├── preview_panel.cpp/h  # Flare preview rendering
│   │   └── file_browser.cpp/h   # Open/save dialogs
│   ├── renderer/
│   │   ├── cuda_renderer.cpp/h  # CUDA flare computation
│   │   └── gl_display.cpp/h     # OpenGL texture display + CUDA interop
│   └── core/                     # Extracted from plugin
│       ├── lens.cpp/h           # Lens file I/O (copy from plugin)
│       ├── trace.cpp/h          # Ray tracing logic
│       ├── fresnel.h            # Optical calculations
│       └── ghost_cuda.cu/h      # CUDA kernel (adapted)
├── external/
│   ├── imgui/                   # Dear ImGui (vendored)
│   └── glfw/                    # GLFW (submodule or find_package)
└── resources/
    └── lenses/                  # Bundled .lens files
```

## Implementation Steps

### Phase 1: Project Scaffold & Core Extraction (~2-3 days)
1. Create lens_editor/ directory structure
2. Extract standalone optics code from plugin:
   - Copy lens.cpp/h, trace.cpp/h, fresnel.h, ghost_cuda.cu/h
   - Remove Nuke dependencies (replace Op::message with logging)
3. Set up CMakeLists.txt with CUDA, GLFW, OpenGL
4. Add ImGui as vendored source (no DLL needed)
5. Build verification on Windows (VS2022) and Linux (GCC 11+)

### Phase 2: Basic Window & Lens Loading (~3-4 days)
6. Implement GLFW window creation + ImGui integration
7. Build lens file browser UI (open/save dialogs)
8. Create surface table editor:
   - Editable grid: radius, thickness, ior, abbe, semi_ap, coating, is_stop
   - Add/remove surface buttons
   - Undo/redo stack for edits
9. Recent files list

### Phase 3: 2D Lens Diagram (~2-3 days)
10. Implement lens cross-section renderer:
    - Draw surfaces as arcs (using radius + z position)
    - Show aperture stop location
    - Interactive drag to move/resize elements
    - Color-code by coating type
11. Bi-directional sync between diagram and table

### Phase 4: CUDA Preview Integration (~3-4 days)
12. Set up CUDA-OpenGL interop:
    - cudaGraphicsGLRegisterImage for output texture
    - Map/unmap cycle per frame
13. Adapt ghost_cuda kernel for standalone use:
    - Single point source at configurable angle
    - Remove Nuke-specific callbacks
    - Output directly to CUDA-mapped GL texture
14. Implement preview panel with:
    - Light position slider (angle from axis)
    - Light color picker
    - Wavelength/spectral sampling controls
    - Resolution/quality presets

### Phase 5: Polish & Release Prep (~3-4 days)
15. Bundled lens library browser (scan resources/lenses/ directory)
16. Export to Zemax (.zmx) and Code V (.seq) formats
17. File validation & error reporting
18. Add keyboard shortcuts (Ctrl+S save, Ctrl+Z undo)
19. Settings persistence (last directory, window layout)
20. Build scripts for Windows/Linux releases
21. Documentation & README

## Relevant Files to Extract/Reuse

From existing plugin:
- src/lens.cpp — Lens file parsing, LensSystem structure
- src/lens.h — Surface struct definition
- src/trace.cpp — Ray-surface intersection
- src/fresnel.h — Fresnel equations, AR coating, dispersion
- src/ghost_cuda.cu — CUDA kernel (adapt for standalone)
- src/ghost_cuda.h — GpuBufferCache pattern
- lenses/lens_files/ — 100+ lens presets to bundle

## Verification Steps

1. **Build verification**: Compile on Windows (VS2022) and Linux (GCC 11+)
2. **Load/save test**: Round-trip .lens files, diff against originals
3. **Visual regression**: Compare preview output to Nuke plugin output
4. **Performance**: 60fps UI with 512x512 preview on GTX 1070+
5. **Cross-platform**: Test on Windows 10/11, Ubuntu 22.04, (optionally macOS)
6. **Export validation**: Load generated Zemax/Code V files in those tools

## Dependencies Summary

| Dependency | Version | License | Distribution |
|------------|---------|---------|--------------|
| Dear ImGui | 1.91+   | MIT     | Vendored source |
| GLFW       | 3.4+    | Zlib    | Static link or system |
| CUDA       | 12.0+   | NVIDIA  | Runtime DLL (cudart) |
| OpenGL     | 3.3+    | -       | System |

## Decisions (Confirmed)

- **No CPU fallback** — CUDA required (VFX hardware assumed)
- **Bundle lens library** — Include existing 100+ .lens files with browser
- **Add export formats** — Support Zemax and Code V export (Phase 5)
