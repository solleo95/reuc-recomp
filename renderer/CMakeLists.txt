add_library(uc_gx STATIC gx_vertices.cpp gx_packets.cpp gx_textures.cpp texture_replacement.cpp gx_transforms.cpp gx_materials.cpp prompt_draw.cpp scene_boundary.cpp motion_tracker.cpp scene_lights.cpp)
target_include_directories(uc_gx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_gx PUBLIC uc_input_prompts)

enable_language(C)
find_package(Vulkan 1.3 REQUIRED)
include(${CMAKE_SOURCE_DIR}/cmake/upscaler_sdks.cmake)
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
    DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
endforeach()
# Ubershader: the fragment shader with its TEV state read from the draw uniforms; used while a specialized pipeline compiles.
add_custom_command(OUTPUT ${shader_dir}/smoke.uber.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DUBER -target spirv -profile spirv_1_5 -entry fragmentMain -stage fragment -fvk-use-entrypoint-name -o ${shader_dir}/smoke.uber.spv
  DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
# Upscaling variants: jitter in the vertex shader, motion vectors as a second color output.
foreach(stage vert frag uber)
  if(stage STREQUAL "vert")
    set(motion_args -entry vertexMain -stage vertex)
  elseif(stage STREQUAL "frag")
    set(motion_args -entry fragmentMain -stage fragment)
  else()
    set(motion_args -DUBER -entry fragmentMain -stage fragment)
  endif()
  add_custom_command(OUTPUT ${shader_dir}/smoke.motion.${stage}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DMOTION ${motion_args} -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smoke.motion.${stage}.spv
    DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
endforeach()
# Shadow map vertex stage: the motion vertex shader without lighting and texture coordinates (depth only, no fragment stage).
add_custom_command(OUTPUT ${shader_dir}/smoke.shadow.vert.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DMOTION -DSHADOW -entry vertexMain -stage vertex -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smoke.shadow.vert.spv
  DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
add_custom_command(OUTPUT ${shader_dir}/rcas.comp.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rcas.slang -entry main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/rcas.comp.spv
  DEPENDS shaders/rcas.slang VERBATIM)
# SMAA 1x (third_party/smaa, MIT): three compute entry points of one shader.
foreach(smaa_pass edges weights blend)
  add_custom_command(OUTPUT ${shader_dir}/smaa.${smaa_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smaa.slang -I ${CMAKE_SOURCE_DIR}/third_party/smaa -entry ${smaa_pass}Main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smaa.${smaa_pass}.spv
    DEPENDS shaders/smaa.slang ${CMAKE_SOURCE_DIR}/third_party/smaa/SMAA.hlsl VERBATIM)
endforeach()
# Ground-truth ambient occlusion: three compute entry points of one shader.
foreach(gtao_pass ao denoise contact composite)
  add_custom_command(OUTPUT ${shader_dir}/gtao.${gtao_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gtao.slang -entry ${gtao_pass}Main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/gtao.${gtao_pass}.spv
    DEPENDS shaders/gtao.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
endforeach()
# Ray tracing: the BLAS transform pass.
add_custom_command(OUTPUT ${shader_dir}/rt_transform.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rt_transform.slang -entry main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/rt_transform.spv
  DEPENDS shaders/rt_transform.slang VERBATIM)
# The own denoiser of ray-traced signals: temporal accumulation and one à-trous pass, two entry points of one shader.
foreach(denoise_pass temporal atrous)
  add_custom_command(OUTPUT ${shader_dir}/rt_denoise.${denoise_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rt_denoise.slang -entry ${denoise_pass}Main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/rt_denoise.${denoise_pass}.spv
    DEPENDS shaders/rt_denoise.slang shaders/gbuffer.slang VERBATIM)
endforeach()
# Ray-traced shadows: the visibility trace (ray query) and the composite, two entry points of one shader.
foreach(shadow_pass trace composite)
  add_custom_command(OUTPUT ${shader_dir}/rt_shadow.${shadow_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rt_shadow.slang -entry ${shadow_pass}Main -stage compute -target spirv -profile spirv_1_5 -capability spvRayQueryKHR -fvk-use-entrypoint-name -o ${shader_dir}/rt_shadow.${shadow_pass}.spv
    DEPENDS shaders/rt_shadow.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
endforeach()
# Ray-traced reflections: the trace (ray query) and the composite.
foreach(reflect_pass trace composite)
  add_custom_command(OUTPUT ${shader_dir}/rt_reflect.${reflect_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rt_reflect.slang -entry ${reflect_pass}Main -stage compute -target spirv -profile spirv_1_5 -capability spvRayQueryKHR -fvk-use-entrypoint-name -o ${shader_dir}/rt_reflect.${reflect_pass}.spv
    DEPENDS shaders/rt_reflect.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
endforeach()
# Path tracing: the albedo accumulation, the trace (ray query through the hit table) and the composite.
foreach(path_pass accumulate list trace composite)
  add_custom_command(OUTPUT ${shader_dir}/rt_path.${path_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rt_path.slang -entry ${path_pass}Main -stage compute -target spirv -profile spirv_1_5 -capability spvRayQueryKHR -fvk-use-entrypoint-name -o ${shader_dir}/rt_path.${path_pass}.spv
    DEPENDS shaders/rt_path.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
endforeach()
# The guides of DLSS Ray Reconstruction (normal + roughness, specular albedo).
add_custom_command(OUTPUT ${shader_dir}/rr_inputs.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rr_inputs.slang -entry inputsMain -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/rr_inputs.spv
  DEPENDS shaders/rr_inputs.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
# DLSS 5 Neural Rendering: the bridge between the RGBA8 picture and the network's RGBA16F signal, both directions from one source.
foreach(bridge prepare resolve)
  string(TOUPPER ${bridge} bridge_define)
  add_custom_command(OUTPUT ${shader_dir}/dlss5.${bridge}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/dlss5_bridge.slang -D${bridge_define} -entry main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/dlss5.${bridge}.spv
    DEPENDS shaders/dlss5_bridge.slang VERBATIM)
endforeach()
foreach(blur_pass tile blur)
  add_custom_command(OUTPUT ${shader_dir}/motion_blur.${blur_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/motion_blur.slang -entry ${blur_pass}Main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/motion_blur.${blur_pass}.spv
    DEPENDS shaders/motion_blur.slang VERBATIM)
endforeach()
# HDR10 output: the encode of the finished frame into the PQ swapchain.
foreach(stage vert frag)
  if(stage STREQUAL "vert")
    set(hdr_args -entry vertexMain -stage vertex)
  else()
    set(hdr_args -entry fragmentMain -stage fragment)
  endif()
  add_custom_command(OUTPUT ${shader_dir}/hdr.${stage}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hdr.slang ${hdr_args} -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/hdr.${stage}.spv
    DEPENDS shaders/hdr.slang VERBATIM)
endforeach()
set(UC_SHADER_FILES ${shader_dir}/hdr.vert.spv ${shader_dir}/hdr.frag.spv ${shader_dir}/smoke.vert.spv ${shader_dir}/smoke.frag.spv ${shader_dir}/smoke.uber.spv
  ${shader_dir}/smoke.motion.vert.spv ${shader_dir}/smoke.motion.frag.spv ${shader_dir}/smoke.motion.uber.spv ${shader_dir}/rcas.comp.spv
  ${shader_dir}/smaa.edges.spv ${shader_dir}/smaa.weights.spv ${shader_dir}/smaa.blend.spv
  ${shader_dir}/smoke.shadow.vert.spv ${shader_dir}/gtao.ao.spv ${shader_dir}/gtao.denoise.spv ${shader_dir}/gtao.contact.spv ${shader_dir}/gtao.composite.spv
  ${shader_dir}/motion_blur.tile.spv ${shader_dir}/motion_blur.blur.spv ${shader_dir}/rt_transform.spv
  ${shader_dir}/rt_denoise.temporal.spv ${shader_dir}/rt_denoise.atrous.spv ${shader_dir}/rt_shadow.trace.spv ${shader_dir}/rt_shadow.composite.spv
  ${shader_dir}/rt_reflect.trace.spv ${shader_dir}/rt_reflect.composite.spv ${shader_dir}/rt_path.accumulate.spv ${shader_dir}/rt_path.list.spv ${shader_dir}/rt_path.trace.spv ${shader_dir}/rt_path.composite.spv ${shader_dir}/rr_inputs.spv ${shader_dir}/dlss5.prepare.spv ${shader_dir}/dlss5.resolve.spv CACHE INTERNAL "")
add_custom_target(uc_smoke_shaders DEPENDS ${UC_SHADER_FILES})
# DLSS 5 Neural Rendering: the support/failure/parameter logic above the NGX SDK (no SDK or GPU needed, tested with a fake backend).
add_library(uc_dlss5 STATIC ngx_dlss5.cpp)
target_include_directories(uc_dlss5 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_dlss5 PUBLIC Vulkan::Vulkan)
add_library(uc_prompt_icons STATIC prompt_icons.cpp)
target_include_directories(uc_prompt_icons PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_prompt_icons PUBLIC uc_input_prompts uc_imgui)
add_library(uc_vulkan STATIC vulkan_renderer.cpp png.cpp prompt_overlay.cpp menu_overlay.cpp blit_upscaler.cpp smaa_upscaler.cpp occlusion_pass.cpp motion_blur_pass.cpp rt_scene.cpp rt_denoiser.cpp rt_shadow_pass.cpp rt_reflection_pass.cpp rt_path_pass.cpp rr_inputs_pass.cpp shadow_map.cpp rcas_pass.cpp hdr_pass.cpp dlss5_pass.cpp pass_timer.cpp streamline.cpp upscaler_probe.cpp dlss_upscaler.cpp dlss_frame_generation.cpp fsr_upscaler.cpp fsr_frame_generation.cpp ngx_vulkan.cpp)
target_include_directories(uc_vulkan PRIVATE ${CMAKE_SOURCE_DIR}/third_party/smaa)   # AreaTex.h, SearchTex.h
target_link_libraries(uc_vulkan PUBLIC uc_gx SDL3::SDL3 Vulkan::Vulkan windowscodecs uc_imgui_vulkan uc_prompt_icons uc_dlss5 PRIVATE uc_streamline_sdk uc_fidelityfx_sdk uc_ngx_sdk)
add_dependencies(uc_vulkan uc_smoke_shaders)
set_property(TARGET uc_vulkan APPEND PROPERTY INTERFACE_LINK_DEPENDS ${UC_SHADER_FILES})
add_executable(renderer_smoke ${CMAKE_SOURCE_DIR}/tests/renderer_smoke.cpp)
target_include_directories(renderer_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(renderer_smoke PRIVATE uc_vulkan windowscodecs)
add_custom_command(TARGET renderer_smoke POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:renderer_smoke>/data/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UC_SHADER_FILES} $<TARGET_FILE_DIR:renderer_smoke>/data/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3-shared> $<TARGET_FILE_DIR:renderer_smoke> VERBATIM)
add_executable(renderer_upscale_smoke ${CMAKE_SOURCE_DIR}/tests/renderer_upscale_smoke.cpp)
target_include_directories(renderer_upscale_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(renderer_upscale_smoke PRIVATE uc_vulkan)
add_dependencies(renderer_upscale_smoke renderer_smoke)   # shares its data/shaders and SDL copy
add_custom_command(TARGET renderer_upscale_smoke POST_BUILD   # --dlss: Streamline and DLSS next to the test
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UC_UPSCALER_RUNTIME} $<TARGET_FILE_DIR:renderer_upscale_smoke> VERBATIM)
add_executable(renderer_dlss5_smoke ${CMAKE_SOURCE_DIR}/tests/renderer_dlss5_smoke.cpp)
target_include_directories(renderer_dlss5_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(renderer_dlss5_smoke PRIVATE uc_vulkan)
add_dependencies(renderer_dlss5_smoke renderer_smoke)   # shares its data/shaders and SDL copy
add_executable(renderer_effects_smoke ${CMAKE_SOURCE_DIR}/tests/renderer_effects_smoke.cpp)
target_include_directories(renderer_effects_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(renderer_effects_smoke PRIVATE uc_vulkan)
add_dependencies(renderer_effects_smoke renderer_smoke)   # shares its data/shaders and SDL copy
install(TARGETS renderer_smoke RUNTIME DESTINATION tools)
install(FILES $<TARGET_FILE:SDL3-shared> DESTINATION tools)
install(FILES ${UC_SHADER_FILES} DESTINATION tools/data/shaders)
install(FILES ${sdl3_SOURCE_DIR}/LICENSE.txt DESTINATION licenses/SDL3)
