# mimalloc (MIT): the game's global operator new/delete (game/allocator.cpp). Built from source as a static library; a pre-downloaded archive in
# build/sdk_downloads is used when present (offline builds), otherwise the release is fetched; both are pinned by SHA256.
include(FetchContent)
set(uc_mimalloc_file mimalloc-v3.6.0.zip)
set(uc_mimalloc_archive "https://github.com/microsoft/mimalloc/archive/refs/tags/v3.6.0.zip")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/sdk_downloads/${uc_mimalloc_file}")
  set(uc_mimalloc_archive "${CMAKE_SOURCE_DIR}/build/sdk_downloads/${uc_mimalloc_file}")
endif()
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)   # only operator new/delete are replaced (game/allocator.cpp), malloc stays the CRT's
FetchContent_Declare(mimalloc URL "${uc_mimalloc_archive}" URL_HASH SHA256=7397e14916ca5ac5c1666c7fb5fbf932b1ea8eeb99f229dcb358ebf71d5c35b8 DOWNLOAD_EXTRACT_TIMESTAMP FALSE EXCLUDE_FROM_ALL)   # EXCLUDE_FROM_ALL: its install rules stay out of dist
FetchContent_MakeAvailable(mimalloc)
