/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "PathDistanceField.h"

#include "../GameState.h"
#include "../entity/Peep.h"
#include "../profiling/Profiling.h"
#include "../scenario/Scenario.h"
#include "../world/Entrance.h"
#include "../world/Footpath.h"
#include "../world/Map.h"
#include "../world/MapLimits.h"
#include "../world/tile_element/EntranceElement.h"
#include "../world/tile_element/PathElement.h"
#include "../world/tile_element/TileElement.h"
#include "GuestPathfinding.h"

#include <deque>
#include <unordered_map>

namespace OpenRCT2::PathFinding
{
    constexpr uint16_t kDistanceUnreachable = 0xFFFF;
    // Safety net: refresh even without an explicit invalidation, so that any path
    // network mutation not covered by the invalidation hooks self-heals.
    constexpr uint32_t kFieldMaxAgeTicks = 4096;

    struct DistanceField
    {
        std::unordered_map<uint32_t, uint16_t> distances;
        uint32_t builtAtTick = 0;
        bool dirty = true;
    };

    static DistanceField _fields[2];

    static DistanceField& GetFieldStorage(DistanceFieldGoal goalType)
    {
        return _fields[static_cast<size_t>(goalType)];
    }

    // Packs a tile coordinate (x, y < 1024; z = baseHeight, 0..255) into a key.
    static uint32_t PackTileKey(const TileCoordsXYZ& loc)
    {
        return (static_cast<uint32_t>(loc.x) << 18) | (static_cast<uint32_t>(loc.y) << 8) | static_cast<uint32_t>(loc.z & 0xFF);
    }

    /**
     * Finds the path element reachable by walking one tile in the given direction,
     * starting at entryZ (already adjusted for the slope of the source tile).
     * Mirrors the geometry checks used by the legacy heuristic search.
     */
    static const PathElement* GetConnectedPathElement(const TileCoordsXY& tile, int32_t entryZ, Direction direction)
    {
        TileElement* tileElement = MapGetFirstElementAt(tile);
        if (tileElement == nullptr)
            return nullptr;
        do
        {
            if (tileElement->isGhost())
                continue;
            if (tileElement->getType() != TileElementType::Path)
                continue;
            const auto* pathElement = tileElement->asPath();
            if (!FootpathIsZAndDirectionValid(*pathElement, entryZ, direction))
                continue;
            return pathElement;
        } while (!(tileElement++)->isLastForTile());
        return nullptr;
    }

    static const EntranceElement* GetParkEntranceElement(const TileCoordsXY& tile, int32_t z)
    {
        TileElement* tileElement = MapGetFirstElementAt(tile);
        if (tileElement == nullptr)
            return nullptr;
        do
        {
            if (tileElement->isGhost())
                continue;
            if (tileElement->getType() != TileElementType::Entrance)
                continue;
            if (tileElement->baseHeight != z)
                continue;
            const auto* entranceElement = tileElement->asEntrance();
            if (entranceElement->GetEntranceType() != ENTRANCE_TYPE_PARK_ENTRANCE)
                continue;
            return entranceElement;
        } while (!(tileElement++)->isLastForTile());
        return nullptr;
    }

    /**
     * Relaxes the neighbour reached from sourceLoc by walking in `direction`
     * (the direction *towards* the source as seen from the guest: guests walk
     * neighbour -> source, so the neighbour must permit the reverse edge,
     * including banner restrictions).
     */
    static void RelaxNeighbour(
        DistanceField& field, std::deque<TileCoordsXYZ>& queue, const TileCoordsXYZ& sourceLoc, int32_t entryZ,
        Direction direction, uint16_t sourceDistance)
    {
        const TileCoordsXY neighbourTile = TileCoordsXY{ sourceLoc.x, sourceLoc.y }
            + TileCoordsXY{ TileDirectionDelta[direction].x, TileDirectionDelta[direction].y };
        if (neighbourTile.x < 0 || neighbourTile.y < 0 || neighbourTile.x >= kMaximumMapSizeTechnical
            || neighbourTile.y >= kMaximumMapSizeTechnical)
            return;

        const auto* neighbourPath = GetConnectedPathElement(neighbourTile, entryZ, direction);
        if (neighbourPath == nullptr)
            return;

        // The guest walks neighbour -> source, i.e. exits the neighbour through the
        // reverse edge; that edge must be permitted there (banners included).
        if (!(PathGetPermittedEdges(false, neighbourPath) & (1 << DirectionReverse(direction))))
            return;

        const TileCoordsXYZ neighbourLoc = { neighbourTile.x, neighbourTile.y, neighbourPath->baseHeight };
        const uint32_t key = PackTileKey(neighbourLoc);
        const uint16_t newDistance = sourceDistance + 1;
        auto [it, inserted] = field.distances.try_emplace(key, newDistance);
        if (inserted)
        {
            queue.push_back(neighbourLoc);
        }
    }

