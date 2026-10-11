add_executable(s1_scan main.cpp)
target_link_libraries(s1_scan PRIVATE uc_common)
install(TARGETS s1_scan RUNTIME DESTINATION tools)
