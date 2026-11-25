# Piccolo Engine AI-Native Phase 3

## Overview

Phase 3 delivers two things:

1. **Headless mode** — the engine starts and runs its full logic loop (physics, AI, input injection, agent commands) without opening a display window or initialising a Vulkan renderer.
2. **Live reflection schema** — `get_capabilities` now enumerates every type registered in the engine's reflection registry and returns their field names and types as a machine-readable JSON schema, in addition to the hand-written `component_schema.json`.

## Files Changed

### New

| File | Purpose |
|------|---------|
| `docs/agent/phase3.md` | This document |

### Modified

| File | Change |
|------|--------|
| `engine/source/runtime/engine.h` | Added `extern bool g_is_headless`; added `runHeadless(int max_ticks = -1)` |
| `engine/source/runtime/engine.cpp` | Defined `g_is_headless`; implemented `runHeadless()`; guarded all render/window calls in `tickOneFrame()` behind `if (!g_is_headless)` |
| `engine/source/runtime/function/global/global_context.cpp` | Skips `WindowSystem`, `RenderSystem`, `DebugDrawManager` init when `g_is_headless`; also skips `InputSystem::initialize()` (which calls GLFW `registerOn…` hooks); null-guards `RenderSystem::clear()` on shutdown |
| `engine/source/runtime/function/framework/component/mesh/mesh_component.cpp` | Guards render-swap upload with `if (g_runtime_global_context.m_render_system)` |
| `engine/source/runtime/function/framework/component/particle/particle_component.cpp` | Same null-guard; early-returns if no render system |
| `engine/source/runtime/function/framework/component/camera/camera_component.cpp` | `postLoadResource` guards FOV swap-data push; `tick()` early-returns when no render system |
| `engine/source/runtime/function/input/input_system.cpp` | `calculateCursorDeltaAngles()` returns early when window or render system is absent |
| `engine/source/runtime/core/meta/reflection/reflection.h` | Added `static std::vector<std::string> getAllTypeNames()` to `TypeMetaRegisterinterface` |
| `engine/source/runtime/core/meta/reflection/reflection.cpp` | Implemented `getAllTypeNames()` — iterates `m_class_map` |
| `engine/source/runtime/function/agent/agent_command_system.cpp` | `cmdGetCapabilities()` now builds a live reflection schema from `getAllTypeNames()` + `getFieldsList()` and merges it with the static `component_schema.json` |
| `engine/source/editor/source/main.cpp` | Parses `--headless [max_ticks]` and `--config <path>`; sets `g_is_headless` and calls `runHeadless()` instead of creating `PiccoloEditor` |

## How to Use

### Run in headless mode

```bash
# Run indefinitely (agent controls shutdown)
./build/engine/source/editor/PiccoloEditor --headless

# Run for exactly 600 ticks (~10 s at 60 Hz), then exit
./build/engine/source/editor/PiccoloEditor --headless 600

# Run headless with a non-default config
./build/engine/source/editor/PiccoloEditor --headless --config /path/to/PiccoloEditor.ini
```

The MCP server still starts on port 7777. The Python harness connects and uses
`step`, `set_input`, `query_objects` etc. exactly as before.

### Live reflection schema via `get_capabilities`

```python
from scripts.piccolo_client import PiccoloClient
client = PiccoloClient()
result = client.get_capabilities()

schema = result.payload
# schema["live_schema"]      -> all registered C++ types: {TypeName: {fields: {name: {type, is_array}}}}
# schema["component_schema"] -> hand-written descriptions from component_schema.json
# schema["type_count"]       -> total number of registered types
```

Example fragment of `live_schema`:

```json
{
  "MotorComponent": {
    "fields": {
      "m_move_speed":           {"type": "float", "is_array": false},
      "m_jump_height":          {"type": "float", "is_array": false},
      "m_max_move_speed_ratio": {"type": "float", "is_array": false}
    },
    "type_count": 6
  },
  "TransformComponent": {
    "fields": {
      "m_transform": {"type": "Transform", "is_array": false}
    },
    "type_count": 1
  }
}
```

The LLM planner in `agent_planner.py` can call `get_capabilities` once at the
start of a session to populate its context with accurate field names, eliminating
hallucinated field paths.

## Design Notes

### Why null-guard rather than a stub render system

The cleanest headless approach is to simply not allocate `WindowSystem` and
`RenderSystem` and to null-check every access point. A stub render system would
silently swallow data; a null check crashes fast if a code path is missed.
Four access points were found and guarded:
- `MeshComponent::tick` — mesh transform upload
- `ParticleComponent::tick` — particle emitter tick
- `CameraComponent::postLoadResource` and `tick` — camera view-matrix upload
- `InputSystem::calculateCursorDeltaAngles` — FOV-based cursor sensitivity

### Why a fixed delta-time in `runHeadless`

`calculateDeltaTime()` measures wall-clock time between ticks, which in headless
mode can be near-zero (no vsync) causing physics instability or zero-delta-time
bugs. `runHeadless` uses a fixed 1/60 s dt regardless of wall-clock speed.

### `g_is_headless` global

Set before `startEngine()` so `startSystems()` can branch on it.
Declared in `engine.h`, defined in `engine.cpp`, and read by `global_context.cpp`
and any component that needs to skip render-only paths.

## Build Status

```
PiccoloRuntime: compiles cleanly (pre-existing macro warning unrelated)
PiccoloEditor:  compiles cleanly (pre-existing Lua tmpnam warning unrelated)
```

## Acceptance Criteria Status

| Criterion | Status |
|-----------|--------|
| Engine starts and runs without a display window | ✓ `--headless` flag; Window/Render not initialized |
| Agent can enumerate all reflected types and fields in machine-readable JSON | ✓ `get_capabilities` returns `live_schema` from registry |
