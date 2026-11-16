# Cross build for Android on arm64 (Snapdragon handhelds such as the AYN Thor): the NDK's own toolchain file plus the project's flags.
# The NDK comes from UC_ANDROID_NDK, else ANDROID_NDK_HOME, else the newest one in ~/Library/Android/sdk/ndk. See scripts/android/build.sh.
if(NOT DEFINED UC_ANDROID_NDK)
  if(DEFINED ENV{ANDROID_NDK_HOME})
    set(UC_ANDROID_NDK "$ENV{ANDROID_NDK_HOME}")
  else()
    file(GLOB uc_ndks "$ENV{HOME}/Library/Android/sdk/ndk/*")
    list(SORT uc_ndks COMPARE NATURAL ORDER DESCENDING)
    list(GET uc_ndks 0 UC_ANDROID_NDK)
  endif()
endif()
set(UC_ANDROID_NDK "${UC_ANDROID_NDK}" CACHE PATH "Android NDK")
set(ANDROID_ABI arm64-v8a CACHE STRING "")
set(ANDROID_PLATFORM android-33 CACHE STRING "")   # Android 13: the AYN Thor's (memfd_create, renameat2 and execinfo's backtrace are all there)
# libc++ inside libmain.so: game/allocator.cpp replaces operator new/delete with mimalloc, and a separate libc++_shared.so would free the game's
# blocks with Bionic's free (scudo aborts: "Pointer tag ... was truncated"). SDL3, the only other library, is C.
set(ANDROID_STL c++_static CACHE STRING "")
include("${UC_ANDROID_NDK}/build/cmake/android.toolchain.cmake")
# The project's compiler flags are set in the root CMakeLists.txt (UC_ANDROID): the NDK's hooks replace any *_FLAGS_INIT given here.
