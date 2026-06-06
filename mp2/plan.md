# MP2 Router Final Plan

## 1. Core philosophy

Implement a **triggered incremental Link State (LS)** router.

- **Bootstrap once at `router_init()`**: advertise the initial topology immediately so stable scenarios still converge.
- **Trigger on changes only**: on `on_link_change()`, send only the minimum information needed for that event.
- **No periodic advertisements**: the simulator delivers control messages deterministically, so periodic flooding only increases `control_bytes_total`.
- **No ACK / retransmission layer**: `send_control()` copies the payload internally, and in-flight control messages are not lost by later link changes, so extra reliability machinery is unnecessary.
- **Deterministic shortest-path computation**: recompute routes using Dijkstra with a fixed tie-break rule so results are reproducible.
- **Never rely on arbitrary packet dropping**: `on_packet()` should avoid returning `-1` unless there is truly no legal forward hop left.

## 2. RouterState layout

Use a fixed-size state because `N <= 100`.

```cpp
struct RouterState {
    int my_id;
    int num_nodes;

    static constexpr int INF = 999999;

    int lsdb[100][100];      // lsdb[u][v] = cost of the link node u advertised to v
    uint32_t my_seq;         // sequence number used for every control message sent by this router
    uint32_t max_seq[100];   // newest seq observed from each origin

    bool live[100];          // current directly connected up/down status from my viewpoint
    int live_cost[100];      // current direct link cost for each neighbor, INF if down/not adjacent

    int dist[100];           // shortest known distance from me to each destination
    int next_hop[100];       // chosen next hop for each destination, -1 if unknown
    bool dirty;              // topology changed; routes need recomputation
};
```

### State interpretation

- `lsdb[u][v]` stores the cost advertised by origin `u` for edge `u -> v`.
- `live[]` and `live_cost[]` store the router’s own current directly connected neighbor status.
- `dist[]` and `next_hop[]` are derived routing results.
- `my_seq` is monotonic and shared by all control messages emitted by this router.

## 3. Packet formats

Use manual byte packing into `uint8_t payload[]`. Do **not** send C++ structs directly.

### 3.1 Full LSA
Used for bootstrap and for synchronizing a newly appeared neighbor.

```
[Type: 1B = 0]
[Origin: 1B]
[Seq: 4B, big-endian]
[NumNeighbors: 1B]
[Repeated NumNeighbors times: NeighborID 1B + Cost 1B]
```

### 3.2 Delta LSA
Used for a single link change.

```
[Type: 1B = 1]
[Origin: 1B]
[Seq: 4B, big-endian]
[NeighborID: 1B]
[NewCost: 1B]
```

- Encode link-down as `NewCost = 255`.
- Since node IDs and costs are small, 1 byte is enough for each.

### 3.3 Serialization rules

- Use fixed offsets and explicit shifts for packing/unpacking.
- Never depend on `sizeof(struct)` or compiler padding.
- If a payload is malformed or too short, ignore it safely.

## 4. Control-message rules

### 4.1 Sequence number discipline

Every control message emitted by a router must use a **fresh, monotonically increasing sequence number**.

- `my_seq` starts at `1` in `router_init()`.
- Every time this router emits a Full LSA or Delta LSA, increment `my_seq` first, then write it into the packet.
- Do not reuse the same seq for multiple sends of the same logical event.

### 4.2 Duplicate suppression

On receive:

- Parse `Origin` and `Seq`.
- If `Seq <= max_seq[Origin]`, drop the packet.
- Otherwise set `max_seq[Origin] = Seq` and apply the update.

This prevents infinite flooding loops and stale updates.

### 4.3 Flooding scope

Forward control packets only to current live directly connected neighbors, excluding the neighbor they came from.

- `send_control()` must only target currently live neighbors.
- The simulator ignores invalid targets, but the code should not rely on that.

## 5. Callback behavior

### 5.1 `router_init()`

At startup:

1. Initialize all arrays.
2. Set all `lsdb[u][v] = INF`.
3. Set all `max_seq[u] = 0`.
4. Set all `next_hop[d] = -1`.
5. Record the initial directly connected neighbors in `live[]` and `live_cost[]`.
6. Record the local row `lsdb[my_id][neighbor] = cost` for every initial neighbor.
7. Build and send a **Full LSA** to every initial neighbor.
8. Run `recompute_routes()` immediately.

