/**
 * @file    xplane.h
 * @brief   X-Plane 11 UDP DATA packet protocol handler.
 *
 * X-Plane 11 sends flight data via UDP in two modes:
 *   - RPOS (request position): Prologue "RPOS" + 1-byte freq → X-Plane
 *     replies with position data at that rate.
 *   - DATA packets: Prologue "DATA" + 4-byte index + 8×4-byte floats.
 *
 * See: X-Plane/Resources/plugins/DataRefs.txt for data group indices.
 *
 * Common data group indices:
 *    3 — Speeds (IAS, TAS, GS, VVI)
 *    4 — Mach, VVI, g-load
 *   17 — Pitch, roll, heading true, heading mag
 *   18 — Pitch/roll/heading rates
 *   20 — Latitude, longitude, altitude MSL, altitude AGL
 *   25 — Throttle (1-4), mixture, prop
 *   26 — Engine N1 % (1-4)
 *   34 — Engine EGT °C (1-4)
 *   37 — Engine fuel flow lbs/hr (1-4)
 *   63 — Autopilot settings
 *   67 — Barometric setting
 *   68 — Wind speed/direction
 *  111 — NAV1 frequency, NAV2 frequency, NAV1 course, NAV2 course
 *  136 — Gear deployment ratio
 */

#ifndef XPLANE_H
#define XPLANE_H

#include "udp.h"
#include "data/flight_data.h"

#include <stdint.h>

/* =========================================================================
 *  X-Plane DATA packet format
 * ========================================================================= */

#define XP_DATA_PROLOGUE    "DATA"
#define XP_DATA_PROLOGUE_LEN 5     /* "DATA\0" including null */
#define XP_DATA_HEADER_LEN   9     /* 5 prologue + 4 index */
#define XP_DATA_PACKET_LEN   41    /* 5 + 4 + 8*4 = 41 bytes */

/**
 * @brief Parse a raw X-Plane DATA packet and update flight data.
 *
 * Handles the "DATA" prologue format. Ignores non-DATA packets
 * (e.g., RPOS response format).
 *
 * @param data   Raw UDP payload.
 * @param len    Payload length.
 * @param fd     Flight data to update (thread-safe).
 */
void xplane_parse_packet(const uint8_t* data, int len, FlightData* fd);

/**
 * @brief Send a subscription request to X-Plane.
 *
 * Sends an RPOS request so X-Plane starts streaming data to our port.
 * X-Plane listens on its own UDP port for these commands.
 *
 * @param sock       Our UDP socket (for sending the request).
 * @param xp_host    X-Plane host IP (e.g. "127.0.0.1").
 * @param xp_port    X-Plane's receive port (default 49000).
 * @param freq_hz    Data rate request (1-99 Hz).
 * @return 0 on success, -1 on failure.
 */
int xplane_subscribe(UDPSocket* sock, const char* xp_host,
                     int xp_port, int freq_hz);

/**
 * @brief Send a single float value to an X-Plane DataRef via native UDP DREF.
 *
 * This uses X-Plane's built-in DREF command on port 49000. No plugin required.
 * Supports both scalar DataRefs (e.g. "sim/cockpit/autopilot/heading_mag")
 * and indexed array elements (e.g. "sim/cockpit2/engine/actuators/throttle_ratio[0]").
 *
 * @param sock       Our UDP socket.
 * @param xp_host    X-Plane host IP (e.g. "127.0.0.1").
 * @param xp_port    X-Plane receive port (default 49000).
 * @param dref_path  Full DataRef path string.
 * @param value      Float value to set.
 * @return 0 on success, -1 on failure.
 */
int xplane_send_dref(UDPSocket* sock, const char* xp_host, int xp_port,
                     const char* dref_path, float value);

/**
 * @brief Send multiple float values to an X-Plane array DataRef.
 *
 * Use for setting entire arrays at once, e.g. all 4 throttle ratios:
 *   float thr[] = {0.85f, 0.85f, 0.0f, 0.0f};
 *   xplane_send_dref_array(sock, host, port,
 *       "sim/cockpit2/engine/actuators/throttle_ratio", thr, 4);
 *
 * @param sock       Our UDP socket.
 * @param xp_host    X-Plane host IP.
 * @param xp_port    X-Plane receive port (default 49000).
 * @param dref_path  Full DataRef path (without [N] index suffix).
 * @param values     Array of float values.
 * @param count      Number of values.
 * @return 0 on success, -1 on failure.
 */
int xplane_send_dref_array(UDPSocket* sock, const char* xp_host, int xp_port,
                           const char* dref_path, const float* values, int count);

