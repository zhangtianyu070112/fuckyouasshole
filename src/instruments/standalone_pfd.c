#include "standalone_pfd.h"
#include "app.h"
#include "instruments/pfd.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    Instrument* pfd_inst;
} StandalonePFD;

static StandalonePFD* g_spfd = NULL;

extern Instrument* pfd_create(void);

int standalone_pfd_init(App* app) {
    g_spfd = calloc(1, sizeof(StandalonePFD));
    if (!g_spfd) return -1;

    g_spfd->window = SDL_CreateWindow("Standalone PFD", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 772, 721, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_spfd->window) { free(g_spfd); return -1; }

    g_spfd->renderer = SDL_CreateRenderer(g_spfd->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_spfd->renderer) g_spfd->renderer = SDL_CreateRenderer(g_spfd->window, -1, SDL_RENDERER_SOFTWARE);

    g_spfd->pfd_inst = pfd_create();
    if (g_spfd->pfd_inst) {
        g_spfd->pfd_inst->rect.x = 0;
        g_spfd->pfd_inst->rect.y = 0;
        g_spfd->pfd_inst->rect.w = 772;
        g_spfd->pfd_inst->rect.h = 721;
        if (g_spfd->pfd_inst->on_init) {
            g_spfd->pfd_inst->on_init(g_spfd->pfd_inst, app);
        }
    }

    return 0;
}

void standalone_pfd_update(const FlightData* fd, float dt) {
    if (!g_spfd || !g_spfd->pfd_inst) return;
    if (g_spfd->pfd_inst->on_update) {
        g_spfd->pfd_inst->on_update(g_spfd->pfd_inst, fd, dt);
    }
}

void standalone_pfd_render(void) {
    if (!g_spfd || !g_spfd->pfd_inst) return;

    SDL_SetRenderDrawColor(g_spfd->renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_spfd->renderer);

    int win_w, win_h;
    SDL_GetWindowSize(g_spfd->window, &win_w, &win_h);
    g_spfd->pfd_inst->rect.x = 0;
    g_spfd->pfd_inst->rect.y = 0;
    g_spfd->pfd_inst->rect.w = win_w;
    g_spfd->pfd_inst->rect.h = win_h;

    if (g_spfd->pfd_inst->on_render) {
        g_spfd->pfd_inst->on_render(g_spfd->pfd_inst, g_spfd->renderer);
    }

    SDL_RenderPresent(g_spfd->renderer);
}

int standalone_pfd_event(const SDL_Event* ev) {
    if (!g_spfd) return 0;

    if (ev->type == SDL_WINDOWEVENT && ev->window.windowID == SDL_GetWindowID(g_spfd->window)) {
        return 1;
    }

    if (g_spfd->pfd_inst && g_spfd->pfd_inst->on_event) {
        if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.windowID == SDL_GetWindowID(g_spfd->window)) {
            g_spfd->pfd_inst->on_event(g_spfd->pfd_inst, ev);
            return 1;
        }
        if (ev->type == SDL_KEYDOWN && ev->key.windowID == SDL_GetWindowID(g_spfd->window)) {
            g_spfd->pfd_inst->on_event(g_spfd->pfd_inst, ev);
            return 1;
        }
    }
    return 0;
}

void standalone_pfd_destroy(void) {
    if (!g_spfd) return;
    if (g_spfd->pfd_inst) {
        if (g_spfd->pfd_inst->on_destroy) {
            g_spfd->pfd_inst->on_destroy(g_spfd->pfd_inst);
        }
        free(g_spfd->pfd_inst);
    }
    if (g_spfd->renderer) SDL_DestroyRenderer(g_spfd->renderer);
    if (g_spfd->window) SDL_DestroyWindow(g_spfd->window);
    free(g_spfd);
    g_spfd = NULL;
}
