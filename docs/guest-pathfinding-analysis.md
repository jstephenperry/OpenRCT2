# Guest Pathfinding Analysis

This document analyses how guest pathfinding works in OpenRCT2 today, explains
the structural reasons guests get lost, and recommends a path toward
eliminating the major classes of pathfinding bugs — in particular:

1. Guests getting lost even when a route to their goal exists.
2. Guests failing to treat double/triple-wide paths as a single walkable path.
3. Guests getting stuck cycling around path loops.

All file references are relative to the repository root.

---

## 1. How guest pathfinding works today

### 1.1 No routes, only per-tile direction decisions

Guests never compute or store a route. Every time a guest finishes crossing a
tile, `Peep::PerformNextAction` (`src/openrct2/entity/Peep.cpp`) calls
`PathFinding::CalculateNextDestination`
(`src/openrct2/peep/GuestPathfinding.cpp`), which picks a single direction
(0–3) to take off the current tile. The entire search is then thrown away and
repeated from scratch on the next tile.

`CalculateNextDestination` dispatches as follows:

| Guest state | Strategy |
|---|---|
| On surface (no path) | `GuestSurfacePathFinding` — random direction with wall checks |
| Only one usable edge | Take it |
| Outside park, entering | `GuestPathFindParkEntranceEntering` → goal = nearest park entrance |
| Outside park, leaving | `GuestPathFindPeepSpawn` → goal = nearest spawn point |
| Leaving park | `GuestPathFindParkEntranceLeaving` → goal = chosen park entrance |
| Heading to a ride | Goal = queue end / ride entrance, via `GetRideQueueEnd` |
| Otherwise | `GuestPathfindAimless` — random walk, 50% prefer straight |

All goal-directed cases funnel into `ChooseDirection`.

### 1.2 The heuristic search (`ChooseDirection` / `PeepPathfindHeuristicSearch`)

For each permitted edge of the current tile, the code runs a **bounded,
recursive depth-first search** (`PeepPathfindHeuristicSearch`) and keeps the
edge whose search frontier got the best score. Key properties:

- **Score** (`CalculateHeuristicPathingScore`) is a weighted *crow-flies*
  distance to the goal (larger axis dominates; the smaller axis is divided
  by 16; z weighted ×2). It is purely geometric — it knows nothing about the
  path network between the frontier and the goal.
- **Search limits** (the knobs that make guests "dumb"):
  - `numSteps >= 200` per search path;
  - ~15,000 cumulative tiles per decision (50,000 for staff), divided by the
    number of edges under test;
  - a *thin-junction* budget per search path: **5** for ordinary guests,
    **7** with a park map, **7** when leaving the park, **8** when leaving and
    lost (`kMaxJunctionsGuest*`, `PeepPathfindGetMaxNumberJunctions`).
- **DFS, not A\*/Dijkstra**: there is no closed set. The same tile can be
  re-visited via every alternate route, so effort grows exponentially with
  junction density, which is *why* the junction budget must be so small. Loop
  detection exists only at thin junctions, via two mechanisms:
  - `state.history[]` — junctions visited in the *current* search path;
  - `peep.PathfindHistory[4]` — see below.
- The score is only updated at search-path *terminals* (goal reached, wide
  tile, junction budget exhausted, step/tile limit reached) — dead ends are
  correctly ignored.

### 1.3 Per-guest memory: `PathfindHistory`

Each peep remembers the **last 4 thin junctions** it walked through on the way
to its current goal (`std::array<TileCoordsXYZD, 4> PathfindHistory` in
`src/openrct2/entity/Peep.h`, serialised in `park/ParkFile.cpp`). For each
remembered junction it keeps a bitmask of *not-yet-tried* edges. Returning to
a remembered junction restricts the choice to untried edges; when all edges
have been tried the mask resets. The history is cleared whenever the goal
changes.

This is the only thing standing between a guest and an infinite loop — and it
is 4 entries deep, with eviction in round-robin order
(`peep.PathfindGoal.direction` doubles as the ring index).

### 1.4 Wide paths are walls

The "wide path" system is the single biggest source of the behaviour you
describe (guests unable to treat 2/3-wide paths as one path):

