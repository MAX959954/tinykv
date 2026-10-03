#ifndef CRC32_H
#define CRC32_H
#include <stddef.h>
#include <stdint.h>

// CRC-32 (IEEE 802.3, the one zlib/PNG/Ethernet use): reflected polynomial
// 0xEDB88320, init and final XOR 0xFFFFFFFF. crc32("123456789") == 0xCBF43926.
uint32_t crc32(const void *data, size_t len);

#endif
