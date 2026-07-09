#ifndef EICAS2_H
#define EICAS2_H

#include "instrument.h"

/**
 * @brief Create a new EICAS2 instrument instance.
 * @return Heap-allocated Instrument, or NULL on failure.
 */
Instrument* eicas2_create(void);

#endif /* EICAS2_H */