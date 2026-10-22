# The native example mod: a DLL with its own copy of Dear ImGui (exactly the version the port links, see launcher/CMakeLists.txt).
# Added from the root CMakeLists.txt after the launcher, which fetches ImGui. A mod author outside this repository does the same with
# modding/uc_mod.h and the ImGui release named in UC_MOD_IMGUI_VERSION.
FetchContent_GetProperties(imgui SOURCE_DIR hello_imgui_dir)
add_library(hello_native MODULE src/hello_native.cpp
  ${hello_imgui_dir}/imgui.cpp ${hello_imgui_dir}/imgui_draw.cpp ${hello_imgui_dir}/imgui_tables.cpp ${hello_imgui_dir}/imgui_widgets.cpp)
target_include_directories(hello_native PRIVATE ${hello_imgui_dir} ${CMAKE_SOURCE_DIR}/modding)
set_target_properties(hello_native PROPERTIES PREFIX "" OUTPUT_NAME hello_native)
install(TARGETS hello_native RUNTIME DESTINATION mods-examples/hello_native/native LIBRARY DESTINATION mods-examples/hello_native/native)
