# Piccolo 架构评估

**日期:** 2026-11  
**状态:** 交付物 2 — 基于源码的分析  
**规则:** 每个结论都给出本仓库中真实的文件路径、类或函数。

---

## 1. 引擎入口与主循环

**文件:** `engine/source/runtime/engine.h`, `engine/source/runtime/engine.cpp`

```
PiccoloEngine::startEngine()
  → Reflection::TypeMetaRegister::metaRegister()   // 注册所有反射类型
  → g_runtime_global_context.startSystems()         // 启动所有子系统

PiccoloEngine::run()
  → while (!window_system->shouldClose())            // ← 以窗口为退出条件
      → tickOneFrame(delta_time)
            → logicalTick(delta_time)
                 → world_manager->tick()
                 → input_system->tick()
            → render_system->swapLogicRenderData()
            → rendererTick()
                 → render_system->tick()
            → window_system->pollEvents()            // ← 没有窗口就没有 tick
```

**对 AI-native 的关键观察:**  
主循环是窗口驱动的。`run()` 以 `window_system->shouldClose()` 作为退出条件,每帧都调用 `window_system->pollEvents()`。没有无头模式,也没有从外部代码推进固定次数 tick 的机制。`tickOneFrame()` 返回 `bool` 但只检查窗口关闭标志——不支持外部注入的 tick 命令。

`logicalTick()` 和 `rendererTick()` 已经分离,这是无头化的良好起点。最小改动是给 `PiccoloEngine` 添加一个 `runHeadless(int max_ticks)` 方法,在循环里调用 `logicalTick()` 而不碰窗口系统。

---

## 2. 全局上下文与子系统

**文件:** `engine/source/runtime/function/global/global_context.h`

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

全局上下文是一个持有所有子系统 `shared_ptr` 的单例。这是给 `AgentControlSystem` 注入的天然位置——把它加入 `g_runtime_global_context`,在 `startSystems()` 里启动,从任何命令处理器按名访问。

**线程安全性:** 编辑器模式下,全局上下文成员被编辑器线程和运行时线程同时访问(由 `engine.h` 里的 `g_is_editor_mode` 和 `g_editor_tick_component_types` 全局变量控制)。Agent 命令系统如果增加第三个访问路径,必须与 logical tick 同步。

---

## 3. World、Level 与 GObject

### WorldManager
**文件:** `engine/source/runtime/function/framework/world/world_manager.h`

公开的变更方法只有:
- `reloadCurrentLevel()` — 从磁盘重新加载
- `saveCurrentLevel()` — 写入磁盘
- `tick(float delta_time)` — 推进一个逻辑帧
- `getCurrentActiveLevel()` — 返回 `weak_ptr<Level>`

**没有**:
- 用 URL 从 Agent 代码任意加载关卡的 API
- 以程序化方式创建新关卡
- 运行时切换世界

`loadWorld()` 和 `loadLevel()` 是 `private`,只由初始化路径从配置文件调用。一个想创建新关卡的 Agent 必须把合法的 JSON 关卡文件写到磁盘,然后调用 `reloadCurrentLevel()`。这个模式可用,但没有原子性,也没有在 reload 执行前的校验。

### Level
**文件:** `engine/source/runtime/function/framework/level/level.h`

**已存在且对 Agent 有用的方法:**
```cpp
GObjectID createObject(const ObjectInstanceRes& object_instance_res);
void      deleteGObjectByID(GObjectID go_id);
std::weak_ptr<GObject> getGObjectByID(GObjectID go_id) const;
const LevelObjectsMap& getAllGObjects() const;
bool load(const std::string& level_res_url);
bool save();
```

`createObject()` 接受 `ObjectInstanceRes`(名称 + 定义 URL + 实例化组件),可通过反射系统从 JSON 完整反序列化。这是 Agent 创建新游戏对象的正确路径。

**缺口:** 没有 `updateObjectComponent()` 方法。修改存活对象上的组件需要:
1. 删除对象再重建(破坏性)
2. 通过反射直接调用组件的 setter(可行,但需要在运行时知道类型和字段路径)
3. 保存修改后的 JSON 再 reload(慢,且丢失运行时状态)

