# Piccolo Architecture Assessment

**Date:** 2026-11  
**Status:** Deliverable 2 — source-code-grounded analysis  
**Rule:** Every claim cites a real file path, class, or function in this repository.

---

## 1. Engine Entry and Main Loop

**Files:** `engine/source/runtime/engine.h`, `engine/source/runtime/engine.cpp`

```
PiccoloEngine::startEngine()
  → Reflection::TypeMetaRegister::metaRegister()   // registers all reflected types
  → g_runtime_global_context.startSystems()         // starts all subsystems

PiccoloEngine::run()
  → while (!window_system->shouldClose())
      → tickOneFrame(delta_time)
            → logicalTick(delta_time)
                 → world_manager->tick()
                 → input_system->tick()
            → render_system->swapLogicRenderData()
            → rendererTick()
                 → render_system->tick()
            → window_system->pollEvents()
```

**Key observation for AI-native:**  
The main loop is window-driven. `run()` calls `window_system->shouldClose()` as its exit condition and `window_system->pollEvents()` every frame. There is no headless mode and no mechanism to tick the simulation a fixed number of times from external code. `tickOneFrame()` is `bool`-returning but only checks the window close flag — it does not support externally injected tick commands.

The `logicalTick()` and `rendererTick()` are separated, which is a good starting point for headless operation. A minimal change would add a `runHeadless(int max_ticks)` method that calls `logicalTick()` in a loop without touching the window system.

---

## 2. Global Context and Subsystems

**File:** `engine/source/runtime/function/global/global_context.h`

```cpp
class RuntimeGlobalContext {
public:
    void startSystems(const std::string& config_file_path);
    void shutdownSystems();

    std::shared_ptr<LogSystem>         m_logger_system;
    std::shared_ptr<InputSystem>       m_input_system;
    std::shared_ptr<FileSystem>        m_file_system;
    std::shared_ptr<AssetManager>      m_asset_manager;
    std::shared_ptr<ConfigManager>     m_config_manager;
    std::shared_ptr<WorldManager>      m_world_manager;
    std::shared_ptr<PhysicsManager>    m_physics_manager;
    std::shared_ptr<WindowSystem>      m_window_system;
    std::shared_ptr<RenderSystem>      m_render_system;
    std::shared_ptr<ParticleManager>   m_particle_manager;
    std::shared_ptr<DebugDrawManager>  m_debugdraw_manager;
    std::shared_ptr<RenderDebugConfig> m_render_debug_config;
};
extern RuntimeGlobalContext g_runtime_global_context;
```

The global context is a singleton holding shared_ptr to every subsystem. This is the natural injection point for an `AgentControlSystem` — it would be added to `g_runtime_global_context` alongside the existing systems, started in `startSystems()`, and accessed by name from any agent command handler.

**Thread safety:** The global context members are accessed from both the editor thread and the runtime thread in editor mode (controlled by `g_is_editor_mode` and `g_editor_tick_component_types` globals in `engine.h`). An agent command system adding a third access path must be synchronized with the logical tick.

---

## 3. World, Level, and GObject

### WorldManager
**File:** `engine/source/runtime/function/framework/world/world_manager.h`

The only public mutation methods are:
- `reloadCurrentLevel()` — reloads from disk
- `saveCurrentLevel()` — writes to disk
- `tick(float delta_time)` — advances one logical frame
- `getCurrentActiveLevel()` — returns a `weak_ptr<Level>`

There is no API for:
- Loading an arbitrary level by URL from agent code
- Creating a new level programmatically
- Swapping worlds at runtime

`loadWorld()` and `loadLevel()` are `private`, called only from `initialize()` and from the config file path. An agent that wants to create a new level must write a valid JSON level file to disk, then call `reloadCurrentLevel()`. This pattern works but has no atomicity or validation before the reload executes.

### Level
**File:** `engine/source/runtime/function/framework/level/level.h`

The useful agent-facing methods that *already exist*:
```cpp
GObjectID createObject(const ObjectInstanceRes& object_instance_res);
void      deleteGObjectByID(GObjectID go_id);
std::weak_ptr<GObject> getGObjectByID(GObjectID go_id) const;
const LevelObjectsMap& getAllGObjects() const;
bool load(const std::string& level_res_url);
bool save();
```

`createObject()` and `deleteGObjectByID()` are the primary runtime mutation entry points. `createObject()` takes an `ObjectInstanceRes` struct (name + definition URL + instanced components), which is fully deserializable from JSON using the reflection system. This is the correct path for an agent to spawn a new game object.

**Gap:** There is no `updateObjectComponent()` method. Modifying a component on an existing live object requires either:
1. Deleting the object and re-creating it (destructive)
2. Calling the component's setter directly via reflection (possible, but requires knowing the type and field path at runtime)
3. Saving the modified JSON and reloading (slow, loses runtime state)

