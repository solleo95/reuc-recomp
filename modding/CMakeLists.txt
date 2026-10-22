include(FetchContent)
set(miniz_archive "https://github.com/richgel999/miniz/archive/refs/tags/3.0.2.zip")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/deps/miniz-3.0.2.zip")
  set(miniz_archive "${CMAKE_SOURCE_DIR}/build/deps/miniz-3.0.2.zip")
endif()
# SOURCE_SUBDIR points at a folder without CMakeLists.txt: only download and unpack, the target below builds the four sources.
FetchContent_Declare(miniz URL "${miniz_archive}"
  URL_HASH SHA256=1b4fac7144a64d8d26c4642c25bd5b237599071eaf7c12fc6eaff294af31e931
  SOURCE_SUBDIR no_cmake
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(miniz)
add_library(uc_miniz STATIC ${miniz_SOURCE_DIR}/miniz.c ${miniz_SOURCE_DIR}/miniz_tdef.c ${miniz_SOURCE_DIR}/miniz_tinfl.c ${miniz_SOURCE_DIR}/miniz_zip.c)
target_include_directories(uc_miniz PUBLIC ${miniz_SOURCE_DIR} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_include_directories(uc_miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
install(FILES ${miniz_SOURCE_DIR}/LICENSE DESTINATION licenses/miniz)
install(FILES ${miniz_SOURCE_DIR}/LICENSE DESTINATION licenses/miniz)

set(lua_archive "https://www.lua.org/ftp/lua-5.4.7.tar.gz")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/deps/lua-5.4.7.tar.gz")
  set(lua_archive "${CMAKE_SOURCE_DIR}/build/deps/lua-5.4.7.tar.gz")
endif()
FetchContent_Declare(lua URL "${lua_archive}"
  URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
  SOURCE_SUBDIR no_cmake
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(lua)
# A single string.find/gsub can run for minutes without executing a Lua instruction, so the pattern matcher polls the script time budget (uc_lua_poll, runtime_lua.cpp).
file(READ ${lua_SOURCE_DIR}/src/lstrlib.c uc_lstrlib)
if(NOT uc_lstrlib MATCHES "uc_lua_poll")
  set(uc_match_head "static const char *match (MatchState *ms, const char *s, const char *p) {")
  string(REPLACE "${uc_match_head}" "void uc_lua_poll(lua_State *L);\n${uc_match_head}\n  uc_lua_poll(ms->L);" uc_lstrlib_patched "${uc_lstrlib}")
  if(uc_lstrlib_patched STREQUAL uc_lstrlib)
    message(FATAL_ERROR "lstrlib.c: the pattern matcher entry point was not found, the Lua time-budget patch cannot be applied")
  endif()
  file(WRITE ${lua_SOURCE_DIR}/src/lstrlib.c "${uc_lstrlib_patched}")
endif()
# Lua is compiled as C++ so that script errors are C++ exceptions: our frames are unwound properly, no longjmp across destructors.
file(GLOB uc_lua_sources ${lua_SOURCE_DIR}/src/*.c)
list(FILTER uc_lua_sources EXCLUDE REGEX "/(lua|luac)\\.c$")
set_source_files_properties(${uc_lua_sources} PROPERTIES LANGUAGE CXX)
add_library(uc_lua STATIC ${uc_lua_sources})
target_include_directories(uc_lua PUBLIC ${lua_SOURCE_DIR}/src)
target_compile_options(uc_lua PRIVATE -w)
install(FILES ${lua_SOURCE_DIR}/doc/readme.html DESTINATION licenses/lua RENAME license-and-readme.html)

set(quickjs_archive "https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.10.1.tar.gz")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/deps/quickjs-ng-0.10.1.tar.gz")
  set(quickjs_archive "${CMAKE_SOURCE_DIR}/build/deps/quickjs-ng-0.10.1.tar.gz")
endif()
FetchContent_Declare(quickjs URL "${quickjs_archive}"
  URL_HASH SHA256=4e917eea3bb6051a0551fdd3aac45199ff705fa08920c15b824a08c5fbb032e4
  SOURCE_SUBDIR no_cmake
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(quickjs)
# Array builtins loop in C over `length` without reaching the interrupt handler: Array.prototype.indexOf.call({length: 2 ** 53}, 0) would freeze the game
# thread. An array-like longer than 16M elements (a real array of that size does not fit the mods' memory cap) is refused with a RangeError instead.
file(READ ${quickjs_SOURCE_DIR}/quickjs.c uc_quickjs_source)
if(NOT uc_quickjs_source MATCHES "uc_array_like_limit")
  set(uc_length_tail "    return JS_ToLengthFree(ctx, pres, len_val);\n}\n\nstatic __exception int js_set_length64")
  string(REPLACE "${uc_length_tail}" "    if (JS_ToLengthFree(ctx, pres, len_val) < 0) return -1;\n    if (*pres > 16777216) { /* uc_array_like_limit */ *pres = 0; JS_ThrowRangeError(ctx, \"array-like is too long (more than 16777216 elements)\"); return -1; }\n    return 0;\n}\n\nstatic __exception int js_set_length64" uc_quickjs_patched "${uc_quickjs_source}")
  if(uc_quickjs_patched STREQUAL uc_quickjs_source)
    message(FATAL_ERROR "quickjs.c: js_get_length64 was not found, the array-like length patch cannot be applied")
  endif()
  file(WRITE ${quickjs_SOURCE_DIR}/quickjs.c "${uc_quickjs_patched}")
endif()
# The engine only (no quickjs-libc: nothing here gives scripts std/os/file/network access).
add_library(uc_quickjs STATIC ${quickjs_SOURCE_DIR}/cutils.c ${quickjs_SOURCE_DIR}/libregexp.c ${quickjs_SOURCE_DIR}/libunicode.c ${quickjs_SOURCE_DIR}/quickjs.c ${quickjs_SOURCE_DIR}/xsum.c)
target_include_directories(uc_quickjs PUBLIC ${quickjs_SOURCE_DIR})
target_compile_definitions(uc_quickjs PRIVATE _GNU_SOURCE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
target_compile_options(uc_quickjs PRIVATE -w)
install(FILES ${quickjs_SOURCE_DIR}/LICENSE DESTINATION licenses/quickjs-ng)

add_library(uc_mods STATIC manifest.cpp mod_paths.cpp mod_zip.cpp registry.cpp overlay.cpp settings.cpp api_registry.cpp game_api.cpp game_api_player.cpp runtime_lua.cpp runtime_js.cpp runtime_native.cpp host.cpp hook_registry.cpp data_patch.cpp profile.cpp)
target_include_directories(uc_mods PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_mods PUBLIC nlohmann_json::nlohmann_json uc_miniz uc_lua uc_quickjs uc_runtime bcrypt)

# The ImGui side (Mods tab, mod tabs, widgets): kept apart so the core library and its tests do not need ImGui.
add_library(uc_mod_menu STATIC menu_ui.cpp)
target_link_libraries(uc_mod_menu PUBLIC uc_mods uc_imgui)
