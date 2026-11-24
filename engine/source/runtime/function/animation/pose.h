#pragma once

#include "runtime/core/math/quaternion.h"
#include "runtime/core/math/vector3.h"

#include <vector>

namespace Piccolo
{
    // Blended transform of a single bone.
    struct BoneAnimationPose
    {
        Vector3    position  {Vector3::ZERO};
        Vector3    scale     {Vector3::UNIT_SCALE};
        Quaternion rotation  {Quaternion::IDENTITY};
        // Accumulated blend weight for this bone so far in the current blend operation.
        float m_blend_weight {0.0f};
    };

    // Pose of an entire skeleton: one BoneAnimationPose per bone.
    // Use blend() to fold in additional weighted poses one at a time.
    class AnimationPose
    {
    public:
        std::vector<BoneAnimationPose> m_bone_poses;

        // Blend 'other' into this pose with the given weight.
        //
        // The first call with an empty m_bone_poses initialises the pose directly
        // from 'other'.  Subsequent calls accumulate the weighted average.
        //
        // Internally the method tracks m_blend_weight per bone so the caller only
        // needs to pass the raw (unnormalized) weight for 'other'.
        void blend(const AnimationPose& other, float weight);
    };
} // namespace Piccolo
