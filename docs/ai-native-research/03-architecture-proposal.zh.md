# 架构方案:AI-Native Piccolo 引擎

**日期:** 2026-11  
**状态:** 交付物 3 — 进入实现前待审阅  
**前置阅读:** 01-research-survey.zh.md, 02-piccolo-assessment.zh.md

---

## 1. 七个专项研究问题

任务描述中提出的七个问题值得直接回答——它们决定哪种设计值得构建。

### Q1:"Agent 成为一等公民"在架构上需要什么?

五项改动:(1) 一个稳定、类型化的命令接收路径,Agent 可以在不碰 GUI 的情况下调用;(2) 一条状态查询路径,Agent 可以不渲染地观测世界;(3) 无头执行模式;(4) 程序化输入注入;(5) 一个快照/回滚机制。这些是控制流改动,不是知识层改动,与 LLM 推理能力正交。没有它们,Agent 仍然能驱动引擎,但只能靠文件系统和解析日志输出,不可靠地做。

其中,(1)–(3) 是严格意义上的**新底层能力**——需要改引擎源码。(4) 是一个方法的添加。(5) 是一个新类,不改动现有代码。

### Q2:功能完整的 MCP 插件能达到 AI-native 目标吗?缺什么?

一个完整 MCP 插件对"编辑期场景创作"这个具体场景能达到约 70% 的有效 Agent 工作流。它失败的地方:
- **物理验证**:Agent 无法在不运行游戏并读取运行时状态的情况下观测游戏交互结果,而 MCP over WebSocket 无法可靠地做到这一点
- **单步模拟**:Agent 无法逐帧推进模拟以做确定性测试
- **回滚**:如果一串 MCP 调用创造了有缺陷的场景,没有结构化方式恢复到已知良好状态
- **用于游戏测试的输入注入**:按"jump"并观察角色 10 帧后的 Z 位置,在 MCP 没有 C++ 层支持的情况下无法实现

对目标用例(Agent **自主创建并测试**游戏),MCP 单独不够。如果目标只是"Agent 编辑场景,人测试",MCP 足够。

### Q3:Piccolo 的反射系统能充当 Agent 控制接口吗?

可以,但有保留。反射能枚举类型和 get/set 字段。缺口:
- 反射元数据里没有语义描述
- 字段访问器里没有校验/约束执行
- 没有类型间依赖编码(如 AnimationComponent 需要合法的骨骼路径)

对最小 Agent 接口已足够。对健壮接口,需要在反射输出之上加一层 JSON schema,记录字段语义、合法取值范围和必要的配套组件。这个 schema 层不需要改引擎。

### Q4:Agent 驱动资产复用里最难的问题是什么?

对 Piccolo 当前资产库,**语义理解**是最难的,不是检索或组合。Agent 必须理解 `player.object.json` 定义了一个带物理、动画和相机的角色——这在任何机器可读文件里都没有说明。解决方案是手写 `asset_catalog.json`,描述每个资产的用途、能力及所需配套资产。对当前 10–20 个资产,这是几小时工作量,不是系统工程问题。

检索基础设施(向量数据库、知识图谱)现阶段为时过早,**不推荐**。

### Q5:什么具体缺口导致 Agent 今天无法在 Piccolo 上自主开发游戏?

按阻塞严重度排序:
1. 没有对主 tick 循环的程序化控制(最阻塞——阻止任何游戏状态测试)
2. 没有命令注入接口(次阻塞——Agent 只能写 JSON 文件)
3. 没有运行时状态查询端点(阻止验证)
4. 没有快照/回滚(阻止安全迭代开发)
5. 没有资产语义描述(让 Agent 资产复用脆弱)

这些都不是难题,都有已知工程方案。差距是实现工作量,不是未解研究。

### Q6:什么放 C++,什么放外部 Harness?

**放 C++ 引擎侧——所有必须与 tick 循环同步的东西:**
- 命令队列(命令落缓冲区,在 tick 开始执行)
- 输入注入(`InputSystem` 的 `setGameCommand()`)
- Tick 控制(对外暴露 `tickOneFrame()`)
- 状态序列化(把 `Level` 快照为 JSON 缓冲区)
- 结果编码(类型化 `CommandResult` 结构体)

**放外部 Harness——所有关于推理的东西:**
- LLM 规划与任务分解
- 重试与错误恢复逻辑
- 资产语义描述与目录
- 验证逻辑(比较期望与观测状态)
- 游戏需求追踪与进度评估
- 多步工作流编排

