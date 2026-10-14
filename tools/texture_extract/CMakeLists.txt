add_library(uc_texture_decode SHARED decode.cpp)
target_link_libraries(uc_texture_decode PRIVATE uc_gx)
install(TARGETS uc_texture_decode RUNTIME DESTINATION tools LIBRARY DESTINATION tools)
install(FILES extract_textures.py extract_textures.bat DESTINATION tools)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_texture_extract COMMAND ${Python3_EXECUTABLE} -I
  ${CMAKE_SOURCE_DIR}/tests/test_texture_extract.py ${CMAKE_SOURCE_DIR} $<TARGET_FILE:uc_texture_decode>)
