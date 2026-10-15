add_library(uc_gx STATIC gx_vertices.cpp gx_packets.cpp gx_textures.cpp gx_transforms.cpp gx_materials.cpp prompt_draw.cpp)
target_include_directories(uc_gx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_gx PUBLIC uc_input_prompts)

enable_language(C)
find_package(Vulkan 1.3 REQUIRED)
find_program(UC_SLANGC slangc HINTS "$ENV{VULKAN_SDK}/Bin" "C:/VulkanSDK/1.4.363.0/Bin" REQUIRED)
include(FetchContent)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(sdl_archive "https://github.com/libsdl-org/SDL/releases/download/release-3.2.0/SDL3-3.2.0.tar.gz")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/SDL3-3.2.0.tar.gz")
  set(sdl_archive "${CMAKE_SOURCE_DIR}/build/SDL3-3.2.0.tar.gz")
endif()
FetchContent_Declare(SDL3 URL "${sdl_archive}"
  URL_HASH SHA256=bf308f92c5688b1479faf5cfe24af72f3cd4ce08d0c0670d6ce55bc2ec1e9a5e
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(SDL3)
if(WIN32)
  include(patch_sdl.cmake)
endif()
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
foreach(stage vert frag)
  if(stage STREQUAL "vert")
    set(entry vertexMain)
    set(shader_stage vertex)
  else()
    set(entry fragmentMain)
    set(shader_stage fragment)
  endif()
  add_custom_command(OUTPUT ${shader_dir}/smoke.${stage}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -target spirv -profile spirv_1_5 -entry ${entry} -stage ${shader_stage} -fvk-use-entrypoint-name -o ${shader_dir}/smoke.${stage}.spv
    DEPENDS shaders/smoke.slang shaders/tev.slang VERBATIM)
endforeach()
# Ubershader: the fragment shader with its TEV state read from the draw uniforms; used while a specialized pipeline compiles.
add_custom_command(OUTPUT ${shader_dir}/smoke.uber.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DUBER -target spirv -profile spirv_1_5 -entry fragmentMain -stage fragment -fvk-use-entrypoint-name -o ${shader_dir}/smoke.uber.spv
  DEPENDS shaders/smoke.slang shaders/tev.slang VERBATIM)
add_custom_target(uc_smoke_shaders DEPENDS ${shader_dir}/smoke.vert.spv ${shader_dir}/smoke.frag.spv ${shader_dir}/smoke.uber.spv)
add_library(uc_prompt_icons STATIC prompt_icons.cpp)
target_include_directories(uc_prompt_icons PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_prompt_icons PUBLIC uc_input_prompts uc_imgui)
add_library(uc_vulkan STATIC vulkan_renderer.cpp png.cpp prompt_overlay.cpp)
target_link_libraries(uc_vulkan PUBLIC uc_gx SDL3::SDL3 Vulkan::Vulkan windowscodecs uc_imgui_vulkan uc_prompt_icons)
add_dependencies(uc_vulkan uc_smoke_shaders)
set_property(TARGET uc_vulkan APPEND PROPERTY INTERFACE_LINK_DEPENDS
  ${shader_dir}/smoke.vert.spv ${shader_dir}/smoke.frag.spv ${shader_dir}/smoke.uber.spv)
add_executable(renderer_smoke ${CMAKE_SOURCE_DIR}/tests/renderer_smoke.cpp)
target_include_directories(renderer_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(renderer_smoke PRIVATE uc_vulkan windowscodecs)
add_custom_command(TARGET renderer_smoke POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:renderer_smoke>/data/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${shader_dir}/smoke.vert.spv ${shader_dir}/smoke.frag.spv ${shader_dir}/smoke.uber.spv $<TARGET_FILE_DIR:renderer_smoke>/data/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3-shared> $<TARGET_FILE_DIR:renderer_smoke> VERBATIM)
install(TARGETS renderer_smoke RUNTIME DESTINATION tools)
install(FILES $<TARGET_FILE:SDL3-shared> DESTINATION tools)
install(FILES ${shader_dir}/smoke.vert.spv ${shader_dir}/smoke.frag.spv ${shader_dir}/smoke.uber.spv DESTINATION tools/data/shaders)
install(FILES ${sdl3_SOURCE_DIR}/LICENSE.txt DESTINATION licenses/SDL3)
