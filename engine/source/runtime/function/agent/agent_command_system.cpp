#include "runtime/function/agent/agent_command_system.h"

#include "runtime/core/base/macro.h"
#include "runtime/core/meta/json.h"
#include "runtime/core/meta/reflection/reflection.h"
#include "runtime/core/meta/serializer/serializer.h"
#include "runtime/engine.h"
#include "runtime/function/agent/agent_mcp_server.h"
#include "runtime/function/framework/component/transform/transform_component.h"
#include "runtime/function/framework/level/level.h"
#include "runtime/function/framework/object/object.h"
#include "runtime/function/framework/world/world_manager.h"
#include "runtime/function/global/global_context.h"
#include "runtime/function/input/input_system.h"
#include "runtime/resource/config_manager/config_manager.h"
#include "runtime/resource/res_type/common/object.h"

#include <filesystem>
#include <fstream>

namespace Piccolo
{
    // Explicit destructor so unique_ptr<AgentMCPServer> works with forward declaration
    AgentCommandSystem::~AgentCommandSystem() = default;

    static std::string readFile(const std::string& path)
    {
        std::ifstream f(path);
        if (!f.is_open())
            return "";
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        return content;
    }

    void AgentCommandSystem::initialize(int mcp_port)
    {
        m_mcp_server = std::make_unique<AgentMCPServer>(this, mcp_port);
        m_mcp_server->start();
        LOG_INFO("AgentCommandSystem initialized, MCP server on port {}", mcp_port);
    }

    void AgentCommandSystem::shutdown()
    {
        if (m_mcp_server)
        {
            m_mcp_server->stop();
            m_mcp_server.reset();
        }
    }

    CommandResult AgentCommandSystem::submitAndWait(Command cmd)
    {
        std::future<CommandResult> fut = cmd.promise.get_future();

        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_queue.push(PendingCommand{std::move(cmd)});
        }

