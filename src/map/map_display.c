/**
 * @file    map_display.c
 * @brief   Cabin 3D globe map — OpenGL implementation.
 *
 * Renders a textured UV sphere as the earth with the current texture.
 * Draws flight route, GPS track, waypoints, and aircraft icon in 3D.
 * Header, data bar, and progress bar are rendered in 2D orthographic pass.
 */

#include "map_display.h"
#include "weather_fetch.h"
#include "config.h"
#include "utils/logger.h"
#include "utils/math_util.h"

#include <SDL2/SDL_image.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* =========================================================================
 *  Constants
 * ========================================================================= */

#define CABIN_WIN_W         1024
#define CABIN_WIN_H         680
#define CABIN_HEADER_H      32
#define CABIN_DATABAR_H     60
#define CABIN_PROGRESS_H    3

#define CAMERA_DIST_DEFAULT 1.15f
#define CAMERA_DIST_MIN     0.6f
#define CAMERA_DIST_MAX     3.0f
#define GLOBE_TILT_DEFAULT  22.0f
#define GLOBE_ZOOM_CYCLE_MS  22000   /* full cycle: 10s + 1s trans + 10s + 1s trans */
#define GLOBE_ZOOM_HOLD_MS   10000   /* hold duration at each zoom level */
#define GLOBE_ZOOM_TRANS_MS  1000    /* transition duration */
#define GLOBE_ZOOM_SCALE     0.6f    /* target screen scale (smaller = more zoomed out) */

#define SPHERE_LATS         64
#define SPHERE_LONS         128
#define SPHERE_RADIUS       1.0f
#define ROUTE_OFFSET        0.004f     /* route lines slightly above sphere */
#define MARKER_OFFSET       0.008f     /* markers slightly above route */

/* =========================================================================
 *  Helpers
 * ========================================================================= */

static double geo_haversine_nm(double lat1, double lon1,
                                double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * 0.017453292519943295;
    double dlon = (lon2 - lon1) * 0.017453292519943295;
    double a = sin(dlat * 0.5) * sin(dlat * 0.5) +
               cos(lat1 * 0.017453292519943295) * cos(lat2 * 0.017453292519943295) *
               sin(dlon * 0.5) * sin(dlon * 0.5);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return 3440.065 * c;  /* Earth radius in nautical miles */
}

/**
 * @brief Convert lat/lon to 3D point on unit sphere.
 *        Sphere axis: Y = north pole, XZ = equatorial plane.
 *        lon=0 maps to +Z, lon=+90 maps to +X.
 */
static void latlon_to_sphere(double lat, double lon,
                              float* x, float* y, float* z)
{
    double theta = (90.0 - lat) * 0.017453292519943295;  /* 0 at north pole */
    double phi   = lon * 0.017453292519943295;
    *x = (float)(sin(theta) * sin(phi));
    *y = (float)(cos(theta));
    *z = (float)(sin(theta) * cos(phi));
}

/* =========================================================================
 *  OpenGL text rendering — TTF → GL texture → quad
 * ========================================================================= */

/**
 * @brief Render text to a new GL texture, return texture ID.
 *        Caller must glDeleteTextures when done.
 */
static GLuint text_to_texture(TTF_Font* font, const char* text,
                               SDL_Color fg, int* out_w, int* out_h)
{
    if (!font || !text || !text[0]) return 0;

    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text, fg);
    if (!surf) return 0;

    /* Convert to RGBA for OpenGL */
    SDL_Surface* rgba = SDL_CreateRGBSurfaceWithFormat(
        0, surf->w, surf->h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!rgba) { SDL_FreeSurface(surf); return 0; }

    SDL_BlitSurface(surf, NULL, rgba, NULL);
    SDL_FreeSurface(surf);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);

    if (out_w) *out_w = rgba->w;
    if (out_h) *out_h = rgba->h;

    SDL_FreeSurface(rgba);
    return tex;
}

/** Draw a GL texture as a 2D quad at (x,y) with given alignment. */
static void draw_text_quad(GLuint tex, int tex_w, int tex_h,
                            int x, int y, int align)
{
    if (!tex) return;
    int dx = 0;
    if (align == 0) dx = tex_w / 2;        /* center */
    else if (align > 0) dx = tex_w;        /* right */

    glBindTexture(GL_TEXTURE_2D, tex);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2i(x - dx, y);
    glTexCoord2f(1.0f, 0.0f); glVertex2i(x - dx + tex_w, y);
    glTexCoord2f(1.0f, 1.0f); glVertex2i(x - dx + tex_w, y + tex_h);
    glTexCoord2f(0.0f, 1.0f); glVertex2i(x - dx, y + tex_h);
    glEnd();
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
}

/* =========================================================================
 *  Sphere mesh generation
 * ========================================================================= */

