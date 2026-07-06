#ifndef STANDALONE_PFD_H
#define STANDALONE_PFD_H

#include "app.h"
#include <SDL2/SDL.h>
#include "data/flight_data.h"

/**
 * @brief Initialize the standalone PFD panel (creates a new window).
 */
int standalone_pfd_init(App* app);

/**
 * @brief Update the standalone PFD panel state.
 */
void standalone_pfd_update(const FlightData* fd, float dt);

/**
 * @brief Render the standalone PFD panel.
 */
void standalone_pfd_render(void);

/**
 * @brief Handle events for the standalone PFD panel.
 * @return 1 if consumed, 0 otherwise.
 */
int standalone_pfd_event(const SDL_Event* ev);

/**
 * @brief Destroy the standalone PFD panel and its resources.
 */
void standalone_pfd_destroy(void);

#endif /* STANDALONE_PFD_H */
