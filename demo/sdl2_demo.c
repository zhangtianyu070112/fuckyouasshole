/**
 * @file    sdl2_demo.c
 * @brief   SDL2 独立动画演示 — 星空 + 旋转几何 + 粒子拖尾
 *
 * 编译（MSYS2 / MinGW-w64）:
 *   gcc -O2 -o sdl2_demo.exe sdl2_demo.c -lmingw32 -lSDL2main -lSDL2 -lm -mwindows
 *
 * 运行:
 *   ./sdl2_demo.exe
 *
 * 操作:
 *   ESC / 关闭窗口 → 退出
 *   空格           → 切换配色主题
 *   1/2/3/4        → 切换几何图形
 */

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ---------- 常量 ---------- */
#define WINDOW_W    1024
#define WINDOW_H    768
#define FPS_CAP     60
#define FRAME_TICKS (1000 / FPS_CAP)

#define MAX_STARS   300
#define MAX_PARTICLES 200
#define MAX_ORBITERS 6
#define TRAIL_LEN   40

/* ---------- 结构体 ---------- */
typedef struct {
    float x, y;
    float radius;
    float brightness;
    float twinkle_speed;
    float twinkle_phase;
    Uint8  r, g, b;
} Star;

typedef struct {
    float x, y;
    float vx, vy;
    float life;        /* 0..1, 1=刚出生 */
    float decay;
    Uint8  r, g, b, a;
} Particle;

typedef struct {
    float  angle;
    float  angular_speed;
    float  orbit_radius;
    float  cx, cy;
    float  size;
    Uint8  r, g, b;
} Orbiter;

typedef struct {
    float x, y;
} TrailPoint;

/* ---------- 全局状态 ---------- */
static Star       stars[MAX_STARS];
static Particle   particles[MAX_PARTICLES];
static Orbiter    orbiters[MAX_ORBITERS];
static TrailPoint trails[MAX_ORBITERS][TRAIL_LEN];
static int        trail_head[MAX_ORBITERS];

static int  theme_index  = 0;
static int  shape_index  = 0;
static Uint64 start_ticks = 0;

/* =========================================================================
 *  工具函数
 * ========================================================================= */

/* 角度转弧度 */
static float rad(float deg) {
    return deg * 3.14159265359f / 180.0f;
}

/* 限制值在 [lo, hi] */
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 线性插值 */
static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

/* 颜色渐变插值 */
static void lerp_color(Uint8 r1, Uint8 g1, Uint8 b1,
                       Uint8 r2, Uint8 g2, Uint8 b2,
                       float t,
                       Uint8 *out_r, Uint8 *out_g, Uint8 *out_b) {
    *out_r = (Uint8)(r1 + (int)(r2 - r1) * t);
    *out_g = (Uint8)(g1 + (int)(g2 - g1) * t);
    *out_b = (Uint8)(b1 + (int)(b2 - b1) * t);
}

/* =========================================================================
 *  SDL2 绘图封装
 * ========================================================================= */

