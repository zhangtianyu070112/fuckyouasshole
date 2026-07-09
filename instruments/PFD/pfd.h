/**
 * @file    pfd.h
 * @brief   Primary Flight Display (PFD) instrument.
 *
 * Displays (Boeing-style glass cockpit layout):
 *   - Flight Mode Annunciator (FMA) at top — 4-column A/THR | ROLL | PITCH | AP
 *   - Thrust Reference (N1 gauges) — dual-engine N1% bars above attitude
 *   - Attitude indicator (artificial horizon, pitch ladder, roll pointer)
 *   - Airspeed tape (left) — continuous float-based scrolling
 *   - Altitude tape (right) — continuous float-based scrolling with bands
 *   - Vertical Speed Indicator (right of altitude) — scale + pointer + digital
 *   - Heading tape (bottom) — compass-rose style scrolling
 *
 * All tapes use float-based position calculation for sub-pixel smooth scrolling.
 *
 * Usage:
 *   Instrument* pfd = pfd_create();
 *   // pfd is fully initialised with all callbacks set
 */

#ifndef PFD_H
#define PFD_H

#include "instrument.h"

/**
 * @brief Create a new PFD instrument instance.
 * @return Heap-allocated Instrument, or NULL on failure.
 */
Instrument* pfd_create(void);

#endif /* PFD_H */
