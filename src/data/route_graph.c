/**
 * @file    route_graph.c
 * @brief   Chain forward-star route graph + A* search.
 *
 * All arrays are dynamically allocated — no fixed limits.
 * Edges built via spatial hash proximity queries (O(n) per node).
 */

#include "route_graph.h"
#include "ds/hash_table.h"
#include "ds/spatial_hash.h"
#include "utils/logger.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define INF_SCORE       (1e18f)

/* =========================================================================
 *  Min-heap (keyed by f_score)
 * ========================================================================= */

static void heap_swap(RouteGraph* g, int i, int j)
{
    int tmp = g->heap[i];
    g->heap[i] = g->heap[j];
    g->heap[j] = tmp;
    g->heap_pos[g->heap[i]] = i;
    g->heap_pos[g->heap[j]] = j;
}

static void heap_sift_up(RouteGraph* g, int idx)
{
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (g->heap_key[g->heap[idx]] < g->heap_key[g->heap[parent]]) {
            heap_swap(g, idx, parent);
            idx = parent;
        } else break;
    }
}

static void heap_sift_down(RouteGraph* g, int idx)
{
    int size = g->heap_size;
    while (1) {
        int smallest = idx;
        int left  = 2 * idx + 1;
        int right = 2 * idx + 2;
        if (left < size && g->heap_key[g->heap[left]] < g->heap_key[g->heap[smallest]])
            smallest = left;
        if (right < size && g->heap_key[g->heap[right]] < g->heap_key[g->heap[smallest]])
            smallest = right;
        if (smallest != idx) { heap_swap(g, idx, smallest); idx = smallest; }
        else break;
    }
}

static void heap_push(RouteGraph* g, int node, float key)
{
    if (g->heap_pos[node] >= 0) {
        float old = g->heap_key[node];
        g->heap_key[node] = key;
        if (key < old) heap_sift_up(g, g->heap_pos[node]);
        else if (key > old) heap_sift_down(g, g->heap_pos[node]);
        return;
    }
    int pos = g->heap_size++;
    g->heap[pos]        = node;
    g->heap_key[node]   = key;
    g->heap_pos[node]   = pos;
    heap_sift_up(g, pos);
}

static int heap_pop(RouteGraph* g)
{
    if (g->heap_size <= 0) return -1;
    int result = g->heap[0];
    g->heap_pos[result] = -1;
    g->heap_size--;
    if (g->heap_size > 0) {
        g->heap[0] = g->heap[g->heap_size];
        g->heap_pos[g->heap[0]] = 0;
        heap_sift_down(g, 0);
    }
    return result;
}

/* =========================================================================
 *  Node lookup
 * ========================================================================= */

static int find_node(const RouteGraph* g, const char* ident)
{
    if (!g->node_lookup || !ident) return -1;
    int* idx = (int*)ht_get((HashTable*)g->node_lookup, ident);
    return idx ? *idx : -1;
}

/* =========================================================================
 *  Chain forward-star: add directed edge
 * ========================================================================= */

static void ensure_edge_capacity(RouteGraph* g, int needed)
{
    if (g->edge_count + needed <= g->edge_capacity) return;
    int new_cap = g->edge_capacity ? g->edge_capacity * 2 : 65536;
    while (new_cap < g->edge_count + needed) new_cap *= 2;
    g->to   = (int*)  realloc(g->to,   (size_t)new_cap * sizeof(int));
    g->next = (int*)  realloc(g->next, (size_t)new_cap * sizeof(int));
    g->dist_nm = (float*)realloc(g->dist_nm, (size_t)new_cap * sizeof(float));
    g->edge_capacity = new_cap;
}

static void add_edge_cfs(RouteGraph* g, int from, int to, float dist)
{
    if (from < 0 || from >= g->node_count) return;
    if (to   < 0 || to   >= g->node_count) return;
    ensure_edge_capacity(g, 1);

    int e = g->edge_count++;
    g->to[e]      = to;
    g->dist_nm[e] = dist;
    g->next[e]    = g->head[from];
    g->head[from]  = e;
}

static void add_edge_bidi(RouteGraph* g, int a, int b, float dist)
{
    add_edge_cfs(g, a, b, dist);
    add_edge_cfs(g, b, a, dist);
}

/* =========================================================================
 *  Graph construction
 * ========================================================================= */

