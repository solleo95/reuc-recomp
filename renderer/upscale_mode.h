#pragma once
namespace uc {
// Launcher "Anti-aliasing / upscaling" (config graphics.upscaler, --upscaler). blit: test-only linear blit, CLI only. smaa: SMAA 1x at the output size.
enum class UpscalerKind { off, dlss, fsr, blit, smaa };
// Launcher "Quality" (config graphics.upscale_mode, --upscale-mode); internal resolution = output / upscale_ratio(mode).
enum class UpscaleMode { native, quality, balanced, performance, ultra_performance };
// Temporal reconstructions want a sub-pixel jitter of the scene projection; SMAA works on the finished frame and wants none.
inline bool uses_jitter(UpscalerKind kind) { return kind==UpscalerKind::dlss || kind==UpscalerKind::fsr || kind==UpscalerKind::blit; }
}