/* 画填充圆（中点算法 + 水平扫描线） */
static void draw_filled_circle(SDL_Renderer *r, int cx, int cy, int radius) {
    int x = 0;
    int y = radius;
    int d = 1 - radius;
    while (x <= y) {
        SDL_RenderDrawLine(r, cx - x, cy + y, cx + x, cy + y);
        SDL_RenderDrawLine(r, cx - x, cy - y, cx + x, cy - y);
        SDL_RenderDrawLine(r, cx - y, cy + x, cx + y, cy + x);
        SDL_RenderDrawLine(r, cx - y, cy - x, cx + y, cy - x);
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

/* 画空心圆 */
static void draw_circle(SDL_Renderer *r, int cx, int cy, int radius) {
    int x = 0;
    int y = radius;
    int d = 1 - radius;
    while (x <= y) {
        SDL_RenderDrawPoint(r, cx + x, cy + y);
        SDL_RenderDrawPoint(r, cx - x, cy + y);
        SDL_RenderDrawPoint(r, cx + x, cy - y);
        SDL_RenderDrawPoint(r, cx - x, cy - y);
        SDL_RenderDrawPoint(r, cx + y, cy + x);
        SDL_RenderDrawPoint(r, cx - y, cy + x);
        SDL_RenderDrawPoint(r, cx + y, cy - x);
        SDL_RenderDrawPoint(r, cx - y, cy - x);
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

/* 画旋转矩形 */
static void draw_rotated_rect(SDL_Renderer *r, float cx, float cy,
                               float w, float h, float angle_deg) {
    float a = rad(angle_deg);
    float cos_a = cosf(a), sin_a = sinf(a);
    float hw = w * 0.5f, hh = h * 0.5f;

    SDL_Point pts[5];
    float corners[4][2] = {
        {-hw, -hh}, { hw, -hh}, { hw,  hh}, {-hw,  hh}
    };
    for (int i = 0; i < 4; i++) {
        pts[i].x = (int)(cx + corners[i][0] * cos_a - corners[i][1] * sin_a);
        pts[i].y = (int)(cy + corners[i][0] * sin_a + corners[i][1] * cos_a);
    }
    pts[4] = pts[0]; /* 闭合 */
    SDL_RenderDrawLines(r, pts, 5);
}

/* =========================================================================
 *  配色主题
 * ========================================================================= */

/* 共 4 套主题: 霓虹紫 / 深海蓝 / 烈焰橙 / 极光绿 */
typedef struct {
    const char *name;
    Uint8 bg_r, bg_g, bg_b;
    Uint8 star_min_r, star_min_g, star_min_b;   /* 高亮星 */
    Uint8 star_max_r, star_max_g, star_max_b;
    Uint8 geo_r, geo_g, geo_b;                  /* 几何图形基色 */
} Theme;

static const Theme themes[] = {
    {"NEON PURPLE",  10,  5, 25,  200, 100, 255,  255, 180, 255,  200, 80, 255},
    {"OCEAN BLUE",    5,  8, 20,   80, 180, 255,  180, 220, 255,   40, 140, 240},
    {"FIRE ORANGE",  15,  5,  5,  255, 200, 100,  255, 160,  80,  255, 100,  30},
    {"AURORA GREEN",  5, 12,  8,   80, 255, 180,  160, 255, 200,   30, 220, 130},
};

/* =========================================================================
 *  初始化
 * ========================================================================= */

static void init_stars(void) {
    for (int i = 0; i < MAX_STARS; i++) {
        stars[i].x      = (float)(rand() % WINDOW_W);
        stars[i].y      = (float)(rand() % WINDOW_H);
        stars[i].radius = 0.5f + (float)(rand() % 100) / 50.0f;   /* 0.5 ~ 2.5 */
        stars[i].brightness    = (float)(rand() % 100) / 100.0f;
        stars[i].twinkle_speed = 1.0f + (float)(rand() % 100) / 30.0f; /* 1~4.3 Hz */
        stars[i].twinkle_phase = (float)(rand() % 628) / 100.0f;       /* 0~2π */
    }
}

static void init_orbiters(void) {
    for (int i = 0; i < MAX_ORBITERS; i++) {
        orbiters[i].cx           = WINDOW_W / 2.0f;
        orbiters[i].cy           = WINDOW_H / 2.0f;
        orbiters[i].orbit_radius = 80.0f + (float)i * 60.0f;
        orbiters[i].angular_speed = 60.0f - (float)i * 8.0f;  /* deg/s */
        orbiters[i].angle        = (float)i * 60.0f;
        orbiters[i].size         = 15.0f + (float)i * 5.0f;
        trail_head[i] = 0;
        for (int j = 0; j < TRAIL_LEN; j++) {
            trails[i][j].x = -100;
            trails[i][j].y = -100;
        }
    }
}

static void init_particles(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particles[i].life = 0.0f;  /* 死亡状态 */
    }
}

/* =========================================================================
 *  粒子系统
 * ========================================================================= */

static void spawn_particle(float x, float y, const Theme *t) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life <= 0.0f) {
            particles[i].x  = x;
            particles[i].y  = y;
            float angle = rad((float)(rand() % 360));
            float speed = 40.0f + (float)(rand() % 160);
            particles[i].vx = cosf(angle) * speed;
            particles[i].vy = sinf(angle) * speed;
            particles[i].life  = 1.0f;
            particles[i].decay = 0.4f + (float)(rand() % 40) / 100.0f; /* 0.4~0.8 */
            /* 颜色在几何色和白色之间随机 */
            float mix = (float)(rand() % 100) / 100.0f;
            lerp_color(t->geo_r, t->geo_g, t->geo_b,
                       255, 255, 255, mix,
                       &particles[i].r, &particles[i].g, &particles[i].b);
            particles[i].a = 255;
            return;
        }
    }
}

