#include "standalone_nd.h"
#include "app.h"
#include "instruments/nd.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    Instrument* nd_inst;
} StandaloneND;

static StandaloneND* g_snd = NULL;

extern Instrument* nd_create(void);

int standalone_nd_init(App* app) {
    g_snd = calloc(1, sizeof(StandaloneND));
    if (!g_snd) return -1;

    g_snd->window = SDL_CreateWindow("Standalone ND", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 772, 721, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_snd->window) { free(g_snd); return -1; }

    g_snd->renderer = SDL_CreateRenderer(g_snd->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_snd->renderer) g_snd->renderer = SDL_CreateRenderer(g_snd->window, -1, SDL_RENDERER_SOFTWARE);

    g_snd->nd_inst = nd_create();
    if (g_snd->nd_inst) {
        g_snd->nd_inst->rect.x = 0;
        g_snd->nd_inst->rect.y = 0;
        g_snd->nd_inst->rect.w = 772;
        g_snd->nd_inst->rect.h = 721;
        if (g_snd->nd_inst->on_init) {
            g_snd->nd_inst->on_init(g_snd->nd_inst, app);
        }
    }

    return 0;
}

void standalone_nd_update(const FlightData* fd, float dt) {
    if (!g_snd || !g_snd->nd_inst) return;
    if (g_snd->nd_inst->on_update) {
        g_snd->nd_inst->on_update(g_snd->nd_inst, fd, dt);
    }
}

void standalone_nd_render(void) {
    if (!g_snd || !g_snd->nd_inst) return;

    SDL_SetRenderDrawColor(g_snd->renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_snd->renderer);

    int win_w, win_h;
    SDL_GetWindowSize(g_snd->window, &win_w, &win_h);
    g_snd->nd_inst->rect.x = 0;
    g_snd->nd_inst->rect.y = 0;
    g_snd->nd_inst->rect.w = win_w;
    g_snd->nd_inst->rect.h = win_h;

    if (g_snd->nd_inst->on_render) {
        g_snd->nd_inst->on_render(g_snd->nd_inst, g_snd->renderer);
    }

    SDL_RenderPresent(g_snd->renderer);
}

int standalone_nd_event(const SDL_Event* ev) {
    if (!g_snd) return 0;

    if (ev->type == SDL_WINDOWEVENT && ev->window.windowID == SDL_GetWindowID(g_snd->window)) {
        return 1;
    }

    if (g_snd->nd_inst && g_snd->nd_inst->on_event) {
        if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.windowID == SDL_GetWindowID(g_snd->window)) {
            g_snd->nd_inst->on_event(g_snd->nd_inst, ev);
            return 1;
        }
        if (ev->type == SDL_KEYDOWN && ev->key.windowID == SDL_GetWindowID(g_snd->window)) {
            g_snd->nd_inst->on_event(g_snd->nd_inst, ev);
            return 1;
        }
    }
    return 0;
}

void standalone_nd_destroy(void) {
    if (!g_snd) return;
    if (g_snd->nd_inst) {
        if (g_snd->nd_inst->on_destroy) {
            g_snd->nd_inst->on_destroy(g_snd->nd_inst);
        }
        free(g_snd->nd_inst);
    }
    if (g_snd->renderer) SDL_DestroyRenderer(g_snd->renderer);
    if (g_snd->window) SDL_DestroyWindow(g_snd->window);
    free(g_snd);
    g_snd = NULL;
}