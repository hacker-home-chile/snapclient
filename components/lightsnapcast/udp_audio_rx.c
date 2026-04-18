// udp-music: UDP audio receive + decode pipeline.
//
// Protocol: each datagram carries udp_audio_header_t (see udp_audio.h) then
// either an Opus frame (data) or an XOR of the group's data payloads
// (parity). Group of N data packets + 1 parity recovers 1 loss per group.
//
// On unrecoverable gap: opus_decode(NULL) drives PLC so the sample clock
// does not drift.

#include "udp_audio_rx.h"
#include "udp_audio.h"

#include <errno.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "opus.h"

#include "player.h"
#include "snapcast.h"

#define TAG "udp-rx"

// ── Configuration constants ─────────────────────────────────────────────────

// Max Opus frame size we'll ever see: 160 kbps × 60 ms ≈ 1200 B, well below.
#define UDP_RX_MAX_PAYLOAD   1500
// Reorder ring (power of two for cheap modulo). 16 × ~500 B ≈ 8 KB.
#define UDP_RX_RING_SIZE     16
#define UDP_RX_RING_MASK     (UDP_RX_RING_SIZE - 1)
// How far a packet's group id may lag before we flush it.
#define UDP_RX_FLUSH_LAG_GROUPS  2
// How often to re-send the registration datagram (ms) — also keeps NAT open
// if any intermediate router needs it (dev setup uses direct LAN).
#define UDP_RX_REGISTER_PERIOD_MS 2000
// Sample count per Opus frame (48 kHz × 20 ms). Adjust if server changes.
#define UDP_RX_OPUS_FRAMES_PER_PACKET 960
// Stereo output.
#define UDP_RX_OPUS_CHANNELS 2

// ── Ring slot ───────────────────────────────────────────────────────────────

typedef struct {
    bool     valid;
    bool     is_parity;
    uint32_t seq;
    uint32_t fec_group;
    uint32_t timestamp_sec;     // from hdr aux on data packets (server clock)
    uint32_t timestamp_usec;    // from hdr timestamp_us (0..999999)
    uint16_t payload_len;
    uint8_t  payload[UDP_RX_MAX_PAYLOAD];
} udp_rx_slot_t;

// Per-group aggregator for FEC accounting.
typedef struct {
    uint32_t group_id;
    uint8_t  group_size;
    uint8_t  data_seen;       // number of non-parity pkts observed
    bool     parity_seen;
    uint16_t parity_len;      // parity's payload_len (= max padded len)
    uint32_t parity_length_xor; // parity header aux (= XOR of data lens)
    uint8_t  parity_payload[UDP_RX_MAX_PAYLOAD];
    uint32_t expected_first_seq; // first data seq in the group (unknown until we see it)
    bool     expected_first_seq_known;
    uint32_t seen_seqs_mask;  // bit i = seq (expected_first_seq+i) received (up to 32 bits — plenty for N≤8)
} udp_rx_group_t;

#define UDP_RX_GROUP_HISTORY 4
static udp_rx_group_t g_groups[UDP_RX_GROUP_HISTORY];

// ── State ───────────────────────────────────────────────────────────────────

static udp_audio_rx_config_t g_cfg;
static TaskHandle_t g_task = NULL;
static int g_sock = -1;

static SemaphoreHandle_t g_codec_mx;
static OpusDecoder*      g_opus = NULL;
static int32_t           g_sr = 48000;
static uint8_t           g_ch = 2;
static uint8_t           g_bits = 16;
static codec_type_t      g_codec = NONE;

static udp_rx_slot_t g_ring[UDP_RX_RING_SIZE];
static bool          g_have_first = false;
static uint32_t      g_next_expected_seq = 0;
static uint32_t      g_highest_group_closed = 0;

static udp_audio_rx_stats_t g_stats = {0};

// ── Helpers ─────────────────────────────────────────────────────────────────

static udp_rx_group_t* group_for(uint32_t group_id, uint8_t group_size)
{
    udp_rx_group_t* oldest = &g_groups[0];
    for (size_t i = 0; i < UDP_RX_GROUP_HISTORY; ++i)
    {
        if (g_groups[i].group_size != 0 && g_groups[i].group_id == group_id)
            return &g_groups[i];
        if (g_groups[i].group_size == 0) // empty slot
            oldest = &g_groups[i];
        else if (g_groups[i].group_id < oldest->group_id)
            oldest = &g_groups[i];
    }
    // Re-use oldest slot.
    memset(oldest, 0, sizeof(*oldest));
    oldest->group_id    = group_id;
    oldest->group_size  = group_size;
    return oldest;
}

