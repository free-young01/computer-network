#include "netsim2.h"

#include <stdint.h>

struct RouterState {
    int my_id;
    int num_nodes;

    static const int MAX_NODES = 100;
    static const int INF = 999999;

    int lsdb[MAX_NODES][MAX_NODES];
    uint32_t my_seq;
    uint32_t max_seq[MAX_NODES];

    bool live[MAX_NODES];
    int live_cost[MAX_NODES];

    int dist[MAX_NODES];
    int next_hop[MAX_NODES];
    bool dirty;
};

namespace {

const uint8_t TYPE_FULL = 0;
const uint8_t TYPE_DELTA = 1;
const uint8_t COST_DOWN = 255;

uint32_t read_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xff);
    p[1] = (uint8_t)((v >> 16) & 0xff);
    p[2] = (uint8_t)((v >> 8) & 0xff);
    p[3] = (uint8_t)(v & 0xff);
}

bool valid_node(const RouterState* s, int node) {
    return node >= 0 && node < s->num_nodes;
}

bool can_send_to(const RouterState* s, int neighbor) {
    return valid_node(s, neighbor) && s->live[neighbor];
}

uint32_t fresh_seq(RouterState* s) {
    ++s->my_seq;
    return s->my_seq;
}

void send_full_row_to(RouterState* s, int neighbor, int origin, uint32_t seq) {
    if (!can_send_to(s, neighbor)) return;

    uint8_t payload[7 + 2 * RouterState::MAX_NODES];
    int count = 0;
    for (int i = 0; i < s->num_nodes; ++i) {
        if (s->lsdb[origin][i] != RouterState::INF) ++count;
    }

    payload[0] = TYPE_FULL;
    payload[1] = (uint8_t)origin;
    write_u32_be(payload + 2, seq);
    payload[6] = (uint8_t)count;

    int pos = 7;
    for (int i = 0; i < s->num_nodes; ++i) {
        if (s->lsdb[origin][i] == RouterState::INF) continue;
        payload[pos++] = (uint8_t)i;
        payload[pos++] = (uint8_t)s->lsdb[origin][i];
    }

    send_control(neighbor, payload, pos);
}

void send_full_lsa_to(RouterState* s, int neighbor) {
    send_full_row_to(s, neighbor, s->my_id, fresh_seq(s));
}

void sync_known_lsdb_to(RouterState* s, int neighbor) {
    for (int origin = 0; origin < s->num_nodes; ++origin) {
        if (origin == s->my_id) continue;
        if (s->max_seq[origin] == 0) continue;
        send_full_row_to(s, neighbor, origin, s->max_seq[origin]);
    }
}

void send_delta_lsa_to(RouterState* s, int neighbor, int changed_neighbor, int new_cost, uint32_t seq) {
    if (!can_send_to(s, neighbor)) return;

    uint8_t payload[8];
    payload[0] = TYPE_DELTA;
    payload[1] = (uint8_t)s->my_id;
    write_u32_be(payload + 2, seq);
    payload[6] = (uint8_t)changed_neighbor;
    payload[7] = (new_cost == RouterState::INF) ? COST_DOWN : (uint8_t)new_cost;

    send_control(neighbor, payload, 8);
}

void flood_payload(RouterState* s, int from, const uint8_t* payload, int len) {
    for (int i = 0; i < s->num_nodes; ++i) {
        if (i == from) continue;
        if (can_send_to(s, i)) send_control(i, payload, len);
    }
}

void init_route_arrays(RouterState* s) {
    for (int i = 0; i < s->num_nodes; ++i) {
        s->dist[i] = RouterState::INF;
        s->next_hop[i] = -1;
    }
}

void recompute_from_source(const RouterState* s, int source, int out_dist[RouterState::MAX_NODES]) {
    bool used[RouterState::MAX_NODES];
    for (int i = 0; i < s->num_nodes; ++i) {
        out_dist[i] = RouterState::INF;
        used[i] = false;
    }
    out_dist[source] = 0;

    for (int iter = 0; iter < s->num_nodes; ++iter) {
        int u = -1;
        for (int i = 0; i < s->num_nodes; ++i) {
            if (used[i]) continue;
            if (u == -1 || out_dist[i] < out_dist[u] ||
                (out_dist[i] == out_dist[u] && i < u)) {
                u = i;
            }
        }
        if (u == -1 || out_dist[u] == RouterState::INF) break;
        used[u] = true;

        for (int v = 0; v < s->num_nodes; ++v) {
            int cost = s->lsdb[u][v];
            if (cost == RouterState::INF) continue;
            int cand = out_dist[u] + cost;
            if (cand < out_dist[v]) out_dist[v] = cand;
        }
    }
}

