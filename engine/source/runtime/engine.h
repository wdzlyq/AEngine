#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace Piccolo
{
    extern bool                            g_is_editor_mode;
    extern std::unordered_set<std::string> g_editor_tick_component_types;

    // When true the engine runs without a display window / Vulkan renderer.
    // Set before startEngine(). Drives runHeadless() and null-guards render code.
    extern bool g_is_headless;

    class PiccoloEngine
    {
        friend class PiccoloEditor;

        static const float s_fps_alpha;

    public:
        void startEngine(const std::string& config_file_path);
        void shutdownEngine();

        void initialize();
        void clear();

        bool isQuit() const { return m_is_quit; }
        void run();
        bool tickOneFrame(float delta_time);

        // Run without a display window for up to max_ticks logic ticks.
        // Uses a fixed delta-time of 1/60 s. Pass -1 for unlimited.
        void runHeadless(int max_ticks = -1);

        int getFPS() const { return m_fps; }

    protected:
        void logicalTick(float delta_time);
        bool rendererTick(float delta_time);

        void calculateFPS(float delta_time);

        /**
         *  Each frame can only be called once
         */
        float calculateDeltaTime();

    protected:
        bool m_is_quit {false};

        std::chrono::steady_clock::time_point m_last_tick_time_point {std::chrono::steady_clock::now()};

        float m_average_duration {0.f};
        int   m_frame_count {0};
        int   m_fps {0};
    };

} // namespace Piccolo