static int add_node(RouteGraph* g, const char* ident, WaypointType type,
                     GeoPos pos, float elev)
{
    if (ht_contains((HashTable*)g->node_lookup, ident)) {
        int* idx = (int*)ht_get((HashTable*)g->node_lookup, ident);
        return idx ? *idx : -1;
    }
    /* Grow node array if needed */
    if (g->node_count >= g->node_capacity) {
        int new_cap = g->node_capacity ? g->node_capacity * 2 : 8192;
        g->nodes = (RouteNode*)realloc(g->nodes, (size_t)new_cap * sizeof(RouteNode));
        g->head  = (int*)realloc(g->head,  (size_t)new_cap * sizeof(int));
        g->g_score    = (float*)realloc(g->g_score,    (size_t)new_cap * sizeof(float));
        g->f_score    = (float*)realloc(g->f_score,    (size_t)new_cap * sizeof(float));
        g->came_from  = (int*)  realloc(g->came_from,  (size_t)new_cap * sizeof(int));
        g->closed_set = (int*)  realloc(g->closed_set, (size_t)new_cap * sizeof(int));
        g->heap       = (int*)  realloc(g->heap,       (size_t)new_cap * sizeof(int));
        g->heap_key   = (float*)realloc(g->heap_key,   (size_t)new_cap * sizeof(float));
        g->heap_pos   = (int*)  realloc(g->heap_pos,   (size_t)new_cap * sizeof(int));
        for (int i = g->node_capacity; i < new_cap; i++) {
            g->head[i]      = -1;
            g->heap_pos[i]  = -1;
            g->g_score[i]   = (float)INF_SCORE;
            g->closed_set[i]= 0;
        }
        g->node_capacity = new_cap;
    }

    int idx = g->node_count++;
    memset(&g->nodes[idx], 0, sizeof(RouteNode));
    strncpy(g->nodes[idx].ident, ident, sizeof(g->nodes[idx].ident) - 1);
    g->nodes[idx].type  = type;
    g->nodes[idx].pos   = pos;
    g->nodes[idx].elevation_ft = elev;
    g->head[idx]        = -1;

    if (g->node_lookup) {
        int* p = (int*)malloc(sizeof(int));
        if (p) { *p = idx; ht_put((HashTable*)g->node_lookup, ident, p); }
    }
    return idx;
}