static GLuint build_sphere_display_list(int lats, int lons)
{
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);

    for (int lat = 0; lat < lats; lat++) {
        float theta1 = (float)lat       * 3.14159265f / (float)lats;
        float theta2 = (float)(lat + 1) * 3.14159265f / (float)lats;

        glBegin(GL_TRIANGLE_STRIP);
        for (int lon = 0; lon <= lons; lon++) {
            float phi = (float)lon * 2.0f * 3.14159265f / (float)lons;

            float sx1 = sinf(theta1) * sinf(phi);
            float sy1 = cosf(theta1);
            float sz1 = sinf(theta1) * cosf(phi);
            /* U=0.5 at Greenwich (lon=0), wraps via GL_REPEAT */
            float u1  = 0.5f + (float)lon / (float)lons;
            /* V=0 at north pole, V=1 at south pole */
            float v1  = (float)lat / (float)lats;

            float sx2 = sinf(theta2) * sinf(phi);
            float sy2 = cosf(theta2);
            float sz2 = sinf(theta2) * cosf(phi);
            float u2  = 0.5f + (float)lon / (float)lons;
            float v2  = (float)(lat + 1) / (float)lats;

            glTexCoord2f(u1, v1); glNormal3f(sx1, sy1, sz1);
            glVertex3f(sx1, sy1, sz1);

            glTexCoord2f(u2, v2); glNormal3f(sx2, sy2, sz2);
            glVertex3f(sx2, sy2, sz2);
        }
        glEnd();
    }

    glEndList();
    return list;
}

/* =========================================================================
 *  Earth texture loading
 * ========================================================================= */

static GLuint load_earth_texture(const char* path)
{
    SDL_Surface* surf = IMG_Load(path);
    if (!surf) {
        LOG_WARN("Failed to load earth texture: %s — %s", path, IMG_GetError());
        return 0;
    }

    /* Convert to RGB for GL */
    SDL_Surface* rgb = SDL_CreateRGBSurfaceWithFormat(
        0, surf->w, surf->h, 24, SDL_PIXELFORMAT_RGB24);
    if (!rgb) { SDL_FreeSurface(surf); return 0; }
    SDL_BlitSurface(surf, NULL, rgb, NULL);
    SDL_FreeSurface(surf);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, rgb->w, rgb->h,
                      GL_RGB, GL_UNSIGNED_BYTE, rgb->pixels);

    SDL_FreeSurface(rgb);
    LOG_INFO("Earth texture loaded: %dx%d", surf->w, surf->h);
    return tex;
}

/* =========================================================================
 *  Progress calculation (adapted from original, uses lat/lon pairs)
 * ========================================================================= */

static void calc_progress(const FlightDataValues* fd, const FMCState* fmc,
                          double* flown_nm, double* total_nm,
                          double* remain_nm, double* progress_pct)
{
    *flown_nm = *total_nm = *remain_nm = *progress_pct = 0.0;
    if (!fmc || fmc->flight_plan.waypoint_count < 2) return;
    const FlightPlan* fp = &fmc->flight_plan;
    int n = fp->waypoint_count;

    for (int i = 0; i < n - 1; i++)
        *total_nm += geo_haversine_nm(
            fp->waypoints[i].pos.lat_deg, fp->waypoints[i].pos.lon_deg,
            fp->waypoints[i+1].pos.lat_deg, fp->waypoints[i+1].pos.lon_deg);
    if (*total_nm <= 0.0) return;

    int active = fp->active_waypoint_index;
    if (active <= 0) active = 0;
    if (active >= n) active = n - 1;
    for (int i = 0; i < active; i++)
        *flown_nm += geo_haversine_nm(
            fp->waypoints[i].pos.lat_deg, fp->waypoints[i].pos.lon_deg,
            fp->waypoints[i+1].pos.lat_deg, fp->waypoints[i+1].pos.lon_deg);
    if (fd->lat_deg != 0.0 && active < n)
        *flown_nm += geo_haversine_nm(
            fp->waypoints[active].pos.lat_deg, fp->waypoints[active].pos.lon_deg,
            fd->lat_deg, fd->lon_deg);

    *remain_nm = geo_haversine_nm(fd->lat_deg, fd->lon_deg,
        fp->waypoints[n-1].pos.lat_deg, fp->waypoints[n-1].pos.lon_deg);
    *progress_pct = (*total_nm > 0.0) ? (*flown_nm / *total_nm * 100.0) : 0.0;
    if (*progress_pct < 0.0) *progress_pct = 0.0;
    if (*progress_pct > 100.0) *progress_pct = 100.0;
}

static void interpolate_position(MapDisplay* md)
{
    FlightDataValues fd;
    SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);
    if (fd.lat_deg == 0.0 && fd.lon_deg == 0.0) return;
    if (md->disp_lat == 0.0 && md->disp_lon == 0.0) {
        md->disp_lat = fd.lat_deg; md->disp_lon = fd.lon_deg; return;
    }
    md->disp_lat += (fd.lat_deg - md->disp_lat) * 0.35f;
    md->disp_lon += (fd.lon_deg - md->disp_lon) * 0.35f;
}

/* =========================================================================
 *  Weather fetch (background thread, fire-and-forget)
 * ========================================================================= */

typedef struct {
    MapDisplay* md;
    double      dep_lat, dep_lon;
    double      arr_lat, arr_lon;
} WeatherFetchCtx;

