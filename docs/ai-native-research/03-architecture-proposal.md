# Architecture Proposal: AI-Native Piccolo Engine

**Date:** 2026-11  
**Status:** Deliverable 3 — pending review before implementation begins  
**Prerequisite reading:** 01-research-survey.md, 02-piccolo-assessment.md

---

## 1. The Seven Special Research Questions

Before presenting architectures, the seven questions from the task description deserve direct answers, since they will determine which design is worth building.

### Q1: What does "Agent as first-class citizen" require architecturally?

Five changes: (1) a stable, typed command intake path the agent can call without touching the GUI; (2) a state query path the agent can use to observe the world without rendering; (3) a headless execution mode; (4) programmatic input injection; (5) a snapshot/rollback mechanism. These are control-flow changes, not knowledge changes. They are orthogonal to the LLM's reasoning ability. Without them, the agent can still drive the engine but must do so unreliably, through the filesystem and by parsing log output.

Of these five, (1) through (3) are strictly *new bottom-layer capabilities* — they require engine source changes. (4) is a one-method addition. (5) is a new class with no changes to existing code.

### Q2: Does a full-featured MCP plugin achieve AI-native goals? What is missing?

A full MCP plugin achieves approximately 70% of an effective agent workflow for the specific case of editor-time scene authoring. It fails at:
- **Physics verification**: the agent cannot observe the result of a game interaction without running the game and reading runtime state, which MCP over WebSocket cannot do reliably
- **Single-step simulation**: the agent cannot advance the simulation one frame at a time for deterministic testing
- **Rollback**: if a sequence of MCP calls creates a broken scene, there is no structured way to restore a known-good state
- **Input injection for game testing**: pressing "jump" and observing the character's Z position after 10 frames is not achievable via MCP without engine-level support

For the target use case (agent autonomously creates *and tests* a game), MCP alone is not sufficient. If the goal were only "agent edits scenes while a human tests," MCP would be adequate.

### Q3: Can Piccolo's reflection system serve as the agent control interface?

Yes, with caveats. Reflection can enumerate types and get/set fields. The gaps are:
- No semantic descriptions embedded in the reflection metadata
- No validation/constraint enforcement built into field accessors
- No inter-type dependency encoding (e.g., AnimationComponent requires a valid skeleton path)

For a minimal agent interface, reflection is sufficient. For a robust one, add a JSON-schema layer on top of reflection output that documents field semantics, valid ranges, and required co-components. This schema layer does not require engine modification.

### Q4: What is the hardest problem in agent-driven asset reuse?

**Semantic understanding** is the hardest problem for Piccolo's current corpus, not retrieval or combination. The agent must understand that `player.object.json` defines a character with physics, animation, and a camera — none of this is stated in any machine-readable file. The solution is a hand-authored `asset_catalog.json` that describes each asset's purpose, capabilities, and required companion assets. For the current 10–20 asset corpus, this is a few hours of work, not a system-engineering problem.

Retrieval infrastructure (vector databases, knowledge graphs) is premature and not recommended for this project phase.

### Q5: What specific gaps prevent autonomous game development in Piccolo today?

In rough order of blocking severity:
1. No programmatic control of the main tick loop (most blocking — prevents any game-state testing)
2. No command injection interface (second most blocking — agent can only write JSON files)
3. No runtime state query endpoint (prevents verification)
4. No snapshot/rollback (prevents safe iterative development)
5. No asset semantic descriptions (makes agent asset reuse fragile)

None of these are hard problems. All have known engineering solutions. The gap is implementation work, not unsolved research.

### Q6: What belongs in C++ vs. the external harness?

**Put in C++ (engine side) everything that must be synchronized with the tick loop:**
- Command queue (commands land in a buffer, execute at tick start)
- Input injection (`setGameCommand()` on `InputSystem`)
- Tick control (`tickOneFrame()` exposed to external caller)
- State serialization (snapshot of `Level` to JSON buffer)
- Result encoding (typed `CommandResult` struct)

**Put in the external harness everything that is about reasoning:**
- LLM planning and task decomposition
- Retry and error recovery logic
- Asset semantic descriptions and catalog
- Verification logic (compare expected vs observed state)
- Game requirements tracking and progress evaluation
- Multi-step workflow orchestration

The boundary test: "if this code needs to know the engine tick is about to happen, it's in C++; if it needs to reason about what to do next, it's in the harness."

