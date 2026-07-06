import re

with open('src/instruments/standalone_fmc.c', 'r', encoding='utf-8') as f:
    code = f.read()

# 1. Includes
code = code.replace('#include "fmc.h"', '#include "standalone_fmc.h"\n#include <SDL2/SDL_image.h>')

# 2. Add global instance
code = code.replace('typedef struct {', '''typedef struct {
    char label[16];
    float bbox[4];
} FMCButtonDef;

typedef struct {''')

# Add window, renderer, tex, json config to FMCData
code = re.sub(r'(typedef struct \{.*?)(} FMCData;)', 
r'''\1
    SDL_Window*   win;
    SDL_Renderer* rend;
    SDL_Texture*  tex;
    float         screen_bbox[4];
    FMCButtonDef  buttons[128];
    int           num_buttons;
\2''', code, flags=re.DOTALL)

# 3. Add JSON parser function
json_parser = '''
static void load_fmc_json(FMCData* d, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        LOG_ERROR("Failed to open %s", path);
        return;
    }
    char line[256];
    d->num_buttons = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "DISPLAY_SCREEN")) {
            fgets(line, sizeof(line), f); // "bbox": [ ... ]
            sscanf(line, "      \\"bbox\\": [%f, %f, %f, %f]", &d->screen_bbox[0], &d->screen_bbox[1], &d->screen_bbox[2], &d->screen_bbox[3]);
        } else if (strstr(line, "\\"label\\":")) {
            char label[32];
            float b0, b1, b2, b3;
            if (sscanf(line, "    {\\"label\\": \\"%[^\\]\\", \\"bbox\\": [%f, %f, %f, %f]}", label, &b0, &b1, &b2, &b3) == 5) {
                strncpy(d->buttons[d->num_buttons].label, label, 15);
                d->buttons[d->num_buttons].bbox[0] = b0;
                d->buttons[d->num_buttons].bbox[1] = b1;
                d->buttons[d->num_buttons].bbox[2] = b2;
                d->buttons[d->num_buttons].bbox[3] = b3;
                d->num_buttons++;
            }
        }
    }
    fclose(f);
    LOG_INFO("Loaded %d buttons from FMC json", d->num_buttons);
}
'''
code = code.replace('/* =========================================================================\n *  Color helper', json_parser + '\n/* =========================================================================\n *  Color helper')

# 4. Rewrite draw_fmc_screen to just draw the texture and then text inside screen_bbox
new_draw_fmc_screen = '''
static void draw_fmc_screen(SDL_Renderer* r, FMCData* d) {
    int w, h;
    SDL_GetWindowSize(d->win, &w, &h);
    
    // Draw texture
    SDL_RenderCopy(r, d->tex, NULL, NULL);
    
    // Calculate screen rect from bbox
    int sx = (int)(d->screen_bbox[0] * w);
    int sy = (int)(d->screen_bbox[1] * h);
    int sw = (int)((d->screen_bbox[2] - d->screen_bbox[0]) * w);
    int sh = (int)((d->screen_bbox[3] - d->screen_bbox[1]) * h);
    
    int line_h = sh / 14; // Slightly more lines to fit text nicely, originally 12 + scratchpad
    int screen_top = sy + 4;
    int screen_left = sx + 4;
    
    // Title bar
    set_col(r, COL_WHITE);
    const char* page_names[] = { "IDENT", "RTE", "LEGS", "PERF", "PROG", "RADIO" };
    char page_title[32];
    snprintf(page_title, sizeof(page_title), "%d/6 %s", d->current_page + 1, page_names[d->current_page]);
    draw_text_simple(r, sx + sw - 40, screen_top + line_h/2, page_title, 0.6f);
    draw_text_simple(r, sx + sw/2, screen_top + line_h/2, "FMC-CDU", 0.7f);

    // Display lines (12 lines)
    for (int ln = 0; ln < 12; ln++) {
        int y = screen_top + (ln + 1) * line_h + line_h / 2;
        set_col(r, COL_WHITE);
        font_draw_scaled_aligned(r, screen_left, y, d->display[ln], 0.55f, FONT_MONO, FONT_ALIGN_LEFT);
    }
    
    // Scratchpad
    int scratch_line_y = screen_top + 13 * line_h;
    char sp_display[32];
    int blink = ((SDL_GetTicks() / 500) % 2);
    snprintf(sp_display, sizeof(sp_display), "%s%c", d->scratchpad, blink ? '_' : ' ');
    set_col(r, COL_CYAN);
    font_draw_scaled_aligned(r, screen_left, scratch_line_y, sp_display, 0.65f, FONT_MONO, FONT_ALIGN_LEFT);
    
    // Message history
    int msg_y = scratch_line_y - line_h;
    int total_msgs = d->message_history ? ll_size(d->message_history) : 0;
    if (total_msgs > 0) {
        int idx = total_msgs - 1 - d->message_scroll;
        if (idx < 0) idx = 0;
        FmcMessage* m = (FmcMessage*)ll_get(d->message_history, idx);
        if (m) {
            int show = (d->message_scroll > 0) || ((SDL_GetTicks() - m->timestamp) < MSG_TIMEOUT_MS);
            if (show) {
                set_col(r, COL_AMBER);
                draw_text_simple(r, sx + sw / 2, msg_y, m->text, 0.6f);
            }
        }
    }
}
'''
code = re.sub(r'static void draw_fmc_screen\(SDL_Renderer\* r, const SDL_Rect\* rect, FMCData\* d\).*?/\* =========================================================================\n \*  Instrument vtable', new_draw_fmc_screen + '\n/* =========================================================================\n *  Instrument vtable', code, flags=re.DOTALL)