void recompute_routes(RouterState* s) {
    bool used[RouterState::MAX_NODES];
    int first_hop[RouterState::MAX_NODES];

    init_route_arrays(s);
    for (int i = 0; i < s->num_nodes; ++i) {
        used[i] = false;
        first_hop[i] = -1;
    }

    s->dist[s->my_id] = 0;

    for (int iter = 0; iter < s->num_nodes; ++iter) {
        int u = -1;
        for (int i = 0; i < s->num_nodes; ++i) {
            if (used[i]) continue;
            if (u == -1 || s->dist[i] < s->dist[u] ||
                (s->dist[i] == s->dist[u] && first_hop[i] < first_hop[u])) {
                u = i;
            }
        }
        if (u == -1 || s->dist[u] == RouterState::INF) break;
        used[u] = true;

        for (int v = 0; v < s->num_nodes; ++v) {
            int edge_cost = s->lsdb[u][v];
            if (edge_cost == RouterState::INF) continue;

            int hop;
            if (u == s->my_id) {
                if (!s->live[v]) continue;
                hop = v;
            } else {
                hop = first_hop[u];
            }
            if (hop < 0 || !s->live[hop]) continue;

            int cand = s->dist[u] + edge_cost;
            if (cand < s->dist[v] ||
                (cand == s->dist[v] &&
                 (first_hop[v] == -1 || hop < first_hop[v]))) {
                s->dist[v] = cand;
                first_hop[v] = hop;
                s->next_hop[v] = hop;
            }
        }
    }

    s->next_hop[s->my_id] = -1;
    s->dirty = false;
}

void clear_origin_row(RouterState* s, int origin) {
    for (int i = 0; i < s->num_nodes; ++i) {
        s->lsdb[origin][i] = RouterState::INF;
    }
}

void init_state(RouterState* s, int my_id, int num_nodes) {
    s->my_id = my_id;
    s->num_nodes = num_nodes;
    s->my_seq = 1;
    s->dirty = true;

    for (int i = 0; i < RouterState::MAX_NODES; ++i) {
        s->max_seq[i] = 0;
        s->live[i] = false;
        s->live_cost[i] = RouterState::INF;
        s->dist[i] = RouterState::INF;
        s->next_hop[i] = -1;
        for (int j = 0; j < RouterState::MAX_NODES; ++j) {
            s->lsdb[i][j] = RouterState::INF;
        }
    }
}

}  // namespace

