#include "runtime/function/agent/scene_snapshot.h"

#include "runtime/core/meta/json.h"
#include "runtime/core/meta/reflection/reflection.h"
#include "runtime/core/meta/serializer/serializer.h"
#include "runtime/function/framework/component/transform/transform_component.h"
#include "runtime/function/framework/level/level.h"
#include "runtime/function/framework/object/object.h"
#include "runtime/resource/res_type/common/object.h"

namespace Piccolo
{
    void SceneSnapshot::capture(Level* level)
    {
        if (!level)
        {
            json_data.clear();
            return;
        }

        const LevelObjectsMap& objects = level->getAllGObjects();
        std::vector<Json>      obj_array;
        obj_array.reserve(objects.size());

        for (const auto& [id, obj_ptr] : objects)
        {
            if (!obj_ptr)
                continue;

            // Serialise each component via the reflection system to avoid
            // including all_serializer.ipp (which causes multiple-definition
            // linker errors when included in more than one TU).
            auto              components = obj_ptr->getComponents();
            std::vector<Json> comp_array;
            comp_array.reserve(components.size());
            for (auto& comp : components)
            {
                void*       inst      = comp.operator->();
                std::string type_name = comp.getTypeName();

                // TransformComponent: also capture the runtime position from
                // the double-buffer (not visible via reflection).
                if (type_name == "TransformComponent")
                {
                    auto* tc  = static_cast<TransformComponent*>(inst);
                    Vector3 pos = tc->getPosition();
                    Json comp_data = Json(Json::object{
                        {"rt_position", Json(Json::object{
                            {"x", pos.x}, {"y", pos.y}, {"z", pos.z}})}
                    });
                    comp_array.push_back(
                        Json(Json::object{{"type", type_name}, {"data", comp_data}}));
                    continue;
                }

                Json comp_data = Reflection::TypeMeta::writeByName(type_name, inst);
                comp_array.push_back(
                    Json(Json::object{{"type", type_name}, {"data", comp_data}}));
            }

            obj_array.push_back(Json(Json::object{
                {"name",       obj_ptr->getName()},
                {"components", Json(comp_array)}
            }));
        }

        json_data = Json(obj_array).dump();
    }

    void SceneSnapshot::restore(Level* level) const
    {
        if (!level || json_data.empty())
            return;

        std::string err;
        Json        root = Json::parse(json_data, err);
        if (!err.empty() || !root.is_array())
            return;

        const LevelObjectsMap& objects = level->getAllGObjects();

        for (const Json& obj_json : root.array_items())
        {
            std::string target_name = obj_json["name"].string_value();

            for (const auto& [id, obj_ptr] : objects)
            {
                if (!obj_ptr || obj_ptr->getName() != target_name)
                    continue;

                auto components = obj_ptr->getComponents();
                for (auto& comp : components)
                {
                    void*       inst      = comp.operator->();
                    std::string type_name = comp.getTypeName();

                    for (const Json& comp_json : obj_json["components"].array_items())
                    {
                        if (comp_json["type"].string_value() != type_name)
                            continue;

                        const Json& data = comp_json["data"];

                        // TransformComponent: restore runtime position directly
                        if (type_name == "TransformComponent" && !data["rt_position"].is_null())
                        {
                            auto* tc = static_cast<TransformComponent*>(inst);
                            Vector3 pos(
                                (float)data["rt_position"]["x"].number_value(),
                                (float)data["rt_position"]["y"].number_value(),
                                (float)data["rt_position"]["z"].number_value());
                            tc->setPosition(pos);
                            break;
                        }

                        // Restore scalar fields via reflection
                        Reflection::TypeMeta        meta   = Reflection::TypeMeta::newMetaFromName(type_name);
                        Reflection::FieldAccessor*  fields = nullptr;
                        int                         count  = meta.getFieldsList(fields);

                        for (int fi = 0; fi < count; ++fi)
                        {
                            const char* fname = fields[fi].getFieldName();
                            const char* ftype = fields[fi].getFieldTypeName();
                            const Json& fval  = data[fname];
                            if (fval.is_null())
                                continue;

                            if (std::string(ftype) == "float")
                            {
                                float v = (float)fval.number_value();
                                fields[fi].set(inst, &v);
                            }
                            else if (std::string(ftype) == "int")
                            {
                                int v = fval.int_value();
                                fields[fi].set(inst, &v);
                            }
                            else if (std::string(ftype) == "bool")
                            {
                                bool v = fval.bool_value();
                                fields[fi].set(inst, &v);
                            }
                            // Vector3, Transform, Quaternion: skipped for MVP
                        }
                        delete[] fields;
                        break;
                    }
                }
                break;
            }
        }
    }
} // namespace Piccolo
