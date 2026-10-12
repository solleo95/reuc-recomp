add_executable(gen_e2e_dols gen_e2e_dols.cpp)
target_include_directories(gen_e2e_dols PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_SOURCE_DIR}/runtime)

set(E2E_PROGRAMS loop calls jt0 jt1 jt2 jt9 indirect float ps datawords badbctr illegal_exec secondary fallthrough outside)
foreach(p ${E2E_PROGRAMS})
  set(gen ${CMAKE_CURRENT_BINARY_DIR}/gen_${p})
  add_custom_command(
    OUTPUT ${gen}/game_decl.h ${gen}/game_table.cpp ${gen}/game_000.cpp
    COMMAND gen_e2e_dols ${p} ${gen}/${p}.dol
    COMMAND translator --dol ${gen}/${p}.dol --out ${gen} --shard 100000
    DEPENDS gen_e2e_dols translator
    COMMENT "translate e2e program ${p}")
  add_executable(e2e_${p} e2e_main.cpp ${gen}/game_table.cpp ${gen}/game_000.cpp)
  target_compile_definitions(e2e_${p} PRIVATE E2E_NAME="${p}")
  target_compile_options(e2e_${p} PRIVATE -Wno-unused-label -Wno-unused-variable)
  target_include_directories(e2e_${p} PRIVATE ${gen} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(e2e_${p} PRIVATE uc_common test_support)
  add_test(NAME e2e_${p} COMMAND e2e_${p})
endforeach()
set_tests_properties(e2e_badbctr PROPERTIES PASS_REGULAR_EXPRESSION "guest trap at 80003FFC")
set_tests_properties(e2e_illegal_exec PROPERTIES PASS_REGULAR_EXPRESSION "guest trap at 80003104.*illegal or data word 14000000")
set_tests_properties(e2e_outside PROPERTIES PASS_REGULAR_EXPRESSION "guest trap at 80003100.*not inside any translated function")

add_test(NAME translator_deterministic
  COMMAND ${CMAKE_COMMAND} -DTRANSLATOR=$<TARGET_FILE:translator> -DDOL=${CMAKE_CURRENT_BINARY_DIR}/gen_jt1/jt1.dol
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/det -P ${CMAKE_CURRENT_SOURCE_DIR}/compare.cmake)

add_test(NAME translator_cleans_stale
  COMMAND ${CMAKE_COMMAND} -DTRANSLATOR=$<TARGET_FILE:translator> -DDOL=${CMAKE_CURRENT_BINARY_DIR}/gen_jt1/jt1.dol
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/stale -P ${CMAKE_CURRENT_SOURCE_DIR}/stale.cmake)

add_test(NAME translator_two_cfgs
  COMMAND ${CMAKE_COMMAND} -DTRANSLATOR=$<TARGET_FILE:translator> -DDOL=${CMAKE_CURRENT_BINARY_DIR}/gen_jt1/jt1.dol
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/cfgs -P ${CMAKE_CURRENT_SOURCE_DIR}/../test_cfg_merge.cmake)
