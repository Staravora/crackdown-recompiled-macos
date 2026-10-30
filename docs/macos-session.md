# Native macOS port: session details

Updated 2026-10-29. Implementation and live testing are paused at Bruce's request. The latest request authorizes writing this handoff, not resuming rendering experiments.

## Objective and user preferences

Make Crackdown playable on this M1 Mac; consider contributing useful changes upstream later. Conventional mouse/keyboard controls are essential, with practical trackpad support too. Contextual on-foot/driving/menu bindings remain pending. Aerospace tiling must preserve the picture's shape; the user confirmed this now works. Personal Mac compatibility credits can name Zuhl Productions while retaining the upstream game's identity.

This is an explicit GitHub-upstream exception to Bruce's usual GitLab workflow. Do not create a public fork, push, open an issue/PR, or contact maintainers without a new instruction. No such actions have occurred. No project commits were made. Existing source changes are intentional session work, including the many SDK patch changes; do not reset them.

## Location map

Paths below are relative to the Codex workspace root, two directories above this document's repository root.

| Purpose | Workspace path |
| --- | --- |
| Game source, branch `macos-arm64` | `work/crackdown` |
| SDK source, branch `crackdown-macos-arm64` | `work/rexglue-sdk` |
| Installed patched ARM64 SDK | `work/rexglue-mac/mac-arm64` |
| SDK build | `work/sdk-build-macos` |
| Game build and generated executable | `work/game-build-macos` |
| Current manually assembled test app | `work/Crackdown Recompiled.app` |
| Extracted disc files | `work/Crackdown Data/game` |
| Saves, preferences and caches | `work/Crackdown Data/user_data` |
| Runtime logs | `work/Crackdown Data/logs/crackdown_001.log` through `005.log` |
| Research report | `outputs/rendering-optimization-research.html` |
| Archived native-wait prototype | `work/experiments/native-gpu-waits` |
| Pre-bounds-prototype libraries | `work/experiments/render-area/previous` |

`work/crackdown/game/assets` is an absolute symlink to the extracted game directory. The game's SDK folder also links to the installed SDK above. Update links if moving data; avoid duplicating the approximately 6.55 GB disc files. Preserve player data and `user_data/cache/shaders-before-black-screen-restore`.

Original ISO: `~/Downloads/Crackdown (World) (En,Ja,Fr,De,Es,It,Zh,Ko,Cs,Hu).iso`. All 196 required files were verified. Required Default.xex SHA-256: `7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a`.

## Pinned sources and completed compatibility work

- Game baseline: `33ab1b632e38cb5c1c39a3d56c479537b9e0d185`.
- SDK v0.10.0 baseline: `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`.
- Launcher submodule: `b384a28c712c9320eea8f9d50639dec49981b6c1`.
- Apply all 69 common/non-Windows game SDK patches, including the Linux Vulkan/POSIX ones, then `patches/rexglue-sdk-macos/*.patch`.
- Mac patches: reuse verified prebuilt Vulkan libraries; table-based guest registration; optional experimental bounded render areas. Source and installed SDK have matching patch stamps.
- The SDK already has native ARM64/MoltenVK support. The installed driver reports MoltenVK 1.4.1. A Metal backend rewrite has not been attempted.

Game changes cover Apple SDK selection/verification, Mach-O weak tail thunks, macOS process/executable paths and restart arguments, system Arial lookup, data beside the app, launch-time SDL aspect constraints with letterboxing, FPS diagnostics, and native test/build support. Native GPU address waits were reverted; macOS currently uses original guest polling.

The generated code has 41,704 functions in 177 recompilation chunks. AppleClang stalled on repeated virtual registration calls; the table-based registration patch emits the same existing function mappings in a loop. A separate registration-source no-PCH/O0 workaround remains in game CMake and may be redundant; changing broad compiler settings can rebuild every chunk.

## Build reference — only after implementation resumes