static void update_particles(float dt) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life <= 0.0f) continue;
        particles[i].x  += particles[i].vx * dt;
        particles[i].y  += particles[i].vy * dt;
        particles[i].life -= particles[i].decay * dt;
        particles[i].a = (Uint8)(particles[i].life * 200.0f);
        if (particles[i].life < 0.0f) particles[i].life = 0.0f;
    }
}

/* =========================================================================
 *  绘制
 * ========================================================================= */

/* 星空背景 */
static void draw_stars(SDL_Renderer *r, float time, const Theme *t) {
    for (int i = 0; i < MAX_STARS; i++) {
        float twinkle = sinf(time * stars[i].twinkle_speed + stars[i].twinkle_phase);
        float alpha   = 0.3f + 0.7f * (twinkle * 0.5f + 0.5f) * stars[i].brightness;
        Uint8 sr, sg, sb;
        lerp_color(t->star_min_r, t->star_min_g, t->star_min_b,
                   t->star_max_r, t->star_max_g, t->star_max_b,
                   alpha, &sr, &sg, &sb);

        SDL_SetRenderDrawColor(r, sr, sg, sb, 255);
        int px = (int)stars[i].x;
        int py = (int)stars[i].y;
        /* 大星画小十字 */
        if (stars[i].radius > 1.5f) {
            int rr = (int)stars[i].radius;
            SDL_RenderDrawLine(r, px - rr, py, px + rr, py);
            SDL_RenderDrawLine(r, px, py - rr, px, py + rr);
        } else {
            SDL_RenderDrawPoint(r, px, py);
        }
    }
}

/* 轨迹线 */
static void draw_trails(SDL_Renderer *r, const Theme *t) {
    for (int i = 0; i < MAX_ORBITERS; i++) {
        int head = trail_head[i];
        for (int j = 0; j < TRAIL_LEN; j++) {
            int idx = (head - j + TRAIL_LEN) % TRAIL_LEN;
            float alpha = 1.0f - (float)j / (float)TRAIL_LEN;
            if (alpha < 0.02f) continue;
            Uint8 tr, tg, tb;
            lerp_color(t->bg_r, t->bg_g, t->bg_b,
                       t->geo_r, t->geo_g, t->geo_b,
                       alpha, &tr, &tg, &tb);
            SDL_SetRenderDrawColor(r, tr, tg, tb, (Uint8)(alpha * 120));
            SDL_RenderDrawPoint(r, (int)trails[i][idx].x, (int)trails[i][idx].y);
        }
    }
}

