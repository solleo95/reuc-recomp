# The translator must actually drop/fold the CR writes of the crflow program (the run test only proves the result is unchanged).
include(${GEN}/game_sources.cmake)
file(READ ${UC_GENERATED_INCLUDE_DIR}/game_000.cpp text)
foreach(needle "is never read" "compare is folded into the next branch" "ppc::op_cmpi(c, 0, 4, 3)" "ppc::op_cmpi(c, 2, 4, 5)")
  string(FIND "${text}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "expected '${needle}' in the translated crflow program")
  endif()
endforeach()