Host: M1, 16 GB RAM, macOS 26.6.2, Xcode AppleClang 17, CMake 4.4, Homebrew Python 3.15. Use Unix Makefiles and four jobs; Ninja is not installed. Current binaries target macOS 26.0. Older macOS support is unverified.

Run from the Codex workspace root to reuse this checkout:

```sh
export CRACKDOWN_MAC_SDK_SOURCE="$PWD/work/rexglue-sdk"
export CRACKDOWN_MAC_SDK_BUILD="$PWD/work/sdk-build-macos"
export CRACKDOWN_MAC_BUILD="$PWD/work/game-build-macos"
export CRACKDOWN_SDK_DIR="$PWD/work/rexglue-mac/mac-arm64"
CRACKDOWN_BUILD_JOBS=4 bash work/crackdown/tools/build-macos.sh
```

The bootstrap builds the patched SDK/code generator, overlays SDK headers, verifies artifacts, runs native codegen, and builds the game. It does not yet produce the finished app bundle. It deliberately refuses a changed patch fingerprint against an already patched source; resolve source/patch consistency instead of resetting dirty source or bypassing verification. Codegen needs an unsandboxed 4 GB guest mapping; restricted execution failed previously.

Useful existing-build checks, also from workspace root:

```sh
python3 work/crackdown/tools/check-macos-sdk.py --sdk work/rexglue-mac/mac-arm64
python3 work/crackdown/tools/check-macos-sdk.py --sdk work/rexglue-mac/mac-arm64 --staged 'work/Crackdown Recompiled.app/Contents/MacOS'
cmake --build work/game-build-macos --parallel 4
```

Do not restore the SDK libraries marked `.stock`: those are the original unpatched release libraries, not the working game-patched runtime. The pre-bounds patched libraries are in `work/experiments/render-area/previous`.

Never replace a live executable or dylib. The application bundle and SDK installation are separate staging locations. After an SDK rebuild, copy matching libraries/headers into the installed SDK, use the checker's `--record-source` option, and stage the appropriate libraries to the closed app. The checker verifies hashes, headers and the selected patches; do not just edit its record to make a mismatch pass.

## Gameplay state and experiments

Native 1280×720 at 100% internal scale renders correctly at about 6 FPS in the Agency. User screenshots verify actual gameplay and aspect shape. Menus may run near 30 FPS; that is not gameplay evidence. The frame counter counts guest submissions, not independently confirmed screen presentation.

| Comparison | Recent median FPS | Caveat |
| --- | --- | --- |
| Original visible gameplay | 5.79 | Native 1280×720 |
| Async MoltenVK submissions | 6.24 | No useful gain |
| 50% internal scale | 9.00 | 640×360, visibly lower quality |
| 50% plus bounded scissors | 10.95 | User still reports 6–11 FPS |
| 50% plus triangle viewport bounds | 9.71 | User screenshot 9.88 FPS |

These were not fixed-camera trials. Do not infer a reliable performance improvement from the different medians.

The last two trials use `vulkan_trim_render_areas=true`; default is false. Patch 0003 adds a deferred-command prepass that unions draw scissors, triangle viewport bounds, and diagnostics. Latest diagnostics confirmed it active and reported about 46.4% of original declared area. This is rectangle area, not hardware bandwidth. New bounds tests cover 500 randomized pixel-coverage cases plus persistent/expanding/unknown scissor and clear/resolve fallback logic. Broad GPU correctness testing is outstanding.

Async submission was a process-only `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` override. Reduced rendering and bounds were process arguments, not defaults. Normal launch configuration remains native internal resolution, 30 FPS cap, mouse mode, skip intro, letterboxing and diagnostic FPS logs.

An earlier native-address-wait experiment produced black output with choppy audio after several simultaneous changes. Kernel wait microtests passed, but that did not prove game correctness. The cause was not isolated; do not re-enable the archived prototype assuming it was validated or assuming it alone caused the black screen.

## Evidence and prioritized research

