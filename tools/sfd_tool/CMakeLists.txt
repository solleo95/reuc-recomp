install(FILES sfd_tool.py sfd_tool.bat DESTINATION tools)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_sfd_tool COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/test_sfd_tool.py ${CMAKE_SOURCE_DIR})
