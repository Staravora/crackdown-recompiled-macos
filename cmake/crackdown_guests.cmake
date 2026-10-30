# The recompiled guests linked into crackdown: the base game (generated/default) and, once tools/codegen-tu6.ps1 has
# produced generated/tu6, Title Update 6. Both come from a project named "crackdown", so their function symbols and image
# tables have the same names; each guest is compiled with a generated crackdown_guest_symbols.h that renames them with
# its own prefix (src/guest_symbol_macros.h), and the host picks one guest descriptor per process (src/guest_image.h).
#
# The guest addresses of each build are named only by its src/guest_bindings_<id>.cpp and its layout in guest_layout.h.

# Writes <out_dir>/crackdown_guest_symbols.h: a macro per guest function (except the kernel imports, which the runtime
# provides and both guests share) and per image table, adding `prefix`.
function(crackdown_write_guest_symbols id prefix generated_dir out_dir)
    set(_funcs "${generated_dir}/crackdown_funcs.h")
    file(READ "${generated_dir}/crackdown_pch.h" _pch)
    foreach(_fragment "REX_EXTERN(name);" "REX_EXTERN(__imp__##name)" "alias(\"__imp__\" #name)")
        string(FIND "${_pch}" "${_fragment}" _found)
        if(_found EQUAL -1)
            message(FATAL_ERROR "Guest ${id}: ${generated_dir}/crackdown_pch.h no longer defines DECLARE_REX_FUNC and "
                "DEFINE_REX_FUNC as src/guest_symbol_macros.h assumes (missing: ${_fragment})")
        endif()
    endforeach()

    file(READ "${_funcs}" _funcs_text)
    string(REGEX MATCHALL "DECLARE_REX_FUNC\\([A-Za-z0-9_]+\\)" _decls "${_funcs_text}")
    string(REGEX REPLACE "DECLARE_REX_FUNC\\(([A-Za-z0-9_]+)\\)" "\\1" _decls "${_decls}")
    list(FILTER _decls EXCLUDE REGEX "^__imp__")
    list(LENGTH _decls _count)
    if(_count LESS 1000)
        message(FATAL_ERROR "Guest ${id}: ${_funcs} declares only ${_count} guest functions")
    endif()
    list(APPEND _decls PPCImageConfig PPCFuncMappings crackdown_RegisterFunctions)
    list(JOIN _decls "\n" _names)
    string(REGEX REPLACE "([A-Za-z0-9_]+)(\n|$)" "#define \\1 ${prefix}\\1\n" _body "${_names}")

    set(_text "#pragma once\n// Written by cmake/crackdown_guests.cmake for the ${id} guest. Do not edit.\n")
    string(APPEND _text "#include \"guest_symbol_macros.h\"\n${_body}")
    set(_header "${out_dir}/crackdown_guest_symbols.h")
    set(_current "")
    if(EXISTS "${_header}")
        file(READ "${_header}" _current)
    endif()
    if(NOT _current STREQUAL _text)
        file(MAKE_DIRECTORY "${out_dir}")
        file(WRITE "${_header}" "${_text}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_funcs}" "${generated_dir}/crackdown_pch.h")
endfunction()

# Adds crackdown_guest_<id>_recomp (the generated code) and crackdown_guest_<id> (the host's bindings and descriptor
# for it). Both are object libraries the executable links directly, so the bindings' strong definitions override the
# generated weak function aliases; appends them to CRACKDOWN_GUEST_TARGETS in the caller.
function(crackdown_add_guest id prefix generated_dir bindings_source)
    include("${generated_dir}/sources.cmake")
    set(_generated ${GENERATED_SOURCES})
    set_source_files_properties(${_generated} PROPERTIES COMPILE_OPTIONS "${REXGLUE_RECOMP_OPTIONS}")
    if(APPLE)
        # This startup-only function is tens of thousands of virtual calls.
        # AppleClang spends minutes optimizing it without improving gameplay.
        set_source_files_properties("${generated_dir}/crackdown_register.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
        set_property(SOURCE "${generated_dir}/crackdown_register.cpp" APPEND PROPERTY COMPILE_OPTIONS
            -O0 -include "${CMAKE_CURRENT_SOURCE_DIR}/src/crackdown_guest_pch.h")
    endif()

    set(_symbols_dir "${CMAKE_BINARY_DIR}/guest-symbols/${id}")
    crackdown_write_guest_symbols(${id} ${prefix} "${generated_dir}" "${_symbols_dir}")

    set(_recomp crackdown_guest_${id}_recomp)
    add_library(${_recomp} OBJECT ${_generated})
    target_include_directories(${_recomp} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        "${_symbols_dir}"
        "${generated_dir}"
    )
    target_link_libraries(${_recomp} PRIVATE rex::runtime)
    rexglue_apply_target_settings(${_recomp})
    rexglue_apply_recomp_settings(${_recomp} "${CMAKE_CURRENT_SOURCE_DIR}/src/crackdown_guest_pch.h")

    set(_host crackdown_guest_${id})
    add_library(${_host} OBJECT "${bindings_source}")
    target_include_directories(${_host} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src "${_symbols_dir}")
    target_link_libraries(${_host} PRIVATE rex::runtime)
    rexglue_apply_target_settings(${_host})
    set_property(SOURCE "${bindings_source}" APPEND PROPERTY COMPILE_OPTIONS
        "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:${CRACKDOWN_WARNINGS}>")

    crackdown_enable_thinlto(${_recomp})
    crackdown_enable_thinlto(${_host})
    set(CRACKDOWN_GUEST_TARGETS ${CRACKDOWN_GUEST_TARGETS} ${_host} ${_recomp} PARENT_SCOPE)
endfunction()

set(CRACKDOWN_GUEST_TARGETS)
crackdown_add_guest(base guest_base_ "${CMAKE_CURRENT_SOURCE_DIR}/generated/default" src/guest_bindings_base.cpp)

set(CRACKDOWN_TU6_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/tu6")
if(EXISTS "${CRACKDOWN_TU6_DIR}/sources.cmake")
    crackdown_add_guest(tu6 guest_tu6_ "${CRACKDOWN_TU6_DIR}" src/guest_bindings_tu6.cpp)
    set(CRACKDOWN_TU6_GUEST_LINKED TRUE)
else()
    message(STATUS "Title Update 6: no guest code in generated/tu6, crackdown carries the base game only")
    set(CRACKDOWN_TU6_GUEST_LINKED FALSE)
endif()

target_link_libraries(crackdown PRIVATE ${CRACKDOWN_GUEST_TARGETS})
if(CRACKDOWN_TU6_GUEST_LINKED)
    target_compile_definitions(crackdown PRIVATE CRACKDOWN_TU6_GUEST_LINKED=1)
endif()
