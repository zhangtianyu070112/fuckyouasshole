/**
 * @file    eicas.h
 * @brief   Engine Indication and Crew Alerting System (EICAS) instrument.
 *
 * Displays:
 *   - N1 (fan speed) gauges for 2 engines
 *   - EGT (exhaust gas temperature) bars
 *   - Fuel flow indicators
 *   - Total fuel quantity
 *   - Flap / speedbrake position
 *   - Warning/alerts area
 */

#ifndef EICAS_H
#define EICAS_H

#include "instrument.h"

Instrument* eicas_create(void);

#endif /* EICAS_H */
