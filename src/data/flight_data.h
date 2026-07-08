/**
 * @file    flight_data.h
 * @brief   Thread-safe flight data container.
 *
 * Holds all real-time flight parameters received from X-Plane 11.
 * The UDP thread writes; the main thread reads via atomic snapshots.
 *
 * Thread safety: All reads go through flight_data_snapshot() which
 * copies the current values while holding the internal mutex briefly.
 * The UDP thread calls flight_data_update() with the same mutex.
 */

#ifndef FLIGHT_DATA_H
#define FLIGHT_DATA_H

#include <SDL2/SDL.h>
#include <stdint.h>

/* =========================================================================
 *  Flight data values (the actual parameters)
 * ========================================================================= */

typedef struct FlightDataValues {
    /* --- Attitude (degrees) --- */
    float roll_deg;           /* -180..+180, positive = right wing down */
    float pitch_deg;          /* -90..+90, positive = nose up */
    float heading_true_deg;   /* 0..360 true heading */
    float heading_mag_deg;    /* 0..360 magnetic heading */

    /* --- Air data --- */
    float ias_kts;            /* Indicated airspeed (knots) */
    float tas_kts;            /* True airspeed (knots) */
    float gs_kts;             /* Ground speed (knots) */
    float mach;               /* Mach number */
    float vs_fpm;             /* Vertical speed (feet/min, + = climb) */
    float altitude_ft;        /* MSL altitude (feet) */
    float altitude_agl_ft;    /* AGL altitude (feet) */
    float baro_setting_inhg;  /* Barometric setting (inHg) */

    /* --- Position --- */
    double lat_deg;           /* Latitude (-90..+90) */
    double lon_deg;           /* Longitude (-180..+180) */
    float  alt_msl_ft;        /* Altitude from GPS (MSL, feet) */

    /* --- Engine (up to 4 engines) --- */
    float n1_pct[4];          /* N1 / fan speed (%) */
    float n2_pct[4];          /* N2 / core speed (%) */
    float egt_c[4];           /* Exhaust gas temperature (°C) */
    float fuel_flow_pph[4];   /* Fuel flow (lbs/hour) */
    float oil_press_psi[4];   /* Oil pressure (psi) */
    float oil_temp_c[4];      /* Oil temperature (°C) */

    /* --- Extra Engine Data from index.txt --- */
    float mpr_inhg[4];        /* Manifold pressure (inHg) */
    float epr[4];             /* Engine pressure ratio (EPR) */
    float itt_c[4];           /* Turbine inlet temperature (ITT, °C) */
    float cht_c[4];           /* Cylinder head temperature (CHT, °C) */
    float fuel_press_psi[4];  /* Fuel pressure (psi) */
    float engine_power_hp[4]; /* Engine power (hp or watts depending on XP) */
    float engine_rpm[4];      /* Engine RPM */

    /* --- Fuel --- */
    float fuel_total_lbs;     /* Total fuel remaining (lbs) */
    float fuel_flow_total_pph;/* Total fuel flow (lbs/hour) */

    /* --- Flight controls --- */
    float throttle[4];        /* Throttle position (0..1) */
    float flap_ratio;         /* Flap deployment (0..1) */
    float speedbrake_ratio;   /* Speedbrake (0..1) */
    float elevator_deg;       /* Elevator deflection (°) */
    float aileron_deg;        /* Aileron deflection (°) */
    float rudder_deg;         /* Rudder deflection (°) */

    /* --- Autopilot --- */
    int   ap_engaged;         /* 1 = AP on */
    float ap_hdg;             /* Selected heading */
    float ap_alt;             /* Selected altitude (ft) */
    float ap_spd;             /* Selected speed (kts) */
    float ap_vs;              /* Selected vertical speed (fpm) */
    int   ap_athr_engaged;    /* 1 = autothrottle on */

    /* --- Navigation --- */
    float nav1_freq;          /* NAV1 frequency (MHz) */
    float nav2_freq;          /* NAV2 frequency (MHz) */
    float nav1_course;        /* NAV1 OBS course (°) */
    float nav2_course;        /* NAV2 OBS course (°) */
    float nav1_cdi;           /* NAV1 CDI deflection (−1..+1) */
    float nav2_cdi;           /* NAV2 CDI deflection (−1..+1) */
    float nav1_radial;        /* NAV1 radial (°) */
    float nav2_radial;        /* NAV2 radial (°) */
    float dme_dist_nm;        /* DME distance (NM) */
    float dme_speed_kts;      /* DME ground speed (kts) */

    /* --- COM Radio (new) --- */
    float com1_freq;          /* COM1 frequency (MHz) */
    float com2_freq;          /* COM2 frequency (MHz) */

    /* --- Transponder (new) --- */
    int   xpdr_code;          /* Squawk code (e.g. 1200) */
    int   xpdr_mode;          /* 0=off, 1=stby, 2=on, 3=alt */

    /* --- Landing gear --- */
    int   gear_deployed;      /* 1 = gear down */
    float gear_ratio;         /* 0..1 deployment */

    /* --- Brakes (new) --- */
    int   parking_brake;      /* 1 = parking brake set */
    float brake_temp_c[2];    /* Brake temperature °C (L/R) */

    /* --- Environment --- */
    float wind_speed_kts;     /* Wind speed */
    float wind_dir_deg;       /* Wind direction (°) */
    float oat_c;              /* Outside air temperature (°C) */
    float oat_isa_dev_c;      /* ISA deviation (°C) */
    float tat_c;              /* Total air temperature (°C) */

    /* --- Pressurization (new) --- */
    float cabin_alt_ft;       /* Cabin altitude (feet) */
    float cabin_diff_psi;     /* Cabin differential pressure (psi) */

    /* --- Electrical (new) --- */
    float elec_bus_volts;     /* Main DC bus voltage */
    float elec_gen_amps[2];   /* Generator load (amps per engine) */

    /* --- Anti-ice (new) --- */
    int   anti_ice_wing;      /* Wing anti-ice on */
    int   anti_ice_eng[2];    /* Engine anti-ice on */

    /* --- Hydraulics (new) --- */
    float hyd_press_psi[2];   /* Hydraulic system pressure A/B (psi) */
    float hyd_qty_pct[2];     /* Hydraulic reservoir quantity A/B (%) */

    /* --- APU (new) --- */
    float apu_n1_pct;         /* APU N1 (%) */
    float apu_egt_c;          /* APU EGT (°C) */
    int   apu_running;        /* 1 = APU running */

    /* --- Annunciators (new) --- */
    int   master_warning;     /* Master warning light */
    int   master_caution;     /* Master caution light */

    /* --- DREF alert state bits (populated by RREF, 1-4 Hz) --- */
    int   dref_bank_angle;        /* sim/cockpit/warnings/annunciators/bank_angle */

    /* --- ND-specific DREF values (populated by ND RREF subscriptions) --- */
    float dref_nd_lat;            /* sim/flightmodel/position/latitude (float, degrees) */
    float dref_nd_lon;            /* sim/flightmodel/position/longitude (float, degrees) */
    float dref_nd_mag_psi;        /* sim/flightmodel/position/mag_psi (float, degrees) */
    float dref_nd_true_airspeed;  /* sim/flightmodel/position/true_airspeed (float, m/s) */
    float dref_nd_groundspeed;    /* sim/flightmodel/position/groundspeed (float, m/s) */
    int   dref_nd_valid;          /* Bitmask: 0x01=lat, 0x02=lon, 0x04=hdg, 0x08=tas, 0x10=gs */
    int   dref_stall_warning;     /* sim/cockpit/warnings/annunciators/stall_warning */
    int   dref_gear_warning;      /* sim/cockpit/warnings/annunciators/gear_warning */
    int   dref_gpws;              /* sim/cockpit/warnings/annunciators/GPWS */
    int   dref_overspeed;         /* sim/cockpit/warnings/annunciators/overspeed */
    int   dref_windshear;         /* sim/cockpit/warnings/annunciators/windshear */
    int   dref_ap_disconnect;     /* sim/cockpit/warnings/annunciators/autopilot_disconnect */
    int   dref_engine_fire;       /* sim/cockpit/warnings/annunciators/engine_fire (index 0/1) */
    int   dref_fire_warning;      /* sim/cockpit/warnings/annunciators/fire_warning */
    int   dref_door;              /* sim/cockpit/warnings/annunciators/door */
    int   dref_generator;         /* sim/cockpit/warnings/annunciators/generator */
    int   dref_anti_ice;          /* sim/cockpit/warnings/annunciators/anti_ice */
    int   dref_hyd_pressure;      /* sim/cockpit/warnings/annunciators/hydraulic_pressure */
    int   dref_hyd_quantity;      /* sim/cockpit/warnings/annunciators/hydraulic_quantity */
    int   dref_cabin_altitude;    /* sim/cockpit/warnings/annunciators/cabin_altitude */
    int   dref_fuel_quantity;     /* sim/cockpit/warnings/annunciators/fuel_quantity */
    int   dref_oil_pressure;      /* sim/cockpit/warnings/annunciators/oil_pressure */
    int   dref_oil_temperature;   /* sim/cockpit/warnings/annunciators/oil_temperature */
    int   dref_voltage;           /* sim/cockpit/warnings/annunciators/voltage */
    int   dref_pressurization;    /* sim/cockpit/warnings/annunciators/pressurization */
    int   dref_ice;               /* sim/cockpit/warnings/annunciators/ice */

    /* --- DREF hydraulic values --- */
    float dref_hyd_press_psi[2];  /* sim/cockpit2/hydraulics/indicators/hydraulic_pressure_psi[0..1] */
    float dref_hyd_qty_pct[2];    /* sim/cockpit2/hydraulics/indicators/hydraulic_fluid_ratio[0..1] */

    /* --- Timestamp --- */
    uint64_t last_update_ticks;  /* SDL_GetTicks() when this was written */

} FlightDataValues;

