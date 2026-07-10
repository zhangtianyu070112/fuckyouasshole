/**
 * @file    xplane.c
 * @brief   X-Plane 11 UDP protocol implementation.
 *
 * Parses DATA packets and maps known data group indices to FlightData fields.
 * Unknown indices are silently ignored.
 *
 * X-Plane DATA packet layout (41 bytes total):
 *   [0..4]  Prologue: "DATA\0" or "DATA "  (5 ASCII bytes)
 *   [5..8]  Index: int32 (4 bytes, little-endian)
 *   [9..40] Values: 8 × float32 (32 bytes, little-endian)
 *
 * Note: X-Plane sometimes sends fewer than 8 floats per group.
 *       We only read as many as the packet provides.
 */

#include "xplane.h"
#include "utils/logger.h"

#include <string.h>

/* =========================================================================
 *  Packet header validation
 * ========================================================================= */

/**
 * @brief Read a little-endian int32 from buffer.
 */
static int32_t read_i32le(const uint8_t* buf)
{
    return (int32_t)buf[0]
         | ((int32_t)buf[1] << 8)
         | ((int32_t)buf[2] << 16)
         | ((int32_t)buf[3] << 24);
}

/**
 * @brief Read a little-endian float32 from buffer.
 * (Assumes IEEE 754 compatibility — true on x86/ARM.)
 */
static float read_f32le(const uint8_t* buf)
{
    int32_t raw = read_i32le(buf);
    float f;
    memcpy(&f, &raw, sizeof(f));
    return f;
}

/* =========================================================================
 *  Data group index to flight-data field mapping
 * ========================================================================= */

/**
 * @brief Apply a single data group's values to the flight data snapshot.
 *
 * This is called for each DATA packet received. We lock the flight data
 * only once per packet (to write ALL fields from this group atomically).
 */
