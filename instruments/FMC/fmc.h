/**
 * @file    fmc.h
 * @brief   Flight Management Computer (FMC) CDU-style instrument.
 *
 * Simulates the Control Display Unit (CDU) with:
 *   - Multi-page display (INIT, RTE, LEGS, PERF, PROG)
 *   - Line-select keys (LSK L1-L6, R1-R6)
 *   - Scratchpad for data entry
 *   - Alpha-numeric key simulation via keyboard
 *   - Flight plan display & editing
 */

#ifndef FMC_H
#define FMC_H

#include "instrument.h"

Instrument* fmc_create(void);

#endif /* FMC_H */