/* 几何图形（4种） */
static void draw_geometry(SDL_Renderer *r, float time, const Theme *t,
                          int shape) {
    SDL_SetRenderDrawColor(r, t->geo_r, t->geo_g, t->geo_b, 255);

    float cx = WINDOW_W / 2.0f, cy = WINDOW_H / 2.0f;

    switch (shape % 4) {
    case 0: { /* 多层旋转嵌套六边形 */
        for (int layer = 5; layer >= 0; layer--) {
            float scale = 50.0f + (float)layer * 45.0f;
            float angle = time * (30.0f + (float)layer * 15.0f) * ((layer & 1) ? -1.0f : 1.0f);
            int sides = 6 + layer;
            Uint8 lr, lg, lb;
            float mix = (float)layer / 5.0f;
            lerp_color(t->geo_r, t->geo_g, t->geo_b,
                       255, 255, 255, mix, &lr, &lg, &lb);
            SDL_SetRenderDrawColor(r, lr, lg, lb, (Uint8)(60 + layer * 30));

            SDL_Point pts[16];
            for (int s = 0; s < sides; s++) {
                float a = rad(angle + (float)s * 360.0f / (float)sides);
                pts[s].x = (int)(cx + cosf(a) * scale);
                pts[s].y = (int)(cy + sinf(a) * scale);
            }
            pts[sides] = pts[0];
            SDL_RenderDrawLines(r, pts, sides + 1);
        }
        break;
    }
    case 1: { /* 旋转嵌套方块 */
        for (int layer = 3; layer >= 0; layer--) {
            float sz  = 80.0f + (float)layer * 70.0f;
            float ang = time * (25.0f + (float)layer * 20.0f) * ((layer & 1) ? 1.0f : -1.0f);
            Uint8 lr, lg, lb;
            lerp_color(t->geo_r, t->geo_g, t->geo_b,
                       t->star_max_r, t->star_max_g, t->star_max_b,
                       (float)layer / 3.0f, &lr, &lg, &lb);
            SDL_SetRenderDrawColor(r, lr, lg, lb, 200);
            draw_rotated_rect(r, cx, cy, sz, sz, ang);
        }
        break;
    }
    case 2: { /* 同心波纹圆 */
        for (int ring = 0; ring < 8; ring++) {
            float base_r = 40.0f + (float)ring * 42.0f;
            float pulse  = sinf(time * 2.0f + (float)ring * 0.8f) * 15.0f;
            float rr     = base_r + pulse;
            Uint8 lr, lg, lb;
            lerp_color(t->geo_r, t->geo_g, t->geo_b,
                       255, 255, 255,
                       (float)ring / 7.0f, &lr, &lg, &lb);
            SDL_SetRenderDrawColor(r, lr, lg, lb, 180);
            draw_circle(r, (int)cx, (int)cy, (int)rr);
        }
        break;
    }
    case 3: { /* 旋转星形射线 */
        int rays = 24;
        for (int ray = 0; ray < rays; ray++) {
            float base_ang = rad((float)ray * 360.0f / (float)rays);
            float angle    = base_ang + time * 1.2f * ((ray & 1) ? 1.0f : -0.7f);
            float len      = 120.0f + sinf(time * 3.0f + (float)ray * 0.5f) * 60.0f;
            float x2 = cx + cosf(angle) * len;
            float y2 = cy + sinf(angle) * len;

            Uint8 lr, lg, lb;
            lerp_color(t->geo_r, t->geo_g, t->geo_b,
                       255, 220, 100,
                       (float)ray / (float)rays, &lr, &lg, &lb);
            SDL_SetRenderDrawColor(r, lr, lg, lb, 200);
            SDL_RenderDrawLine(r, (int)cx, (int)cy, (int)x2, (int)y2);

            /* 射线端点小球 */
            SDL_Rect dot = { (int)x2 - 3, (int)y2 - 3, 6, 6 };
            SDL_RenderFillRect(r, &dot);
        }
        break;
    }
    }
}