    static void ExpandPathNode(DistanceField& field, std::deque<TileCoordsXYZ>& queue, const TileCoordsXYZ& loc)
    {
        const uint16_t sourceDistance = field.distances[PackTileKey(loc)];

        TileElement* tileElement = MapGetFirstElementAt(TileCoordsXY{ loc.x, loc.y });
        if (tileElement == nullptr)
            return;
        do
        {
            if (tileElement->isGhost())
                continue;
            if (tileElement->getType() != TileElementType::Path)
                continue;
            if (tileElement->baseHeight != loc.z)
                continue;
            const auto* pathElement = tileElement->asPath();

            uint32_t edges = pathElement->GetEdges();
            for (Direction direction : kAllDirections)
            {
                if (!(edges & (1 << direction)))
                    continue;
                int32_t entryZ = loc.z;
                if (pathElement->IsSloped() && pathElement->GetSlopeDirection() == direction)
                {
                    entryZ += 2;
                }
                RelaxNeighbour(field, queue, loc, entryZ, direction, sourceDistance);
            }
        } while (!(tileElement++)->isLastForTile());
    }

    static void SeedGoalTile(DistanceField& field, std::deque<TileCoordsXYZ>& queue, const TileCoordsXYZ& loc)
    {
        const uint32_t key = PackTileKey(loc);
        auto [it, inserted] = field.distances.try_emplace(key, 0);
        if (inserted)
        {
            queue.push_back(loc);
        }
    }

    static void BuildField(DistanceFieldGoal goalType, DistanceField& field)
    {
        PROFILED_FUNCTION();

        field.distances.clear();
        field.dirty = false;
        field.builtAtTick = getGameState().currentTicks;

        std::deque<TileCoordsXYZ> queue;
        auto& gameState = getGameState();

        if (goalType == DistanceFieldGoal::parkEntrances)
        {
            for (const auto& parkEntrance : gameState.park.entrances)
            {
                const TileCoordsXYZ entranceLoc{ CoordsXYZ{ parkEntrance.x, parkEntrance.y, parkEntrance.z } };
                /* The entrance tile is not a path element, so rather than entering the
                 * BFS queue it is expanded here directly: adjacent path tiles connecting
                 * to it get distance 1. */
                if (field.distances.try_emplace(PackTileKey(entranceLoc), 0).second)
                {
                    for (Direction direction : kAllDirections)
                    {
                        RelaxNeighbour(field, queue, entranceLoc, entranceLoc.z, direction, 0);
                    }
                }
            }
        }
        else // DistanceFieldGoal::peepSpawns
        {
            for (const auto& spawn : gameState.peepSpawns)
            {
                const TileCoordsXYZ spawnLoc{ CoordsXYZ{ spawn.x, spawn.y, spawn.z } };
                // Only seed spawns that sit on an actual path tile; surface spawns
                // are handled by the legacy surface pathfinding.
                const auto* pathElement = MapGetPathElementAt(spawnLoc);
                if (pathElement == nullptr)
                    continue;
                SeedGoalTile(field, queue, { spawnLoc.x, spawnLoc.y, pathElement->baseHeight });
            }
        }

        while (!queue.empty())
        {
            const TileCoordsXYZ loc = queue.front();
            queue.pop_front();
            ExpandPathNode(field, queue, loc);
        }
    }

