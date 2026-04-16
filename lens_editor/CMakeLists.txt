cmake_minimum_required(VERSION 3.18)
project(FlareSim_LensEditor LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Match the plugin build conventions ----------------------------------------
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set(CMAKE_CUDA_RUNTIME_LIBRARY Shared)
else()
    set(CMAKE_CUDA_RUNTIME_LIBRARY Static)
endif()

set(CMAKE_CUDA_ARCHITECTURES all)

# ---------------------------------------------------------------------------
# Dependencies via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)

# GLFW 3.4
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# Dear ImGui — docking branch (has DockSpace / DockBuilder API)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# OpenGL
find_package(OpenGL REQUIRED)

# ---------------------------------------------------------------------------
# Optics core — shared .cpp files from the plugin's src/ directory.
# No Nuke headers are included by any of these files, so they build cleanly
# as a standalone static library.
# ---------------------------------------------------------------------------
set(OPTICS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_library(optics_core STATIC
    ${OPTICS_SRC}/lens.cpp
    ${OPTICS_SRC}/trace.cpp
    ${OPTICS_SRC}/ghost.cpp
    ${OPTICS_SRC}/starburst.cpp
    ${OPTICS_SRC}/ghost_cuda.cu
)

target_include_directories(optics_core PUBLIC "${OPTICS_SRC}")

if(WIN32)
    target_compile_definitions(optics_core PUBLIC
        NOMINMAX
        _USE_MATH_DEFINES
        WIN32_LEAN_AND_MEAN
    )
endif()

if(MSVC)
    target_compile_options(optics_core PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W3 /wd4996>
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W3>
    )
else()
    target_compile_options(optics_core PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>
    )
endif()

# ---------------------------------------------------------------------------
# ImGui — build the core + GLFW/OpenGL3 backends as a static library.
# imgui_impl_opengl3.cpp uses its bundled mini GL loader
# (imgui_impl_opengl3_loader.h) so no separate glad/glew is needed.
# ---------------------------------------------------------------------------
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

if(WIN32)
    target_compile_definitions(imgui_lib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# Editor executable
# ---------------------------------------------------------------------------
add_executable(FlareSim_LensEditor
    src/main.cpp
    src/app.cpp
)

target_include_directories(FlareSim_LensEditor PRIVATE src)

target_link_libraries(FlareSim_LensEditor PRIVATE
    optics_core
    imgui_lib
    glfw
    OpenGL::GL
)

if(UNIX AND NOT APPLE)
    target_link_libraries(FlareSim_LensEditor PRIVATE dl pthread)
endif()

if(WIN32)
    target_compile_definitions(FlareSim_LensEditor PRIVATE
        NOMINMAX
        _USE_MATH_DEFINES
        WIN32_LEAN_AND_MEAN
    )
endif()

if(MSVC)
    target_compile_options(FlareSim_LensEditor PRIVATE /W3 /wd4996)
endif()

# Put the binary next to the CMakeLists for easy access
set_target_properties(FlareSim_LensEditor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)