判断依据:"如果这段代码需要知道引擎 tick 即将发生 → C++;如果需要推理下一步做什么 → Harness"。

### Q7:哪些架构改造最能展示 C++ / Agent Infra / 可靠性工程能力?

实习作品集高信号项:
1. **C++ Agent 命令子系统**——类型化命令队列、分发表、结果编码、线程安全协议。展示:C++ 系统编程、并发、接口设计。
2. **快照/回滚机制**——存活场景的内存序列化,失败时恢复。展示:可靠性工程、数据结构、所有权语义。
3. **Python 外部 Agent Harness**——规划循环、命令排序、验证、带回滚重试。展示:Agent 基础设施设计、LLM API 使用、测试 Harness 工程。
4. **带校验的资产目录**——反射输出之上的 JSON schema、组件依赖约束。展示:schema 设计、约束建模、文档工程。

---

## 2. 四个候选架构

### 架构 A:外部 Agent + MCP / JSON 文件接口

**是什么:** 一个外部 Python 进程充当 Agent harness。它调用 LLM API,把响应翻译成对 `engine/asset/` 的 JSON 文件写入,然后信号让引擎 `reloadCurrentLevel()`。引擎照常带 GUI 运行。不改引擎源码。

**Agent 如何操作:**
1. 写带新对象定义的 `1-1.level.json`
2. 发"reload"信号(如写一个引擎轮询的哨兵文件,或加一个最小 HTTP 端点)
3. 等引擎重新加载并渲染
4. 抓引擎日志输出找错误
5. 截图喂给视觉模型做验证

**能力上限:** 编辑期创建/修改场景对象 ✅；不能注入输入测游戏行为 ❌；不能观测运行时物理状态 ❌；不能确定性步进模拟 ❌；不能回滚失败操作 ❌

**引擎改动:** 最小——只一个 reload 信号机制。

**工程复杂度:** 低。Agent harness 直接;差距在于它不能测试什么。

**1–2 月范围:** 能做编辑场景的 Agent,可以组合并展示场景。不能验证游戏逻辑。

**评估:** 这是大多数 Godot MCP 项目实现的。是有用起点,但不足以达到自主游戏开发+行为验证的既定目标。

---

### 架构 B:引擎内 Agent 控制子系统

**是什么:** 新建一个 C++ 模块 `AgentControlSystem`,加入 `RuntimeGlobalContext`,与现有子系统并列。它在后台线程跑 HTTP server(如用 cpp-httplib 这个 header-only 库),接收 JSON 命令,排队等待下次 `logicalTick()` 开始时执行,返回类型化 JSON 结果。基于 LLM 的 Agent harness 在外部运行,与此 server 通信。

**新增 C++ 代码(全部本地化):**
```
engine/source/runtime/function/agent/
  agent_command_system.h       // HTTP server + 命令队列
  agent_command_system.cpp
  agent_command_types.h        // 类型化命令与结果
  agent_scene_snapshot.h       // 内存关卡序列化
  agent_scene_snapshot.cpp
```

**对现有文件的修改:**
- `engine/source/runtime/function/global/global_context.h` — 添加 `m_agent_command_system` 成员
- `engine/source/runtime/engine.cpp` — 在 `logicalTick()` 开头添加 `agentCommandSystem->dispatchQueuedCommands()`
- `engine/source/runtime/function/input/input_system.h` — 添加 `setGameCommand(unsigned int)` 方法

**能力上限:**
- 通过类型化命令创建/修改/删除 GObjects ✅
- 存取关卡 ✅
- 注入输入命令测试游戏逻辑 ✅
- N 个 tick 后观测 GObject 位置和组件状态 ✅
- 取场景快照 ✅
- 失败时回滚到快照 ✅
- 物理单步(通过 `runLogicalTicks(n)` 命令)✅
- 不能不带显示窗口运行 ❌(除非同时加无头模式)
- 不能离屏渲染做视觉验证 ❌

**引擎改动:** 中——新模块 + 对 3 个现有文件的最小触碰。

**工程复杂度:** 中。HTTP server 线程化、带 mutex 的命令队列、快照序列化。

**1–2 月范围:** 完整命令驱动的场景创作、游戏逻辑测试(跳跃、移动)、回滚。没有视觉验证。

---

### 架构 C:完整 AI-Native 架构

**是什么:** 架构 B 加无头模式、通过 Vulkan 离屏 framebuffer 的离屏渲染、物理单步 API、语义资产目录。引擎可以完全不带显示运行,一次一帧步进模拟,把像素数据或结构化状态观测返回给 Agent。

