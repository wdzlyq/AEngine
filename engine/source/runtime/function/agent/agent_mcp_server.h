#pragma once

#include <memory>
#include <string>
#include <thread>

namespace Piccolo
{
    class AgentCommandSystem;

    // Minimal JSON-RPC 2.0 / MCP server embedded in the engine process.
    // Runs on a dedicated thread; dispatches tool calls to AgentCommandSystem.
    class AgentMCPServer
    {
    public:
        explicit AgentMCPServer(AgentCommandSystem* cmd_system, int port = 7777);
        ~AgentMCPServer();

        void start();
        void stop();

    private:
        std::string handleRequest(const std::string& body);
        std::string dispatchToolCall(const std::string& tool_name,
                                     const std::string& args_json);
        std::string toolListJson();

        AgentCommandSystem* m_cmd_system;
        int                 m_port;

        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace Piccolo
