# Stable test build outputs forward into the atomically published fixture generation.
include(${GEN}/game_sources.cmake)
foreach(name game_decl.h game_table.cpp game_000.cpp)
  file(WRITE ${GEN}/${name} "#include \"${UC_GENERATED_INCLUDE_DIR}/${name}\"\n")
endforeach()