static int weather_fetch_thread_func(void* data)
{
    WeatherFetchCtx* ctx = (WeatherFetchCtx*)data;
    MapDisplay* md = ctx->md;

    weather_fetch_for_coords(ctx->dep_lat, ctx->dep_lon,
        md->weather_dep.weather, sizeof(md->weather_dep.weather),
        &md->weather_dep.temp_c, &md->weather_dep.humidity);

    weather_fetch_for_coords(ctx->arr_lat, ctx->arr_lon,
        md->weather_arr.weather, sizeof(md->weather_arr.weather),
        &md->weather_arr.temp_c, &md->weather_arr.humidity);

    md->last_weather_fetch_ms = SDL_GetTicks64();
    SDL_AtomicSet(&md->weather_fetching, 0);
    free(ctx);
    return 0;
}

static void check_weather_fetch(MapDisplay* md)
{
    if (SDL_AtomicGet(&md->weather_fetching)) return;
    if (!md->fmc || md->fmc->flight_plan.waypoint_count < 2) return;

    uint64_t now = SDL_GetTicks64();
    if (md->last_weather_fetch_ms != 0 &&
        now - md->last_weather_fetch_ms < 600000ULL) return;

    const FlightPlan* fp = &md->fmc->flight_plan;
    WeatherFetchCtx* ctx = (WeatherFetchCtx*)calloc(1, sizeof(WeatherFetchCtx));
    if (!ctx) return;
    ctx->md = md;
    ctx->dep_lat = fp->waypoints[0].pos.lat_deg;
    ctx->dep_lon = fp->waypoints[0].pos.lon_deg;
    ctx->arr_lat = fp->waypoints[fp->waypoint_count - 1].pos.lat_deg;
    ctx->arr_lon = fp->waypoints[fp->waypoint_count - 1].pos.lon_deg;

    SDL_AtomicSet(&md->weather_fetching, 1);
    SDL_Thread* th = SDL_CreateThread(weather_fetch_thread_func,
                                      "WeatherFetch", ctx);
    if (!th) {
        SDL_AtomicSet(&md->weather_fetching, 0);
        free(ctx);
    } else {
        SDL_DetachThread(th);
    }
}

/* =========================================================================
 *  Create / Destroy
 * ========================================================================= */

