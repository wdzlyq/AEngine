#include "runtime/function/animation/pose.h"

#include <algorithm>

namespace Piccolo
{
    void AnimationPose::blend(const AnimationPose& other, float weight)
    {
        if (weight < 1e-6f)
            return;

        if (m_bone_poses.empty())
        {
            // First pose contribution: copy directly and record initial weight.
            m_bone_poses = other.m_bone_poses;
            for (auto& bp : m_bone_poses)
                bp.m_blend_weight = weight;
            return;
        }

        const size_t bone_count = std::min(m_bone_poses.size(), other.m_bone_poses.size());

        for (size_t i = 0; i < bone_count; ++i)
        {
            BoneAnimationPose&       dst = m_bone_poses[i];
            const BoneAnimationPose& src = other.m_bone_poses[i];

            // Running total of weights seen so far for this bone.
            float sum_weight = dst.m_blend_weight;
            float new_total  = sum_weight + weight;

            // Fraction of the new total that the incoming pose should contribute.
            float cur_weight = (new_total > 1e-6f) ? weight / new_total : 1.0f;

            dst.position        = Vector3::lerp(dst.position, src.position, cur_weight);
            dst.scale           = Vector3::lerp(dst.scale,    src.scale,    cur_weight);
            dst.rotation        = Quaternion::nLerp(cur_weight, dst.rotation, src.rotation, false);
            dst.m_blend_weight  = new_total;
        }
    }
} // namespace Piccolo
