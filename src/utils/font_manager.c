/**
 * @file    font_manager.c
 * @brief   TTF font manager — loading, caching, and drawing.
 *
 * Architecture:
 *   1. TTF_Font objects loaded on demand at requested point sizes.
 *   2. Rendered text textures cached in a chained hash table.
 *   3. Cache is flushed when it exceeds MAX_CACHE entries.
 *   4. All drawing is centers text at (x, y) by default.
 */

#include "font_manager.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 *  Constants
 * ========================================================================= */

#define MAX_FONT_SIZES  32      /* pt_size 8..39 supported */
#define SIZE_OFFSET     8       /* index = pt_size - SIZE_OFFSET */
#define CACHE_BUCKETS   512
#define MAX_CACHE       1024    /* Flush entire cache when exceeded */

/* TTF filenames indexed by FontID */
static const char* FONT_FILENAMES[FONT_COUNT] = {
    "B612-Regular.ttf",         /* FONT_REGULAR    */
    "B612-Bold.ttf",            /* FONT_BOLD       */
    "B612Mono-Regular.ttf",     /* FONT_MONO       */
    "B612Mono-Bold.ttf",        /* FONT_MONO_BOLD  */
};

/* =========================================================================
 *  Texture cache entry (linked list for hash chaining)
 * ========================================================================= */

typedef struct CacheEntry {
    struct CacheEntry* next;
    char*   key;            /* "font_id:pt_size:color:text" — owned */
    SDL_Texture* tex;
    int     w, h;
} CacheEntry;

/* =========================================================================
 *  Global font system state
 * ========================================================================= */

static struct {
    char      font_dir[256];              /* Base directory for TTF files   */
    TTF_Font* fonts[FONT_COUNT][MAX_FONT_SIZES]; /* Loaded TTF_Font objects */
    int       font_loaded[FONT_COUNT][MAX_FONT_SIZES];

    CacheEntry* cache[CACHE_BUCKETS];     /* Hash table */
    int         cache_count;              /* Current number of entries      */
    int         cache_flushes;            /* Diagnostic counter             */
} g_font;

/* =========================================================================
 *  Internal: hash & cache
 * ========================================================================= */

/**
 * @brief Build a cache key from rendering parameters.
 * Format: "F:SS:CCCCCCCC:text"  (F=font_id, SS=pt_size, C=RGBA)
 */
static void make_cache_key(char* buf, size_t bufsz,
                           FontID font_id, int pt_size,
                           uint8_t R, uint8_t G, uint8_t B, uint8_t A,
                           const char* text)
{
    uint32_t color = ((uint32_t)R << 24) | ((uint32_t)G << 16)
                   | ((uint32_t)B << 8)  |  (uint32_t)A;
    snprintf(buf, bufsz, "%d:%d:%08X:%s", (int)font_id, pt_size, color, text);
}

/**
 * @brief djb2 hash of a string, modulo CACHE_BUCKETS.
 */
static unsigned hash_str(const char* s)
{
    unsigned h = 5381;
    while (*s) {
        h = ((h << 5) + h) + (unsigned char)*s;
        s++;
    }
    return h % CACHE_BUCKETS;
}

/**
 * @brief Find a cached texture. Returns NULL on miss.
 */
static CacheEntry* cache_find(const char* key)
{
    unsigned bucket = hash_str(key);
    for (CacheEntry* e = g_font.cache[bucket]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            return e;
        }
    }
    return NULL;
}

/**
 * @brief Insert a texture into the cache. Flushes entire cache if full.
 */
static void cache_insert(const char* key, SDL_Texture* tex, int w, int h)
{
    if (g_font.cache_count >= MAX_CACHE) {
        /* Flush entire cache */
        LOG_DEBUG("Font cache flush #%d (%d entries)",
                  g_font.cache_flushes + 1, g_font.cache_count);
        for (int i = 0; i < CACHE_BUCKETS; i++) {
            CacheEntry* e = g_font.cache[i];
            while (e) {
                CacheEntry* next = e->next;
                if (e->tex) SDL_DestroyTexture(e->tex);
                free(e->key);
                free(e);
                e = next;
            }
            g_font.cache[i] = NULL;
        }
        g_font.cache_count = 0;
        g_font.cache_flushes++;
    }

    CacheEntry* entry = calloc(1, sizeof(CacheEntry));
    if (!entry) return;

    entry->key = strdup(key);
    if (!entry->key) { free(entry); return; }

    entry->tex = tex;
    entry->w   = w;
    entry->h   = h;

    unsigned bucket = hash_str(key);
    entry->next = g_font.cache[bucket];
    g_font.cache[bucket] = entry;
    g_font.cache_count++;
}

/* =========================================================================
 *  Internal: font loading
 * ========================================================================= */

/**
 * @brief Get a TTF_Font for the given font_id and pt_size.
 * Loads the font on first access (lazy).
 * @return TTF_Font* or NULL if the file cannot be loaded.
 */
static TTF_Font* get_font(FontID font_id, int pt_size)
{
    /* Clamp size */
    if (pt_size < 8)  pt_size = 8;
    if (pt_size > 39) pt_size = 39;

    int idx = pt_size - SIZE_OFFSET;

    /* Already loaded? */
    if (g_font.font_loaded[font_id][idx]) {
        return g_font.fonts[font_id][idx];
    }

    /* Build path */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s",
             g_font.font_dir, FONT_FILENAMES[font_id]);

    /* Open font at requested size */
    TTF_Font* font = TTF_OpenFont(path, pt_size);
    if (!font) {
        LOG_WARN("Font load failed: %s (size %dpt) — %s",
                 path, pt_size, TTF_GetError());
        g_font.font_loaded[font_id][idx] = 1;  /* Mark as tried (even though NULL) */
        g_font.fonts[font_id][idx] = NULL;
        return NULL;
    }

    g_font.fonts[font_id][idx]          = font;
    g_font.font_loaded[font_id][idx]    = 1;

    LOG_DEBUG("Font loaded: %s @ %dpt", path, pt_size);
    return font;
}

