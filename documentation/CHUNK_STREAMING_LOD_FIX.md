# Chunk LOD and streaming hitch mitigation

## Findings from the 20-chunk capture

The supplied F3 capture reported approximately 34.43 ms for the asynchronous GPU chunk pass,
against 2.01 ms CPU update and 5.38 ms CPU render. The visible scene contained about 1.46M
triangles and 2,173 visible sections. This points to a GPU-heavy chunk pass in that capture;
it does not by itself identify the cause of every hitch while new terrain is being streamed.

The previous coarse LOD proxy merged a 2x2x2 or 4x4x4 voxel volume into one representative
block when only a quarter of the volume was filled. Thin terrain layers and underwater slopes
could therefore become artificially thick columns. The new proxy performs horizontal X/Z
reduction independently for each Y layer and keeps vertical resolution exact. The outer X/Z
border remains exact, and the greedy compute shader now uses independent tangent strides so
that vertical faces and layer heights are not collapsed.

## Streaming changes

- `load_around()` builds and sorts the column window only when the center, radius, vertical
  window, or generation changes. Subsequent queue-refill calls drain a persistent backlog
  rather than re-scanning the full 41x41 area at 20 chunks of radius on every refill.
- In-flight chunk requests are capped based on hardware concurrency, with only a small
  speed bonus. Fast movement no longer grows the speculative queue by one request per block/s.
- Completed-chunk handoff is limited to small bursts and a short time budget. Dirty mesh work
  and background LOD rebuilds are throttled, but visible LOD transitions are now scheduled
  independently of whether generation/streaming queues have drained.
- Liquid/source discovery reads neighbors from the unpacked local chunk buffer for in-chunk
  neighbors and uses world lookups only at chunk borders.
- GPU mesh upload batches are capped at 4 MiB / 64 chunks, and shrink to 512 KiB / 8 chunks
  when the previous asynchronous GPU timer exceeds 28 ms (with intermediate budgets at 14/20 ms).

## Conservative Hi-Z

The Hi-Z test still requires an unchanged camera, viewport and resident geometry signature.
Its projected rectangle is expanded by two source pixels and the depth comparison uses a
larger bias. These changes can reduce culling efficiency slightly, but favour retaining
borderline visible sections over false occlusion.

## Diagnostics

F3+F4 now reports queued generation jobs, queued mesh jobs and remaining column-backlog count
in addition to requested chunks and pending mesh rebuilds. This helps distinguish CPU streaming
backlog from a GPU-heavy chunk pass.

## Validation status

The source diff passes `git diff --check`. LOD sample-coordinate checks confirm that strides 1,
2 and 4 tile exactly 32 horizontal blocks and preserve the exact chunk border. A backlog
simulation at radius 20 scheduled all 15,129 chunk requests across 1,681 columns exactly once.
A full native CMake build and GLSL driver compilation were not possible in the editing
container because OpenGL development headers/libraries, GLM headers and a GLSL compiler are
not installed. Runtime performance and visuals still need to be verified on the target GPU.

## Follow-up after the fast-flight capture

The next F3 capture (6 FPS, 143.25 ms frame; 135.28 ms CPU update; 19.29 ms CPU render;
51.35 ms asynchronous GPU chunk pass; 96 load requests, 33 generation jobs and 363 remaining
columns) shows that streaming/update work had become the largest CPU-side stall, while the GPU
chunk pass remained a separate bottleneck. The following changes address the main-thread work:

- Active gameplay previously collected completed chunks and meshes at the start of
  `update_gameplay()` and then collected them a second time inside `update_player_position()`.
  The loading-screen pump had the same duplication. Active/loading frames now collect once;
  paused frames still collect at the gameplay entry point because player-position updates are
  intentionally skipped while paused.
- The completed-chunk apply cap used to grow with player speed. At creative-flight speeds near
  198 blocks/s this could apply up to 18 chunks in a single frame. It now decreases to two
  completed chunks per frame at high speed, three at medium speed and six while moving slowly;
  worker queues continue processing and nearer work retains priority.
- Per-chunk liquid/source discovery no longer allocates and scans a 65,536-entry temporary
  vector for each arrival, nor calls `get_block_props()` repeatedly for every neighboring water
  voxel. It uses precomputed ID-indexed liquid, passability and tick-delay tables.
- Sky-light recomputation no longer waits for every requested Y chunk in a column to finish.
  It recalculates the loaded part of the column immediately, and a later chunk arrival queues the
  same column again to correct shadows. This prevents surface water columns from remaining at
  zero sky light during continuous streaming.
- Sky-light transmission has its own table: every liquid state passes direct sky light even if a
  data-driven definition changes its visual transparency/opacity flags. Water still receives the
  normal day/night and weather ambient multiplier; this deliberately preserves the existing
  0.32 minimum scene-light floor rather than globally brightening the world.
- Sky-column work is nearest-first and limited to a small per-frame budget; block-light propagation
  processes at most eight newly arrived chunks per update instead of consuming the entire backlog
  in one burst.

- Skylight no longer clears every loaded cell and marks every chunk dirty before restoring the
  computed result. It writes only the final value (zero for opaque blocks, 15 for unshadowed
  transparent/liquid cells, zero inside the configured shadow reach) and marks a chunk dirty
  only when that final value differs from the previous value.

The new capture is not a runtime test of these changes. Native CMake configuration remains
blocked in this container by missing OpenGL/GLX and GLM development dependencies, and no target
GPU/driver is available for visual validation.

## 0.10.2: fast-flight LOD and GPU residency priority

The previous `lod_streaming_idle` gate made a visible chunk wait for the entire streaming wave
and almost-empty mesh queue before scheduling its next LOD stride. During fast flight that
condition could remain false for a long time. LOD transitions are now considered every renderable
chunk scan and a small nearest-first batch is enqueued per frame. They use a separate priority
tier: visible LOD replacements can finish before speculative first meshes several rings away,
while the current ring's first meshes remain near the front of the queue. The old GPU mesh stays
visible until the replacement has been applied and uploaded.

Mesh priorities use the signed vector from the player to the chunk and the smoothed movement
velocity. Chunks ahead of travel receive a smaller distance penalty; chunks behind do not get
that bonus. Pending tasks and completed results are reprioritized on chunk transitions, and are
refreshed every 120 ms during fast movement even when the player remains inside the same chunk.
The completed-result queue uses coarser buckets to reduce empty-bucket scans during frequent
main-thread collection.

The renderer places chunks that have no GPU handle ahead of background mesh replacements in
the upload batch, preserving near-to-far order within each group. The compute mesher and the
GPU-generated culling/draw commands are submitted in-order on the rendering context, allowing a
newly uploaded chunk to become eligible in that same frame without a CPU readback wait.

## 0.10.2: appearance and world lighting

Newly GPU-resident chunks reveal block-by-block over approximately 0.42 seconds. The effect is
a stable fragment-shader dissolve, not a geometry scale/translation, so adjacent chunk borders
never separate during the animation. A light distance fog fades toward the current sky and
weather color; its range follows the configured render distance. Directional sun light adds a
subtle daytime face contrast and fades near the horizon, while the existing 0.32 scene-light
floor is preserved.
