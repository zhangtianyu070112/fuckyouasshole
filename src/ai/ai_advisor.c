/**
 * @file    ai_advisor.c
 * @brief   AI Co-pilot Advisor — WebSocket client implementation.
 *
 * Connects to the inference server (Python, workstation LAN) over a
 * persistent WebSocket (RFC 6455).  Pushes flight-state JSON when
 * parameters change; receives token-by-token advisory text stream.
 *
 * Protocol: see docs/开发机连接与通信协议.md
 *
 * Design:
 *   - Non-blocking socket — safe to call from main render loop at 60 Hz.
 *   - Minimal WebSocket (RFC 6455) — text frames, masking, ping/pong.
 *   - No third-party dependencies beyond Winsock2 (already linked).
 *   - Graceful degradation — returns NULL advisory when disconnected.
 */

#include "ai_advisor.h"
#include "data/flight_data.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* --- Platform: Windows (MinGW-w64) ------------------------------------- */
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
  #define SOCK_INVALID  INVALID_SOCKET
  #define SOCK_ERR      SOCKET_ERROR
  #define sock_close(s) closesocket(s)
  #define sock_last_err() WSAGetLastError()
  #define WOULD_BLOCK    WSAEWOULDBLOCK
  #define IN_PROGRESS    WSAEWOULDBLOCK
  #define IS_CONNECTING(err) ((err) == WSAEWOULDBLOCK || (err) == WSAEALREADY)
  static int winsock_refcount = 0;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int sock_t;
  #define SOCK_INVALID  (-1)
  #define SOCK_ERR      (-1)
  #define sock_close(s) close(s)
  #define sock_last_err() errno
  #define WOULD_BLOCK    EWOULDBLOCK
  #define IN_PROGRESS    EINPROGRESS
  #define IS_CONNECTING(err) ((err) == EINPROGRESS || (err) == EALREADY)
#endif

#include <SDL2/SDL.h>

/* =========================================================================
 *  Constants
 * ========================================================================= */

#define WS_GUID            "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define RECV_BUF_SIZE      4096
#define ADVISORY_MAX       512
#define HOST_MAX           64
#define WS_KEY_LEN         24

/* Reconnection backoff (ms) */
#define RECONNECT_BASE_MS   2000
#define RECONNECT_MAX_MS    30000

/* Push timing (ms) — per protocol §4.3 */
#define MIN_PUSH_INTERVAL_MS   200
#define MAX_SILENCE_MS         5000

/* Advisory timeout — mark stale if no {"done":true} within this (ms) */
#define ADVISORY_TIMEOUT_MS    2000

/* Delta thresholds — push if any continuous field changes by more than this */
#define DELTA_ALT_FT        50.0f
#define DELTA_AGL_FT        50.0f
#define DELTA_IAS_KTS       5.0f
#define DELTA_VS_FPM        100.0f
#define DELTA_MACH          0.01f
#define DELTA_HDG_DEG       3.0f
#define DELTA_ROLL_DEG      2.0f
#define DELTA_PITCH_DEG     1.0f
#define DELTA_N1_PCT        2.0f
#define DELTA_EGT_C         10.0f
#define DELTA_FLAP          0.05f
#define DELTA_FUEL_LBS      200.0f
#define DELTA_OAT_C         5.0f

/* =========================================================================
 *  Internal helpers — Base64
 * ========================================================================= */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const unsigned char* src, int len, char* dst)
{
    int i, j = 0;
    for (i = 0; i < len; i += 3) {
        unsigned int val = (unsigned int)(src[i]) << 16;
        if (i + 1 < len) val |= (unsigned int)(src[i + 1]) << 8;
        if (i + 2 < len) val |= (unsigned int)(src[i + 2]);
        dst[j++] = b64_table[(val >> 18) & 0x3F];
        dst[j++] = b64_table[(val >> 12) & 0x3F];
        dst[j++] = (i + 1 < len) ? b64_table[(val >> 6) & 0x3F] : '=';
        dst[j++] = (i + 2 < len) ? b64_table[val & 0x3F] : '=';
    }
    dst[j] = '\0';
}

/* =========================================================================
 *  Internal helpers — Winsock init / cleanup
 * ========================================================================= */

static int winsock_init(void)
{
#ifdef _WIN32
    if (winsock_refcount == 0) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    }
    winsock_refcount++;
