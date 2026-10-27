# The GX algorithms stay shared; only binding layout and clip-space Y differ.
set(UC_DIRECTX_SHADER_FILES)
foreach(api dx11 dx12)
  if(api STREQUAL "dx11")
    set(dx_target dxbc)
    set(dx_profile sm_5_0)
  else()
    set(dx_target dxil)
    set(dx_profile sm_6_0)
  endif()
  foreach(shader smoke smoke.motion smoke.logic smoke.motion.logic smoke.motion.logic.color smoke.shadow blit hdr depth)
    set(dx_defines -DUC_DIRECTX -DUBER)
    if(shader MATCHES "motion")
      list(APPEND dx_defines -DMOTION)
    endif()
    if(shader MATCHES "logic")
      list(APPEND dx_defines -DUC_LOGIC)
    endif()
    if(shader MATCHES "logic.color")
      list(APPEND dx_defines -DUC_LOGIC_COLOR_ONLY)
    endif()
    if(shader STREQUAL "smoke.shadow")
      list(APPEND dx_defines -DSHADOW)
    endif()
    if(shader MATCHES "^smoke")
      set(dx_source ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang)
    elseif(shader STREQUAL "blit")
      set(dx_source ${CMAKE_CURRENT_SOURCE_DIR}/shaders/native_blit.slang)
    elseif(shader STREQUAL "depth")
      set(dx_source ${CMAKE_CURRENT_SOURCE_DIR}/shaders/native_depth.slang)
    else()
      set(dx_source ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hdr.slang)
    endif()
    foreach(stage vert frag)
      if(shader MATCHES "logic" AND stage STREQUAL "vert")
        continue()
      endif()
      if(shader STREQUAL "smoke.shadow" AND stage STREQUAL "frag")
        continue()
      endif()
      if(stage STREQUAL "vert")
        set(dx_entry vertexMain)
        set(dx_stage vertex)
      else()
        set(dx_entry fragmentMain)
        set(dx_stage fragment)
      endif()
      set(dx_output ${shader_dir}/${shader}.${api}.${stage}.cso)
      add_custom_command(OUTPUT ${dx_output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
        COMMAND ${UC_SLANGC} ${dx_source} ${dx_defines} -entry ${dx_entry} -stage ${dx_stage}
          -target ${dx_target} -profile ${dx_profile} -o ${dx_output}
        DEPENDS ${dx_source} shaders/tev.slang shaders/gbuffer.slang VERBATIM)
      list(APPEND UC_DIRECTX_SHADER_FILES ${dx_output})
    endforeach()
  endforeach()
  foreach(pass down_first down up composite)
    if(pass STREQUAL "down_first")
      set(dx_entry downFirstMain)
    elseif(pass STREQUAL "down")
      set(dx_entry downMain)
    elseif(pass STREQUAL "up")
      set(dx_entry upMain)
    else()
      set(dx_entry compositeMain)
    endif()
    set(dx_output ${shader_dir}/bloom.${api}.${pass}.cso)
    add_custom_command(OUTPUT ${dx_output}
      COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/bloom.slang -DUC_DIRECTX
        -entry ${dx_entry} -stage compute -target ${dx_target} -profile ${dx_profile} -o ${dx_output}
      DEPENDS shaders/bloom.slang VERBATIM)
    list(APPEND UC_DIRECTX_SHADER_FILES ${dx_output})
  endforeach()
  foreach(pass edges weights blend)
    set(dx_output ${shader_dir}/smaa.${api}.${pass}.cso)
    add_custom_command(OUTPUT ${dx_output}
      COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smaa.slang -DUC_DIRECTX
        -I ${CMAKE_SOURCE_DIR}/third_party/smaa -entry ${pass}Main -stage compute
        -target ${dx_target} -profile ${dx_profile} -o ${dx_output}
      DEPENDS shaders/smaa.slang ${CMAKE_SOURCE_DIR}/third_party/smaa/SMAA.hlsl VERBATIM)
    list(APPEND UC_DIRECTX_SHADER_FILES ${dx_output})
  endforeach()
  set(dx_output ${shader_dir}/rcas.${api}.cso)
  add_custom_command(OUTPUT ${dx_output}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rcas.slang -DUC_DIRECTX
      -entry main -stage compute -target ${dx_target} -profile ${dx_profile} -o ${dx_output}
    DEPENDS shaders/rcas.slang VERBATIM)
  list(APPEND UC_DIRECTX_SHADER_FILES ${dx_output})
  foreach(pass ao denoise contact composite)
    set(dx_output ${shader_dir}/gtao.${api}.${pass}.cso)
    add_custom_command(OUTPUT ${dx_output}
      COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gtao.slang -DUC_DIRECTX
        -entry ${pass}Main -stage compute -target ${dx_target} -profile ${dx_profile} -o ${dx_output}
      DEPENDS shaders/gtao.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
    list(APPEND UC_DIRECTX_SHADER_FILES ${dx_output})
  endforeach()
  foreach(pass tile blur)
    set(dx_output ${shader_dir}/motion_blur.${api}.${pass}.cso)
    add_custom_command(OUTPUT ${dx_output}
      COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/motion_blur.slang -DUC_DIRECTX
        -entry ${pass}Main -stage compute -target ${dx_target} -profile ${dx_profile} -o ${dx_output}
      DEPENDS shaders/motion_blur.slang VERBATIM)
    list(APPEND UC_DIRECTX_SHADER_FILES ${dx_output})
  endforeach()
endforeach()
foreach(shader rt_transform rt_shadow rt_reflect rt_path rt_denoise rr_inputs)
  if(shader STREQUAL "rt_transform")
    set(dx_entries main)
  elseif(shader STREQUAL "rt_path")
    set(dx_entries accumulateMain listMain traceMain compositeMain)
  elseif(shader STREQUAL "rt_denoise")
    set(dx_entries temporalMain atrousMain)
  elseif(shader STREQUAL "rr_inputs")
    set(dx_entries inputsMain)
  else()
    set(dx_entries traceMain compositeMain)
  endif()
  foreach(entry IN LISTS dx_entries)
    string(REPLACE "Main" "" pass ${entry})
    set(dx_output ${shader_dir}/${shader}.dx12.${pass}.cso)
    add_custom_command(OUTPUT ${dx_output}
      COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.slang -DUC_DIRECTX
        -entry ${entry} -stage compute -target dxil -profile sm_6_5 -o ${dx_output}
      DEPENDS shaders/${shader}.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
    list(APPEND UC_DIRECTX_SHADER_FILES ${dx_output})
  endforeach()
endforeach()
foreach(pass prepare resolve)
  string(TOUPPER ${pass} pass_define)
  set(dx_output ${shader_dir}/dlss5.dx12.${pass}.cso)
  add_custom_command(OUTPUT ${dx_output}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/dlss5_bridge.slang -DUC_DIRECTX -D${pass_define}
      -entry main -stage compute -target dxil -profile sm_6_0 -o ${dx_output}
    DEPENDS shaders/dlss5_bridge.slang VERBATIM)
  list(APPEND UC_DIRECTX_SHADER_FILES ${dx_output})
endforeach()
# The TEV-specialized pixel shaders (native_tev.cpp) are compiled by the game from this HLSL, with a table of the draw's TEV words in front.
foreach(variant plain motion)
  set(spec_defines -DUC_DIRECTX -DUBER -DUC_SPEC)
  if(variant STREQUAL "motion")
    list(APPEND spec_defines -DMOTION)
    set(spec_output ${shader_dir}/smoke.motion.spec.hlsl)
  else()
    set(spec_output ${shader_dir}/smoke.spec.hlsl)
  endif()
  add_custom_command(OUTPUT ${spec_output}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang ${spec_defines} -entry fragmentMain -stage fragment
      -target hlsl -profile sm_5_0 -o ${spec_output}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
  list(APPEND UC_DIRECTX_SHADER_FILES ${spec_output})
endforeach()
add_custom_target(uc_directx_shaders DEPENDS ${UC_DIRECTX_SHADER_FILES})
install(FILES ${UC_DIRECTX_SHADER_FILES} DESTINATION data/shaders)