- `FootpathUpdatePathWideFlags` (`src/openrct2/world/Footpath.cpp`) marks path
  tiles as *wide* when they sit inside a slab of interconnected paths. The
  computation is an incremental, order-dependent heuristic: each pass clears
  the flag and only ~1/8 of tiles (`x & 0xE0`, `y & 0xE0` filter) are
  candidates for re-flagging per update, so the flags are eventually
  consistent at best. Queue and sloped tiles are never wide.
- In `PeepPathfindHeuristicSearch`, hitting a wide tile **terminates the
  search path** (`PathSearchResult::Wide`). Its score may only be recorded if
  the *current* tile is also wide. The search can therefore never "see"
  through a plaza or a wide boulevard; a goal on the far side of a wide region
  is unreachable to the search even though guests can physically walk there.
- Worse, in `CalculateNextDestination`, a guest heading for a ride or the park
  exit actively **prunes every edge that leads onto a wide path** as long as
  any thin edge exists — even when the wide path is the only sensible route.
- `PathIsThinJunction` ignores wide neighbours when counting edges, so a thin
  path running alongside a wide one isn't treated as a junction; combined
  with the above, wide regions behave like fences with occasional gates.

The net effect: build a 2-tile-wide main street and (depending on tile-update
order) one lane gets flagged wide. Guests then refuse to step onto half the
street while goal-seeking, search paths die at the boundary, the heuristic
returns failure, and the guest falls back to `GuestPathfindAimless` — a pure
random walk. This is exactly the observed "guests mill around on wide paths /
get lost on them" behaviour.

### 1.5 The fallback chain ends in a random walk

Whenever `ChooseDirection` fails (`kInvalidDirection`) — search limits too
tight, goal beyond a wide region, all remembered edges tried — the guest
falls back to `GuestPathfindAimless`, which is a uniform random walk with a
50% straight-ahead preference. There is no memory, no bias toward unexplored
territory, no compass. The "lost" feedback to the player is also weak:
`Guest::checkIfLost` only fires at *dead ends*, so a guest endlessly orbiting
a loop never reports being lost.

---

## 2. Root causes of guests getting lost

**RC1 — Greedy local search with a geometric heuristic.** Once the goal is
further away than the junction budget (5 junctions!), the chosen edge is
simply "the direction whose frontier ended geometrically closest to the
goal". Any concave layout — a lake, a U-shaped detour, a station building, a
themed area with one exit — defeats it: the guest commits to edges that
reduce crow-flies distance but don't lead anywhere, discovers nothing (the
search is discarded each tile), and oscillates.

**RC2 — 4-junction memory cannot cover real loops.** A loop containing more
than 4 thin junctions evicts its own history: by the time the guest returns
to junction A, A has been forgotten, the "untried edges" mechanism resets,
and the same "best" edge is chosen again — a stable cycle. Tie-breaking is
deterministic (`bitScanForward`, lowest direction wins), so two equal-score
edges never alternate; whole crowds make identical wrong decisions.

**RC3 — Wide paths are unsearchable obstacles** (see §1.4). This both
directly blocks navigation and *consumes the fallback*: failed searches put
guests into random-walk mode, which reads as "lost" to the player.

**RC4 — Exponential DFS forces tiny limits.** Without a closed set, the
15,000-tile budget is mostly spent re-visiting the same tiles via alternate
routes. The junction cap exists to contain that blow-up, but it is the same
cap that blinds the search (RC1). The per-tile re-search multiplies all of
this by every guest, every tile — performance pressure is why none of the
limits can simply be raised.

**RC5 — No shared computation.** Thousands of guests path to the same handful
of goals (park entrances, spawn points, popular ride entrances), yet every
guest re-derives everything alone, per tile.

---

## 3. About the proposed "compass to the park entrance"

The instinct is right, but a *geometric* compass is the one thing the code
already effectively has — and it is the thing that fails. `ChooseDirection`
is already given the park-entrance coordinates as the goal
(`GuestPathFindParkEntranceLeaving`), and the search already optimises
"which edge gets me geometrically closer to the entrance". Guests who get
lost while leaving do not lack knowledge of *where* the entrance is; they
lack knowledge of *which edges lead there along the network*. A pure compass
("at each intersection, pick the edge closest to the entrance bearing")
deadlocks at any junction where every edge initially points away from the
entrance — precisely the loops and detours that cause lostness today — and it
oscillates on loops because it has no memory of failure.

