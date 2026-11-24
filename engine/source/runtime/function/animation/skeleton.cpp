#include "runtime/function/animation/skeleton.h"

#include "runtime/core/math/math.h"

#include "runtime/function/animation/pose.h"
#include "runtime/function/animation/utilities.h"

namespace Piccolo
{
    Skeleton::~Skeleton() { delete[] m_bones; }

    void Skeleton::resetSkeleton()
    {
        for (size_t i = 0; i < m_bone_count; i++)
            m_bones[i].resetToInitialPose();
    }

    void Skeleton::buildSkeleton(const SkeletonData& skeleton_definition)
    {
        m_is_flat = skeleton_definition.is_flat;
        if (m_bones != nullptr)
        {
            delete[] m_bones;
        }
        if (!m_is_flat || !skeleton_definition.in_topological_order)
        {
            // LOG_ERROR
            return;
        }
        m_bone_count = skeleton_definition.bones_map.size();
        m_bones      = new Bone[m_bone_count];
        for (size_t i = 0; i < m_bone_count; i++)
        {
            const RawBone bone_definition = skeleton_definition.bones_map[i];
            Bone*         parent_bone     = find_by_index(m_bones, bone_definition.parent_index, i, m_is_flat);
            m_bones[i].initialize(std::make_shared<RawBone>(bone_definition), parent_bone);
        }
    }

    void Skeleton::applyAnimation(const BlendStateWithClipData& blend_state)
    {
        if (!m_bones)
        {
            return;
        }
        resetSkeleton();

        // Build one AnimationPose per clip, then blend them all together
        // using the per-bone blend weights supplied in blend_state.
        AnimationPose result_pose;

        for (size_t clip_index = 0; clip_index < static_cast<size_t>(blend_state.clip_count); clip_index++)
        {
            if (clip_index >= blend_state.blend_clip.size() ||
                clip_index >= blend_state.blend_anim_skel_map.size())
                break;

            const AnimationClip& animation_clip = blend_state.blend_clip[clip_index];
            const float          phase          = blend_state.blend_ratio[clip_index];
            const AnimSkelMap&   anim_skel_map  = blend_state.blend_anim_skel_map[clip_index];

            // Sample all channels for this clip at the given phase.
            float exact_frame        = phase * (animation_clip.total_frame - 1);
            int   current_frame_low  = static_cast<int>(floor(exact_frame));
            int   current_frame_high = static_cast<int>(ceil(exact_frame));
            float lerp_ratio         = exact_frame - static_cast<float>(current_frame_low);

            // Build the pose for this clip.
            AnimationPose clip_pose;
            clip_pose.m_bone_poses.resize(m_bone_count);

            for (size_t node_index = 0;
                 node_index < animation_clip.node_count && node_index < anim_skel_map.convert.size();
                 node_index++)
            {
                const AnimationChannel& channel    = animation_clip.node_channels[node_index];
                size_t                  bone_index = anim_skel_map.convert[node_index];

                if (bone_index == std::numeric_limits<size_t>().max() || bone_index >= m_bone_count)
                    continue;

                int frame_hi = current_frame_high;
                int frame_lo = current_frame_low;
                if (channel.position_keys.size() <= static_cast<size_t>(frame_hi))
                    frame_hi = static_cast<int>(channel.position_keys.size()) - 1;
                if (channel.scaling_keys.size() <= static_cast<size_t>(frame_hi))
                    frame_hi = static_cast<int>(channel.scaling_keys.size()) - 1;
                if (channel.rotation_keys.size() <= static_cast<size_t>(frame_hi))
                    frame_hi = static_cast<int>(channel.rotation_keys.size()) - 1;
                frame_lo = (frame_lo < frame_hi) ? frame_lo : frame_hi;

                clip_pose.m_bone_poses[bone_index].position =
                    Vector3::lerp(channel.position_keys[frame_lo], channel.position_keys[frame_hi], lerp_ratio);
                clip_pose.m_bone_poses[bone_index].scale =
                    Vector3::lerp(channel.scaling_keys[frame_lo], channel.scaling_keys[frame_hi], lerp_ratio);
                clip_pose.m_bone_poses[bone_index].rotation =
                    Quaternion::nLerp(lerp_ratio,
                                      channel.rotation_keys[frame_lo],
                                      channel.rotation_keys[frame_hi],
                                      true);
            }

            // Determine the blend weight for this clip.
            // Use per-bone weights when available; otherwise fall back to the
            // global clip weight stored in blend_state.blend_weight.
            float global_clip_weight = (clip_index < blend_state.blend_weight.size() &&
                                        !blend_state.blend_weight[clip_index].blend_weight.empty())
                                           ? blend_state.blend_weight[clip_index].blend_weight[0]
                                           : 1.0f;

            result_pose.blend(clip_pose, global_clip_weight);
        }

        // Apply the blended pose to the skeleton bones.
        for (size_t i = 0; i < m_bone_count && i < result_pose.m_bone_poses.size(); i++)
        {
            Bone*                    bone = &m_bones[i];
            const BoneAnimationPose& bp   = result_pose.m_bone_poses[i];
            bone->rotate(bp.rotation);
            bone->scale(bp.scale);
            bone->translate(bp.position);
        }

        for (size_t i = 0; i < m_bone_count; i++)
        {
            m_bones[i].update();
        }
    }

    AnimationResult Skeleton::outputAnimationResult()
    {
        AnimationResult animation_result;
        for (size_t i = 0; i < m_bone_count; i++)
        {
            std::shared_ptr<AnimationResultElement> animation_result_element =
                std::make_shared<AnimationResultElement>();
            Bone* bone                      = &m_bones[i];
            animation_result_element->index = bone->getID() + 1;
            Vector3 temp_translation        = bone->_getDerivedPosition();

            // TODO: the unit of the joint matrices is wrong
            Vector3 temp_scale = bone->_getDerivedScale();

            Quaternion temp_rotation = bone->_getDerivedOrientation();

            // auto scale = bone->_getDerivedTScale();
            // scale.x = 1.f / scale.x;
            // scale.y = 1.f / scale.y;
            // scale.z = 1.f / scale.z;

            // auto revTmat = getMatrix(
            //	-bone->_getDerivedTPosition(),
            //	scale,
            //	conjugate( bone->_getDerivedTOrientation())
            //);
            auto objMat =
                Transform(bone->_getDerivedPosition(), bone->_getDerivedOrientation(), bone->_getDerivedScale())
                    .getMatrix();

            auto resMat = objMat * bone->_getInverseTpose();

            animation_result_element->transform = resMat.toMatrix4x4_();

            animation_result.node.push_back(*animation_result_element);
        }
        return animation_result;
    }

    const Bone* Skeleton::getBones() const
    {
        return m_bones;
    }

    int32_t Skeleton::getBonesCount() const
    {
        return m_bone_count;
    }
} // namespace Piccolo