static void decode_and_push(const uint8_t* opus_bytes, size_t opus_len,
                            uint32_t timestamp_sec, uint32_t timestamp_usec,
                            bool plc)
{
    if (xSemaphoreTake(g_codec_mx, pdMS_TO_TICKS(50)) != pdTRUE)
        return;
    if (!g_opus) { xSemaphoreGive(g_codec_mx); return; }

    const int frame_samples = UDP_RX_OPUS_FRAMES_PER_PACKET;
    const size_t bytes = (size_t)frame_samples * g_ch * (g_bits / 8);

    // Task-local scratch: single-threaded, keeps the 3.8 KB buffer off the
    // FreeRTOS task stack (which opus_decode already eats several KB of).
    static opus_int16 tmp_audio[960 * 2];

    int decoded = opus_decode(g_opus,
                              plc ? NULL : opus_bytes,
                              plc ? 0    : (opus_int32)opus_len,
                              tmp_audio, frame_samples,
                              plc ? 1 : 0);
    if (decoded <= 0) {
        xSemaphoreGive(g_codec_mx);
        return;
    }

    pcm_chunk_message_t* pcm = NULL;
    if (allocate_pcm_chunk_memory(&pcm, bytes) < 0) {
        xSemaphoreGive(g_codec_mx);
        return;
    }
    pcm->timestamp.sec  = (int32_t)timestamp_sec;
    pcm->timestamp.usec = (int32_t)timestamp_usec;

    // Same interleave as main.c handle_chunk_message: pack L/R into 32-bit
    // words so the I2S peripheral emits them in the right order.
    if (pcm->fragment && pcm->fragment->payload && g_ch == 2 && g_bits == 16) {
        volatile uint32_t* sample;
        uint32_t cnt = 0;
        for (size_t i = 0; i + 3 < bytes; i += 4) {
            sample = (volatile uint32_t*)(&(pcm->fragment->payload[i]));
            uint32_t tmp = (((uint32_t)tmp_audio[cnt]     << 16) & 0xFFFF0000) |
                           (((uint32_t)tmp_audio[cnt + 1] <<  0) & 0x0000FFFF);
            *sample = tmp;
            cnt += 2;
        }
    }

    insert_pcm_chunk(pcm);
    if (plc) g_stats.plc_invoked++;
    else     g_stats.decoded_ok++;

    xSemaphoreGive(g_codec_mx);
}

// Try to XOR-recover a single missing data packet in `grp`, given the
// received data slots in the reorder ring. Returns true on success.
static bool try_fec_recover(udp_rx_group_t* grp)
{
    if (!grp->parity_seen) return false;
    if (grp->data_seen != grp->group_size - 1) return false;

    // Find the hole: the data packet in [first_seq, first_seq+N) that we lack.
    if (!grp->expected_first_seq_known) return false;
    uint32_t miss_seq = UINT32_MAX;
    for (uint8_t i = 0; i < grp->group_size; ++i)
    {
        if (!(grp->seen_seqs_mask & (1u << i)))
            miss_seq = grp->expected_first_seq + i;
    }
    if (miss_seq == UINT32_MAX) return false;

    // XOR together parity + all received data payloads (padded to parity_len).
    // Static — single-threaded, avoids 1.5 KB on the task stack.
    static uint8_t recovered[UDP_RX_MAX_PAYLOAD];
    uint16_t plen = grp->parity_len;
    if (plen > UDP_RX_MAX_PAYLOAD) return false;
    memcpy(recovered, grp->parity_payload, plen);

    uint32_t length_xor = grp->parity_length_xor;
    uint32_t timestamp_sec = 0;
    uint32_t timestamp_usec = 0;
    for (size_t i = 0; i < UDP_RX_RING_SIZE; ++i)
    {
        udp_rx_slot_t* s = &g_ring[i];
        if (!s->valid) continue;
        if (s->fec_group != grp->group_id) continue;
        if (s->is_parity) continue;
        for (uint16_t b = 0; b < s->payload_len && b < plen; ++b)
            recovered[b] ^= s->payload[b];
        length_xor ^= (uint32_t)s->payload_len;
        // Approximate recovered packet's timestamp as the neighbor's — good
        // enough since all frames in a group are ~N×20 ms apart.
        timestamp_sec  = s->timestamp_sec;
        timestamp_usec = s->timestamp_usec;
    }
    uint16_t recovered_len = (uint16_t)length_xor;
    if (recovered_len == 0 || recovered_len > plen) return false;

    decode_and_push(recovered, recovered_len, timestamp_sec, timestamp_usec, false);
    g_stats.fec_recovered++;
    return true;
}

