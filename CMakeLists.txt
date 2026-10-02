# bots - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(bots LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BOTS_SOURCES
    src/main.cpp
    src/game_hooks.cpp
    src/gfx/d3d_hooks.cpp
    src/gfx/capture.cpp
    src/setup/iso_extract.cpp
    src/setup/settings.cpp
)
# First-run setup window's native file picker (B-061).
if(WIN32)
    list(APPEND BOTS_SOURCES src/setup/file_picker_win.cpp)
elseif(APPLE)
    enable_language(OBJCXX)
    list(APPEND BOTS_SOURCES src/setup/file_picker_mac.mm)
else()
    list(APPEND BOTS_SOURCES src/setup/file_picker_linux.cpp)
endif()

if(WIN32)
    list(APPEND BOTS_SOURCES src/crash_win.cpp)
    add_executable(bots WIN32 ${BOTS_SOURCES})
else()
    add_executable(bots ${BOTS_SOURCES})
endif()

rexglue_setup_target(bots GPU_PLUGINS xenos)
if(APPLE)
    target_link_libraries(bots PRIVATE "-framework AppKit")
    # The precompiled header is C++ only.
    set_source_files_properties(src/setup/file_picker_mac.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
elseif(WIN32)
    target_link_libraries(bots PRIVATE ole32 shell32)
endif()

# SDK built from source (REXSDK_DIR, the Windows cross build): rex_app.cpp includes imgui.h, which
# rexui only exposes through its own link interface. Headers only - the objects stay in the SDK.
if(TARGET imgui)
    target_include_directories(bots PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# bots-replay: offline renderer for D3D frame captures (native renderer, P2). Needs the SDK source tree: the
# Xenos register and shader-translation code it reuses lives in the rexgpu-xenos plugin, which exports nothing.
if(REXSDK_DIR AND NOT WIN32)
    set(REX_GRAPHICS_SRC ${REXSDK_DIR}/src/graphics)
    add_executable(bots-replay
        src/gfx/pm4.cpp
        src/gfx/replay/capture_file.cpp
        src/gfx/replay/main.cpp
        src/gfx/vk/draw.cpp
        src/gfx/vk/renderer.cpp
        src/gfx/vk/shaders.cpp
        src/gfx/vk/texture.cpp
        src/gfx/vk/transfer.cpp
        ${REX_GRAPHICS_SRC}/register_file.cpp
        ${REX_GRAPHICS_SRC}/registers.cpp
        # Xenos microcode -> SPIR-V
        ${REX_GRAPHICS_SRC}/flags.cpp
        ${REX_GRAPHICS_SRC}/format/ucode.cpp
        ${REX_GRAPHICS_SRC}/sampler_info.cpp
        ${REX_GRAPHICS_SRC}/pipeline/texture/conversion.cpp
        ${REX_GRAPHICS_SRC}/pipeline/texture/extent.cpp
        ${REX_GRAPHICS_SRC}/pipeline/texture/info.cpp
        ${REX_GRAPHICS_SRC}/pipeline/texture/info_formats.cpp
        ${REX_GRAPHICS_SRC}/pipeline/texture/util.cpp
        ${REX_GRAPHICS_SRC}/util/draw.cpp
        ${REX_GRAPHICS_SRC}/pipeline/shader/shader.cpp
        ${REX_GRAPHICS_SRC}/pipeline/shader/translator.cpp
        ${REX_GRAPHICS_SRC}/pipeline/shader/translator_disasm.cpp
        ${REX_GRAPHICS_SRC}/pipeline/shader/spirv.cpp
        ${REX_GRAPHICS_SRC}/pipeline/shader/spirv_builder.cpp
        ${REX_GRAPHICS_SRC}/pipeline/shader/spirv_translator.cpp
        ${REX_GRAPHICS_SRC}/pipeline/shader/spirv_translator_alu.cpp
        ${REX_GRAPHICS_SRC}/pipeline/shader/spirv_translator_fetch.cpp
        ${REX_GRAPHICS_SRC}/pipeline/shader/spirv_translator_memexport.cpp
        ${REX_GRAPHICS_SRC}/pipeline/shader/spirv_translator_rb.cpp
        ${REX_GRAPHICS_SRC}/vulkan/shader.cpp
    )
    target_link_libraries(bots-replay PRIVATE rexruntime Vulkan::Headers GPUOpen::VulkanMemoryAllocator
        xxHash::xxhash glslang::SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler)
    target_include_directories(bots-replay PRIVATE ${REXSDK_DIR}/thirdparty/renderdoc)  # rex/ui/vulkan headers
endif()

# bots_register.cpp is one function with ~143k SetFunction() calls (runs once at startup).
# Clang goes superlinear on it: >25 min at -O2 -g, and >16 min at -O0 too, where arm64 uses
# GlobalISel and its Localizer pass is quadratic here. -O0 with SelectionDAG avoids both.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/default/bots_register.cpp
    PROPERTIES COMPILE_OPTIONS "-O0;-g0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)  # PCH is built -O2; clang rejects the mismatch
