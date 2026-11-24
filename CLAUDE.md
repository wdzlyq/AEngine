# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Configure (run once, or after adding/removing .cpp files)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# Build everything
cmake --build build -j$(nproc)

# Build specific targets
cmake --build build --target PiccoloRuntime -j$(nproc)
cmake --build build --target PiccoloEditor  -j$(nproc)

# Linux convenience script (debug / release)
./build_linux.sh debug
./build_linux.sh release
```

Output binary: `build/engine/source/editor/PiccoloEditor`

CMakeLists uses `GLOB_RECURSE` for all `.cpp`/`.h` under `engine/source/runtime/` and `engine/source/editor/`. Adding or removing source files requires re-running cmake configure.

No automated test suite exists. Manual testing is done by running `PiccoloEditor`.

## Repo Layout

```
engine/
  source/
    runtime/          # PiccoloRuntime static library
      core/           # math, logging, reflection macros
      function/       # all subsystems (render, animation, physics, …)
      resource/       # asset types and serialization
    editor/           # PiccoloEditor executable (links PiccoloRuntime)
    meta_parser/      # code-gen tool that reads CLASS/STRUCT macros
    precompile/       # cmake hooks that run meta_parser at build time
  shader/glsl/        # GLSL source; compiled to SPIR-V at build time
  asset/              # game assets (levels, textures, meshes)
  3rdparty/           # vendored libs (JoltPhysics, VMA, GLFW, sol2, …)
docs/
  ai-native-research/ # architecture research for AI-agent integration
```

## Architecture

### Engine Loop
`PiccoloEngine::run()` (`engine.h/cpp`) drives a window-bound loop:
1. `logicalTick(dt)` → `WorldManager::tick()` + `InputSystem::tick()`
2. `RenderSystem::swapLogicRenderData()` — double-buffer logic↔render
3. `rendererTick(dt)` → `RenderSystem::tick()`

The loop exits when the GLFW window closes; there is no headless mode.

### Global Context
`g_runtime_global_context` (`function/global/global_context.h`) is the
singleton that owns every subsystem (`shared_ptr`): `LogSystem`,
`WorldManager`, `RenderSystem`, `PhysicsManager`, `ParticleManager`, etc.
All subsystems start in `startSystems()` and shut down in `shutdownSystems()`.

### World / Level / GObject
`WorldManager` owns the active `Level`. A `Level` holds a flat list of
`GObject`s. Each `GObject` owns a `vector<ReflectionPtr<Component>>`.
Components tick in `Level::tickAllObjects()`. Adding a component type
requires both a `.h/.cpp` and a reflection registration (see below).

### Reflection & Code Generation
The engine uses a custom reflection system. Source files annotate types with:
- `REFLECTION_TYPE(ClassName)` + `CLASS(ClassName, Fields)` or `WhiteListFields`
- `REFLECTION_BODY(ClassName)` inside the class body
- `META(Enable)` / `META(Disable)` to include/exclude individual fields

`meta_parser` reads these macros at build time and emits glue code under
`engine/source/runtime/core/meta/reflection/`. When adding a new reflected
type, trigger a full configure+build so the parser re-generates that glue.

### Render Pipeline
`RenderSystem` owns a `RenderPipelineBase` (default: deferred).
The pipeline runs ordered `RenderPass` subclasses:
`DirectionalLightShadowPass` → `PointLightShadowPass` → `MainCameraPass`
(which contains 8 Vulkan subpasses: gbuffer → deferred lighting → forward →
tone mapping → color grading → FXAA → UI → combine-UI) → `PickPass` →
`ParticlePass`.

The RHI abstraction lives in `function/render/interface/vulkan/vulkan_rhi.h`.
All Vulkan objects are owned there and released in `VulkanRHI::~VulkanRHI()`.

Data flows logic→render through `RenderSwapContext` (double-buffered swap
data): mesh/material uploads, particle emitter events, camera updates.

### Shader Workflow
GLSL sources are in `engine/shader/glsl/`. CMake compiles them to SPIR-V
(via `glslangValidator`) and embeds the byte arrays as C++ headers in
`build/engine/shader/generated/cpp/`. The pass `.cpp` files `#include`
those generated headers directly (e.g. `#include <color_grading_frag.h>`).
Changing a shader requires rebuilding; no hot-reload.

### Animation System
`Skeleton::applyAnimation()` samples `BlendStateWithClipData` (N clips,
per-bone weights) using the `AnimationPose` blending helpers in
`function/animation/pose.h`. State transitions are driven by
`AnimationFSM` (`function/animation/animation_FSM.h`).
`AnimationComponent::tick()` reads jump/move state from `MotorComponent`
to drive the FSM each frame.

### Physics
`PhysicsScene` wraps JoltPhysics. `CharacterController::move()` performs
two sweep passes (vertical then horizontal) using `PhysicsScene::sweep()`.
Only static rigid bodies are currently supported by the scene builder;
dynamic/kinematic support is a known gap (`physics_scene.cpp:119`).

## Known Gaps / Open TODOs
- `skeleton.cpp`: joint matrix units may be wrong (affects bone transform output)
- `skeleton_data.h`: non-topological-order skeletons are silently rejected in `buildSkeleton()`
- `RenderResource::clear()`: persistent mapped GPU buffers (`_global_upload_ringbuffer`, `_axis_inefficient_storage_buffer`) are never unmapped
- `physics_scene.cpp`: only static objects supported
- `vulkan_rhi.h`: `m_vk_current_command_buffer` declared but never assigned

## 3rdparty Build Notes
Two patches applied to vendored libraries to fix builds on GCC 13+:
- `JoltPhysics/Jolt/Core/Core.h`: added `#include <cstdint>`
- `vulkanmemoryallocator/include/vk_mem_alloc.h`: added `#include <cstdio>`
- `engine/3rdparty/CMakeLists.txt`: Jolt target gets `-Wno-error` to suppress new-warning build failures
