#ifndef STANDALONE_EICAS1_H
#define STANDALONE_EICAS1_H

#include "app.h"
#include <SDL2/SDL.h>
#include "data/flight_data.h"

int standalone_eicas1_init(App* app);
void standalone_eicas1_update(const FlightData* fd, float dt);
void standalone_eicas1_render(void);
int standalone_eicas1_event(const SDL_Event* ev);
void standalone_eicas1_destroy(void);

#endif /* STANDALONE_EICAS1_H */
