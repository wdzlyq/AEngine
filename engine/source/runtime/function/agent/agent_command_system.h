#pragma once

#include "runtime/function/agent/command_types.h"
#include "runtime/function/agent/scene_snapshot.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

namespace Piccolo
{
    class AgentMCPServer;

    class AgentCommandSystem
    {
    public:
        ~AgentCommandSystem();
        void initialize(int mcp_port = 7777);
        void shutdown();

        // Called by AgentMCPServer thread: enqueue a command and block until result
        CommandResult submitAndWait(Command cmd);

        // Called by main loop at frame boundary (top of tickOneFrame)
        void tick();

    private:
        CommandResult execute(Command& cmd);

        CommandResult cmdGetCapabilities();
        CommandResult cmdCreateObject(const CreateObjectParams& p);
        CommandResult cmdDeleteObject(const std::string& name);
        CommandResult cmdSetField(const SetFieldParams& p);
        CommandResult cmdGetField(const GetFieldParams& p);
        CommandResult cmdSetInput(const SetInputParams& p);
        CommandResult cmdStep(StepParams& p, std::promise<CommandResult> promise);
        CommandResult cmdSnapshot(const SnapshotParams& p);
        CommandResult cmdGetState();
        CommandResult cmdQueryObjects();

        // --- command queue ---
        struct PendingCommand
        {
            Command cmd;
        };
        std::queue<PendingCommand>  m_queue;
        std::mutex                  m_queue_mutex;

        // --- step command state: runs over multiple frames ---
        bool                            m_step_active {false};
        int                             m_step_frames_remaining {0};
        std::promise<CommandResult>     m_step_promise;

        // --- snapshots ---
        std::unordered_map<std::string, SceneSnapshot> m_snapshots;

        // --- embedded MCP server ---
        std::unique_ptr<AgentMCPServer> m_mcp_server;
    };
} // namespace Piccolo
