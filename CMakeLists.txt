# bots - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(bots LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Before the SDK: built from source (REXSDK_DIR), its SDL compiles Objective-C, which otherwise gets C++ flags.
if(APPLE)
    enable_language(C OBJC OBJCXX)
endif()

# The SDK compiles Tracy zones and its perf counters into every non-Release build; the counters add an atomic
# increment to every guest indirect call (~3% of the guest main thread on the Deck). Off unless asked for.
option(BOTS_PROFILING "Build the SDK from source with Tracy and its perf counters" OFF)
if(NOT BOTS_PROFILING)
    set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
    set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "" FORCE)
endif()

# Release builds compile through ccache (tools/ci/build.sh). Precompiled headers then must not record header mtimes,
# or ccache can't reuse them; it also needs the sloppiness build.sh sets.
option(BOTS_CCACHE "Compile through ccache" OFF)
if(BOTS_CCACHE)
    find_program(CCACHE_PROGRAM ccache REQUIRED)
    foreach(lang C CXX OBJC OBJCXX)
        set(CMAKE_${lang}_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    endforeach()
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Xclang=-fno-pch-timestamp>)
endif()

include(generated/rexglue.cmake)

# SDK built from source (REXSDK_DIR): its macOS helper stages MoltenVK's ICD file from paths that only the installed
# package's config defines. Without them the post-build copy reads "/cmake/MoltenVK_icd.json" and fails.
if(APPLE AND REXSDK_DIR)
    set(REXGLUE_ROOT ${REXSDK_DIR})
    set(REXGLUE_MOLTENVK_ICD ${REXSDK_DIR}/cmake/MoltenVK_icd.json)
endif()

# Sources
set(BOTS_SOURCES
    src/main.cpp
    src/coop.cpp
    src/game_hooks.cpp
    src/aim_assist.cpp
    src/mnk_input.cpp
    src/motion_blur.cpp
    src/hybrid.cpp
    src/instr.cpp
    src/idle_backoff.cpp
    src/scene_feed.cpp
    src/slow_frame.cpp
    src/level_stream.cpp
    src/gc.cpp
    src/widescreen.cpp
    src/gfx/d3d_hooks.cpp
    src/gfx/capture.cpp
    src/gfx/shadow.cpp
    src/setup/iso_extract.cpp
    src/setup/settings.cpp
    src/setup/update_check.cpp
)
# Release builds carry their version and where to check for a newer one (src/setup/update_check.h); the release CI
# sets both. Only that file sees them, so a new version recompiles one file.
set(BOTS_VERSION "dev" CACHE STRING "Version shown in the launcher")
set(BOTS_UPDATE_URL "" CACHE STRING "Address of the releases JSON the launcher checks (empty: no check)")
set_source_files_properties(src/setup/update_check.cpp PROPERTIES
    COMPILE_DEFINITIONS "BOTS_VERSION=\"${BOTS_VERSION}\";BOTS_UPDATE_URL=\"${BOTS_UPDATE_URL}\""
    SKIP_PRECOMPILE_HEADERS ON)
# First-run setup window's native file picker (B-061).
if(WIN32)
    list(APPEND BOTS_SOURCES src/setup/file_picker_win.cpp src/setup/firewall_win.cpp)
elseif(CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS)$")
    # Experimental iOS and tvOS builds (tools/ios/build.sh): no launcher; on iOS an on-screen controller.
    enable_language(OBJCXX)
    list(APPEND BOTS_SOURCES src/setup/file_picker_ios.cpp src/ios/ios_app.mm src/ios/game_import.mm)
elseif(APPLE)
    enable_language(OBJCXX)
    list(APPEND BOTS_SOURCES src/setup/file_picker_mac.mm)
elseif(ANDROID)
    # Experimental Android build (tools/android/build.sh): no launcher, like iOS; the app's Java screens call into
    # src/android.
    list(APPEND BOTS_SOURCES src/setup/file_picker_android.cpp src/android/android_app.cpp)
else()
    list(APPEND BOTS_SOURCES src/setup/file_picker_linux.cpp)
endif()

