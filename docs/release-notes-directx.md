Umbrella Chronicles native port: DirectX 11 and DirectX 12

New: choose your graphics API
- The launcher's graphics settings now offer Vulkan, DirectX 11 and DirectX 12. Vulkan stays the default. Changing the API needs a game restart. You can also use `uc.exe --renderer vulkan|dx11|dx12`.
- All three use the same game rendering and the same effects code, so the picture matches. Your saved settings are kept when you switch.

What each API supports
- All APIs: bloom, ambient occlusion, contact shadows, shadow maps, per-pixel lighting, motion blur, SMAA, sharpening, HDR10, render scale, DLSS Super Resolution / DLAA (NVIDIA RTX).
- DirectX 12 and Vulkan only: FSR, DLSS and FSR Frame Generation, path tracing, DLSS Ray Reconstruction (needs an RTX GPU and DXR 1.1). DLSS 5 Neural Rendering needs your own nvngx_dlssnr.dll and works on DirectX 12 and Vulkan.
- DirectX 11 has no ray tracing API and the supplied FSR and frame generation SDKs have no DirectX 11 backend. On DirectX 11 the launcher and the in-game menu hide these options instead of greying them out, and a saved choice of them is ignored (and kept for when you switch back).

Performance
- DirectX 11 and DirectX 12 now use specialized pixel shaders for each material, as the Vulkan renderer does. This cut GPU time per frame from about 5.2 ms to about 2.1 ms on DirectX 11 and 2.4 ms on DirectX 12 (RTX 4080, 1280x720 test window, Vulkan: 2.0 ms). Shaders compile in the background and are cached in data/dx_tev_cache.bin, so the first minutes of the first launch use a little more CPU.
- At typical settings the frame time is now close to Vulkan: about 5.8 ms on DirectX 11 and 6.1-6.3 ms on DirectX 12, against 5.9 ms on Vulkan.
- Turning DLSS, Frame Generation or Ray Reconstruction on and off no longer rebuilds the renderer on DirectX 12 (the freeze dropped from about 2.3 s to 0.1 s). Path tracing pipelines are prepared in the background at startup.
- Path tracing on DirectX 12 is still heavier than on Vulkan (about 8 ms against 5.9 ms of GPU time in our test scene).

Fixes
- The game no longer crashes when path tracing is switched on under DirectX 11.
- The launcher and the in-game menu no longer wait for the slow exit of the GPU capability check: DLSS, SMAA and the other options become available a few seconds after start instead of staying on "Checking this PC".

Known limits
- FSR creates its context on the first use, which can stall for about half a second.
- Output of DirectX 11 and DirectX 12 can differ from Vulkan in rare single pixels.
- Physical HDR10 output, and GPUs other than NVIDIA GeForce RTX 4080, were not tested.
- The first run after an update can be slower: the GPU driver and the shader cache are warming up.
