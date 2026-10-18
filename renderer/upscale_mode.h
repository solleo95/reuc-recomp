#pragma once
namespace uc {
// Launcher "Upscaling" (config graphics.upscaler, --upscaler). blit: test-only linear blit, CLI only.
enum class UpscalerKind { off, dlss, fsr, blit };
// Launcher "Quality" (config graphics.upscale_mode, --upscale-mode); internal resolution = output / upscale_ratio(mode).
enum class UpscaleMode { native, quality, balanced, performance, ultra_performance };
}