MapDisplay* map_display_create(const Config* cfg, FMCState* fmc)
{
    (void)cfg;  /* unused — weather now uses Open-Meteo (no API key needed) */
    MapDisplay* md = (MapDisplay*)calloc(1, sizeof(MapDisplay));
    if (!md) return NULL;
    md->fmc = fmc;

    /* OpenGL attributes */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    md->window = SDL_CreateWindow("Cabin Moving Map — 3D Globe",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CABIN_WIN_W, CABIN_WIN_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_OPENGL | SDL_WINDOW_ALWAYS_ON_TOP);
    if (!md->window) {
        LOG_ERROR("MapDisplay: SDL_CreateWindow (GL) failed: %s", SDL_GetError());
        free(md); return NULL;
    }

    md->gl_ctx = SDL_GL_CreateContext(md->window);
    if (!md->gl_ctx) {
        LOG_ERROR("MapDisplay: SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(md->window); free(md); return NULL;
    }
    SDL_GL_MakeCurrent(md->window, md->gl_ctx);
    SDL_GL_SetSwapInterval(1);  /* vsync */

    /* OpenGL state */
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glClearColor(0.05f, 0.10f, 0.17f, 1.0f);  /* dark blue bg */

    /* Load earth texture */
    md->earth_tex = load_earth_texture("assets/earth_daymap.jpg");
    if (!md->earth_tex) {
        LOG_WARN("MapDisplay: no earth texture — using wireframe fallback");
    }

    /* Load aircraft sprite */
    {
        SDL_Surface* ps = IMG_Load("assets/assets/plane.png");
        if (ps) {
            md->plane_w = ps->w;
            md->plane_h = ps->h;
            SDL_Surface* rgba = SDL_CreateRGBSurfaceWithFormat(
                0, ps->w, ps->h, 32, SDL_PIXELFORMAT_RGBA32);
            if (rgba) {
                SDL_BlitSurface(ps, NULL, rgba, NULL);
                glGenTextures(1, &md->plane_tex);
                glBindTexture(GL_TEXTURE_2D, md->plane_tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
                SDL_FreeSurface(rgba);
            }
            SDL_FreeSurface(ps);
            LOG_INFO("Plane sprite loaded: %dx%d", md->plane_w, md->plane_h);
        } else {
            md->plane_tex = 0;
            LOG_WARN("Plane sprite not found: assets/assets/plane.png");
        }
    }

    /* Build sphere mesh */
    md->sphere_lats = SPHERE_LATS;
    md->sphere_lons = SPHERE_LONS;
    md->sphere_list = build_sphere_display_list(SPHERE_LATS, SPHERE_LONS);

    /* Camera defaults */
    md->camera_dist = CAMERA_DIST_DEFAULT;
    md->globe_tilt  = GLOBE_TILT_DEFAULT;
    md->globe_rot_y = 0.0f;
    md->target_rot_y = 0.0f;

    /* Load fonts for 2D overlay — Alibaba PuHuiTi supports CJK */
    md->font_small = TTF_OpenFont("resources/fonts/ALIBABAPUHUITI-2-45-LIGHT.TTF", 14);
    md->font_large = TTF_OpenFont("resources/fonts/ALIBABAPUHUITI-2-45-LIGHT.TTF", 20);
    md->font_bold  = TTF_OpenFont("resources/fonts/ALIBABAPUHUITI-2-45-LIGHT.TTF", 22);

    /* Window size */
    SDL_GetWindowSize(md->window, &md->win_w, &md->win_h);

    /* Sync */
    md->data_mutex = SDL_CreateMutex();
    md->zoom_start_ms = SDL_GetTicks64();

    LOG_INFO("MapDisplay 3D Globe: created %dx%d GL window", md->win_w, md->win_h);
    return md;
}

void map_display_destroy(MapDisplay* md)
{
    if (!md) return;
    if (md->gl_ctx) {
        SDL_GL_MakeCurrent(md->window, md->gl_ctx);
        if (md->earth_tex)    glDeleteTextures(1, &md->earth_tex);
        if (md->plane_tex)    glDeleteTextures(1, &md->plane_tex);
        if (md->sphere_list)  glDeleteLists(md->sphere_list, 1);
    }
    if (md->font_small) TTF_CloseFont(md->font_small);
    if (md->font_large) TTF_CloseFont(md->font_large);
    if (md->font_bold)  TTF_CloseFont(md->font_bold);
    if (md->data_mutex) SDL_DestroyMutex(md->data_mutex);
    if (md->gl_ctx)     SDL_GL_DeleteContext(md->gl_ctx);
    if (md->window)     SDL_DestroyWindow(md->window);
    free(md);
    LOG_INFO("MapDisplay 3D Globe: destroyed");
}

/* =========================================================================
 *  Update
 * ========================================================================= */

void map_display_update_position(MapDisplay* md, const FlightDataValues* fd)
{
    if (!md || !fd) return;
    SDL_LockMutex(md->data_mutex);
    md->last_fd = *fd;
    SDL_UnlockMutex(md->data_mutex);
}

/* =========================================================================
 *  3D rendering: globe + routes
 * ========================================================================= */

static void render_globe(MapDisplay* md)
{
    glPushMatrix();

    /* Orient: rotate globe so aircraft longitude faces camera */
    glRotatef(md->globe_tilt, 1.0f, 0.0f, 0.0f);  /* tilt from above */
    glRotatef(md->globe_rot_y, 0.0f, 1.0f, 0.0f);  /* longitude rotation */

    if (md->earth_tex) {
        /* Textured sphere */
        glBindTexture(GL_TEXTURE_2D, md->earth_tex);
        glEnable(GL_TEXTURE_2D);
        glColor3f(1.0f, 1.0f, 1.0f);
        glCallList(md->sphere_list);
        glDisable(GL_TEXTURE_2D);
    } else {
        /* Fallback: wireframe sphere */
        glColor3f(0.2f, 0.4f, 0.6f);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glCallList(md->sphere_list);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    glPopMatrix();
}

/**
 * @brief Draw a small filled circle / disk at a 3D point on the sphere,
 *        oriented toward the camera via a billboard quad.
 */
static void draw_marker(float x, float y, float z,
                         float r, float g, float b, float size)
{
    glPushMatrix();
    glTranslatef(x, y, z);
    /* Billboard: point the quad toward camera by undoing rotation.
     * Simple approach: draw a small GL_POINTS or a tiny sphere-like dot. */
    glPointSize(size);
    glColor3f(r, g, b);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    glPopMatrix();
}

static void render_routes(MapDisplay* md)
{
    glPushMatrix();
    /* Apply same rotation as globe */
    glRotatef(md->globe_tilt, 1.0f, 0.0f, 0.0f);
    glRotatef(md->globe_rot_y, 0.0f, 1.0f, 0.0f);

    const float R = SPHERE_RADIUS + ROUTE_OFFSET;

    /* --- Flown GPS track (solid yellow, thick) --- */
    if (md->track_count >= 2) {
        glColor3f(1.0f, 0.85f, 0.0f);
        glLineWidth(6.0f);
        glBegin(GL_LINE_STRIP);
        for (int i = 0; i < md->track_count; i++) {
            float sx, sy, sz;
            latlon_to_sphere(md->track[i].lat, md->track[i].lon, &sx, &sy, &sz);
            glVertex3f(sx * R, sy * R, sz * R);
        }
        glEnd();
    }

    /* --- FMC planned route (dashed yellow) --- */
    if (md->fmc && md->fmc->flight_plan.waypoint_count >= 2) {
        const FlightPlan* fp = &md->fmc->flight_plan;
        int n = fp->waypoint_count;

        /* Yellow dashed route */
        glColor3f(1.0f, 0.8f, 0.0f);
        glLineWidth(2.0f);
        glEnable(GL_LINE_STIPPLE);
        glLineStipple(1, 0x0F0F);  /* dash pattern */
        glBegin(GL_LINE_STRIP);
        for (int i = 0; i < n; i++) {
            float sx, sy, sz;
            latlon_to_sphere(fp->waypoints[i].pos.lat_deg,
                             fp->waypoints[i].pos.lon_deg,
                             &sx, &sy, &sz);
            glVertex3f(sx * R, sy * R, sz * R);
        }
        glEnd();
        glDisable(GL_LINE_STIPPLE);

        /* Waypoint markers */
        float mr = SPHERE_RADIUS + MARKER_OFFSET;
        for (int i = 0; i < n; i++) {
            float sx, sy, sz;
            latlon_to_sphere(fp->waypoints[i].pos.lat_deg,
                             fp->waypoints[i].pos.lon_deg,
                             &sx, &sy, &sz);
            /* Skip start/end — they get special markers */
            if (i == 0) {
                draw_marker(sx * mr, sy * mr, sz * mr, 0.15f, 0.68f, 0.38f, 6.0f); /* green */
            } else if (i == n - 1) {
                draw_marker(sx * mr, sy * mr, sz * mr, 0.9f, 0.3f, 0.24f, 6.0f);  /* red */
            } else {
                draw_marker(sx * mr, sy * mr, sz * mr, 0.4f, 0.55f, 0.7f, 3.0f);  /* blue-gray */
            }
        }
    }

    glPopMatrix();
}

/**
 * @brief Store the projected screen position and heading for the aircraft.
 *        Called during 3D pass; actual sprite drawn in 2D pass.
 */
static int   ac_screen_x, ac_screen_y;  /* projected screen position */
static float ac_heading;                /* aircraft heading for rotation */
static int   ac_visible;                /* 1 = aircraft is on screen */

static void compute_aircraft_screen_pos(MapDisplay* md)
{
    ac_visible = 0;
    FlightDataValues fd;
    SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);

    if (fd.lat_deg == 0.0 && fd.lon_deg == 0.0) return;

    /* Get 3D position on unit sphere from raw lat/lon */
    float ax, ay, az;
    latlon_to_sphere(md->disp_lat, md->disp_lon, &ax, &ay, &az);
    float R = SPHERE_RADIUS + MARKER_OFFSET;

    /* Apply globe rotations on top of the current modelview (which has
     * camera only — render_globe/render_routes already popped theirs) */
    glPushMatrix();
    glRotatef(md->globe_tilt, 1.0f, 0.0f, 0.0f);
    glRotatef(md->globe_rot_y, 0.0f, 1.0f, 0.0f);

    GLdouble modelview[16], projection[16];
    GLint viewport[4];
    glGetDoublev(GL_MODELVIEW_MATRIX, modelview);
    glGetDoublev(GL_PROJECTION_MATRIX, projection);
    glGetIntegerv(GL_VIEWPORT, viewport);

    GLdouble sx, sy, sz;
    gluProject((GLdouble)(ax * R), (GLdouble)(ay * R), (GLdouble)(az * R),
               modelview, projection, viewport, &sx, &sy, &sz);

    glPopMatrix();

    if (sz > 1.0) return;
    if (sx < -50 || sx > viewport[2] + 50) return;
    if (sy < -50 || sy > viewport[3] + 50) return;

    ac_screen_x = (int)sx;
    ac_screen_y = (int)(md->win_h - sy);
    ac_heading  = fd.heading_true_deg;
    ac_visible  = 1;
}

/** Draw the aircraft sprite during the 2D orthographic pass. */
static void render_aircraft_sprite(MapDisplay* md)
{
    if (!ac_visible || !md->plane_tex) return;

    int cx = ac_screen_x;
    int cy = ac_screen_y;

    /* Scale: sprite size ~32px */
    float scale = 1.0f;
    int dw = (int)((float)md->plane_w * scale);
    int dh = (int)((float)md->plane_h * scale);
    if (dw > 64) { dw = 64; dh = (int)((float)dh * (64.0f / (float)md->plane_w)); }
    if (dh > 64) { dh = 64; dw = (int)((float)dw * (64.0f / (float)md->plane_h)); }

    float hdg_rad = ac_heading * 0.01745329f;
    float c = cosf(hdg_rad);
    float s = sinf(hdg_rad);
    float hw = (float)dw * 0.5f;
    float hh = (float)dh * 0.5f;

    /* 4 corners of the sprite, rotated around center by heading */
    float corners[4][2] = {
        {-hw, -hh}, { hw, -hh}, { hw,  hh}, {-hw,  hh}
    };

    glBindTexture(GL_TEXTURE_2D, md->plane_tex);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBegin(GL_QUADS);
    for (int i = 0; i < 4; i++) {
        float rx = corners[i][0] * c - corners[i][1] * s;
        float ry = corners[i][0] * s + corners[i][1] * c;
        float u = (i == 0 || i == 3) ? 0.0f : 1.0f;
        float v = (i < 2) ? 0.0f : 1.0f;
        glTexCoord2f(u, v);
        glVertex2f((float)cx + rx, (float)cy + ry);
    }
    glEnd();

    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
}

/* =========================================================================
 *  2D overlay rendering (orthographic)
 * ========================================================================= */

static void begin_2d(int win_w, int win_h)
{
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (double)win_w, (double)win_h, 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
}

static void end_2d(void)
{
    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

static void render_header_2d(MapDisplay* md)
{
    int w = md->win_w, hh = CABIN_HEADER_H;

    /* Background bar */
    glColor4f(0.04f, 0.12f, 0.20f, 1.0f);
    glRecti(0, 0, w, hh);

    /* Bottom accent line */
    glColor4f(0.16f, 0.42f, 0.67f, 1.0f);
    glRecti(0, hh - 2, w, hh);

    /* Origin → Dest text */
    char buf[64] = "----  →  ----";
    if (md->fmc && md->fmc->flight_plan.waypoint_count >= 2)
        snprintf(buf, sizeof(buf), "%s  →  %s",
                 md->fmc->flight_plan.departure.icao,
                 md->fmc->flight_plan.arrival.icao);

    SDL_Color white = {255, 255, 255, 255};
    int tw, th;
    GLuint tex = text_to_texture(md->font_bold, buf, white, &tw, &th);
    draw_text_quad(tex, tw, th, w / 2, hh / 2 - th / 2, 0);
    glDeleteTextures(1, &tex);
}

static void render_databar_2d(MapDisplay* md)
{
    FlightDataValues fd;
    SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);

    int w = md->win_w, bar_h = CABIN_DATABAR_H;
    int bar_y = md->win_h - bar_h;

    /* Background */
    glColor4f(0.04f, 0.12f, 0.20f, 0.94f);
    glRecti(0, bar_y, w, md->win_h);

    /* Top line */
    glColor4f(0.16f, 0.42f, 0.67f, 1.0f);
    glRecti(0, bar_y, w, bar_y + 2);

    /* 7 columns of data */
    int col_w = w / 7;
    const char* labels[] = {"GS","ALT","HDG","TAS","OAT","DIST","ETA"};
    char vals[7][32];

    snprintf(vals[0], 32, "%d KTS", (int)fd.gs_kts);
    snprintf(vals[1], 32, "%d FT",  (int)fd.alt_msl_ft);
    snprintf(vals[2], 32, "%d°",   (int)fd.heading_true_deg);
    snprintf(vals[3], 32, "%d KTS", (int)fd.tas_kts);
    if (fd.oat_c > -99.0f) snprintf(vals[4], 32, "%.1f°C", (double)fd.oat_c);
    else snprintf(vals[4], 32, "--°C");

    double flown, total, remain, pct;
    calc_progress(&fd, md->fmc, &flown, &total, &remain, &pct);
    if (remain > 1.0) snprintf(vals[5], 32, "%d NM", (int)remain);
    else snprintf(vals[5], 32, "--- NM");
    if (fd.gs_kts > 30.0f && remain > 1.0) {
        double h = remain / (double)fd.gs_kts;
        snprintf(vals[6], 32, "%02d:%02d", (int)h, (int)((h-(int)h)*60.0));
    } else snprintf(vals[6], 32, "--:--");

    SDL_Color c_label = {138, 180, 216, 255};
    SDL_Color c_value = {255, 255, 255, 255};

    for (int i = 0; i < 7; i++) {
        int cx = col_w * i + col_w / 2;
        int tw, th;
        GLuint tex;

        tex = text_to_texture(md->font_small, labels[i], c_label, &tw, &th);
        draw_text_quad(tex, tw, th, cx, bar_y + 8, 0);
        glDeleteTextures(1, &tex);

        tex = text_to_texture(md->font_large, vals[i], c_value, &tw, &th);
        draw_text_quad(tex, tw, th, cx, bar_y + 34, 0);
        glDeleteTextures(1, &tex);
    }
}

static void render_progress_2d(MapDisplay* md)
{
    FlightDataValues fd;
    SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);

    int w = md->win_w, ph = CABIN_PROGRESS_H;
    int py = md->win_h - CABIN_DATABAR_H - ph;

    double flown, total, remain, pct;
    calc_progress(&fd, md->fmc, &flown, &total, &remain, &pct);

    /* Track background */
    glColor4f(1.0f, 1.0f, 1.0f, 0.08f);
    glRecti(0, py, w, py + ph);

    /* Progress fill */
    int fw = (int)((double)w * pct / 100.0);
    if (fw < 0) fw = 0;
    if (fw > w) fw = w;
    glColor4f(0.10f, 0.45f, 0.91f, 1.0f);
    glRecti(0, py, fw, py + ph);
}

/* =========================================================================
 *  Weather overlay (10s show / 10s hide cycle)
 * ========================================================================= */

#define WEATHER_CYCLE_MS  20000   /* full cycle: 10s show + 10s hide */
#define WEATHER_SHOW_MS   10000   /* visible duration */

/** Return 1 if the weather overlay should be visible now. */
static int weather_overlay_visible(void)
{
    uint64_t now = SDL_GetTicks64();
    return ((now % (uint64_t)WEATHER_CYCLE_MS) < (uint64_t)WEATHER_SHOW_MS) ? 1 : 0;
}

/* =========================================================================
 *  Globe zoom animation — 10s hold → 1s ease → 10s hold → 1s ease → repeat
 * ========================================================================= */

/** Smoothstep easing: 0→0, 1→1, zero derivative at both ends. */
static float smoothstepf(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/**
 * @brief Compute animated camera-distance multiplier for the zoom cycle.
 *
 * Cycle timeline (22 s total):
 *   [0, 10s)  → factor = 1.0        (normal size)
 *   [10s, 11s) → factor = 1 → 1.667 (ease to 60% size)
 *   [11s, 21s) → factor = 1.667     (small size)
 *   [21s, 22s) → factor = 1.667 → 1 (ease back)
 *
 * @return multiplier for camera_dist (>1 = zoomed out).
 */
static float compute_globe_zoom(void)
{
    uint64_t now     = SDL_GetTicks64();
    uint64_t cycle   = (uint64_t)GLOBE_ZOOM_CYCLE_MS;
    uint64_t hold    = (uint64_t)GLOBE_ZOOM_HOLD_MS;
    uint64_t trans   = (uint64_t)GLOBE_ZOOM_TRANS_MS;
    uint64_t phase   = now % cycle;

    /* Screen scale = 1 / camera_distance, so at 0.6x size distance *= 1/0.6 */
    float target = 1.0f / GLOBE_ZOOM_SCALE;   /* ≈ 1.667 */

    if (phase < hold) {
        /* Phase A: normal size */
        return 1.0f;
    } else if (phase < hold + trans) {
        /* Phase B: ease normal → small */
        float t = (float)(phase - hold) / (float)trans;
        return 1.0f + (target - 1.0f) * smoothstepf(t);
    } else if (phase < hold + trans + hold) {
        /* Phase C: small size */
        return target;
    } else {
        /* Phase D: ease small → normal */
        float t = (float)(phase - hold - trans - hold) / (float)trans;
        return target + (1.0f - target) * smoothstepf(t);
    }
}

static void render_weather_overlay_2d(MapDisplay* md)
{
    if (!weather_overlay_visible()) return;

    FlightDataValues fd;
    SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);

    int w = md->win_w, h = md->win_h;
    int map_top = CABIN_HEADER_H;
    int map_h   = h - CABIN_HEADER_H - CABIN_DATABAR_H - CABIN_PROGRESS_H;

    /* Semi-transparent gray overlay on upper ~40% of map area */
    int overlay_h = map_h * 2 / 5;
    if (overlay_h > 280) overlay_h = 280;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.08f, 0.08f, 0.10f, 0.75f);  /* dark semi-transparent */
    glRecti(0, map_top, w, map_top + overlay_h);
    glDisable(GL_BLEND);

    /* Get airport ICAOs */
    const char* dep_icao = "----";
    const char* arr_icao = "----";
    if (md->fmc && md->fmc->flight_plan.waypoint_count >= 2) {
        dep_icao = md->fmc->flight_plan.departure.icao;
        arr_icao = md->fmc->flight_plan.arrival.icao;
    }

    int col_w = w / 2;
    int cx_left  = col_w / 2;
    int cx_right = col_w + col_w / 2;
    int y = map_top + 28;

    /* --- Departure (left column) --- */
    {
        SDL_Color c_label = {138, 180, 216, 255};
        SDL_Color c_white = {255, 255, 255, 255};
        SDL_Color c_gray  = {180, 190, 200, 255};
        int tw, th;
        GLuint tex;

        /* DEPARTURE label */
        tex = text_to_texture(md->font_small, "DEPARTURE", c_label, &tw, &th);
        draw_text_quad(tex, tw, th, cx_left, y, 0); glDeleteTextures(1, &tex);
        y += 26;

        /* ICAO */
        tex = text_to_texture(md->font_large, dep_icao, c_white, &tw, &th);
        draw_text_quad(tex, tw, th, cx_left, y, 0); glDeleteTextures(1, &tex);
        y += 32;

        /* Weather */
        char wx_str[64] = "--";
        if (md->weather_dep.weather[0])
            snprintf(wx_str, sizeof(wx_str), "%s  %.0f°C",
                     md->weather_dep.weather, (double)md->weather_dep.temp_c);
        tex = text_to_texture(md->font_large, wx_str, c_gray, &tw, &th);
        draw_text_quad(tex, tw, th, cx_left, y, 0); glDeleteTextures(1, &tex);
        y += 32;

        /* Local time */
        char dep_time[16] = "--:--";
        {
            time_t now_tt = time(NULL);
            struct tm* tm_info = localtime(&now_tt);
            if (tm_info) snprintf(dep_time, sizeof(dep_time), "%02d:%02d LT",
                                  tm_info->tm_hour, tm_info->tm_min);
        }
        tex = text_to_texture(md->font_large, dep_time, c_white, &tw, &th);
        draw_text_quad(tex, tw, th, cx_left, y, 0); glDeleteTextures(1, &tex);
    }

    /* --- Arrival (right column) --- */
    {
        y = map_top + 28;  /* reset y */
        SDL_Color c_label = {138, 180, 216, 255};
        SDL_Color c_white = {255, 255, 255, 255};
        SDL_Color c_gray  = {180, 190, 200, 255};
        int tw, th;
        GLuint tex;

        /* ARRIVAL label */
        tex = text_to_texture(md->font_small, "ARRIVAL", c_label, &tw, &th);
        draw_text_quad(tex, tw, th, cx_right, y, 0); glDeleteTextures(1, &tex);
        y += 26;

        /* ICAO */
        tex = text_to_texture(md->font_large, arr_icao, c_white, &tw, &th);
        draw_text_quad(tex, tw, th, cx_right, y, 0); glDeleteTextures(1, &tex);
        y += 32;

        /* Weather */
        char wx_str[64] = "--";
        if (md->weather_arr.weather[0])
            snprintf(wx_str, sizeof(wx_str), "%s  %.0f°C",
                     md->weather_arr.weather, (double)md->weather_arr.temp_c);
        tex = text_to_texture(md->font_large, wx_str, c_gray, &tw, &th);
        draw_text_quad(tex, tw, th, cx_right, y, 0); glDeleteTextures(1, &tex);
        y += 32;

        /* ETA */
        char arr_time[16] = "--:--";
        if (fd.gs_kts > 30.0f) {
            double flown, total, remain, pct;
            calc_progress(&fd, md->fmc, &flown, &total, &remain, &pct);
            if (remain > 1.0) {
                double hrs = remain / (double)fd.gs_kts;
                int hh = (int)hrs, mm = (int)((hrs - (double)hh) * 60.0);
                snprintf(arr_time, sizeof(arr_time), "+%02d:%02d", hh, mm);
            }
        }
        tex = text_to_texture(md->font_large, arr_time, c_white, &tw, &th);
        draw_text_quad(tex, tw, th, cx_right, y, 0); glDeleteTextures(1, &tex);
    }
}

