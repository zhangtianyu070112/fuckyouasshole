#ifndef STANDALONE_FMC_H
#define STANDALONE_FMC_H

#include "app.h"
#include <SDL2/SDL.h>
#include "data/flight_data.h"

/**
 * @brief Initialize the standalone FMC panel (creates a new window).
 */
int standalone_fmc_init(App* app);

/**
 * @brief Update the standalone FMC panel state.
 */
void standalone_fmc_update(const FlightData* fd, float dt);

/**
 * @brief Render the standalone FMC panel.
 */
void standalone_fmc_render(void);

/**
 * @brief Handle events for the standalone FMC panel (mouse clicks, etc.).
 * @return 1 if consumed, 0 otherwise.
 */
int standalone_fmc_event(const SDL_Event* ev);

/**
 * @brief Destroy the standalone FMC panel and its resources.
 */
void standalone_fmc_destroy(void);

#endif /* STANDALONE_FMC_H */