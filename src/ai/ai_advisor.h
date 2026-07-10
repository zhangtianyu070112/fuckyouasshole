/**
 * @file    ai_advisor.h
 * @brief   AI Co-pilot Advisor — WebSocket client for remote inference.
 *
 * Maintains a persistent WebSocket connection to the inference server
 * (workstation on LAN). Sends flight state when it changes significantly;
 * receives token-by-token streaming advisory text.
 *
 * Usage:
 *   AIAdvisor* ai = ai_advisor_create(config);
 *   // each frame:
 *   ai_advisor_update(ai, &snapshot);
 *   const char* text = ai_advisor_get_advisory(ai);
 *   // on shutdown:
 *   ai_advisor_destroy(ai);
 *
 * All public functions are thread-safe. The update call is non-blocking —
 * designed to be called from the main render loop at 60 Hz.
 */

#ifndef AI_ADVISOR_H
#define AI_ADVISOR_H

#include <stdint.h>

typedef struct FlightDataValues FlightDataValues;
typedef struct Config          Config;

/** Opaque handle to the AI advisor client. */
typedef struct AIAdvisor AIAdvisor;

/**
 * @brief Create and begin connecting (non-blocking).
 * @param config  Parsed config — reads [ai] section.
 * @return New AIAdvisor, or NULL if disabled or allocation failed.
 */
AIAdvisor* ai_advisor_create(const Config* config);

/**
 * @brief Non-blocking per-frame update.
 *
 * - Sends flight state JSON if key parameters changed enough and
 *   push_interval_ms has elapsed.
 * - Reads available WebSocket frames, assembles streaming tokens
 *   into the internal advisory buffer.
 * - Handles reconnection with exponential backoff.
 *
 * @param ai  Advisor handle.
 * @param fd  Latest flight data snapshot.
 */
void ai_advisor_update(AIAdvisor* ai, const FlightDataValues* fd);

/**
 * @brief Get the latest complete advisory text.
 * @return NUL-terminated text, or NULL if no advice yet.
 *         Pointer is valid until the next ai_advisor_update() call.
 */
const char* ai_advisor_get_advisory(const AIAdvisor* ai);

/**
 * @brief Check connection status.
 * @return 1 if WebSocket is connected, 0 otherwise.
 */
int ai_advisor_is_connected(const AIAdvisor* ai);

/**
 * @brief Shutdown, close socket, free memory.
 */
void ai_advisor_destroy(AIAdvisor* ai);

#endif /* AI_ADVISOR_H */
