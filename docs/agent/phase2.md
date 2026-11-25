# Piccolo Engine AI-Native Phase 2

## Overview

Phase 2 delivers three things:

1. **`asset_catalog.json`** — a hand-curated catalog of every asset prefab in the engine, read by the Python harness to construct valid object-creation requests.
2. **`verifier.py`** — a behavioural assertion module that confirms character movement, jumping, and wall collision by driving the engine and inspecting runtime state.
3. **`agent_planner.py`** — a full LLM planning loop that takes a natural-language game goal, generates engine commands via Claude, executes them with snapshot/restore safety, and runs the verifier at the end.

Two Phase 1 stubs were also promoted to full implementations as part of this work:

- `cmdCreateObject` — now creates a GObject from a `.object.json` definition path and optionally sets its initial position.
- `cmdSaveLevel` / `save_level` tool — persists the in-memory level back to the JSON file on disk via `Level::save()`.

## Files Changed / Created

### New

| File | Purpose |
|------|---------|
| `docs/ai-native-research/asset_catalog.json` | Catalog of all 9 prefab types with components, fields, dimensions, and usage notes |
| `scripts/piccolo_client.py` | Clean HTTP/JSON-RPC 2.0 client for the MCP server (port 7777) |
| `scripts/verifier.py` | `Verifier` + `VerificationSuite` classes with 5 behavioural checks |
| `scripts/agent_planner.py` | `Planner` + `Executor` + `run_demo()` end-to-end entry point |
| `docs/agent/phase2.md` | This document |

### Modified

| File | Change |
|------|--------|
| `engine/source/runtime/function/agent/command_types.h` | Added `SaveLevel` to `CommandType`; added `definition` and `set_position` fields to `CreateObjectParams` |
| `engine/source/runtime/function/agent/agent_command_system.h` | Added `cmdSaveLevel()` declaration |
| `engine/source/runtime/function/agent/agent_command_system.cpp` | Replaced `cmdCreateObject` stub with real implementation; added `cmdSaveLevel` |
| `engine/source/runtime/function/agent/agent_mcp_server.cpp` | Added `save_level` dispatch + tool entry; updated `create_object` to accept `definition` field |

## Asset Catalog

Path: `docs/ai-native-research/asset_catalog.json`

Covers all 9 prefab types:

| ID | Definition | Type | Key capabilities |
|----|-----------|------|-----------------|
| `character/player` | `asset/objects/character/player/player.object.json` | character | move, jump, sprint, animate, camera-follow |
| `environment/wall` | `.../wall/wall.object.json` | environment | block-movement, block-sight |
| `environment/wall_block` | `.../wall/wall_block.object.json` | environment | block-movement (2×2×2 cube) |
| `environment/wall_with_window` | `.../wall/wall_with_window.object.json` | environment | partial block |
| `environment/wall_with_door` | `.../wall/wall_with_door.object.json` | environment | allows passage through doorway |
| `environment/fence` | `.../fence/fence.object.json` | environment | perimeter boundary |
| `environment/stairs` | `.../stairs/stairs.object.json` | environment | walkable slope |
| `environment/floor` | `.../floor/floor.object.json` | environment | ground plane (86×48 m) |
| `environment/particle` | `.../particle/particle.object.json` | effect | visual only |

Each entry records: components list, collision shape + half-extents, configurable fields with types/ranges, and usage notes.

## Python Modules

### piccolo_client.py

```python
from scripts.piccolo_client import PiccoloClient, InputCommand

client = PiccoloClient()           # connects to localhost:7777
state  = client.get_state()        # MCPResult with .success, .payload, .error
client.set_input(InputCommand.FORWARD, duration_frames=30)
result = client.step(30)           # advance 30 ticks, return new state
client.snapshot("save", tag="before_test")
client.snapshot("restore", tag="before_test")
client.create_object("MyWall", "asset/objects/environment/wall/wall.object.json",
                     position={"x": 5, "y": 0, "z": 0})
client.save_level()
```