/* 环绕小球 */
static void draw_orbiters(SDL_Renderer *r, const Theme *t) {
    for (int i = 0; i < MAX_ORBITERS; i++) {
        Orbiter *o = &orbiters[i];
        float px = o->cx + cosf(rad(o->angle)) * o->orbit_radius;
        float py = o->cy + sinf(rad(o->angle)) * o->orbit_radius;

        /* 球体光晕 */
        SDL_SetRenderDrawColor(r, o->r, o->g, o->b, 60);
        draw_filled_circle(r, (int)px, (int)py, (int)(o->size * 1.8f));
        /* 球体 */
        SDL_SetRenderDrawColor(r, o->r, o->g, o->b, 230);
        draw_filled_circle(r, (int)px, (int)py, (int)o->size);
        /* 高光 */
        SDL_SetRenderDrawColor(r, 255, 255, 255, 150);
        draw_filled_circle(r, (int)(px - o->size * 0.25f),
                              (int)(py - o->size * 0.25f),
                              (int)(o->size * 0.3f));
    }
}

/* 粒子 */
static void draw_particles(SDL_Renderer *r) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life <= 0.0f) continue;
        SDL_SetRenderDrawColor(r, particles[i].r, particles[i].g,
                                  particles[i].b, particles[i].a);
        int px = (int)particles[i].x;
        int py = (int)particles[i].y;
        SDL_RenderDrawPoint(r, px, py);
        /* 小尾迹 */
        SDL_SetRenderDrawColor(r, particles[i].r, particles[i].g,
                                  particles[i].b, particles[i].a / 3);
        SDL_RenderDrawPoint(r, px - 1, py);
        SDL_RenderDrawPoint(r, px, py - 1);
    }
}

/* 标题文字 - 用 SDL 点阵简单画 */
static void draw_title(SDL_Renderer *r, float time) {
    /* 底部脉冲横线 */
    float pulse = sinf(time * 2.0f) * 0.3f + 0.7f;
    int alpha   = (int)(100 + pulse * 155);
    int bar_y   = WINDOW_H - 50;
    int bar_w   = 400;
    int bar_x   = (WINDOW_W - bar_w) / 2;

    SDL_SetRenderDrawColor(r, 255, 255, 255, (Uint8)alpha);
    SDL_Rect bar = { bar_x, bar_y, bar_w, 2 };
    SDL_RenderFillRect(r, &bar);

    /* 装饰小菱形 */
    int cx = WINDOW_W / 2;
    float dia_size = 4.0f + pulse * 3.0f;
    SDL_Point diamond[5] = {
        {cx,              (int)(bar_y - dia_size)},
        {(int)(cx + dia_size), bar_y},
        {cx,              (int)(bar_y + dia_size)},
        {(int)(cx - dia_size), bar_y},
        {cx,              (int)(bar_y - dia_size)},
    };
    SDL_RenderDrawLines(r, diamond, 5);
}

