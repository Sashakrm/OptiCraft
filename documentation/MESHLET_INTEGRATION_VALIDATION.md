# Meshlet Renderer integration — validation report (OptiCraft 0.10.6)

## Scope

The experimental GPU Meshlet mode is integrated into the current OptiCraft renderer. It reuses the current GPU greedy mesher and quad pool; it does not replace `Chunk_Manager`, world generation, lighting, or the legacy renderer. Standard mode remains available for A/B comparison.

## Implemented path

- Persistent 48-byte AABB descriptors, one per 32-quad pool block.
- Separate bound decoding for greedy solid quads and shaped box records.
- Per-meshlet frustum and existing temporal Hi-Z culling.
- Fixed-count MDI and optional compact MDI via runtime-checked `glMultiDrawArraysIndirectCount`.
- Legacy asynchronous per-section occlusion-query results are propagated to the Meshlet culler.
- Opaque Meshlet command preparation happens before legacy chunk culling. On allocation or range-validation failure, Standard mode is restored before the legacy culler emits this frame's opaque commands.
- Compute dispatches are split using `GL_MAX_COMPUTE_WORK_GROUP_COUNT.x` and base-offset uniforms.
- Solid and shaped geometry use the current vertex-pulling programs. Flora and liquid remain on their existing renderer path.
- The Options UI reads the actual renderer state; if the renderer falls back after a runtime allocation/capacity failure, the saved option does not remain stale.

## Automated checks completed

Run these from the project root on Linux with a working surfaceless EGL/OpenGL runtime:

```bash
python3 tools/meshlet_validation/check_glsl_link_egl.py
python3 tools/meshlet_validation/test_meshlet_compute.py
```

Results in this environment:

- `git diff --check`: passed.
- Russian and English localization JSON parsing: passed.
- Surfaceless Mesa 25.0.7 OpenGL 4.5 context: passed.
- Shader compile/link: `gpu_build_meshlets.comp`, `gpu_cull_meshlets.comp`, `gpu_cull_commands.comp`, `terrain_gpu_vertex.glsl + terrain_gpu_fragment.glsl`, `terrain_gpu_misc_vertex.glsl + terrain_gpu_misc_fragment.glsl`: passed.
- Synthetic GPU dispatch: solid AABB `(0,0,0.998)..(1,1,1.002)` passed.
- Synthetic GPU dispatch: shaped AABB `(1,2,3)..(1.5,2.5,3.5)` from the engine's packed 1/64 box coordinates passed.
- Fixed MDI output for solid and shaped streams: passed.
- Compact MDI command counts and command contents: passed.
- Legacy occlusion-query rejection in fixed and compact modes: passed.

## Not verified in this environment

- Full C++ project build: CMake configuration stops at `FindOpenGL` because OpenGL development headers/libraries are unavailable in this environment. Compilation of the complete game therefore cannot be claimed.
- Real OptiCraft gameplay on Intel UHD: not run here.
- FPS improvement: not measured. The Meshlet path may reduce vertex work in scenes with substantial partial occlusion, but stream uploads and additional culling/command work can cost more in open scenes. A/B measurement on the target GPU is required.

## A/B procedure

1. Use the same world, seed, camera route, resolution, render distance, graphics options and weather.
2. Wait for generation and remesh to settle; discard the first activation frame because Meshlet bounds may be built on mode enable.
3. Record Standard and GPU Meshlet runs in alternating order.
4. Compare CPU/GPU frame-time p50/p95/p99, not only average FPS.
5. Include open terrain, dense forests, mountains with occlusion, caves, shaped blocks, fast flight/chunk streaming, and water/flora.
6. Keep the GPU/driver version and render settings in the result log.
