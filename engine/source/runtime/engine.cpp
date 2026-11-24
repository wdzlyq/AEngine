#include "runtime/engine.h"

#include "runtime/core/base/macro.h"
#include "runtime/core/meta/reflection/reflection_register.h"
#include "runtime/core/meta/json.h"

#include "runtime/function/framework/component/transform/transform_component.h"
#include "runtime/function/framework/level/level.h"
#include "runtime/function/framework/object/object.h"
#include "runtime/function/framework/world/world_manager.h"
#include "runtime/function/global/global_context.h"
#include "runtime/function/input/input_system.h"
#include "runtime/function/agent/agent_command_system.h"
#include "runtime/function/particle/particle_manager.h"
#include "runtime/function/physics/physics_manager.h"
#include "runtime/function/render/render_system.h"
#include "runtime/function/render/window_system.h"
#include "runtime/function/render/debugdraw/debug_draw_manager.h"
#include "runtime/resource/config_manager/config_manager.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace Piccolo
{
    bool                            g_is_editor_mode {false};
    std::unordered_set<std::string> g_editor_tick_component_types {};

    void PiccoloEngine::startEngine(const std::string& config_file_path)
    {
        Reflection::TypeMetaRegister::metaRegister();

        g_runtime_global_context.startSystems(config_file_path);

        LOG_INFO("engine start");
    }

    void PiccoloEngine::shutdownEngine()
    {
        LOG_INFO("engine shutdown");

        g_runtime_global_context.shutdownSystems();

        Reflection::TypeMetaRegister::metaUnregister();
    }

    void PiccoloEngine::initialize() {}
    void PiccoloEngine::clear() {}

    void PiccoloEngine::run()
    {
        std::shared_ptr<WindowSystem> window_system = g_runtime_global_context.m_window_system;
        ASSERT(window_system);

        while (!window_system->shouldClose())
        {
            const float delta_time = calculateDeltaTime();
            tickOneFrame(delta_time);
        }
    }

    float PiccoloEngine::calculateDeltaTime()
    {
        float delta_time;
        {
            using namespace std::chrono;

            steady_clock::time_point tick_time_point = steady_clock::now();
            duration<float> time_span = duration_cast<duration<float>>(tick_time_point - m_last_tick_time_point);
            delta_time                = time_span.count();

            m_last_tick_time_point = tick_time_point;
        }
        return delta_time;
    }

    bool PiccoloEngine::tickOneFrame(float delta_time)
    {
        // Frame boundary: consume Agent commands before any logic runs
        if (g_runtime_global_context.m_agent_command_system)
        {
            g_runtime_global_context.m_agent_command_system->tick();
        }

        logicalTick(delta_time);
        calculateFPS(delta_time);

        // single thread
        // exchange data between logic and render contexts
        g_runtime_global_context.m_render_system->swapLogicRenderData();

        rendererTick(delta_time);

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
        g_runtime_global_context.m_physics_manager->renderPhysicsWorld(delta_time);
#endif

        g_runtime_global_context.m_window_system->pollEvents();


        g_runtime_global_context.m_window_system->setTitle(
            std::string("Piccolo - " + std::to_string(getFPS()) + " FPS").c_str());

        const bool should_window_close = g_runtime_global_context.m_window_system->shouldClose();
        return !should_window_close;
    }

    // ── Agent bridge helpers (phase 0) ──────────────────────────────────
    namespace
    {
        std::filesystem::path agentDir()
        {
            return g_runtime_global_context.m_config_manager->getAssetFolder() / "agent";
        }

        // Task 2: reload-level sentinel
        void pollReloadSignal()
        {
            const auto signal = agentDir() / "reload_signal";
            std::error_code ec;
            if (std::filesystem::exists(signal, ec))
            {
                g_runtime_global_context.m_world_manager->reloadCurrentLevel();
                std::filesystem::remove(signal, ec);
            }
        }

        // Task 5: read input_command.json and inject into InputSystem
        void pollInputCommand()
        {
            const auto cmd_file = agentDir() / "input_command.json";
            std::error_code ec;
            if (!std::filesystem::exists(cmd_file, ec))
                return;

            std::ifstream ifs(cmd_file);
            if (!ifs.is_open())
                return;
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            ifs.close();

            std::string err;
            Json        json = Json::parse(content, err);
            if (!err.empty() || !json.is_object())
            {
                std::filesystem::remove(cmd_file, ec);
                return;
            }

            unsigned int mask = static_cast<unsigned int>(json["command_mask"].int_value());
            int           frames = json["duration_frames"].is_number()
                                       ? json["duration_frames"].int_value()
                                       : -1;
            g_runtime_global_context.m_input_system->setGameCommand(mask, frames);
            std::filesystem::remove(cmd_file, ec);
        }

        // Task 4: write state.json with object positions (every 10 frames)
        void writeAgentState(int frame_count)
        {
            if (frame_count % 10 != 0)
                return;

            auto level_wp = g_runtime_global_context.m_world_manager->getCurrentActiveLevel();
            auto level    = level_wp.lock();
            if (!level)
                return;

            Json::array objects;
            for (const auto& [id, obj] : level->getAllGObjects())
            {
                auto* tc = obj->tryGetComponentConst(TransformComponent);
                if (!tc)
                    continue;
                Vector3 pos = tc->getPosition();
                objects.push_back(Json::object{
                    {"name",     obj->getName()                                      },
                    {"position", Json::object{{"x", pos.x}, {"y", pos.y}, {"z", pos.z}}}
                });
            }

            Json state = Json::object{
                {"tick",    frame_count},
                {"objects", objects    }
            };

            const auto state_file = agentDir() / "state.json";
            std::ofstream ofs(state_file);
            if (ofs.is_open())
            {
                ofs << state.dump();
            }
        }
    } // anonymous namespace

    void PiccoloEngine::logicalTick(float delta_time)
    {
        g_runtime_global_context.m_world_manager->tick(delta_time);
        g_runtime_global_context.m_input_system->tick();

        // Agent bridge (phase 0)
        pollReloadSignal();
        pollInputCommand();
        writeAgentState(m_frame_count);
    }

    bool PiccoloEngine::rendererTick(float delta_time)
    {
        g_runtime_global_context.m_render_system->tick(delta_time);
        return true;
    }

    const float PiccoloEngine::s_fps_alpha = 1.f / 100;
    void        PiccoloEngine::calculateFPS(float delta_time)
    {
        m_frame_count++;

        if (m_frame_count == 1)
        {
            m_average_duration = delta_time;
        }
        else
        {
            m_average_duration = m_average_duration * (1 - s_fps_alpha) + delta_time * s_fps_alpha;
        }

        m_fps = static_cast<int>(1.f / m_average_duration);
    }
} // namespace Piccolo
