#ifndef STANDALONE_ND_H
#define STANDALONE_ND_H

#include "app.h"
#include <SDL2/SDL.h>
#include "data/flight_data.h"

/**
 * @brief Initialize the standalone ND panel (creates a new window).
 */
int standalone_nd_init(App* app);

/**
 * @brief Update the standalone ND panel state.
 */
void standalone_nd_update(const FlightData* fd, float dt);

/**
 * @brief Render the standalone ND panel.
 */
void standalone_nd_render(void);

/**
 * @brief Handle events for the standalone ND panel.
 * @return 1 if consumed, 0 otherwise.
 */
int standalone_nd_event(const SDL_Event* ev);

/**
 * @brief Destroy the standalone ND panel and its resources.
 */
void standalone_nd_destroy(void);

#endif /* STANDALONE_ND_H */