### Q7: What architecture modifications best demonstrate C++ / Agent Infra / reliability engineering skills?

The highest-signal items for an internship portfolio:
1. **Agent Command Subsystem in C++** — a typed command queue, dispatch table, result encoding, and thread-safety protocol. Demonstrates: C++ systems programming, concurrency, interface design.
2. **Snapshot/Rollback Mechanism** — in-memory serialization of a live scene, restore on command failure. Demonstrates: reliability engineering, data structures, careful ownership semantics.
3. **External Agent Harness in Python** — planning loop, command sequencing, verification pass, retry with rollback. Demonstrates: agent infrastructure design, LLM API usage, test harness engineering.
4. **Asset Catalog with Validation** — JSON schema over reflection output, component dependency constraints. Demonstrates: schema design, constraint modeling, documentation engineering.

---

## 2. Four Candidate Architectures

### Architecture A: External Agent + MCP / JSON-File Interface

**What it is:** An external Python process plays the role of agent harness. It calls an LLM API, translates responses into JSON file writes to `engine/asset/`, then signals the engine to `reloadCurrentLevel()`. The engine runs normally with its GUI. No engine source changes.

**How the agent operates:**
1. Write `1-1.level.json` with new object definitions
2. Send a "reload" signal (e.g., write a sentinel file the engine polls, or add a minimal HTTP endpoint)
3. Wait for the engine to reload and render
4. Scrape the engine's log output for errors
5. Take a screenshot and feed it to a vision model for verification

**Capability ceiling:**
- Can create/modify scene objects at editor-time ✅
- Cannot inject input to test game behavior ❌
- Cannot observe runtime physics state ❌
- Cannot step the simulation deterministically ❌
- Cannot roll back a failed operation ❌

**Engine changes required:** Minimal — only a reload signal mechanism.

**Engineering complexity:** Low. The agent harness is straightforward; the gap is in what it cannot test.

**1–2 month scope:** Working scene editor agent that can compose and display a scene. Cannot verify game logic.

**Assessment:** This is what most Godot MCP projects implement. It is a useful starting point but not adequate for the stated goal of autonomous game development with verified behavior.

---

### Architecture B: In-Engine Agent Control Subsystem

**What it is:** A new C++ module, `AgentControlSystem`, is added to `RuntimeGlobalContext` alongside the existing subsystems. It runs an HTTP server (e.g., using cpp-httplib, a header-only library) in a background thread, receives JSON commands, queues them for execution at the start of the next `logicalTick()`, and returns typed JSON results. The LLM-based agent harness runs externally and communicates with this server.

**New C++ additions (all localized):**
```
engine/source/runtime/function/agent/
  agent_command_system.h       // HTTP server + command queue
  agent_command_system.cpp
  agent_command_types.h        // typed commands and results
  agent_scene_snapshot.h       // in-memory level serialization
  agent_scene_snapshot.cpp
```

**Modifications to existing files:**
- `engine/source/runtime/function/global/global_context.h` — add `m_agent_command_system` member
- `engine/source/runtime/engine.cpp` — add `agentCommandSystem->dispatchQueuedCommands()` at start of `logicalTick()`
- `engine/source/runtime/function/input/input_system.h` — add `setGameCommand(unsigned int)` method

**Capability ceiling:**
- Create/modify/delete GObjects via typed commands ✅
- Save/reload levels ✅
- Inject input commands to test game logic ✅
- Observe GObject positions and component state after N ticks ✅
- Take scene snapshots ✅
- Roll back to snapshot on failure ✅
- Physics single-step (via `runLogicalTicks(n)` command) ✅
- Cannot run without a display window ❌ (unless headless mode is also added)
- Cannot do offscreen rendering for visual verification ❌

**Engine changes required:** Medium — new module + minimal touches to 3 existing files.

**Engineering complexity:** Medium. HTTP server threading, command queue with mutex, snapshot serialization.

**1–2 month scope:** Full command-driven scene authoring, game-logic testing (jump mechanics, movement), and rollback. No visual verification.

---

### Architecture C: Full AI-Native Architecture

**What it is:** Architecture B plus headless mode, offscreen rendering via Vulkan offscreen framebuffer, a physics single-step API, and a semantic asset catalog. The engine can run entirely without a display, step the simulation one frame at a time, and return pixel data or structured state observations to the agent.

