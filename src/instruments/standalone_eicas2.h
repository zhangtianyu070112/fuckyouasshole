#ifndef STANDALONE_EICAS2_H
#define STANDALONE_EICAS2_H

#include "app.h"
#include <SDL2/SDL.h>
#include "data/flight_data.h"

int standalone_eicas2_init(App* app);
void standalone_eicas2_update(const FlightData* fd, float dt);
void standalone_eicas2_render(void);
int standalone_eicas2_event(const SDL_Event* ev);
void standalone_eicas2_destroy(void);

#endif /* STANDALONE_EICAS2_H */