// Drain all ring slots whose seq <= `until_seq_exclusive`-1 in order. Missing
// seqs inside the window trigger PLC. Slots for seqs == up to and including
// end seq are cleared.
static void drain_ring_in_order(uint32_t until_seq_exclusive)
{
    while (g_have_first && (int32_t)(until_seq_exclusive - g_next_expected_seq) > 0)
    {
        uint32_t s = g_next_expected_seq;
        size_t   idx = s & UDP_RX_RING_MASK;
        udp_rx_slot_t* slot = &g_ring[idx];

        if (slot->valid && slot->seq == s)
        {
            if (!slot->is_parity)
                decode_and_push(slot->payload, slot->payload_len,
                                slot->timestamp_sec, slot->timestamp_usec, false);
            slot->valid = false;
        }
        else
        {
            // Lost — PLC one frame. Timestamp irrelevant: zero means
            // "unknown" to the player, which then schedules via its own
            // running clock.
            decode_and_push(NULL, 0, 0, 0, true);
            g_stats.drop_detected++;
        }
        g_next_expected_seq++;
    }
}

static void on_packet(const udp_audio_header_t* h, const uint8_t* payload, size_t payload_len)
{
    // Sanity
    if (payload_len > UDP_RX_MAX_PAYLOAD) { g_stats.rx_bad++; return; }
    if (h->fec_group_size == 0 || h->fec_group_size > 8) { g_stats.rx_bad++; return; }

    g_stats.rx_ok++;

    if (!g_have_first)
    {
        g_next_expected_seq = h->seq;
        g_have_first = true;
    }

    // If seq is way older than next_expected, drop silently.
    int32_t diff = (int32_t)(h->seq - g_next_expected_seq);
    if (diff < 0)
    {
        g_stats.reorder_late++;
        return;
    }
    // If seq is too far in the future, we risk ring collisions — flush head.
    if (diff >= UDP_RX_RING_SIZE)
    {
        drain_ring_in_order(h->seq - UDP_RX_RING_SIZE + 1);
    }

    // Store in ring.
    size_t idx = h->seq & UDP_RX_RING_MASK;
    udp_rx_slot_t* slot = &g_ring[idx];
    slot->valid        = true;
    slot->is_parity    = (h->flags & UDP_AUDIO_FLAG_IS_PARITY) != 0;
    slot->seq          = h->seq;
    slot->fec_group    = h->fec_group;
    slot->timestamp_usec = h->timestamp_us;
    // For data packets, `aux` carries the seconds portion; for parity it
    // carries the length-XOR and the slot's sec is unused.
    slot->timestamp_sec  = slot->is_parity ? 0 : h->aux;
    slot->payload_len  = h->payload_len;
    if (h->payload_len > 0)
        memcpy(slot->payload, payload, h->payload_len);

    // Update group state.
    udp_rx_group_t* grp = group_for(h->fec_group, h->fec_group_size);
    if (slot->is_parity)
    {
        grp->parity_seen       = true;
        grp->parity_len        = h->payload_len;
        grp->parity_length_xor = h->aux;
        if (h->payload_len > 0 && h->payload_len <= UDP_RX_MAX_PAYLOAD)
            memcpy(grp->parity_payload, payload, h->payload_len);
    }
    else
    {
        // First data packet of a group — remember its seq as base.
        if (!grp->expected_first_seq_known)
        {
            // The first data seq of a group equals parity_seq - N if parity
            // seen, else this packet's seq if this is the earliest data seen.
            // We assume data packets are contiguous and emitted before parity,
            // so the lowest data seq in the group is the group's first seq.
            grp->expected_first_seq = h->seq;
            grp->expected_first_seq_known = true;
        }
        else if (h->seq < grp->expected_first_seq)
            grp->expected_first_seq = h->seq;
        grp->data_seen++;
        uint32_t offset = h->seq - grp->expected_first_seq;
        if (offset < 32) grp->seen_seqs_mask |= (1u << offset);
    }

    // If a group is now recoverable, try FEC. Otherwise progress is made by
    // draining the ring in seq order in the main loop.
    (void)try_fec_recover(grp);

    // Drain everything we have in contiguous order up to seq+1.
    drain_ring_in_order(h->seq + 1);
}

// ── Registration / socket setup ─────────────────────────────────────────────

static void send_registration(void)
{
    if (g_sock < 0 || !g_cfg.client_id) return;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(g_cfg.server_port);
    if (inet_aton(g_cfg.server_ip, &dest.sin_addr) == 0) return;

    char buf[260];
    size_t n = 0;
    buf[n++] = 'U'; buf[n++] = 'D'; buf[n++] = 'P'; buf[n++] = 'R';
    size_t id_len = strnlen(g_cfg.client_id, 255);
    memcpy(&buf[n], g_cfg.client_id, id_len);
    n += id_len;

    int sent = sendto(g_sock, buf, n, 0, (struct sockaddr*)&dest, sizeof(dest));
    if (sent < 0)
        ESP_LOGW(TAG, "registration sendto errno=%d", errno);
    else
        ESP_LOGI(TAG, "registered '%s' -> %s:%u (%d bytes)",
                 g_cfg.client_id, g_cfg.server_ip, g_cfg.server_port, sent);
}

