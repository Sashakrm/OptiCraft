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

## GPU-driven frame preparation and current-frame camera-ray visibility

CPU frame preparation now performs only a whole-chunk AABB/frustum test. It does not
reject individual sections using portal/air-connectivity reachability: that traversal could
incorrectly mark exposed mountain or water sections unreachable. CPU submits active GPU
handles and per-section metadata snapshots that belong to the currently committed COW mesh
generation.

The GPU performs a half-resolution opaque depth-only prepass into a separate depth target.
For every raster sample, hardware depth testing records the nearest opaque surface along
that camera-view ray. The compute pipeline copies this depth into a MAX Hi-Z pyramid. The
MAX reduction is conservative: a screen tile with a clear/far sample cannot prove that a
meshlet is hidden. Opaque section/meshlet AABBs are rejected only when their nearest depth
is behind every sample in the covered Hi-Z region. Water never consumes opaque Hi-Z and
keeps a fixed far-to-near command order.

The half-resolution ray-depth target is only a culling source. It is not the scene's final
depth buffer. The full-resolution colour pass returns to the main framebuffer and performs
its own normal `GL_LESS` depth test with depth writes enabled. This distinction matters:
using `GL_LEQUAL` with depth writes disabled after rendering depth into a separate FBO left
the main depth buffer unpopulated and allowed overlapping terrain/water to draw incorrectly.
The final full-resolution depth test resolves the exact visible pixels; Hi-Z only avoids
submitting fully hidden coarse bounds.

The current method is hardware raster/depth-based camera-ray visibility, not a separate
software ray marcher through every voxel. For a given screen sample, the depth test finds
the first opaque hit; objects fully behind proven occluders can be culled conservatively.
Water remains transparent and is not allowed to occlude later water commands through
opaque Hi-Z.

## Experimental per-meshlet rendering path (0.10.5)

An opt-in `GPU Meshlet` mode is layered over the existing quad pool. The compute greedy mesher and
its 16-byte quad format are unchanged. A second build pass writes one persistent world-space AABB
per 32-quad pool block; solid and shaped records use different decoders because `mesh_misc.comp`
stores shaped box coordinates in `rec.z/rec.w`, not in the greedy-face width/height bits. Liquid uses
a conservative whole-cell AABB with a small vertical animation margin. Descriptors are rebuilt only
for geometry uploads or once when the mode is first enabled.

A frame pass uploads records for resident solid/shaped/liquid streams, then one workgroup per stream checks
its Meshlets against the camera frustum and (for detailed opaque streams only) the current-frame
conservative Hi-Z pyramid. Coarse LOD streams skip Hi-Z rejection to avoid speckled holes. It writes `DrawArraysIndirectCommand` records directly into three command ranges (solid,
shaped and liquid); the liquid range is assembled back-to-front and is never query/Hi-Z culled. When the
indirect-count API is available, visible commands are appended with shared-memory aggregation and
the GPU also provides the count; otherwise fixed-count MDI submits zero-count commands for culled
Meshlets. The old chunk culler still owns flora and chunk-level stats, but skips writing duplicate
solid/shaped commands while Meshlet mode is active. The Meshlet stream record marks coarse LOD streams so they skip Hi-Z; the old asynchronous
per-section occlusion-query path is not used for gameplay visibility. Meshlet command preparation runs
before the legacy culler; if buffer allocation/capacity validation fails, Standard mode is restored
before the legacy opaque command lists are generated in that same frame.

The mode is toggled in in-world Options and persisted as `meshlet_renderer=0|1`. It uses classic
vertex-pulling shaders for solid, shaped and liquid MDI. Liquid has an independent command range,
keeps far-to-near chunk ordering, bypasses opaque Hi-Z/query rejection and retains alpha blending
without depth writes. Flora remains on the legacy path. Native EXT/NV task/mesh draws remain part of
the Standard mode and are not mixed with the Meshlet command stream. See
[`MESHLET_RENDERER.md`](MESHLET_RENDERER.md) for SSBO layouts, dispatch bindings, barriers, known
limits and the A/B test protocol.

## What still belongs to the CPU

The renderer is GPU-driven after candidate upload, but it is not a fully autonomous GPU world
engine. CPU work remains for world simulation, chunk residency and streaming, block/palette/light
payload packing, chunk-level frustum filtering, mesh-version bookkeeping, prioritizing the
resident candidate list and issuing OpenGL commands. Chunk generation remains in the existing
Chunk_Manager workers. The renderer's persistent CPU worker pool parallelizes distance
calculation and stable sorting; it never calls OpenGL from background threads. Geometry edits carry
an 8-block-high section mask through the mesh job and GPU upload. The greedy/misc compute shaders
skip untouched sections; their quad ranges, counters and Meshlet bounds remain resident. First
meshes, LOD transitions and buffer regrowth still rebuild the full chunk. A swap between two full
opaque cubes can avoid remeshing altogether when all six adjacent cells occlude it. `load_around()`
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
culling, Meshlet AABB construction and indirect commands require OpenGL 4.3. EXT/NV mesh shading
is optional and is used only by the Standard renderer path when the runtime extension and entry
points are present. In this development environment the new Compute programs and the existing
classic vertex/fragment pairs were compiled and linked on a surfaceless Mesa OpenGL 4.5 context;
synthetic dispatch tests checked solid/shaped AABBs and fixed/compact MDI output. A complete C++ game
build could not be confirmed because OpenGL/GLFW/GLM development dependencies are absent here.
No Intel UHD runtime performance result is claimed; the in-game A/B test must be performed on the
target machine.


The camera-ray visibility pass is a raster depth prepass, not a software voxel ray marcher: each covered screen sample stores the nearest opaque surface along its camera direction. It runs at half resolution; conservative MAX Hi-Z rejects only bounds proven behind every depth sample in the selected region. LOD streams and liquid skip Hi-Z rejection, while the final full-resolution depth test resolves pixel visibility.
