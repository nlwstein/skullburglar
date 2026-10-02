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
    src/gfx/shadow.cpp
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

# Native renderer (src/gfx/vk): bots-replay, and in-process shadow mode (BOTS_GFX_SHADOW) in the game where enabled.
# Needs the SDK source tree: the Xenos register and shader-translation code it reuses lives in the rexgpu-xenos
# plugin, a shared library that exports nothing, so it is compiled in again here.
if(REXSDK_DIR AND NOT WIN32)
    set(REX_GRAPHICS_SRC ${REXSDK_DIR}/src/graphics)
    set(BOTS_NATIVE_GFX_SOURCES
        src/gfx/pm4.cpp
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
    set(BOTS_NATIVE_GFX_LIBS Vulkan::Headers GPUOpen::VulkanMemoryAllocator
        xxHash::xxhash glslang::SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler)

    add_executable(bots-replay src/gfx/replay/capture_file.cpp src/gfx/replay/main.cpp ${BOTS_NATIVE_GFX_SOURCES})
    target_link_libraries(bots-replay PRIVATE rexruntime ${BOTS_NATIVE_GFX_LIBS})
    target_include_directories(bots-replay PRIVATE ${REXSDK_DIR}/thirdparty/renderdoc)  # rex/ui/vulkan headers

    # In the game: Linux first (the shadow-mode test host); macOS and Windows once it is proven there.
    option(BOTS_NATIVE_GFX "Build the native renderer into the game (shadow mode)" ${LINUX})
    if(BOTS_NATIVE_GFX)
        target_sources(bots PRIVATE ${BOTS_NATIVE_GFX_SOURCES})
        target_link_libraries(bots PRIVATE ${BOTS_NATIVE_GFX_LIBS})
        target_include_directories(bots PRIVATE ${REXSDK_DIR}/thirdparty/renderdoc)
        target_compile_definitions(bots PRIVATE BOTS_NATIVE_GFX=1)
        # The SDK sources are not written against the game's precompiled header.
        set_source_files_properties(${BOTS_NATIVE_GFX_SOURCES} TARGET_DIRECTORY bots
            PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()

# bots_register.cpp is one function with ~143k SetFunction() calls (runs once at startup).
# Clang goes superlinear on it: >25 min at -O2 -g, and >16 min at -O0 too, where arm64 uses
# GlobalISel and its Localizer pass is quadratic here. -O0 with SelectionDAG avoids both.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/default/bots_register.cpp
    PROPERTIES COMPILE_OPTIONS "-O0;-g0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)  # PCH is built -O2; clang rejects the mismatch