### GObject
**File:** `engine/source/runtime/function/framework/object/object.h`

```cpp
class GObject : public std::enable_shared_from_this<GObject> {
    GObjectID m_id;
    std::string m_name;
    std::string m_definition_url;
    std::vector<Reflection::ReflectionPtr<Component>> m_components;

public:
    bool load(const ObjectInstanceRes&);
    void save(ObjectInstanceRes&);
    void tick(float delta_time);
    std::vector<Reflection::ReflectionPtr<Component>> getComponents();
    template<typename TComponent> TComponent* tryGetComponent(const std::string& name);
    bool hasComponent(const std::string& component_type_name) const;
};
```

The `tryGetComponent<T>()` template takes a string type name and does a linear scan of `m_components`, matching by `ReflectionPtr::getTypeName()`. This is the runtime component access path. The reflection ptr carries the type name string, so it works without static casting when the type name is known.

---

## 4. Reflection System

**Files:** `engine/source/runtime/core/meta/reflection/reflection.h` (372 lines), `engine/source/runtime/core/meta/reflection/reflection_register.h`

Piccolo's reflection system is code-generated. The meta parser (in `engine/source/meta_parser/`) scans header files at compile time for `CLASS(..., Fields)` or `CLASS(..., WhiteListFields)` macros, generating registration code that runs at startup via `TypeMetaRegister::metaRegister()`.

The reflection API provides:
```cpp
TypeMeta meta = TypeMeta::newMetaFromName("MotorComponent");
// → iterate fields:
meta.getFields()  // → vector<FieldAccessor>
// → get/set a field:
FieldAccessor f = meta.getFieldByName("m_move_speed");
f.set(component_ptr, &new_value);
f.get(component_ptr, &out_value);
// → call a method:
MethodAccessor m = meta.getMethodByName("getOffStuckDead");
m.invoke(component_ptr);
```

**What reflection can do for an agent:**
- Enumerate all registered types at runtime → capability discovery
- Get/set any `META(Enable)`-tagged field on any component by name → general component property mutation
- Invoke registered methods → general component method call
- Serialize/deserialize any reflected object to/from JSON → scene save/load, command encoding

**What reflection cannot do:**
- It does not carry semantic descriptions of fields ("what is `m_move_speed` in human/agent terms?")
- It does not enforce preconditions or invariants (setting `m_move_speed` to -1000 will compile and run)
- It does not distinguish fields that are safe to set mid-tick from those that must be set at load time
- Methods are only registered if the author explicitly added them to the reflection registration code (there are few registered methods compared to fields)
- It does not expose inter-component dependencies (e.g., MotorComponent requires a TransformComponent; this is not encoded in the reflection metadata)

**Conclusion:** The reflection system is a valid foundation for a general agent control interface. It needs semantic annotation to become a *good* agent interface.

---

## 5. Asset System

**File:** `engine/source/runtime/resource/asset_manager/asset_manager.h`

```cpp
class AssetManager {
    template<typename AssetType>
    bool loadAsset(const std::string& asset_url, AssetType& out_asset) const;
    
    template<typename AssetType>
    bool saveAsset(const AssetType& out_asset, const std::string& asset_url) const;

    std::filesystem::path getFullPath(const std::string& relative_path) const;
};
```

The asset manager is a thin serialization wrapper. It resolves a relative URL to a full path and reads/writes JSON via the reflection serializer. There is **no asset registry**, **no metadata catalog**, **no type-based discovery**, and **no dependency tracking**.

An agent trying to discover what assets are available must scan the filesystem and infer type from the JSON structure or file extension.

### Asset file structure (observed from real assets)

**Level file** (`engine/asset/level/1-1.level.json`):
```json
{
  "gravity": {"x": 0, "y": 0, "z": -15},
  "character_name": "Player",
  "objects": [
    {
      "name": "Player",
      "definition": "asset/objects/character/player/player.object.json",
      "instanced_components": [
        {"$typeName": "TransformComponent", "$context": {...}}
      ]
    }
  ]
}
```

**Object definition** (`engine/asset/objects/character/player/player.object.json`):
```json
{
  "components": [
    {"$typeName": "TransformComponent", "$context": {...}},
    {"$typeName": "AnimationComponent", "$context": {"animation_res": {"skeleton_file_path": "...", ...}}},
    {"$typeName": "MeshComponent", "$context": {"mesh_res": {"sub_meshes": [{"obj_file_ref": "...", "material": "..."}]}}},
    {"$typeName": "MotorComponent", "$context": {"motor_res": {"move_speed": 2, "jump_height": 1, ...}}},
    {"$typeName": "CameraComponent", "$context": {...}},
    {"$typeName": "LuaComponent", "$context": {"lua_script": "..."}}
  ]
}
```

