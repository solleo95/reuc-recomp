# Native internal resolution

Logical SDK EFB coordinates remain640x480. GPU color/depth attachments now use640*scale by480*scale; GPU-only immutable copy versions use their logical base extent multiplied by the same scale. CPU asset texels stay unchanged. Display cropping640x360 at6x produces3840x2160 scene pixels.

`--render-scale auto|1..6` controls scale. Default auto chooses the larger integer ceiling of output width/640 and height/480, bounded1..6:1280x720=2,1920x1080=3,2560x1440=4,3840x2160=6. Renderer rebuild retires GPU work before replacing color/depth images; retained copy versions remain valid. Free-copy allocation cache resets on a scale change and otherwise remains bounded32images,64*scale*scale MiB capped512MiB.

CPU parser/scale edge tests pass; full build64/64 tests pass. GPU RED: missing scale API, then fixture corrected to capture EFB before post-copy clear. GREEN:1→2→6→1, actual readback1280x960/3840x2880, invalid scale rejection, old copy ownership across change, clean Vulkan validation/shutdown. An eight-stripe pixel fixture proves a logical four-texel copy retains eight physical texels at2x rather than collapsing to four and merely enlarging presentation.

Visible audible HD run PID52032, log `build/autotest/1791909484512997800/run.log`: train scene and characters render; active intervals about28–30 presentations/sec and105–115ms total CPU render preparation/sec. Capture `S:/Games/Umbrella Chronicles/debug/frame-1791909553292336-52032-33465.png`. Audio output remains enabled;13 loading overruns stopped increasing during sampled gameplay. Later death-screen60FPS is excluded from active gameplay acceptance.

4K verification completed in PID39700, log `build/autotest/1791909993085755300/run.log`: native EFB3840x2880 and output3840x2160, recognizable train/HUD/characters, approximately29–31 active presentations/sec and105–115ms CPU preparation/sec. GPU scale now caps by device maxImageDimension/1024 to cover every valid logical copy;4096px-limit test caps6→4. RTX4080 retains6x. Scoped review correction passed;64/64 CPU tests and Vulkan validation pass.

The first4Kprofile captured24intermediate EFB PNGs automatically and accumulated90audio overruns. Ordinary `--profile` now measures performance without those readbacks; explicit `--capture-scene-passes` retains the old bounded material-debug capture. Route screenshots reduced to three milestones plus final capture. New150-second4Krun has zero audio overruns (maximum11164queued bytes),49966native audio blocks,15052/15052submitted/completed graphics batches and clean shutdown. Original100msqueue-recovery policy is still debt; this run establishes that diagnostic capture stalls caused the observed bursts, not general audio-clock correctness.

Exact copy filtering/LOD, STQ/indirect/fog, regional logical EFB bounds and arbitrary simulation FPS remain separate fidelity work. No interpreter, device model, OS input or new dependency.
