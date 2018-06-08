#ifndef UENDIAN_H__
#define UENDIAN_H__
#include <inttypes.h>
#include <string.h>

#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__OpenBSD__)
#include <endian.h>
#elif __FreeBSD__
#include <sys/endian.h>
#elif defined(__APPLE__) && defined(__MACH__)

#include <libkern/OSByteOrder.h>

#define htobe16(x) OSSwapHostToBigInt16(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)

#define htobe32(x) OSSwapHostToBigInt32(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)

#define htobe64(x) OSSwapHostToBigInt64(x)
#define htole64(x) OSSwapHostToLittleInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#define le64toh(x) OSSwapLittleToHostInt64(x)

#else
#error Couldn't find any appropriate endian.h
#endif // PLATFORMS

#endif // UENDIAN_H__