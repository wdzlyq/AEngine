#pragma once

namespace Piccolo
{
    // All states of the character animation state machine.
    enum class AnimationState : unsigned char
    {
        _idle = 0,
        _walk_start,
        _walk_run,
        _walk_stop,
        _jump_start_from_idle,
        _jump_loop_from_idle,
        _jump_end_from_idle,
        _jump_start_from_walk_run,
        _jump_loop_from_walk_run,
        _jump_end_from_walk_run
    };

    class AnimationFSM
    {
    public:
        AnimationState m_state {AnimationState::_idle};

        // Evaluate one FSM tick and transition to the appropriate next state.
        //
        // @clip_finish  true when the currently-playing clip just completed one cycle
        // @jumping      true while the character is airborne (rising or falling)
        // @speed        horizontal movement speed; treated as "is_moving" when > 0.01f
        //
        // Returns true if the state changed.
        bool update(bool clip_finish, bool jumping, float speed);
    };
} // namespace Piccolo
