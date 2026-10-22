install(FILES dump_text.py dump_text.bat DESTINATION tools)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_dump_text COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/test_dump_text.py ${CMAKE_SOURCE_DIR})
