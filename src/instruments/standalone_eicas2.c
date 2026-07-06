#include "standalone_eicas2.h"
#include "app.h"
#include "instruments/eicas2.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    Instrument* eicas2_inst;
} StandaloneEICAS2;

static StandaloneEICAS2* g_se2 = NULL;

extern Instrument* eicas2_create(void);

int standalone_eicas2_init(App* app) {
    g_se2 = calloc(1, sizeof(StandaloneEICAS2));
    if (!g_se2) return -1;

    g_se2->window = SDL_CreateWindow("Standalone EICAS 2", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 772, 721, SDL_WINDOW_SHOWN);
    if (!g_se2->window) { free(g_se2); return -1; }

    g_se2->renderer = SDL_CreateRenderer(g_se2->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_se2->renderer) g_se2->renderer = SDL_CreateRenderer(g_se2->window, -1, SDL_RENDERER_SOFTWARE);

    g_se2->eicas2_inst = eicas2_create();
    if (g_se2->eicas2_inst) {
        g_se2->eicas2_inst->rect.x = 0;
        g_se2->eicas2_inst->rect.y = 0;
        g_se2->eicas2_inst->rect.w = 772;
        g_se2->eicas2_inst->rect.h = 721;
        if (g_se2->eicas2_inst->on_init) {
            g_se2->eicas2_inst->on_init(g_se2->eicas2_inst, app);
        }
    }

    return 0;
}

void standalone_eicas2_update(const FlightData* fd, float dt) {
    if (!g_se2 || !g_se2->eicas2_inst) return;
    if (g_se2->eicas2_inst->on_update) {
        g_se2->eicas2_inst->on_update(g_se2->eicas2_inst, fd, dt);
    }
}

void standalone_eicas2_render(void) {
    if (!g_se2 || !g_se2->eicas2_inst) return;

    SDL_SetRenderDrawColor(g_se2->renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_se2->renderer);

    int win_w, win_h;
    SDL_GetWindowSize(g_se2->window, &win_w, &win_h);
    g_se2->eicas2_inst->rect.x = 0;
    g_se2->eicas2_inst->rect.y = 0;
    g_se2->eicas2_inst->rect.w = win_w;
    g_se2->eicas2_inst->rect.h = win_h;

    if (g_se2->eicas2_inst->on_render) {
        g_se2->eicas2_inst->on_render(g_se2->eicas2_inst, g_se2->renderer);
    }

    SDL_RenderPresent(g_se2->renderer);
}

int standalone_eicas2_event(const SDL_Event* ev) {
    if (!g_se2) return 0;

    if (ev->type == SDL_WINDOWEVENT && ev->window.windowID == SDL_GetWindowID(g_se2->window)) {
        return 1;
    }

    if (g_se2->eicas2_inst && g_se2->eicas2_inst->on_event) {
        if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.windowID == SDL_GetWindowID(g_se2->window)) {
            g_se2->eicas2_inst->on_event(g_se2->eicas2_inst, ev);
            return 1;
        }
        if (ev->type == SDL_KEYDOWN && ev->key.windowID == SDL_GetWindowID(g_se2->window)) {
            g_se2->eicas2_inst->on_event(g_se2->eicas2_inst, ev);
            return 1;
        }
    }
    return 0;
}

void standalone_eicas2_destroy(void) {
    if (!g_se2) return;
    if (g_se2->eicas2_inst) {
        if (g_se2->eicas2_inst->on_destroy) {
            g_se2->eicas2_inst->on_destroy(g_se2->eicas2_inst);
        }
        free(g_se2->eicas2_inst);
    }
    if (g_se2->renderer) SDL_DestroyRenderer(g_se2->renderer);
    if (g_se2->window) SDL_DestroyWindow(g_se2->window);
    free(g_se2);
    g_se2 = NULL;
}