**World file** (`engine/asset/world/hello.world.json`):
```json
{
  "name": "HelloWorld",
  "level_urls": ["asset/level/1-1.level.json"],
  "default_level_url": "asset/level/1-1.level.json"
}
```

### Asset types present in the repository

| Directory | Contents |
|---|---|
| `asset/objects/basic/` | `capsule.obj, cone.obj, convexmesh.obj, cube.obj, cylinder.obj, sphere.obj` |
| `asset/objects/character/player/` | Player object definition + animation/mesh/material assets |
| `asset/objects/environment/` | Wall, terrain, particle emitter objects |
| `asset/global/` | Global config (render, physics) |
| `asset/texture/` | Textures |

**Agent asset discovery today:** An agent can call `g_runtime_global_context.m_file_system` to list files, but there is no semantic metadata. The agent must infer from file naming conventions and JSON content what each asset is and what it can be used for. This is fragile.

---

## 6. Input System

**File:** `engine/source/runtime/function/input/input_system.h`

```cpp
enum class GameCommand : unsigned int {
    forward  = 1 << 0,  // W
    backward = 1 << 1,  // S
    left     = 1 << 2,  // A
    right    = 1 << 3,  // D
    jump     = 1 << 4,  // SPACE
    squat    = 1 << 5,  // not implemented
    sprint   = 1 << 6,  // LEFT SHIFT
    fire     = 1 << 7,  // not implemented
    free_camera = 1 << 8,  // F
    invalid  = 1 << 31
};

class InputSystem {
    unsigned int m_game_command {0};  // bitmask of active commands
    int m_cursor_delta_x, m_cursor_delta_y;
    Radian m_cursor_delta_yaw, m_cursor_delta_pitch;
public:
    void onKey(int key, int scancode, int action, int mods);  // GLFW callback
    void tick();  // processes pending commands
    unsigned int getGameCommand() const;
};
```

**Key gap:** `m_game_command` is a bitmask set entirely by `onKey()`, which is a GLFW keyboard callback. There is no `setGameCommand()` method and no way to inject a command bitmask programmatically without triggering a GLFW key event.

An agent that wants to test whether the character can jump needs to either:
1. Simulate GLFW events (possible but fragile, platform-dependent)
2. Add a `setGameCommand(unsigned int)` method (minimal, correct change)

The second option is the right path. This is a one-line change to `input_system.h` with a getter already present.

---

## 7. Lua Scripting

**File:** `engine/source/runtime/function/framework/component/lua/lua_component.h`

```cpp
class LuaComponent : public Component {
    sol::state m_lua_state;
    std::string m_lua_script;  // META(Enable) — editable via reflection
public:
    void tick(float delta_time) override;  // executes m_lua_script each tick
    static void set(weak_ptr<GObject>, const char* name, T value);
    static void get(weak_ptr<GObject>, const char* name, T value);
    static void invoke(weak_ptr<GObject>, const char* name);
};
```

The LuaComponent gives per-object scripting via sol2 bindings. The `set()` / `get()` / `invoke()` statics already bridge between Lua and the component reflection system. An agent can inject Lua scripts into an object at load time by writing a `LuaComponent` JSON with an arbitrary `lua_script` string — this is a usable but coarse mechanism.

The existing level file already demonstrates this:
```json
"lua_script": "if (get_bool(GameObject, \"MotorComponent.m_is_moving\")) then 
  set_float(GameObject, \"MotorComponent.m_motor_res.m_jump_height\", 10)
  else set_float(GameObject, \"MotorComponent.m_motor_res.m_jump_height\", 5) end 
  invoke(GameObject, \"MotorComponent.getOffStuckDead\")"
```

This confirms that Lua can read and write reflected component fields by name at runtime. This is a viable path for an agent to inject per-object behavior without recompiling C++.

---

## 8. Editor Subsystem

**Files:** `engine/source/editor/include/editor_scene_manager.h`, `editor_input_manager.h`, `editor.h`

The editor adds:
- `EditorSceneManager`: mouse picking, axis gizmo, `onDeleteSelectedGObject()`, `onGObjectSelected()`
- `EditorInputManager`: translates editor keyboard/mouse events to world operations
- `editor.h`: ties PiccoloEngine into a PiccoloEditor subclass

**Key observation:** The editor's object manipulation (select, move, delete) goes through the same `Level::createObject()` / `deleteGObjectByID()` APIs used by the runtime. This means those APIs are battle-tested for mid-session object manipulation.

The editor itself is not useful to an agent because it is entirely GUI-driven and processes mouse/keyboard events, not structured commands. However, the underlying Level APIs it uses are the right target for an agent command system.

---

## 9. Physics System