### GObject
**文件:** `engine/source/runtime/function/framework/object/object.h`

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

`tryGetComponent<T>()` 模板接受字符串类型名,对 `m_components` 做线性扫描,通过 `ReflectionPtr::getTypeName()` 匹配。这是运行时组件访问路径。

---

## 4. 反射系统

**文件:** `engine/source/runtime/core/meta/reflection/reflection.h` (372 行), `engine/source/runtime/core/meta/reflection/reflection_register.h`

Piccolo 的反射系统是**代码生成的**。元解析器(`engine/source/meta_parser/`)在编译期扫描带 `CLASS(..., Fields)` 或 `CLASS(..., WhiteListFields)` 宏的头文件,生成注册代码,在启动时通过 `TypeMetaRegister::metaRegister()` 执行。

反射 API 提供:
```cpp
TypeMeta meta = TypeMeta::newMetaFromName("MotorComponent");
meta.getFields()                           // → 枚举所有已反射字段
FieldAccessor f = meta.getFieldByName("m_move_speed");
f.set(component_ptr, &new_value);          // → get/set 任意字段
MethodAccessor m = meta.getMethodByName("getOffStuckDead");
m.invoke(component_ptr);                   // → 调用注册的方法
```

**反射对 Agent 能做什么:**
- 在运行时枚举所有已注册类型 → 能力发现
- 按名 get/set 任意 `META(Enable)` 标记字段 → 通用组件属性变更
- 调用已注册方法 → 通用组件方法调用
- 把任何已反射对象序列化/反序列化为 JSON → 场景存取、命令编码

**反射的限制:**
- 不携带字段的语义描述("`m_move_speed` 在 Agent 语境下是什么意思?")
- 不执行前置条件或不变量(把 `m_move_speed` 设为 -1000 可以编译通过并运行)
- 不区分 tick 中途可安全设置的字段与只能在加载时设置的字段
- 方法只有作者显式加到注册代码里才会出现(已注册方法远少于字段)
- 不表达组件间依赖(MotorComponent 需要 TransformComponent——反射元数据里没有)

**结论:** 反射系统是通用 Agent 控制接口的合法基础。若要成为**好的** Agent 接口,需要在反射输出之上加一层 JSON schema,记录字段语义、合法取值范围和必要的配套组件。这个 schema 层不需要修改引擎。

---

## 5. 资产系统

**文件:** `engine/source/runtime/resource/asset_manager/asset_manager.h`

```cpp
class AssetManager {
    template<typename AssetType>
    bool loadAsset(const std::string& asset_url, AssetType& out_asset) const;
    
    template<typename AssetType>
    bool saveAsset(const AssetType& out_asset, const std::string& asset_url) const;

    std::filesystem::path getFullPath(const std::string& relative_path) const;
};
```

AssetManager 是一个薄序列化包装层:解析相对 URL 到完整路径,通过反射序列化器读写 JSON。**没有资产注册表、没有元数据目录、没有基于类型的发现、没有依赖追踪。** Agent 若想知道有哪些资产,只能扫描文件系统,从 JSON 结构或文件命名约定推断类型。

### 实际资产文件结构(从真实文件观察)

**关卡文件** (`engine/asset/level/1-1.level.json`):
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

**对象定义** (`engine/asset/objects/character/player/player.object.json`):
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

现有关卡 JSON 中已经内嵌了一段 `LuaComponent` 脚本,它在运行时用按名方式读写反射字段——这证明 Lua + 反射是 Agent 注入每对象行为、无需重编 C++ 的可行路径。

### 仓库中现有的资产类型

| 目录 | 内容 |
|---|---|
| `asset/objects/basic/` | `capsule.obj, cone.obj, convexmesh.obj, cube.obj, cylinder.obj, sphere.obj` |
| `asset/objects/character/player/` | 玩家对象定义 + 动画/网格/材质资产 |
| `asset/objects/environment/` | 墙壁、地形、粒子发射器对象 |
| `asset/global/` | 全局配置(渲染、物理) |
| `asset/world/hello.world.json` | 唯一的世界文件 |

