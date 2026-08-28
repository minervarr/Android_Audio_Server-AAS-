# ── AOAS console (desktop build) ─────────────────────────────────────────────
#
# The PHONE build is native/CMakeLists.txt, driven by Gradle. This one exists so
# the console's UI can be developed without an install cycle: the same
# ConsoleApp and the same draw_console() over a Wayland window, fed by a
# FakeSource instead of a Binder connection to AoasService.
#
# It deliberately does NOT build the service. Nothing here links ae_usb, libusb
# or any JNI: if a UI file ever starts needing one of those, this build breaks,
# which is exactly the guard that keeps drawing code out of the audio path.
cmake_minimum_required(VERSION 3.22)
project(aoas_console CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AOAS_ROOT      ${CMAKE_CURRENT_SOURCE_DIR})
set(FIRSTPARTY_DIR ${AOAS_ROOT}/firstparty)

# app_shell defaults VK_CANVAS_DIR to ../vk_canvas; ours is checked out under
# its upstream name, so it has to be told. Set BEFORE add_subdirectory.
set(VK_CANVAS_DIR ${FIRSTPARTY_DIR}/Vk_Canvas_Lb_LAW CACHE PATH "" FORCE)

set(APP_SHELL_APP_NAME     "aoas"        CACHE STRING "" FORCE)
set(APP_SHELL_WINDOW_TITLE "AOAS console" CACHE STRING "" FORCE)
set(APP_SHELL_APP_ID       "io.nava.aoas" CACHE STRING "" FORCE)

add_subdirectory(${VK_CANVAS_DIR}/core           ${CMAKE_BINARY_DIR}/vk_canvas_core)
add_subdirectory(${VK_CANVAS_DIR}/platform/linux ${CMAKE_BINARY_DIR}/vk_canvas_wayland)
add_subdirectory(${FIRSTPARTY_DIR}/App_shell     ${CMAKE_BINARY_DIR}/app_shell)

# The legibility floor is derived from the faces the APP ships — list exactly
# the one it loads, or the console can render below its own floor.
include(AppShellMinTextSize)
app_shell_generate_min_text_size(${AOAS_ROOT}/assets/fonts/ui.otf)
add_dependencies(app_shell generate_ui_min_text_size)

add_executable(aoas_console
    native/ui/console_app.cc
    native/ui/console_draw.cc
    native/ui/desktop_main.cc
    native/ui/fake_source.cc)
add_dependencies(aoas_console generate_ui_min_text_size)

# Shaders and the font, beside the binary: FileAssetReader roots itself at
# <exe dir>/assets/, so this IS the install layout.
list(APPEND CMAKE_MODULE_PATH ${AOAS_ROOT}/cmake)
include(AoasAssets)
find_program(VCE_SLANGC slangc HINTS /opt/shader-slang/bin $ENV{VULKAN_SDK}/bin REQUIRED)
aoas_stage_shaders(aoas_console ${CMAKE_BINARY_DIR}/assets)
aoas_stage_font(aoas_console ${CMAKE_BINARY_DIR}/assets)

target_include_directories(aoas_console PRIVATE ${AOAS_ROOT}/native/ui)
target_link_libraries(aoas_console PRIVATE app_shell app_shell_wayland vk_canvas_wayland)
