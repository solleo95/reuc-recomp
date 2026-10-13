# Vulkan descriptor pool reuse

Render currently destroys/creates a descriptor pool for every nonempty pass after waiting for its slot fence. Train uses roughly20passes per frame, so repeat allocation is a CPU cost candidate.

1. Instrument pool creations and add a GPU regression proving unchanged draw counts reuse both slots; verify RED on the current lifecycle.
2. Keep each retired slot's pool until capacity grows or resize/shutdown retires it. Reset only after its fence signals; use owner-thread checks. Preserve validation, descriptor updates and current texture ownership.
3. GPU test warm reuse, grow/shrink, empty passes and resize; Vulkan validation, full tests/build/install. Compare active audible train run preparation time/FPS, excluding death-screen values. Record measured results, including a negligible result if that is what evidence shows.

Independent build reliability issue: test_savefs temp paths keyed only by PID collide when Windows reuses PIDs. Reproduced failure at create_dir after10repeat runs; added monotonic suffix,40repeats pass. No saves/plans/files removed.
