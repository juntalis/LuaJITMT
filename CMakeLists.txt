cmake_minimum_required(VERSION 3.20)
project(LuaJIT-MT C)

# ---------------------------------------------------------------------------
# First-draft two-stage bootstrap build for LuaJIT-MT.
#
# Stage 1 (host tools):
#   minilua               -> src/host/minilua.c
#   buildvm_arch.h         -> minilua + dynasm.lua + src/vm_x64.dasc
#   buildvm               -> src/host/buildvm*.c (depends on buildvm_arch.h)
#
# Stage 2 (generated headers/objects via buildvm, then the real library):
#   lj_vm.obj (peobj mode), lj_bcdef.h, lj_ffdef.h, lj_libdef.h, lj_recdef.h,
#   lj_folddef.h, jit/vmdef.lua, luajit.h (via genversion.lua)
#
# This file uses only CMake-native primitives (add_executable, add_library,
# add_custom_command, add_custom_target) to drive the two stages. It does
# NOT shell out to make/mingw32-make/nmake.
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(LJ_SRC_DIR   "${CMAKE_SOURCE_DIR}/src")
set(LJ_HOST_DIR  "${LJ_SRC_DIR}/host")
set(LJ_DASM_DIR  "${CMAKE_SOURCE_DIR}/dynasm")

