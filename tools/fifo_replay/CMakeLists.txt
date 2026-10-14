add_library(uc_dff STATIC dff.cpp memory.cpp)
target_include_directories(uc_dff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_library(uc_fifo_stream STATIC gx_stream.cpp gx_state.cpp gx_texture.cpp)
target_link_libraries(uc_fifo_stream PUBLIC uc_dff uc_gx)
add_executable(uc_fifo_replay main.cpp)
add_library(uc_replay_images STATIC image_compare.cpp)
target_include_directories(uc_replay_images PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_replay_images PUBLIC uc_vulkan)
target_link_libraries(uc_fifo_replay PRIVATE uc_fifo_stream uc_replay_images)
add_custom_command(TARGET uc_fifo_replay POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:uc_fifo_replay>/data/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/renderer/shaders/smoke.vert.spv ${CMAKE_BINARY_DIR}/renderer/shaders/smoke.frag.spv $<TARGET_FILE_DIR:uc_fifo_replay>/data/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3-shared> $<TARGET_FILE_DIR:uc_fifo_replay> VERBATIM)
install(TARGETS uc_fifo_replay RUNTIME DESTINATION tools)