        return fut.get(); // blocks until main loop executes the command
    }

    void AgentCommandSystem::tick()
    {
        // Process any pending step command first
        if (m_step_active)
        {
            --m_step_frames_remaining;
            if (m_step_frames_remaining <= 0)
            {
                m_step_active = false;
                // Return current state as result
                m_step_promise.set_value(cmdGetState());
            }
            return;
        }

        // Drain the command queue
        while (true)
        {
            PendingCommand pc;
            {
                std::lock_guard<std::mutex> lock(m_queue_mutex);
                if (m_queue.empty())
                    break;
                pc = std::move(m_queue.front());
                m_queue.pop();
            }

            CommandResult result = execute(pc.cmd);

            // Step commands move the promise into m_step_promise; don't
            // set_value here — it will be fulfilled when frames complete.
            if (pc.cmd.type == CommandType::Step && m_step_active)
                break;

            pc.cmd.promise.set_value(std::move(result));

            // If a step command was activated, break out and let frames run
            if (m_step_active)
                break;
        }
    }

    CommandResult AgentCommandSystem::execute(Command& cmd)
    {
        switch (cmd.type)
        {
            case CommandType::GetCapabilities:
                return cmdGetCapabilities();
            case CommandType::CreateObject:
                return cmdCreateObject(std::get<CreateObjectParams>(cmd.params));
            case CommandType::DeleteObject:
                return cmdDeleteObject(std::get<std::string>(cmd.params));
            case CommandType::SetField:
                return cmdSetField(std::get<SetFieldParams>(cmd.params));
            case CommandType::GetField:
                return cmdGetField(std::get<GetFieldParams>(cmd.params));
            case CommandType::SetInput:
                return cmdSetInput(std::get<SetInputParams>(cmd.params));
            case CommandType::Step:
                return cmdStep(std::get<StepParams>(cmd.params), std::move(cmd.promise));
            case CommandType::Snapshot:
                return cmdSnapshot(std::get<SnapshotParams>(cmd.params));
            case CommandType::GetState:
                return cmdGetState();
            case CommandType::QueryObjects:
                return cmdQueryObjects();
            default:
                return {false, "Unknown command type", ""};
        }
    }

    // --- Individual command implementations ---

    CommandResult AgentCommandSystem::cmdGetCapabilities()
    {
        auto asset_folder = g_runtime_global_context.m_config_manager->getAssetFolder();
        std::string schema_path = (asset_folder / "agent" / "component_schema.json").string();
        std::string schema = readFile(schema_path);
        if (schema.empty())
            return {false, "component_schema.json not found at " + schema_path, ""};
        return {true, "", schema};
    }

    CommandResult AgentCommandSystem::cmdGetState()
    {
        auto level_ptr = g_runtime_global_context.m_world_manager->getCurrentActiveLevel().lock();
        if (!level_ptr)
            return {false, "No active level", ""};

        const LevelObjectsMap& objects = level_ptr->getAllGObjects();

        Json::array obj_array;
        for (const auto& [id, obj_ptr] : objects)
        {
            if (!obj_ptr)
                continue;

            Vector3 pos {Vector3::ZERO};
            auto* tc = obj_ptr->tryGetComponentConst<TransformComponent>("TransformComponent");
            if (tc)
                pos = tc->getPosition();

            obj_array.push_back(Json::object {
                {"name", obj_ptr->getName()},
                {"id", (int)id},
                {"position", Json::object {{"x", pos.x}, {"y", pos.y}, {"z", pos.z}}}
            });
        }

        std::string payload = Json(Json::object{{"objects", obj_array}}).dump();
        return {true, "", payload};
    }

    CommandResult AgentCommandSystem::cmdQueryObjects()
    {
        return cmdGetState();
    }

    CommandResult AgentCommandSystem::cmdCreateObject(const CreateObjectParams& p)
    {
        // For Phase 1: use a simplified approach — write a JSON object file
        // and call reload. Full programmatic creation requires ObjectInstanceRes
        // construction which is complex. This is a placeholder that returns
        // guidance until the full implementation is wired.
        //
        // TODO: construct ObjectInstanceRes in-memory and call Level::createObject
        return {false,
                "create_object not yet fully implemented in Phase 1. "
                "Use set_field on existing objects, or write level JSON + reload.",
                ""};
    }

    CommandResult AgentCommandSystem::cmdDeleteObject(const std::string& name)
    {
        auto level_ptr = g_runtime_global_context.m_world_manager->getCurrentActiveLevel().lock();
        if (!level_ptr)
            return {false, "No active level", ""};

        const LevelObjectsMap& objects = level_ptr->getAllGObjects();
        for (const auto& [id, obj_ptr] : objects)
        {
            if (obj_ptr && obj_ptr->getName() == name)
            {
                level_ptr->deleteGObjectByID(id);
                return {true, "", Json(Json::object{{"deleted", name}}).dump()};
            }
        }
        return {false, "Object not found: " + name, ""};
    }

    CommandResult AgentCommandSystem::cmdSetField(const SetFieldParams& p)
    {
        auto level_ptr = g_runtime_global_context.m_world_manager->getCurrentActiveLevel().lock();
        if (!level_ptr)
            return {false, "No active level", ""};

        const LevelObjectsMap& objects = level_ptr->getAllGObjects();
        for (const auto& [id, obj_ptr] : objects)
        {
            if (!obj_ptr || obj_ptr->getName() != p.object_name)
                continue;

            auto components = obj_ptr->getComponents();
            for (auto& comp : components)
            {
                if (comp.getTypeName() != p.component_type)
                    continue;

                void* instance = comp.operator->();
                Reflection::TypeMeta meta = Reflection::TypeMeta::newMetaFromName(p.component_type);
                Reflection::FieldAccessor fa = meta.getFieldByName(p.field_name.c_str());
                if (!fa.getFieldName())
                    return {false, "Field not found: " + p.field_name, ""};

                // Parse the value JSON
                std::string err;
                Json        val_json = Json::parse(p.value_json, err);
                if (!err.empty())
                    return {false, "Invalid value JSON: " + err, ""};

                // Get the field type name to decide how to set
                const char* field_type = fa.getFieldTypeName();
                Reflection::TypeMeta field_meta;
                bool is_reflectable = fa.getTypeMeta(field_meta);

                if (is_reflectable && field_meta.isValid())
                {
                    // It's a complex type — use reflection-based JSON read
                    void* field_ptr = fa.get(instance);
                    Reflection::TypeMeta::newFromNameAndJson(field_type, val_json);
                    // Use Serializer::read approach for complex types
                    // For now, support Vector3 explicitly
                    if (std::string(field_type) == "Vector3")
                    {
                        Vector3* v = static_cast<Vector3*>(field_ptr);
                        v->x = val_json["x"].number_value();
                        v->y = val_json["y"].number_value();
                        v->z = val_json["z"].number_value();
                        fa.set(instance, v);
                        return {true, "", Json(Json::object{{"set", true}}).dump()};
                    }
                    return {false, std::string("Complex type '") + field_type + "' set not yet supported", ""};
                }

                // Simple scalar types
                void* field_ptr = fa.get(instance);
                if (std::string(field_type) == "float")
                {
                    float val = (float)val_json.number_value();
                    fa.set(instance, &val);
                }
                else if (std::string(field_type) == "int" || std::string(field_type) == "unsigned int")
                {
                    int val = val_json.int_value();
                    fa.set(instance, &val);
                }
                else if (std::string(field_type) == "bool")
                {
                    bool val = val_json.bool_value();
                    fa.set(instance, &val);
                }
                else if (std::string(field_type) == "std::string")
                {
                    // string field — need to handle as std::string
                    std::string val = val_json.string_value();
                    fa.set(instance, &val);
                }
                else
                {
                    return {false, std::string("Unsupported field type: ") + field_type, ""};
                }

                return {true, "", Json(Json::object{{"set", true}}).dump()};
            }
            return {false, "Component not found: " + p.component_type, ""};
        }
        return {false, "Object not found: " + p.object_name, ""};
    }

    CommandResult AgentCommandSystem::cmdGetField(const GetFieldParams& p)
    {
        auto level_ptr = g_runtime_global_context.m_world_manager->getCurrentActiveLevel().lock();
        if (!level_ptr)
            return {false, "No active level", ""};

        const LevelObjectsMap& objects = level_ptr->getAllGObjects();
        for (const auto& [id, obj_ptr] : objects)
        {
            if (!obj_ptr || obj_ptr->getName() != p.object_name)
                continue;

            auto components = obj_ptr->getComponents();
            for (auto& comp : components)
            {
                if (comp.getTypeName() != p.component_type)
                    continue;

                void* instance = comp.operator->();
                Reflection::TypeMeta meta = Reflection::TypeMeta::newMetaFromName(p.component_type);
                Reflection::FieldAccessor fa = meta.getFieldByName(p.field_name.c_str());
                if (!fa.getFieldName())
                    return {false, "Field not found: " + p.field_name, ""};

                const char* field_type = fa.getFieldTypeName();
                void*       field_ptr  = fa.get(instance);

                // Return as JSON
                if (std::string(field_type) == "float")
                {
                    float* v = static_cast<float*>(field_ptr);
                    return {true, "", Json(Json::object{{"value", *v}}).dump()};
                }
                else if (std::string(field_type) == "int" || std::string(field_type) == "unsigned int")
                {
                    int* v = static_cast<int*>(field_ptr);
                    return {true, "", Json(Json::object{{"value", *v}}).dump()};
                }
                else if (std::string(field_type) == "bool")
                {
                    bool* v = static_cast<bool*>(field_ptr);
                    return {true, "", Json(Json::object{{"value", *v}}).dump()};
                }
                else if (std::string(field_type) == "Vector3")
                {
                    Vector3* v = static_cast<Vector3*>(field_ptr);
                    Json v3 = Json(Json::object{{"x", v->x}, {"y", v->y}, {"z", v->z}});
                    return {true, "", Json(Json::object{{"value", v3}}).dump()};
                }

                // Use reflection serialization for complex types
                std::string type_name = field_type;
                Json        written   = Reflection::TypeMeta::writeByName(type_name, field_ptr);
                return {true, "", Json(Json::object{{"value", written}}).dump()};
            }
            return {false, "Component not found: " + p.component_type, ""};
        }
        return {false, "Object not found: " + p.object_name, ""};
    }

    CommandResult AgentCommandSystem::cmdSetInput(const SetInputParams& p)
    {
        auto* input = g_runtime_global_context.m_input_system.get();
        if (!input)
            return {false, "InputSystem not available", ""};

        input->setGameCommand(p.command_mask, p.duration_frames);

        // Switch to Game Mode so the character tick actually runs.
        // The motor component only ticks when g_is_editor_mode == false.
        if (p.command_mask != 0)
            g_is_editor_mode = false;

        Json r = Json(Json::object{{"mask", (int)p.command_mask},
                                   {"duration_frames", p.duration_frames}});
        return {true, "", r.dump()};
    }

    CommandResult AgentCommandSystem::cmdStep(StepParams& p, std::promise<CommandResult> promise)
    {
        if (m_step_active)
        {
            return {false, "A step is already in progress", ""};
        }

        if (p.n_frames <= 0)
            p.n_frames = 1;

        m_step_active         = true;
        m_step_frames_remaining = p.n_frames;
        m_step_promise        = std::move(promise);

        // Return empty — the actual result will be set when frames complete
        // This return value is not used because the promise was already moved
        return {true, "", ""};
    }

    CommandResult AgentCommandSystem::cmdSnapshot(const SnapshotParams& p)
    {
        auto level_ptr = g_runtime_global_context.m_world_manager->getCurrentActiveLevel().lock();
        if (!level_ptr)
            return {false, "No active level", ""};

        if (p.action == "save")
        {
            SceneSnapshot snap;
            snap.capture(level_ptr.get());
            m_snapshots[p.tag] = std::move(snap);
            Json saved = Json(Json::object{{"saved", p.tag},
                                           {"size_bytes", (int)m_snapshots[p.tag].json_data.size()}});
            return {true, "", saved.dump()};
        }
        else if (p.action == "restore")
        {
            auto it = m_snapshots.find(p.tag);
            if (it == m_snapshots.end())
                return {false, "Snapshot not found: " + p.tag, ""};
            it->second.restore(level_ptr.get());
            return {true, "", Json(Json::object{{"restored", p.tag}}).dump()};
        }
        return {false, "Invalid action: " + p.action + " (use 'save' or 'restore')", ""};
    }

} // namespace Piccolo