The **robust generalisation of the compass idea** is a **goal-rooted distance
field** (a "flow field" / reverse-BFS): compute, once, the true walking
distance from *every path tile* to the park entrance **along the path
network**, by flooding outward from all entrances simultaneously. Then the
"compass" at every intersection is exact and O(edges): *step onto the
neighbour with the smallest stored distance*. It cannot be fooled by loops,
plazas, or concave layouts, because it is not a direction — it is a gradient
of real network distance. This keeps everything attractive about the compass
(O(1) per-guest decisions, no search, no memory needed) while fixing its
failure mode. Section 4.1 makes this the centrepiece recommendation.

---

## 4. Recommendations

Ordered by impact-per-risk; phases 1–2 are independent of each other.

### 4.1 Goal-rooted distance fields for shared goals (the network compass)

**What:** Maintain a per-tile `uint16_t` distance-to-goal field over walkable
path tiles, computed by multi-source BFS/Dijkstra flooding *backwards* from
the goal set:

- one field for **park entrances** (used by every leaving guest);
- one field for **peep spawns** (guests outside the park);
- optionally, lazily-built + LRU-cached fields for **ride entrances** that
  currently have guests heading to them.

**Decision rule:** at every tile, a goal-seeking guest inspects its ≤4
permitted edges and walks toward the neighbour with the lowest field value
(ties broken by `ScenarioRand` for crowd dispersion). No search, no junction
limits, no `PathfindHistory` needed for these goals. A guest on a path
connected to the entrance **can never get lost leaving the park** — the field
is a monotone gradient, so loops and wide regions are simply not a hazard.
Unreachable tiles hold a sentinel value, which doubles as an *instant,
accurate* "I'm cut off" signal for `checkCantFindExit` instead of today's
countdown guesswork.

**Cost:** BFS over path tiles is O(tiles); even a huge park has tens of
thousands of path tiles, and the flood is cheaper than *one* guest's current
15,000-tile DFS budget. Recompute incrementally: mark the field dirty on
footpath place/remove/edge change (the hooks already exist where
`FootpathUpdatePathWideFlags` is queued) and rebuild time-sliced over a few
ticks; guests use the stale field meanwhile, which degrades gracefully.

**Determinism:** the field is a pure function of map state, so multiplayer
sync (`GameStateSnapshots`) and replays are safe. Randomised tie-breaks must
use `ScenarioRand` (already the convention).

### 4.2 Make wide paths traversable: cost, not wall

Replace the binary "wide = stop searching / prune edge" semantics:

1. In `PeepPathfindHeuristicSearch`, treat wide tiles as ordinary searchable
   tiles with a small additive step penalty (preserves the aesthetic of
   guests favouring thin paths without making plazas opaque).
2. Delete the wide-edge pruning block in `CalculateNextDestination`
   (`headingForRideOrParkExit` section) — with (1) in place it is harmful.
3. For the distance fields of §4.1, ignore wideness entirely (or apply the
   same mild penalty): a double/triple-wide street then behaves exactly as
   one logical path, which is the requested behaviour.
4. Longer term, retire `FootpathUpdatePathWideFlags`' 1/8-per-pass sweep and
   recompute wideness deterministically for the affected neighbourhood when a
   path changes; the current eventual-consistency scheme makes pathfinding
   results depend on tile-update phase.

Note: the wide flag must survive as a *rendering/decision hint* (guests
spreading out, benches in plazas), so keep the flag, change only its meaning
to the pathfinder.

### 4.3 Replace the per-decision DFS with bounded Dijkstra/A* (closed set)

