add_library(uc_runtime STATIC guest_mem.cpp dispatch.cpp)
target_include_directories(uc_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
