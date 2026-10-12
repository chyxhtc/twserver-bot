#pragma once

// A132 (2026-10-12): where the world currently HAS a live gathering node.
//
// Why: the goal windows spent three and a half hours walking their pools and never met a live herb
// (16 work-phase probes, every one seen=0), while a 96 second LootGoSeen sample showed the world did
// have live herbs and veins at that very moment (Peacebloom in zone 44, a copper vein in zone 38, an
// iron deposit in zone 46 ...). The knowledge came from ordinary bots wandering past them and was
// written to the log and thrown away - nothing fed it back to the window.
//
// So: whenever a bot has a game object in its loot list - which is what the LootGoSeen probe already
// watches - record (entry, position, time) here. A window then prefers the pool whose spawn points
// sit near a fresh sighting of the same entry, instead of cycling a fixed list of pools and hoping
// the node happens to be up when the bot walks past it.
//
// Deliberately plain: single world thread, a handful of entries, newest N kept, everything older
// than the TTL dropped. No locking, no persistence (a restart starts the memory empty, which is
// correct - node states do not survive a restart either).

#include <ctime>
#include <map>
#include <vector>

namespace ai { namespace gather_sightings
{
    struct Sighting
    {
        uint32 mapId;
        float x;
        float y;
        float z;
        time_t seenAt;
    };

    inline int32 TtlSeconds()
    {
        return 900; // one respawn cycle: older than this says nothing about the node being up now
    }

    inline size_t MaxPerEntry()
    {
        return 12;
    }

    inline std::map<uint32, std::vector<Sighting>>& Table()
    {
        static std::map<uint32, std::vector<Sighting>> table;
        return table;
    }

    inline void Record(uint32 goEntry, uint32 mapId, float x, float y, float z)
    {
        // Per-bot callers already throttle themselves to one row per 20-30 seconds; this guard keeps
        // the table small even if a future caller does not.
        static std::map<uint32, time_t> lastRecorded;
        time_t const now = time(nullptr);
        time_t& last = lastRecorded[goEntry];
        if (last == now)
            return;
        last = now;

        std::vector<Sighting>& list = Table()[goEntry];
        for (Sighting& s : list)
            if (s.mapId == mapId && std::abs(s.x - x) < 5.0f && std::abs(s.y - y) < 5.0f)
            {
                s.z = z;
                s.seenAt = now;
                return;
            }

        list.push_back(Sighting{ mapId, x, y, z, now });
        while (list.size() > MaxPerEntry())
            list.erase(list.begin());
    }

    // Is there a fresh sighting of this entry within reach of the given position?
    inline bool FreshNear(uint32 goEntry, uint32 mapId, float x, float y, float radius)
    {
        auto const it = Table().find(goEntry);
        if (it == Table().end())
            return false;

        time_t const now = time(nullptr);
        float const r2 = radius * radius;
        for (Sighting const& s : it->second)
        {
            if (s.mapId != mapId || now - s.seenAt > TtlSeconds())
                continue;
            float const dx = s.x - x;
            float const dy = s.y - y;
            if (dx * dx + dy * dy <= r2)
                return true;
        }
        return false;
    }
}}