For goals that don't justify a cached field (one-off mechanic targets, ride
goals before a field exists), keep a per-decision search but make it a
proper frontier search with a visited set (`flat_hash_set` of packed
x/y/z, or a generation-stamped per-tile scratch array). The same 15,000-tile
budget then explores 15,000 *unique* tiles instead of re-walking permutations
— in practice this is the difference between seeing 5 junctions ahead and
seeing the whole park. The junction caps, `state.history`, and most of
`PathfindHistory` become unnecessary; loop handling falls out of the closed
set. This is also a large CPU win, which is what currently forces every limit
to be small (RC4).

### 4.4 Cheap interim mitigations (small diffs, immediate relief)

If a staged approach is wanted before §4.1–4.3 land:

- **Enlarge `PathfindHistory`** from 4 to 16 junctions (park-file version
  bump; importers default old saves to empty history). This directly fixes
  loops with ≤16 junctions (RC2).
- **Randomise equal-score tie-breaks** in `ChooseDirection` with
  `ScenarioRand` instead of `bitScanForward` order, so guests don't
  systematically favour direction 0 and crowds don't synchronise on the same
  mistake.
- **Raise `kMaxJunctionsGuest`** modestly (5 → 8) for goal-seeking guests; with
  the tile budget unchanged the cost is bounded.
- **Call `checkIfLost` on junction-loop detection**, not only at dead ends, so
  players get feedback when guests orbit loops.

### 4.5 Hierarchical navigation graph (long-term architecture)

Ultimately the tile-level model should be lifted into a maintained **junction
graph**: nodes are thin junctions, goal portals (ride entrances, queue ends,
park entrances) and wide-region portals; edges are corridor segments with
walking costs; contiguous wide regions collapse into single plaza nodes whose
internal movement is straight-line steering. Path build/remove events update
the graph locally. Per-guest work becomes A* over a graph that is 100–1000×
smaller than the tile map, cached as next-hop tables per goal. §4.1's
distance fields are the special case of this for shared goals and are the
right first step; the graph generalises them to *every* goal and removes the
last search limits.

### 4.6 Test and regression strategy

`test/tests/Pathfinding.cpp` + `pathfinding-tests.sv6` already provide a
deterministic harness (fixed RNG seed, step-count assertions). Extend it with
scenarios that encode today's failure modes *before* changing the engine:

- 2-wide and 3-wide straight paths and corners (goal beyond a wide region);
- a plaza (5×5 wide region) between start and goal;
- loops with 5, 8 and 16 junctions (currently defeat `PathfindHistory`);
- U-shaped detour where crow-flies distance is anti-correlated with network
  distance (defeats `CalculateHeuristicPathingScore`);
- park-exit navigation tests (current tests only cover ride goals);
- an invariant test: for a park, every path tile connected to an entrance
  (by graph reachability computed independently in the test) must yield a
  successful simulated walk to the entrance within N×distance steps.

Add lightweight runtime telemetry — a counter of `ChooseDirection` failures
falling back to `GuestPathfindAimless` per 1,000 decisions — assertable in
tests and invaluable for before/after comparison on real parks.

---

## 5. Suggested sequencing

| Phase | Content | Risk | Outcome |
|---|---|---|---|
| 0 | §4.6 tests for current failure modes; telemetry | none | safety net + baseline |
| 1 | §4.4 mitigations | low | fewer loop traps, less crowd synchronisation |
| 2 | §4.2 wide paths traversable | low–medium | double/triple-wide paths usable as one path |
| 3 | §4.1 distance field for park entrances/spawns | medium | leaving guests can no longer get lost |
| 4 | §4.3 closed-set search for ride goals | medium | junction caps removed, big CPU win |
| 5 | §4.5 hierarchical graph; retire legacy DFS | high | search limits eliminated entirely |

Compatibility notes for all phases: peep pathfinding state is serialised
(`park/ParkFile.cpp`) — changing `PathfindHistory` shape needs a park-file
version bump with import defaults; all new randomness must come from
`ScenarioRand` and all caches must be pure functions of game state to keep
multiplayer and replays in sync (`GameStateSnapshots.cpp` will catch
violations).

---

## 6. Feasibility: dynamic A* from any path coordinate to a destination

This section analyses whether full A* can be computed — and recomputed on
demand — from an arbitrary path tile to a goal at runtime, without degrading
the game: no mutation of the tile map structures, no save-format coupling,
no frame stalls, no multiplayer desyncs.

