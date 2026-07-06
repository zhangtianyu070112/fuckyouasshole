#include "standalone_eicas1.h"
#include "app.h"
#include "instruments/eicas.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    Instrument* eicas_inst;
} StandaloneEICAS1;

static StandaloneEICAS1* g_se1 = NULL;

extern Instrument* eicas_create(void);

int standalone_eicas1_init(App* app) {
    g_se1 = calloc(1, sizeof(StandaloneEICAS1));
    if (!g_se1) return -1;

    g_se1->window = SDL_CreateWindow("Standalone EICAS 1", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 772, 721, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_se1->window) { free(g_se1); return -1; }

    g_se1->renderer = SDL_CreateRenderer(g_se1->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_se1->renderer) g_se1->renderer = SDL_CreateRenderer(g_se1->window, -1, SDL_RENDERER_SOFTWARE);

    g_se1->eicas_inst = eicas_create();
    if (g_se1->eicas_inst) {
        g_se1->eicas_inst->rect.x = 0;
        g_se1->eicas_inst->rect.y = 0;
        g_se1->eicas_inst->rect.w = 772;
        g_se1->eicas_inst->rect.h = 721;
        if (g_se1->eicas_inst->on_init) {
            g_se1->eicas_inst->on_init(g_se1->eicas_inst, app);
        }
    }

    return 0;
}

void standalone_eicas1_update(const FlightData* fd, float dt) {
    if (!g_se1 || !g_se1->eicas_inst) return;
    if (g_se1->eicas_inst->on_update) {
        g_se1->eicas_inst->on_update(g_se1->eicas_inst, fd, dt);
    }
}

void standalone_eicas1_render(void) {
    if (!g_se1 || !g_se1->eicas_inst) return;

    SDL_SetRenderDrawColor(g_se1->renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_se1->renderer);

    int win_w, win_h;
    SDL_GetWindowSize(g_se1->window, &win_w, &win_h);
    g_se1->eicas_inst->rect.x = 0;
    g_se1->eicas_inst->rect.y = 0;
    g_se1->eicas_inst->rect.w = win_w;
    g_se1->eicas_inst->rect.h = win_h;

    if (g_se1->eicas_inst->on_render) {
        g_se1->eicas_inst->on_render(g_se1->eicas_inst, g_se1->renderer);
    }

    SDL_RenderPresent(g_se1->renderer);
}

int standalone_eicas1_event(const SDL_Event* ev) {
    if (!g_se1) return 0;

    if (ev->type == SDL_WINDOWEVENT && ev->window.windowID == SDL_GetWindowID(g_se1->window)) {
        return 1;
    }

    if (g_se1->eicas_inst && g_se1->eicas_inst->on_event) {
        if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.windowID == SDL_GetWindowID(g_se1->window)) {
            g_se1->eicas_inst->on_event(g_se1->eicas_inst, ev);
            return 1;
        }
        if (ev->type == SDL_KEYDOWN && ev->key.windowID == SDL_GetWindowID(g_se1->window)) {
            g_se1->eicas_inst->on_event(g_se1->eicas_inst, ev);
            return 1;
        }
    }
    return 0;
}

void standalone_eicas1_destroy(void) {
    if (!g_se1) return;
    if (g_se1->eicas_inst) {
        if (g_se1->eicas_inst->on_destroy) {
            g_se1->eicas_inst->on_destroy(g_se1->eicas_inst);
        }
        free(g_se1->eicas_inst);
    }
    if (g_se1->renderer) SDL_DestroyRenderer(g_se1->renderer);
    if (g_se1->window) SDL_DestroyWindow(g_se1->window);
    free(g_se1);
    g_se1 = NULL;
}