static void apply_data_group(FlightDataValues* f, int index,
                             const float* vals, int num_vals)
{
    switch (index) {

    /* =====================================================================
     *  FLIGHT PARAMETERS (indices unchanged from XP11)
     * ===================================================================== */

    /* 3: Speeds */
    case 3:
        if (num_vals > 0) f->ias_kts  = vals[0];
        if (num_vals > 2) f->tas_kts  = vals[2];
        if (num_vals > 3) f->gs_kts   = vals[3];
        break;

    /* 4: Mach, VVI, G-load — XP12 already ft/min */
    case 4:
        if (num_vals > 0) f->mach   = vals[0];
        if (num_vals > 2) f->vs_fpm = vals[2];  /* VVI is usually at index 2 in group 4 */
        
        /* Calculate TAT accurately using OAT and Mach */
        f->tat_c = (f->oat_c + 273.15f) * (1.0f + 0.2f * f->mach * f->mach) - 273.15f;

        /* Debug log group 4 to see what it contains */
        static int print_count = 0;
        if (print_count++ % 100 == 0) {
            LOG_INFO("Group 4 debug: [0]=%f, [1]=%f, [2]=%f, [3]=%f, [4]=%f", 
                     (double)vals[0], (double)vals[1], (double)vals[2], (double)vals[3], (double)vals[4]);
        }
        break;

    /* 17: Pitch, roll, headings */
    case 17:
        if (num_vals > 0) f->pitch_deg        = vals[0];
        if (num_vals > 1) f->roll_deg         = vals[1];  /* XP +right = our -right */
        if (num_vals > 2) f->heading_true_deg = vals[2];
        if (num_vals > 3) {
            float mag = vals[3];
            if (mag < 0.0f || mag > 360.0f) mag = vals[2];
            f->heading_mag_deg = mag;
        }
        break;

    /* 20: Latitude, longitude, altitude — XP12 already feet */
    case 20:
        if (num_vals > 0) f->lat_deg          = vals[0];
        if (num_vals > 1) f->lon_deg          = vals[1];
        if (num_vals > 2) {
            f->alt_msl_ft   = vals[2];
            f->altitude_ft  = vals[2];   /* primary altitude source */
        }
        if (num_vals > 3) f->altitude_agl_ft   = vals[3];
        break;

    /* 25: Throttle (commanded) — 0.0 .. 1.0 */
    case 25:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->throttle[i] = vals[i];
        break;

    /* =====================================================================
     *  CONTROLS
     * ===================================================================== */

    /* 8: Joystick aileron/elevator/rudder (degrees) */
    case 8:
        if (num_vals > 0) f->elevator_deg = vals[0];
        if (num_vals > 1) f->aileron_deg  = vals[1];
        if (num_vals > 2) f->rudder_deg   = vals[2];
        break;

    /* 13: Trim, flaps, slats & speedbrakes */
    case 13:
        if (num_vals > 0) f->flap_ratio       = vals[0];
        if (num_vals > 3) f->speedbrake_ratio = vals[3];
        break;

    /* 14: Gear & brakes */
    case 14:
        if (num_vals > 0) {
            f->gear_ratio    = vals[0];
            f->gear_deployed = (vals[0] > 0.9f) ? 1 : 0;
        }
        if (num_vals > 1) f->parking_brake   = (vals[1] > 0.5f) ? 1 : 0;
        if (num_vals > 2) f->brake_temp_c[0] = vals[2];
        if (num_vals > 3) f->brake_temp_c[1] = vals[3];
        break;

    /* 67: Landing gear deployment (was baro in XP11!) */
    case 67:
        if (num_vals > 0) {
            f->gear_ratio    = vals[0];
            f->gear_deployed = (vals[0] > 0.9f) ? 1 : 0;
        }
        break;

    /* =====================================================================
     *  ENGINE (XP12 indices — ALL changed from XP11!)
     * ===================================================================== */

    /* 41: N1 — XP12 sends ratio (0.0–1.0), convert to percentage */
    case 41:
        for (int i = 0; i < 4 && i < num_vals; i++) {
            float n1 = vals[i];
            if (n1 > 0.0f && n1 <= 2.0f) n1 *= 100.0f;
            f->n1_pct[i] = n1;
        }
        break;

    /* 42: N2 — same ratio → percentage as N1 */
    case 42:
        for (int i = 0; i < 4 && i < num_vals; i++) {
            float n2 = vals[i];
            if (n2 > 0.0f && n2 <= 2.0f) n2 *= 100.0f;
            f->n2_pct[i] = n2;
        }
        break;

    /* 47: EGT — Exhaust Gas Temperature (°C) */
    case 47:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->egt_c[i] = vals[i];
        break;

    /* 45: Fuel Flow — XP12 already lbs/hr (pph) */
    case 45:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->fuel_flow_pph[i] = vals[i];
        break;

    /* 62: Fuel weights — total fuel per tank (lbs) */
    case 62:
        {
            float total = 0.0f;
            /* Store individual tank values: left[0], center[1], right[2] */
            for (int i = 0; i < num_vals && i < 3; i++) {
                f->fuel_tank_lbs[i] = vals[i];
                total += vals[i];
            }
            /* Sum any extra tanks beyond the first 3 */
            for (int i = 3; i < num_vals && i < 8; i++)
                total += vals[i];
            if (total > 0.0f) f->fuel_total_lbs = total;
        }
        break;

    /* 49: Oil pressure (psi) */
    case 49:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->oil_press_psi[i] = vals[i];
        break;

    /* 50: Oil temperature (°C) */
    case 50:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->oil_temp_c[i] = vals[i];
        break;

    /* 43: Manifold pressure */
    case 43:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->mpr_inhg[i] = vals[i];
        break;

    /* 44: Engine pressure ratio (EPR) */
    case 44:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->epr[i] = vals[i];
        break;

    /* 46: Turbine inlet temperature (ITT) */
    case 46:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->itt_c[i] = vals[i];
        break;

    /* 48: Cylinder head temperature (CHT) */
    case 48:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->cht_c[i] = vals[i];
        break;

    /* 51: Fuel pressure */
    case 51:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->fuel_press_psi[i] = vals[i];
        break;

    /* 34: Engine power */
    case 34:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->engine_power_hp[i] = vals[i];
        break;

    /* 37: Engine RPM */
    case 37:
        for (int i = 0; i < 4 && i < num_vals; i++)
            f->engine_rpm[i] = vals[i];
        break;

    /* =====================================================================
     *  AUTOPILOT (XP12 splits across 108 + 118)
     * ===================================================================== */

    /* 108: AP/FD/HUD switches */
    case 108:
        if (num_vals > 0) f->ap_engaged      = (vals[0] > 0.5f) ? 1 : 0;
        if (num_vals > 5) f->ap_athr_engaged = (vals[5] > 0.5f) ? 1 : 0;
        break;

    /* 118: AP values — hdg, alt, spd, vs (already imperial) */
    case 118:
        if (num_vals > 0) f->ap_hdg = vals[0];
        if (num_vals > 1) f->ap_alt = vals[1];   /* ft */
        if (num_vals > 2) f->ap_spd = vals[2];   /* kt */
        if (num_vals > 3) f->ap_vs  = vals[3];   /* ft/min */
        break;

    /* =====================================================================
     *  NAV / COM / DME / XPDR
     * ===================================================================== */

    /* 96: COM 1 & COM 2 frequencies (MHz) */
    case 96:
        if (num_vals > 0) f->com1_freq = vals[0];
        if (num_vals > 1) f->com2_freq = vals[1];
        break;

    /* 97: NAV 1 & NAV 2 frequencies + OBS course */
    case 97:
        if (num_vals > 0) f->nav1_freq   = vals[0];
        if (num_vals > 1) f->nav2_freq   = vals[1];
        if (num_vals > 2) f->nav1_course = vals[2];
        if (num_vals > 3) f->nav2_course = vals[3];
        break;

    /* 99: Pilot NAV 1 deflections (CDI) */
    case 99:
        if (num_vals > 0) f->nav1_cdi = vals[0];
        break;

    /* 102: DME status — already NM + kts in XP12 */
    case 102:
        if (num_vals > 0) f->dme_dist_nm   = vals[0];
        if (num_vals > 1) f->dme_speed_kts = vals[1];
        break;

    /* 104: Transponder status */
    case 104:
        if (num_vals > 0) f->xpdr_code = (int)vals[0];
        if (num_vals > 2) f->xpdr_mode = (int)vals[2];
        break;

    /* =====================================================================
     *  SYSTEMS
     * ===================================================================== */

    /* 5: METAR wind/dir/temp — OAT + wind */
    case 5:
        if (num_vals > 3) f->oat_c          = vals[3];
        if (num_vals > 0) f->wind_speed_kts = vals[1];
        if (num_vals > 1) f->wind_dir_deg   = vals[2];
        
        /* Calculate TAT accurately using OAT and Mach */
        f->tat_c = (f->oat_c + 273.15f) * (1.0f + 0.2f * f->mach * f->mach) - 273.15f;
        break;

    /* 109: Anti-ice switches */
    case 109:
        if (num_vals > 0) f->anti_ice_wing   = (vals[0] > 0.5f) ? 1 : 0;
        if (num_vals > 1) f->anti_ice_eng[0] = (vals[1] > 0.5f) ? 1 : 0;
        if (num_vals > 2) f->anti_ice_eng[1] = (vals[2] > 0.5f) ? 1 : 0;
        break;

    /* 113: General annunciators 1 — master warning/caution */
    case 113:
        if (num_vals > 0) f->master_warning = (vals[0] > 0.5f) ? 1 : 0;
        if (num_vals > 1) f->master_caution = (vals[1] > 0.5f) ? 1 : 0;
        break;

    /* 120: Pressurization status — already ft, psi */
    case 120:
        if (num_vals > 0) f->cabin_alt_ft   = vals[0];
        if (num_vals > 2) f->cabin_diff_psi = vals[2];
        break;

    /* 121: APU & GPU status */
    case 121:
        if (num_vals > 0) f->apu_n1_pct       = vals[0] * 100.0f;  /* 0..1 → % */
        if (num_vals > 1) f->apu_egt_c        = vals[1];
        if (num_vals > 2) f->apu_running      = (vals[2] > 0.5f) ? 1 : 0;
        if (num_vals > 3) f->elec_gen_amps[0] = vals[3];
        if (num_vals > 4) f->elec_gen_amps[1] = vals[4];
        if (num_vals > 5) f->elec_bus_volts   = vals[5];
        break;

    default:
        /* Unknown data group — silently ignore */
        break;
    }
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

void xplane_parse_packet(const uint8_t* data, int len, FlightData* fd)
{
    if (!data || !fd) return;

    /* Must have at least the 9-byte header (5 prologue + 4 index) */
    if (len < XP_DATA_HEADER_LEN) return;

    /* Check prologue — accept "DATA\0", "DATA ", or "DATA*" (XP11/12) */
    if (memcmp(data, "DATA", 4) != 0) {
        /* Try RREF response first (very common for alert state subscriptions) */
        if (xplane_parse_rref(data, len, fd)) {
            return;  /* handled as RREF */
        }
        /* Log the first few truly unknown packets */
        static int unknown_logged = 0;
        if (unknown_logged < 5) {
            unknown_logged++;
            LOG_DEBUG("Unknown packet: len=%d, hex=%02X %02X %02X %02X %02X %02X %02X %02X...",
                len, data[0], data[1], data[2], data[3],
                data[4], data[5], data[6], data[7]);
        }
        return;
    }

    /* Lock once per packet, apply all data groups found inside */
    SDL_LockMutex(fd->mutex);
    fd->packet_count++;
    fd->current.last_update_ticks = SDL_GetTicks();

    /* X-Plane 12 bundles many data groups into one UDP packet.
     * Format: "DATAx" (5 bytes) then repeated blocks of:
     *   [4 bytes LE int32: group index] [up to 8 × 4 bytes LE float32: values]
     *
     * X-Plane 11 sends one group per packet (41 bytes total).
     * We handle both by looping until we run out of data. */
    int offset = XP_DATA_PROLOGUE_LEN;   /* skip "DATAx" */
    int groups_parsed = 0;

    while (offset + 4 <= len) {
        /* Read group index */
        int32_t index = read_i32le(data + offset);
        offset += 4;

        /* Read up to 8 float values (or fewer if near end of packet) */
        float vals[8];
        int remaining = len - offset;
        int num_vals = remaining / 4;
        if (num_vals > 8) num_vals = 8;
        if (num_vals <= 0) break;

        for (int i = 0; i < num_vals; i++) {
            vals[i] = read_f32le(data + offset + i * 4);
        }
        offset += num_vals * 4;

        apply_data_group(&fd->current, (int)index, vals, num_vals);

        groups_parsed++;
    }

    SDL_UnlockMutex(fd->mutex);

    /* Log first batch: show key groups with XP12-corrected indices */
    static int first_batch = 1;
    if (first_batch) {
        first_batch = 0;
        LOG_INFO("📊 Parsed %d groups from %d-byte packet", groups_parsed, len);

        int off = XP_DATA_PROLOGUE_LEN;
        while (off + 8 <= len) {
            int32_t idx = read_i32le(data + off);
            off += 4;
            int nv = (len - off) / 4;
            if (nv > 8) nv = 8;
            if (nv <= 0) break;
            float v0 = (nv > 0) ? read_f32le(data + off) : 0.0f;
            float v1 = (nv > 1) ? read_f32le(data + off + 4) : 0.0f;
            float v2 = (nv > 2) ? read_f32le(data + off + 8) : 0.0f;
            float v3 = (nv > 3) ? read_f32le(data + off + 12) : 0.0f;
            switch (idx) {
            case 3:  LOG_INFO("  [3]  Spd: IAS=%.0f GS=%.0f TAS=%.0f", (double)v0,(double)v3,(double)read_f32le(data+off+20)); break;
            case 4:  LOG_INFO("  [4]  Mach/VVI: M=%.3f VVI=%.0f fpm", (double)v0,(double)v1); break;
            case 5:  LOG_INFO("  [5]  Wx: wind=%.0f kt @ %.0f° OAT=%.0f°C", (double)v1,(double)v2,(double)v3); break;
            case 17: LOG_INFO("  [17] Att: pitch=%.1f roll=%.1f hdgT=%.1f hdgM=%.1f", (double)v0,(double)v1,(double)v2,(double)v3); break;
            case 20: LOG_INFO("  [20] Pos: lat=%.4f lon=%.4f alt=%.0f ft agl=%.0f ft", (double)v0,(double)v1,(double)v2,(double)v3); break;
            case 25: LOG_INFO("  [25] Thr: %.2f %.2f %.2f %.2f", (double)v0,(double)v1,(double)v2,(double)v3); break;
            case 41: LOG_INFO("  [41] N1:  %.1f%% %.1f%% %.1f%% %.1f%%", (double)v0*100,(double)v1*100,(double)v2*100,(double)v3*100); break;
            case 42: LOG_INFO("  [42] N2:  %.1f%% %.1f%% %.1f%% %.1f%%", (double)v0*100,(double)v1*100,(double)v2*100,(double)v3*100); break;
            case 47: LOG_INFO("  [47] EGT: %.0f %.0f %.0f %.0f °C", (double)v0,(double)v1,(double)v2,(double)v3); break;
            case 45: LOG_INFO("  [45] FF:  %.0f %.0f %.0f %.0f pph", (double)v0,(double)v1,(double)v2,(double)v3); break;
            case 49: LOG_INFO("  [49] OilP: %.1f %.1f %.1f %.1f psi", (double)v0,(double)v1,(double)v2,(double)v3); break;
            case 50: LOG_INFO("  [50] OilT: %.1f %.1f %.1f %.1f °C", (double)v0,(double)v1,(double)v2,(double)v3); break;
            default: break;
            }
            off += nv * 4;
        }
    }
}

int xplane_subscribe(UDPSocket* sock, const char* xp_host,
                     int xp_port, int freq_hz)
{
    if (!sock || !xp_host) return -1;
    if (freq_hz < 1)  freq_hz = 1;
    if (freq_hz > 99) freq_hz = 99;

    /* Send "RPOS" + 1-byte frequency to request position data stream.
     * X-Plane 11 and 12 both still accept this, but its effect varies. */
    uint8_t rpos[5];
    rpos[0] = 'R';
    rpos[1] = 'P';
    rpos[2] = 'O';
    rpos[3] = 'S';
    rpos[4] = (uint8_t)freq_hz;
    udp_socket_sendto(sock, rpos, 5, xp_host, xp_port);
    LOG_INFO("RPOS sent to %s:%d @ %d Hz", xp_host, xp_port, freq_hz);

    /* Always ALSO send DSEL for every data group we care about.
     * X-Plane 12 relies on DSEL more than RPOS for multi-group streaming.
     * Indices updated for X-Plane 12 Data Output numbering. */
    static const int groups[] = {
        3, 4, 5, 8, 13, 14, 17, 20, 25,        /* flight + controls */
        34, 37, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,  /* engine: N1,N2,FF,EGT,OilP,OilT, etc */
        62,                                        /* fuel weights */
        96, 97, 99, 102, 104,                    /* nav/com/dme/xpdr */
        108, 118,                                 /* autopilot */
        109, 113, 120, 121, 67                   /* systems: anti-ice,annun,press,APU,gear */
    };
    static const int num_groups = (int)(sizeof(groups) / sizeof(groups[0]));

    for (int i = 0; i < num_groups; i++) {
        uint8_t dsel[6];
        dsel[0] = 'D';
        dsel[1] = 'S';
        dsel[2] = 'E';
        dsel[3] = 'L';
        dsel[4] = (uint8_t)(groups[i] & 0xFF);
        dsel[5] = (uint8_t)freq_hz;

        udp_socket_sendto(sock, dsel, 6, xp_host, xp_port);
        /* Small delay to avoid flooding X-Plane's command buffer */
        SDL_Delay(5);
    }

    LOG_INFO("DSEL sent for %d data groups @ %d Hz → %s:%d",
             num_groups, freq_hz, xp_host, xp_port);
    return 0;
}

/* =========================================================================
 *  DREF write functions (native X-Plane 12 UDP, no plugin required)
 * ========================================================================= */

/**
 * X-Plane 12 native DREF UDP packet format (port 49000, FIXED 509 bytes):
 *
 *   Offset  Size   Field
 *   ------  ----   -----
 *   0       5      "DREF\0"  (null-terminated header)
 *   5       4      Value: 32-bit float (little-endian)
 *   9       400    DataRef path string (null-padded to 400 bytes)
 *   409     100    Zero padding
 *
 *   TOTAL: 509 bytes (fixed — XP12 rejects variable-length DREF)
 *
 * NOTE: Only ONE float value per DREF in XP12 native protocol.
 * For array datarefs, use the XPUIPC plugin instead.
 *
 * Reference: X-Plane 12 Instructions/Exchanging Data with X-Plane.rtfd
 */

#define XP12_DREF_PATH_MAX  400
#define XP12_DREF_PACKET_SZ 509

int xplane_send_dref(UDPSocket* sock, const char* xp_host, int xp_port,
                     const char* dref_path, float value)
{
    if (!sock || !xp_host || !dref_path) return -1;

    int path_len = (int)strlen(dref_path);
    if (path_len < 1 || path_len >= XP12_DREF_PATH_MAX) return -1;

    uint8_t buf[XP12_DREF_PACKET_SZ];
    memset(buf, 0, sizeof(buf));

    /* [0..4] "DREF\0" */
    memcpy(buf, "DREF", 4);

    /* [5..8] float value (little-endian) */
    uint32_t raw;
    memcpy(&raw, &value, sizeof(raw));
    buf[5] = (uint8_t)(raw & 0xFF);
    buf[6] = (uint8_t)((raw >> 8) & 0xFF);
    buf[7] = (uint8_t)((raw >> 16) & 0xFF);
    buf[8] = (uint8_t)((raw >> 24) & 0xFF);

    /* [9..408] DataRef path (null-padded to 400 bytes) */
    memcpy(buf + 9, dref_path, (size_t)path_len);
    /* rest is already zero from memset */

    int ret = udp_socket_sendto(sock, buf, XP12_DREF_PACKET_SZ, xp_host, xp_port);
    if (ret > 0) {
        return 0;
    }

    LOG_WARN("xplane_send_dref: sendto failed for %s (ret=%d)", dref_path, ret);
    return -1;
}

/* Keep old array variant for backward compat — internally unused now */
int xplane_send_dref_array(UDPSocket* sock, const char* xp_host, int xp_port,
                           const char* dref_path, const float* values, int count)
{
    if (!sock || !xp_host || !dref_path || !values || count < 1) return -1;
    /* XP12 only supports single-float DREF; forward first value */
    return xplane_send_dref(sock, xp_host, xp_port, dref_path, values[0]);
}

/* =========================================================================
 *  CMND — execute X-Plane commands via native UDP
 * ========================================================================= */

/**
 * X-Plane native CMND UDP packet format (port 49000):
 *   [0..3]  "CMND"   4 ASCII bytes
 *   [4]     0x00     null terminator
 *   [5..N]  command_path\0  null-terminated command string
 */

int xplane_send_command(UDPSocket* sock, const char* xp_host, int xp_port,
                        const char* command)
{
    if (!sock || !xp_host || !command) return -1;

    int cmd_len = (int)strlen(command);
    if (cmd_len < 1 || cmd_len > 400) return -1;

    /* "CMND\0" + command + "\0" */
    uint8_t buf[512];
    int pos = 0;

    memcpy(buf + pos, "CMND", 4);  pos += 4;
    buf[pos++] = 0x00;

    memcpy(buf + pos, command, cmd_len);  pos += cmd_len;
    buf[pos++] = '\0';

    int ret = udp_socket_sendto(sock, buf, pos, xp_host, xp_port);
    if (ret > 0) {
        return 0;
    }

    LOG_WARN("xplane_send_command: sendto failed for %s (ret=%d)", command, ret);
    return -1;
}

/* =========================================================================
 *  RREF — Request DataRef (read values from X-Plane)
 * ========================================================================= */

/**
 * X-Plane RREF protocol (port 49000):
 *
 * Request (413 bytes fixed for XP12 compatibility):
 *   [0..4]   "RREF\0"  (5 bytes, null-terminated)
 *   [5..8]   Frequency: int32 LE (Hz, 1-10)
 *   [9..12]  Request ID: int32 LE (unique per subscription)
 *   [13..412] DataRef path: null-padded to 400 bytes
 *
 * Response (variable, received on our listen port):
 *   [0..3]   "RREF"  (4 bytes, no null)
 *   [4..7]   Request ID: int32 LE  (matches our subscription)
 *   [8..N]   float value(s): one 4-byte LE float per array element
 *
 * NOTE: For array datarefs (e.g. engine_fire[0]), X-Plane returns
 * a single scalar value. Use indexed paths to subscribe to each element.
 */

/* Map request ID → flight data field offset and type */
typedef struct {
    int   request_id;       /* our subscription ID */
    int   field_offset;     /* byte offset within FlightDataValues */
    int   is_float;         /* 1 = float, 0 = int (boolean) */
    const char* name;       /* for debug logging */
} RREFMapping;

static int rref_mapping_count = 0;

int xplane_rref_subscribe(UDPSocket* sock, const char* xp_host, int xp_port,
                          const char* dref_path, int request_id, int freq_hz)
{
    if (!sock || !xp_host || !dref_path) return -1;
    if (freq_hz < 1)  freq_hz = 1;
    if (freq_hz > 10) freq_hz = 10;

    int path_len = (int)strlen(dref_path);
    if (path_len < 1 || path_len >= XP_RREF_PATH_MAX) return -1;

    /* Build 413-byte RREF request */
    uint8_t buf[XP_RREF_PACKET_SZ];
    memset(buf, 0, sizeof(buf));

    /* [0..4] "RREF\0" */
    memcpy(buf, "RREF", 4);

    /* [5..8] frequency (int32 LE) */
    buf[5] = (uint8_t)(freq_hz & 0xFF);
    buf[6] = (uint8_t)((freq_hz >> 8) & 0xFF);
    buf[7] = (uint8_t)((freq_hz >> 16) & 0xFF);
    buf[8] = (uint8_t)((freq_hz >> 24) & 0xFF);

    /* [9..12] request ID (int32 LE) */
    buf[9]  = (uint8_t)(request_id & 0xFF);
    buf[10] = (uint8_t)((request_id >> 8) & 0xFF);
    buf[11] = (uint8_t)((request_id >> 16) & 0xFF);
    buf[12] = (uint8_t)((request_id >> 24) & 0xFF);

    /* [13..412] DataRef path (null-padded to 400 bytes) */
    memcpy(buf + 13, dref_path, (size_t)path_len);
    /* rest is already zero from memset */

    int ret = udp_socket_sendto(sock, buf, XP_RREF_PACKET_SZ, xp_host, xp_port);
    if (ret > 0) {
        LOG_INFO("RREF subscribed: [%d] %s @ %d Hz", request_id, dref_path, freq_hz);
        return 0;
    }

    LOG_WARN("xplane_rref_subscribe: sendto failed for %s (ret=%d)", dref_path, ret);
    return -1;
}

int xplane_parse_rref(const uint8_t* data, int len, FlightData* fd)
{
    if (!data || !fd) return 0;

    /* Must have at least: "RREF"(4) + request_id(4) + float(4) = 12 bytes */
    if (len < 12) return 0;

    /* Check prologue: "RREF" (4 bytes, no null — XP sends without null) */
    if (memcmp(data, "RREF", 4) != 0) return 0;

    /* Read request ID */
    int32_t req_id = (int32_t)data[4]
                   | ((int32_t)data[5] << 8)
                   | ((int32_t)data[6] << 16)
                   | ((int32_t)data[7] << 24);

    if (req_id < 0 || req_id >= XP_RREF_MAX_DREFS) {
        return 1;  /* acknowledge but out of range */
    }

    /* Read float value */
    int32_t raw = (int32_t)data[8]
                | ((int32_t)data[9] << 8)
                | ((int32_t)data[10] << 16)
                | ((int32_t)data[11] << 24);
    float value;
    memcpy(&value, &raw, sizeof(value));

    SDL_LockMutex(fd->mutex);

    /* Map request ID → FlightDataValues field
     * IDs 0-20: boolean annunciators (threshold at 0.5)
     * IDs 21-24: float hydraulic values */
    switch (req_id) {
    case 0:  fd->current.dref_bank_angle      = (value > 0.5f) ? 1 : 0; break;
    case 1:  fd->current.dref_stall_warning   = (value > 0.5f) ? 1 : 0; break;
    case 2:  fd->current.dref_gear_warning    = (value > 0.5f) ? 1 : 0; break;
    case 3:  fd->current.dref_gpws            = (value > 0.5f) ? 1 : 0; break;
    case 4:  fd->current.dref_overspeed       = (value > 0.5f) ? 1 : 0; break;
    case 5:  fd->current.dref_windshear       = (value > 0.5f) ? 1 : 0; break;
    case 6:  fd->current.dref_ap_disconnect   = (value > 0.5f) ? 1 : 0; break;
    case 7:  fd->current.dref_engine_fire     = (value > 0.5f) ? 1 : 0; break;
    case 8:  fd->current.dref_fire_warning    = (value > 0.5f) ? 1 : 0; break;
    case 9:  fd->current.dref_door            = (value > 0.5f) ? 1 : 0; break;
    case 10: fd->current.dref_generator       = (value > 0.5f) ? 1 : 0; break;
    case 11: fd->current.dref_anti_ice        = (value > 0.5f) ? 1 : 0; break;
    case 12: fd->current.dref_hyd_pressure    = (value > 0.5f) ? 1 : 0; break;
    case 13: fd->current.dref_hyd_quantity    = (value > 0.5f) ? 1 : 0; break;
    case 14: fd->current.dref_cabin_altitude  = (value > 0.5f) ? 1 : 0; break;
    case 15: fd->current.dref_fuel_quantity   = (value > 0.5f) ? 1 : 0; break;
    case 16: fd->current.dref_oil_pressure    = (value > 0.5f) ? 1 : 0; break;
    case 17: fd->current.dref_oil_temperature = (value > 0.5f) ? 1 : 0; break;
    case 18: fd->current.dref_voltage         = (value > 0.5f) ? 1 : 0; break;
    case 19: fd->current.dref_pressurization  = (value > 0.5f) ? 1 : 0; break;
    case 20: fd->current.dref_ice             = (value > 0.5f) ? 1 : 0; break;
    /* Hydraulic numeric values */
    case 21: fd->current.dref_hyd_press_psi[0] = value; break;
    case 22: fd->current.dref_hyd_press_psi[1] = value; break;
    case 23: fd->current.dref_hyd_qty_pct[0]   = value; break;
    case 24: fd->current.dref_hyd_qty_pct[1]   = value; break;
    /* ND-specific DREF values (indices 30-34) */
    case XP_RREF_ND_LAT:     fd->current.dref_nd_lat          = value; fd->current.dref_nd_valid |= 0x01; break;
    case XP_RREF_ND_LON:     fd->current.dref_nd_lon          = value; fd->current.dref_nd_valid |= 0x02; break;
    case XP_RREF_ND_MAG_PSI: fd->current.dref_nd_mag_psi      = value; fd->current.dref_nd_valid |= 0x04; break;
    case XP_RREF_ND_TAS:     fd->current.dref_nd_true_airspeed = value; fd->current.dref_nd_valid |= 0x08; break;
    case XP_RREF_ND_GS:      fd->current.dref_nd_groundspeed   = value; fd->current.dref_nd_valid |= 0x10; break;
    default: break;
    }

    SDL_UnlockMutex(fd->mutex);
    return 1;
}

/**
 * @brief Subscribe to all required DREF alert state datarefs.
 *
 * Call once during initialization, after X-Plane DATA subscription is set up.
 * Uses request IDs 0-24. All at 4 Hz (good balance between responsiveness
 * and network load for alert states that change infrequently).
 */
int xplane_rref_subscribe_all(UDPSocket* sock, const char* xp_host, int xp_port)
{
    if (!sock || !xp_host) return -1;

    /* Structure: { request_id, dref_path, freq_hz } */
    struct { int id; const char* path; int freq; } subs[] = {
        /* GPWS/warning annunciators (20 boolean) */
        {0,  "sim/cockpit/warnings/annunciators/bank_angle",            4},
        {1,  "sim/cockpit/warnings/annunciators/stall_warning",         4},
        {2,  "sim/cockpit/warnings/annunciators/gear_warning",          4},
        {3,  "sim/cockpit/warnings/annunciators/GPWS",                  4},
        {4,  "sim/cockpit/warnings/annunciators/overspeed",             4},
        {5,  "sim/cockpit/warnings/annunciators/windshear",             4},
        {6,  "sim/cockpit/warnings/annunciators/autopilot_disconnect",  4},
        {7,  "sim/cockpit/warnings/annunciators/engine_fire",           4},
        {8,  "sim/cockpit/warnings/annunciators/fire_warning",          4},
        {9,  "sim/cockpit/warnings/annunciators/door",                  2},
        {10, "sim/cockpit/warnings/annunciators/generator",             2},
        {11, "sim/cockpit/warnings/annunciators/anti_ice",              2},
        {12, "sim/cockpit/warnings/annunciators/hydraulic_pressure",    4},
        {13, "sim/cockpit/warnings/annunciators/hydraulic_quantity",    4},
        {14, "sim/cockpit/warnings/annunciators/cabin_altitude",        4},
        {15, "sim/cockpit/warnings/annunciators/fuel_quantity",         2},
        {16, "sim/cockpit/warnings/annunciators/oil_pressure",          2},
        {17, "sim/cockpit/warnings/annunciators/oil_temperature",       2},
        {18, "sim/cockpit/warnings/annunciators/voltage",               2},
        {19, "sim/cockpit/warnings/annunciators/pressurization",        4},
        {20, "sim/cockpit/warnings/annunciators/ice",                   2},
        /* Hydraulic system numeric values */
        {21, "sim/cockpit2/hydraulics/indicators/hydraulic_pressure_psi[0]", 4},
        {22, "sim/cockpit2/hydraulics/indicators/hydraulic_pressure_psi[1]", 4},
        {23, "sim/cockpit2/hydraulics/indicators/hydraulic_fluid_ratio[0]",  2},
        {24, "sim/cockpit2/hydraulics/indicators/hydraulic_fluid_ratio[1]",  2},
    };

    int num_subs = (int)(sizeof(subs) / sizeof(subs[0]));
    int ok = 0;

    for (int i = 0; i < num_subs; i++) {
        if (xplane_rref_subscribe(sock, xp_host, xp_port,
                                  subs[i].path, subs[i].id, subs[i].freq) == 0) {
            ok++;
        }
        /* Small delay between requests to avoid flooding XP's buffer */
        SDL_Delay(10);
    }

    LOG_INFO("RREF subscribed %d/%d alert datarefs", ok, num_subs);
    return (ok > 0) ? 0 : -1;
}

/* =========================================================================
 *  ND-specific RREF subscriptions
 * ========================================================================= */

int xplane_rref_subscribe_nd(UDPSocket* sock, const char* xp_host, int xp_port)
{
    if (!sock || !xp_host) return -1;

    struct { int id; const char* path; int freq; } subs[] = {
        { XP_RREF_ND_LAT,     "sim/flightmodel/position/latitude",       4 },
        { XP_RREF_ND_LON,     "sim/flightmodel/position/longitude",      4 },
        { XP_RREF_ND_MAG_PSI, "sim/flightmodel/position/mag_psi",        4 },
        { XP_RREF_ND_TAS,     "sim/flightmodel/position/true_airspeed",  4 },
        { XP_RREF_ND_GS,      "sim/flightmodel/position/groundspeed",    4 },
    };

    int num_subs = (int)(sizeof(subs) / sizeof(subs[0]));
    int ok = 0;

    for (int i = 0; i < num_subs; i++) {
        if (xplane_rref_subscribe(sock, xp_host, xp_port,
                                  subs[i].path, subs[i].id, subs[i].freq) == 0) {
            ok++;
        }
        SDL_Delay(10);
    }

    LOG_INFO("RREF ND: %d/%d datarefs subscribed", ok, num_subs);
    return (ok > 0) ? 0 : -1;
}
