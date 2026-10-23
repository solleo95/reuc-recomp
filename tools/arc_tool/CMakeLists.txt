install(FILES arc_tool.py arc_tool.bat DESTINATION tools)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_arc_tool COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/test_arc_tool.py ${CMAKE_SOURCE_DIR})