#endif
    return 0;
}

static void winsock_cleanup(void)
{
#ifdef _WIN32
    winsock_refcount--;
    if (winsock_refcount <= 0) {
        WSACleanup();
        winsock_refcount = 0;
    }
#endif
}

/* =========================================================================
 *  Internal helpers — non-blocking socket
 * ========================================================================= */

static int sock_set_nonblock(sock_t s)
{
#ifdef _WIN32
    unsigned long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* =========================================================================
 *  AIAdvisor struct
 * ========================================================================= */

struct AIAdvisor {
    sock_t      sock;
    char        host[HOST_MAX];
    int         port;
    int         enabled;

    /* Connection state */
    int         ws_connected;        /* WebSocket handshake complete */
    int         connecting;          /* TCP connect in progress */
    int         handshake_sent;      /* HTTP upgrade request sent */
    uint64_t    last_connect_attempt;
    int         reconnect_backoff_ms;

    /* Send throttling / delta detection */
    uint64_t        last_push_ticks;
    FlightDataValues last_sent;

    /* Receive buffer (raw bytes) */
    char        recv_buf[RECV_BUF_SIZE];
    int         recv_len;

    /* In-flight advisory buffer (tokens accumulate between pushes) */
    char        pending[ADVISORY_MAX];
    int         pending_len;

    /* Committed advisory — only updated on {"done":true} */
    char        committed[ADVISORY_MAX];
    int         committed_len;
    int         committed_stale;     /* 1 if timed out waiting for done */
    uint64_t    last_done_ticks;     /* When we last got a done marker */

    /* Inference gate — prevent pushing new state while server is busy */
    int         inferring;           /* 1 = waiting for server response */
    int         has_pending_push;    /* Discrete event happened during inference */
    FlightDataValues pending_push_fd;/* State to push after current inference done */
};

/* =========================================================================
 *  WebSocket frame helpers
 * ========================================================================= */

static void ws_gen_mask_key(unsigned char key[4])
{
    /* Mix SDL ticks, payload address (ASLR), and a counter for entropy */
    static uint32_t counter = 0;
    uint32_t seed = SDL_GetTicks() ^ (uint32_t)(uintptr_t)&counter;
    counter++;
    /* SplitMix32 */
    seed = (seed ^ (seed >> 15)) * 0x85EBCA77U;
    seed = (seed ^ (seed >> 13)) * 0xC2B2AE35U;
    seed = seed ^ (seed >> 16);
    /* Ensure no zero bytes — server rejects all-zero mask */
    key[0] = (unsigned char)((seed & 0xFF)       | 1);
    key[1] = (unsigned char)(((seed >> 8) & 0xFF) | 1);
    key[2] = (unsigned char)(((seed >> 16) & 0xFF)| 1);
    key[3] = (unsigned char)(((seed >> 24) & 0xFF)| 1);
}

static int ws_build_text_frame(const char* payload, int payload_len,
                               unsigned char* out)
{
    int pos = 0;
    out[pos++] = 0x81;  /* FIN=1, opcode=text */

    if (payload_len < 126) {
        out[pos++] = (unsigned char)(0x80 | payload_len);
    } else if (payload_len < 65536) {
        out[pos++] = 0x80 | 126;
        out[pos++] = (unsigned char)((payload_len >> 8) & 0xFF);
        out[pos++] = (unsigned char)(payload_len & 0xFF);
    } else {
        out[pos++] = 0x80 | 127;
        memset(out + pos, 0, 4);
        pos += 4;
        out[pos++] = (unsigned char)((payload_len >> 24) & 0xFF);
        out[pos++] = (unsigned char)((payload_len >> 16) & 0xFF);
        out[pos++] = (unsigned char)((payload_len >> 8) & 0xFF);
        out[pos++] = (unsigned char)(payload_len & 0xFF);
    }

    unsigned char mask[4];
    ws_gen_mask_key(mask);
    memcpy(out + pos, mask, 4);
    pos += 4;

    for (int i = 0; i < payload_len; i++) {
        out[pos + i] = (unsigned char)(payload[i]) ^ mask[i % 4];
    }
    pos += payload_len;
    return pos;
}

static int ws_parse_frame(const unsigned char* buf, int buf_len,
                          const unsigned char** payload, int* payload_len,
                          int* opcode, int* consumed)
{
    if (buf_len < 2) return 0;

    int fin  = (buf[0] >> 7) & 1;
    *opcode  = buf[0] & 0x0F;
    int masked = (buf[1] >> 7) & 1;
    int len   = buf[1] & 0x7F;
    int pos   = 2;

    if (len == 126) {
        if (buf_len < 4) return 0;
        len = ((int)buf[2] << 8) | (int)buf[3];
        pos = 4;
    } else if (len == 127) {
        if (buf_len < 10) return 0;
        len = ((int)buf[6] << 24) | ((int)buf[7] << 16)
            | ((int)buf[8] << 8)  | (int)buf[9];
        pos = 10;
    }

    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (buf_len < pos + 4) return 0;
        memcpy(mask, buf + pos, 4);
        pos += 4;
    }

    if (buf_len < pos + len) return 0;
    *payload     = buf + pos;
    *payload_len = len;
    *consumed    = pos + len;

    if (masked) {
        unsigned char* p = (unsigned char*)(*payload);
        for (int i = 0; i < len; i++) {
            p[i] ^= mask[i % 4];
        }
    }

    (void)fin;
    return 1;
}