**Files:** `engine/source/runtime/function/physics/physics_manager.h`, `engine/source/runtime/function/physics/jolt/`

JoltPhysics is the backend. The PhysicsManager creates/deletes `PhysicsScene` instances. Physics is stepped as part of `logicalTick()` → `world_manager->tick()` → `level->tick()` → component ticks.

There is no exposed single-step API — physics advances exactly once per `logicalTick()` call. To test whether a jump works, an agent must run multiple ticks and observe position change over time. This requires either:
1. The headless tick loop described in Section 1 above
2. A `stepSimulation(int n_ticks)` method that runs the logical tick N times without rendering

---

## 10. Capability Gap Summary

### What Piccolo already provides (agent-usable today)

| Capability | API path | Quality |
|---|---|---|
| Create a GObject in a live level | `Level::createObject(ObjectInstanceRes)` | Good |
| Delete a GObject | `Level::deleteGObjectByID(GObjectID)` | Good |
| Enumerate all GObjects | `Level::getAllGObjects()` | Good |
| Read/write component fields | `TypeMeta::getFieldByName() + FieldAccessor` | Good (needs semantic annotation) |
| Save level to disk | `Level::save()` / `WorldManager::saveCurrentLevel()` | Good |
| Load level from disk | `WorldManager::reloadCurrentLevel()` | Good (no URL param, though) |
| Inject per-object scripted logic | `LuaComponent` via JSON | Usable |
| Discover all reflected types | `TypeMetaRegister` at startup | Good for capability list |
| Serialize any reflected type to JSON | `Serializer::write()` | Good |

### What Piccolo is missing (requires engine changes)

| Capability | Missing mechanism | Minimum change |
|---|---|---|
| Headless mode | Window-driven main loop | Add `runHeadless(int ticks)` to `PiccoloEngine` |
| Programmatic input injection | `InputSystem` only reads from GLFW callbacks | Add `setGameCommand(unsigned int)` to `InputSystem` |
| External command interface | No IPC/socket/HTTP server in engine | New `AgentCommandServer` module (can be in-process or out-of-process) |
| Single-step physics observation | No external step control | Expose `logicalTick()` via agent API |
| Scene snapshot before changes | No snapshot/rollback mechanism | New `SceneSnapshot` class (serialize current level to in-memory buffer) |
| Asset registry / metadata | Only filesystem paths known | New `AssetRegistry` class with optional semantic annotations |
| State query API | No structured observation endpoint | New `RuntimeObservation` serialization path |
| Runtime assertion / validation | No test-assertion primitives | New `EngineValidator` module |
| Component mutation on live object | No `updateComponent()` API | New `Level::updateObjectComponent()` method |
| Structured error from failed op | Only `LOG_ERROR` to stderr | New typed `CommandResult` struct |

---

## 11. Research Questions: Piccolo-Specific Answers

**Q1: What does "Agent as first-class citizen" require here?**  
Piccolo must add: an external command intake path (new module), programmatic input injection (one added method), headless tick mode (one added method), and a snapshot/rollback mechanism (new class). The reflection system already provides the type-safe operation surface. The gap is in control flow, not in domain knowledge.

**Q2: Does the reflection system cover an agent control interface?**  
Partially. It can enumerate types and fields, and can get/set fields by name. It does not provide semantic descriptions, precondition enforcement, or inter-component dependency graphs. For a *functional* agent interface it is sufficient; for a *robust* agent interface it needs annotation.

**Q3: What are the hardest problems for asset-based agent development in Piccolo?**  
In order: (1) **Semantic understanding** — the asset system stores paths, not intent; an agent does not know that `player.object.json` is a "character with physics and animation." (2) **Dependency constraints** — there is no machine-readable specification of which component combinations are valid. (3) **Combination planning** — constructing a valid `ObjectDefinitionRes` JSON that correctly references skeleton, mesh, and animation assets in a mutually consistent way requires knowledge of asset cross-references. (4) **Retrieval** — there are few assets; this is not a retrieval scaling problem yet. Vector databases and knowledge graphs are not needed for the current asset corpus.

**Q4: What is the riskiest Piccolo architecture issue for an agent?**  
The global context is a single-process singleton. All subsystems share one logical thread. An agent command that accesses `g_runtime_global_context` from a background thread (e.g., a separate MCP server thread) while `logicalTick()` is running will produce a race condition. The agent command system **must** queue commands for execution at the beginning of the next `logicalTick()`, not execute them immediately.

**Q5: What belongs in C++ vs. the external harness?**  
- C++ layer: tick control, input injection, snapshot, state serialization, asset path resolution
- External harness: LLM planning loop, command sequencing, error retry, semantic asset descriptions, verification logic, game requirements tracking
- The boundary is: "does this require synchronization with the engine tick?" → yes → C++; no → harness.