RouteGraph* route_graph_build(const FMCState* state)
{
    if (!state) return NULL;

    RouteGraph* g = calloc(1, sizeof(RouteGraph));
    if (!g) return NULL;

    g->node_lookup = ht_create(65536);
    g->node_count  = 0;
    g->edge_count  = 0;
    g->node_capacity = 0;
    g->edge_capacity = 0;
    g->heap_size   = 0;
    g->visit_token = 0;

    /* --- Phase 1: Add all airports --- */
    for (int i = 0; i < state->nav_apt_count; i++) {
        const Airport* apt = &state->nav_airports[i];
        add_node(g, apt->icao, WPT_AIRPORT, apt->pos, apt->elevation_ft);
    }

    /* --- Phase 1b: Add all waypoints from nav database --- */
    for (int i = 0; i < state->nav_wpt_count; i++) {
        const Waypoint* wpt = &state->nav_waypoints[i];
        add_node(g, wpt->ident, wpt->type, wpt->pos, wpt->elevation_ft);
    }

    LOG_INFO("RouteGraph: %d nodes (airports + waypoints)", g->node_count);

    /* --- Phase 2: Proximity edges via spatial hash --- */
    if (state->spatial_hash) {
        #define PROX_QUERY_MAX  32
        #define PROX_RANGE_NM   300.0f
        NavSpatialEntry* results[PROX_QUERY_MAX];
        int total_added = 0, zero_results = 0, find_failed = 0;

        for (int i = 0; i < g->node_count; i++) {
            int added = 0;
            /* Query 4 cardinal directions (N/E/S/W) for even coverage */
            for (int dir = 0; dir < 4 && added < 20; dir++) {
                float hdg = (float)(dir * 90);
                int n = spatial_hash_query(state->spatial_hash,
                             g->nodes[i].pos.lat_deg,
                             g->nodes[i].pos.lon_deg,
                             hdg, PROX_RANGE_NM,
                             results, PROX_QUERY_MAX);

                if (n == 0) { zero_results++; continue; }

                for (int r = 0; r < n && added < 20; r++) {
                NavSpatialEntry* e = results[r];
                if (!e || !e->ident[0]) continue;
                int j = find_node(g, e->ident);
                if (j < 0) { find_failed++; continue; }
                if (j == i) continue;
                float dist = (float)geo_distance_nm(g->nodes[i].pos,
                                                     g->nodes[j].pos);
                    add_edge_bidi(g, i, j, dist);
                    added++; total_added++;
                }
            } /* dir loop */
        }
        LOG_INFO("RouteGraph: proximity — %d edges, %d zero-results, %d find-failed",
                 total_added, zero_results, find_failed);
    } else {
        LOG_WARN("RouteGraph: spatial_hash is NULL — no edges built!");
    }

    /* --- Phase 3: Airport ↔ waypoint direct connections ---
     * Airports are NOT in the spatial hash, so waypoints can't find them.
     * Scan all nodes and connect airports directly to nearby waypoints
     * using great-circle distance (cheap — only 162 airports × ~160K waypoints,
     * filtered by lat/lon bounding box first). */
    {
        int apt_edges = 0;
        for (int a = 0; a < g->node_count; a++) {
            if (g->nodes[a].type != WPT_AIRPORT) continue;
            double alat = g->nodes[a].pos.lat_deg;
            double alon = g->nodes[a].pos.lon_deg;
            int added = 0;
            for (int w = 0; w < g->node_count && added < 8; w++) {
                if (a == w) continue;
                /* Quick bounding-box filter: ±3° ≈ ±180 NM */
                if (fabs(g->nodes[w].pos.lat_deg - alat) > 3.0) continue;
                if (fabs(g->nodes[w].pos.lon_deg - alon) > 3.5) continue;
                float d = (float)geo_distance_nm(g->nodes[a].pos, g->nodes[w].pos);
                if (d > 300.0f) continue;
                add_edge_bidi(g, a, w, d);
                added++; apt_edges++;
            }
        }
        LOG_INFO("RouteGraph: airport direct links — %d edges", apt_edges);
    }

    /* Quick sanity: check KHIO and KSEA edge counts + hash table health */
    {
        int ht_sz = g->node_lookup ? ((HashTable*)g->node_lookup)->size : -1;
        int khi = find_node(g, "KHIO"), kse = find_node(g, "KSEA");
        int khi_edges = 0, kse_edges = 0;
        if (khi >= 0) for (int e = g->head[khi]; e >= 0; e = g->next[e]) khi_edges++;
        if (kse >= 0) for (int e = g->head[kse]; e >= 0; e = g->next[e]) kse_edges++;

        /* Also test ht_get directly */
        void* raw_khi = ht_get((HashTable*)g->node_lookup, "KHIO");
        void* raw_kse = ht_get((HashTable*)g->node_lookup, "KSEA");

        LOG_INFO("RouteGraph: ht_size=%d, KHIO(idx %d raw %p)=%d edges, KSEA(idx %d raw %p)=%d edges",
                 ht_sz, khi, raw_khi, khi_edges, kse, raw_kse, kse_edges);

        /* Auto-test: KHIO→KSEA waypoint dump + edge check */
        {
            int fesas = find_node(g, "FESAS");
            int jbbdt = find_node(g, "JBBDT");
            int f2k = 0, k2f = 0, f2j = 0;
            if (fesas >= 0) for (int e = g->head[fesas]; e >= 0; e = g->next[e]) {
                if (g->to[e] == kse) f2k = 1;
                if (g->to[e] == jbbdt) f2j = 1;
            }
            if (kse >= 0) for (int e = g->head[kse]; e >= 0; e = g->next[e])
                if (g->to[e] == fesas) k2f = 1;
            LOG_INFO("Edges: FESAS→KSEA=%d KSEA→FESAS=%d FESAS→JBBDT=%d",
                     f2k, k2f, f2j);

            RoutePath tp; memset(&tp, 0, sizeof(tp));
            if (route_graph_find_route(g, "KHIO", "KSEA", 35000, &tp)) {
                LOG_INFO("KHIO→KSEA: %d WP, %.0f NM",
                         tp.waypoint_count, (double)tp.total_distance_nm);
                for (int w = 0; w < tp.waypoint_count; w++)
                    LOG_INFO("  [%d] %s (%.4f %.4f)",
                             w, tp.waypoints[w].ident,
                             tp.waypoints[w].pos.lat_deg,
                             tp.waypoints[w].pos.lon_deg);
            }
        }
    }

    int total_edges = g->edge_count;
    LOG_INFO("RouteGraph: build complete — %d nodes, %d directed edges",
             g->node_count, total_edges);
    return g;
}