/* =========================================================================
 *  State serialization — 17-field JSON per protocol §3.2
 * ========================================================================= */

/**
 * @brief Compute alert bits from raw flight parameters (fallback when
 *        DREF RREF data is unavailable or stale).
 *        Uses the same thresholds as alert_system.c / eval_alerts in eicas.c.
 */
static void compute_alerts(const FlightDataValues* f,
                           int* stall, int* gpws, int* windshear,
                           int* overspeed, int* bank_angle,
                           int* gear_warn, int* engine_fire)
{
    float agl  = f->altitude_agl_ft;
    float vs   = f->vs_fpm;
    float ias  = f->ias_kts;
    float roll = fabsf(f->roll_deg);
    float n1_0 = f->n1_pct[0], n1_1 = f->n1_pct[1];
    float egt_0 = f->egt_c[0], egt_1 = f->egt_c[1];

    /* Stall: IAS < 110 kt + descending, OR IAS < 60 kt at any VS */
    *stall = ((ias < 110.0f && ias > 0.0f && vs < -50.0f)
              || (ias < 60.0f && ias > 0.0f)) ? 1 : 0;

    /* GPWS / terrain: AGL < 1000ft and sinking fast */
    *gpws = (agl < 1000.0f && agl > 0.0f && vs < -1500.0f) ? 1 : 0;

    /* Windshear: rapid IAS change (DREF only) */
    *windshear = f->dref_windshear;

    /* Overspeed: IAS > 340 kt */
    *overspeed = (ias > 340.0f) ? 1 : 0;

    /* Bank angle: |roll| > 45° at ANY altitude (spin / upset) */
    *bank_angle = (roll > 45.0f) ? 1 : 0;

    /* Gear warning: gear up below 500ft, sinking */
    *gear_warn = (!f->gear_deployed && agl < 500.0f && agl > 10.0f
                  && vs < -50.0f) ? 1 : 0;

    /* Engine fire: EGT > 950°C or N1 > 105% */
    *engine_fire = (egt_0 > 950.0f || egt_1 > 950.0f
                    || n1_0 > 105.0f || n1_1 > 105.0f) ? 1 : 0;
}

/**
 * @brief Serialize flight state to the JSON format the server expects.
 */
