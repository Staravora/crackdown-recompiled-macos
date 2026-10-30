# Lasting project decisions

| Date | Decision | Rationale | Status |
| --- | --- | --- | --- |
| 2026-10-29 | Keep an isolated local macOS branch against the original GitHub upstream. | Prioritize personal play and reviewable patches before deciding where to publish or contribute. | active |
| 2026-10-29 | Use native ARM64 ReXGlue v0.10.0 and pinned MoltenVK binaries with the game's full SDK patch set. | SDK already supports Mac; unpatched release runtime lacks required game behavior. | active |
| 2026-10-29 | Register guest functions through the existing mapping table. | Equivalent 41,704 entries avoid pathological AppleClang compile time for repeated virtual registration calls. | active |
| 2026-10-29 | Keep game assets and saves in Crackdown Data beside the real app bundle. | App updates need not duplicate the disc or replace player data; bundle and data can move together. | active |
| 2026-10-29 | Set game aspect once at launch and preserve presenter letterboxing. | Cooperates with Aerospace tiling without continually forcing window dimensions. | active |
| 2026-10-29 | Retain polling fallback until native address waits are isolated and visually verified. | A multi-change trial produced black output; kernel wait microtests alone did not establish game correctness. | active |
| 2026-10-29 | Treat mouse/keyboard and trackpad as supported control targets. | User requires conventional PC input and wants practical Mac trackpad alternatives. | active |
| 2026-10-29 | Require repeatable gameplay timing and image correctness before promoting GPU experiments to defaults. | Smaller declared render areas and passing logic tests did not establish useful real-world performance. | active |
| 2026-10-30 | Count render-pass break reasons before redesigning queries or uploads. | Disabling queries removed ~120 of ~330 breaks for ~1 FPS; uploads are the largest single cause. | active |
| 2026-10-30 | Keep the pixel-rate alpha-to-coverage fix behind a default-off flag. | It is correct and removes per-sample shading on MoltenVK but showed no measured FPS gain. | active |
| 2026-10-30 | Keep upload hoisting behind a default-off flag and exclude texture-load uploads. | Correct image and ~95 fewer pass breaks/frame, but only ~0.5 FPS; texture loads rely on the upload ending the pass. | active |
| 2026-10-30 | Stop optimizing pass count; investigate GPU dependency stalls next. | Identical passes vary 3x in duration and counters show a busy GPU at low utilization. | active |
| 2026-10-30 | Benchmark renderer changes on the menu, unattended, before gameplay. | Stable ~0.2 FPS spread, needs no player time, and the menu reproduces the gameplay pixel bottleneck. | active |
| 2026-10-30 | Enable shader math contraction (pixel and vertex, invariant position) in the Mac app config. | Removes SPIRV-Cross optnone helper calls; menu 3x and gameplay ~2x faster with correct rendering in garage and city. | active |
| 2026-10-30 | Use the offline MSL harness to evaluate shader code generation before game runs. | Seconds per experiment and no player time; in-game FPS can hide changes below occupancy cliffs. | active |
| 2026-10-30 | Seek storage files to the start after "a+b" open. | BSD libc positions a+ streams at EOF, which silently wiped the shader cache every launch on macOS. | active |