**在 B 基础上的额外改动:**
- `PiccoloEngine::runHeadless(int max_ticks)` — 不带窗口系统的 tick 循环
- `RenderSystem` 里的 Vulkan 离屏渲染路径 — 渲染到 framebuffer,返回像素数据
- `PhysicsScene::stepOne()` — 推进 JoltPhysics 一个确定性步
- `AssetRegistry` — 文件系统爬取 + 目录 JSON 用于语义资产描述
- 完整基于反射的能力 schema 导出为 JSON,Agent 请求时返回

**引擎改动:** 高——触碰渲染管线(非平凡)、主循环、物理、资产系统。

**工程复杂度:** 高。离屏 Vulkan 渲染需要大量渲染管线工作。

**1–2 月范围:** 若限定为:无头模式+离屏渲染(3–4 周)+ 现有架构 B 命令(2–3 周),可行。完整能力集超过 2 个月。

---

### 架构 D:渐进式混合架构

**是什么:** 从架构 B 的最小子集起步(解除 Agent 阻塞的绝对最小集),端到端验证可行,然后按观察到的瓶颈扩展。

**阶段 0(第 1–2 周):最小可行 Agent 接口**
- 给 `InputSystem` 加 `setGameCommand(unsigned int)`(1 个方法,1 小时)
- 通过简单的 UNIX domain socket 或管道暴露 `PiccoloEngine::tickOneFrame()`
- 写一个 Python harness,能调用 LLM、写 JSON 关卡文件、信号 reload、注入输入、通过状态序列化调用读回 GObject 位置

**阶段 1(第 3–5 周):命令系统**
- 实现架构 B 的完整 `AgentCommandSystem`
- 加快照/回滚
- 围绕此 API 写 Python harness

**阶段 2(第 6–8 周):资产目录 + 验证**
- 为所有当前资产手写 `asset_catalog.json`
- 在 harness 里实现验证逻辑(期望 vs 观测状态)
- 端到端测试:Agent 创建可玩的第三人称角色场景

**阶段 3(可选,第 9 周起):无头 + 扩展覆盖**
- 无头模式(移除窗口依赖)
- 基于反射的能力 schema

---

## 3. 推荐

**推荐:架构 D(渐进式混合),以架构 B 为终点状态。**

**理由:**
- 架构 A 无法达成既定目标(经过验证的自主游戏开发)。它不应该是终点。
- 架构 B 在 1–2 月内可行,覆盖核心 Agent 自主性需求,产出作品集价值最高的 C++ 系统工程工作。
- 架构 C 是正确的长期目标,但因离屏渲染复杂度超过 2 月窗口。
- 架构 D 第 2 周就交付可工作代码,提供早期假设验证,第 8 周自然达到架构 B。

**核心赌注:** `AgentCommandSystem` HTTP server + 命令队列 + 快照机制是核心工程挑战。它必须处理 HTTP server 线程和主 tick 线程的并发访问。这不难但需要谨慎。把这个做对是项目技术价值最高的部分。

---

## 4. 目标架构图

```
┌───────────────────────────────────────────────────────────────────┐
│                        外部 Agent Harness (Python)                │
│                                                                    │
│  LLM API (Anthropic/OpenAI) ◄──── 规划循环                         │
│       ↕                               ↕                           │
│  Asset Catalog          Task Tracker ◄──── 验证逻辑               │
│  (asset_catalog.json)                      (期望 vs 观测)          │
│       ↕                               ↕                           │
│               AgentHarness Client (HTTP/JSON)                     │
└─────────────────────────┬─────────────────────────────────────────┘
                          │ JSON over HTTP (localhost)
                          │
┌─────────────────────────▼─────────────────────────────────────────┐
│                        Piccolo 引擎进程                            │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │                   AgentCommandSystem                         │  │
│  │  HTTP Server 线程 ──► CommandQueue (mutex 保护)              │  │
│  │  CommandResult ◄──────── 分发表                              │  │
│  │  SceneSnapshot ◄──────── 内存 Level 序列化                   │  │
│  └──────────────────────────┬────────────────────────────────┘  │
│                              │  在 tick 开始时分发                │
│  ┌────────────────────────────▼──────────────────────────────┐  │
│  │               RuntimeGlobalContext                         │  │
│  │  WorldManager ──► Level ──► GObject ──► Components         │  │
│  │  InputSystem  ──► setGameCommand() [新方法]                 │  │
│  │  AssetManager ──► loadAsset<T>() / saveAsset<T>()          │  │
│  │  PhysicsManager ──► PhysicsScene ──► Jolt step             │  │
│  │  RenderSystem ──► (可选离屏路径)                            │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                    │
│  PiccoloEngine::tickOneFrame()                                     │
│    → agentCommandSystem.dispatchQueuedCommands()  [新]            │
│    → logicalTick()                                                 │
│    → rendererTick()                                                │
└───────────────────────────────────────────────────────────────────┘
```