---

## 6. 输入系统

**文件:** `engine/source/runtime/function/input/input_system.h`

```cpp
enum class GameCommand : unsigned int {
    forward  = 1 << 0,  // W
    backward = 1 << 1,  // S
    left     = 1 << 2,  // A
    right    = 1 << 3,  // D
    jump     = 1 << 4,  // SPACE
    squat    = 1 << 5,  // 未实现
    sprint   = 1 << 6,  // LEFT SHIFT
    fire     = 1 << 7,  // 未实现
    free_camera = 1 << 8,  // F
    invalid  = 1 << 31
};

class InputSystem {
    unsigned int m_game_command {0};  // 活跃命令位掩码
public:
    void onKey(int key, int scancode, int action, int mods);  // GLFW 回调
    void tick();
    unsigned int getGameCommand() const;
};
```

**关键缺口:** `m_game_command` 完全由 `onKey()` 设置,而 `onKey()` 是 GLFW 键盘回调。没有 `setGameCommand()` 方法,没有办法在不触发 GLFW 键盘事件的前提下以程序化方式注入命令位掩码。

Agent 要测试角色是否能跳,需要:
1. 模拟 GLFW 事件(可行但脆弱、平台相关)
2. **添加 `setGameCommand(unsigned int)` 方法**(最小、正确的改动)

第二个选项是正确路径。这是 `input_system.h` 里的一行改动,已有对应的 getter。

---

## 7. Lua 脚本

**文件:** `engine/source/runtime/function/framework/component/lua/lua_component.h`

```cpp
class LuaComponent : public Component {
    sol::state m_lua_state;
    std::string m_lua_script;  // META(Enable) — 可通过反射编辑
public:
    void tick(float delta_time) override;  // 每 tick 执行 m_lua_script
    static void set(weak_ptr<GObject>, const char* name, T value);
    static void get(weak_ptr<GObject>, const char* name, T value);
    static void invoke(weak_ptr<GObject>, const char* name);
};
```

LuaComponent 通过 sol2 绑定提供每对象脚本。`set()` / `get()` / `invoke()` 静态方法已经在 Lua 和组件反射系统之间建立了桥梁。Agent 可以在加载时通过写入带任意 `lua_script` 字符串的 `LuaComponent` JSON 来注入 Lua 脚本——这是粗粒度但可用的机制。

---

## 8. 编辑器子系统

**文件:** `engine/source/editor/include/editor_scene_manager.h`, `editor_input_manager.h`, `editor.h`

编辑器对象操作(选择、移动、删除)走的是运行时相同的 `Level::createObject()` / `deleteGObjectByID()` API。这意味着这些 API 经过了实战检验。编辑器本身对 Agent 无用——它完全是 GUI 驱动的,处理鼠标/键盘事件而非结构化命令。但它底层使用的 Level API 是 Agent 命令系统的正确目标。

---

## 9. 物理系统

**文件:** `engine/source/runtime/function/physics/physics_manager.h`, `engine/source/runtime/function/physics/jolt/`

JoltPhysics 是后端。PhysicsManager 创建/删除 `PhysicsScene` 实例。物理作为 `logicalTick()` → `world_manager->tick()` → `level->tick()` → 组件 tick 的一部分推进。

**没有**暴露单步 API——物理每次 `logicalTick()` 调用恰好前进一步。Agent 要测试跳跃是否有效,必须运行多个 tick 并随时间观测位置变化。这需要:
1. 上文第 1 节描述的无头 tick 循环
2. 一个 `stepSimulation(int n_ticks)` 方法,不渲染地运行 N 次逻辑 tick

---

## 10. 能力差距汇总

### Piccolo 已有(今天即可供 Agent 使用)