/* =========================================================================
 *  Scale → pt_size conversion
 * ========================================================================= */

static int scale_to_pt(float scale)
{
    /* Mapping matches legacy draw_text_simple scale values:
     *   0.40→9, 0.45→10, 0.50→11, 0.55→12, 0.60→13,
     *   0.65→14, 0.70→15, 0.75→16, 0.80→18, 0.85→19,
     *   0.90→20, 1.00→22
     */
    int pt = (int)(scale * 22.0f);
    if (pt < 8)  pt = 8;
    if (pt > 28) pt = 28;
    return pt;
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

int font_system_init(const char* font_dir)
{
    memset(&g_font, 0, sizeof(g_font));
    strncpy(g_font.font_dir, font_dir, sizeof(g_font.font_dir) - 1);
    g_font.font_dir[sizeof(g_font.font_dir) - 1] = '\0';

    LOG_INFO("Font system initialized (dir: %s)", font_dir);
    return 0;
}

void font_system_shutdown(void)
{
    /* Free cached textures */
    int tex_freed = 0;
    for (int i = 0; i < CACHE_BUCKETS; i++) {
        CacheEntry* e = g_font.cache[i];
        while (e) {
            CacheEntry* next = e->next;
            if (e->tex) { SDL_DestroyTexture(e->tex); tex_freed++; }
            free(e->key);
            free(e);
            e = next;
        }
    }

    /* Free TTF_Font objects */
    int fonts_freed = 0;
    for (int fid = 0; fid < FONT_COUNT; fid++) {
        for (int s = 0; s < MAX_FONT_SIZES; s++) {
            if (g_font.fonts[fid][s]) {
                TTF_CloseFont(g_font.fonts[fid][s]);
                fonts_freed++;
            }
        }
    }

    LOG_INFO("Font system shutdown: %d textures, %d fonts freed",
             tex_freed, fonts_freed);
    memset(&g_font, 0, sizeof(g_font));
}

/* =========================================================================
 *  Core: font_draw_aligned — all other draw functions delegate here
 * ========================================================================= */

void font_draw_aligned(SDL_Renderer* r, int x, int y, const char* text,
                       int pt_size, FontID font_id, FontAlign align)
{
    if (!text || !text[0]) return;

    /* Read current render draw color */
    uint8_t R, G, B, A;
    SDL_GetRenderDrawColor(r, &R, &G, &B, &A);

    /* Skip invisible text */
    if (A == 0) return;

    /* Build cache key (alignment doesn't affect texture, only placement) */
    char key[512];
    make_cache_key(key, sizeof(key), font_id, pt_size, R, G, B, A, text);

    /* Try cache */
    CacheEntry* cached = cache_find(key);
    if (cached) {
        int dx;
        switch (align) {
            case FONT_ALIGN_LEFT:  dx = 0;              break;
            case FONT_ALIGN_RIGHT: dx = -cached->w;     break;
            default:               dx = -cached->w / 2; break;  /* CENTER */
        }
        SDL_Rect dst = { x + dx, y - cached->h / 2,
                         cached->w, cached->h };
        SDL_RenderCopy(r, cached->tex, NULL, &dst);
        return;
    }

    /* Load font (lazy) */
    TTF_Font* font = get_font(font_id, pt_size);
    if (!font) {
        /* Fallback: try regular font */
        if (font_id != FONT_REGULAR) {
            font = get_font(FONT_REGULAR, pt_size);
        }
        if (!font) return;
    }

    /* Render to surface → texture */
    SDL_Color fg = { R, G, B, A };
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text, fg);
    if (!surf) return;

    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    int tw = surf->w;
    int th = surf->h;
    SDL_FreeSurface(surf);

    if (!tex) return;

    /* Draw with alignment */
    int dx;
    switch (align) {
        case FONT_ALIGN_LEFT:  dx = 0;         break;
        case FONT_ALIGN_RIGHT: dx = -tw;       break;
        default:               dx = -tw / 2;   break;  /* CENTER */
    }
    SDL_Rect dst = { x + dx, y - th / 2, tw, th };
    SDL_RenderCopy(r, tex, NULL, &dst);

    /* Cache for reuse */
    cache_insert(key, tex, tw, th);
}

/* --- Convenience wrappers ------------------------------------------------ */

void font_draw(SDL_Renderer* r, int x, int y, const char* text,
               int pt_size, FontID font_id)
{
    font_draw_aligned(r, x, y, text, pt_size, font_id, FONT_ALIGN_CENTER);
}

void font_draw_scaled_aligned(SDL_Renderer* r, int x, int y, const char* text,
                              float scale, FontID font_id, FontAlign align)
{
    font_draw_aligned(r, x, y, text, scale_to_pt(scale), font_id, align);
}

void font_draw_scaled(SDL_Renderer* r, int x, int y, const char* text,
                      float scale, FontID font_id)
{
    font_draw_aligned(r, x, y, text, scale_to_pt(scale), font_id,
                      FONT_ALIGN_CENTER);
}
