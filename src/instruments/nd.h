/**
 * @file    nd.h
 * @brief   Navigation Display (ND) instrument.
 *
 * Displays a compass rose with:
 *   - Heading (rotating compass card)
 *   - Range rings with distance labels
 *   - Aircraft symbol at center
 *   - Wind vector
 *   - Active waypoint / track line
 *   - Selected heading bug
 *   - Mode (ROSE / ARC / PLAN) and range selector
 */

#ifndef ND_H
#define ND_H

#include "instrument.h"

Instrument* nd_create(void);

#endif /* ND_H */