Read `outputs/rendering-optimization-research.html` first. The current CPU profile places the render thread mostly in GPU completion waits; hardware utilization was around 90%. The earlier drawable/queue-lock profile was not sufficient to diagnose the cause: async submission moved the waiting without fixing FPS.

The Metal trace `work/gameplay-half-metal.trace` is about 128 MB. Parsed data are in `metal-gpu-intervals.xml`, `metal-encoders.xml`, `metal-gpu-summary.json`, and `metal-encoder-summary.json`. It includes 13,895 dynamic-render encoders, 4,310 query-accumulation encoders, many uploads, and query reset/copy work. Vertex/fragment intervals can describe the same encoder and overlap; compute-channel intervals include blits. Do not sum all durations into a frame budget or equate each interval with a separate render pass.

The SDK's `BeginGuestOcclusionQuery()` and `EndGuestOcclusionQuery()` in `src/graphics/vulkan/command_processor.cpp` explicitly end rendering, reset/begin queries outside passes, then end/copy results immediately. This comes from existing game patches 0038/0053. Investigate whether query lifecycle forces avoidable pass boundaries; this is a hypothesis, not a confirmed diagnosis.

- MoltenVK PR https://github.com/KhronosGroup/MoltenVK/pull/2867 defers accumulation but explicitly retains immediate accumulation when queries end outside passes. Our current query/copy pattern limits its applicability; it is not a drop-in fix.
- Issue https://github.com/KhronosGroup/MoltenVK/issues/2864 describes a reset/reuse ordering bug in another setup. Include the scenario in tests; do not label it a confirmed local defect.
- Vulkan queries must begin/end in the same subpass or both outside passes. Resets/result copies are outside-pass commands. Removing end-pass calls without redesigning query scope is invalid.
- Existing `render_target_path_vulkan=fsi` is a secondary architectural comparison. Logs advertise interlock, but all capability checks must pass. FSI rejects reduced internal scaling: compare both paths at 100% and validate selected path and images.

On resume: establish a fixed Agency camera/position and warm-up procedure; capture a gameplay Metal frame with hardware counters; count why each pass ends; then design reset/copy batching and valid query scopes while preserving guest result availability, MSAA normalization and pool-reuse ordering. Only then choose further renderer changes. Inspect shaders if counters identify geometry/ALU/texture limits. The pinned MoltenVK supports shader dumps and on-demand named-pipe capture, so no first-launcher-frame capture is necessary.

## Tests, packaging and remaining work

The original Mac test baseline passed 52/53 suites. `packaging_tests` had two pre-existing Windows script string assertions fail. The two native process checks and new render-area test passed separately. The complete current suite has not been rerun; do not claim it all passes.

After resuming, from workspace root:

```sh
CRACKDOWN_TEST_BUILD="$PWD/work/tests-macos" CRACKDOWN_SDK_DIR="$PWD/work/rexglue-mac/mac-arm64" bash work/crackdown/tools/test.sh
```

App packaging is still manual. Put non-code folders/config in Contents/Resources, with compatibility symlinks in Contents/MacOS. A staged app signed and passed `codesign --verify --deep --strict` under `/private/tmp`; its temporary location is recorded in `work/verified-bundle-path.txt` and may be cleaned by the OS. Under Documents, FileProvider re-adds root FinderInfo and strict verification then fails. This does not establish binary corruption. The current test bundle was not replaced by the cleaned packaging trial. A reproducible bundle-packaging script and final installed app remain undone.

GUI automation's skill files were missing after a tool update during this session. User navigated the game manually; CLI profiling worked. Re-discover current tools if needed, rather than assuming the old desktop API is available.

Secret guard ignore rules and a local gitleaks pre-commit hook were added. Earlier scan findings were known false positives (a key-code expression and public signing-key fingerprints). No public release or upstream-ready claim is appropriate yet. Keep platform compatibility changes distinct from optional GPU experiments when preparing a future contribution.
