#include "runtime/function/controller/character_controller.h"

#include "runtime/core/base/macro.h"
#include "runtime/core/math/transform.h"

#include "runtime/function/framework/component/motor/motor_component.h"
#include "runtime/function/framework/world/world_manager.h"
#include "runtime/function/global/global_context.h"
#include "runtime/function/physics/physics_scene.h"

namespace Piccolo
{
    CharacterController::CharacterController(const Capsule& capsule) : m_capsule(capsule)
    {
        m_rigidbody_shape                                    = RigidBodyShape();
        m_rigidbody_shape.m_geometry                         = PICCOLO_REFLECTION_NEW(Capsule);
        *static_cast<Capsule*>(m_rigidbody_shape.m_geometry) = m_capsule;

        m_rigidbody_shape.m_type = RigidBodyShapeType::capsule;

        Quaternion orientation;
        orientation.fromAngleAxis(Radian(Degree(90.f)), Vector3::UNIT_X);

        m_rigidbody_shape.m_local_transform =
            Transform(Vector3(0, 0, capsule.m_half_height + capsule.m_radius), orientation, Vector3::UNIT_SCALE);
    }

    Vector3 CharacterController::move(const Vector3& current_position, const Vector3& displacement)
    {
        std::shared_ptr<PhysicsScene> physics_scene =
            g_runtime_global_context.m_world_manager->getCurrentActivePhysicsScene().lock();
        ASSERT(physics_scene);

        // Separate displacement into horizontal (XY) and vertical (Z) components.
        // Piccolo uses Z-up: horizontal motion is in the XY plane.
        Vector3 horizontal_displacement(displacement.x, displacement.y, 0.0f);
        float   vertical_z = displacement.z;

        Vector3 position = current_position;

        // ----------------------------------------------------------------
        // SWEEP_PASS_UP / SWEEP_PASS_DOWN  (vertical)
        // ----------------------------------------------------------------
        if (std::abs(vertical_z) > 1e-6f)
        {
            Vector3 vertical_dir = (vertical_z > 0.0f) ? Vector3::UNIT_Z : Vector3::NEGATIVE_UNIT_Z;
            float   vertical_len = std::abs(vertical_z);

            Transform                   current_transform(position, Quaternion::IDENTITY, Vector3::UNIT_SCALE);
            std::vector<PhysicsHitInfo> hits;

            if (physics_scene->sweep(m_rigidbody_shape, current_transform.getMatrix(),
                                     vertical_dir, vertical_len, hits))
            {
                // Hit something above (ceiling) or below (ground).
                // Move only as far as the first contact allows.
                position += vertical_dir * hits[0].hit_distance;
            }
            else
            {
                position.z += vertical_z;
            }
        }

        // ----------------------------------------------------------------
        // SWEEP_PASS_SIDE  (horizontal)
        // ----------------------------------------------------------------
        float horizontal_len = horizontal_displacement.length();
        if (horizontal_len > 1e-6f)
        {
            Vector3 horizontal_dir = horizontal_displacement / horizontal_len;

            Transform                   current_transform(position, Quaternion::IDENTITY, Vector3::UNIT_SCALE);
            std::vector<PhysicsHitInfo> hits;

            if (physics_scene->sweep(m_rigidbody_shape, current_transform.getMatrix(),
                                     horizontal_dir, horizontal_len, hits))
            {
                // Blocked by a wall: slide up to the contact point only.
                position += horizontal_dir * hits[0].hit_distance;
            }
            else
            {
                position += horizontal_displacement;
            }
        }

        return position;
    }
} // namespace Piccolo