void route_graph_destroy(RouteGraph* g)
{
    if (!g) return;
    free(g->nodes);
    free(g->head);  free(g->to);  free(g->next);  free(g->dist_nm);
    free(g->g_score); free(g->f_score); free(g->came_from); free(g->closed_set);
    free(g->heap); free(g->heap_key); free(g->heap_pos);
    if (g->node_lookup) ht_destroy((HashTable*)g->node_lookup, 1);
    free(g);
}

/* =========================================================================
 *  A* search
 * ========================================================================= */

static float heuristic(const RouteNode* a, const RouteNode* b)
{
    return (float)geo_distance_nm(a->pos, b->pos);
}

int route_graph_find_route(RouteGraph* g,
                           const char* origin_icao,
                           const char* dest_icao,
                           float cruise_alt_ft,
                           RoutePath* path)
{
    (void)cruise_alt_ft;
    if (!g || !origin_icao || !dest_icao || !path) return 0;
    memset(path, 0, sizeof(*path));

    int start = find_node(g, origin_icao);
    int goal  = find_node(g, dest_icao);
    if (start < 0) { LOG_WARN("A*: origin '%s' not in graph", origin_icao); return 0; }
    if (goal  < 0) { LOG_WARN("A*: dest '%s' not in graph",   dest_icao); return 0; }

    g->visit_token++;
    g->heap_size = 0;

    int N = g->node_count;
    for (int i = 0; i < N; i++) {
        g->g_score[i]   = (float)INF_SCORE;
        g->f_score[i]   = (float)INF_SCORE;
        g->heap_pos[i]  = -1;
    }

    g->g_score[start]    = 0.0f;
    g->f_score[start]    = heuristic(&g->nodes[start], &g->nodes[goal]);
    g->came_from[start]  = -1;
    heap_push(g, start, g->f_score[start]);

    int explored = 0;

    while (g->heap_size > 0) {
        int current = heap_pop(g);
        explored++;

        if (current == goal) {
            int steps = 0;
            int node  = goal;
            while (node >= 0 && steps < ROUTE_PATH_MAX_WP) {
                steps++; node = g->came_from[node];
            }
            path->waypoint_count = steps;
            node = goal;
            for (int i = steps - 1; i >= 0; i--) {
                memset(&path->waypoints[i], 0, sizeof(Waypoint));
                strncpy(path->waypoints[i].ident, g->nodes[node].ident,
                        sizeof(path->waypoints[i].ident) - 1);
                path->waypoints[i].type = g->nodes[node].type;
                path->waypoints[i].pos  = g->nodes[node].pos;
                node = g->came_from[node];
            }
            path->found = 1;
            path->total_distance_nm = g->g_score[goal];
            path->estimated_time_hours = path->total_distance_nm / 450.0f;
            LOG_INFO("A*: route found! %d WP, %.0f NM, explored %d",
                     path->waypoint_count, (double)path->total_distance_nm, explored);
            return 1;
        }

        g->closed_set[current] = g->visit_token;

        /* Precompute bearing to goal for direction penalty */
        float brg_to_goal = (float)geo_bearing_deg(g->nodes[current].pos,
                                                    g->nodes[goal].pos);

        /* Traverse chain forward-star */
        for (int e = g->head[current]; e >= 0; e = g->next[e]) {
            int nb = g->to[e];
            if (g->closed_set[nb] == g->visit_token) continue;

            /* Direction penalty: prefer edges heading toward the goal.
             * Off-course edges cost extra 0.025 NM per degree of deviation. */
            float brg_to_nb = (float)geo_bearing_deg(g->nodes[current].pos,
                                                      g->nodes[nb].pos);
            float diff = fabsf(brg_to_nb - brg_to_goal);
            if (diff > 180.0f) diff = 360.0f - diff;
            float penalty = diff * 0.5f;  /* strong penalty for off-course edges */

            float tent = g->g_score[current] + g->dist_nm[e] + penalty;
            if (tent < g->g_score[nb]) {
                g->came_from[nb] = current;
                g->g_score[nb]   = tent;
                g->f_score[nb]   = tent + heuristic(&g->nodes[nb], &g->nodes[goal]);
                heap_push(g, nb, g->f_score[nb]);
            }
        }
    }

    LOG_WARN("A*: no route %s→%s (explored %d)", origin_icao, dest_icao, explored);
    return 0;
}

void route_path_free(RoutePath* path)
{
    if (path) memset(path, 0, sizeof(*path));
}