**Additional changes beyond B:**
- `PiccoloEngine::runHeadless(int max_ticks)` — tick loop without window system
- Vulkan offscreen rendering path in `RenderSystem` — render to a framebuffer, return pixel data
- `PhysicsScene::stepOne()` — advance JoltPhysics one deterministic step
- `AssetRegistry` — filesystem crawler + catalog JSON for semantic asset descriptions
- Full reflection-based capability schema exported as JSON on agent request

**Engine changes required:** High — touches rendering pipeline (non-trivial), main loop, physics, asset system.

**Engineering complexity:** High. Offscreen Vulkan rendering requires significant render pipeline work.

**1–2 month scope:** Achievable if scoped to: headless mode + offscreen rendering (3–4 weeks) + existing Architecture B commands (2–3 weeks). The full capability set exceeds 2 months.

---

### Architecture D: Progressive Hybrid

**What it is:** Start with a minimal subset of Architecture B (the absolute minimum to unblock the agent), verify it works end-to-end, then extend based on observed bottlenecks.

**Phase 0 (week 1–2): Minimum viable agent interface**
- Add `setGameCommand(unsigned int)` to `InputSystem` (1 method, 1 hour)
- Expose `PiccoloEngine::tickOneFrame()` as a callable from a simple UNIX domain socket or pipe
- Write a Python harness that can call LLM, write JSON level files, signal reload, inject inputs, and read back GObject positions via a state serialization call

**Phase 1 (week 3–5): Command system**
- Implement the full `AgentCommandSystem` from Architecture B
- Add snapshot/rollback
- Write the Python harness around this API

**Phase 2 (week 6–8): Asset catalog + verification**
- Hand-author `asset_catalog.json` for all current assets
- Implement verification logic in the harness (expected vs observed state)
- Run end-to-end test: agent creates playable third-person character scene

**Phase 3 (optional, week 9+): Headless + extended coverage**
- Headless mode (remove window dependency)
- Reflection-based capability schema

---

## 3. Recommendation

**Recommended: Architecture D (Progressive Hybrid) targeting Architecture B as the end state.**

**Rationale:**
- Architecture A cannot achieve the stated goal (verified autonomous game development). It should not be the endpoint.
- Architecture B is achievable in 1–2 months, covers the core agent autonomy requirements, and produces the C++ systems engineering work that has the highest portfolio value.
- Architecture C is the right long-term target but exceeds the 2-month window due to the offscreen rendering complexity.
- Architecture D starts delivering working code in week 2, provides early validation of assumptions, and naturally reaches Architecture B by week 8.

**The key bet:** The `AgentCommandSystem` HTTP server + command queue + snapshot mechanism is the core engineering challenge. It must handle concurrent access from the HTTP server thread and the main tick thread. This is not hard but requires care. Getting this right is the highest-value technical work in the project.

---

## 4. Target Architecture Diagram

```
┌───────────────────────────────────────────────────────────────────┐
│                        External Agent Harness (Python)             │
│                                                                    │
│  LLM API (Anthropic/OpenAI) ◄──── Planning Loop                   │
│       ↕                               ↕                           │
│  Asset Catalog         Task Tracker ◄──── Verification Logic       │
│  (asset_catalog.json)                      (expected vs observed)  │
│       ↕                               ↕                           │
│               AgentHarness Client (HTTP/JSON)                      │
└─────────────────────────┬─────────────────────────────────────────┘
                          │ JSON over HTTP (localhost)
                          │
┌─────────────────────────▼─────────────────────────────────────────┐
│                        Piccolo Engine Process                       │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │                   AgentCommandSystem                         │  │
│  │  HTTP Server Thread ──► CommandQueue (mutex-protected)       │  │
│  │  CommandResult ◄──────── Dispatch Table                     │  │
│  │  SceneSnapshot ◄──────── in-memory Level serialization       │  │
│  └──────────────────────────┬────────────────────────────────┘  │
│                              │  dispatched at tick start         │
│  ┌────────────────────────────▼──────────────────────────────┐  │
│  │               RuntimeGlobalContext                         │  │
│  │  WorldManager ──► Level ──► GObject ──► Components         │  │
│  │  InputSystem  ──► setGameCommand() [new method]            │  │
│  │  AssetManager ──► loadAsset<T>() / saveAsset<T>()          │  │
│  │  PhysicsManager ──► PhysicsScene ──► Jolt step             │  │
│  │  RenderSystem ──► (optional offscreen path)                │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                    │
│  PiccoloEngine::tickOneFrame()                                     │
│    → agentCommandSystem.dispatchQueuedCommands()  [new]           │
│    → logicalTick()                                                 │
│    → rendererTick()                                                │
└───────────────────────────────────────────────────────────────────┘
```