---

## 5. 模块设计

### 5.1 AgentCommandSystem (C++)

**位置:** `engine/source/runtime/function/agent/`

**职责:**
- 在后台 `std::thread` 跑 HTTP server(端口可配,默认 7890)
- 接受 `/command` 的 JSON POST 请求
- 校验命令类型和参数
- 把合法命令推入 `std::queue<AgentCommand>`,由 `std::mutex` 保护
- 在每个 `logicalTick()` 开始时清空队列并分发每条命令
- 返回类型化 `CommandResult` 结构体,通过反射系统序列化为 JSON

**关键数据结构:**

```cpp
// agent_command_types.h

enum class AgentCommandType {
    CreateObject,     // 从定义 + 覆盖创建 GObject
    DeleteObject,     // 按 ID 删除 GObject
    SetComponentField, // 设置组件上的反射字段
    InvokeMethod,     // 调用反射方法
    SaveLevel,        // 保存当前关卡到磁盘
    LoadLevel,        // 从磁盘或新 URL reload 当前关卡
    SetInput,         // 注入游戏命令位掩码
    TakeSnapshot,     // 把当前关卡序列化到内存缓冲区
    RestoreSnapshot,  // 从内存缓冲区恢复关卡
    QueryObjects,     // 返回 GObjects 列表及组件状态
    GetCapabilities,  // 返回所有反射类型的 JSON schema
    RunTicks,         // 推进 N 个逻辑 tick
};

struct AgentCommand {
    AgentCommandType type;
    std::string      request_id;      // 结果中返回以做关联
    Json             params;          // 类型相关参数
};

struct CommandResult {
    std::string request_id;
    bool        success;
    std::string error_message;        // 成功时为空
    Json        data;                 // 类型相关结果负载
};
```

**线程安全契约:**
- HTTP server 线程:只写 `m_pending_queue`
- Tick 线程:只在 `logicalTick()` 开头读/清空 `m_pending_queue`
- 单个 `std::mutex m_queue_mutex` 保护队列
- 结果写入 `m_completed_results`(同样 mutex 保护),客户端轮询时由 HTTP server 线程读

**engine.cpp 集成点:**
```cpp
void PiccoloEngine::logicalTick(float delta_time)
{
    if (g_runtime_global_context.m_agent_command_system)
        g_runtime_global_context.m_agent_command_system->dispatchQueuedCommands();  // 新

    g_runtime_global_context.m_world_manager->tick(delta_time);
    g_runtime_global_context.m_input_system->tick();
}
```

### 5.2 SceneSnapshot

**位置:** `engine/source/runtime/function/agent/agent_scene_snapshot.h`

```cpp
class SceneSnapshot {
public:
    void capture(const Level& level);     // 把关卡序列化到 m_data (JSON 缓冲区)
    bool restore(Level& level) const;     // 把 m_data 反序列化回关卡
    bool isEmpty() const;
    std::string toJson() const;           // 调试用

private:
    std::string m_data;     // 捕获时刻关卡的 JSON 表示
    std::chrono::steady_clock::time_point m_timestamp;
};
```

快照用 `Level::getAllGObjects()` → 对每个 GObject 调用 `gobject->save(ObjectInstanceRes)` → 序列化为 JSON。恢复从 `ObjectInstanceRes` 重建对象。这安全,因为 `Level::createObject()` 已接受 `ObjectInstanceRes`——这个往返在架构上是受支持的。

**风险:** 只有运行时状态(如当前动画帧、物理速度)不会被 JSON 序列化完整捕获。对 MVP 用例(失败命令后回滚,而非战斗中途状态恢复),这可接受。

### 5.3 Asset Catalog

**位置:** `docs/ai-native-research/asset_catalog.json`(数据文件,无需 C++)

手写的 JSON 目录,由 Python harness 读取,引擎不读。格式:

