#include "runtime/function/framework/component/animation/animation_component.h"

#include "runtime/function/animation/animation_FSM.h"
#include "runtime/function/animation/animation_system.h"
#include "runtime/function/framework/component/motor/motor_component.h"
#include "runtime/function/framework/object/object.h"

namespace Piccolo
{
    void AnimationComponent::postLoadResource(std::weak_ptr<GObject> parent_object)
    {
        m_parent_object = parent_object;

        auto skeleton_res = AnimationManager::tryLoadSkeleton(m_animation_res.skeleton_file_path);

        m_skeleton.buildSkeleton(*skeleton_res);
    }

    void AnimationComponent::tick(float delta_time)
    {
        // --- Advance blend ratio for the active clip ---
        m_animation_res.blend_state.blend_ratio[0] +=
            (delta_time / m_animation_res.blend_state.blend_clip_file_length[0]);

        // Detect clip finish before wrapping ratio back to [0,1).
        bool clip_finish = (m_animation_res.blend_state.blend_ratio[0] >= 1.0f);
        m_animation_res.blend_state.blend_ratio[0] -= floor(m_animation_res.blend_state.blend_ratio[0]);

        // --- Read motor state for FSM signals ---
        bool  is_jumping = false;
        float speed      = 0.0f;

        auto parent = m_parent_object.lock();
        if (parent)
        {
            const MotorComponent* motor = parent->tryGetComponentConst(MotorComponent);
            if (motor)
            {
                is_jumping = (motor->getJumpState() != JumpState::idle);
                speed      = motor->getIsMoving() ? 1.0f : 0.0f;
            }
        }

        // --- Update FSM ---
        m_fsm.update(clip_finish, is_jumping, speed);

        // --- Apply animation ---
        m_skeleton.applyAnimation(AnimationManager::getBlendStateWithClipData(m_animation_res.blend_state));
        m_animation_res.animation_result = m_skeleton.outputAnimationResult();
    }

    const AnimationResult& AnimationComponent::getResult() const { return m_animation_res.animation_result; }

    const Skeleton& AnimationComponent::getSkeleton() const { return m_skeleton; }
} // namespace Piccolo
