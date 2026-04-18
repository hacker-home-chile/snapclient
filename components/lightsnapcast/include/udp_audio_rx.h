// udp-music: UDP audio receive pipeline.
//
// Owns a UDP socket bound locally, periodically registers with the server,
// receives Opus-in-UDP datagrams, reorders them in a small ring, recovers
// missing packets via XOR parity where possible, and on unrecoverable gaps
// drives Opus packet-loss concealment so the sample clock never slips.
//
// Decoded PCM is pushed through `insert_pcm_chunk` into the existing player
// queue, so the I2S playback loop in player.c is untouched.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "player.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Local UDP port to bind; server will send audio here.
    uint16_t local_port;
    // Server IP (string) + port for the registration datagram.
    const char* server_ip;
    uint16_t    server_port;
    // Client ID string (≤255 bytes). Persisted by reference — must outlive task.
    const char* client_id;
} udp_audio_rx_config_t;

// Start the rx task. Safe to call once after WiFi is up.
int udp_audio_rx_start(const udp_audio_rx_config_t* cfg);

// Tell the rx task the currently negotiated codec parameters. Call this when
// the server delivers a CodecHeader over TCP. If the decoder already exists
// with the same parameters this is a no-op; otherwise it's rebuilt.
// codec must be OPUS for now.
void udp_audio_rx_set_codec(codec_type_t codec, int32_t sample_rate,
                            uint8_t channels, uint8_t bits);

// One-line stats snapshot (also logged at 1 Hz from the task). Meant for
// external debug hooks.
typedef struct {
    uint32_t rx_ok;
    uint32_t rx_bad;
    uint32_t reorder_late;
    uint32_t drop_detected;
    uint32_t fec_recovered;
    uint32_t plc_invoked;
    uint32_t decoded_ok;
} udp_audio_rx_stats_t;

void udp_audio_rx_get_stats(udp_audio_rx_stats_t* out);

#ifdef __cplusplus
}
#endif