### 6.1 The graph already exists; the constant factor is the tile map

A* needs nodes, neighbours and costs. The path network already defines them
implicitly:

- **Node** = `(x, y, baseZ)` of a `PathElement`. The z component is required:
  paths stack (bridges, multi-storey layouts) and slopes shift z by ±2 per
  tile (`FootpathIsZAndDirectionValid`).
- **Neighbour expansion** = for each of ≤4 permitted edges
  (`PathGetPermittedEdges`, which already folds in banners), probe the
  adjacent tile via `MapGetFirstElementAt` (a `TilePointerIndex` lookup —
  O(1) to the tile, then a linear scan of that tile's element list) and
  validate z/slope/connection — exactly what `PeepPathfindHeuristicSearch`
  does per step today.
- **Cost** = 1 per tile (optionally +penalty for wide tiles, slopes, or
  crowding).

So no new persistent graph is required: A* can run directly against the live
map, which means **the park map is never modified and never becomes a
secondary source of truth**. The element-list scan per probe is the dominant
constant factor (typically 1–10 elements per tile); a realistic budget is
~100–300 ns per node expansion.

### 6.2 Search bookkeeping must live outside the map

The cardinal rule for "don't render the map useless": A* state
(open/closed/g-scores/parents) must never be stored in `TileElement`s.
There are no spare bits worth taking, multiple overlapping path elements per
tile make per-element state ambiguous, and anything written into elements
leaks into saves, replays and `GameStateSnapshots` comparisons.

Instead use a **shared, transient scratch structure**, one per search (or
per worker thread):

- key: packed `(x:10 | y:10 | z:8)` → 28 bits, fits `uint32_t`;
- storage: open-addressing hash map, or sparse paged arrays with a
  **generation counter** so the scratch is "cleared" by bumping an integer
  rather than memset-ing 1001×1001 cells;
- open set: binary heap keyed `(f, h, packedCoord)` — the third component
  gives **total deterministic ordering**, independent of hash iteration or
  allocation order, which is mandatory for multiplayer.

Memory: a worst-case full-network search over ~50k path tiles at ~16
bytes/node is under 1 MB, shared across all guests. Nothing is serialised.

### 6.3 The real constraint is search *frequency*, not search *cost*

Single-search cost is acceptable: with an admissible Manhattan heuristic and
unit costs, A* explores a corridor between start and goal; a typical
medium-park route (~100–300 expansions) costs tens of microseconds, and a
pathological full-network flood of ~50k expansions costs ~5–15 ms. The
problem is multiplication. At 40 game ticks/second (`kGameUpdateFPS`),
guests cross a tile every ~0.5–1 s of walking; a busy park with ~10,000
walking guests makes **~10–20k per-tile decisions per second**. Running A*
per decision — i.e. a drop-in replacement for `CalculateNextDestination`'s
current search — is out of the question (~1–4 full cores in the best case,
frame-length stalls in the worst).

The conclusion is structural, and it is the key design change: **A* only
works if its output is kept**. Compute a route once, then *follow* it:

- store per guest a compact route: a start tile + a sequence of 2-bit
  directions (`uint8_t` array, 64 tiles ≈ 16 bytes; cap at ~128 steps and
  re-path on exhaustion). 10k guests ≈ ~1 MB total;
- per tile step, route-following costs O(1): take the next direction after a
  cheap local validation (does the current tile still have that permitted
  edge — one `PathGetPermittedEdges` call, same cost as today's
  single-edge case);
- re-path only on: goal change, route exhaustion, local validation failure,
  or network invalidation (below).

This drops steady-state search load from ~10–20k/s to **~200–400/s**
(routes of ~50–100 tiles ending every ~30–60 s of walking per guest), i.e.
~0.1–0.5 ms per tick amortised — comfortably inside budget, and *cheaper*
than today's bounded DFS, which burns its 15,000-tile budget per guest per
tile at every multi-edge junction.

### 6.4 Recomputation: invalidation without storms

"Dynamically recompute from any coordinate" reduces to an invalidation
policy. Guests are always on (or entering) a tile node, so a re-path can
start from `NextLoc` at any moment — A* has no warm-up state, which is
precisely its advantage over the cached-field approach for ad-hoc goals.

- **Global path-network generation counter**: bump on footpath
  place/remove/edge change, banner change, queue assignment, ride
  open/close. Each stored route records the generation it was computed
  under.
- **Lazy validation, not eager invalidation**: a stale generation does *not*
  immediately discard the route. The guest keeps walking, validating each
  next step locally (§6.3); only a failed step — or a periodic staleness
  check at junctions — enqueues a re-path. Most edits (scenery paths far
  away) never cost anything. Optional refinement: dirty *regions* (chunk
  bitmap) so only routes passing near the edit re-path eagerly.
- **Time-sliced re-path queue**: a deterministic FIFO (ordered by entity id
  within a tick) with a per-tick budget (e.g. 1–2 ms or N searches). The
  worst case — deleting a main artery and invalidating thousands of routes
  at once — drains over a few seconds; affected guests visibly walk on for
  a moment, hit the missing tile, fall back to local edge-following
  (today's single-edge behaviour) until their request is serviced. That is
  graceful degradation, not a freeze.
- **Per-guest re-path rate limit** (e.g. ≥1 s between full searches) bounds
  adversarial layouts (players toggling a path tile every tick).

Incremental algorithms (D* Lite / LPA*) are *not* recommended here: they
repair routes cheaply after edge changes but require per-agent, per-node
g/rhs state — untenable for thousands of agents, and full A* at ~100 µs is
cheap enough that invalidate-and-recompute wins on simplicity and memory.

### 6.5 Determinism and multiplayer

All of the above is multiplayer-safe if three rules hold:

1. open-set ordering is fully deterministic (the `(f, h, packedCoord)` heap
   key — never pointer or hash order);
2. the re-path queue is drained in game-state order (entity id), budgeted by
   *count* per tick, not wall-clock time — a time-based budget would desync
   clients of different speeds; pick N searches/tick and tune N;
3. randomness (tie-breaks among equal-cost successors, dispersion) comes
   from `ScenarioRand` only.

Per-guest route storage is runtime state; serialising it in the park file is
*optional* (routes can be lazily recomputed on load from `PathfindGoal`),
which keeps the save format untouched — only a version bump if we choose to
persist routes for perfect determinism across save/load.

### 6.6 Where A* fits relative to distance fields (§4.1)

The two are complements, not competitors:

| Goal type | Best tool | Why |
|---|---|---|
| Park entrances / spawns (shared by thousands) | distance field | one BFS amortised over all guests; O(1) decisions; immune to invalidation storms (rebuild is one flood) |
| Ride entrances (≤ ~255 candidates, hot subset) | either: LRU-cached fields, or per-guest A* routes | fields win when many guests target the same ride; A* wins for the long tail |
| One-off targets (mechanic to breakdown, guest to specific bench/toilet) | dynamic A* + cached route | unique goal, no reuse to exploit |

A practical end-state: distance fields for the shared goals, dynamic A* with
route caching for everything else, both running on the same neighbour-
expansion primitives and the same scratch infrastructure, with the legacy
bounded DFS deleted. Both approaches also make the wide-path "wall"
semantics (§4.2) unnecessary — wide tiles become ordinary nodes with a small
cost penalty.

### 6.7 Verdict

Dynamic A* from any path coordinate is **feasible and safe** under four
non-negotiable design rules:

1. **never** store search state in the tile map — shared generation-stamped
   scratch only;
2. **never** search per tile-step — search per *route*, follow the cached
   route with O(1) local validation;
3. **never** recompute eagerly on map edits — lazy validation plus a
   deterministic, budgeted re-path queue;
4. **never** let ordering depend on memory layout — deterministic heap keys
   and `ScenarioRand` only.

Violating rule 2 is what makes naive "just use real A*" proposals fail (and
is effectively why the original game shipped a crippled DFS); the other
three rules are what "without rendering the park map useless" means in
practice: the map stays a pure, unannotated source of truth, saves and
multiplayer stay byte-compatible, and the per-tick cost stays bounded and
tunable.
