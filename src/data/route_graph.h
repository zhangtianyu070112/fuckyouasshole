/**
 * @file    route_graph.h
 * @brief   Airway route graph + A* shortest-path engine for FMC navigation.
 *
 * Uses chain forward-star (adjacency array) for compact O(1) edge traversal.
 * All arrays are dynamically allocated — no hard node/edge limits.
 * Edges are built via spatial hash proximity queries (O(n) instead of O(n²)).
 */

#ifndef ROUTE_GRAPH_H
#define ROUTE_GRAPH_H

#include "navdata.h"
#include <stdint.h>

/* =========================================================================
 *  Graph node
 * ========================================================================= */

typedef struct RouteNode {
    char         ident[8];          /* Waypoint/airport identifier */
    WaypointType type;              /* Node type */
    GeoPos       pos;               /* Geographic position */
    float        elevation_ft;      /* Station elevation */
} RouteNode;

/* =========================================================================
 *  Route graph (chain forward-star)
 * ========================================================================= */

typedef struct RouteGraph {
    /* Nodes */
    RouteNode*   nodes;
    int          node_count;
    int          node_capacity;

    /* Chain forward-star edges:
     *   head[i] = first edge index for node i, or -1
     *   to[e]   = destination node index
     *   next[e] = next edge index in same chain, or -1
     *   dist[e] = edge distance in NM                              */
    int*         head;
    int*         to;
    int*         next;
    float*       dist_nm;
    int          edge_count;
    int          edge_capacity;

    /* O(1) ident → index lookup (hash table) */
    void*        node_lookup;

    /* A* working state (malloc'd to match node_capacity) */
    float*       g_score;
    float*       f_score;
    int*         came_from;
    int*         closed_set;
    int          visit_token;

    /* Min-heap for open set */
    int*         heap;
    float*       heap_key;
    int*         heap_pos;
    int          heap_size;
} RouteGraph;

/* =========================================================================
 *  Route result
 * ========================================================================= */

#define ROUTE_PATH_MAX_WP 256

typedef struct {
    Waypoint    waypoints[ROUTE_PATH_MAX_WP];
    int         waypoint_count;
    float       total_distance_nm;
    float       estimated_time_hours;
    int         found;              /* 1 = route found */
} RoutePath;

/* =========================================================================
 *  API
 * ========================================================================= */

RouteGraph* route_graph_build(const FMCState* state);
void        route_graph_destroy(RouteGraph* g);
int         route_graph_find_route(RouteGraph* g,
                                   const char* origin_icao,
                                   const char* dest_icao,
                                   float cruise_alt_ft,
                                   RoutePath* path);
void        route_path_free(RoutePath* path);

#endif
