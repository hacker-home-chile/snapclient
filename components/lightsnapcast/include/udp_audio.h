// udp-music: UDP audio transport wire format (C mirror of udp_audio.hpp).
//
// Kept byte-exact with the server-side C++ header so that both ends agree
// on the 24-byte framing of every audio datagram.

#pragma once

#include <stdint.h>
#include <string.h>

#define UDP_AUDIO_MAGIC        0xA0u
#define UDP_AUDIO_VERSION      1u
#define UDP_AUDIO_HEADER_SIZE  24u

// flags bits
#define UDP_AUDIO_FLAG_IS_PARITY  (1u << 0)

#pragma pack(push, 1)
typedef struct
{
    uint8_t  magic;            // UDP_AUDIO_MAGIC
    uint8_t  version;          // UDP_AUDIO_VERSION
    uint8_t  flags;            // UDP_AUDIO_FLAG_*
    uint8_t  fec_group_size;   // N: data packets per FEC group
    uint32_t seq;              // global packet sequence
    uint32_t fec_group;        // FEC group id
    // Playout timestamp fraction in μs (0..999999). Seconds half is in `aux`
    // for data packets (see below). u32 μs wraps at ~71 min, which is why
    // we split the timestamp instead of packing it.
    uint32_t timestamp_us;
    uint16_t payload_len;      // bytes after this header
    uint16_t reserved0;
    // data   → timestamp_sec (server-clock seconds part of playout time)
    // parity → XOR of all data packet payload_lens in this group (used to
    //          recover the missing packet's true length).
    uint32_t aux;
} udp_audio_header_t;
#pragma pack(pop)

_Static_assert(sizeof(udp_audio_header_t) == UDP_AUDIO_HEADER_SIZE,
               "udp_audio_header_t must be exactly 24 bytes");

static inline void udp_audio_write_le16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}
static inline void udp_audio_write_le32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t udp_audio_read_le16(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t udp_audio_read_le32(const uint8_t* p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

// Parse a header from `buf` (must be at least UDP_AUDIO_HEADER_SIZE bytes).
// Returns 0 on success, -1 on invalid magic/version.
static inline int udp_audio_decode_header(const uint8_t* buf, udp_audio_header_t* out)
{
    if (buf[0] != UDP_AUDIO_MAGIC) return -1;
    if (buf[1] != UDP_AUDIO_VERSION) return -1;
    out->magic           = buf[0];
    out->version         = buf[1];
    out->flags           = buf[2];
    out->fec_group_size  = buf[3];
    out->seq             = udp_audio_read_le32(buf + 4);
    out->fec_group       = udp_audio_read_le32(buf + 8);
    out->timestamp_us    = udp_audio_read_le32(buf + 12);
    out->payload_len     = udp_audio_read_le16(buf + 16);
    out->reserved0       = udp_audio_read_le16(buf + 18);
    out->aux             = udp_audio_read_le32(buf + 20);
    return 0;
}
