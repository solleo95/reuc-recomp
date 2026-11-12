# Native build for macOS on Apple Silicon: Apple clang, the libraries from Homebrew (SDL3, the Vulkan loader, MoltenVK). See scripts/macos/build.sh.
# Not a cross build: the translator runs on this Mac too. The shader compiler comes in as -DUC_SLANGC=<Slang release>/bin/slangc.
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES arm64)
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0)   # libc++'s floating-point from_chars (options.cpp, dlss5_settings.h) is available from macOS 26; every Apple Silicon Mac runs it
endif()
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

# -ffp-contract=off as on Windows: Apple clang would fuse a*b+c into one FMA, which rounds once where the PowerPC rounds twice.
set(CMAKE_C_FLAGS_INIT "-ffp-contract=off")
set(CMAKE_CXX_FLAGS_INIT "-ffp-contract=off")
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")     # -O2 as the Windows build's /O2 (CMake's default -O3 also folds signalling-NaN round trips differently: test_ppc_float)
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")

if(EXISTS /opt/homebrew)
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)
endif()
