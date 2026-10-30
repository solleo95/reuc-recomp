# Cross build for x86-64 Linux (SteamOS, Steam Deck) from Windows: LLVM against the Steam Runtime "sniper" SDK sysroot.
# Set up the sysroot with scripts/linux/extract_sysroot.py; UC_LINUX_SYSROOT and UC_LLVM_DIR override the defaults.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED UC_LINUX_SYSROOT)
  set(UC_LINUX_SYSROOT "D:/Toolchains/sniper-sysroot")
endif()
if(NOT DEFINED UC_LLVM_DIR)
  set(UC_LLVM_DIR "C:/Program Files/LLVM")
endif()

set(CMAKE_SYSROOT "${UC_LINUX_SYSROOT}")
set(CMAKE_C_COMPILER "${UC_LLVM_DIR}/bin/clang.exe")
set(CMAKE_CXX_COMPILER "${UC_LLVM_DIR}/bin/clang++.exe")
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_AR "${UC_LLVM_DIR}/bin/llvm-ar.exe")
set(CMAKE_RANLIB "${UC_LLVM_DIR}/bin/llvm-ranlib.exe")
set(CMAKE_STRIP "${UC_LLVM_DIR}/bin/llvm-strip.exe")

if(NOT DEFINED UC_TUNE)
  set(UC_TUNE "znver2")   # -mtune target: the Steam Deck's CPU (measured on the Deck: +3.8% FPS in the first scene over generic); build.ps1 -Tune generic
endif()
set(CMAKE_C_FLAGS_INIT "-march=x86-64-v3 -mtune=${UC_TUNE} -pthread")
get_filename_component(UC_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_CXX_FLAGS_INIT "-march=x86-64-v3 -mtune=${UC_TUNE} -pthread -include \"${UC_ROOT_DIR}/runtime/linux_std_shim.h\"")   # quoted: the checkout path has a space
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")     # -O2 as the Windows build's /O2 (CMake's default -O3 also folds signalling-NaN round trips differently: test_ppc_float)
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
foreach(kind EXE SHARED MODULE)
  set(CMAKE_${kind}_LINKER_FLAGS_INIT "-fuse-ld=lld")
endforeach()

# Libraries and headers come from the sysroot; programs (cmake, slangc, python) from the host.
set(CMAKE_LIBRARY_ARCHITECTURE x86_64-linux-gnu)   # libraries live in <sysroot>/usr/lib/x86_64-linux-gnu; find_library needs it (no compiler probe in a cross build)
set(CMAKE_FIND_ROOT_PATH "${UC_LINUX_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