if(WIN32)
    list(APPEND BOTS_SOURCES src/crash_win.cpp src/cpu_sampler_win.cpp src/display_awake_win.cpp)
    add_executable(bots WIN32 ${BOTS_SOURCES})
elseif(ANDROID)
    # SDL's Java activity (tools/android) loads the game as libmain.so and calls its SDL_main.
    add_library(bots SHARED ${BOTS_SOURCES})
    set_target_properties(bots PROPERTIES OUTPUT_NAME main)
else()
    add_executable(bots ${BOTS_SOURCES})
endif()

rexglue_setup_target(bots GPU_PLUGINS xenos)
# Experiments with the recompiled code's flags (-O3, LTO, PGO, memory-access modes) without rebuilding the SDK:
#   -DBOTS_RECOMP_EXTRA_OPTIONS="-DREX_MEM_PLAIN;-O3"  -DBOTS_EXTRA_LINK_OPTIONS="-flto=thin"
set(BOTS_RECOMP_EXTRA_OPTIONS "" CACHE STRING "Extra compile options for the recompiled guest code (; separated)")
set(BOTS_EXTRA_LINK_OPTIONS "" CACHE STRING "Extra link options for the game executable (; separated)")
if(TARGET bots_recomp AND BOTS_RECOMP_EXTRA_OPTIONS)
    target_compile_options(bots_recomp PRIVATE ${BOTS_RECOMP_EXTRA_OPTIONS})
endif()
if(BOTS_EXTRA_LINK_OPTIONS)
    target_link_options(bots PRIVATE ${BOTS_EXTRA_LINK_OPTIONS})
endif()
if(CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS)$")
    target_link_libraries(bots PRIVATE "-framework UIKit" "-framework GameController" "-framework Network" "-framework CoreImage"
        "-framework UniformTypeIdentifiers")
    target_include_directories(bots PRIVATE src)
    set_source_files_properties(src/ios/ios_app.mm src/ios/game_import.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS -fobjc-arc)
    # The app bundle (tools/ios/build.sh) keeps the dylibs next to the executable, as on macOS.
    set_target_properties(bots PROPERTIES INSTALL_RPATH "@executable_path" BUILD_WITH_INSTALL_RPATH ON)
