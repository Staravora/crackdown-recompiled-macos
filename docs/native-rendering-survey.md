# Native rendering survey (level 3)

Scoping survey for replacing the emulated Xenos GPU with native Metal rendering: hook the game's Xbox Direct3D calls and reimplement them, with shaders translated ahead of time, as Unleashed Recompiled did. Written 2026-10-30. No game or benchmark was run for it; everything comes from the generated sources, the shipped game files, the shader caches, the SDK and public repositories. Nothing here has been built or tried.

## Bottom line

Level 3 looks feasible for Crackdown, and in several ways easier than Sonic Unleashed was.

- **The D3D boundary is nearly clean.** No game function outside the D3D block writes command packets itself, but 41% of city draws arrive in baked command buffers that need a small decoder ([Census results](#census-results-2026-10-31)). About 50-60 entry points need replacing; Unleashed Recompiled needed 42 hooks and 15 stubs.
- **The shaders are in hand.** The game ships them in the container format XenosRecomp reads, with constant names intact. 254 of the 262 shaders seen at runtime (97%) trace back to those files.
- **The rendering is simple.** No memexport anywhere, no multiple render targets, one colour format on 367 of 371 colour-target pipelines, and shaders with a median of 47 (VS) and 30 (PS) instructions.
- **A Metal path exists to build on.** Unleashed Recompiled has an open native Metal PR, with XenosRecomp MSL output in a companion PR. Neither is merged.

Three things are harder than Unleashed and carry most of the risk: textures and vertex data that live raw in streamed guest memory with no creation call to hook, predicated tiling (a large win to remove, but it must be removed correctly), and CPU reads of resolved targets.

**Recommended first milestone:** a D3D call census plus an offline shader trial (see [Plan](#5-phased-plan)). It costs a few sessions, needs one short game run, gives the emulator work real per-frame targets, and decides go or no-go on level 3 with numbers. Skip a full native Metal backend for the emulator (level 2) unless the census finds a blocker.

**Confidence.** Function names below are inferred from code shape, callers and the XDK API as remembered; there are no symbols. Counts are measured. What only a live run can answer is listed under [Open questions](#open-questions).

## Census results (2026-10-31)

Phase 0 ran on the live game: `--crackdown_d3d_census` during a benchmark, at the Los Muertos supply point and in the Agency garage. Read reports with `tools/census-lists.py`. Median per frame:

| Per frame | City | Garage |
| --- | --- | --- |
| Draws made directly | 2,763 | 784 |
| Precompiled command buffer runs | 813 | 918 |
| Draws inside those buffers | 1,902 | 1,579 |
| **Unique draws** | **4,665** | **2,363** |
| Direct draws inside tiling | 2,221 | 217 |
| Tiles (always the same two rects) | 2 | 2 |
| Resolves | 23 | 41 |
| Texture changes | 1,476 | 600 |
| Occlusion queries | 71 (max 270) | 33 (max 214) |
| Texture locks | 1 | 1 |

A second city spot (second Los Muertos supply point, a rooftop) came to almost the same total by a different route: **4,689 unique draws**, 1,669 direct and 3,020 inside 1,160 buffer runs. So about 4,700 looks like a fair figure for standing in the city, while the share in baked buffers ranges from 41% to 64%.

**This corrects the survey in four ways.**

- **The boundary is not fully clean.** `826C3750` is `RunCommandBuffer`, used about 800 times per frame, and those buffers hold raw PM4 that no D3D hook sees. They look baked into the game data rather than recorded: `826C4148` turns offsets in the object into pointers at load, and `826C3D90` patches in resources, both called from the loader `823BD140`. Hooking D3D alone would miss 41% of city draws, so **a native layer needs a PM4 decoder for them.**
- **That decoder is small.** Across 598 buffers (2 MB) in the city and 633 in the garage there are exactly 7 packet types (`DRAW_INDX`, `SET_CONSTANT`, `LOAD_ALU_CONSTANT`, `IM_LOAD`, `WAIT_REG_MEM`, `SET_BIN_MASK_LO`, `SET_BIN_MASK_HI`) plus register writes. None nests another list, none is truncated, none has an unknown packet, and none changed during the window. 64-71% of runs hold one draw. Each run has one `WAIT_REG_MEM`, whose purpose is not yet known.
- **The draw names were wrong.** `826B91E0` and `826B96D0` are not indexed draws: they are `BeginVertices` and `BeginIndexedVertices`, which return ring space for the caller to fill (`826B9688`, `DrawPrimitiveUP`, is a wrapper). `826B9FF8(prim, base, start, count)` is the indexed draw and `826B9C08` the non-indexed one.
- **Tile replays explain less than hoped.** The 6,207 draws seen by the emulator are about 4,665 unique ones plus replays, so native rendering cuts draws by roughly a quarter, not by half. The CPU budget is therefore tighter than the bottom line above implied: about 4,700 draws, which leaves about 7 microseconds each on one thread at 33 ms if nothing else ran.

**Open questions answered.**

1. **Tiles:** always two, `0,0,1280,484` and `0,480,1280,720`. Almost all tiled draws use the full mask, so each replays in both.
2. **CPU reads of resolves:** only the `k_8_8_8_8` 320x180 targets were locked, three of four, once per frame. That is about 230 KB, under the existing `readback_resolve_max_bytes`. A lock is only evidence of a read: a direct read would not show. All but one of the 18 city resolve shapes are bound by the GPU.
3. **Textures:** 189 of 207 in the city and 68 of 95 in the garage never passed through `CreateTexture` or a resolve, so the texture cache keyed by address has to stay. DXT1 and DXT4/5 dominate.
4. **`826C3750`:** frequent, static, raw PM4, as above.

**Still open:** whether `826C4C70` is a reliable dirty signal. It runs 77-90 times per frame for about 292 KB, but a benchmark that stands still streams too little to tell. Draws come from two threads, about 96% and 4%, so a native layer must expect both. The census only samples standing still, so driving through the city may be heavier.

**Verdict: go, with the plan changed.** Nothing found is a blocker. Phase 2 gains the buffer decoder, and the first thing to prove is how fast about 4,700 draws can be encoded on this machine.

## Encoding test (2026-10-31)

`tools/encode_bench.mm` records a census-shaped frame in Metal on this M1 Air, standalone: 4,700 draws, 24 passes, 391 pipelines, 200 textures, 4 KB of vertex and 1 KB of fragment constants copied per draw, paced at 30 FPS. Pipeline, texture, depth and scissor changes the census did not count are set high on purpose (50%, 60%, 15%, 10% of draws). "Load" is flat-out threads standing in for the game, on a 4 performance-core chip. Three interleaved rounds of 150 frames, CPU milliseconds per frame:

| Case | Load 2, p50 | Load 3, p50 | Load 3, p95 | Load 3, max |
| --- | --- | --- | --- | --- |
| Bound textures, 1 thread | 6.30 | 7.81 | 10.58 | 11.41 |
| Bindless, 1 thread | 4.32 | 6.54 | 9.26 | 11.04 |
| Bindless, 2 threads | 3.68 | 5.01 | 6.96 | 7.91 |
| Bindless, 3 threads | 3.30 | 5.67 | 6.86 | 9.31 |
| Bindless, baked buffers replayed | 1.92 | 2.56 | 4.17 | 5.90 |

The stress run uses the emulator's raw 6,207 draws with every state changing on every draw. Worst case there was bound textures on one thread under load 3: 17.27 ms p50, 20.26 ms max. Bindless on one thread under the same load took 12.62 ms p50, 14.74 ms max.

**Result: Metal recording fits, comfortably, on one thread.** It costs about 1.4 microseconds per draw against the 7 microsecond budget, which leaves roughly four fifths of it for everything else a real layer does per draw. The risk named above is retired for recording itself. Threads help less than bindless does, so Phase 2 can start single-threaded with bindless textures.

- **Not measured:** the layer's own work per draw: reading device state, decoding baked buffers, texture and pipeline lookups, dirty tracking and vertex handling. That is now the real budget to watch.
- **GPU time says nothing here.** The shaders and textures are placeholders.
- **Baked replay is a lead, not a result.** Indirect command buffers can't bind textures or samplers, so they need bindless. Replay also assumes those draws need nothing that changes per frame. Whether baked draws depend on registers set by the direct calls around them is unknown, and it would need checking before relying on it.
- **Idle reads slower than load 2** (6.56 against 4.32 ms for bindless on one thread), probably because cores clock down between paced frames. The loaded figures are closer to the game, but that explanation is untested.
- **Pipelines were already compiled**, as the plan assumes, and the frame comes from a standing-still census. The stress run covers a heavier frame.

## Baked buffer decoder (2026-10-31)

Phase 2's first piece is `src/native/baked_buffer.cpp`, with `tools/baked_bench.cpp` running it on the command lists the census saved. No game code runs, and the decoder isn't in the game build yet.

| Scene | Lists | Draws per frame | Pipelines | Texture sets | Decode all once | Replay per draw |
| --- | --- | --- | --- | --- | --- | --- |
| Los Muertos | 598 | 1,902 | 47 | 949 | 3.2 ms | 0.057 microseconds |
| Rooftop | 1,054 | 3,020 | 75 | 1,534 | 2.8 ms | 0.036 microseconds |
| Agency garage | 633 | 1,579 | 39 | 668 | 2.1 ms | 0.040 microseconds |

No unknown or truncated packets in any scene, and draw totals match `census-lists.py` exactly. Replay here covers the pass check, the list's float constants (97-115 dwords per draw), redundant-state checks and a byte-swapped copy of the literals, on a shuffled frame. It doesn't include Metal encoding or anything from the device.

**What the lists turned out to be.** Each draw carries both shaders, all 32 fetch constants, render state, bools and loops, plus many of its float constants. So a list needs decoding only once, and only 4 dynamic states appear.

- **`WAIT_REG_MEM`** is a wait on `COHER_STATUS_HOST`, a cache flush handshake. Native rendering doesn't need it.
- **`SET_CONSTANT`** in these lists only moves the tile's window offset, which collapses with tiling.
- **`LOAD_ALU_CONSTANT`** only loads shader literals, at registers 252-255 of each stage, and at 236-239 (VS) in some lists.
- **Float constants:** type 0 packets write VS registers 12-27 and 248-251, and PS registers 0-19, 64-67 and 248-255 (rooftop). No list writes VS registers 0-11, so those, probably including the view and projection, have to come from the device.
- **Bin masks:** the low 32 bits are tiles. The high bits pick a pass, and many objects come as a pair of buffers (flags `0x6801` and `0x6841`) for it. The select is `RunCommandBuffer`'s third argument, which it writes straight into `SET_BIN_SELECT_HI`, so the native layer gets it on every run. It's nearly always `0xFFFFFFFF`, with `0x1` and `0x2` on a few hundred runs a frame at most.

**Live in the game.** `--crackdown_native_probe=true` runs the same decoder from the `RunCommandBuffer` hook during a benchmark's recording window, drawing nothing, and writes `native-probe-<label>.json`. It copies each list out of validated memory and never follows a pointer inside one, which is what froze the first live parser.

| Per frame | Rooftop | Los Muertos |
| --- | --- | --- |
| Buffer runs | 1,115 | 822 |
| Baked draws that pass | 2,843 | 1,879 |
| Baked draws skipped by pass | 36 | 26 |
| Replay | 0.20 ms | 0.11 ms |
| Device constants, VS 0-11 only | 0.02 ms | 0.02 ms |
| Device constants, all 512 on every run | 0.26 ms | 0.19 ms |

Decoding everything took 3-5 ms in total, spread over first sights. No unknown or truncated packets, nothing unreadable, and no buffer changed. The worst single run was 43-57 us, with one 976 us outlier that looks like a preemption but wasn't investigated.

So even the wasteful option costs under half a millisecond per frame for baked draws. That closes the per-draw question for baked draws: replay at about 0.07 microseconds plus Metal's 1.4 against a 7 microsecond budget. Direct draws through the Direct3D setters aren't measured yet.

**No cost when off.** Interleaved on the rooftop, off against on: 24.44 and 23.89 FPS against 24.25 and 24.89. Off, the hook adds one atomic load.

**Direct Metal, not plume.** One game on one API doesn't need the abstraction, `tools/encode_bench.mm` already shows direct Metal is fast enough, and the design needs bindless textures and tight control of encoding.

## Texture change test (2026-10-31)

Phase 2's gate: can a native renderer learn cheaply when a texture changes? `--crackdown_texture_probe=true` tracks every texture the game binds, from baked and direct draws, and compares four signals with a rotating hash of the real memory: the cache flush `826C4C70`, CPU locks, resolves by page, and a per-frame sample of 8 spots of 64 bytes across each bound texture. It runs through normal play and writes `texture-probe-<time>.json` to the logs folder every 900 frames.

**Standing still only**, from the menu to the rooftop, 2,700 frames:

| Per frame | |
| --- | --- |
| Textures bound | about 200-220, 44-47 MB |
| Tracking flushes | 0.001 ms |
| Tracking binds | 0.16-0.18 ms |
| Sampling every bound texture | 0.10 ms |
| Fully hashing bound textures, by comparison | about 10 ms, at 4.6 GB/s |

| 42,893 checks of the real memory | Changes |
| --- | --- |
| Announced by a flush | 250 |
| By a CPU lock | 86 |
| By a resolve | 91 |
| Only the sample saw it | 1,092 |
| Nothing saw it | 208 (189 linear, 19 tiled) |

So signals caught 88% of 1,727 changes. Most changes are linear `k_8` planes in 1280x720 and 640x360 groups, which look like video frames, rewritten without a flush. That's inferred from their shape and not confirmed.

**Driving**, Bruce's 10-minute session in the same app with the setting on (`crackdown_242.log`, 18,000 frames, 8,273 textures, 524 MB of them):

| Per frame | |
| --- | --- |
| Textures bound | about 430, 75 MB |
| Tracking flushes | 0.003 ms |
| Tracking binds | 0.45 ms |
| Sampling every bound texture | 0.23 ms |
| Fully hashing bound textures, by comparison | about 17 ms, at 4.33 GB/s |

| 540,178 checks of the real memory | Changes | Share |
| --- | --- | --- |
| Announced by a flush | 3,909 | 52% |
| By a CPU lock | 79 | 1% |
| By a resolve | 1,368 | 18% |
| Only the sample saw it | 1,201 | 16% |
| Nothing saw it | 1,000 (702 tiled, 298 linear) | 13% |

Misses are 6.5% of changed bytes. 191 of them are in the first 30 seconds, and the other 809 run through the whole drive at about 1.4 a second, so this isn't a loading effect. The textures listed are nearly all tiled DXT at 8-64 KB in a few address runs, each missed 3-5 times, which looks like a streaming pool reusing slots. That's inferred from the addresses. If so, a miss would show the wrong texture and not just an old one.

The frame rate with the probe on was median 29.8 FPS over 650 seconds, with 0 seconds under 12 and 28 under 20. That isn't a controlled comparison.

**Verdict: no-go for these signals as the way a native renderer invalidates textures.** Cost is fine at about 0.7 ms a frame, but one change in eight goes unseen. Hashing everything is out at 17 ms a frame. More samples or hashing a texture's first binds could shrink the misses but can't prove there are none.

**Next candidate: the emulator's own page watch.** Its texture cache doesn't use these signals. `texture/cache.cpp` calls `SharedMemory::WatchMemoryRange`, which arms page protection through `Memory::EnablePhysicalMemoryAccessCallbacks`, so the first write to a watched page faults and notifies. File reads into watched memory go through the physical view and trigger the same callbacks by hand in `XFile::ReadInternal`. `RegisterPhysicalMemoryInvalidationCallback` is public and takes several listeners, so the probe listens without an SDK patch.

The probe arms each texture just before hashing it, as a renderer would arm before reading, so a change that fired nothing is a real gap and not a timing artefact. Its listener returns the exact fired range. Otherwise the emulator's listener widens the unwatch to as much as 256 KB around the write, and the pages around it lose their watch without telling the probe. The first run left that in and showed 186 unwatched changes against 98 after this fix.

**Unattended benchmark only**, 1,800 frames from the menu to the rooftop, not moving:

| 1,369 changes | |
| --- | --- |
| The watch fired | 1,271 (93%) |
| Not fired, but a resolve wrote the page | 86 (6%) |
| Neither | 12 (1%, 1.6 MB) |
| Of the 301 no cheap signal saw, the watch fired on | 295 |

The remaining 12 are mostly small linear textures, and nothing yet says what writes them. A resolve's readback writes through the physical view, which is why those don't fire, and a native renderer would know about its own resolves. False alarms are 761 checks, or 389 MB, which is the price of 16 KB host pages over 4 KB guest pages: a texture gets re-read without having changed.

| Benchmark, interleaved | Average FPS | Median frame |
| --- | --- | --- |
| Probe off | 25.73, 24.92 | 39.0, 39.6 ms |
| Probe on, watch armed | 23.49, 23.24 | 42.1, 42.9 ms |

That's the whole probe, hashing included, so it isn't the cost of the watch alone. The watch fired about 50 times a frame, and one bare fault takes 4.6 us on this Mac (`mprotect` plus signal, measured outside the SDK), so about 0.2 ms before the SDK's own lock and bookkeeping. One earlier run read 18.75 FPS and didn't repeat.

**Driving with the watch** (`crackdown_249.log`, `texture-probe-1793488834.json`), 23,400 frames over 13 minutes, including jumps between agency spawns in the Volk, Shai Gen and Los Muertos. Frames 2,700-8,100 streamed almost nothing, so about 8.5 minutes of it was streaming.

| 9,187 changes | |
| --- | --- |
| The watch fired | 2,986 (33%) |
| Not fired, but a resolve wrote the page | 6,153 (67%) |
| Neither | 48 (0.5%, 3.5 MB of 2,097 MB) |
| Of the 1,073 no cheap signal saw, the watch fired on | 1,043 (97%) |

The resolve share is a steady 240 or so per 30 seconds in every window, moving or not, so it's render targets being read back and not streaming. The share of streamed textures the watch catches is the row that matters.

The `unwatched` list holds 38 of the 48. 37 are linear `k_8_8_8_8`, mostly 128x1 and 24-48 pixels square, and one is a tiled 128x128 that changed 33 times, so it's probably a render target. No DXT texture is in the list. The other 10 aren't listed, so they could be anything. Nothing yet says what writes those small linear ones.

| Per frame | |
| --- | --- |
| Watch fires | 120, against 50 on the benchmark |
| Arming | 0.06 ms |
| Bytes reported | 1.9 MB |
| False alarms | 46,652 checks, 8.2 GB over the session, about 0.35 MB a frame |

Fires hold at about 113 a frame in the windows with no streaming and about 125 in the ones with it, so streaming isn't what drives them. Hot data sharing a 16 KB page with a texture is the likely cause, inflated here because the probe rearms every texture it checks and the exact range keeps pages armed. At about 5-10 us each that's roughly 1 ms a frame at worst, spread across the threads that wrote. The 5-10 us is an estimate from the bare 4.6 us fault.

The frame rate with all this on was median 29.5 FPS over 826 seconds, with 0 seconds under 12 and 28 under 20. That isn't a controlled comparison.

**Verdict: go. A native renderer learns of texture changes from the page watch, plus its own resolves.** Coverage is 99.5% of changes and 99.8% of bytes, and streamed textures don't appear among the known gaps. Three things carry into the texture path:

- **The 48.** Small linear textures could be hashed in full at bind, which is cheap at those sizes. That's a plan and not a measurement.
- **The listener's range.** In the native renderer the emulator's listener is gone, so the renderer sets the range. Exact costs more faults, and a wider one costs more false uploads. The renderer also arms only when it uploads, so fires should fall from what the probe saw.
- **Reads vs writes.** The first read after a fire has to arm before it reads, as the probe does.

Vertex buffers are live guest memory with the same question, and the probe doesn't track them. The same watch would cover them if it holds up, as `primitive_processor.cpp` already does for index buffers.

Texture sizes are estimates, since the SDK's layout code isn't reachable from the game.

## Shadow mode (2026-10-31)

`--crackdown_native_shadow=true` starts a Metal device and a render thread of their own beside the emulator. `src/native/shadow.cpp` takes each `RunCommandBuffer` from the game's threads and hands the frame over at the swap, without ever waiting: if the thread falls behind, the frame is dropped and counted. The thread replays constants into a per-frame arena, 5 KB per draw (4 KB vertex, 1 KB pixel), and records one placeholder draw per passing baked draw into an off-screen target. `src/native/metal_device.mm` is the only file that sees Metal. Off by default, and there's no window yet because placeholders have nothing to show.

**Guest memory goes to the GPU without a copy.** All 512 MB of the physical view wraps in one `newBufferWithBytesNoCopy` buffer, which worked first try on the file-backed mapping. That view is the one page watches don't protect, so the GPU can't fault on an armed page. Every 300 frames a blit through the GPU's view is compared with the CPU's, on 4 KB from a live index buffer: 12 of 12 matched across four runs. That proves reads of static data, not that the GPU sees a fresh write the moment it lands.

**Four rooftop runs**, 900 game frames each:

| Per frame | |
| --- | --- |
| Baked draws recorded | about 1,800, plus 28 skipped by pass |
| Dropped frames | 0 |
| Handing runs over, on the game's thread | about 0.02 ms |
| Render thread: recording, p50 | 0.57-0.59 ms |
| Render thread: recording, p95 | 0.75-0.90 ms |
| Render thread: recording, worst | 2.07 ms |
| GPU | 0.34-0.49 ms, worst 3.4 ms, 0 errors |
| Waiting for a free frame | 0 ms |

| Benchmark, interleaved | Average FPS | Median frame | Peak memory |
| --- | --- | --- | --- |
| Off | 23.05, 24.25, 24.00 | 43.3, 41.3, 41.6 ms | 2,528-2,564 MB |
| On | 24.54, 24.94, 23.72 | 40.5, 40.1, 42.1 ms | 2,631-2,651 MB |

**No measurable frame rate cost**, within the noise of three rounds, and about 85 MB more memory, most of it the 60 MB of constant arenas. Wrapping guest memory didn't add its 512 MB.

**This understates a real renderer.** Recording at about 0.3 microseconds a draw is cheaper than the 1.4 in the encoding test because these draws change much less: 16 placeholder pipelines, one pass, no textures, depth, scissor or viewport changes. They produce no pixels and put almost no load on the GPU. Direct draws aren't captured, so this is roughly the baked 40-60% of a frame. What it does prove is the plumbing: Metal runs inside the game next to MoltenVK, the thread keeps up, and the hand-over costs the game nothing measurable. Only the rooftop has been run, not a drive.

## First picture (2026-10-31)

Shadow mode now draws the frame's main scene with the game's real vertex shaders, live camera and live geometry, in grey with one plain pixel shader, off screen or in a window with `--crackdown_native_shadow_window=true`. There are no textures, no real pixel shaders and no direct draws yet.

**Shipped vertex shaders hold blanks.** 155 of the 218 have format 0, stride 0 and slot 95 in every vertex fetch, because D3D patches those into a copy of the shader when it binds it. So the layout can only be read from live ucode. `tools/shader-trial/pull_vertices.py` gives each shader one `pullElement` call per vertex element, and `src/native/shadow.cpp` builds the table from the live ucode at the addresses `manifest.txt` lists. Live shaders are matched by a SHA-256 with the patched fields blanked, which leaves 191 distinct shaders out of 218. Every live shader on the rooftop matched: 32-40 per run, 0 missing, 0 failed.

**Vertices are pulled from guest memory on the GPU; indices are copied.** A vertex descriptor can't read big-endian data, and `pullElement` reads through the no-copy wrap and swaps in the shader, following the emulator's decoding. Indices go through a per-frame byte-swapped copy so the GPU keeps its post-transform cache and primitive restart. That settles the Phase 2 vertex question for now, without a comparison of the alternatives. Fetch constants and all 256 vertex constants are copied per draw, which is wasteful and simple.

**The game uses reversed depth:** `RB_DEPTHCONTROL` is `0x00708767`, greater-or-equal, so depth clears to 0. The first run cleared to 1 and drew nothing.

**Only one stretch of the frame was drawn at this point.** Frames were cut at each `Resolve` and only the stretch with the most draws was recorded, about 660 of 1,830 baked draws. [Every pass](#every-pass-2026-10-31) replaced that. Culling is off.

| Rooftop, per frame, three runs | |
| --- | --- |
| Drawn | about 665 draws, 633,000 indices |
| Render thread, recording, p50 / p95 / worst | 0.70-0.72 / 1.07-1.14 / 8.5 ms |
| Copying indices | 0.14 ms |
| GPU | 1.30-1.36 ms, worst 3.9 ms, 0 errors |
| Dropped frames | 0 |

| Benchmark, interleaved | Average FPS | Median frame | Peak memory |
| --- | --- | --- | --- |
| Off | 25.59, 23.55, 24.37 | 39.0, 41.8, 40.9 ms | 2,547-2,605 MB |
| On | 23.18, 22.82, 23.84 | 43.0, 43.5, 41.9 ms | 2,785-2,798 MB |

So shadow mode now costs about 1 FPS, or 2 ms on the median frame, and 220 MB, mostly 192 MB of frame arenas. It runs on top of the emulator, so that's its load added to the frame and says nothing about a takeover.

**The GPU time flatters it.** Vertex work for those 633,000 indices is real, but the pixel shader is trivial, there's a single target, and two thirds of the baked draws and every direct draw are missing.

**Compared with the emulator on two scenes, by eye.** Screenshots of both windows, taken a moment apart in one benchmark run, show the same camera and geometry on the rooftop and inside the Agency tower: columns, wall slots, the X panel, floor lines, the balcony and the ledge the player stands on, which is the band across the bottom of the rooftop frame. A frame from a street in Bruce's session also shows parked cars, so some cars are baked. The player and HUD are missing, as direct draws. That's two scenes by eye, not a pixel comparison.

**The window needed three fixes.** `CAMetalLayer` has to be created and set up on the main thread: on the render thread its changes sit in a transaction that thread's missing run loop never commits, and the layer displays nothing with every frame reporting `presentedTime` 0. A view has to supply the layer through `makeBackingLayer`, and keeping the picture's shape takes `layerContentsPlacement`, because AppKit overrides `contentsGravity`. After the fix, 9 of the first 100 frames weren't shown and none of the next 100 were.

## Every pass (2026-10-31)

Shadow mode now records every baked draw, each to the target it belongs to, and shows the colour target with the most draws.

**A frame's baked draws fall into four stretches on two targets.** From one rooftop frame, split at resolves:

| Stretch | Target | Baked draws | Depth test |
| --- | --- | --- | --- |
| Before the first resolve | 1024x1024 depth only | 148 | less-or-equal |
| After it | Same | 426 | less-or-equal |
| Next | 1280x720 colour and depth, 2x MSAA in the game | 640 | greater-or-equal |
| After 12 more resolves with no baked draws | Same | 680 | greater-or-equal, stencil on |

So the 1024x1024 target is presumably a shadow map and the main view is drawn twice, which suggests a depth pass followed by the colour pass. The first version drew only the last row.

**How passes are built.** Each run records the device's colour and depth surfaces, at +12160 and +12176, and the size comes from the draw's viewport, since surfaces only give a pitch. A new pass starts when the target changes or after `Clear`, and a target's first pass in a frame clears, to 0 for greater-or-equal and 1 otherwise. That's a guess at what the game clears to, because `Clear`'s arguments aren't read. MSAA is left out.

| Rooftop, per frame, three runs | |
| --- | --- |
| Drawn | 1,830 draws, 1.59 million indices, 5 Metal passes |
| Render thread, recording, p50 / p95 / worst | 1.10-1.18 / 1.81-1.99 / 15.3 ms |
| Copying indices | 0.24-0.25 ms |
| GPU | 2.47-2.48 ms, worst 8.4 ms, 0 errors |
| Dropped frames | 0 |
| Live vertex shaders | 51-54, 0 missing |

| Benchmark, interleaved | Average FPS | Median frame | Peak memory |
| --- | --- | --- | --- |
| Off | 25.52, 25.68, 26.86 | 37.5, 38.8, 36.9 ms | 2,563-2,607 MB |
| On | 24.76, 23.78, 23.74 | 40.2, 41.0, 41.5 ms | 2,814-2,890 MB |

Shadow mode now costs about 2 FPS, 3 ms on the median frame and 260 MB, on top of the emulator.

**The rooftop picture didn't change,** as expected if the first main stretch draws the same geometry as the second. Colour from the shadow passes is never sampled yet, since there are no pixel shaders.

**What baked draws leave out, from Bruce's drive with this build.** Some cars draw and some don't. Those that do are complete far away and lose their bodies up close, leaving wheels and trim, and many of the missing vehicles are gang vehicles. Trees, throwable objects, miscellaneous props, people and characters are all missing. Drawing every pass didn't bring these back, so the likely explanation is that they're direct draws: a car's far LOD is baked, and its near body goes through the Direct3D setters so it can dent and come apart. That fits the census, where baked draws are 41-64% of city draws, but no draw has been traced to a specific object yet.

## Direct draws (2026-11-01)

Shadow mode now records direct draws alongside baked ones, in the order the game made them. The player, pedestrians, car bodies, trees and the sky dome appear, which settles the guess in [Every pass](#every-pass-2026-10-31): those were direct draws.

**How they're read: as a stream, the way the GPU reads them.** D3D's ring holds everything the GPU will do, as only what changed, in the packets the baked decoder already knows. `BakedLibrary::DecodeRing` keeps its registers between calls and does what the GPU would to them, including `SET_CONSTANT` and loads from memory, so it always knows what the GPU would hold. Commands sit in contiguous segments, from `device + 14552` to the write pointer at `device + 48`, and D3D keeps its transient data, such as patched shaders and vertices, in the same memory between them. `826BB1A0` closes a segment, so a hook there reads the rest of it first. The stream is read after every draw and before every baked run, which keeps the two in order. Nothing about the device's layout had to be worked out beyond those fields. A takeover removes the ring, so this is a shadow mode shortcut: the same state has to come from the device then.

**Reading a span around each draw call didn't work.** That was the first version, and it looked right until a drive. A span could hold a stretch of other work, including transient data, so a draw call's words weren't always its own. Only the last draw was kept, and parsing stopped at the data. That lost draws, constants and whole objects, and left registers stale, which showed as flicker.

**The exploded cars were embedded shaders.** Vehicles grew tents of flat sheets, fixed to them, with the bodies otherwise correct. D3D can put a patched vertex shader inside the ring as `IM_LOAD_IMMEDIATE`, and those draws were left with the previous draw's shader and its vertex layout, so they read their vertices at the wrong stride. `DecodeRing` now keeps that code and it travels with the draw. Finding it took Bruce's observation that the bodies were fine and the sheets stayed attached, then a build that coloured each draw by its number, so a pixel of a sheet named its draw. Stale constants, the first guess, weren't the cause, and neither was the targeting glow. Several minutes of driving showed no more of them.

**Other things that were found the hard way:**

- **The ring pointer rests on the last word written**, not the next free one.
- **A shader bound to a new vertex format is often patched into a copy in the ring.** Those are gone by the time the render thread runs, so their code now travels with the draw, and they're cached by content.
- **Binding also rewrites each fetch's destination swizzle**, for example to swap red and blue or to force W to 1. The hash now ignores it, and the translated shaders, which are fixed to the shipped swizzle, get a per-element remap (`select`) built from the shipped and live swizzles. **That changed `manifest.txt` and the metallibs:** rerun `pull_vertices.py` after updating, and old shaders won't work with this build. 177 shaders now, down from 191, since some differed only in swizzle.
- **One shader can load several blocks of literals,** and the baked decoder kept only the last, so some baked draws had partly wrong constants. It now keeps up to six, per stage. No difference in the picture was looked for.
- **Surfaces know their size.** `surface + 36` holds width and height, and a tiled surface holds one tile, so the tile rects give the whole height. Targets are keyed on the surfaces and no longer guessed from viewports, which gave up to 53 targets in place of 3.
- **Effect draws are left out of the grey view.** A draw that writes no depth and is blended, or writes no colour at all, comes out as a solid sheet with no pixel shader: a spiky wedge across the player turned out to be one, alpha blended with depth writes off. The sky writes no depth and isn't blended, so it stays. About 40 alpha-tested draws a frame stay too, since only a pixel shader can cut them out, so foliage and fences show as solid planes.

**Ruled out for the flicker:** reading late. Holding the game at each frame's end until Metal finished didn't change it. With time frozen and the camera still, the old build jumped by about 100,000 pixels every so often, because the sky dome came and went. The stream build held 1,500-2,300 pixels between all eight frozen pairs, which is pedestrians, who don't freeze. A few draws a frame were also going to bogus targets.

| Rooftop, one run, last 300 frames | |
| --- | --- |
| Drawn | About 4,000 draws, 2,716 of them direct, about 2 million indices, 5 passes, 3 targets |
| Left out as blended, or writing no colour | 233, 11 |
| Left out as not indexed lists or strips | 342 |
| No matching shader | 0, with 1 shader unmatched |
| No target | 0 |
| Ring parse errors | 0 |
| Render thread, recording, p50 | 3.5 ms, measured before embedded shaders |
| Game thread, capture | 0.88 ms, measured before embedded shaders |
| GPU errors | 0 |
| Dropped frames | 3 in the last 300 |

Before the stream, 2,380 direct draws were captured a frame on the same spot. The interleaved cost below was measured on that earlier build and hasn't been repeated:

| Benchmark, interleaved, earlier build | Average FPS | Median frame |
| --- | --- | --- |
| Off | 28.12, 22.48, 20.05 | 35.3, 39.4, 45.9 ms |
| On | 22.70, 20.51, 14.06 | 40.9, 42.6, 69.2 ms |

The Air warmed through those runs, so compare each pair rather than the rows: on is 2-6 FPS below the run before it. Peak memory wasn't measured: resident size didn't separate the two.

**Not drawn yet:** the 342 above, and quads drawn from vertices in the ring (`DRAW_INDX_2`, about 340). Fullscreen passes are likely among them, and they'd paint over a grey picture, so they're better added with pixel shaders.

**Other limits:**

- Vertex data is read a frame late from live memory, so a buffer rewritten every frame could tear. Nothing has been seen to do so.
- Baked shaders are cached by address, so one streamed out and replaced at the same address would stay wrong.
- The hook on `826BB1A0` is bound for the base guest only. The title update 6 guest hasn't got it, and these device offsets haven't been checked there.
- A list being recorded is skipped in the stream, on the flag at `device + 10812`, because it's decoded when it runs. Nothing has shown one being recorded.

## Free camera (2026-10-31)

A test tool in the cheat menu, under Testing, that flies the game's camera, so both renderers follow it. Session only: it isn't in the launcher. WASD moves, Q and E go down and up, the mouse looks, Shift goes faster, 1-9 jump to saved spots, Shift+1-9 save one to `camera-spots.txt`, and F freezes time. `--crackdown_free_camera_start=x,y,z,yaw,pitch` starts with it on, for scripted runs.

**The world's detail follows the player, not the camera.** With the camera about 480 units from the player at 1000,120,-1100, buildings lost their walls and props. Writing the camera's position over the player's (`character + 0x140`) brought back the full city, in the same view:

| Run | Player's position | Detail at the camera |
| --- | --- | --- |
| Camera alone | not touched | low |
| Three global copies of the player's position overwritten | not touched | low, byte-identical to the row above |
| Rewritten every update, mid-play | rewritten | full |
| Rewritten, then restored at simulation start, at the aim update, or at steps 11, 14, 16 or after the simulation | restored | low |

So the write has to stay in place through step 16 of `SimulationUpdate` (`824EC028`, a loop over 61 objects), up to the camera update. That is as far as the search went: **the code that reads it hasn't been found.** The ground check reads it in the same stretch, so a restore that saves the player's height also loses the detail.

What the write does to the player, from a run that turned the camera off after 150 updates:

- **Position on the map:** unchanged. The game's physics puts the real x and z back every update.
- **Height:** they drop under the map, as Bruce saw in play. From the 39 m ledge the log shows about -1.5, and they stayed there after the camera went off, with no damage.
- **Not tested:** a camera over tall ground.

Bruce's play also found that the cursor stays on screen and stops the camera turning all the way round, and that NPCs away from the player look like they drop to a reduced animation state, which isn't traced. All of this is parked: it's good enough for testing draws.

A write held through the game's spawn moves the player for real, because spawning takes their physics position from this field. Starting with `--crackdown_free_camera_start` does this, so scripted runs stand the player under the camera.

**Freeze Time:** a car in view stayed in place across 20 seconds of captures, where the street changed between captures before freezing. Two other cars appeared in one capture only and didn't move, and that isn't explained.

**Tried and dropped:** skipping the player's own update (`8229B5C0`) doesn't stop the height change, and left the player at their saved position instead of their spawn. A byte-level memory scan found 68 copies of the player's position and 11 of the game camera's, including a struct at `82F5D1C0` holding the camera, the player and a point 50 units ahead of the player. Nothing in the code refers to that struct directly, and the scan isn't kept in the build.

## Shader trial (2026-10-31)

Phase 1 ran XenosRecomp's Metal branch ([PR 22](https://github.com/hedge-dev/XenosRecomp/pull/22), commit `4906992`) over every shipped shader, first as it stands, then with this project's patch (`tools/shader-trial/xenosrecomp-crackdown.patch`).

| Step | Unpatched | Patched |
| --- | --- | --- |
| Translated without crashing | 368 of 382 | **382** |
| Compiled by Apple's compiler | 253 | **382** |
| Features flagged unsupported | not reported | **none** |

The survey counted 384 shaders, two PS more; that gap is unexplained.

**The patch fixes:**

- **Conditional exec blocks never tested their condition,** so those blocks always ran. No error showed this. 181 shaders now have bool conditions. Conditional ends also stay inside the condition, as the emulator has them, and one end opcode that never ended the shader is fixed.
- **Texture levels:** explicit LOD and LOD bias now work, used by 101 and 22 shaders, following the emulator's rule that vertex shaders always pick their level.
- **Vertex texture fetch:** vertex shaders get the texture tables, and fetch constants 16 to 19 map to table registers 0 to 3 (checked against the constant tables).
- **Relative constants:** the second and third constants read `const1Relative`, as the emulator does. This was what tripped the asserts.
- **Loops:** loop constants the game sets at runtime are read from shared constants, and the loop now uses its start and step. 9 shaders loop.
- **Smaller gaps:** `NORMAL0` and `COLOR2` interpolators, 1D fetches as 2D one texel high, unlisted texture slots and bools, empty `vfetch` swizzles, and extra vertex element locations.

- **Cube lookups** ignored the instruction's source swizzle, so shaders keeping their direction in other components sampled the wrong direction. Only the pixel comparison below caught this.

Anything the output doesn't implement now prints `UNSUPPORTED` for that shader: gradient fetches, relative registers, and loops in complex control flow. None of the 382 does. The survey's count of 32 VS and 32 PS using `aL`-relative sources looks wrong, since no shader sets the bits the translator checks.

**Pixel comparison.** `tools/shader-trial/pixel_diff.swift` draws each of the 108 pixel shaders seen in the benchmark in both forms, from identical inputs, into a floating-point target, with bools off, on and mixed. Textures have full mip chains. Constants map by guest register, shader literals are placed in the emulator's buffer, and interpolators map by register.

| Result (worst of the three bool settings) | As shipped | Mip selection matched |
| --- | --- | --- |
| Exact (under 2/255 on 99.9% of pixels) | **100** | **106** |
| Close (under 8/255 on 99% of pixels) | 7 | 2 |
| Off | 1 | 0 |

The difference between the columns is mip selection: the emulator computes gradients itself and XenosRecomp leaves them to the GPU, which can pick a slightly different level on per-pixel coordinates. The native layer can copy the emulator's approach if that shows. 9 shaders have one constant only a discarded result reads, placed by trying each position, and all 9 match exactly, as do the 3 using relative constants.

**Pixel shader cost, drawing the same picture.** 1280x720 with 2x MSAA, one draw per pass, 108 shaders. XenosRecomp as a share of the emulator:

| Inputs | Median | Middle 80% | Total, ms |
| --- | --- | --- | --- |
| Noise textures, no mips, bools on | **28%** | 18% to 48% | 32.0 against 114.1 |
| Noise textures, no mips, bools off | 28% | 18% to 47% | 31.9 against 109.3 |
| Noise textures, mips, bools on | 25% | 18% to 37% | 26.3 against 108.9 |
| Smooth textures, mips, bools on | 26% | 17% to 40% | 21.9 against 86.4 |

So pixel shaders cost about a quarter to 30% of the emulator's. Turning bools on raises both sides alike. The outline shader (`B20B230BE9FD2568`) takes 0.24 ms against 0.88 ms, and the heaviest (`0FB4021B3EBDDBAE`) 0.24 ms against 2.5 ms. One or two shaders run slightly dearer, the worst 1.12 times.

**The earlier 44% understated the emulator.** XenosRecomp's total barely moved (34 ms then, 32 ms now under the same kind of textures), while the emulator's went from 75 ms to 109 ms. The earlier harness left the emulator's alpha test at never pass, with its constants unmapped, so it wasn't drawing a real picture. The likely cause is the discard, though that wasn't tested on its own.

**Verdict: stay with XenosRecomp, with `xenosrecomp-crackdown.patch`.** Every shipped shader compiles, the 108 compared draw what the emulator draws, and they cost well under half. Phase 1 is done.

**What this doesn't show:**

- **Synthetic inputs.** Real textures, constants and bools come from the game; these are generated. Agreement across three bool settings and random constants is strong evidence, not proof.
- **Coverage.** 108 of 164 pixel shaders ran in the benchmark and were compared. The other 56 and all 218 vertex shaders have compiled but not been compared or timed.
- **Whole-frame gain is unknown.** The emulator's frame time also holds vertex work, transfers and passes, and how much is pixel shading in play hasn't been measured. A quarter of the shader cost doesn't mean a quarter of the GPU time.
- **Texture format work isn't included.** Signed, gamma and other format handling is off here. Some of it moves into the native layer's textures and some may cost shader time.
- **Requirements this adds to the native layer:** upload the shaders' literal constants, because relative reads go to the buffer; supply bool and loop constants at the shared offsets; bind 1D textures as 2D.
- **The HLSL branch of the generated code wasn't kept working.** The new LOD fetches and runtime loops exist for Metal only.

## 1. How the game reaches the GPU

### The path today

Game code calls the statically linked D3D runtime, which keeps a shadow device in guest memory and builds PM4 packets in a ring. `VdSwap` and the write pointer hand that to the emulated command processor, which decodes PM4, emulates EDRAM, and translates shaders from Xenos to SPIR-V to MSL at runtime through MoltenVK.

The XEX header lists the libraries, all XDK 2.0.4802: `D3D9LTCG`, `D3DX9`, `XGRAPHC`, `XUIRUNL`, `XUIRNDRL`, plus audio, network and kernel libraries. **`D3D9LTCG` matters:** D3D was link-time optimised into the game, so its small functions are partly inlined into game code.

### Finding the functions

Three anchors locate it. All addresses are for the base (1.0) image.

1. **`Vd*` imports.** All callers sit between `0x826BB950` and `0x826CD6F0`; `VdSwap` has one caller, `sub_826BCA98`.
2. **The address map.** `config/tu6/address-map.csv` already names the swap builder, swap packet, swap callback, GPU fence and GPU poll, mapped to TU6 with identical code. `src/gpu_fence_wait.cpp` documents the device writeback block.
3. **PM4 headers.** Scanning instruction comments in `generated/default/crackdown_recomp.*.cpp` for type-3 packet headers finds the draw, event and bin packets.

The D3D code forms one contiguous block, `0x826AC160`-`0x826D0800`:

| Measure | Count |
| --- | --- |
| Functions in the block | 567 (about 37,000 PPC instructions) |
| ...of which another library (`0x826B4000`-`0x826B8FFF`, no device fields) | 68 |
| Functions with direct callers outside the block | 181 |
| Render and sampler state setters and getters with no direct caller (`0x826AF800`-`0x826B2000`) | 182, of which 71 are getters |
| Functions that can reach the ring | 113 |
| ...of which have callers outside the block | 51 |

For scale the whole program is 41,219 functions and 2.67 million instructions, so the block is under 2% of the code.

### The shadow device

Every setter takes the device in `r3`, writes a field and sets a dirty bit, as in Unleashed's `GuestDevice`:

| Device offset | Contents |
| --- | --- |
| +0 upward | 64-bit dirty masks (at least five) |
| +48, +56 | ring write pointer and limit |
| +1024 | texture fetch constants, 24 bytes per stage |
| +1792 | vertex shader float constants, 16 bytes each; pixel constants follow at +5888 |
| +9984 onward | bool constants (`2496 * 4`), then integer constants (`2504 * 4`) |
| +10768 | GPU writeback block pointer (`src/gpu_fence_wait.cpp`) |
| +12156 | vertex declaration |
| +12160 to +12176 | colour render targets, depth stencil |
| +12412, +12416 | vertex shader, pixel shader |
| +12428, +12432 | bin mask, low and high halves (tiling). The select's high half is `RunCommandBuffer`'s third argument |
| +12468, +12472 | tile count, tile rects |
| +13220 | present interval |

**LTCG inlined the state writes, and that is fine.** 82 game functions set dirty bits inline and 67 touch two or more device fields directly, mostly in the renderer around `0x825A0000`-`0x825F0000`. Hooking every setter would miss those. The design that survives this is the one Unleashed uses: leave the state code running on the shadow device, hook only the functions that would touch the ring, and read shadow state plus dirty bits at draw time.

**The game does not write packets itself.** No function outside the block has the ring-write pattern. Four game functions call the ring function `sub_826BB408` directly (`82324330`, `82326A90`, `825E12F8`, `826A7AD0`), and in all four it is an inlined fence or kick, not packet writing.

### What must be replaced

| Group | Function | Evidence | Game fns / sites |
| --- | --- | --- | --- |
| Device | `826BBFB8` CreateDevice | reaches `VdInitializeEngines`, ring init; called by `82930930` | 1 / 1 |
| | `826BC0A0` device release | reaches engine shutdown | 7 / 8 |
| Frame | `826BCA98` swap builder | only `VdSwap` caller; in address map | 1 / 3 |
| | `826BC568`, `826BC3D0` swap packet, callback | address map | internal |
| | `826BD1B8` Present | calls both resolve and swap | 1 / 1 |
| | `826CE0C8` area, D3D thread | repeats a present after 30 ms (`repeat_present_return`) | internal |
| Sync | `826BB408` ring reserve and kick | 94 sites | 4 / 94 |
| | `826BB4F0`, `826BBEE0` fence, poll | address map | 8 / 29 |
| | `826BB7D8` block until idle | writes a wait, then fence-waits | 5 / 19 |
| | `826BB8B0`, `826BB928` fence pending, block on resource | fence word at object +8 | 3-4 / 4 |
| | `826BB5C8` insert callback | `WAIT_REG_MEM` + `INTERRUPT`; game passes a function | 2 / 5 |
| Draw | `826B91E0`, `826B96D0` BeginVertices, BeginIndexedVertices (+ `826B9688` DrawPrimitiveUP) | return ring space; stride stored at +12248 | 25, 7 / 38 |
| | `826B9C08`, `826B9FF8` non-indexed and indexed draw | `DRAW_INDX`, shared flush `826C6C48` | 14, 30 / 63 |
| Targets | `826BE578` Resolve | 897 instructions; loads a copy shader; `IM_LOAD_IMMEDIATE`, `EVENT_WRITE` | 15 / 31 |
| | `826C04A0`, `826C03E0` Clear, ClearF | quad via `826BF380`; used by tiling | 8, 3 / 16 |
| | `826B1EE0`, `826B2218` SetRenderTarget, SetDepthStencil | device +12160, +12176 | 12, 11 / 43 |
| | `826B1C60`, `826B1B60` SetViewport, SetScissor | `D3DVIEWPORT9`, `RECT` shapes; emit window state | 6, 6 / 34 |
| Tiling | `826BD488` BeginTiling | args match the XDK call | 4 / 5 |
| | `826BDA38` EndTiling | args match; calls Resolve | 3 / 3 |
| | `826BD300`, `826AF1B0` predication | emit `SET_BIN_MASK`, `SET_BIN_SELECT` | 12 / 32 |
| Queries | `826C0958`, `826C0B38`, `826C0D38` create, issue, get data | `EVENT_WRITE_ZPD` via `826C05C8` | 4, 6, 4 / 23 |
| Resources | `826AEC58` CreateTexture | allocates a 52-byte object | 18 / 26 |
| | `826AED90` create surface or buffer | 48-byte object, critical section | 6 / 15 |
| | `826AE1C0`, `826AC8F0`, `826ACB80` lock, unlock | lock flags | 14-27 / 107 |
| | `826ACD08`, `826ACC90` Release, AddRef | atomic count at +4 | 115, 59 / 544 |
| Shaders | `826B3338`, `826B3748` CreateVertexShader, CreatePixelShader | parse the container | 6, 7 / 13 |
| | `826B3AE8` CreateVertexDeclaration | walks elements to the 255 terminator | 30 / 39 |
| Buffers | `826C3750` RunCommandBuffer | `INDIRECT_BUFFER` per list, full state flush; one caller, `823B3328`; about 800 runs per frame | 1 / 1 |
| | `826B3C48` | unidentified | 2 / 11 |
| | `826C42F8`, `826C4258` state block capture, free | walks 26 textures, 16 streams | 4 / 8 |

Setters that stay as guest code, because they only touch the shadow device: `826AF038` SetTexture, `826B2F68`/`826B3070` float constants, `826B3178`/`826B31D8` bool, `826B3238`/`826B3290` integer, `826B3458`/`826B3848` SetVertexShader/SetPixelShader, `826B1E50` SetVertexDeclaration, `826B1D30` SetStreamSource, and the sampler and render state tables. A native layer reads their results.

**Total to cover: about 50-60 functions**, roughly 35 replaced outright and 15-25 stubbed or adapted. Most of the remaining 450 can keep running as guest code.

`sub_826C4C70` deserves a note: a `dcbf` loop over a physical range with 23 game callers and 53 sites. It tells us where the game has just written GPU-visible memory, which may be a cheaper dirty signal than page protection (see section 2).

## 2. What the game relies on that is hard

Counts come from the pipeline cache snapshot described in section 3.

| Feature | Evidence in Crackdown | On Metal | Risk |
| --- | --- | --- | --- |
| **EDRAM resolves** | Resolve has 31 sites in 15 game functions, plus EndTiling and Present. The menu samples a resolved `k_24_8_FLOAT` depth at 1280x720 | Becomes a render pass store or a copy. No EDRAM transfers at all, which removes the ~34 per frame the emulator pays | Medium |
| **CPU reads of resolves** | `config/crackdown.toml` sets `readback_resolve = "fast"` capped at 256 KB because the game reads small resolves on the CPU | Blit to a shared buffer and write guest memory. Must be timed so the game sees it. Needs a census of which resolves | **High** |
| **MSAA** | 2x on 293 of 391 pipelines, 1x on 91, 4x on 7 | Native. On a tile GPU, resolving at end of pass is cheap. Xenos 2x sample positions may differ, and resolves may pick one sample rather than average (patch 0062 warns about this) | Low-medium |
| **Predicated tiling** | See below | Remove it: draw once into a real 720p target | **High effort, high reward** |
| **Memexport** | None. No shader allocates a memory export: 0 of 384 shipped, 0 of 262 cached | Nothing to do. `readback_memexport` is unused here | None |
| **Occlusion queries** | Issued from 6 game functions; about 2000 every few seconds, 97% return 0. In-pass recording gained 58% | Visibility result buffer in counting mode, inside the pass, in shared memory | Low |
| **Render targets** | `8_8_8_8` on 367 of 371 colour pipelines, `16_16_16_16_FLOAT` on 3, `16_16_FLOAT` on 1. No MRT. `2_10_10_10_FLOAT` and `8_8_8_8_GAMMA` never appear | Direct format mapping | Low |
| **Depth** | `D24FS8` on 358 pipelines, `D24S8` on 17 | Depth32Float_Stencil8 for both; no emulated 20e4 needed unless the CPU reads it | Low |
| **Textures** | Fetch constants come from resource headers at +28. Streamed data sits raw in `.resblock` memory: big-endian and tiled | Untile and convert, reusing `src/graphics/pipeline/texture/` and the load shaders. BC formats are native on M1 | **High** |
| **Vertex data** | Big-endian, raw in guest memory. 94 runtime VS differ from shipped only in `vfetch` fields, patched when D3D binds a declaration | Metal cannot swap bytes in vertex fetch. Either pull vertices in the shader or copy and swap | **Medium-high** |
| **GPU fences and callbacks** | Five wait sites on the writeback block; the swap throttle and vblank callback pace the game | Signal the same words from Metal completion handlers, keeping `crackdown_native_gpu_waits` | Medium |

### Predicated tiling

720p at 2x with 32-bit colour and depth needs 1280 × 720 × 2 × 8 = 14.7 MB, and EDRAM holds 10 MiB. Since 293 pipelines render 2x colour plus depth, and the benchmark runs 1280x720 at 100% scale, the main pass cannot fit without tiling.

The code agrees. `BeginTiling` and `EndTiling` are called from the frame renderer (`82326A90`, tile count read from a table by mode) and from `825C5790` (one path passes a literal 4), and both clear predication just before `EndTiling`. The menu notes in `HANDOFF.md` also fit: a `k_8_8_8_8` resolve at 1280x484 looks like one tile, and the 1280x1024 main target is exactly the height all 2048 EDRAM tiles give at that pitch and sample count.

In the emulator every tile costs resolves and EDRAM transfers, and predicated packets are filtered one by one (`CommandProcessor` checks `bin_select_ & bin_mask_` per packet). Natively, BeginTiling and EndTiling become bookkeeping, predication goes away, and the deferred resolves run once at the end. The 6,207 draws per city frame may be partly tile replays, but that is unmeasured.

Do not try collapsing tiles inside the emulator as a shortcut: the target would no longer fit the emulated 10 MiB.

### The streamed-data problem

This is the largest difference from Unleashed. That game funnels textures through hookable paths such as `MakePictureData`, so it can create host textures up front and cache them by object. Crackdown's streamer may load prebuilt headers plus raw data, in which case the first the runtime sees of a texture is `SetTexture` copying its fetch constant.

A native layer probably needs what the emulator has anyway: a cache keyed by address and format, invalidated when guest memory changes. So the texture cache is a component to keep, not to delete. The options are page protection as now, hooking the streamer's completion, or the `sub_826C4C70` flush signal.

For vertex and index data the M1's unified memory offers a shortcut worth testing: wrap guest memory in a no-copy `MTLBuffer` and pull vertices in the shader, swapping bytes there. That removes uploads entirely, which is the source of the largest pass-break count measured so far (~128 per frame).

## 3. Shaders

### Runtime cache

Parsed from `user_data/cache/shaders/shareable/4D5307DC.xsh` and `.fbo.vk.xpso` (snapshot taken during this survey; it grows with play).

| Measure | Count |
| --- | --- |
| Unique shaders | 262: 152 VS, 110 PS |
| Pipelines | 391 |
| VS and PS pairs | 236 |
| VS and PS with translation variants | 156 VS, 112 PS (incl. no PS) |

The count of variants is striking: almost every shader has exactly one emulator translation, so pipeline state is a small problem. 391 pipelines is few enough for a Metal binary archive built ahead of time.

### Shipped shaders

The game carries its shaders as containers with magic `0x102A1100` (PS) and `0x102A1101` (VS). That is the format XenosRecomp reads, including constant tables with names such as `SkinBones`, `ViewProjection`, `Diffuse_map` and `TargettingData`.

| File | Contents |
| --- | --- |
| `ScreenRenderer/prebuilt_shadercache.dat` | 160 VS, 160 PS, inline |
| `ScreenRenderer/RTWUberShader.bin` | 160 VS, 160 PS, same ucode as above |
| `vfx_shaders.spk` | 118 VS, 118 PS; listed in `vfx_shaders.lst` |
| `boundshaders.bsh` | 103 prebound VS (flags `0x102A1141`) |

Pack layout: 16-byte header, then a 12-byte index entry per shader, then a virtual section (872-byte prefix for VS, 40 for PS, then the container), then a physical section holding the ucode.

Unique shipped ucode: **384 shaders, 218 VS and 166 PS.** `RTWUberShader.fx` and `M12UberShaderStatic.fx` also ship, apparently as source (not read for this survey), and `vfx_shaders.lst` gives the compile defines of 250 entries.

### Coverage of what actually runs

| Runtime shaders | Count |
| --- | --- |
| Byte-identical to a shipped shader | 160 (107 PS, 53 VS) |
| Identical once `vfetch` fields are masked | 94 (VS patched at bind time) |
| **Matched** | **254 of 262 (97%)** |
| Not found | 8 (5 VS, 3 PS), all 9-54 dwords |

The 8 are tiny and probably D3D or XUI built-ins inside the XEX, which is encrypted and was not scanned. `global.resblock`, which is 73 MB and evicted to iCloud, was not scanned either.

Translation must therefore be keyed by container, not by the runtime ucode hash, since D3D patches the vertex shaders. `CreateVertexShader` receives the container, so a hook can bind it to the precompiled entry.

### What the shaders use

Over the 384 shipped shaders. Instruction counts are per shader.

| Feature | VS (218) | PS (166) | XenosRecomp today |
| --- | --- | --- | --- |
| Median, p90, max instructions | 47, 102, 157 | 30, 96, 121 | fine |
| Memory export | 0 | 0 | not implemented, not needed |
| Loops (integer constants) | 6 | 3 | **not implemented** |
| Conditional exec on bools | 157 | 51 | supported |
| `a0`-relative constants (skinning) | 65 | 8 | partly; known issues with multiple operands |
| aL-relative register source bit | 32 | 32 | **not implemented**; count needs checking, as it exceeds the loop count |
| Vertex texture fetch | 102 | - | texture path, untested here |
| Explicit LOD fetch | 33 | 4 | **not supported** |
| Fetch offsets | 23 | 30 | partly |
| Cube | - | 47 (48 use `cube`) | supported with a trick |
| 1D | - | 4 | **not implemented** |
| 3D | - | 1 | supported |
| `kill` | - | 7 | supported |
| Depth export | - | 4 | supported |
| Mini `vfetch` | 52 (136 at runtime) | - | **not implemented as such**; relies on declarations |

There is almost no dynamic control flow: one PS jump across the shipped set. `shader_particles_gpu.fx` accounts for 36 entries in `vfx_shaders.lst` and does not use memexport, which fits the high vertex texture fetch count.

Why this matters for frame time: the emulator's translated shaders reached 146-254 registers before the contraction fix. These are short, plain D3D9-era shaders, and a flat translation should land far below that. That is a hypothesis until measured, and the trial below measures it.

## 4. How Unleashed Recompiled did it

### API layer

- `UnleashedRecomp/gpu/video.cpp`, one 292 KB file, written fresh. It hooks 42 guest functions and stubs 15; another 19 guest functions are overridden for game-level behaviour.
- The hooks cover device creation, resources, locks, Present, render targets, Clear, `StretchRect`, viewport, scissor, textures, draws, vertex declarations and shaders, plus D3DX fill helpers and game functions such as `MakePictureData`, `SetResolution` and `ScreenShaderInit`. It is not purely API-level.
- `GuestDevice` mirrors the Xbox device block at fixed offsets (asserted at `0x5E00` bytes). The hook allocates it in guest memory.
- Its render and sampler state function tables are re-pointed at host handlers, because inlined game code calls through them.
- At draw time `FlushRenderStateForMainThread` reads `dirtyFlags`, then copies dirty float constants, bools and samplers into a queue. That is exactly the shadow-device approach proposed here.
- A dedicated render thread drains a blocking queue into plume, a D3D12/Vulkan/Metal abstraction (MIT). Guest threads never touch the GPU API.
- Present waits for the frame and advances resources over 2 frames in flight.

### Shaders

- XenosRecomp reads containers from the game files and emits HLSL, compiled with DXC to DXIL and SPIR-V, embedded in the binary and found by XXH3 of the whole container.
- Constants sit in big buffers mirroring the guest registers. Textures are bindless.
- Vertex declarations become native input layouts instead of `vfetch` in shader.
- Alpha test and `R11G11B10` normals are specialisation constants, and pipelines compile during loading on background threads.
- Gaps it does not cover are listed in the table above. The author warns it will not work out of the box, and game-specific assumptions sit under a macro.

### Render targets and resolves

- `SetRenderTarget` and friends only queue state. `StretchRect` records a pending copy and links the destination texture to the source surface. Copies run before the next draw or clear, and redundant ones are detected and skipped.
- MSAA resolves use hardware resolve where possible, else shaders with 2, 4 or 8 samples.
- A comment records that depth textures are transient in that game, so their copies are skipped. That is a game-specific shortcut Crackdown will need its own version of.

### What they could not avoid

- Endian swaps of vertex data, with swizzle fixes for 16-bit formats.
- Manual `R11G11B10` unpack.
- Quad list and triangle fan index generation.
- Half-pixel offset in the vertex shader.
- Alpha test emulation.
- A cube map trick that relies on the optimiser.
- Xbox colour correction at present.
- Replacing some guest shaders with hand-written ones.

They avoided EDRAM, PM4 and memexport entirely. Its hook list has no tiling or query entries, and the code reviewed shows no handling for either, so those two have no precedent here.

### Metal status

- [UnleashedRecomp PR 1648](https://github.com/hedge-dev/UnleashedRecomp/pull/1648), by squidbus: native Metal, default on macOS, +1232/-283 across 76 files on top of plume's Metal backend. Tested with a full play-through and refreshed 2026-09-21, but **still open**. It recommends macOS 15 or later for best performance.
- [XenosRecomp PR 22](https://github.com/hedge-dev/XenosRecomp/pull/22): MSL output, +1090/-278, **open**, backported from MarathonRecomp's fork. It generates code valid as both HLSL and MSL and compiles to Metal AIR with `xcrun metal`, tested with both games.
- No M1 frame rates were found for it, so there is no public number to anchor expectations.
- [catherine-macos](https://github.com/ammocache/catherine-macos), another ReXGlue title on Mac, reaches the same conclusion: it blames EDRAM's many small passes and is exploring a native renderer with ahead-of-time shaders.

### Licences

XenonRecomp, XenosRecomp and plume are MIT. UnleashedRecomp is GPL-3.0, and this repo's licence file is GPL-2. Do not copy `video.cpp` code here without checking compatibility; study its design and use the MIT parts freely. Compiled shaders derive from the game, so like the generated sources they stay local and are built from the player's disc.

## 5. Phased plan

Effort is in focused working sessions and is a rough estimate with wide error bars. Each phase keeps the emulator working behind a runtime flag: when native rendering is off, hooks call `original(ctx, base)`. Hook sites go through `crackdown::Layout()` so that TU6 can follow; the D3D functions mapped so far have identical code there.

### Phase 0: Census (1-2 sessions) — recommended first milestone, part 1

Add `--crackdown_d3d_census` (default off), using the existing hook machinery in observe-only mode. For one benchmark window at Los Muertos and one garage window, log per frame:

- draws, by entry point and by tiled or untiled, and how many are tile replays;
- tile counts and rects;
- resolves: source, destination, format, size, and whether the CPU reads them;
- texture formats, tiling and dimensions, and where each texture came from (`CreateTexture` or streamed header);
- `826C3750` use, queries and locks.

**Done:** one JSON report per scene. It also serves the emulator work today: real numbers on which resolves and transfers dominate.

### Phase 1: Offline shader trial (3-5 sessions) — recommended first milestone, part 2

**Done:** see [Shader trial](#shader-trial-2026-10-31).

Build XenosRecomp with PR 22 on this Mac, as a tool outside the game build. Point it at the four shipped files and fix container differences (these shaders come from XDK 4802, older than Unleashed's).

Measure with the existing Metal harness (`experiments/menu-bench/harness/`):

- how many of the 384 shaders translate and compile;
- register counts and time per full-screen draw, against the emulator's MSL for the same shaders, including `B20B230BE9FD2568`.

**Done:** a table per shader, and a list of required XenosRecomp changes. **Gate:** if under about 70% translate unaided, or the gaps need a rewrite, write an MSL backend on the SDK's `ShaderTranslator` instead, which already parses every op.

The output helps the emulator too: the per-shader gap shows how much cost is still emulation overhead in the SPIR-V translator, and which patterns are worth backporting.

### Phase 2: Native vertical slice (6-10 sessions)

**Started:** the [baked buffer decoder](#baked-buffer-decoder-2026-10-31) runs live behind a setting, the [texture change test](#texture-change-test-2026-10-31) passed, and [shadow mode](#shadow-mode-2026-10-31) draws [the main scene in grey](#first-picture-2026-10-31) with real vertex shaders. [Every pass](#every-pass-2026-10-31) draws to its own target, a [free camera](#free-camera-2026-10-31) can test any spot, and [direct draws](#direct-draws-2026-11-01) are in. Next: real pixel shaders and textures.

- Metal device, render thread and command queue. Choose plume's Metal backend or direct Metal with a small spike; direct is likely simpler for one game on one API.
- Shadow-device reader, texture path (reusing SDK untile and format code), no-copy guest memory, vertex pulling or copy-and-swap decided by a spike.
- **Shadow mode:** the emulator keeps presenting while the native layer renders the same frame to a debug window. Compare with screenshots.

**Done:** menus and XUI render correctly in shadow mode. **Gate:** the texture path is the real test. If invalidation needs per-frame page scans that cost as much as the emulator's, stop and rethink.

### Phase 3: Takeover in simple scenes (8-15 sessions)

Hooks stop the guest D3D from emitting packets. Native draw, clear, resolve, tiling collapse, swap, fences and vblank pacing. Occlusion queries in pass. Re-home what depends on the current path:

- `src/frame_pacing.cpp`, `src/frame_rate.cpp` (swap hooks)
- `src/aspect_render.cpp`, `src/internal_render_scale.h`, `src/multisampling.h`
- `src/benchmark.cpp` (frame count source)

**Done:** the garage runs at correct image with the PM4 path idle, and the benchmark runs native.

### Phase 4: The city (10-20 sessions)

Shadows (`shader_tsm*`), water, GPU particles, SpeedTree, crowds, post-processing. CPU-read resolves. Parity pass against the emulator and against Xbox footage. Metal binary archive for pipelines. Native becomes the default on macOS when parity holds; the emulator stays as a fallback flag.

**Done:** stable 30 on the M1 Air in the benchmark scene, image parity on Bruce's play-test.

### Phase 5: Beyond parity (open-ended)

60+ FPS, higher internal resolution, MetalFX. Possibly a Windows and Linux port of the layer, which is a separate decision.

### Effort summary

| Phase | Sessions | New code (rough) |
| --- | --- | --- |
| 0 Census | 1-2 | 300 lines |
| 1 Shader trial | 3-5 | tool patches |
| 2 Slice | 6-10 | 4-6k lines |
| 3 Takeover | 8-15 | 3-5k lines |
| 4 City | 10-20 | 2-4k lines, plus shader fixes |
| **Total to parity** | **about 30-50** | **about 10-15k lines** |

For comparison, the Vulkan backend is 22,598 lines before the SPIR-V translator (about 12,400 more) and common code. Level 3 replaces far more than it adds.

### Why not the alternatives first

- **Ahead-of-time MSL for the emulator.** Hitches are already gone after first visit, and it would not raise steady frame rate, because the ceiling is the command thread, not shader compilation. XenosRecomp's output also cannot plug into the emulator's binding model. A cheap variant does exist: MoltenVK accepts compiled MSL through `kMVKMagicNumberMSLCompiledCode`, and since the shader set is known, shipped shaders could be precompiled before first play. Worth doing only if first-visit hitches become the top complaint.
- **Native Metal backend for the emulator (level 2).** It would remove MoltenVK encoding and let guest memory be shared, both real gains. But it means rewriting about 22,000 lines while keeping PM4 decode, EDRAM emulation and tiling, and most of that code does not survive into level 3. Only the Metal foundation and texture work carry over, and Phase 2 builds those anyway. Revisit it only if Phase 0 or 2 finds a blocker.

### Where the time goes now, and what level 3 removes

| Cost today | Level 3 |
| --- | --- |
| Guest D3D building PM4 on the game thread | gone |
| Command thread ~76% busy; `IssueDraw` ~37%, MoltenVK encoding ~18-38% | replaced by a thin render thread |
| ~980 Metal passes per frame (before later trims) | a few dozen, estimated |
| Uploads, EDRAM transfers, tile replays | gone or much reduced |
| GPU 19.3 ms for a 6,207-draw frame | should fall; to be measured |

No frame rate is promised here. CPU encoding was the risk to watch, and the [encoding test](#encoding-test-2026-10-31) puts Metal recording at about 7 ms a frame on one thread alongside a simulated game load. What's left to prove is the layer's own per-draw work.

## Open questions

Only a live run or a closer read can settle these. The census answered the first four; see [Census results](#census-results-2026-10-31).

1. Tile count per mode and how many draws are replays.
2. Which resolves the CPU reads, and how often.
3. Texture format census, and whether streamed textures ever pass through a hookable path.
4. Whether `826C3750` is precompiled command buffer replay in regular use. If it is frequent and the buffers hold raw PM4, a replay path must decode them, which is the main way the clean-boundary finding could be wrong.
5. Whether `826C4C70` is reliable as a dirty signal.
6. The aL-relative count in section 3, which looks high against the loop count. Check it with the SDK disassembler.
7. Whether XenosRecomp's container parsing holds for XDK 4802 shaders. The header fields line up, but nothing has been run.
8. The block at `0x82780000`-`0x82900000` (3,192 functions) calls neither shader creation function directly and was not identified. No runtime shader compilation was seen, though indirect calls were not traced.
9. Whether Metal's programmable sample positions are needed for visual parity at 2x.

## Sources

- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp): README, `main.cpp`, `shader.h`, `shader_recompiler.cpp`
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp): README
- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp): README, `gpu/video.cpp`, `gpu/video.h`
- [UnleashedRecomp PR 1648](https://github.com/hedge-dev/UnleashedRecomp/pull/1648) and [XenosRecomp PR 22](https://github.com/hedge-dev/XenosRecomp/pull/22)
- [plume](https://github.com/renderbag/plume)
- [catherine-macos](https://github.com/ammocache/catherine-macos)
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK): `mvk_private_api.h`, `MVKShaderModule.mm`
- This repo: `HANDOFF.md`, `DECISIONS.md`, `config/tu6/address-map.csv`, `config/crackdown.toml`, `src/gpu_fence_wait.cpp`, `src/frame_pacing.cpp`, `src/frame_rate.cpp`, `generated/default/`
- SDK: `src/graphics/command_processor.cpp`, `src/graphics/vulkan/`, `include/rex/graphics/format/ucode.h`, `include/rex/graphics/vulkan/pipeline_cache.h`