/* =========================================================================
 *  Thread-safe wrapper
 * ========================================================================= */

typedef struct FlightData {
    FlightDataValues  current;        /* Current values */
    SDL_mutex*        mutex;          /* Protects `current` */
    uint32_t          packet_count;   /* Total packets received */
    uint32_t          packet_dropped; /* Packets dropped (parse errors) */
    uint32_t          update_count;   /* Times flight_data_update() called */
} FlightData;

/* --- Lifecycle --------------------------------------------------------- */

/**
 * @brief Create a thread-safe flight data container (zero-filled).
 */
FlightData* flight_data_create(void);

/**
 * @brief Free the container and its mutex.
 */
void        flight_data_destroy(FlightData* fd);

/* --- Thread-safe access ------------------------------------------------- */

/**
 * @brief Thread-safe snapshot: copies current values under lock.
 *
 * Call this from the main thread each frame. The lock is held only
 * for the duration of the memcpy, not during rendering.
 *
 * @param fd       The shared flight data.
 * @param snapshot Output buffer filled with current values.
 */
void flight_data_snapshot(FlightData* fd, FlightDataValues* snapshot);

/**
 * @brief Thread-safe update: writes new values under lock.
 *
 * Called by the UDP/networking thread. Only fields that were actually
 * updated in the packet should be set before calling.
 *
 * @param fd     The shared flight data.
 * @param src    Source values to copy in.
 */
void flight_data_update(FlightData* fd, const FlightDataValues* src);

/**
 * @brief Get the current value of a single field by offset.
 *
 * Used for lightweight reads where a full snapshot is overkill.
 * @param fd      The shared flight data.
 * @param offset  Byte offset of the field within FlightDataValues.
 * @param size    Size of the field (sizeof(float), sizeof(double), etc.).
 * @param out     Output buffer.
 */
void flight_data_read_field(FlightData* fd, size_t offset,
                            size_t size, void* out);

#endif /* FLIGHT_DATA_H */
