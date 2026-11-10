add_library(uc_binding_editor STATIC binding_editor.cpp)
if(WIN32)   # the self-updater is Windows-only; the Linux build (Steam Deck) uses the shared UI pieces below
add_library(uc_launcher_update STATIC update.cpp update_common.cpp)
target_include_directories(uc_launcher_update PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_launcher_update PUBLIC uc_update_download uc_update_process PRIVATE ole32)
target_compile_definitions(uc_launcher_update PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(NOT ANDROID)   # Linux (Steam Deck): update_posix.cpp, the same controller without a helper process (Android updates by installing the app)
add_library(uc_launcher_update STATIC update_common.cpp update_posix.cpp)
target_include_directories(uc_launcher_update PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_launcher_update PUBLIC uc_update_download)
endif()
add_library(uc_launcher_locale STATIC localization.cpp)
target_include_directories(uc_launcher_locale PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_launcher_locale PUBLIC uc_input_config)
target_include_directories(uc_binding_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_binding_editor PUBLIC uc_input)
add_library(uc_settings_registry STATIC settings_registry.cpp)   # the one table of settings: where each shows (desktop/Deck menu, desktop/Deck launcher) and how the simple ones are read and changed
target_include_directories(uc_settings_registry PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/renderer)
target_link_libraries(uc_settings_registry PUBLIC uc_live_settings)
add_library(uc_gamepad_panel STATIC gamepad_panel.cpp wiimote_panel.cpp)   # the Controllers tab with its Gamepad and Wii Remote windows: shared by the launcher and the in-game menu
target_include_directories(uc_gamepad_panel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_gamepad_panel PUBLIC uc_binding_editor uc_gamepad_config uc_wiimote_config uc_input_prompts uc_prompt_icons uc_imgui uc_ui_style)
if(WIN32)
# Wit (Wiimms ISO Tools, GPL-2.0+) unpacks the disc image selected in the launcher. It runs as a separate program; only the files it needs are installed.
set(wit_archive "https://wit.wiimm.de/download/wit-v3.05a-r8638-cygwin64.zip")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/deps/wit-v3.05a-r8638-cygwin64.zip")
  set(wit_archive "${CMAKE_SOURCE_DIR}/build/deps/wit-v3.05a-r8638-cygwin64.zip")
endif()
include(FetchContent)
FetchContent_Declare(wit URL "${wit_archive}"
  URL_HASH SHA256=049670558970f0cea2796d68e0ba1e48491474b5708bf12a95ab8a185f4e59c1 DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(wit)
install(FILES ${wit_SOURCE_DIR}/bin/wit.exe ${wit_SOURCE_DIR}/bin/cygwin1.dll ${wit_SOURCE_DIR}/bin/cygz.dll
  ${wit_SOURCE_DIR}/bin/cygcrypto-1.1.dll ${wit_SOURCE_DIR}/bin/cygncursesw-10.dll DESTINATION tools/wit)
install(FILES ${wit_SOURCE_DIR}/gpl-2.0.txt ${wit_SOURCE_DIR}/doc/wit-source.txt ${CMAKE_SOURCE_DIR}/third_party/wit/NOTICE.txt DESTINATION licenses/wit)
add_library(uc_image_unpack STATIC image_unpack.cpp image_unpack_common.cpp)
target_include_directories(uc_image_unpack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(uc_image_unpack PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(uc_image_unpack PRIVATE uc_windows_args)
add_library(uc_launch STATIC launch.cpp)
add_library(uc_cpu_calibration STATIC cpu_calibration.cpp)
target_include_directories(uc_cpu_calibration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_cpu_calibration PUBLIC uc_cpu_topology PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(uc_launch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_launch PUBLIC uc_options_config uc_diagnostics uc_windows_args uc_client_update_lock)
endif()
if(WIN32)   # netplay (NAT traversal) is Windows-only for now
include(FetchContent)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
set(miniupnpc_archive "https://codeload.github.com/miniupnp/miniupnp/zip/refs/tags/miniupnpc_2_3_3")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/deps/miniupnpc-2.3.3.zip")
  set(miniupnpc_archive "${CMAKE_SOURCE_DIR}/build/deps/miniupnpc-2.3.3.zip")
endif()
FetchContent_Declare(miniupnpc URL "${miniupnpc_archive}"
  URL_HASH SHA256=f6e7fad0c42c5a0434ef721228e5eee6e62bed6adbb1f785ba16082787c80e5f
  SOURCE_SUBDIR miniupnpc DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(miniupnpc)
set(enet_archive "https://codeload.github.com/lsalzman/enet/zip/refs/tags/v1.3.18")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/deps/enet-1.3.18.zip")
  set(enet_archive "${CMAKE_SOURCE_DIR}/build/deps/enet-1.3.18.zip")
endif()
FetchContent_Declare(enet URL "${enet_archive}"
  URL_HASH SHA256=32d6b15611c2667b88cd865e0cb48c3c011ddefcff6dfbdb266b8e0b04fffab3
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(enet)
target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
target_link_libraries(enet PUBLIC ws2_32 winmm)
install(FILES ${enet_SOURCE_DIR}/LICENSE DESTINATION licenses/enet)
add_library(uc_nat STATIC nat_mapping.cpp nat_upnp.cpp stun.cpp nat_tunnel.cpp network_session.cpp)
target_include_directories(uc_nat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_nat PUBLIC uc_netplay PRIVATE miniupnpc::miniupnpc enet ws2_32 ole32 bcrypt iphlpapi)
install(FILES ${miniupnpc_SOURCE_DIR}/LICENSE DESTINATION licenses/miniupnpc)
endif()
set(imgui_archive "https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/deps/imgui-1.92.9b.zip")
  set(imgui_archive "${CMAKE_SOURCE_DIR}/build/deps/imgui-1.92.9b.zip")
endif()
FetchContent_Declare(imgui URL "${imgui_archive}"
  URL_HASH SHA256=e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(imgui)
# The scroll arrows of a tab bar (they appear when the tabs do not fit): ImGui draws them narrow, as tall as the bar, so their background covers the bar's
# bottom line and they touch the last tab. Wider, two pixels shorter (the line stays visible) and a gap before them.
file(READ ${imgui_SOURCE_DIR}/imgui_widgets.cpp uc_imgui_widgets)
if(NOT uc_imgui_widgets MATCHES "uc_tab_scroll_buttons")
  string(REPLACE "const ImVec2 arrow_button_size(g.FontSize - 2.0f, g.FontSize + g.Style.FramePadding.y * 2.0f);" "const ImVec2 arrow_button_size(g.FontSize + 10.0f, g.FontSize + g.Style.FramePadding.y * 2.0f - 2.0f); /* uc_tab_scroll_buttons */" uc_imgui_patched "${uc_imgui_widgets}")
  string(REPLACE "tab_bar->BarRect.Max.x -= scrolling_buttons_width + 1.0f;" "tab_bar->BarRect.Max.x -= scrolling_buttons_width + 8.0f;" uc_imgui_patched "${uc_imgui_patched}")
  if(uc_imgui_patched STREQUAL uc_imgui_widgets OR NOT uc_imgui_patched MATCHES "scrolling_buttons_width \\+ 8.0f")
    message(FATAL_ERROR "imgui_widgets.cpp: the tab bar scrolling buttons were not found, the patch cannot be applied")
  endif()
  file(WRITE ${imgui_SOURCE_DIR}/imgui_widgets.cpp "${uc_imgui_patched}")
endif()
add_library(uc_imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_sources(uc_imgui PRIVATE ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(uc_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(uc_imgui PUBLIC SDL3::SDL3)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND WIN32)
  # SDL3-3.2.0's prefetch workaround needs the Clang intrinsic guard loaded first.
  target_compile_options(uc_imgui PRIVATE /FIintrin.h)
endif()
install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION licenses/imgui)
include(${CMAKE_SOURCE_DIR}/cmake/stb_image.cmake)
add_library(uc_imgui_vulkan STATIC ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
find_package(Vulkan 1.3 REQUIRED)
target_link_libraries(uc_imgui_vulkan PUBLIC uc_imgui Vulkan::Vulkan)
if(TARGET uc_volk)   # Android: ImGui's backend calls Vulkan through volk too (renderer/CMakeLists.txt)
  target_link_libraries(uc_imgui_vulkan PUBLIC uc_volk)
endif()
if(WIN32)
add_library(uc_imgui_dx11 STATIC ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_link_libraries(uc_imgui_dx11 PUBLIC uc_imgui d3d11 d3dcompiler)
add_library(uc_imgui_dx12 STATIC ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
target_link_libraries(uc_imgui_dx12 PUBLIC uc_imgui d3d12 d3dcompiler)
endif()
# The launcher's look and fonts, shared with the in-game settings menu (uc.exe).
add_library(uc_ui_style STATIC graphite_style.cpp ui_fonts.cpp dlss5_panel.cpp)
target_link_libraries(uc_ui_style PRIVATE uc_public_release)
target_include_directories(uc_ui_style PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/renderer)   # dlss5_panel draws the table of renderer/dlss5_settings.h
target_link_libraries(uc_ui_style PUBLIC uc_imgui)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND WIN32)
  target_compile_options(uc_ui_style PRIVATE /FIintrin.h)
endif()
add_library(uc_deck_pages STATIC deck/settings_pages.cpp deck/rows.cpp deck/mods_page.cpp)   # the Steam Deck launcher's tabs over the settings registry and the mod manager (built on both platforms so the host tests cover it)
target_include_directories(uc_deck_pages PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/deck)
target_link_libraries(uc_deck_pages PUBLIC uc_deck_nav uc_settings_registry uc_gamepad_panel uc_binding_editor uc_ui_style uc_imgui uc_mod_menu)
add_library(uc_deck_nav STATIC deck/focus.cpp deck/nav_keys.cpp)   # the Deck launcher's focus model and key queue (no drawing): host-testable
target_include_directories(uc_deck_nav PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/deck)
target_link_libraries(uc_deck_nav PUBLIC SDL3::SDL3)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND WIN32)   # SDL3-3.2.0's prefetch workaround needs the Clang intrinsic guard loaded first
  target_compile_options(uc_deck_nav PUBLIC /FIintrin.h)
endif()
add_library(uc_deck_game STATIC deck/game_path.cpp deck/exec_game.cpp)   # whether the chosen path holds the game, and the command that starts it (host-testable)
target_include_directories(uc_deck_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/deck)
add_library(uc_file_browser STATIC deck/file_browser.cpp)   # the Deck launcher's file browser model (no UI): built on both platforms so the host tests cover it
target_include_directories(uc_file_browser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/deck)
target_link_libraries(uc_file_browser PUBLIC nlohmann_json::nlohmann_json)
if(WIN32)   # the launcher program and its update window
add_library(uc_update_ui STATIC update_ui.cpp)
target_link_libraries(uc_update_ui PUBLIC uc_launcher_update uc_launcher_locale uc_imgui)
target_compile_definitions(uc_update_ui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_executable(uc_launcher WIN32 main.cpp graphite.cpp graphite_art.cpp ${CMAKE_SOURCE_DIR}/assets/app.rc)
target_link_libraries(uc_launcher PRIVATE uc_update_ui uc_public_release)
set(uc_launcher_libraries uc_binding_editor uc_settings_registry uc_gamepad_panel uc_gamepad_input uc_wiimote_input uc_launch uc_image_unpack uc_cpu_calibration uc_imgui uc_ui_style uc_prompt_icons uc_launcher_locale uc_nat uc_mod_menu shell32 ole32 windowscodecs dwmapi)
target_link_libraries(uc_launcher PRIVATE ${uc_launcher_libraries})
add_executable(test_launcher_update_bootstrap WIN32 ${CMAKE_SOURCE_DIR}/tests/test_launcher_update_bootstrap.cpp graphite.cpp graphite_art.cpp ${CMAKE_SOURCE_DIR}/assets/app.rc)
target_link_libraries(test_launcher_update_bootstrap PRIVATE ${uc_launcher_libraries} uc_update_ui)
target_include_directories(test_launcher_update_bootstrap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/renderer)
target_compile_options(test_launcher_update_bootstrap PRIVATE /FIintrin.h)
add_custom_command(TARGET test_launcher_update_bootstrap POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3-shared> $<TARGET_FILE_DIR:test_launcher_update_bootstrap>)
target_include_directories(uc_launcher PRIVATE ${CMAKE_SOURCE_DIR}/renderer)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND WIN32)
  target_compile_options(uc_launcher PRIVATE /FIintrin.h)
endif()
add_custom_command(TARGET uc_launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3-shared> $<TARGET_FILE_DIR:uc_launcher>)
install(PROGRAMS ${UC_COMPAT_DIR}/launcher/uc_launcher.exe DESTINATION .)   # the x86-64-v2 build: the launcher starts on every CPU (UC_ARCH in the root CMakeLists.txt)
add_custom_command(TARGET uc_launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:uc_launcher>/assets/launcher
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/assets/launcher/graphite-train.png $<TARGET_FILE_DIR:uc_launcher>/assets/launcher/graphite-train.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/assets/launcher/graphite-brand.png $<TARGET_FILE_DIR:uc_launcher>/assets/launcher/graphite-brand.png)
install(FILES ${CMAKE_SOURCE_DIR}/assets/launcher/graphite-train.png ${CMAKE_SOURCE_DIR}/assets/launcher/graphite-brand.png DESTINATION assets/launcher)
else()   # the Steam Deck launcher (docs/superpowers/specs/2026-10-31-steamdeck-launcher-design.md)
# Wit (Wiimms ISO Tools, GPL-2.0+) unpacks the disc image chosen in the launcher: the same version as the Windows package, the static Linux build. It runs as a
# separate program; only the binary is installed. Android has no launcher (the game starts from its app), so neither.
if(NOT ANDROID)
set(wit_linux_archive "https://wit.wiimm.de/download/wit-v3.05a-r8638-x86_64.tar.gz")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/deps/wit-v3.05a-r8638-x86_64.tar.gz")
  set(wit_linux_archive "${CMAKE_SOURCE_DIR}/build/deps/wit-v3.05a-r8638-x86_64.tar.gz")
endif()
FetchContent_Declare(wit_linux URL "${wit_linux_archive}"
  URL_HASH SHA256=df95a14f1b4b2bd95d3111b70c6304e91d4ed3e4c7f72b1ad5306c3616d8933f DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(wit_linux)
endif()
if(NOT ANDROID)
add_library(uc_update_ui STATIC update_ui.cpp)
target_link_libraries(uc_update_ui PUBLIC uc_launcher_update uc_launcher_locale uc_imgui)
endif()
add_library(uc_launch_probe STATIC launch_posix.cpp)   # query_features: the GPU report asked of uc itself
target_include_directories(uc_launch_probe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/game)
target_link_libraries(uc_launch_probe PUBLIC uc_options_config uc_netplay)   # launch_game checks a network endpoint
add_library(uc_image_unpack STATIC image_unpack_posix.cpp image_unpack_common.cpp)   # the Linux ImageUnpack (Wit through posix_spawn)
target_include_directories(uc_image_unpack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT ANDROID)   # the launcher program; on Android the app starts the game itself
# One program, two views: main_linux.cpp picks the Steam Deck's Game Mode launcher (deck/main_deck.cpp) or the launcher of the Windows build (main.cpp) at start.
# network_unavailable_posix.cpp and cpu_calibration_posix.cpp stand in for the Windows-only network play and CPU calibration code that main.cpp names.
add_executable(uc_launcher main_linux.cpp main.cpp network_unavailable_posix.cpp cpu_calibration_posix.cpp
  deck/main_deck.cpp deck/paths_posix.cpp deck/instance_lock.cpp deck/file_browser_ui.cpp graphite_art.cpp graphite_posix.cpp)
target_include_directories(uc_launcher PRIVATE ${stb_image_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/deck ${CMAKE_SOURCE_DIR}/common ${CMAKE_SOURCE_DIR}/renderer)
target_link_libraries(uc_launcher PRIVATE uc_ui_mode uc_update_ui uc_deck_nav uc_deck_pages uc_deck_game uc_file_browser uc_launch_probe uc_image_unpack uc_gamepad_input uc_ui_style uc_settings_registry uc_launcher_locale uc_options_config uc_imgui
  uc_binding_editor uc_gamepad_panel uc_wiimote_input uc_prompt_icons uc_mod_menu uc_diagnostics uc_public_release uc_netplay uc_cpu_topology)
add_custom_command(TARGET uc_launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:uc_launcher>/assets/launcher
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/assets/launcher/graphite-train.png $<TARGET_FILE_DIR:uc_launcher>/assets/launcher/graphite-train.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/assets/launcher/graphite-brand.png $<TARGET_FILE_DIR:uc_launcher>/assets/launcher/graphite-brand.png)
endif()
# The Deck package is assembled by scripts/linux/deck.py (package): `cmake --install` of this tree would also want the unbuilt test tools.
endif()
