# The tests that build for Linux (cross build, run on the Deck with scripts/linux/deck.py run build-linux/tests/linux/<name>).
# Each platform-port task adds its tests here; sources stay in tests/ and are shared with the Windows list in tests/CMakeLists.txt.
function(uc_linux_test name)
  add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/../${name}.cpp)
  # runtime/ by quote only: runtime/sched.h would otherwise shadow <sched.h>, which glibc's <pthread.h> includes
  target_compile_options(${name} PRIVATE "-iquote${CMAKE_SOURCE_DIR}/runtime")
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/common)
  if(ARGN)
    target_link_libraries(${name} PRIVATE ${ARGN})
  endif()
endfunction()

uc_linux_test(test_compat_bswap)

add_library(uc_clock_only STATIC ${CMAKE_SOURCE_DIR}/runtime/clock.cpp)   # until the whole runtime links for Linux
target_compile_options(uc_clock_only PUBLIC "-iquote${CMAKE_SOURCE_DIR}/runtime")
add_library(uc_guest_mem_only STATIC ${CMAKE_SOURCE_DIR}/runtime/guest_mem_posix.cpp)   # until the whole runtime links for Linux
uc_linux_test(test_guest_mem_alias uc_guest_mem_only)
target_include_directories(test_guest_mem_alias PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # check.h

set(R ${CMAKE_SOURCE_DIR}/runtime)
add_library(uc_crash_only STATIC ${R}/fatal_posix.cpp ${R}/crash_report_posix.cpp ${R}/dispatch.cpp ${R}/trace.cpp ${R}/guest_mem_posix.cpp ${R}/clock.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../mmio_stub.cpp)   # until the whole runtime links for Linux
target_compile_options(uc_crash_only PUBLIC "-iquote${R}")
target_link_libraries(uc_crash_only PUBLIC ${CMAKE_DL_LIBS})   # dladdr, glibc 2.31 keeps it in libdl
uc_linux_test(test_crash_report_posix uc_crash_only)
target_include_directories(test_crash_report_posix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # check.h

add_library(uc_savefs_only STATIC ${R}/savefs_posix.cpp ${R}/save_rollback.cpp)   # until the whole runtime links for Linux
target_compile_options(uc_savefs_only PUBLIC "-iquote${R}")
foreach(t test_savefs test_save_rollback test_savefs_atomic)
  uc_linux_test(${t} uc_savefs_only)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # check.h, test_pid.h
endforeach()

uc_linux_test(test_path_io uc_path_io)
target_include_directories(test_path_io PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # check.h
uc_linux_test(test_config_save_posix uc_input_config)
target_include_directories(test_config_save_posix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # check.h, test_pid.h
uc_linux_test(test_cpu_topology_posix uc_cpu_topology)
target_include_directories(test_cpu_topology_posix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # check.h
uc_linux_test(test_diagnostic_log_posix uc_diagnostics)
target_include_directories(test_diagnostic_log_posix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # check.h, test_pid.h

add_library(test_support STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../mmio_stub.cpp)   # the same stubs the Windows tests use
target_link_libraries(test_support PUBLIC uc_runtime)
foreach(t test_ppc_int test_ppc_mem test_ppc_float test_mmio test_lowmem test_sched test_sched_restore test_os_hle test_dvd test_vi test_exi test_ios test_dsp
          test_gx_audit test_sdk_math_hle test_sdk_regs_hle test_save_hle test_rollback_memory)   # not test_leaf_hle: it catches host faults with Windows SEH (__try/__except)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../${t}.cpp)
    uc_linux_test(${t} test_support)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # check.h
  endif()
endforeach()

foreach(t test_clock test_clock_sleep)
  uc_linux_test(${t} uc_clock_only)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # check.h
endforeach()
