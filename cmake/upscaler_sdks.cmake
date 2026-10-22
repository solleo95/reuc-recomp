# NVIDIA Streamline (DLSS 4.5 SR, DLSS-G, Reflex) and AMD FidelityFX SDK (FSR 3.1 upscaler and frame generation, Vulkan).
# Headers only at build time; the signed runtime DLLs are copied next to uc.exe and installed into dist. A pre-downloaded archive in
# build/sdk_downloads is used when present (offline builds), otherwise the release is fetched; both are pinned by SHA256.
include(FetchContent)
macro(uc_sdk name file url hash)   # a macro: FetchContent's <name>_SOURCE_DIR must reach this scope
  set(uc_sdk_archive "${url}")
  if(EXISTS "${CMAKE_SOURCE_DIR}/build/sdk_downloads/${file}")
    set(uc_sdk_archive "${CMAKE_SOURCE_DIR}/build/sdk_downloads/${file}")
  endif()
  # SOURCE_SUBDIR points at a folder without CMakeLists.txt: the SDKs' own build scripts are not used.
  FetchContent_Declare(${name} URL "${uc_sdk_archive}" URL_HASH SHA256=${hash} DOWNLOAD_EXTRACT_TIMESTAMP FALSE SOURCE_SUBDIR uc-headers-only)
  FetchContent_MakeAvailable(${name})
endmacro()
uc_sdk(streamline streamline-sdk-v2.14.16.zip
  "https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip"
  32041E51DB0CAB709A271B72A352281B690C2ABBD669A827809401A5DD23956B)
uc_sdk(fidelityfx FidelityFX-SDK-v1.1.4.zip
  "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v1.1.4/FidelityFX-SDK-v1.1.4.zip"
  0216556BFB0E243CEC30004A2A98D38F4E3F7406CB7938E3C1B85C758E95D952)

add_library(uc_streamline_sdk INTERFACE)
target_include_directories(uc_streamline_sdk INTERFACE ${streamline_SOURCE_DIR}/include)
target_link_libraries(uc_streamline_sdk INTERFACE wintrust crypt32)
# NGX SDK of the Streamline archive: DLSS 5 Neural Rendering is an NGX feature with no Streamline plugin (renderer/ngx_dlss5.h).
add_library(uc_ngx_sdk INTERFACE)
target_include_directories(uc_ngx_sdk INTERFACE ${streamline_SOURCE_DIR}/external/ngx-sdk/include)
target_link_libraries(uc_ngx_sdk INTERFACE ${streamline_SOURCE_DIR}/external/ngx-sdk/lib/Windows_x86_64/nvsdk_ngx_d.lib)
# The DLSS 5 runtime (nvngx_dlssnr.dll, including custom unsigned builds) is not in git: drop it into project/dlss5/ or point UC_DLSS5_RUNTIME (cache variable or
# environment variable) at it. It is copied beside the build executable for development only and is never installed into dist: players supply their own.
set(UC_DLSS5_RUNTIME "" CACHE FILEPATH "path of nvngx_dlssnr.dll (DLSS 5 Neural Rendering runtime)")
if(NOT UC_DLSS5_RUNTIME AND DEFINED ENV{UC_DLSS5_RUNTIME})
  set(UC_DLSS5_RUNTIME "$ENV{UC_DLSS5_RUNTIME}")
endif()
if(NOT UC_DLSS5_RUNTIME AND EXISTS "${CMAKE_SOURCE_DIR}/project/dlss5/nvngx_dlssnr.dll")
  set(UC_DLSS5_RUNTIME "${CMAKE_SOURCE_DIR}/project/dlss5/nvngx_dlssnr.dll")
endif()
if(UC_DLSS5_RUNTIME AND EXISTS "${UC_DLSS5_RUNTIME}")
  message(STATUS "DLSS 5 runtime: ${UC_DLSS5_RUNTIME}")
  set(UC_DLSS5_RUNTIME_FILES "${UC_DLSS5_RUNTIME}" CACHE INTERNAL "")
else()
  message(STATUS "DLSS 5 runtime: not found (project/dlss5/nvngx_dlssnr.dll or UC_DLSS5_RUNTIME); the feature will report it missing")
  set(UC_DLSS5_RUNTIME_FILES "" CACHE INTERNAL "")
endif()
add_library(uc_fidelityfx_sdk INTERFACE)
target_include_directories(uc_fidelityfx_sdk INTERFACE ${fidelityfx_SOURCE_DIR}/ffx-api/include)

# Production (signed) runtime files loaded from the exe directory.
set(UC_UPSCALER_RUNTIME
  ${streamline_SOURCE_DIR}/bin/x64/sl.interposer.dll ${streamline_SOURCE_DIR}/bin/x64/sl.common.dll
  ${streamline_SOURCE_DIR}/bin/x64/sl.dlss.dll ${streamline_SOURCE_DIR}/bin/x64/sl.dlss_d.dll ${streamline_SOURCE_DIR}/bin/x64/sl.dlss_g.dll
  ${streamline_SOURCE_DIR}/bin/x64/sl.reflex.dll ${streamline_SOURCE_DIR}/bin/x64/sl.pcl.dll
  ${streamline_SOURCE_DIR}/bin/x64/nvngx_dlss.dll ${streamline_SOURCE_DIR}/bin/x64/nvngx_dlssd.dll ${streamline_SOURCE_DIR}/bin/x64/nvngx_dlssg.dll
  ${streamline_SOURCE_DIR}/bin/x64/NvLowLatencyVk.dll
  ${fidelityfx_SOURCE_DIR}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll
  CACHE INTERNAL "")
install(FILES ${streamline_SOURCE_DIR}/license.txt ${streamline_SOURCE_DIR}/bin/x64/nvngx_dlss.license.txt ${streamline_SOURCE_DIR}/bin/x64/reflex.license.txt
  DESTINATION licenses/Streamline)
install(FILES ${fidelityfx_SOURCE_DIR}/sdk/LICENSE.txt DESTINATION licenses/FidelityFX)