/**
 * @brief Send an X-Plane command via native UDP CMND protocol.
 *
 * Format: "CMND\0" + command_path + "\0"
 * This triggers an X-Plane command just like a joystick button or key binding.
 *
 * Examples:
 *   xplane_send_command(sock, "127.0.0.1", 49000, "sim/FMS/key_A");
 *   xplane_send_command(sock, "127.0.0.1", 49000, "sim/FMS/key_CLR");
 *   xplane_send_command(sock, "127.0.0.1", 49000, "sim/autopilot/heading_sync");
 *
 * @param sock      Our UDP socket.
 * @param xp_host   X-Plane host IP.
 * @param xp_port   X-Plane receive port (default 49000).
 * @param command   Full X-Plane command path string.
 * @return 0 on success, -1 on failure.
 */
int xplane_send_command(UDPSocket* sock, const char* xp_host, int xp_port,
                        const char* command);

/* =========================================================================
 *  RREF — Request DataRef (read values back from X-Plane)
 * ========================================================================= */

#define XP_RREF_PATH_MAX    400
#define XP_RREF_PACKET_SZ   413    /* "RREF\0"(5) + freq(4) + id(4) + path(400) */
#define XP_RREF_HEADER      4      /* "RREF" without null */
#define XP_RREF_MAX_DREFS    32    /* max concurrent subscriptions */

/**
 * @brief Subscribe to a DataRef value stream from X-Plane.
 *
 * Sends an RREF request packet. X-Plane will stream the value back at the
 * requested frequency on our receive port. Responses are parsed by
 * xplane_parse_rref() in the packet handler.
 *
 * @param sock       Our UDP socket (for sending).
 * @param xp_host    X-Plane host IP.
 * @param xp_port    X-Plane receive port (default 49000).
 * @param dref_path  Full DataRef path (e.g. "sim/cockpit/warnings/annunciators/bank_angle").
 * @param request_id Unique ID (0..255) to identify this subscription in responses.
 * @param freq_hz    Desired update rate (1-10 Hz; lower = less network load).
 * @return 0 on success, -1 on failure.
 */
int xplane_rref_subscribe(UDPSocket* sock, const char* xp_host, int xp_port,
                          const char* dref_path, int request_id, int freq_hz);

/**
 * @brief Parse an RREF response packet and update flight data.
 *
 * Call this from the packet handler when a non-DATA packet is received.
 * RREF responses have the prologue "RREF" (4 bytes, no null).
 *
 * @param data   Raw UDP payload.
 * @param len    Payload length.
 * @param fd     Flight data to update (thread-safe — call under lock).
 * @return 1 if packet was an RREF response, 0 otherwise.
 */
int xplane_parse_rref(const uint8_t* data, int len, FlightData* fd);

/**
 * @brief Subscribe to all required DREF alert state datarefs.
 *
 * Convenience function — calls xplane_rref_subscribe() for each of the
 * 25 alert-state datarefs needed for the B737 alert system.
 *
 * @param sock     Our UDP socket.
 * @param xp_host  X-Plane host IP.
 * @param xp_port  X-Plane receive port (default 49000).
 * @return 0 if at least one subscription succeeded, -1 on total failure.
 */
int xplane_rref_subscribe_all(UDPSocket* sock, const char* xp_host, int xp_port);

/* =========================================================================
 *  ND-specific RREF subscriptions
 * ========================================================================= */

/**
 * @brief RREF request IDs for ND data.
 *
 * These supplement the existing DATA packet stream with higher-precision
 * DREF values for the Navigation Display.
 */
#define XP_RREF_ND_LAT       30  /* sim/flightmodel/position/latitude        */
#define XP_RREF_ND_LON       31  /* sim/flightmodel/position/longitude       */
#define XP_RREF_ND_MAG_PSI   32  /* sim/flightmodel/position/mag_psi         */
#define XP_RREF_ND_TAS       33  /* sim/flightmodel/position/true_airspeed   */
#define XP_RREF_ND_GS        34  /* sim/flightmodel/position/groundspeed     */
#define XP_RREF_ND_HPATH     35  /* sim/flightmodel/position/hpath           */
#define XP_RREF_ND_MAG_HPATH 36  /* sim/flightmodel/position/mag_hpath       */

/**
 * @brief Subscribe to all 5 ND-specific DREFs.
 *
 * Sends RREF requests for:
 *   - sim/flightmodel/position/latitude       (float, degrees)
 *   - sim/flightmodel/position/longitude      (float, degrees)
 *   - sim/flightmodel/position/mag_psi        (float, degrees)
 *   - sim/flightmodel/position/true_airspeed  (float, m/s)
 *   - sim/flightmodel/position/groundspeed    (float, m/s)
 *
 * Values are parsed by xplane_parse_rref() and stored in
 * FlightDataValues::dref_nd_* fields.
 *
 * @param sock     Our UDP socket.
 * @param xp_host  X-Plane host IP.
 * @param xp_port  X-Plane receive port (default 49000).
 * @return 0 on success, -1 on failure.
 */
int xplane_rref_subscribe_nd(UDPSocket* sock, const char* xp_host, int xp_port);

#endif /* XPLANE_H */
