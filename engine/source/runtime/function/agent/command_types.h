#pragma once

#include "runtime/core/math/vector3.h"

#include <future>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Piccolo
{
    // Result returned from every command execution
    struct CommandResult
    {
        bool        success {true};
        std::string error_message;
        std::string json_payload; // JSON string returned to Agent
    };

    enum class CommandType
    {
        GetCapabilities,
        CreateObject,
        DeleteObject,
        SetField,
        GetField,
        SetInput,
        Step,
        Snapshot,   // save or restore
        GetState,
        QueryObjects,
    };

    struct CreateObjectParams
    {
        std::string              name;
        Vector3                  position {Vector3::ZERO};
        std::vector<std::string> components;
    };

    struct SetFieldParams
    {
        std::string object_name;
        std::string component_type;
        std::string field_name;
        std::string value_json;
    };

    struct GetFieldParams
    {
        std::string object_name;
        std::string component_type;
        std::string field_name;
    };

    struct SetInputParams
    {
        unsigned int command_mask {0};
        int          duration_frames {-1};
    };

    struct StepParams
    {
        int n_frames {1};
    };

    struct SnapshotParams
    {
        std::string action; // "save" or "restore"
        std::string tag {"default"};
    };

    struct Command
    {
        CommandType type;
        std::variant<std::monostate,
                     CreateObjectParams,
                     SetFieldParams,
                     GetFieldParams,
                     SetInputParams,
                     StepParams,
                     SnapshotParams,
                     std::string>       params; // std::string = object name for delete
        std::promise<CommandResult>     promise;

        // non-copyable, movable
        Command()                        = default;
        Command(Command&&)               = default;
        Command& operator=(Command&&)    = default;
        Command(const Command&)          = delete;
        Command& operator=(const Command&) = delete;
    };
} // namespace Piccolo
