/**
 * @file    tile_cache.c
 * @brief   LRU tile cache implementation.
 *
 * Simple fixed-size cache with O(n) LRU eviction. For n ≤ 64, linear scan
 * is fast enough and avoids the complexity of a linked-list-based LRU.
 */

#include "tile_cache.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>

/* =========================================================================
 *  Create / Destroy
 * ========================================================================= */

TileCache* tile_cache_create(SDL_Renderer* renderer)
{
    TileCache* tc = (TileCache*)calloc(1, sizeof(TileCache));
    if (!tc) {
        LOG_ERROR("TileCache: out of memory");
        return NULL;
    }
    tc->renderer = renderer;
    LOG_INFO("TileCache: created (capacity=%d)", TILE_CACHE_MAX);
    return tc;
}

void tile_cache_destroy(TileCache* tc)
{
    if (!tc) return;
    tile_cache_clear(tc);
    free(tc);
    LOG_INFO("TileCache: destroyed");
}

/* =========================================================================
 *  Lookup
 * ========================================================================= */

SDL_Texture* tile_cache_get(TileCache* tc, const char* key)
{
    if (!tc || !key) return NULL;

    for (int i = 0; i < tc->count; i++) {
        if (tc->tiles[i].texture && strcmp(tc->tiles[i].key, key) == 0) {
            tc->tiles[i].last_access = SDL_GetTicks64();
            LOG_DEBUG("TileCache: hit  %s", key);
            return tc->tiles[i].texture;
        }
    }
    LOG_DEBUG("TileCache: miss %s", key);
    return NULL;
}

/* =========================================================================
 *  Insert
 * ========================================================================= */

void tile_cache_put(TileCache* tc, const char* key, SDL_Surface* surf)
{
    if (!tc || !key || !surf) return;

    /* Convert surface to texture */
    SDL_Texture* tex = SDL_CreateTextureFromSurface(tc->renderer, surf);
    if (!tex) {
        LOG_WARN("TileCache: SDL_CreateTextureFromSurface failed: %s",
                 SDL_GetError());
        SDL_FreeSurface(surf);
        return;
    }

    int tex_w = surf->w;
    int tex_h = surf->h;
    SDL_FreeSurface(surf);  /* No longer needed */

    /* Check for existing slot (update-in-place) */
    for (int i = 0; i < tc->count; i++) {
        if (tc->tiles[i].texture && strcmp(tc->tiles[i].key, key) == 0) {
            SDL_DestroyTexture(tc->tiles[i].texture);
            tc->tiles[i].texture     = tex;
            tc->tiles[i].tex_w       = tex_w;
            tc->tiles[i].tex_h       = tex_h;
            tc->tiles[i].last_access = SDL_GetTicks64();
            LOG_DEBUG("TileCache: updated %s", key);
            return;
        }
    }

    /* Find empty slot first, then LRU slot if full */
    int slot = -1;
    if (tc->count < TILE_CACHE_MAX) {
        slot = tc->count;
        tc->count++;
    } else {
        /* Evict LRU */
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < TILE_CACHE_MAX; i++) {
            if (tc->tiles[i].texture && tc->tiles[i].last_access < oldest) {
                oldest = tc->tiles[i].last_access;
                slot = i;
            }
        }
        if (slot >= 0 && tc->tiles[slot].texture) {
            LOG_DEBUG("TileCache: evict %s", tc->tiles[slot].key);
            SDL_DestroyTexture(tc->tiles[slot].texture);
        }
    }

    if (slot >= 0) {
        strncpy(tc->tiles[slot].key, key, sizeof(tc->tiles[slot].key) - 1);
        tc->tiles[slot].key[sizeof(tc->tiles[slot].key) - 1] = '\0';
        tc->tiles[slot].texture     = tex;
        tc->tiles[slot].tex_w       = tex_w;
        tc->tiles[slot].tex_h       = tex_h;
        tc->tiles[slot].last_access = SDL_GetTicks64();
        LOG_DEBUG("TileCache: put   %s (%d×%d)", key, tex_w, tex_h);
    }
}

/* =========================================================================
 *  Clear
 * ========================================================================= */

void tile_cache_clear(TileCache* tc)
{
    if (!tc) return;
    for (int i = 0; i < tc->count; i++) {
        if (tc->tiles[i].texture) {
            SDL_DestroyTexture(tc->tiles[i].texture);
            tc->tiles[i].texture = NULL;
        }
    }
    tc->count = 0;
    LOG_DEBUG("TileCache: cleared");
}
