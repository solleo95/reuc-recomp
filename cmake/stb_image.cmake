# stb_image (one header) decodes the PNGs of the launcher's artwork on Linux and the compile screen's picture on both systems (the Windows launcher itself uses WIC);
# pinned like the other dependencies. Sets stb_image_SOURCE_DIR in the including scope; safe to include from several folders.
set(stb_image_url "https://raw.githubusercontent.com/nothings/stb/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5/stb_image.h")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/deps/stb_image-2.30.h")
  set(stb_image_url "${CMAKE_SOURCE_DIR}/build/deps/stb_image-2.30.h")
endif()
include(FetchContent)
FetchContent_Declare(stb_image URL "${stb_image_url}" URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
  DOWNLOAD_NO_EXTRACT TRUE DOWNLOAD_NAME stb_image.h)
FetchContent_MakeAvailable(stb_image)
