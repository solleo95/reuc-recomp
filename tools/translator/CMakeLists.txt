add_executable(translator main.cpp)
target_link_libraries(translator PRIVATE uc_common)
install(TARGETS translator RUNTIME DESTINATION tools)