# 5. Remove Instrument vtable and replace with standalone functions
new_standalone_funcs = '''
static FMCData* g_fmc = NULL;

int standalone_fmc_init(App* app) {
    if (g_fmc) return 0;
    g_fmc = calloc(1, sizeof(FMCData));
    g_fmc->app = app;
    g_fmc->fmc = app->fmc_state;
    g_fmc->current_page = 0;
    g_fmc->legs_scroll = 0;
    g_fmc->message_scroll = 0;
    memset(g_fmc->scratchpad, 0, sizeof(g_fmc->scratchpad));
    memset(g_fmc->display, ' ', sizeof(g_fmc->display));
    
    g_fmc->message_history = ll_create();
    if (g_fmc->fmc) {
        g_fmc->graph = route_graph_build(g_fmc->fmc);
        if (g_fmc->graph) set_message(g_fmc, "NAV DB LOADED");
    }
    g_fmc->wpt_tree = avl_create(wpt_compare_ident, NULL);
    
    load_fmc_json(g_fmc, "location-FMC.json");
    
    // Create Window
    g_fmc->win = SDL_CreateWindow("Standalone FMC", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 600, 900, SDL_WINDOW_SHOWN);
    if (!g_fmc->win) return -1;
    g_fmc->rend = SDL_CreateRenderer(g_fmc->win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_fmc->rend) return -1;
    
    // Load texture
    SDL_Surface* surf = IMG_Load("assets/fmc.png");
    if (surf) {
        g_fmc->tex = SDL_CreateTextureFromSurface(g_fmc->rend, surf);
        SDL_FreeSurface(surf);
    } else {
        LOG_ERROR("Failed to load fmc.png: %s", IMG_GetError());
    }
    
    build_page(g_fmc);
    return 0;
}

void standalone_fmc_render(void) {
    if (!g_fmc || !g_fmc->win || !g_fmc->rend) return;
    
    // Update logic (was in fmc_on_update)
    FlightDataValues snapshot;
    flight_data_snapshot(g_fmc->app->flight_data, &snapshot);
    g_fmc->smooth_gs  = exp_smooth(g_fmc->smooth_gs,  snapshot.gs_kts, 0.1f);
    g_fmc->smooth_alt = exp_smooth(g_fmc->smooth_alt, snapshot.altitude_ft, 0.1f);
    build_page(g_fmc);
    
    SDL_RenderClear(g_fmc->rend);
    draw_fmc_screen(g_fmc->rend, g_fmc);
    SDL_RenderPresent(g_fmc->rend);
}

static void map_json_label_to_action(FMCData* d, const char* label) {
    if (strncmp(label, "LSK_", 4) == 0) {
        int line = label[4] - '1';
        int side = (label[5] == 'R') ? 1 : 0;
        handle_lsk(d, side, line);
        return;
    }
    
    // Map JSON labels to cdu_button_action labels
    const char* mapped = label;
    if (strcmp(label, "INIT_REF") == 0) mapped = "INIT REF";
    else if (strcmp(label, "DIR_INTC") == 0) mapped = "DIR INTC";
    else if (strcmp(label, "DEP_ARR") == 0) mapped = "DEP ARR";
    else if (strcmp(label, "NAV_RAD") == 0) mapped = "NAV RAD";
    else if (strcmp(label, "PREV_PAGE") == 0) mapped = "PREV PAGE";
    else if (strcmp(label, "NEXT_PAGE") == 0) mapped = "NEXT PAGE";
    else if (strcmp(label, "PLUS_MINUS") == 0) mapped = "+/-";
    else if (strcmp(label, "DOT") == 0) mapped = ".";
    else if (strcmp(label, "SPACE") == 0) mapped = "SP";
    else if (strcmp(label, "SLASH") == 0) mapped = "/";
    else if (strncmp(label, "NUM_", 4) == 0) mapped = &label[4];
    
    cdu_button_action(d, mapped);
}

int standalone_fmc_event(const SDL_Event* ev) {
    if (!g_fmc) return 0;
    
    if (ev->type == SDL_MOUSEBUTTONDOWN) {
        if (ev->button.windowID == SDL_GetWindowID(g_fmc->win)) {
            int w, h;
            SDL_GetWindowSize(g_fmc->win, &w, &h);
            float fx = (float)ev->button.x / w;
            float fy = (float)ev->button.y / h;
            
            for (int i = 0; i < g_fmc->num_buttons; i++) {
                if (fx >= g_fmc->buttons[i].bbox[0] && fx <= g_fmc->buttons[i].bbox[2] &&
                    fy >= g_fmc->buttons[i].bbox[1] && fy <= g_fmc->buttons[i].bbox[3]) {
                    map_json_label_to_action(g_fmc, g_fmc->buttons[i].label);
                    return 1;
                }
            }
            return 1; // Clicked in FMC window but not on a button
        }
    }
    
    if (ev->type == SDL_WINDOWEVENT && ev->window.windowID == SDL_GetWindowID(g_fmc->win)) {
        if (ev->window.event == SDL_WINDOWEVENT_CLOSE) {
            // Hide or ignore
            return 1;
        }
    }
    
    // Keyboard fallback (only if FMC window has focus)
    if (ev->type == SDL_KEYDOWN && ev->key.windowID == SDL_GetWindowID(g_fmc->win)) {
        SDL_Keycode key = ev->key.keysym.sym;
        // Same as original keyboard logic
        if (key == SDLK_F1) { g_fmc->current_page = 0; g_fmc->legs_scroll = 0; return 1; }
        if (key == SDLK_F2) { g_fmc->current_page = 1; g_fmc->legs_scroll = 0; return 1; }
        if (key == SDLK_F3) { g_fmc->current_page = 2; return 1; }
        if (key == SDLK_F4) { g_fmc->current_page = 3; return 1; }
        if (key == SDLK_F5) { g_fmc->current_page = 4; return 1; }
        if (key == SDLK_F7) { g_fmc->current_page = 5; return 1; }
        if (key == SDLK_F6) {
            g_fmc->legs_sort_mode = !g_fmc->legs_sort_mode;
            g_fmc->legs_scroll = 0;
            return 1;
        }
        if (key == SDLK_TAB) { g_fmc->current_page = (g_fmc->current_page + 1) % 6; g_fmc->legs_scroll = 0; return 1; }
        
        int slen = (int)strlen(g_fmc->scratchpad);
        if (key == SDLK_BACKSPACE || key == SDLK_DELETE) {
            if (slen > 0) g_fmc->scratchpad[slen - 1] = '\\0';
            return 1;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            exec_scratchpad(g_fmc);
            return 1;
        }
        if (key == SDLK_ESCAPE) {
            memset(g_fmc->scratchpad, 0, sizeof(g_fmc->scratchpad));
            return 1;
        }
        
        char ch = '\\0';
        if (key >= SDLK_a && key <= SDLK_z)       ch = (char)('A' + (key - SDLK_a));
        else if (key >= SDLK_0 && key <= SDLK_9)   ch = (char)('0' + (key - SDLK_0));
        else if (key == SDLK_SPACE)                ch = ' ';
        else if (key == SDLK_PERIOD)               ch = '.';
        else if (key == SDLK_SLASH)                ch = '/';
        else if (key == SDLK_MINUS)                ch = '-';
        if (ch && slen < MAX_SCRATCHPAD) {
            g_fmc->scratchpad[slen]     = ch;
            g_fmc->scratchpad[slen + 1] = '\\0';
            return 1;
        }
    }
    return 0;
}

void standalone_fmc_destroy(void) {
    if (!g_fmc) return;
    if (g_fmc->graph) route_graph_destroy(g_fmc->graph);
    if (g_fmc->wpt_tree) avl_destroy(g_fmc->wpt_tree, 0);
    if (g_fmc->message_history) ll_destroy(g_fmc->message_history, 1);
    if (g_fmc->tex) SDL_DestroyTexture(g_fmc->tex);
    if (g_fmc->rend) SDL_DestroyRenderer(g_fmc->rend);
    if (g_fmc->win) SDL_DestroyWindow(g_fmc->win);
    free(g_fmc);
    g_fmc = NULL;
}
'''
code = re.sub(r'static void fmc_on_init.*$', new_standalone_funcs, code, flags=re.DOTALL)

# Delete check_lsk_click and check_cdu_click since they are no longer used
code = re.sub(r'static int check_cdu_click.*?return 0;\n}', '', code, flags=re.DOTALL)
code = re.sub(r'static int check_lsk_click.*?return 0;\n}', '', code, flags=re.DOTALL)


with open('src/instruments/standalone_fmc.c', 'w', encoding='utf-8') as f:
    f.write(code)
