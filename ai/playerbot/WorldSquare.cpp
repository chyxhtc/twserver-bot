#include "WorldSquare.h"
#include "TravelMgr.h"


using namespace ai;

// A58 probe: definitions. Plain integers are enough - the world thread is the only writer while a
// travel search runs, and the counters are read right after the call that owns them.
uint64 SquareProbe::subSquareSteps = 0;
uint64 SquareProbe::pointSteps = 0;
uint64 SquareProbe::portalLists = 0;
uint64 SquareProbe::portals = 0;

void SquareProbe::Reset()
{
    subSquareSteps = 0;
    pointSteps = 0;
    portalLists = 0;
    portals = 0;
}

void WorldPointContainer::printWKT(std::ostringstream& out, bool squares) const
{
    if (!squares)
    {
        for (auto& p : points)
            out << p->GetDisplayX() << " " << p->GetDisplayY() << ",";
    }
    {
        for (auto& p : GetSquare())
            out << p.GetDisplayX() << " " << p.GetDisplayY() << ",";
    }
}

float MapWpSquare::sqOutDistance(const WorldPosition& point) const
{
    if (point.mapId != mapId)
    {
        float minSqDist = FLT_MAX;
        ++SquareProbe::portalLists;
        //distance = point -> portal (on map of square)
        for (auto& [portal, distance] : sTravelMgr.sqMapTransDistances(point, mapId))
        {
            ++SquareProbe::portals;
            float sqDist = distance + sqDistance(portal); //Add sqDistance = portal -> square.
            if (sqDist < minSqDist)
                minSqDist = sqDist;
        }

        return minSqDist;
    }

    return WorldPointSquareContainer::sqOutDistance(point);
}

uint32 WorldWpSquare::ClosestMapId(const WorldPosition& point) const
{
    uint32 closetMapId = 999;
    float minsqDistance = FLT_MAX;
    for (auto& mapId : GetSubSquareIds())
    {
        float sqDistance = GetSubSquare(mapId).sqDistance(point);

        if (sqDistance < minsqDistance)
        {
            closetMapId = mapId;
            minsqDistance = sqDistance;
        }
    }

    return closetMapId;
}
