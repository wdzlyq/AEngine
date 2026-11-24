#pragma once

#include <string>

namespace Piccolo
{
    class Level;

    // Captures the structural state of a Level (all GObjects and their
    // serialisable component fields) so the Agent can roll back failed
    // operations.  Runtime-only physics state (velocity, etc.) is NOT
    // captured — this is intentional for the Phase-1 MVP.
    struct SceneSnapshot
    {
        std::string json_data; // serialised Level as a JSON string

        // Save current level state into this snapshot
        void capture(Level* level);

        // Restore level state from this snapshot (best-effort: sets fields on
        // existing objects; objects not present in the snapshot are untouched)
        void restore(Level* level) const;

        bool valid() const { return !json_data.empty(); }
        void clear()        { json_data.clear(); }
    };
} // namespace Piccolo