All methods return `MCPResult(success, payload, error)`.

### verifier.py

```python
from scripts.piccolo_client import PiccoloClient
from scripts.verifier import Verifier, VerificationSuite

client = PiccoloClient()
v = Verifier(client)

# Individual checks (each is snapshot-bracketed — non-destructive):
v.verify_scene_has_player()       # player object present
v.verify_scene_has_floor()        # floor object present
v.verify_character_moves()        # FORWARD for 60 frames → position changes
v.verify_character_jumps()        # JUMP → peak Z rises ≥ 0.05 m
v.verify_wall_collision()         # FORWARD into wall → movement blocked

# Or run the full suite:
suite  = VerificationSuite(client)
report = suite.run_all()
VerificationSuite.print_report(report)
```

### agent_planner.py

```python
from scripts.agent_planner import run_demo

# Full end-to-end: LLM plans + executor builds + verifier confirms
ok = run_demo("create a minimal third-person game with a player, floor and two walls")
```

Or from the terminal (requires `ANTHROPIC_API_KEY`):

```bash
export ANTHROPIC_API_KEY=sk-…
python3 scripts/agent_planner.py
python3 scripts/agent_planner.py --goal "build an obstacle course with stairs and a fence"
python3 scripts/agent_planner.py --skip-verify   # skip verification suite
```

## C++ Changes

### cmdCreateObject (agent_command_system.cpp)

Replaces the Phase 1 stub. Accepts a `definition` path (e.g. `asset/objects/character/player/player.object.json`), builds an `ObjectInstanceRes` with that path, calls `Level::createObject()`, and optionally calls `TransformComponent::setPosition()` if a position was supplied.

**Failure mode**: if the definition path is wrong or the asset file is missing, `createObject` returns `k_invalid_gobject_id` and the command returns an error.

### cmdSaveLevel (agent_command_system.cpp)

Calls `Level::save()`, which serialises every GObject to `ObjectInstanceRes` and writes the level file back to disk via `AssetManager`.

### New tool: `save_level`

Exposed in the MCP tool list; dispatched in `agent_mcp_server.cpp`.

## Verification Checks

| Check | Method | Pass condition |
|-------|--------|---------------|
| scene_has_player | `verify_scene_has_player` | Object named "Player" exists |
| scene_has_floor | `verify_scene_has_floor` | Object with "floor" / "ground" in name exists |
| character_moves | `verify_character_moves` | Displacement ≥ 0.05 m after 60 FORWARD frames |
| character_jumps | `verify_character_jumps` | Peak Z rise ≥ 0.05 m after JUMP input |
| wall_collision | `verify_wall_collision` | Player doesn't penetrate wall (movement blocked) |

All checks are snapshot-bracketed — the scene state is restored after each one.

## End-to-End Demo Flow

```
user: "create a minimal third-person game"
         ↓
Planner: query asset catalog + get_state → understand current scene
         ↓
Planner: snapshot(save, tag="before_plan")
         ↓
Planner: create_object(Player) → create_object(Floor) → create_object(Wall_1) → …
         ↓ if any step fails → snapshot(restore) → replan
         ↓
Planner: save_level
         ↓
Verifier: 5 checks (scene, movement, jump, collision)
         ↓
Report: "✓ All checks passed — game is playable"
```

## Build Status

```
PiccoloRuntime: compiles cleanly
PiccoloEditor:  compiles cleanly (pre-existing Lua tmpnam warning unrelated)
Python scripts: all compile (py_compile) cleanly
asset_catalog.json: valid JSON
```

## Acceptance Criteria Status

| Criterion | Status |
|-----------|--------|
| `asset_catalog.json` covering all current Piccolo assets | ✓ 9 prefab types documented |
| Verifier module: character movement | ✓ `verify_character_moves` |
| Verifier module: character jump | ✓ `verify_character_jumps` |
| Verifier module: wall collision | ✓ `verify_wall_collision` |
| End-to-end demo with LLM planning loop | ✓ `agent_planner.py` |