extern "C" {

RouterState* router_init(int my_id,
                         int num_nodes,
                         const int* neighbor_ids,
                         const int* link_costs,
                         int num_neighbors) {
    RouterState* s = new RouterState;
    init_state(s, my_id, num_nodes);

    for (int i = 0; i < num_neighbors; ++i) {
        int n = neighbor_ids[i];
        int cost = link_costs[i];
        if (!valid_node(s, n)) continue;
        s->live[n] = true;
        s->live_cost[n] = cost;
        s->lsdb[my_id][n] = cost;
    }

    uint32_t init_seq = fresh_seq(s);
    for (int i = 0; i < num_nodes; ++i) {
        if (s->live[i]) {
            send_full_row_to(s, i, s->my_id, init_seq);
        }
    }

    recompute_routes(s);
    return s;
}

void on_link_change(RouterState* s, int neighbor, int new_cost) {
    if (!valid_node(s, neighbor)) return;

    bool was_live = s->live[neighbor];
    bool now_live = (new_cost != NETSIM2_NO_LINK);
    int encoded_cost = now_live ? new_cost : RouterState::INF;

    s->live[neighbor] = now_live;
    s->live_cost[neighbor] = encoded_cost;
    s->lsdb[s->my_id][neighbor] = encoded_cost;
    s->dirty = true;

    if (now_live && !was_live) {
        uint32_t seq_full = fresh_seq(s);
        send_full_row_to(s, neighbor, s->my_id, seq_full);
        
        sync_known_lsdb_to(s, neighbor); 

        uint32_t seq_delta = fresh_seq(s);
        for (int i = 0; i < s->num_nodes; ++i) {
            if (i == neighbor) continue;
            send_delta_lsa_to(s, i, neighbor, encoded_cost, seq_delta);
        }
    } else {
        uint32_t seq_delta = fresh_seq(s);
        for (int i = 0; i < s->num_nodes; ++i) {
            send_delta_lsa_to(s, i, neighbor, encoded_cost, seq_delta);
        }
    }

    recompute_routes(s);
}

void on_control(RouterState* s, int from, const uint8_t* payload, int len) {
    if (payload == 0 || len < 6) return;

    int type = payload[0];
    int origin = payload[1];
    if (!valid_node(s, origin)) return;
    if (origin == s->my_id) return;

    uint32_t seq = read_u32_be(payload + 2);
    if (seq <= s->max_seq[origin]) return;
    s->max_seq[origin] = seq;

    bool changed = false;
    if (type == TYPE_FULL) {
        if (len < 7) return;
        int count = payload[6];
        if (len != 7 + 2 * count) return;

        bool advertised[RouterState::MAX_NODES];
        int advertised_cost[RouterState::MAX_NODES];
        for (int i = 0; i < s->num_nodes; ++i) {
            advertised[i] = false;
            advertised_cost[i] = RouterState::INF;
        }
        for (int i = 0; i < count; ++i) {
            int pos = 7 + 2 * i;
            int neighbor = payload[pos];
            int cost = payload[pos + 1];
            if (valid_node(s, neighbor) && neighbor != origin &&
                cost > 0 && cost < COST_DOWN) {
                advertised[neighbor] = true;
                advertised_cost[neighbor] = cost;
            }
        }

        for (int i = 0; i < s->num_nodes; ++i) {
            if (s->lsdb[origin][i] != advertised_cost[i]) {
                changed = true;
                break;
            }
        }
        clear_origin_row(s, origin);
        for (int i = 0; i < count; ++i) {
            int pos = 7 + 2 * i;
            int neighbor = payload[pos];
            if (valid_node(s, neighbor)) s->lsdb[origin][neighbor] = advertised_cost[neighbor];
        }
    } else if (type == TYPE_DELTA) {
        if (len != 8) return;
        int neighbor = payload[6];
        int cost_byte = payload[7];
        if (!valid_node(s, neighbor) || neighbor == origin) return;
        int new_cost = (cost_byte == COST_DOWN) ? RouterState::INF : cost_byte;
        if (s->lsdb[origin][neighbor] != new_cost) {
            s->lsdb[origin][neighbor] = new_cost;
            changed = true;
        }
    } else {
        return;
    }

    if (changed) {
        s->dirty = true;
        flood_payload(s, from, payload, len);
        recompute_routes(s);
    }
}

int on_packet(RouterState* s, int dst) {
    if (!valid_node(s, dst)) return -1;

    int hop = s->next_hop[dst];
    if (hop >= 0 && can_send_to(s, hop)) return hop;

    recompute_routes(s);
    hop = s->next_hop[dst];
    if (hop >= 0 && can_send_to(s, hop)) return hop;

    int best = -1;
    int best_score = RouterState::INF;
    int neighbor_dist[RouterState::MAX_NODES];

    for (int n = 0; n < s->num_nodes; ++n) {
        if (!s->live[n]) continue;
        
        recompute_from_source(s, n, neighbor_dist);
        int estimate = neighbor_dist[dst];
        if (estimate == RouterState::INF) continue;
        
        int score = s->live_cost[n] + estimate;
        if (best == -1 || score < best_score || (score == best_score && n < best)) {
            best = n;
            best_score = score;
        }
    }

    if (best != -1 && can_send_to(s, best)) return best;

    int cheapest_neighbor = -1;
    int min_cost = RouterState::INF;
    
    for (int n = 0; n < s->num_nodes; ++n) {
        if (s->live[n] && s->live_cost[n] < min_cost) {
            min_cost = s->live_cost[n];
            cheapest_neighbor = n;
        }
    }

    return cheapest_neighbor; 
}

void on_timer(RouterState* s) {
    (void)s;
}

void router_shutdown(RouterState* s) {
    delete s;
}

}  // extern "C"