    static const DistanceField& EnsureField(DistanceFieldGoal goalType)
    {
        auto& field = GetFieldStorage(goalType);
        const uint32_t currentTicks = getGameState().currentTicks;
        if (field.dirty || (currentTicks - field.builtAtTick) > kFieldMaxAgeTicks)
        {
            BuildField(goalType, field);
        }
        return field;
    }

    void InvalidateDistanceFields()
    {
        for (auto& field : _fields)
        {
            field.dirty = true;
        }
    }

    static uint16_t LookupNeighbourDistance(
        const DistanceField& field, DistanceFieldGoal goalType, const TileCoordsXYZ& loc, int32_t entryZ, Direction direction)
    {
        const TileCoordsXY neighbourTile = TileCoordsXY{ loc.x, loc.y }
            + TileCoordsXY{ TileDirectionDelta[direction].x, TileDirectionDelta[direction].y };
        if (neighbourTile.x < 0 || neighbourTile.y < 0 || neighbourTile.x >= kMaximumMapSizeTechnical
            || neighbourTile.y >= kMaximumMapSizeTechnical)
            return kDistanceUnreachable;

        const auto* neighbourPath = GetConnectedPathElement(neighbourTile, entryZ, direction);
        if (neighbourPath != nullptr)
        {
            const auto it = field.distances.find(PackTileKey({ neighbourTile.x, neighbourTile.y, neighbourPath->baseHeight }));
            return it != field.distances.end() ? it->second : kDistanceUnreachable;
        }

        if (goalType == DistanceFieldGoal::parkEntrances)
        {
            // Stepping directly onto a park entrance tile (the goal itself).
            const auto* entranceElement = GetParkEntranceElement(neighbourTile, entryZ);
            if (entranceElement != nullptr)
            {
                const auto it = field.distances.find(PackTileKey({ neighbourTile.x, neighbourTile.y, entryZ }));
                return it != field.distances.end() ? it->second : kDistanceUnreachable;
            }
        }

        return kDistanceUnreachable;
    }

    Direction ChooseDirectionViaField(DistanceFieldGoal goalType, const TileCoordsXYZ& loc, Peep&)
    {
        PROFILED_FUNCTION();

        const auto& field = EnsureField(goalType);
        if (field.distances.empty())
            return kInvalidDirection;

        uint16_t bestDistance = kDistanceUnreachable;
        Direction candidates[kNumOrthogonalDirections];
        uint8_t numCandidates = 0;
        uint8_t consideredEdges = 0;

        TileElement* tileElement = MapGetFirstElementAt(TileCoordsXY{ loc.x, loc.y });
        if (tileElement == nullptr)
            return kInvalidDirection;
        do
        {
            if (tileElement->isGhost())
                continue;
            if (tileElement->getType() != TileElementType::Path)
                continue;
            if (tileElement->baseHeight != loc.z)
                continue;
            const auto* pathElement = tileElement->asPath();

            // Guests only; banners always apply.
            uint32_t edges = PathGetPermittedEdges(false, pathElement) & ~consideredEdges;
            for (Direction direction : kAllDirections)
            {
                if (!(edges & (1 << direction)))
                    continue;
                consideredEdges |= (1 << direction);

                int32_t entryZ = loc.z;
                if (pathElement->IsSloped() && pathElement->GetSlopeDirection() == direction)
                {
                    entryZ += 2;
                }

                const uint16_t distance = LookupNeighbourDistance(field, goalType, loc, entryZ, direction);
                if (distance == kDistanceUnreachable)
                    continue;
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    candidates[0] = direction;
                    numCandidates = 1;
                }
                else if (distance == bestDistance && numCandidates < kNumOrthogonalDirections)
                {
                    candidates[numCandidates++] = direction;
                }
            }
        } while (!(tileElement++)->isLastForTile());

        if (numCandidates == 0)
            return kInvalidDirection;
        if (numCandidates == 1)
            return candidates[0];
        return candidates[ScenarioRand() % numCandidates];
    }
} // namespace OpenRCT2::PathFinding
