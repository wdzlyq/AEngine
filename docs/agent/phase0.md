# Piccolo Engine AI-Native Phase 0

## Overview

Phase 0 establishes the minimal communication path between a Python agent and the running Piccolo engine. The agent can inject input commands, trigger level reloads, and read object positions -- all through file-based IPC (no socket or shared memory needed).

## Files Modified / Created

### Modified

| File | Change |
|------|--------|
| `engine/source/runtime/function/input/input_system.h` | Added `setGameCommand()` public method, `m_agent_command_frames_remaining` countdown field |
| `engine/source/runtime/function/input/input_system.cpp` | Implemented `setGameCommand()`, added countdown logic in `tick()` |
| `engine/source/runtime/engine.cpp` | Added agent bridge: reload-signal polling (Task 2), input-command polling (Task 5), state.json writing (Task 4) in `logicalTick()` |

### Created

| File | Purpose |
|------|---------|
| `engine/asset/agent/.gitkeep` | Ensures the agent IPC directory exists in the repo |
| `scripts/agent_harness.py` | Python harness with `reload_level()`, `set_input()`, `get_state()`, and `demo_move_forward()` |
| `docs/agent/phase0.md` | This document |

## How the File-Based IPC Works

All communication happens via files in `engine/asset/agent/`:

```
engine/asset/agent/
├── reload_signal       # sentinel file -> engine reloads level, then deletes it
├── input_command.json   # agent writes command -> engine reads & deletes it
└── state.json           # engine writes object positions every 10 frames
```

### Flow: Inject Input

1. Python writes `input_command.json` with `{"command_mask": 1, "duration_frames": 60}`
2. Engine `logicalTick()` reads it, calls `setGameCommand(1, 60)`, deletes the file
3. `InputSystem::tick()` counts down; after 60 frames the command auto-clears
4. Character moves forward during those 60 frames

### Flow: Reload Level

1. Python touches `reload_signal`
2. Engine detects it, calls `reloadCurrentLevel()`, deletes the file

### Flow: Read State

1. Engine writes `state.json` every 10 frames with all GObject positions
2. Python reads it at any time

## Python Harness Usage

### Run the built-in demo

```bash
cd /home/wdzlyq/agentt/Piccolo
python3 scripts/agent_harness.py
```

This sends a 60-frame forward command and prints before/after positions.

### Use as a library

```python
from scripts.agent_harness import reload_level, set_input, get_state, GameCommand

# Move forward for 30 frames
set_input(GameCommand.FORWARD, duration_frames=30)

# Move forward + sprint indefinitely
set_input(GameCommand.FORWARD | GameCommand.SPRINT)

# Stop all movement
set_input(0)

# Reload the current level
reload_level()

# Read current object positions
state = get_state()
for obj in state.get("objects", []):
    print(obj["name"], obj["position"])
```

### GameCommand bit values

| Name | Value | Key |
|------|-------|-----|
| FORWARD | 1 | W |
| BACKWARD | 2 | S |
| LEFT | 4 | A |
| RIGHT | 8 | D |
| JUMP | 16 | SPACE |
| SPRINT | 64 | LEFT SHIFT |
| FREE_CAMERA | 256 | F |

## Verification Steps

1. Build the engine:
   ```bash
   cd /home/wdzlyq/agentt/Piccolo/build
   cmake --build . --target PiccoloEditor -j$(nproc)
   ```

2. Launch the editor and load a level with a controllable character.

3. In another terminal, run the demo:
   ```bash
   cd /home/wdzlyq/agentt/Piccolo
   python3 scripts/agent_harness.py
   ```

4. Observe the character moving forward in the engine window.

5. Alternatively, verify via state files:
   ```bash
   # Before sending command
   cat engine/asset/agent/state.json

   # Send command
   python3 -c "from scripts.agent_harness import *; set_input(GameCommand.FORWARD, 60)"

   # Wait ~1 second, then check state again
   cat engine/asset/agent/state.json
   ```
   The character's Z (or X) position should change between reads.

## Build Status

- `PiccoloRuntime`: compiles cleanly
- `PiccoloEditor`: compiles cleanly (pre-existing `tmpnam` warning from Lua 3rd-party is unrelated)