/* =========================================================================
 *  Main render entry
 * ========================================================================= */

void map_display_render(MapDisplay* md)
{
    if (!md || !md->gl_ctx) return;

    SDL_GL_MakeCurrent(md->window, md->gl_ctx);

    /* Update window size */
    SDL_GetWindowSize(md->window, &md->win_w, &md->win_h);

    /* Interpolate position */
    interpolate_position(md);

    /* Record GPS track breadcrumbs */
    {
        FlightDataValues fd;
        SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);
        uint64_t now = SDL_GetTicks64();
        int should_add = 0;
        if (md->track_count == 0) {
            should_add = 1;
        } else if (now - md->last_track_add_ms > 2000) {
            double d = geo_haversine_nm(md->track[md->track_count-1].lat,
                                        md->track[md->track_count-1].lon,
                                        md->disp_lat, md->disp_lon);
            if (d > 0.5) should_add = 1;
        }
        if (should_add && fd.lat_deg != 0.0 && fd.lon_deg != 0.0) {
            if (md->track_count < CABIN_TRACK_MAX) {
                md->track[md->track_count].lat = md->disp_lat;
                md->track[md->track_count].lon = md->disp_lon;
                md->track_count++;
                md->last_track_add_ms = now;
            }
        }
    }

    /* Rotate globe so aircraft stays exactly at screen center.
     * RotY(-lon) centers longitude at +Z.
     * Tilt(+lat) centers latitude so aircraft → (0,0,R) on the view ray. */
    md->globe_rot_y = -(float)md->disp_lon;
    md->globe_tilt  =  (float)md->disp_lat;

    /* Check weather fetch */
    check_weather_fetch(md);

    /* Route change detection */
    if (md->fmc && md->fmc->flight_plan.waypoint_count >= 2) {
        const FlightPlan* fp = &md->fmc->flight_plan;
        if (fp->waypoint_count != md->last_wpt_count ||
            strcmp(fp->departure.icao, md->last_dep_icao) != 0 ||
            strcmp(fp->arrival.icao, md->last_arr_icao) != 0) {
            md->last_wpt_count = fp->waypoint_count;
            strncpy(md->last_dep_icao, fp->departure.icao, 7);
            strncpy(md->last_arr_icao, fp->arrival.icao, 7);
            md->last_weather_fetch_ms = 0;
        }
    }

    /* =====================================================================
     *  RENDER 3D
     * ===================================================================== */

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, md->win_w, md->win_h);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    double aspect = (double)md->win_w / (double)md->win_h;
    gluPerspective(45.0, aspect, 0.1, 20.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Camera at eye-level (Y=0) so the view ray passes exactly through
     * (0,0,R) where the aircraft is centered by globe rotations.
     * Zoom factor cycles the camera distance for auto-zoom animation. */
    gluLookAt(0.0, 0.0, (double)md->camera_dist * (double)compute_globe_zoom(),
              0.0, 0.0, 0.0,                       /* target */
              0.0, 1.0, 0.0);                       /* up */

    render_globe(md);
    render_routes(md);
    compute_aircraft_screen_pos(md);

    /* =====================================================================
     *  RENDER 2D
     * ===================================================================== */

    begin_2d(md->win_w, md->win_h);
    render_aircraft_sprite(md);
    render_weather_overlay_2d(md);
    render_header_2d(md);
    render_progress_2d(md);
    render_databar_2d(md);
    end_2d();

    SDL_GL_SwapWindow(md->window);
}