```json
{
  "assets": [
    {
      "id": "character/player",
      "definition": "asset/objects/character/player/player.object.json",
      "type": "character",
      "description": "第三人称玩家角色,带物理胶囊体、骨骼动画、网格渲染器、马达控制器和第三人称相机。",
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

这不是知识图谱,而是一份人工策展的参考文档,harness 读它来构造合法 `ObjectInstanceRes` JSON 而不幻觉字段名。

### 5.4 外部 Agent Harness (Python)

一个 Python 进程,围绕三个关注点组织:

**Planner(规划器):** 调用 LLM API,传入用户游戏需求和当前引擎状态。产出一串 `AgentCommand` 对象。用资产目录来指导对象创建请求。

**Executor(执行器):** 把命令发给 `AgentCommandSystem` HTTP server。处理结果,瞬态失败重试,破坏性序列前触发快照,失败时触发回滚。

**Verifier(验证器):** 游戏运行 N 个 tick(通过 `RunTicks` 命令)后,读 `QueryObjects` 结果并检查游戏逻辑断言:"当 `jump` 输入激活的帧里角色的 Z 位置是否升高了?"这是基于状态的验证,不是代码评审。

---

## 6. 命令执行协议(状态机)

```
Agent 发送游戏任务描述
         ↓
Planner: 查询能力,查询现有场景状态
         ↓
Planner: 生成计划(一串引擎操作)
         ↓
         ┌─────────── 对计划中每个操作 ──────────────┐
         │                                             │
         │  TakeSnapshot (保存回滚点)                   │
         │         ↓                                   │
         │  执行命令 → 检查 CommandResult.success      │
         │         ↓ 成功              ↓ 失败          │
         │  继续下一操作        RestoreSnapshot        │
         │                               ↓             │
         │                          从失败信息重新规划 │
         └─────────────────────────────────────────────┘
         ↓
SaveLevel (持久化成功场景)
         ↓
SetInput + RunTicks (测试游戏行为)
         ↓
QueryObjects → Verifier 检查状态断言
         ↓ 通过              ↓ 失败