The bootstrap is mandatory so stable scenarios still converge.

### 5.2 `on_link_change(neighbor, new_cost)`

Handle all link changes locally first.

- If `new_cost == NETSIM2_NO_LINK`, mark that direct link as down.
- Otherwise mark it as up with the new cost.
- Update `lsdb[my_id][neighbor]` to the new direct cost or `INF` for down.
- Set `dirty = true`.

Then advertise:

#### Case A: New link appears

If this neighbor was previously not live:

1. Update local neighbor state.
2. Send a **Full LSA** to the newly appeared neighbor so it gets synchronized quickly.
3. Send a **Delta LSA** to the other live neighbors if needed for the change.
4. Increment `my_seq` separately for each control packet emitted.

#### Case B: Existing link cost changes or goes down

1. Update local neighbor state.
2. Send a **Delta LSA** to all current live neighbors.
3. Increment `my_seq` for that advertisement.

Finally, recompute routes immediately.

### 5.3 `on_control(from, payload, len)`

On receiving a control message:

1. Validate minimum length.
2. Unpack `Type`, `Origin`, `Seq`.
3. Reject stale packets with `Seq <= max_seq[Origin]`.
4. Update `max_seq[Origin]`.
5. Apply the advertised LSDB information.
6. Mark `dirty = true`.
7. Flood the packet to every live neighbor except `from`.
8. Recompute routes immediately.

Important: keep the update logic idempotent. The same advertisement may arrive multiple times through different paths.

### 5.4 `recompute_routes()`

Run Dijkstra from `my_id` over the current LSDB snapshot.

- Use `lsdb[u][v]` as the edge weight when `lsdb[u][v] != INF`.
- The graph is directed in the LSDB representation, but the router’s own direct neighbor state must still be respected for actual forwarding.
- If two paths have the same total cost, choose the one whose next hop has the smaller node ID.
- Update both `dist[]` and `next_hop[]`.
- Clear `dirty` when done.

### 5.5 `on_packet(dst)`

Forwarding rule:

1. If the current route to `dst` is valid and the chosen next hop is a live direct neighbor, return it immediately.
2. Otherwise call `recompute_routes()` and try again.
3. If still invalid, use a **last-resort heuristic**:
   - Evaluate each live direct neighbor `n`.
   - Prefer the neighbor minimizing `direct_cost(my_id, n) + estimated_distance`. (Crucial: The estimated_distance must be computed LOCALLY by running a virtual Dijkstra from neighbor 'n' as the source using this router's own internal lsdb[][] matrix. Never attempt to access another router's state or arrays directly, as it violates memory isolation.)
   - Break ties by smaller neighbor ID.
4. Return that neighbor only if it is a currently live direct neighbor.
5. Return `-1` only if no live neighbor exists at all.

This policy avoids arbitrary fallback choices that could create TTL loops.

### 5.6 `on_timer()`

Use timer events only if needed for internal housekeeping.

- Do not introduce periodic advertisement timers.
- If a timer is used at all, it should serve only local maintenance, not retransmission.

### 5.7 `router_shutdown()`

Free any dynamically allocated memory if the implementation uses it.

## 6. Implementation notes

- Prefer fixed arrays and simple loops over complex dynamic containers.
- Use helper functions for packing/unpacking payloads.
- Keep the routing logic deterministic.
- Avoid stdout entirely.
- Debug output, if needed, must go to stderr only.

## 7. Practical design choices

### Recommended behavior for bootstrap and updates

- **Initial full flood** from `router_init()`.
- **Delta flood** on ordinary link changes.
- **Targeted full sync** when a new neighbor appears and needs the current state immediately.
- **Immediate recomputation** after every local or received update.

### Recommended tie-break rule

When multiple shortest paths have the same cost:

1. Prefer the smaller next-hop ID.
2. If needed, keep the result stable across recomputations.

### Recommended safety rule

Do not use an arbitrary neighbor as the normal fallback. Only fall back after recomputation fails, and only among live neighbors with a cost-based heuristic.

## 8. Final summary

The router should behave as:

**Bootstrap once, trigger on change, suppress duplicates with seq numbers, recompute deterministically, and forward only to live neighbors.**

That gives the best balance of correctness and control-byte efficiency for this simulator.