static int serialize_state_json(const FlightDataValues* f,
                                char* buf, int buf_size)
{
    /* Compute alerts from raw params (DREF may be stale with mock data) */
    int stall, gpws, windshear, overspeed, bank_angle, gear_warn, engine_fire;
    compute_alerts(f, &stall, &gpws, &windshear, &overspeed,
                   &bank_angle, &gear_warn, &engine_fire);

    return snprintf(buf, (size_t)buf_size,
        "{"
        "\"alt_ft\":%.0f,\"agl_ft\":%.0f,"
        "\"ias_kts\":%.0f,\"vs_fpm\":%.0f,"
        "\"mach\":%.3f,"
        "\"roll_deg\":%.1f,\"pitch_deg\":%.1f,"
        "\"heading\":%.0f,"
        "\"n1_pct\":[%.1f,%.1f],"
        "\"egt_c\":[%.0f,%.0f],"
        "\"flap_ratio\":%.2f,\"gear_down\":%d,"
        "\"fuel_lbs\":%.0f,\"oat_c\":%.0f,"
        "\"ap_engaged\":%d,"
        "\"master_warning\":%d,\"master_caution\":%d,"
        "\"stall\":%d,\"gpws\":%d,\"windshear\":%d,"
        "\"overspeed\":%d,\"bank_angle\":%d,"
        "\"gear_warn\":%d,\"engine_fire\":%d"
        "}",
        (double)f->altitude_ft, (double)f->altitude_agl_ft,
        (double)f->ias_kts, (double)f->vs_fpm,
        (double)f->mach,
        (double)f->roll_deg, (double)f->pitch_deg,
        (double)f->heading_mag_deg,
        (double)f->n1_pct[0], (double)f->n1_pct[1],
        (double)f->egt_c[0], (double)f->egt_c[1],
        (double)f->flap_ratio, f->gear_deployed,
        (double)f->fuel_total_lbs, (double)f->oat_c,
        f->ap_engaged,
        f->master_warning, f->master_caution,
        stall, gpws, windshear, overspeed,
        bank_angle, gear_warn, engine_fire);
}

/* =========================================================================
 *  Delta detection — per protocol §4.3
 * ========================================================================= */

/**
 * @brief Check if any discrete field changed (immediate push).
 */
static int discrete_changed(const FlightDataValues* a,
                            const FlightDataValues* b)
{
    if (a->gear_deployed != b->gear_deployed) return 1;
    if (a->ap_engaged != b->ap_engaged) return 1;
    if (a->master_warning != b->master_warning) return 1;
    if (a->master_caution != b->master_caution) return 1;
    return 0;
}

/**
 * @brief Check if any continuous field changed beyond threshold.
 */
static int continuous_changed(const FlightDataValues* a,
                              const FlightDataValues* b)
{
    if (fabsf(a->altitude_ft - b->altitude_ft) > DELTA_ALT_FT) return 1;
    if (fabsf(a->altitude_agl_ft - b->altitude_agl_ft) > DELTA_AGL_FT) return 1;
    if (fabsf(a->ias_kts - b->ias_kts) > DELTA_IAS_KTS) return 1;
    if (fabsf(a->vs_fpm - b->vs_fpm) > DELTA_VS_FPM) return 1;
    if (fabsf(a->mach - b->mach) > DELTA_MACH) return 1;
    if (fabsf(a->heading_mag_deg - b->heading_mag_deg) > DELTA_HDG_DEG) return 1;
    if (fabsf(a->roll_deg - b->roll_deg) > DELTA_ROLL_DEG) return 1;
    if (fabsf(a->pitch_deg - b->pitch_deg) > DELTA_PITCH_DEG) return 1;
    if (fabsf(a->n1_pct[0] - b->n1_pct[0]) > DELTA_N1_PCT) return 1;
    if (fabsf(a->n1_pct[1] - b->n1_pct[1]) > DELTA_N1_PCT) return 1;
    if (fabsf(a->egt_c[0] - b->egt_c[0]) > DELTA_EGT_C) return 1;
    if (fabsf(a->egt_c[1] - b->egt_c[1]) > DELTA_EGT_C) return 1;
    if (fabsf(a->flap_ratio - b->flap_ratio) > DELTA_FLAP) return 1;
    if (fabsf(a->fuel_total_lbs - b->fuel_total_lbs) > DELTA_FUEL_LBS) return 1;
    if (fabsf(a->oat_c - b->oat_c) > DELTA_OAT_C) return 1;
    return 0;
}

/* =========================================================================
 *  Connection management
 * ========================================================================= */

static int begin_connect(AIAdvisor* ai)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)ai->port);

    struct hostent* he = gethostbyname(ai->host);
    if (!he) return -1;
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    ai->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ai->sock == SOCK_INVALID) return -1;

    sock_set_nonblock(ai->sock);

    int ret = connect(ai->sock, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == SOCK_ERR) {
        int err = sock_last_err();
        if (!IS_CONNECTING(err)) {
            sock_close(ai->sock);
            ai->sock = SOCK_INVALID;
            return -1;
        }
    }

    ai->connecting      = 1;
    ai->handshake_sent  = 0;
    ai->ws_connected    = 0;
    return 0;
}

