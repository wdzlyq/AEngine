#include "runtime/function/animation/animation_FSM.h"

namespace Piccolo
{
    bool AnimationFSM::update(bool clip_finish, bool jumping, float speed)
    {
        const AnimationState last_state = m_state;
        const bool           is_moving  = (speed > 0.01f);

        switch (m_state)
        {
            /*** [0] idle ***/
            case AnimationState::_idle:
                if (jumping)
                    m_state = AnimationState::_jump_start_from_idle;
                else if (is_moving)
                    m_state = AnimationState::_walk_start;
                break;

            /*** [1] walk_start ***/
            case AnimationState::_walk_start:
                if (jumping)
                    m_state = AnimationState::_jump_start_from_walk_run;
                else if (clip_finish)
                    m_state = AnimationState::_walk_run;
                break;

            /*** [2] walk_run ***/
            case AnimationState::_walk_run:
                if (jumping)
                    m_state = AnimationState::_jump_start_from_walk_run;
                else if (!is_moving)
                    m_state = AnimationState::_walk_stop;
                break;

            /*** [3] walk_stop ***/
            case AnimationState::_walk_stop:
                if (jumping)
                    m_state = AnimationState::_jump_start_from_idle;
                else if (is_moving)
                    m_state = AnimationState::_walk_start;
                else if (clip_finish)
                    m_state = AnimationState::_idle;
                break;

            /*** [4] jump_start_from_idle ***/
            case AnimationState::_jump_start_from_idle:
                if (clip_finish)
                    m_state = AnimationState::_jump_loop_from_idle;
                break;

            /*** [5] jump_loop_from_idle ***/
            case AnimationState::_jump_loop_from_idle:
                if (!jumping)
                    m_state = AnimationState::_jump_end_from_idle;
                break;

            /*** [6] jump_end_from_idle ***/
            case AnimationState::_jump_end_from_idle:
                if (clip_finish)
                    m_state = is_moving ? AnimationState::_walk_start : AnimationState::_idle;
                break;

            /*** [7] jump_start_from_walk_run ***/
            case AnimationState::_jump_start_from_walk_run:
                if (clip_finish)
                    m_state = AnimationState::_jump_loop_from_walk_run;
                break;

            /*** [8] jump_loop_from_walk_run ***/
            case AnimationState::_jump_loop_from_walk_run:
                if (!jumping)
                    m_state = AnimationState::_jump_end_from_walk_run;
                break;

            /*** [9] jump_end_from_walk_run ***/
            case AnimationState::_jump_end_from_walk_run:
                if (clip_finish)
                    m_state = is_moving ? AnimationState::_walk_run : AnimationState::_idle;
                break;

            default:
                break;
        }

        return last_state != m_state;
    }
} // namespace Piccolo