static int setup_socket(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int rcvbuf = 48 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port   = htons(g_cfg.local_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind :%u failed errno=%d", g_cfg.local_port, errno);
        close(s);
        return -1;
    }
    return s;
}

// ── Task ────────────────────────────────────────────────────────────────────

static void stats_log(void)
{
    static int64_t last_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_us < 1000000) return;
    last_us = now;
    ESP_LOGI(TAG,
             "rx=%"PRIu32" bad=%"PRIu32" drop=%"PRIu32" fec=%"PRIu32
             " plc=%"PRIu32" late=%"PRIu32" dec=%"PRIu32,
             g_stats.rx_ok, g_stats.rx_bad, g_stats.drop_detected,
             g_stats.fec_recovered, g_stats.plc_invoked,
             g_stats.reorder_late, g_stats.decoded_ok);
}

static void udp_rx_task(void* arg)
{
    (void)arg;
    g_sock = setup_socket();
    if (g_sock < 0) { vTaskDelete(NULL); return; }

    int64_t last_reg = 0;
    // Static — single-threaded receive path; keeps it off the task stack.
    static uint8_t buf[UDP_RX_MAX_PAYLOAD + UDP_AUDIO_HEADER_SIZE + 16];

    while (true)
    {
        int64_t now = esp_timer_get_time();
        if (now - last_reg > UDP_RX_REGISTER_PERIOD_MS * 1000LL)
        {
            send_registration();
            last_reg = now;
        }

        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromlen);
        if (n <= 0) {
            stats_log();
            continue;
        }
        if ((size_t)n < UDP_AUDIO_HEADER_SIZE) { g_stats.rx_bad++; continue; }

        udp_audio_header_t h;
        if (udp_audio_decode_header(buf, &h) != 0) { g_stats.rx_bad++; continue; }
        size_t want = (size_t)UDP_AUDIO_HEADER_SIZE + h.payload_len;
        if (want > (size_t)n) { g_stats.rx_bad++; continue; }

        on_packet(&h, buf + UDP_AUDIO_HEADER_SIZE, h.payload_len);
        stats_log();
    }
}

// ── Public API ──────────────────────────────────────────────────────────────

int udp_audio_rx_start(const udp_audio_rx_config_t* cfg)
{
    if (!cfg || !cfg->server_ip || !cfg->client_id) return -1;
    if (g_task) return 0;

    g_cfg = *cfg;
    g_codec_mx = xSemaphoreCreateMutex();
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_ring, 0, sizeof(g_ring));
    memset(g_groups, 0, sizeof(g_groups));

    // Pin to core 1 to keep WiFi/I2S (core 0 by default) responsive.
    // 16 KB — opus_decode internals alone consume several KB of stack; we
    // originally ran at 8 KB and tripped FreeRTOS' stack overflow detector.
    BaseType_t ok = xTaskCreatePinnedToCore(udp_rx_task, "udp_rx",
                                            16 * 1024, NULL, 10, &g_task, 1);
    return (ok == pdPASS) ? 0 : -2;
}

void udp_audio_rx_set_codec(codec_type_t codec, int32_t sample_rate,
                            uint8_t channels, uint8_t bits)
{
    if (!g_codec_mx) return;
    xSemaphoreTake(g_codec_mx, portMAX_DELAY);

    bool changed = (codec != g_codec) || (sample_rate != g_sr) ||
                   (channels != g_ch) || (bits != g_bits);
    g_codec = codec;
    g_sr    = sample_rate;
    g_ch    = channels;
    g_bits  = bits;

    if (changed || !g_opus)
    {
        if (g_opus) { opus_decoder_destroy(g_opus); g_opus = NULL; }
        if (codec == OPUS)
        {
            int err = 0;
            g_opus = opus_decoder_create(sample_rate, channels, &err);
            if (err != OPUS_OK)
            {
                ESP_LOGE(TAG, "opus_decoder_create err=%d", err);
                g_opus = NULL;
            }
            else
            {
                ESP_LOGI(TAG, "opus decoder ready sr=%"PRId32" ch=%u bits=%u",
                         sample_rate, channels, bits);
            }
        }
    }

    xSemaphoreGive(g_codec_mx);
}

void udp_audio_rx_get_stats(udp_audio_rx_stats_t* out)
{
    if (out) *out = g_stats;
}