static int send_handshake(AIAdvisor* ai)
{
    unsigned char rand_key[16];
    for (int i = 0; i < 16; i++) {
        rand_key[i] = (unsigned char)((SDL_GetTicks() >> (i % 4)) & 0xFF);
        rand_key[i] ^= (unsigned char)((rand_key[i] * 97 + 13) & 0xFF);
    }

    char b64_key[WS_KEY_LEN + 1];
    base64_encode(rand_key, 16, b64_key);

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET /api/copilot/stream HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        ai->host, ai->port, b64_key);

    int sent = send(ai->sock, req, req_len, 0);
    if (sent != req_len) return -1;

    ai->handshake_sent = 1;
    return 0;
}

static int check_handshake_response(const char* buf, int len)
{
    const char* end = memchr(buf, '\r', (size_t)len);
    if (!end) end = buf + len;

    if (end - buf >= 12 && memcmp(buf, "HTTP/", 5) == 0) {
        const char* status = buf + 9;
        if (status < end && status[0] == '1' && status[1] == '0'
            && status[2] == '1') {
            return 1;
        }
    }
    return 0;
}

/* =========================================================================
 *  Server message processing — per protocol §3.3 / §4.4
 * ========================================================================= */

/* Forward declarations */
static void do_push(AIAdvisor* ai, const FlightDataValues* fd, uint64_t now);

/* --- Mini string search (avoid potential strstr issues) ---------- */
static int has_str(const char* hay, int hay_len, const char* ndl)
{
    int nl = (int)strlen(ndl);
    if (nl > hay_len) return 0;
    for (int i = 0; i <= hay_len - nl; i++)
        if (memcmp(hay + i, ndl, (size_t)nl) == 0) return 1;
    return 0;
}

/**
 * @brief Parse a JSON frame from the server.
 */
static void process_server_message(AIAdvisor* ai, const char* payload, int len)
{
    uint64_t now = SDL_GetTicks();

    fprintf(stderr, "[AI] MSG has_done=%d has_true=%d has_token=%d: %.*s\n",
            has_str(payload, len, "\"done\""),
            has_str(payload, len, "true"),
            has_str(payload, len, "\"token\""),
            len, payload);

    /* {"done": true} or {"done":true} */
    if (has_str(payload, len, "\"done\"") && has_str(payload, len, "true")) {
        fprintf(stderr, "[AI] DONE, pending_len=%d\n", ai->pending_len);
        if (ai->pending_len > 0) {
            memcpy(ai->committed, ai->pending, (size_t)ai->pending_len);
            ai->committed_len  = ai->pending_len;
            ai->committed[ai->pending_len] = '\0';
            ai->committed_stale = 0;
            ai->last_done_ticks = now;
            fprintf(stderr, "[AI] COMMITTED: '%s'\n", ai->committed);
        }
        ai->pending_len = 0;
        ai->pending[0]  = '\0';
        ai->inferring = 0;

        /* If a discrete event fired during inference, push again now */
        if (ai->has_pending_push) {
            ai->has_pending_push = 0;
            do_push(ai, &ai->pending_push_fd, now);
            ai->inferring = 1;
        }
        return;
    }

    /* {"error":"..."} */
    if (has_str(payload, len, "\"error\"")) {
        fprintf(stderr, "[AI] ERROR: %.*s\n", len, payload);
        ai->pending_len = 0;
        ai->pending[0]  = '\0';
        ai->inferring = 0;
        /* Try pending push if queued */
        if (ai->has_pending_push) {
            ai->has_pending_push = 0;
            do_push(ai, &ai->pending_push_fd, SDL_GetTicks());
            ai->inferring = 1;
        }
        return;
    }

    /* {"heartbeat": ...} */
    if (has_str(payload, len, "\"heartbeat\"")) {
        return;
    }

    /* {"token": "..."} or {"token":"..."} */
    if (has_str(payload, len, "\"token\"")) {
        const char* end   = payload + len;
        const char* last_q = NULL;
        const char* prev_q = NULL;

        /* Find last two double-quote positions */
        for (const char* s = payload; s < end; s++) {
            if (*s == '"') { prev_q = last_q; last_q = s; }
        }

        /* Expect: ... "value" } — value is between prev_q+1 and last_q-1 */
        if (prev_q && last_q && last_q > prev_q + 1) {
            const char* val_start = prev_q + 1;
            int val_len = (int)(last_q - val_start);
            if (val_len > 0 &&
                ai->pending_len + val_len < ADVISORY_MAX - 1) {
                memcpy(ai->pending + ai->pending_len,
                       val_start, (size_t)val_len);
                ai->pending_len += val_len;
                ai->pending[ai->pending_len] = '\0';
            }
            fprintf(stderr, "[AI] TOKEN '%.*s' pending=%d\n",
                    val_len, val_start, ai->pending_len);
            return;
        }
    }
    /* Not a token/done/error/heartbeat — log */
    fprintf(stderr, "[AI] UNKNOWN: %.*s\n", len, payload);
}

