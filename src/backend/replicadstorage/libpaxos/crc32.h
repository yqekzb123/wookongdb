#ifndef __CRC32_H__
#define __CRC32_H__

#include <stdint.h>

uint32_t crc32(uint32_t crc, const uint8_t *buf, int len, int skiplen = 1);

#endif
