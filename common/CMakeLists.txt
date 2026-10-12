add_library(uc_common STATIC dol.cpp dol_scan.cpp ppc/isa.cpp analyzer/config.cpp analyzer/analyzer.cpp translator/emit.cpp sig/dsy.cpp ppc/format.cpp)
target_include_directories(uc_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
