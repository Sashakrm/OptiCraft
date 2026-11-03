# OptiCraft rendering pipeline: GPU-driven culling, mesh shading and CPU workers

## GPU mesh construction and drawing

OptiCraft keeps its OpenGL 4.3 compute mesher. `mesh_greedy.comp` and `mesh_misc.comp`
consume the compact chunk payload and write 16-byte `uvec4` quad records into the shared GPU
pool. The quad layout, atlas lookup, AO, corner light, water animation inputs and flora data
remain compatible with the existing fragment shaders.

At initialization the renderer tries `GL_EXT_mesh_shader`, then `GL_NV_mesh_shader`.
The EXT pipeline uses `mesh_task.task`, `terrain_mesh.mesh` and `terrain_misc_mesh.mesh`;
the NVIDIA pipeline uses the corresponding `_nv` files. The entry points are loaded at
runtime because the project's GLAD baseline is OpenGL 4.3. If neither extension is usable,
rendering falls back to `glMultiDrawArraysIndirect` and the original vertex-pulling shaders.

## GPU-driven frame preparation

CPU frame preparation no longer rejects chunks by the camera frustum and no longer assembles
per-section indirect commands. It sends the GPU only:

- resident chunk handles;
- the section reachability mask produced by the world/portal connectivity traversal;
- section Y bounds and per-section geometry kinds;
- small per-frame flags (for example the flora distance limit).

`gpu_cull_commands.comp` performs the camera-frustum test for each section AABB, optionally
performs temporal Hi-Z occlusion for opaque sections, reverses the transparent candidate order,
and writes the four per-kind order lists plus indirect commands. The draw path consumes those
SSBOs directly with `glMultiDrawArraysIndirect` or `glMultiDrawMeshTasksIndirectEXT/NV`.
There is no CPU readback of the visible lists or commands.

The Hi-Z pyramid is built on the GPU from the opaque depth buffer. It uses maximum-depth
reduction: a tile can reject an opaque section only when every covered depth sample is closer
than the nearest point of the section bounds. Clear pixels remain depth 1.0, so holes do not
become occluders. The pyramid is reused only when the view-projection matrix, viewport and
resident mesh/reachability signature match the frame that produced it. Camera motion, viewport
changes, mesh uploads or candidate changes invalidate it. This intentionally conservative
policy sacrifices some culling opportunities rather than risking stale-depth popping. The
legacy `GL_ANY_SAMPLES_PASSED` query path remains optional and is disabled by default.

## What still belongs to the CPU

The renderer is GPU-driven after candidate upload, but it is not a fully autonomous GPU world
engine. CPU work remains for world simulation, chunk residency and streaming, block/palette/light
payload packing, section connectivity/reachability, mesh-version bookkeeping, prioritizing the
resident candidate list and issuing OpenGL commands. Chunk generation remains in the existing
Chunk_Manager workers. The renderer's persistent CPU worker pool parallelizes distance
calculation and stable sorting; it never calls OpenGL from background threads. `load_around()`
keeps a persistent sorted column backlog, so freeing in-flight slots drains the backlog instead
of rebuilding and sorting the full render-distance square each time. In-flight requests, main-thread
chunk handoff, dirty remeshing and GPU uploads have smaller budgets; upload batches shrink further
when the previous asynchronous GPU timer reports an overloaded chunk pass.

Dynamic distance LOD is selected by `Chunk_Manager::desired_lod_stride()` with a one-ring
hysteresis (strides 1/2/4). It is rebuilt asynchronously; the old mesh remains resident until
the replacement has completed and been uploaded. The coarse block proxy reduces only X/Z
resolution and keeps Y exact. Thus hills, cave ceilings/floors and sea-floor height are not
expanded vertically. The outer X/Z border remains full-resolution, and the GPU greedy mesh
uses axis-specific sampling strides so that the horizontal LOD does not merge vertical layers.
LOD rebuilds remain throttled per frame, but are no longer suspended until all generation and
meshing queues become idle. Their jobs and completed results are prioritised independently from
speculative background remeshes, using current distance and travel direction.

## Appearance, fog and lighting

A chunk records its first GPU-residency time in the otherwise-unused `Origins.w` component.
Both the classic vertex-pulling and EXT/NV mesh-shader paths pass an eased appearance factor to
the fragment shader. A stable voxel-space dissolve reveals complete block cells over about
0.42 seconds. The vertices themselves are not scaled or translated; chunk borders, collision
coordinates and shared surfaces therefore remain aligned during the effect. Rebuilding a mesh
already resident on the GPU does not restart the animation.

World fragments blend toward the current sky/weather color over a fog interval derived from the
configured render distance. The same fog and lighting uniforms are bound in the classic and
EXT/NV shader paths. Directional sunlight contributes a soft diffuse term during daylight and
fades at sunrise/sunset; night lighting continues to use the existing day/night ambient and
block-light model. The scene-light floor of 0.32 is intentionally unchanged.

On each frame, newly resident meshes are stable-partitioned ahead of background replacements
before GPU upload. Each group retains front-to-back order. Compute meshing, the GPU culling pass
and indirect draw preparation are submitted to the same OpenGL command stream, so there is no
CPU readback round-trip between a chunk's compute build and its first eligible draw.

This is still one active representation per chunk rather than simultaneous GPU-resident LOD
levels. It is a safe distance-selected mesh rebuild, not a zero-cost GPU LOD switch.

## Context and validation

Window creation tries OpenGL 4.6, then 4.5, then 4.3 core. Compute meshing, GPU frustum/Hi-Z
culling and indirect commands require OpenGL 4.3. EXT/NV mesh shading is optional. The source
archive was statically reviewed, but this environment does not have the OpenGL development
libraries/headers or a GLSL compiler, so a full native build and driver-side shader validation
must still be performed on a machine with the project's graphics dependencies.