elseif(APPLE)
    target_link_libraries(bots PRIVATE "-framework AppKit")
    # The precompiled header is C++ only.
    set_source_files_properties(src/setup/file_picker_mac.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
elseif(ANDROID)
    target_link_libraries(bots PRIVATE android log)
elseif(WIN32)
    target_link_libraries(bots PRIVATE ole32 oleaut32 shell32 winhttp ws2_32)
    # winsock2.h has to come before windows.h, which the precompiled header pulls in.
    set_source_files_properties(src/setup/firewall_win.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# SDK built from source (REXSDK_DIR, the Windows cross build): rex_app.cpp includes imgui.h, which
# rexui only exposes through its own link interface. Headers only - the objects stay in the SDK.
if(TARGET imgui)
    target_include_directories(bots PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Native renderer (src/gfx/vk): bots-replay, and in-process shadow mode (BOTS_GFX_SHADOW) in the game where enabled.
# Needs the SDK source tree: the Xenos register and shader-translation code it reuses lives in the rexgpu-xenos
# plugin, a shared library that exports nothing, so it is compiled in again here. On Windows the SDK builds Vulkan
# only when asked (-DREXGLUE_USE_VULKAN=ON; D3D12 is its default there).
if(REXSDK_DIR AND (NOT WIN32 OR REXGLUE_USE_VULKAN))
    set(REX_GRAPHICS_SRC ${REXSDK_DIR}/src/graphics)

    # Keyboard/mouse and PlayStation prompt glyphs (assets/prompts: Kenney Input Prompts, CC0) compiled in as byte
    # arrays. Written through configure_file so an unchanged sheet doesn't rebuild.
    set(_glyphs_src "#include <cstddef>\n")
    foreach(_sheet keyboard-mouse:PromptGlyphs playstation:PlayStationGlyphs)
        string(REPLACE ":" ";" _sheet "${_sheet}")
        list(GET _sheet 0 _name)
        list(GET _sheet 1 _symbol)
        foreach(_kind Png Xml)
            string(TOLOWER ${_kind} _ext)
            set(_file ${CMAKE_CURRENT_SOURCE_DIR}/assets/prompts/${_name}.${_ext})
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_file})
            file(READ ${_file} _hex HEX)
            string(LENGTH "${_hex}" _len)
            math(EXPR _len "${_len} / 2")
            string(REGEX REPLACE "(..)" "0x\\1," _hex "${_hex}")
            string(APPEND _glyphs_src "extern const unsigned char k${_symbol}${_kind}[] = {${_hex}};\n"
                "extern const size_t k${_symbol}${_kind}Size = ${_len};\n")
        endforeach()
    endforeach()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/prompt_glyphs.cpp.in "${_glyphs_src}")
    configure_file(${CMAKE_CURRENT_BINARY_DIR}/prompt_glyphs.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/prompt_glyphs.cpp
        COPYONLY)
    set_source_files_properties(src/gfx/prompt_atlas.cpp PROPERTIES INCLUDE_DIRECTORIES ${REXSDK_DIR}/thirdparty/stb)

    set(BOTS_NATIVE_GFX_SOURCES
        src/gfx/prompt_atlas.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/prompt_glyphs.cpp
        src/gfx/pm4.cpp
        src/gfx/vk/draw.cpp
        src/gfx/vk/manifest.cpp
        src/gfx/vk/occlusion.cpp
        src/gfx/vk/present.cpp
        src/gfx/vk/renderer.cpp
        src/gfx/vk/shaders.cpp
        src/gfx/vk/texture.cpp
        src/gfx/vk/transfer.cpp
        src/gfx/vk/hybrid_fill.cpp
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
    # These two define GPU cvars, which belong to whichever GPU plugin is loaded: in our binaries they keep their
    # storage and defaults but are not registered (src/gfx/sdk_cvars.h).
    set_source_files_properties(${REX_GRAPHICS_SRC}/flags.cpp ${REX_GRAPHICS_SRC}/util/draw.cpp
        PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/sdk_cvars.h")
    set(BOTS_NATIVE_GFX_LIBS Vulkan::Headers GPUOpen::VulkanMemoryAllocator
        xxHash::xxhash glslang::SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler)

    if(NOT WIN32)  # the capture reader maps files with POSIX mmap
        add_executable(bots-replay src/gfx/replay/capture_file.cpp src/gfx/replay/main.cpp ${BOTS_NATIVE_GFX_SOURCES})
        target_link_libraries(bots-replay PRIVATE rexruntime ${BOTS_NATIVE_GFX_LIBS})
        target_include_directories(bots-replay PRIVATE ${REXSDK_DIR}/thirdparty/renderdoc)  # rex/ui/vulkan headers
    endif()

    # In the game: Linux first (the shadow-mode test host); macOS and Windows once it is proven there.
    option(BOTS_NATIVE_GFX "Build the native renderer into the game (shadow mode)" ${LINUX})
    if(BOTS_NATIVE_GFX)
        # Live guest memory (shadow live mode, then the cutover): the SDK's page tracking.
        set(BOTS_NATIVE_GFX_GAME_SOURCES ${BOTS_NATIVE_GFX_SOURCES}
            src/gfx/vk/memory_sync.cpp
            ${REX_GRAPHICS_SRC}/shared_memory.cpp)
        target_sources(bots PRIVATE ${BOTS_NATIVE_GFX_GAME_SOURCES})
        target_link_libraries(bots PRIVATE ${BOTS_NATIVE_GFX_LIBS})
        target_include_directories(bots PRIVATE ${REXSDK_DIR}/thirdparty/renderdoc)
        target_compile_definitions(bots PRIVATE BOTS_NATIVE_GFX=1)
        if(APPLE)
            # The GPU plugins compile glslang in too; dyld would bind their weak glslang template instances to the
            # game's (the first image exporting them), mixing two pool allocators: a crash on the first shader.
            target_link_options(bots PRIVATE LINKER:-unexported_symbol,*glslang*)
        endif()
        # The SDK sources are not written against the game's precompiled header.
        set_source_files_properties(${BOTS_NATIVE_GFX_GAME_SOURCES} TARGET_DIRECTORY bots
            PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

        # Native mode (B-075): the renderer as a GPU plugin on the SDK's command processor, staged next to the game
        # under the name the plugin loader expects (librexgpu-native[d|rd].so/.dylib, rexgpu-native[d|rd].dll).
        # REX_GPU_PLUGIN=native selects it.
        add_library(rexgpu-native SHARED
            src/gfx/native.cpp
            src/gfx/host_stats.cpp
            src/gfx/vk/memory_sync.cpp
            src/upk.cpp
            ${BOTS_NATIVE_GFX_SOURCES}
            ${REX_GRAPHICS_SRC}/shared_memory.cpp
            ${REX_GRAPHICS_SRC}/graphics_system.cpp
            ${REX_GRAPHICS_SRC}/command_processor.cpp)
        set_target_properties(rexgpu-native PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
            DEBUG_POSTFIX d
            RELWITHDEBINFO_POSTFIX rd
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})  # a Windows DLL is a runtime output
        target_include_directories(rexgpu-native PRIVATE src ${REXSDK_DIR}/thirdparty/renderdoc)
        target_link_libraries(rexgpu-native PRIVATE rex::runtime ${BOTS_NATIVE_GFX_LIBS})
        if(WIN32)
            target_link_libraries(rexgpu-native PRIVATE powrprof)  # host_stats.cpp: CPU clocks, power plan
        endif()
        if(APPLE)
            # Export only the plugin entry points. Otherwise dyld coalesces the plugin's weak glslang template
            # instances with the game's copies (it links glslang too), and the plugin's parser runs the game's
            # code against its own pool allocator: a crash in TIntermediate::addSwizzle on the first shader.
            target_link_options(rexgpu-native PRIVATE
                LINKER:-exported_symbol,_rex_gpu_abi_version LINKER:-exported_symbol,_rex_gpu_create)
        endif()
        add_dependencies(bots rexgpu-native)

        # Hybrid spike, fill 4 (src/gfx/vk/hybrid_fill.cpp): an external scene renderer linked into the plugin.
        # BOTS_N1FFI_LIBRARY: its static library (libn1ffi.a); BOTS_N1FFI_INCLUDE_DIR: the directory with n1ffi.h.
        set(BOTS_N1FFI_LIBRARY "" CACHE FILEPATH "Hybrid spike: n1 renderer static library")
        set(BOTS_N1FFI_INCLUDE_DIR "" CACHE PATH "Hybrid spike: directory holding n1ffi.h")
        if(BOTS_N1FFI_LIBRARY AND BOTS_N1FFI_INCLUDE_DIR)
            target_compile_definitions(rexgpu-native PRIVATE BOTS_N1FFI=1)
            target_compile_definitions(bots PRIVATE BOTS_HAS_N1=1)  # src/renderer_choice.h: the launcher offers n1
            target_include_directories(rexgpu-native PRIVATE ${BOTS_N1FFI_INCLUDE_DIR})
            target_link_libraries(rexgpu-native PRIVATE ${BOTS_N1FFI_LIBRARY})
            if(APPLE)
                target_link_libraries(rexgpu-native PRIVATE "-framework CoreFoundation" "-framework Security" iconv)
            elseif(WIN32)
                # What rustc reports for the staticlib (--print native-static-libs). Its /defaultlib:msvcrt is the
                # dynamic CRT, the same as this build's default (/MD), so the two agree.
                target_link_libraries(rexgpu-native PRIVATE kernel32 ntdll userenv ws2_32 dbghelp)
            elseif(UNIX)
                target_link_libraries(rexgpu-native PRIVATE dl pthread m)
            endif()
            set_property(TARGET rexgpu-native APPEND PROPERTY LINK_DEPENDS ${BOTS_N1FFI_LIBRARY})
        endif()
    endif()
endif()

# bots_register.cpp is one function with ~143k SetFunction() calls (runs once at startup).
# Clang goes superlinear on it: >25 min at -O2 -g, and >16 min at -O0 too, where arm64 uses
# GlobalISel and its Localizer pass is quadratic here. -O0 with SelectionDAG avoids both.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/default/bots_register.cpp
    PROPERTIES COMPILE_OPTIONS "-O0;-g0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)  # PCH is built -O2; clang rejects the mismatch