# ---------------------------------------------------------------------------
# Stage 1a: minilua host executable.
# ---------------------------------------------------------------------------
add_executable(minilua "${LJ_HOST_DIR}/minilua.c")
if(MSVC)
  target_compile_definitions(minilua PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_link_libraries(minilua PRIVATE m)
endif()

# ---------------------------------------------------------------------------
# Stage 1b: buildvm_arch.h generated via minilua + dynasm.lua from vm_x64.dasc.
# DASM flags mirror src/Makefile's DASM_AFLAGS construction for a
# TARGET_SYS=Windows, x64, P64, JIT, FFI, DUALNUM(off by default on x64),
# FPU, HFABI, little-endian build (see src/Makefile lines ~394-480).
# ---------------------------------------------------------------------------
set(BUILDVM_ARCH_H "${CMAKE_BINARY_DIR}/host/buildvm_arch.h")

set(DASM_FLAGS
  -D ENDIAN_LE
  -D P64
  -D JIT
  -D FFI
  -D FPU
  -D HFABI
  -D VER=21
  -D WIN
)

add_custom_command(
  OUTPUT "${BUILDVM_ARCH_H}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/host"
  COMMAND $<TARGET_FILE:minilua> "${LJ_DASM_DIR}/dynasm.lua" ${DASM_FLAGS}
          -o "${BUILDVM_ARCH_H}" "${LJ_SRC_DIR}/vm_x64.dasc"
  WORKING_DIRECTORY "${LJ_SRC_DIR}"
  DEPENDS minilua "${LJ_SRC_DIR}/vm_x64.dasc" "${LJ_DASM_DIR}/dynasm.lua"
          "${LJ_SRC_DIR}/lj_arch.h" "${LJ_SRC_DIR}/lua.h" "${LJ_SRC_DIR}/luaconf.h"
  COMMENT "DYNASM generating buildvm_arch.h"
  VERBATIM
)

add_custom_target(generate_buildvm_arch_h DEPENDS "${BUILDVM_ARCH_H}")

# ---------------------------------------------------------------------------
# Stage 1c: buildvm host executable.
# ---------------------------------------------------------------------------
add_executable(buildvm
  "${LJ_HOST_DIR}/buildvm.c"
  "${LJ_HOST_DIR}/buildvm_asm.c"
  "${LJ_HOST_DIR}/buildvm_peobj.c"
  "${LJ_HOST_DIR}/buildvm_lib.c"
  "${LJ_HOST_DIR}/buildvm_fold.c"
)
add_dependencies(buildvm generate_buildvm_arch_h)
target_include_directories(buildvm PRIVATE "${LJ_SRC_DIR}" "${CMAKE_BINARY_DIR}/host")
if(MSVC)
  target_compile_definitions(buildvm PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Stage 2a: luajit.h generation via host/genversion.lua (run by minilua).
# ---------------------------------------------------------------------------
set(LUAJIT_H "${LJ_SRC_DIR}/luajit.h")
set(LUAJIT_RELVER_TXT "${LJ_SRC_DIR}/luajit_relver.txt")

add_custom_command(
  OUTPUT "${LUAJIT_H}"
  COMMAND $<TARGET_FILE:minilua> "${LJ_HOST_DIR}/genversion.lua"
          "${LJ_SRC_DIR}/luajit_rolling.h" "${LUAJIT_RELVER_TXT}" "${LUAJIT_H}"
  WORKING_DIRECTORY "${LJ_SRC_DIR}"
  DEPENDS minilua "${LJ_SRC_DIR}/luajit_rolling.h" "${LJ_HOST_DIR}/genversion.lua"
  COMMENT "Generating luajit.h (version stamping)"
  VERBATIM
)

add_custom_target(generate_luajit_h DEPENDS "${LUAJIT_H}")

# Determine LUAJIT_VERSION_STRING for the .pc file. Best-effort: parse
# LUAJIT_VERSION from luajit_rolling.h; fall back to a placeholder if not
# found. This is only used for pkg-config metadata, not compiled code.
set(LUAJIT_VERSION_STRING "2.1.0-moonjitmt")
if(EXISTS "${LJ_SRC_DIR}/luajit_rolling.h")
  file(STRINGS "${LJ_SRC_DIR}/luajit_rolling.h" _lj_ver_line REGEX "#define LUAJIT_VERSION \"")
  if(_lj_ver_line)
    string(REGEX REPLACE ".*#define LUAJIT_VERSION \"([^\"]*)\".*" "\\1" LUAJIT_VERSION_STRING "${_lj_ver_line}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Stage 2b: LJLIB_C source list (used both for buildvm -m invocations and
# as part of the static library sources).
# ---------------------------------------------------------------------------
set(LJLIB_C
  "${LJ_SRC_DIR}/lib_base.c"
  "${LJ_SRC_DIR}/lib_math.c"
  "${LJ_SRC_DIR}/lib_bit.c"
  "${LJ_SRC_DIR}/lib_string.c"
  "${LJ_SRC_DIR}/lib_table.c"
  "${LJ_SRC_DIR}/lib_utf8.c"
  "${LJ_SRC_DIR}/lib_io.c"
  "${LJ_SRC_DIR}/lib_os.c"
  "${LJ_SRC_DIR}/lib_package.c"
  "${LJ_SRC_DIR}/lib_debug.c"
  "${LJ_SRC_DIR}/lib_jit.c"
  "${LJ_SRC_DIR}/lib_ffi.c"
  "${LJ_SRC_DIR}/lib_buffer.c"
  "${LJ_SRC_DIR}/lib_threading.c"
)

# ---------------------------------------------------------------------------
# Stage 2c: buildvm-generated headers and the VM object (peobj mode, for
# MSVC/Windows COFF target -- matches src/Makefile's TARGET_SYS=Windows path
# where LJVM_MODE=peobj and LJVM_BOUT=LJVM_O, i.e. buildvm emits the object
# file directly rather than an .S file to be assembled separately).
# ---------------------------------------------------------------------------
set(LJ_VM_OBJ    "${CMAKE_BINARY_DIR}/lj_vm.obj")
set(LJ_BCDEF_H   "${LJ_SRC_DIR}/lj_bcdef.h")
set(LJ_FFDEF_H   "${LJ_SRC_DIR}/lj_ffdef.h")
set(LJ_LIBDEF_H  "${LJ_SRC_DIR}/lj_libdef.h")
set(LJ_RECDEF_H  "${LJ_SRC_DIR}/lj_recdef.h")
set(LJ_FOLDDEF_H "${LJ_SRC_DIR}/lj_folddef.h")
set(LJ_VMDEF_LUA "${LJ_SRC_DIR}/jit/vmdef.lua")

add_custom_command(
  OUTPUT "${LJ_VM_OBJ}"
  COMMAND $<TARGET_FILE:buildvm> -m peobj -o "${LJ_VM_OBJ}"
  DEPENDS buildvm
  COMMENT "buildvm -m peobj (lj_vm.obj)"
  VERBATIM
)

add_custom_command(
  OUTPUT "${LJ_BCDEF_H}"
  COMMAND $<TARGET_FILE:buildvm> -m bcdef -o "${LJ_BCDEF_H}" ${LJLIB_C}
  DEPENDS buildvm ${LJLIB_C}
  COMMENT "buildvm -m bcdef"
  VERBATIM
)

add_custom_command(
  OUTPUT "${LJ_FFDEF_H}"
  COMMAND $<TARGET_FILE:buildvm> -m ffdef -o "${LJ_FFDEF_H}" ${LJLIB_C}
  DEPENDS buildvm ${LJLIB_C}
  COMMENT "buildvm -m ffdef"
  VERBATIM
)

add_custom_command(
  OUTPUT "${LJ_LIBDEF_H}"
  COMMAND $<TARGET_FILE:buildvm> -m libdef -o "${LJ_LIBDEF_H}" ${LJLIB_C}
  DEPENDS buildvm ${LJLIB_C}
  COMMENT "buildvm -m libdef"
  VERBATIM
)

add_custom_command(
  OUTPUT "${LJ_RECDEF_H}"
  COMMAND $<TARGET_FILE:buildvm> -m recdef -o "${LJ_RECDEF_H}" ${LJLIB_C}
  DEPENDS buildvm ${LJLIB_C}
  COMMENT "buildvm -m recdef"
  VERBATIM
)

add_custom_command(
  OUTPUT "${LJ_FOLDDEF_H}"
  COMMAND $<TARGET_FILE:buildvm> -m folddef -o "${LJ_FOLDDEF_H}" "${LJ_SRC_DIR}/lj_opt_fold.c"
  DEPENDS buildvm "${LJ_SRC_DIR}/lj_opt_fold.c"
  COMMENT "buildvm -m folddef"
  VERBATIM
)

add_custom_command(
  OUTPUT "${LJ_VMDEF_LUA}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${LJ_SRC_DIR}/jit"
  COMMAND $<TARGET_FILE:buildvm> -m vmdef -o "${LJ_VMDEF_LUA}" ${LJLIB_C}
  DEPENDS buildvm ${LJLIB_C}
  COMMENT "buildvm -m vmdef"
  VERBATIM
)

set_source_files_properties("${LJ_VM_OBJ}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_custom_target(generate_lj_headers DEPENDS
  "${LJ_VM_OBJ}"
  "${LJ_BCDEF_H}"
  "${LJ_FFDEF_H}"
  "${LJ_LIBDEF_H}"
  "${LJ_RECDEF_H}"
  "${LJ_FOLDDEF_H}"
  "${LJ_VMDEF_LUA}"
  "${LUAJIT_H}"
)

# ---------------------------------------------------------------------------
# Stage 2d: static library "luajit-5.1" (LJCORE_O + LJLIB_O + lj_vm.obj).
# Source lists confirmed against src/lj_*.c and src/lib_*.c on disk.
# ---------------------------------------------------------------------------
set(LJCORE_C
  "${LJ_SRC_DIR}/lj_assert.c"
  "${LJ_SRC_DIR}/lj_gc.c"
  "${LJ_SRC_DIR}/lj_gc2.c"
  "${LJ_SRC_DIR}/lj_safepoint.c"
  "${LJ_SRC_DIR}/lj_err.c"
  "${LJ_SRC_DIR}/lj_char.c"
  "${LJ_SRC_DIR}/lj_bc.c"
  "${LJ_SRC_DIR}/lj_obj.c"
  "${LJ_SRC_DIR}/lj_buf.c"
  "${LJ_SRC_DIR}/lj_str.c"
  "${LJ_SRC_DIR}/lj_tab.c"
  "${LJ_SRC_DIR}/lj_func.c"
  "${LJ_SRC_DIR}/lj_udata.c"
  "${LJ_SRC_DIR}/lj_meta.c"
  "${LJ_SRC_DIR}/lj_debug.c"
  "${LJ_SRC_DIR}/lj_prng.c"
  "${LJ_SRC_DIR}/lj_state.c"
  "${LJ_SRC_DIR}/lj_dispatch.c"
  "${LJ_SRC_DIR}/lj_tg.c"
  "${LJ_SRC_DIR}/lj_thr.c"
  "${LJ_SRC_DIR}/lj_chan.c"
  "${LJ_SRC_DIR}/lj_arena.c"
  "${LJ_SRC_DIR}/lj_vmevent.c"
  "${LJ_SRC_DIR}/lj_vmmath.c"
  "${LJ_SRC_DIR}/lj_strscan.c"
  "${LJ_SRC_DIR}/lj_strfmt.c"
  "${LJ_SRC_DIR}/lj_strfmt_num.c"
  "${LJ_SRC_DIR}/lj_serialize.c"
  "${LJ_SRC_DIR}/lj_api.c"
  "${LJ_SRC_DIR}/lj_profile.c"
  "${LJ_SRC_DIR}/lj_lex.c"
  "${LJ_SRC_DIR}/lj_parse.c"
  "${LJ_SRC_DIR}/lj_bcread.c"
  "${LJ_SRC_DIR}/lj_bcwrite.c"
  "${LJ_SRC_DIR}/lj_load.c"
  "${LJ_SRC_DIR}/lj_ir.c"
  "${LJ_SRC_DIR}/lj_opt_mem.c"
  "${LJ_SRC_DIR}/lj_opt_fold.c"
  "${LJ_SRC_DIR}/lj_opt_narrow.c"
  "${LJ_SRC_DIR}/lj_opt_dce.c"
  "${LJ_SRC_DIR}/lj_opt_loop.c"
  "${LJ_SRC_DIR}/lj_opt_split.c"
  "${LJ_SRC_DIR}/lj_opt_sink.c"
  "${LJ_SRC_DIR}/lj_mcode.c"
  "${LJ_SRC_DIR}/lj_snap.c"
  "${LJ_SRC_DIR}/lj_record.c"
  "${LJ_SRC_DIR}/lj_crecord.c"
  "${LJ_SRC_DIR}/lj_ffrecord.c"
  "${LJ_SRC_DIR}/lj_asm.c"
  "${LJ_SRC_DIR}/lj_trace.c"
  "${LJ_SRC_DIR}/lj_gdbjit.c"
  "${LJ_SRC_DIR}/lj_ctype.c"
  "${LJ_SRC_DIR}/lj_cdata.c"
  "${LJ_SRC_DIR}/lj_cconv.c"
  "${LJ_SRC_DIR}/lj_ccall.c"
  "${LJ_SRC_DIR}/lj_ccallback.c"
  "${LJ_SRC_DIR}/lj_carith.c"
  "${LJ_SRC_DIR}/lj_clib.c"
  "${LJ_SRC_DIR}/lj_cparse.c"
  "${LJ_SRC_DIR}/lj_lib.c"
  "${LJ_SRC_DIR}/lj_alloc.c"
  "${LJ_SRC_DIR}/lib_aux.c"
  "${LJ_SRC_DIR}/lib_init.c"
)

add_library(luajit-5.1 STATIC ${LJCORE_C} ${LJLIB_C} "${LJ_VM_OBJ}")
set_target_properties(luajit-5.1 PROPERTIES OUTPUT_NAME "luajit-5.1")
target_include_directories(luajit-5.1 PUBLIC "${LJ_SRC_DIR}")
add_dependencies(luajit-5.1 generate_lj_headers)

if(MSVC)
  target_compile_definitions(luajit-5.1 PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(luajit-5.1 PRIVATE /W3)
else()
  target_compile_options(luajit-5.1 PRIVATE -Wall)
endif()

# ---------------------------------------------------------------------------
# Stage 2d-bis: shared library "luajit-5.1" (luajit-5.1.dll on Windows).
# Mirrors src/Makefile's Windows dynamic BUILDMODE (-DLUA_BUILD_AS_DLL on all
# translation units so LUA_API resolves to dllexport for LUA_CORE/LUA_LIB
# sources). Off by default since mixing static and shared luajit-5.1 outputs
# in one build directory requires distinct archive/runtime names.
# ---------------------------------------------------------------------------
option(LUAJIT_BUILD_SHARED "Also build luajit-5.1.dll (shared library)" OFF)

if(LUAJIT_BUILD_SHARED)
  add_library(luajit-5.1-shared SHARED ${LJCORE_C} ${LJLIB_C} "${LJ_VM_OBJ}")
  target_include_directories(luajit-5.1-shared PUBLIC "${LJ_SRC_DIR}")
  target_compile_definitions(luajit-5.1-shared PRIVATE LUA_BUILD_AS_DLL)
  add_dependencies(luajit-5.1-shared generate_lj_headers)
  set_target_properties(luajit-5.1-shared PROPERTIES
    RUNTIME_OUTPUT_NAME "luajit-5.1"
    LIBRARY_OUTPUT_NAME "luajit-5.1"
    ARCHIVE_OUTPUT_NAME "luajit-5.1-dll"
  )
  if(MSVC)
    target_compile_definitions(luajit-5.1-shared PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(luajit-5.1-shared PRIVATE /W3)
  else()
    target_compile_options(luajit-5.1-shared PRIVATE -Wall)
  endif()
endif()

# ---------------------------------------------------------------------------
# Stage 2e: luajit.exe CLI executable.
# ---------------------------------------------------------------------------
add_executable(luajit "${LJ_SRC_DIR}/luajit.c")
target_include_directories(luajit PRIVATE "${LJ_SRC_DIR}")
target_link_libraries(luajit PRIVATE luajit-5.1)
add_dependencies(luajit generate_lj_headers)
if(MSVC)
  target_compile_definitions(luajit PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_link_libraries(luajit PRIVATE m dl)
endif()
if(WIN32)
  target_link_libraries(luajit PRIVATE Synchronization.lib)
endif()

# ---------------------------------------------------------------------------
# pkg-config file generation from cmake/luajit.pc.in (read-only reference).
# ---------------------------------------------------------------------------
set(LUAJIT_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(LUAJIT_PC_LIBDIR "${CMAKE_INSTALL_PREFIX}/lib")
set(LUAJIT_PC_INCLUDEDIR "${CMAKE_INSTALL_PREFIX}/include/luajit-2.1")

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/luajit.pc.in"
  "${CMAKE_BINARY_DIR}/luajit.pc"
  @ONLY
)