| 能力 | API 路径 | 质量 |
|---|---|---|
| 在存活关卡中创建 GObject | `Level::createObject(ObjectInstanceRes)` | 良好 |
| 删除 GObject | `Level::deleteGObjectByID(GObjectID)` | 良好 |
| 枚举所有 GObject | `Level::getAllGObjects()` | 良好 |
| 读/写组件字段 | `TypeMeta::getFieldByName() + FieldAccessor` | 良好(需语义标注) |
| 保存关卡到磁盘 | `Level::save()` / `WorldManager::saveCurrentLevel()` | 良好 |
| 从磁盘加载关卡 | `WorldManager::reloadCurrentLevel()` | 良好(没有 URL 参数) |
| 注入每对象脚本逻辑 | `LuaComponent` 通过 JSON | 可用 |
| 发现所有已反射类型 | `TypeMetaRegister` 在启动时 | 良好 |
| 把任意反射类型序列化为 JSON | `Serializer::write()` | 良好 |

### Piccolo 缺失(需要引擎改动)

| 能力 | 缺失机制 | 最小改动 |
|---|---|---|
| 无头模式 | 主循环是窗口驱动的 | `PiccoloEngine` 添加 `runHeadless(int ticks)` |
| 程序化输入注入 | `InputSystem` 只从 GLFW 回调读取 | `InputSystem` 添加 `setGameCommand(unsigned int)` |
| 外部命令接口 | 引擎内没有 IPC/socket/HTTP server | 新建 `AgentCommandServer` 模块 |
| 物理单步观测 | 没有外部步进控制 | 通过 Agent API 暴露 `logicalTick()` |
| 操作前场景快照 | 没有快照/回滚机制 | 新建 `SceneSnapshot` 类(序列化关卡到内存缓冲区) |
| 资产注册表/元数据 | 只知道文件系统路径 | 新建 `AssetRegistry` 或手写目录 JSON |
| 对存活对象的组件变更 | 没有 `updateObjectComponent()` API | 新建 `Level::updateObjectComponent()` |
| 操作失败的结构化错误 | 只有 `LOG_ERROR` 到 stderr | 新建类型化 `CommandResult` 结构体 |

---

## 11. 研究问题:Piccolo 专项回答

**Q1:Agent 成为一等公民这里具体要求什么?**  
Piccolo 必须新增:外部命令接收路径(新模块)、程序化输入注入(一个新方法)、无头 tick 模式(一个新方法)和快照/回滚机制(新类)。反射系统已经提供了类型安全的操作面。差距在控制流,不在领域知识。

**Q2:反射系统能覆盖 Agent 控制接口吗?**  
部分能。它能枚举类型和字段,能按名 get/set 字段。它不提供语义描述、前置条件执行或组件间依赖图。对于**功能性** Agent 接口已经足够;对于**健壮性** Agent 接口,需要在反射输出之上加标注。

**Q3:资产复用中最难的问题是什么?**  
按阻塞严重度排列:(1)**语义理解**——资产系统存路径,不存意图;Agent 不知道 `player.object.json` 是"有物理和动画的角色"。(2)**依赖约束**——没有哪些组件组合有效的机器可读规范。(3)**组合规划**——构造一个正确互相引用骨骼、网格和动画资产的合法 `ObjectDefinitionRes` JSON 需要知道资产交叉引用。(4)**检索**——当前资产极少,不是检索扩展性问题。向量数据库和知识图谱现阶段**没有必要**。

**Q4:Piccolo 架构中对 Agent 最危险的风险是什么?**  
全局上下文是单进程单例,所有子系统共享一个逻辑线程。从后台线程(如独立的 MCP server 线程)访问 `g_runtime_global_context` 的 Agent 命令,在 `logicalTick()` 运行时会产生竞态。Agent 命令系统**必须**把命令排入队列等待下次 `logicalTick()` 开始时执行,而不是立即执行。

**Q5:什么放在引擎 C++ 层,什么放外部 Harness?**  
- C++ 层:tick 控制、输入注入、快照、状态序列化、结果编码
- 外部 Harness:LLM 规划循环、命令排序、错误重试、语义资产描述、验证逻辑、游戏需求追踪
- 判断依据:如果需要知道引擎 tick 即将发生 → C++；如果需要推断下一步做什么 → Harness
