/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../world/Location.hpp"

namespace OpenRCT2
{
    struct Peep;
}

namespace OpenRCT2::PathFinding
{
    /**
     * Goal-rooted distance fields ("network compass").
     *
     * For goals shared by many guests (park entrances, peep spawns), a multi-source
     * BFS floods true walking distance outward from the goal tiles over the actual
     * path network. A guest then navigates by stepping onto the neighbouring path
     * tile with the smallest stored distance: an O(edges) decision per tile that,
     * unlike a heuristic search, cannot be defeated by loops, wide path regions or
     * concave layouts. See docs/guest-pathfinding-analysis.md (sections 3 and 4.1).
     *
     * Fields are rebuilt lazily when marked dirty (on path network mutations) and
     * additionally refreshed after a fixed tick age as a safety net for unhooked
     * mutations. All state is derived purely from the map, keeping multiplayer
     * deterministic; nothing is serialised.
     */
    enum class DistanceFieldGoal : uint8_t
    {
        parkEntrances,
        peepSpawns,
    };

    // Marks all distance fields for lazy rebuild. Call whenever path connectivity,
    // banners, park entrances or peep spawns change.
    void InvalidateDistanceFields();

    /**
     * Chooses the permitted edge of the path tile at loc that leads to the
     * neighbouring tile closest (by network distance) to the given goal set.
     * Ties are broken with ScenarioRand for crowd dispersion.
     * Returns kInvalidDirection when the guest's tile is not connected to any
     * goal (caller should fall back to legacy pathfinding).
     */
    Direction ChooseDirectionViaField(DistanceFieldGoal goalType, const TileCoordsXYZ& loc, Peep& peep);
} // namespace OpenRCT2::PathFinding
