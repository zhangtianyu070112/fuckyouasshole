/**
 * @file    mock_data.h
 * @brief   Mock flight data generator — simulates a complete flight profile
 *          so instruments can animate without X-Plane 11 connected.
 *
 * Runs in its own thread and periodically overwrites the shared FlightData
 * container with synthetic but realistic values.
 *
 * Usage:
 *   MockDataCtx* mock = mock_data_create(fd, 20);
 *   Thread*      t    = thread_create("MockData", mock_data_thread, mock);
 *   // ... main loop ...
 *   thread_stop(t, 2000); thread_free(t);
 *   mock_data_free(mock);
 */

#ifndef MOCK_DATA_H
#define MOCK_DATA_H

#include "data/flight_data.h"
#include <stdint.h>

/**
 * @brief Context for the mock data generator thread.
 */
typedef struct MockDataCtx {
    FlightData*  flight_data;    /* Shared data container (owned by App) */
    int          update_rate_hz; /* Target updates per second */
    int          running;        /* Set to 0 to stop */
} MockDataCtx;

/* --- Lifecycle ----------------------------------------------------------- */

MockDataCtx* mock_data_create(FlightData* fd, int update_rate_hz);
void         mock_data_free(MockDataCtx* ctx);

/* --- Thread worker (pass to thread_create) ------------------------------- */

/**
 * @brief Thread entry point. Loops generating mock flight data at the
 *        configured rate until ctx->running is set to 0.
 * @param userdata  Must be a MockDataCtx*.
 * @return 0 on clean exit.
 */
int mock_data_thread(void* userdata);

#endif /* MOCK_DATA_H */
