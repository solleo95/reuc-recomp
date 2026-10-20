# Third-party components

Their licence texts are in the `licenses` folder next to the game.

| Component | Used for | Licence file |
|---|---|---|
| [NVIDIA Streamline](https://github.com/NVIDIAGameWorks/Streamline) 2.14.16 (`sl.*.dll`, `NvLowLatencyVk.dll`) | DLSS, DLSS Frame Generation, Reflex | `licenses/Streamline/license.txt` |
| [NVIDIA DLSS](https://github.com/NVIDIA/DLSS) runtime (`nvngx_dlss.dll`, `nvngx_dlssg.dll`, `nvngx_dlssd.dll`) | DLSS Super Resolution, Frame Generation and Ray Reconstruction | `licenses/Streamline/nvngx_dlss.license.txt` |
| NVIDIA NGX SDK (`nvsdk_ngx_d.lib`, from the Streamline archive) and the optional DLSS 5 Neural Rendering runtime (`nvngx_dlssnr.dll`, not part of the repository: `docs/superpowers/specs/2026-10-20-dlss5-neural-rendering-design.md`) | DLSS 5 Neural Rendering | `licenses/Streamline/license.txt` |
| NVIDIA Reflex (`NvLowLatencyVk.dll`) | low latency with frame generation | `licenses/Streamline/reflex.license.txt` |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 1.1.4 (`amd_fidelityfx_vk.dll`) | FSR 3.1 upscaling and frame generation | `licenses/FidelityFX/LICENSE.txt` |
| [SMAA](https://github.com/iryoku/smaa) (commit 71c806a, `third_party/smaa/SOURCE.md` lists the files and hashes) | SMAA 1x anti-aliasing | `licenses/SMAA/LICENSE.txt` |
| [SDL3](https://github.com/libsdl-org/SDL) | window, input | `licenses/SDL3/LICENSE.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) | launcher | `licenses/imgui/LICENSE.txt` |

NVIDIA, GeForce RTX, DLSS and Reflex are trademarks and/or registered trademarks of NVIDIA Corporation. AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc.
