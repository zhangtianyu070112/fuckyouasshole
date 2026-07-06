#ifndef CABIN_SERVER_H
#define CABIN_SERVER_H

#include "config.h"
#include "data/flight_data.h"
#include "data/navdata.h"

typedef struct CabinServer CabinServer;

/**
 * @brief Create and start the cabin HTTP server.
 * @param cfg  Application config (reads [cabin] section).
 * @param fmc  FMC state for route data (can be NULL).
 * @return Server handle, or NULL on failure.
 */
CabinServer* cabin_server_create(const Config* cfg, FMCState* fmc);

/**
 * @brief Stop and free the cabin server.
 */
void cabin_server_destroy(CabinServer* server);

/**
 * @brief Update the latest flight data snapshot (call each frame).
 */
void cabin_server_update_position(CabinServer* server, const FlightData* fd);

#endif /* CABIN_SERVER_H */