/* =========================================================================
 *  主循环
 * ========================================================================= */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* 随机种子 */
    srand((unsigned)time(NULL));

    /* 初始化 SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow(
        "SDL2 Demo — Flight Cockpit Visual",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *rend = SDL_CreateRenderer(
        win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!rend) {
        fprintf(stderr, "CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    /* 启用混合模式 */
    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);

    /* 初始化场景 */
    init_stars();
    init_orbiters();
    init_particles();

    /* 轨道球颜色 */
    for (int i = 0; i < MAX_ORBITERS; i++) {
        const Theme *t0 = &themes[0];
        float mix = (float)i / (float)(MAX_ORBITERS - 1);
        lerp_color(t0->geo_r, t0->geo_g, t0->geo_b,
                   255, 255, 255, mix,
                   &orbiters[i].r, &orbiters[i].g, &orbiters[i].b);
    }

    start_ticks = SDL_GetTicks64();

    int running  = 1;
    SDL_Event ev;
    Uint64 last_tick = start_ticks;

    printf("=== SDL2 Demo Started ===\n");
    printf("ESC: quit  |  SPACE: theme  |  1-4: shape\n");
    printf("Theme: %s\n", themes[theme_index].name);

    while (running) {
        Uint64 now  = SDL_GetTicks64();
        float  dt   = (float)(now - last_tick) / 1000.0f;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.1f)  dt = 0.1f;   /* 防止大帧跳跃 */
        last_tick = now;

        float total_time = (float)(now - start_ticks) / 1000.0f;

        /* ---- 事件处理 ---- */
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE:
                    running = 0;
                    break;
                case SDLK_SPACE:
                    theme_index = (theme_index + 1) % 4;
                    printf("Theme: %s\n", themes[theme_index].name);
                    break;
                case SDLK_1: shape_index = 0; printf("Shape: Nested Hexagons\n"); break;
                case SDLK_2: shape_index = 1; printf("Shape: Rotating Squares\n"); break;
                case SDLK_3: shape_index = 2; printf("Shape: Pulsing Rings\n");   break;
                case SDLK_4: shape_index = 3; printf("Shape: Star Rays\n");       break;
                default: break;
                }
                break;
            default:
                break;
            }
        }

        const Theme *t = &themes[theme_index];

        /* ---- 更新轨道球 ---- */
        for (int i = 0; i < MAX_ORBITERS; i++) {
            orbiters[i].angle += orbiters[i].angular_speed * dt;
            if (orbiters[i].angle > 360.0f)  orbiters[i].angle -= 360.0f;
            if (orbiters[i].angle < 0.0f)    orbiters[i].angle += 360.0f;

            float px = orbiters[i].cx + cosf(rad(orbiters[i].angle)) * orbiters[i].orbit_radius;
            float py = orbiters[i].cy + sinf(rad(orbiters[i].angle)) * orbiters[i].orbit_radius;
            trails[i][trail_head[i]].x = px;
            trails[i][trail_head[i]].y = py;
            trail_head[i] = (trail_head[i] + 1) % TRAIL_LEN;
        }

        /* ---- 更新粒子 ---- */
        update_particles(dt);
        /* 随机在几何中心附近生成粒子 */
        if (rand() % 3 == 0) {
            float spread = 150.0f;
            float sx = WINDOW_W / 2.0f + (float)(rand() % (int)(spread * 2)) - spread;
            float sy = WINDOW_H / 2.0f + (float)(rand() % (int)(spread * 2)) - spread;
            spawn_particle(sx, sy, t);
        }

        /* ---- 绘制 ---- */
        /* 背景渐变: 顶部暗→中间亮→底部暗 */
        for (int y = 0; y < WINDOW_H; y += 2) {
            float grad = 1.0f - fabsf((float)y / (float)WINDOW_H - 0.5f) * 2.0f; /* 0..1..0 */
            float bright = 0.15f + grad * 0.25f;
            Uint8 gr = (Uint8)((float)t->bg_r * bright);
            Uint8 gg = (Uint8)((float)t->bg_g * bright);
            Uint8 gb = (Uint8)((float)t->bg_b * bright);
            SDL_SetRenderDrawColor(rend, gr, gg, gb, 255);
            SDL_RenderDrawLine(rend, 0, y, WINDOW_W, y);
        }

        draw_stars(rend, total_time, t);
        draw_trails(rend, t);
        draw_geometry(rend, total_time, t, shape_index);
        draw_orbiters(rend, t);
        draw_particles(rend);
        draw_title(rend, total_time);

        SDL_RenderPresent(rend);

        /* 帧率控制（VSync 已开，但做 fallback） */
        Uint64 frame_end = SDL_GetTicks64();
        Sint64 elapsed   = (Sint64)(frame_end - now);
        if (elapsed < FRAME_TICKS) {
            SDL_Delay((Uint32)(FRAME_TICKS - elapsed));
        }
    }

    /* ---- 清理 ---- */
    SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    SDL_Quit();

    printf("=== Demo Ended ===\n");
    return 0;
}