---

## 5. Module Design

### 5.1 AgentCommandSystem (C++)

**Location:** `engine/source/runtime/function/agent/`

**Responsibilities:**
- Run an HTTP server (port configurable, default 7890) in a background std::thread
- Accept JSON POST requests to `/command`
- Validate command type and parameters
- Push valid commands onto a `std::queue<AgentCommand>` protected by `std::mutex`
- At the start of each `logicalTick()`, drain the queue and dispatch each command
- Return typed `CommandResult` structs serialized to JSON via the reflection system

**Key data structures:**

```cpp
// agent_command_types.h

enum class AgentCommandType {
    CreateObject,     // spawn a GObject from definition + overrides
    DeleteObject,     // delete GObject by ID
    SetComponentField, // set a reflected field on a component
    InvokeMethod,     // invoke a reflected method
    SaveLevel,        // save current level to disk
    LoadLevel,        // reload current level from disk or new URL
    SetInput,         // inject game command bitmask
    TakeSnapshot,     // serialize current level to in-memory buffer
    RestoreSnapshot,  // restore level from in-memory buffer
    QueryObjects,     // return list of GObjects with component state
    GetCapabilities,  // return JSON schema of all reflected types
    RunTicks,         // advance simulation N logical ticks
};

struct AgentCommand {
    AgentCommandType type;
    std::string      request_id;      // returned in result for correlation
    Json             params;          // type-specific parameters
};

struct CommandResult {
    std::string request_id;
    bool        success;
    std::string error_message;        // empty on success
    Json        data;                 // type-specific result payload
};
```

**Thread safety contract:**
- HTTP server thread: only writes to `m_pending_queue`
- Tick thread: only reads/drains `m_pending_queue` at the beginning of `logicalTick()`
- A single `std::mutex m_queue_mutex` protects the queue
- Results are written to `m_completed_results` (also mutex-protected) and read by the HTTP server thread when the client polls

**Integration point in engine.cpp:**
```cpp
void PiccoloEngine::logicalTick(float delta_time)
{
    if (g_runtime_global_context.m_agent_command_system)
        g_runtime_global_context.m_agent_command_system->dispatchQueuedCommands();  // new

    g_runtime_global_context.m_world_manager->tick(delta_time);
    g_runtime_global_context.m_input_system->tick();
}
```

### 5.2 SceneSnapshot

**Location:** `engine/source/runtime/function/agent/agent_scene_snapshot.h`

```cpp
class SceneSnapshot {
public:
    void capture(const Level& level);     // serialize level to m_data (JSON buffer)
    bool restore(Level& level) const;     // deserialize m_data back into level
    bool isEmpty() const;
    std::string toJson() const;           // for debugging

private:
    std::string m_data;     // JSON representation of the level at capture time
    std::chrono::steady_clock::time_point m_timestamp;
};
```

The snapshot uses `Level::getAllGObjects()` → for each GObject, calls `gobject->save(ObjectInstanceRes)` → serializes to JSON. Restore recreates objects from their `ObjectInstanceRes`. This is safe because `Level::createObject()` already takes an `ObjectInstanceRes` — the round-trip is architecturally supported.

**Risk:** Objects with runtime-only state (e.g., current animation frame, physics velocity) will not be fully captured by JSON serialization. For the MVP use case (rollback after a failed command, not mid-combat state recovery), this is acceptable.

### 5.3 Asset Catalog

**Location:** `docs/ai-native-research/asset_catalog.json` (data file, no C++ needed)

A hand-authored JSON catalog read by the Python harness, not the engine. Format:

```json
{
  "assets": [
    {
      "id": "character/player",
      "definition": "asset/objects/character/player/player.object.json",
      "type": "character",
      "description": "Third-person player character with physics capsule, skeletal animation, mesh renderer, motor controller, and third-person camera.",
      "capabilities": ["move", "jump", "sprint", "animate"],
      "components": ["TransformComponent", "AnimationComponent", "MeshComponent", "MotorComponent", "CameraComponent"],
      "required_companions": [],
      "configurable_fields": {
        "MotorComponent.move_speed": {"type": "float", "default": 2.0, "range": [0.1, 20.0]},
        "MotorComponent.jump_height": {"type": "float", "default": 1.0, "range": [0.1, 10.0]}
      }
    }
  ]
}
```

This is not a knowledge graph. It is a manually curated reference document that the agent harness reads to construct valid `ObjectInstanceRes` JSON without hallucinating field names.

### 5.4 External Agent Harness (Python)

A Python process organized around three concerns:

**Planner:** Calls the LLM API with the user's game requirement and the current engine state. Produces a sequence of `AgentCommand` objects. Uses the asset catalog to inform object creation requests.

**Executor:** Sends commands to the `AgentCommandSystem` HTTP server. Handles results, retries on transient failures, triggers snapshot before destructive sequences, triggers rollback on failure.

**Verifier:** After the game runs for N ticks (via `RunTicks` command), reads the `QueryObjects` result and checks game-logic assertions: "did the character's Z position increase during the frames when `jump` input was active?" This is state-based verification, not code review.

---

## 6. Command Execution Protocol (State Machine)

```
Agent sends game task description
         ↓
Planner: query capabilities, query existing scene state
         ↓
Planner: generate plan (sequence of engine operations)
         ↓
         ┌─────────── for each operation in plan ──────────────┐
         │                                                     │
         │  TakeSnapshot (save rollback point)                 │
         │         ↓                                           │
         │  Execute command → check CommandResult.success      │
         │         ↓ success              ↓ failure            │
         │  Continue to next op     RestoreSnapshot            │
         │                               ↓                     │
         │                          Replan from failure info   │
         └─────────────────────────────────────────────────────┘
         ↓
SaveLevel (persist successful scene)
         ↓
SetInput + RunTicks (test game behavior)
         ↓
QueryObjects → Verifier checks state assertions
         ↓ pass              ↓ fail
Report success         Replan (fix behavior)
```

---

## 7. MVP Design

**Goal:** Given the existing Piccolo asset corpus, an agent creates a minimal but functional third-person game: character spawns, player can move and jump, camera follows character, there is at least one static wall for collision. The agent verifies that the character's position changes when movement input is injected and that the jump raises the Z coordinate.

**Assets available today:** Player character (player.object.json), wall objects, particle emitter.

**What the agent does:**
1. Query asset catalog → identifies `character/player` and `environment/wall`
2. Load the existing level template or start from an empty level
3. Create a Player GObject at position (3, 3, 0)
4. Create three Wall GObjects forming a small arena
5. SaveLevel
6. SetInput(forward) + RunTicks(30) → QueryObjects → verify player.position.y changed
7. SetInput(jump) + RunTicks(10) → QueryObjects → verify player.position.z > 0.1 during ticks 3–8
8. Report verified game

**What it does NOT do (explicitly out of scope for MVP):**
- Generate new art assets
- Write custom gameplay logic beyond what MotorComponent provides
- Create AI enemy characters
- Render screenshots

---

## 8. Phased Implementation Roadmap

### Phase 0: Unblock agent access (week 1–2)

**Acceptance criteria:**
- An external Python script can write a valid level JSON, signal the engine to reload, and read back the list of GObjects and their component fields via a simple endpoint.
- Input injection: Python can set `m_game_command` bitmask, and the character moves.

**Work:**
- Add `setGameCommand(unsigned int)` to `InputSystem` (1 line)
- Add a minimal in-process Python-C++ bridge or simple file-polling mechanism for reload signal
- Write Python harness client skeleton with `reload()`, `query_objects()`, `set_input()` calls

**Risk:** Low. These are minimal changes with no new subsystems.

### Phase 1: AgentCommandSystem (week 3–6)

**Acceptance criteria:**
- Full command set (create, delete, set field, snapshot, restore, run ticks, query) working over HTTP/JSON
- Thread-safe command queue verified (no crashes under rapid concurrent requests)
- Python harness can run the MVP scenario end-to-end

**Work:**
- Implement `AgentCommandSystem` C++ module with cpp-httplib
- Add `SceneSnapshot` class
- Extend Python harness to full executor loop with rollback

