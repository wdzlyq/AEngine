#pragma once

#include "runtime/function/animation/animation_FSM.h"
#include "runtime/function/animation/skeleton.h"
#include "runtime/function/framework/component/component.h"
#include "runtime/resource/res_type/components/animation.h"

namespace Piccolo
{
    REFLECTION_TYPE(AnimationComponent)
    CLASS(AnimationComponent : public Component, WhiteListFields)
    {
        REFLECTION_BODY(AnimationComponent)

    public:
        AnimationComponent() = default;

        void postLoadResource(std::weak_ptr<GObject> parent_object) override;

        void tick(float delta_time) override;

        const AnimationResult& getResult() const;

        const Skeleton& getSkeleton() const;

        AnimationFSM& getFSM() { return m_fsm; }
        const AnimationFSM& getFSM() const { return m_fsm; }

    protected:
        META(Enable)
        AnimationComponentRes m_animation_res;

        Skeleton m_skeleton;
        AnimationFSM m_fsm;
    };
} // namespace Piccolo
