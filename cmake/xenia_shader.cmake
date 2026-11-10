# Xenos microcode -> SPIR-V translator, ported from Xenia (BSD-3, see thirdparty/xenia/LICENSE).
# Only the shader translator is built; the parts of Xenia it would normally pull in
# (logging, cvars, Vulkan device, memory system) are replaced by small shims in
# runtime/gpu/xenia_shim, which is searched before Xenia's own headers.
set(XE_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/xenia")
set(XE_GPU "${XE_ROOT}/src/xenia/gpu")

# Xenia includes its dependencies as "third_party/<name>/...". Those live elsewhere in this
# repo, so thirdparty/xenia_third_party/third_party/ holds links to them (not in git; created
# here if missing - existing Windows junctions are kept).
set(XE_THIRD_PARTY "${CMAKE_SOURCE_DIR}/thirdparty/xenia_third_party/third_party")
file(MAKE_DIRECTORY "${XE_THIRD_PARTY}")
foreach(link IN ITEMS
        "fmt|${CMAKE_SOURCE_DIR}/thirdparty/fmt"
        "glslang|${CMAKE_SOURCE_DIR}/thirdparty/glslang"
        "xxhash|${CMAKE_SOURCE_DIR}/tools/XenonRecomp/thirdparty/xxHash")
    string(REPLACE "|" ";" parts "${link}")
    list(GET parts 0 name)
    list(GET parts 1 target)
    if(NOT EXISTS "${XE_THIRD_PARTY}/${name}")
        file(CREATE_LINK "${target}" "${XE_THIRD_PARTY}/${name}" SYMBOLIC)
    endif()
endforeach()

add_library(XeniaShader STATIC
    ${XE_GPU}/ucode.cc
    ${XE_GPU}/shader.cc
    ${XE_GPU}/shader_translator.cc
    ${XE_GPU}/shader_translator_disasm.cc
    ${XE_ROOT}/src/xenia/base/string_buffer.cc
    ${XE_GPU}/spirv_builder.cc
    ${XE_GPU}/spirv_shader.cc
    ${XE_GPU}/spirv_shader_translator.cc
    ${XE_GPU}/spirv_shader_translator_alu.cc
    ${XE_GPU}/spirv_shader_translator_fetch.cc
    ${XE_GPU}/spirv_shader_translator_memexport.cc
    ${XE_GPU}/spirv_shader_translator_rb.cc
    ${XE_GPU}/xenos.cc
    ${XE_GPU}/registers.cc
    ${XE_GPU}/register_file.cc
    ${XE_GPU}/draw_util.cc
    ${XE_GPU}/draw_extent_estimator.cc
    ${XE_GPU}/shader_interpreter.cc
    ${XE_GPU}/texture_info.cc
    ${XE_GPU}/texture_info_formats.cc
    ${XE_GPU}/texture_util.cc
    ${XE_ROOT}/src/xenia/ui/graphics_util.cc
    ${CMAKE_SOURCE_DIR}/runtime/gpu/xenia_shim/xenia_shim.cc
    # glslang's SPIR-V builder (used by Xenia's SpirvBuilder)
    ${CMAKE_SOURCE_DIR}/thirdparty/glslang/SPIRV/SpvBuilder.cpp
    ${CMAKE_SOURCE_DIR}/thirdparty/glslang/SPIRV/InReadableOrder.cpp
    ${CMAKE_SOURCE_DIR}/thirdparty/glslang/SPIRV/Logger.cpp
    ${CMAKE_SOURCE_DIR}/thirdparty/glslang/SPIRV/doc.cpp
    ${CMAKE_SOURCE_DIR}/thirdparty/glslang/SPIRV/disassemble.cpp
    ${CMAKE_SOURCE_DIR}/thirdparty/fmt/src/format.cc
)
target_include_directories(XeniaShader PUBLIC
    "${CMAKE_SOURCE_DIR}/runtime/gpu/xenia_shim"   # shims first
    "${XE_ROOT}/src"
    "${CMAKE_SOURCE_DIR}/thirdparty/xenia_third_party"
    "${CMAKE_SOURCE_DIR}/thirdparty/fmt/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/Vulkan-Headers/include"
)
# Xenia's platform.h detects the platform itself (XE_PLATFORM_MAC on Apple, incl. iOS).
if(WIN32)
    target_compile_definitions(XeniaShader PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN XE_PLATFORM_WIN32=1 _CRT_SECURE_NO_WARNINGS)
endif()
# Third-party code: keep the build log readable.
target_compile_options(XeniaShader PRIVATE -w)
