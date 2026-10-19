# SMAA sources

Taken unchanged from https://github.com/iryoku/smaa at commit `71c806a838bdd7d517df19192a20f0c61b3ca29d` (MIT licence, `LICENSE.txt`):

| File | Upstream path | SHA256 |
|---|---|---|
| `SMAA.hlsl` | `SMAA.hlsl` | `4db53d92ef0b45661f8450303589ffabce93c352cae4282a6f32dd2b80efbeb9` |
| `AreaTex.h` | `Textures/AreaTex.h` | `1933bf43ce86ba71f7ed5f0fa94b148610c634dc3395b9bc67a817d51ee73af2` |
| `SearchTex.h` | `Textures/SearchTex.h` | `1fac315ab5f87b60083c9b8387af17f056204d3d1bf21500f2acc5b3e8c1a37a` |
| `LICENSE.txt` | `LICENSE.txt` | `2b848b67ba50ac40b53e11cf1ccdaee2a190ca83b4652cba695a5f1ce81f52ae` |

The renderer includes them through `renderer/shaders/smaa.slang` (compute entry points) and `renderer/smaa_upscaler.cpp` (lookup textures).
