/**
 * @file    route_graph.h
 * @brief   Airway route graph + A* shortest-path engine for FMC navigation.
 *
 * Architecture:
 *   1. RouteGraph — adjacency-list graph built from nav database waypoints,
 *      airports, and airway segments. Nodes are waypoints/navaids/airports;
 *      edges are airway segments or direct legs with distance + altitude
 *      constraints.
 *   2. A* search — finds the shortest-distance route between any two nodes
 *      using great-circle distance as an admissible heuristic. Returns a
 *      reconstructed waypoint sequence (FlightPlan-compatible).
 *   3. Min-heap priority queue — O(log N) extract-min for the open set.
 *
 * Usage:
 *   RouteGraph* g = route_graph_build(state);
 *   RoutePath   path;
 *   int found = route_graph_find_route(g, "ZBAA", "ZSSS", 35000, &path);
 *   // path.waypoints[0..path.count-1] is the route
 *   route_path_free(&path);
 *   route_graph_destroy(g);
 */

#ifndef ROUTE_GRAPH_H
#define ROUTE_GRAPH_H

#include "navdata.h"
#include <stdint.h>

/* =========================================================================
 *  Graph edge
 * ========================================================================= */

/**
 * @brief One directed edge in the airway graph.
 *        For bidirectional airways, two edges are created (A→B and B→A).
 */
typedef struct RouteEdge {
    int           to_index;         /* Index into graph->nodes[] */
    float         distance_nm;      /* Segment length (nautical miles) */
    char          airway[16];       /* Airway designator, e.g. "A593", or "" */
    int           min_alt_ft;       /* Minimum enroute altitude (0 = none) */
    int           max_alt_ft;       /* Maximum enroute altitude (99999 = none) */
    struct RouteEdge* next;         /* Next edge in adjacency list */
} RouteEdge;

/* =========================================================================
 *  Graph node
 * ========================================================================= */

typedef struct RouteNode {
    char         ident[8];          /* Waypoint/airport identifier */
    WaypointType type;              /* Node type */
    GeoPos       pos;               /* Geographic position */
    float        elevation_ft;      /* Station elevation */
    RouteEdge*   edges;             /* Adjacency list head */
    int          edge_count;        /* Outgoing edge count */
    int          is_portal;         /* 1 = SID exit or STAR entry portal */
    char         portal_airport[8]; /* Associated airport ICAO (portals only) */
} RouteNode;

/* =========================================================================
 *  Route graph
 * ========================================================================= */

/** Maximum nodes in graph (waypoints + airports + navaids) */
#define ROUTE_GRAPH_MAX_NODES 1024

typedef struct RouteGraph {
    RouteNode   nodes[ROUTE_GRAPH_MAX_NODES];
    int         node_count;

    /* O(1) ident → index lookup (hash table) */
    void*       node_lookup;       /* HashTable* — opaque to avoid header deps */

    /* A* working state (reusable across queries to avoid re-allocation) */
    float       g_score[ROUTE_GRAPH_MAX_NODES];   /* Cost from start */
    float       f_score[ROUTE_GRAPH_MAX_NODES];   /* g + heuristic */
    int         came_from[ROUTE_GRAPH_MAX_NODES]; /* Previous node index */
    int         closed_set[ROUTE_GRAPH_MAX_NODES];/* Visited bitmap */
    int         visit_token;                       /* Generational marker */

    /* Min-heap for open set */
    int         heap[ROUTE_GRAPH_MAX_NODES];       /* Heap of node indices */
    float       heap_key[ROUTE_GRAPH_MAX_NODES];   /* f_score per heap entry */
    int         heap_pos[ROUTE_GRAPH_MAX_NODES];   /* Position in heap (-1 = not present) */
    int         heap_size;
} RouteGraph;

/* =========================================================================
 *  Route result
 * ========================================================================= */

#define ROUTE_PATH_MAX_WP 128

typedef struct {
    Waypoint    waypoints[ROUTE_PATH_MAX_WP];
    int         waypoint_count;
    float       total_distance_nm;
    float       estimated_time_hours;
    int         found;              /* 1 = route found, 0 = no path */
} RoutePath;

/* =========================================================================
 *  API
 * ========================================================================= */

/**
 * @brief Build a route graph from FMC navigation database.
 *
 * Nodes = all waypoints + airports + navaids in FMCState.
 * Edges = airway segments (shared airway designator between waypoints) +
 *         direct legs (nearby unconnected nodes) +
 *         airport→nearest-waypoint links (SID/STAR emulation).
 *
 * @param state  FMC state with loaded nav data.
 * @return Populated graph (owned by caller), or NULL on failure.
 */
RouteGraph* route_graph_build(const FMCState* state);

/**
 * @brief Free the route graph and all edge lists.
 */
void route_graph_destroy(RouteGraph* g);

/**
 * @brief A* shortest-path search from origin to destination.
 *
 * Uses great-circle distance to the destination as an admissible heuristic.
 * Respects altitude constraints on airway segments (filters edges where
 * cruise_alt_ft is outside [min_alt, max_alt]).
 *
 * @param g               Route graph.
 * @param origin_icao     Origin airport ICAO (e.g. "ZBAA").
 * @param dest_icao       Destination airport ICAO (e.g. "ZSSS").
 * @param cruise_alt_ft   Planned cruise altitude (for airway filtering).
 * @param path            Output: the found route.
 * @return 1 if a route was found, 0 if no path exists.
 */
int route_graph_find_route(RouteGraph* g,
                           const char* origin_icao,
                           const char* dest_icao,
                           float cruise_alt_ft,
                           RoutePath* path);

/**
 * @brief Free a RoutePath (clears internal state).
 */
void route_path_free(RoutePath* path);

/**
 * @brief Get debug statistics from the last search.
 * @param nodes_explored  Output: nodes visited during search.
 */
void route_graph_stats(RouteGraph* g, int* nodes_explored);

#endif /* ROUTE_GRAPH_H */
