#pragma once

#include "runtime/core/math/math.h"

namespace Piccolo
{
    enum class GameCommand : unsigned int
    {
        forward  = 1 << 0,                 // W
        backward = 1 << 1,                 // S
        left     = 1 << 2,                 // A
        right    = 1 << 3,                 // D
        jump     = 1 << 4,                 // SPACE
        squat    = 1 << 5,                 // not implemented yet
        sprint   = 1 << 6,                 // LEFT SHIFT
        fire     = 1 << 7,                 // not implemented yet
        free_carema = 1 << 8,              // F
        invalid  = (unsigned int)(1 << 31) // lost focus
    };

    extern unsigned int k_complement_control_command;

    class InputSystem
    {

    public:
        void onKey(int key, int scancode, int action, int mods);
        void onCursorPos(double current_cursor_x, double current_cursor_y);

        void initialize();
        void tick();
        void clear();

        int m_cursor_delta_x {0};
        int m_cursor_delta_y {0};

        Radian m_cursor_delta_yaw {0};
        Radian m_cursor_delta_pitch {0};

        void         resetGameCommand() { m_game_command = 0; }
        unsigned int getGameCommand() const { return m_game_command; }

        // Agent API: inject a command mask directly (bypasses keyboard input).
        // duration_frames == -1  -> hold until overwritten
        // duration_frames >  0   -> auto-clear after N ticks
        void setGameCommand(unsigned int command_mask, int duration_frames = -1);

    private:
        void onKeyInGameMode(int key, int scancode, int action, int mods);

        void calculateCursorDeltaAngles();

        unsigned int m_game_command {0};

        // countdown for timed agent commands; -1 means indefinite
        int m_agent_command_frames_remaining {0};

        // true while an agent-injected command is active; suppresses the
        // focus-mode (invalid bit) override so Agent input always works
        bool m_agent_command_active {false};

        int m_last_cursor_x {0};
        int m_last_cursor_y {0};
    };
} // namespace Piccolo