报告成功             重新规划(修复行为)
```

---

## 7. MVP 设计

**目标:** 给定现有 Piccolo 资产库,Agent 创建一个最小但可玩的第三人称游戏:角色出生、玩家可移动和跳跃、相机跟随角色、至少有一面静态墙做碰撞。Agent 验证移动输入注入时角色位置变化,以及跳跃时 Z 坐标升高。

**今天可用的资产:** 玩家角色(player.object.json)、墙对象、粒子发射器。

**Agent 做什么:**
1. 查询资产目录 → 识别 `character/player` 和 `environment/wall`
2. 加载现有关卡模板或从空关卡开始
3. 在 (3, 3, 0) 创建 Player GObject
4. 创建三面墙 GObject 形成小竞技场
5. SaveLevel
6. SetInput(forward) + RunTicks(30) → QueryObjects → 验证 player.position.y 变化
7. SetInput(jump) + RunTicks(10) → QueryObjects → 验证 tick 3–8 期间 player.position.z > 0.1
8. 报告已验证游戏

**MVP 明确不做:**
- 生成新美术资产
- 写 MotorComponent 之外的自定义游戏逻辑
- 创建 AI 敌人角色
- 渲染截图

---

## 8. 分阶段实现路线图

### 阶段 0:解除 Agent 访问阻塞(第 1–2 周)

**验收标准:**
- 外部 Python 脚本能写合法关卡 JSON、信号引擎 reload、通过简单端点读回 GObjects 列表及其组件字段
- 输入注入:Python 能设 `m_game_command` 位掩码,角色移动

**工作:**
- 给 `InputSystem` 加 `setGameCommand(unsigned int)`(1 行)
- 加最小进程内 Python-C++ 桥或简单文件轮询做 reload 信号
- 写 Python harness 客户端骨架,带 `reload()`、`query_objects()`、`set_input()` 调用

**风险:** 低。这些是最小改动,无新子系统。

### 阶段 1:AgentCommandSystem(第 3–6 周)

**验收标准:**
- 完整命令集(创建、删除、设字段、快照、恢复、跑 tick、查询)通过 HTTP/JSON 工作
- 线程安全命令队列已验证(快速并发请求下无崩溃)
- Python harness 端到端跑 MVP 场景

**工作:**
- 用 cpp-httplib 实现 `AgentCommandSystem` C++ 模块
- 加 `SceneSnapshot` 类
- 扩展 Python harness 到完整执行循环带回滚

**风险:** 中。C++ 的 HTTP server 线程化需要谨慎。cpp-httplib 是 header-only 且经过充分测试,降低风险。命令分发循环不得阻塞 tick 线程。

### 阶段 2:资产目录 + 验证(第 7–8 周)

**验收标准:**
- 手写 `asset_catalog.json` 覆盖所有当前 Piccolo 资产
- Verifier 模块确认:角色移动、角色跳跃、墙碰撞
- 完整端到端 demo:用户输入"做个第三人称游戏",Agent 产出并验证

**工作:**
- 写 `asset_catalog.json`(手工,约 100 行)
- 在 Python harness 实现 `Verifier` 类,带可配状态断言
- 完整 LLM 规划循环集成

**风险:** 目录低。完整规划循环中——取决于 LLM 指令遵循质量。

### 阶段 3:无头模式 + 反射 schema(可选,第 9 周起)

**验收标准:**
- 引擎启动并运行不需要显示窗口
- Agent 能以机器可读 JSON schema 格式枚举所有反射类型及其字段

**工作:**
- 加 `PiccoloEngine::runHeadless(int max_ticks)` 方法
- 无头模式有条件跳过 `WindowSystem` 和 `RenderSystem` 初始化
- 写 `GetCapabilities` 命令处理器,把所有 `TypeMeta` 条目序列化为 JSON schema

**风险:** 中。无头渲染需要解开渲染系统与逻辑 tick 的耦合。JoltPhysics 无显示依赖。动画可能需要无操作渲染路径。

---

## 9. 关键技术风险

### 风险 1:命令队列死锁或竞态
**场景:** HTTP 线程写队列时 tick 线程在清空它。  
**缓解:** 用 `std::mutex` + `std::queue`;tick 线程在 `std::lock_guard` 下清空,HTTP 线程在 `std::lock_guard` 下写入。执行命令时绝不持锁(先清空,锁外执行)。

### 风险 2:场景快照不完整
**场景:** 快照捕获 JSON 状态但漏了运行时专属字段(动画帧、物理速度)。恢复产生合法但不同的状态。  
**缓解:** MVP 接受(快照用于失败操作回滚,不是游戏中状态恢复)。记录此限制。需要时再加物理速度到快照。

### 风险 3:Agent 命令的反射缺口
**场景:** Agent 试图设一个未 `META(Enable)` 标记的字段,或调用未注册到反射系统的方法。  
**缓解:** `GetCapabilities` 命令只返回实际已注册的内容——Agent 只看到它合法能访问的。记录哪些字段需要 `META(Enable)` 标注作为贡献者扩展点。

### 风险 4:LLM 幻觉非法字段路径
**场景:** LLM 生成 `SetComponentField` 命令,`field_path = "MotorComponent.m_speed"`,但真实字段是 `m_move_speed`。  
**缓解:** `GetCapabilities` 命令返回精确字段名。Harness 在生成字段访问命令前应总是调 `GetCapabilities` 并把结果喂入 LLM 上下文。

### 风险 5:cpp-httplib 增加依赖
**场景:** 用 cpp-httplib 构建 Piccolo 需要把它加到 CMake 构建。  
**缓解:** cpp-httplib 是单头库,无外部依赖。加到 `engine/3rdparty/` 并加入 `CMakeLists.txt`。10 分钟改动。或者用 named pipe 或 UNIX domain socket 完全避免新依赖。

---

## 10. 构建前需要验证什么

三个技术实验,按风险降低程度排序,在写主实现前跑:

**实验 1(1 天):** 给 `InputSystem` 加 `setGameCommand(unsigned int cmd)`,在 `startEngine()` 后从简单测试 harness 调用它,验证角色移动。这验证了核心输入注入假设,先于任何更大 C++ 工作。

**实验 2(1 天):** 用现有反射 + 序列化器把一个 `Level` 序列化为 JSON,写磁盘,reload,验证 GObjects 存在。这验证快照往返假设。`Level::save()` 已经这样做——问题是 `clear()` 后 `Level::createObject()` 是否产生相同运行时状态。

**实验 3(2 天):** 引擎主循环跑在主线程时,在 `std::thread` 里跑一个简单 HTTP server;让 HTTP server 通过 mutex 保护标志推一个"reload level"信号,tick 循环检查它。验证快速请求下无崩溃。这在构建完整命令系统前验证线程模型。

这些实验都不需要承诺完整架构。每个都可以在独立分支上构建,确认可行,然后在主实现吸收已验证设计后丢弃。