**Risk:** Medium. HTTP server threading in C++ requires care. cpp-httplib is header-only and well-tested, which reduces risk. The command dispatch loop must not block the tick thread.

### Phase 2: Asset catalog + verification (week 7–8)

**Acceptance criteria:**
- Hand-authored `asset_catalog.json` covering all current Piccolo assets
- Verifier module confirms: character moves, character jumps, walls collide
- Full end-to-end demo: user types "make a third-person game," agent produces and verifies it

**Work:**
- Write `asset_catalog.json` (manually, ~100 lines)
- Implement `Verifier` class in Python harness with configurable state assertions
- Full LLM planning loop integration

**Risk:** Low for the catalog. Medium for the full planning loop — depends on LLM instruction-following quality.

### Phase 3: Headless mode + reflection schema (optional, week 9+)

**Acceptance criteria:**
- Engine starts and runs without a display window
- Agent can enumerate all reflected types and their fields in machine-readable JSON schema format

**Work:**
- Add `PiccoloEngine::runHeadless(int max_ticks)` method
- Conditionally skip `WindowSystem` and `RenderSystem` initialization in headless mode
- Write `GetCapabilities` command handler that serializes all `TypeMeta` entries to JSON schema

**Risk:** Medium. Headless rendering requires disentangling the render system from the logical tick. JoltPhysics has no display dependency. Animation may need a no-op render path.

---

## 9. Key Technical Risks

### Risk 1: Command queue deadlock or race condition
**Scenario:** HTTP thread writes to queue while tick thread is draining it.  
**Mitigation:** Use `std::mutex` + `std::queue`; tick thread drains under a `std::lock_guard`, HTTP thread writes under a `std::lock_guard`. Never hold the lock while executing commands (drain first, execute outside lock).

### Risk 2: Scene snapshot incompleteness
**Scenario:** Snapshot captures JSON state but misses runtime-only fields (animation frame, physics velocity). Restore produces a valid but different state.  
**Mitigation:** Accept this for MVP (the snapshot is for rollback on failed operations, not mid-game state recovery). Document the limitation. Add physics velocity to snapshot later if needed.

### Risk 3: Reflection system gaps for agent commands
**Scenario:** An agent tries to set a field that is not `META(Enable)` tagged, or calls a method not registered to the reflection system.  
**Mitigation:** The `GetCapabilities` command returns only what is actually registered — the agent sees only what it can legitimately access. Document which fields require `META(Enable)` annotation as an extension point for contributors.

### Risk 4: LLM hallucinating invalid field paths
**Scenario:** LLM generates a `SetComponentField` command with `field_path = "MotorComponent.m_speed"` but the real field is `m_move_speed`.  
**Mitigation:** The `GetCapabilities` command returns exact field names. The harness should always call `GetCapabilities` and feed the result into the LLM context before generating field-access commands.

### Risk 5: cpp-httplib adds a dependency
**Scenario:** Building Piccolo with cpp-httplib requires adding it to the CMake build.  
**Mitigation:** cpp-httplib is a single-header library with no external dependencies. Add it to `engine/3rdparty/` and add to `CMakeLists.txt`. This is a 10-minute change. Alternatively, use a named pipe or UNIX domain socket to avoid any new dependency entirely.

---

## 10. What to Validate Before Building

Three technical experiments to run before writing the main implementation, in order of risk reduction:

**Experiment 1 (1 day):** Add `setGameCommand(unsigned int cmd)` to `InputSystem`, call it from a simple test harness after `startEngine()`, verify the character moves. This validates the core input injection assumption before any larger C++ work.

**Experiment 2 (1 day):** Serialize a `Level` to JSON using the existing reflection + serializer, write it to disk, reload it, verify the GObjects are present. This validates the snapshot round-trip assumption. The `Level::save()` method already does this — the question is whether `Level::createObject()` after a `clear()` produces identical runtime state.

**Experiment 3 (2 days):** Run a simple HTTP server in a std::thread while the engine main loop runs in the main thread; have the HTTP server push a `reload level` signal via a mutex-protected flag that the tick loop checks. Verify no crashes under rapid requests. This validates the threading model before building the full command system.

None of these experiments require committing to the full architecture. Each can be built on its own branch, confirmed to work, and then discarded once the main implementation absorbs the validated design.
