#define CPPHTTPLIB_OPENSSL_SUPPORT 0
// Suppress OpenSSL to keep zero external dependencies
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#undef CPPHTTPLIB_OPENSSL_SUPPORT
#endif

#include "runtime/function/agent/agent_mcp_server.h"
#include "runtime/function/agent/agent_command_system.h"
#include "runtime/function/agent/command_types.h"
#include "runtime/core/base/macro.h"

#include <httplib.h>
#include <thread>
#include <atomic>

// json11 is Piccolo's JSON library
#include "runtime/core/meta/json.h"

namespace Piccolo
{
    struct AgentMCPServer::Impl
    {
        httplib::Server     server;
        std::thread         server_thread;
        std::atomic<bool>   running {false};
    };

    AgentMCPServer::AgentMCPServer(AgentCommandSystem* cmd_system, int port)
        : m_cmd_system(cmd_system), m_port(port), m_impl(std::make_unique<Impl>())
    {}

    AgentMCPServer::~AgentMCPServer()
    {
        stop();
    }

    void AgentMCPServer::start()
    {
        m_impl->server.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            std::string response = handleRequest(req.body);
            res.set_content(response, "application/json");
        });

        m_impl->running = true;
        m_impl->server_thread = std::thread([this]() {
            LOG_INFO("AgentMCPServer listening on port {}", m_port);
            m_impl->server.listen("0.0.0.0", m_port);
        });
    }

    void AgentMCPServer::stop()
    {
        if (m_impl->running)
        {
            m_impl->server.stop();
            if (m_impl->server_thread.joinable())
                m_impl->server_thread.join();
            m_impl->running = false;
        }
    }

    // -----------------------------------------------------------------
    // JSON-RPC 2.0 dispatcher
    // -----------------------------------------------------------------

    static Json makeError(int id, int code, const std::string& message)
    {
        Json::object err_obj{{"code", code}, {"message", message}};
        return Json(Json::object{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json(err_obj)}});
    }

    static Json makeResult(int id, const Json& result)
    {
        return Json(Json::object{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    }

    std::string AgentMCPServer::handleRequest(const std::string& body)
    {
        std::string err;
        Json        req = Json::parse(body, err);
        if (!err.empty())
            return makeError(0, -32700, "Parse error: " + err).dump();

        int         id     = req["id"].int_value();
        std::string method = req["method"].string_value();

        if (method == "initialize")
        {
            Json server_info = Json(Json::object{{"name", "piccolo-mcp"}, {"version", "0.1.0"}});
            Json tools_cap   = Json(Json::object{{"listChanged", false}});
            Json caps        = Json(Json::object{{"tools", tools_cap}});
            Json result      = Json(Json::object{
                {"protocolVersion", "2024-11-05"},
                {"serverInfo",      server_info},
                {"capabilities",    caps}
            });
            return makeResult(id, result).dump();
        }

        if (method == "tools/list")
        {
            std::string err2;
            Json tools = Json::parse(toolListJson(), err2);
            return makeResult(id, Json(Json::object{{"tools", tools}})).dump();
        }

        if (method == "tools/call")
        {
            const Json& params    = req["params"];
            std::string tool_name = params["name"].string_value();
            std::string args_str  = params["arguments"].dump();

            std::string dispatch_result = dispatchToolCall(tool_name, args_str);

            std::string err3;
            Json        result_json = Json::parse(dispatch_result, err3);

            if (!err3.empty())
            {
                Json text_item = Json(Json::object{{"type", "text"}, {"text", dispatch_result}});
                Json content   = Json(Json::array{text_item});
                return makeResult(id, Json(Json::object{{"content", content}})).dump();
            }

            bool success = result_json["success"].bool_value();
            std::string payload   = result_json["json_payload"].string_value();
            std::string error_msg = result_json["error_message"].string_value();

            if (!success)
            {
                Json text_item = Json(Json::object{{"type", "text"}, {"text", "Error: " + error_msg}});
                Json content   = Json(Json::array{text_item});
                return makeResult(id, Json(Json::object{{"content", content}, {"isError", true}})).dump();
            }

            Json text_item   = Json(Json::object{{"type", "text"}, {"text", payload}});
            Json content     = Json(Json::array{text_item});
            return makeResult(id, Json(Json::object{{"content", content}})).dump();
        }

        if (method == "notifications/initialized")
        {
            // No response needed for notifications
            return "";
        }

        return makeError(id, -32601, "Method not found: " + method).dump();
    }

    std::string AgentMCPServer::dispatchToolCall(const std::string& tool_name,
                                                  const std::string& args_json)
    {
        std::string err;
        Json        args = Json::parse(args_json, err);
        if (!err.empty())
        {
            Json r = Json(Json::object{{"success", false},
                                       {"error_message", "Invalid arguments JSON: " + err},
                                       {"json_payload",  ""}});
            return r.dump();
        }

        auto makeCommandResult = [](const CommandResult& r) -> std::string {
            Json j = Json(Json::object{{"success",       r.success},
                                       {"error_message", r.error_message},
                                       {"json_payload",  r.json_payload}});
            return j.dump();
        };

        Command cmd;

        if (tool_name == "get_capabilities")
        {
            cmd.type   = CommandType::GetCapabilities;
            cmd.params = std::monostate{};
        }
        else if (tool_name == "get_state")
        {
            cmd.type   = CommandType::GetState;
            cmd.params = std::monostate{};
        }
        else if (tool_name == "query_objects")
        {
            cmd.type   = CommandType::QueryObjects;
            cmd.params = std::monostate{};
        }
        else if (tool_name == "create_object")
        {
            CreateObjectParams p;
            p.name = args["name"].string_value();
            if (args["position"].is_object())
            {
                p.position.x = (float)args["position"]["x"].number_value();
                p.position.y = (float)args["position"]["y"].number_value();
                p.position.z = (float)args["position"]["z"].number_value();
            }
            for (const auto& c : args["components"].array_items())
                p.components.push_back(c.string_value());
            cmd.type   = CommandType::CreateObject;
            cmd.params = std::move(p);
        }
        else if (tool_name == "delete_object")
        {
            cmd.type   = CommandType::DeleteObject;
            cmd.params = args["name"].string_value();
        }
        else if (tool_name == "set_field")
        {
            SetFieldParams p;
            p.object_name    = args["object_name"].string_value();
            p.component_type = args["component_type"].string_value();
            p.field_name     = args["field_name"].string_value();
            p.value_json     = args["value"].dump();
            cmd.type   = CommandType::SetField;
            cmd.params = std::move(p);
        }
        else if (tool_name == "get_field")
        {
            GetFieldParams p;
            p.object_name    = args["object_name"].string_value();
            p.component_type = args["component_type"].string_value();
            p.field_name     = args["field_name"].string_value();
            cmd.type   = CommandType::GetField;
            cmd.params = std::move(p);
        }
        else if (tool_name == "set_input")
        {
            SetInputParams p;
            p.command_mask    = (unsigned int)args["command_mask"].int_value();
            p.duration_frames = args["duration_frames"].is_number()
                                    ? args["duration_frames"].int_value()
                                    : -1;
            cmd.type   = CommandType::SetInput;
            cmd.params = std::move(p);
        }
        else if (tool_name == "step")
        {
            StepParams p;
            p.n_frames = args["n_frames"].is_number() ? args["n_frames"].int_value() : 1;
            cmd.type   = CommandType::Step;
            cmd.params = std::move(p);
        }
        else if (tool_name == "snapshot")
        {
            SnapshotParams p;
            p.action = args["action"].string_value();
            p.tag    = args["tag"].is_string() ? args["tag"].string_value() : "default";
            cmd.type   = CommandType::Snapshot;
            cmd.params = std::move(p);
        }
        else
        {
            CommandResult r;
            r.success = false;
            r.error_message = "Unknown tool: " + tool_name;
            return makeCommandResult(r);
        }

        CommandResult result = m_cmd_system->submitAndWait(std::move(cmd));
        return makeCommandResult(result);
    }

    std::string AgentMCPServer::toolListJson()
    {
        return R"JSON([
  {
    "name": "get_capabilities",
    "description": "Returns the component schema: all Component types, their fields, units and valid ranges.",
    "inputSchema": {"type": "object", "properties": {}}
  },
  {
    "name": "get_state",
    "description": "Returns a summary of all GObjects in the current level (name, id, position).",
    "inputSchema": {"type": "object", "properties": {}}
  },
  {
    "name": "query_objects",
    "description": "Alias for get_state.",
    "inputSchema": {"type": "object", "properties": {}}
  },
  {
    "name": "create_object",
    "description": "Create a new GObject in the current level.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "name": {"type": "string", "description": "Unique name for the new object"},
        "position": {
          "type": "object",
          "properties": {"x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"}}
        },
        "components": {"type": "array", "items": {"type": "string"}, "description": "Component type names to attach"}
      },
      "required": ["name"]
    }
  },
  {
    "name": "delete_object",
    "description": "Delete a GObject by name.",
    "inputSchema": {
      "type": "object",
      "properties": {"name": {"type": "string"}},
      "required": ["name"]
    }
  },
  {
    "name": "set_field",
    "description": "Set a component field on a GObject. Use get_capabilities to discover available fields.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "object_name":    {"type": "string"},
        "component_type": {"type": "string", "description": "e.g. MotorComponent"},
        "field_name":     {"type": "string", "description": "e.g. m_move_speed"},
        "value":          {"description": "The new value (number, bool, string, or x/y/z object for Vector3)"}
      },
      "required": ["object_name", "component_type", "field_name", "value"]
    }
  },
  {
    "name": "get_field",
    "description": "Read a component field value from a GObject.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "object_name":    {"type": "string"},
        "component_type": {"type": "string"},
        "field_name":     {"type": "string"}
      },
      "required": ["object_name", "component_type", "field_name"]
    }
  },
  {
    "name": "set_input",
    "description": "Inject a game input command. command_mask bits: 1=forward 2=backward 4=left 8=right 16=jump 64=sprint. duration_frames=-1 means hold indefinitely.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "command_mask":    {"type": "integer"},
        "duration_frames": {"type": "integer", "default": -1}
      },
      "required": ["command_mask"]
    }
  },
  {
    "name": "step",
    "description": "Advance the engine exactly n_frames logic ticks, then return the current scene state.",
    "inputSchema": {
      "type": "object",
      "properties": {"n_frames": {"type": "integer", "default": 1}}
    }
  },
  {
    "name": "snapshot",
    "description": "Save or restore a named scene snapshot. action='save' captures current state; action='restore' rolls back to it.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "action": {"type": "string", "enum": ["save", "restore"]},
        "tag":    {"type": "string", "default": "default"}
      },
      "required": ["action"]
    }
  }
])JSON";
    }

} // namespace Piccolo
