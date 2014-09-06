#ifndef CRC_H
#define CRC_H

#include <stdint.h>

uint32_t crc(uint8_t const * first, uint8_t const * last, uint32_t seed = 0);

#endif // CRC_H