/* =========================================================================
 *  Send flight state
 * ========================================================================= */

static int send_state(AIAdvisor* ai, const FlightDataValues* fd)
{
    char json[768];
    int json_len = serialize_state_json(fd, json, sizeof(json));
    if (json_len < 0 || json_len >= (int)sizeof(json)) return -1;

    unsigned char frame[1024];
    int frame_len = ws_build_text_frame(json, json_len, frame);

    int sent = send(ai->sock, (const char*)frame, frame_len, 0);
    fprintf(stderr, "[AI] PUSH %d bytes (sent=%d) hdr=", frame_len, sent);
    for (int _di = 0; _di < (frame_len < 16 ? frame_len : 16); _di++)
        fprintf(stderr, "%02X ", frame[_di]);
    fprintf(stderr, "\n");
    return (sent == frame_len) ? 0 : -1;
}

/**
 * @brief Push flight state and reset in-flight advisory.
 *
 * Per §3.5 (request cancellation): when we push new state, discard any
 * in-progress token stream — the server cancels the old inference too.
 */
static void do_push(AIAdvisor* ai, const FlightDataValues* fd, uint64_t now)
{
    if (send_state(ai, fd) == 0) {
        ai->last_sent = *fd;
        ai->last_push_ticks = now;
        /* Cancel old in-flight advisory — server does the same (§3.5) */
        ai->pending_len = 0;
        ai->pending[0]  = '\0';
    }
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

AIAdvisor* ai_advisor_create(const Config* config)
{
    int enabled = config_get_bool(config, "ai", "enabled", 0);
    if (!enabled) return NULL;

    AIAdvisor* ai = (AIAdvisor*)calloc(1, sizeof(AIAdvisor));
    if (!ai) return NULL;

    if (winsock_init() != 0) {
        free(ai);
        return NULL;
    }

    const char* host = config_get_str(config, "ai", "inference_host",
                                       "127.0.0.1");
    int port = (int)config_get_int(config, "ai", "inference_port", 8090);

    ai->sock             = SOCK_INVALID;
    ai->enabled          = 1;
    ai->ws_connected     = 0;
    ai->connecting       = 0;
    ai->handshake_sent   = 0;
    ai->committed_stale  = 0;

    strncpy(ai->host, host, HOST_MAX - 1);
    ai->host[HOST_MAX - 1] = '\0';
    ai->port = port;

    ai->last_connect_attempt = SDL_GetTicks();
    ai->reconnect_backoff_ms = RECONNECT_BASE_MS;
    begin_connect(ai);

    return ai;
}

void ai_advisor_update(AIAdvisor* ai, const FlightDataValues* fd)
{
    if (!ai || !ai->enabled) return;

    uint64_t now = SDL_GetTicks();

    /* =====================================================================
     *  Connection state machine
     * ===================================================================== */
    if (!ai->ws_connected) {
        if (ai->connecting) {
            if (ai->handshake_sent) {
                fd_set rfds;
                struct timeval tv = {0, 0};
                FD_ZERO(&rfds);
                FD_SET(ai->sock, &rfds);
                if (select(0, &rfds, NULL, NULL, &tv) > 0 &&
                    FD_ISSET(ai->sock, &rfds)) {
                    char buf[512];
                    int n = recv(ai->sock, buf, sizeof(buf) - 1, 0);
                    if (n > 0) {
                        buf[n] = '\0';
                        if (check_handshake_response(buf, n)) {
                            fprintf(stderr, "[AI] WS CONNECTED\n");
                            ai->ws_connected   = 1;
                            ai->connecting     = 0;
                            ai->handshake_sent = 0;
                            ai->reconnect_backoff_ms = RECONNECT_BASE_MS;
                            ai->committed_len  = 0;
                            ai->committed[0]   = '\0';
                            ai->pending_len    = 0;
                            ai->pending[0]     = '\0';
                            ai->committed_stale = 0;
                            ai->recv_len       = 0;
                            memset(&ai->last_sent, 0, sizeof(ai->last_sent));
                            /* §4.2: push first frame immediately after handshake */
                            ai->last_push_ticks = 0;
                        }
                    } else if (n == 0 ||
                               (n < 0 && sock_last_err() != WOULD_BLOCK)) {
                        sock_close(ai->sock);
                        ai->sock = SOCK_INVALID;
                        ai->connecting = 0;
                        ai->handshake_sent = 0;
                    }
                }
            } else {
                fd_set wfds;
                struct timeval tv = {0, 0};
                FD_ZERO(&wfds);
                FD_SET(ai->sock, &wfds);
                if (select(0, NULL, &wfds, NULL, &tv) > 0 &&
                    FD_ISSET(ai->sock, &wfds)) {
                    int err = 0;
                    socklen_t len = sizeof(err);
#ifdef _WIN32
                    getsockopt(ai->sock, SOL_SOCKET, SO_ERROR,
                               (char*)&err, &len);
#else
                    getsockopt(ai->sock, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
                    if (err == 0) {
                        send_handshake(ai);
                    } else {
                        sock_close(ai->sock);
                        ai->sock = SOCK_INVALID;
                        ai->connecting = 0;
                    }
                }
            }
        } else {
            if (now - ai->last_connect_attempt >
                (uint64_t)ai->reconnect_backoff_ms) {
                ai->last_connect_attempt = now;
                if (begin_connect(ai) == 0) {
                    ai->reconnect_backoff_ms *= 2;
                    if (ai->reconnect_backoff_ms > RECONNECT_MAX_MS)
                        ai->reconnect_backoff_ms = RECONNECT_MAX_MS;
                }
            }
        }
    }

    /* =====================================================================
     *  Receive frames (if connected)
     * ===================================================================== */
    if (ai->ws_connected && ai->sock != SOCK_INVALID) {
        fd_set rfds;
        struct timeval tv = {0, 0};
        FD_ZERO(&rfds);
        FD_SET(ai->sock, &rfds);

        int sel = select(0, &rfds, NULL, NULL, &tv);
        while (sel > 0 && FD_ISSET(ai->sock, &rfds)) {
            int space = RECV_BUF_SIZE - ai->recv_len - 1;
            if (space <= 0) { ai->recv_len = 0; space = RECV_BUF_SIZE - 1; }
            int n = recv(ai->sock, ai->recv_buf + ai->recv_len, space, 0);
            if (n > 0) {
                ai->recv_len += n;
                ai->recv_buf[ai->recv_len] = '\0';
                fprintf(stderr, "[AI] RECV %d bytes, total=%d buf[0..%d]=",
                        n, ai->recv_len, ai->recv_len < 40 ? ai->recv_len : 40);
                for (int _di = 0; _di < (ai->recv_len < 40 ? ai->recv_len : 40); _di++)
                    fprintf(stderr, "%02X ", (unsigned char)ai->recv_buf[_di]);
                fprintf(stderr, "\n");

                const unsigned char* payload;
                int payload_len, opcode, consumed;
                while (ws_parse_frame((const unsigned char*)ai->recv_buf,
                                      ai->recv_len, &payload, &payload_len,
                                      &opcode, &consumed) == 1) {
                    fprintf(stderr, "[AI] FRAME op=%d len=%d consumed=%d buf=%d\n",
                            opcode, payload_len, consumed, ai->recv_len);
                    switch (opcode) {
                    case 0x1:  /* Text */
                        fprintf(stderr, "[AI] TEXT: '%.*s'\n",
                                payload_len, (const char*)payload);
                        process_server_message(ai,
                            (const char*)payload, payload_len);
                        break;
                    case 0x8:  /* Close */
                        sock_close(ai->sock);
                        ai->sock = SOCK_INVALID;
                        ai->ws_connected = 0;
                        ai->recv_len = 0;
                        ai->last_connect_attempt = now;
                        goto recv_done;
                    case 0x9: { /* Ping → Pong (MUST be masked!) */
                        unsigned char pong[256];
                        unsigned char mask[4];
                        ws_gen_mask_key(mask);
                        pong[0] = 0x8A;  /* FIN + Pong */
                        pong[1] = (unsigned char)(0x80 | payload_len);
                        pong[2] = mask[0]; pong[3] = mask[1];
                        pong[4] = mask[2]; pong[5] = mask[3];
                        for (int _mi = 0; _mi < payload_len; _mi++)
                            pong[6 + _mi] = ((const unsigned char*)payload)[_mi] ^ mask[_mi % 4];
                        send(ai->sock, (const char*)pong, payload_len + 6, 0);
                        break;
                    }
                    case 0xA:  /* Pong — ignore */
                        break;
                    default:
                        break;
                    }
                    int remaining = ai->recv_len - consumed;
                    if (remaining > 0)
                        memmove(ai->recv_buf, ai->recv_buf + consumed,
                                (size_t)remaining);
                    ai->recv_len = remaining;
                }
            } else if (n == 0) {
                sock_close(ai->sock);
                ai->sock = SOCK_INVALID;
                ai->ws_connected = 0;
                ai->last_connect_attempt = now;
                goto recv_done;
            } else {
                if (sock_last_err() != WOULD_BLOCK) {
                    sock_close(ai->sock);
                    ai->sock = SOCK_INVALID;
                    ai->ws_connected = 0;
                    ai->last_connect_attempt = now;
                    goto recv_done;
                }
            }
            FD_ZERO(&rfds);
            FD_SET(ai->sock, &rfds);
            sel = select(0, &rfds, NULL, NULL, &tv);
        }
        recv_done:;
    }

    /* =====================================================================
     *  Send flight state — per protocol §4.3
     *  Gated on !inferring to avoid canceling in-progress inference.
     *  Discrete events during inference are queued, not immediate.
     * ===================================================================== */
    if (ai->ws_connected && ai->sock != SOCK_INVALID) {
        int should_push = 0;

        if (ai->inferring) {
            /* Inference in progress — don't push (avoids canceling server).
             * But queue discrete events for a follow-up push after done. */
            if (discrete_changed(fd, &ai->last_sent)) {
                ai->has_pending_push = 1;
                ai->pending_push_fd  = *fd;
            }
        } else {
            /* Idle — push if anything changed */
            if (discrete_changed(fd, &ai->last_sent)) {
                should_push = 1;
            } else if (continuous_changed(fd, &ai->last_sent) &&
                       now - ai->last_push_ticks >= MIN_PUSH_INTERVAL_MS) {
                should_push = 1;
            } else if (now - ai->last_push_ticks >= MAX_SILENCE_MS) {
                should_push = 1;
            }
        }

        if (should_push) {
            do_push(ai, fd, now);
            ai->inferring = 1;
        }
    }

    /* =====================================================================
     *  Advisory timeout check — per protocol §4.4
     * ===================================================================== */
    if (ai->committed_len > 0 && !ai->committed_stale &&
        ai->last_done_ticks > 0 &&
        now - ai->last_done_ticks > ADVISORY_TIMEOUT_MS) {
        ai->committed_stale = 1;
    }
}

const char* ai_advisor_get_advisory(const AIAdvisor* ai)
{
    if (!ai || !ai->enabled) return NULL;
    /* Return committed (complete response) first; fall back to
     * pending (streaming tokens in progress) so the user sees
     * advice appearing token-by-token without waiting for done. */
    if (ai->committed_len > 0) return ai->committed;
    if (ai->pending_len > 0)   return ai->pending;
    return NULL;
}

int ai_advisor_is_connected(const AIAdvisor* ai)
{
    if (!ai) return 0;
    return ai->ws_connected;
}

void ai_advisor_destroy(AIAdvisor* ai)
{
    if (!ai) return;
    if (ai->sock != SOCK_INVALID) {
        sock_close(ai->sock);
        ai->sock = SOCK_INVALID;
    }
    winsock_cleanup();
    free(ai);
}
