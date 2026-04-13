/* Play flac file by audio pipeline
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET || \
    CONFIG_ETH_USE_OPENETH
#include "eth_interface.h"
#endif

#include "nvs_flash.h"
#include "wifi_interface.h"

// Minimum ESP-IDF stuff only hardware abstraction stuff
#include <wifi_provisioning.h>

#include "board.h"
#include "es8388.h"
#include "esp_netif.h"
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "mdns.h"
#include "net_functions.h"

// Web socket server
// #include "websocket_if.h"
// #include "websocket_server.h"

#include <sys/time.h>

#include "driver/i2s_std.h"
#if CONFIG_USE_DSP_PROCESSOR
#include "dsp_processor.h"
#endif

// Opus decoder is implemented as a subcomponet from master git repo
#include "opus.h"

// flac decoder is implemented as a subcomponet from master git repo
#include "FLAC/stream_decoder.h"
#include "ota_server.h"
#include "player.h"
#include "snapcast.h"
#include "ui_http_server.h"

static bool isCachedChunk = false;
static uint32_t cachedBlocks = 0;

static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
    void *client_data);
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data);
static void metadata_callback(const FLAC__StreamDecoder *decoder,
                              const FLAC__StreamMetadata *metadata,
                              void *client_data);
static void error_callback(const FLAC__StreamDecoder *decoder,
                           FLAC__StreamDecoderErrorStatus status,
                           void *client_data);

static FLAC__StreamDecoder *flacDecoder = NULL;

const char *VERSION_STRING = "0.0.3";

#define HTTP_TASK_PRIORITY 9
#define HTTP_TASK_CORE_ID tskNO_AFFINITY

#define OTA_TASK_PRIORITY 6
#define OTA_TASK_CORE_ID tskNO_AFFINITY
// 1  // tskNO_AFFINITY

TaskHandle_t t_ota_task = NULL;
TaskHandle_t t_http_get_task = NULL;

#define FAST_SYNC_LATENCY_BUF 10000      // in µs
#define NORMAL_SYNC_LATENCY_BUF 1000000  // in µs

struct timeval tdif, tavg;

/* snapast parameters; configurable in menuconfig */
#define SNAPCAST_SERVER_USE_MDNS CONFIG_SNAPSERVER_USE_MDNS
#if !SNAPCAST_SERVER_USE_MDNS
#define SNAPCAST_SERVER_HOST CONFIG_SNAPSERVER_HOST
#define SNAPCAST_SERVER_PORT CONFIG_SNAPSERVER_PORT
#endif
#define SNAPCAST_CLIENT_NAME CONFIG_SNAPCLIENT_NAME
#define SNAPCAST_USE_SOFT_VOL CONFIG_SNAPCLIENT_USE_SOFT_VOL

/* Logging tag */
static const char *TAG = "SC";

// static QueueHandle_t playerChunkQueueHandle = NULL;
SemaphoreHandle_t timeSyncSemaphoreHandle = NULL;

#if CONFIG_USE_DSP_PROCESSOR
#if CONFIG_SNAPCLIENT_DSP_FLOW_STEREO
dspFlows_t dspFlow = dspfStereo;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASSBOOST
dspFlows_t dspFlow = dspfBassBoost;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BIAMP
dspFlows_t dspFlow = dspfBiamp;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASS_TREBLE_EQ
dspFlows_t dspFlow = dspfEQBassTreble;
#endif
#endif

typedef struct audioDACdata_s {
  bool mute;
  int volume;
} audioDACdata_t;

audioDACdata_t audioDAC_data;
static QueueHandle_t audioDACQHdl = NULL;
SemaphoreHandle_t audioDACSemaphore = NULL;

typedef struct decoderData_s {
  uint32_t type;  // should be SNAPCAST_MESSAGE_CODEC_HEADER
                  // or SNAPCAST_MESSAGE_WIRE_CHUNK
  uint8_t *inData;
  tv_t timestamp;
  uint8_t *outData;
  uint32_t bytes;
} decoderData_t;

void time_sync_msg_cb(void *args);

static char base_message_serialized[BASE_MESSAGE_SIZE];

static const esp_timer_create_args_t tSyncArgs = {
    .callback = &time_sync_msg_cb,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "tSyncMsg",
    .skip_unhandled_events = false};

struct netconn *lwipNetconn;

static int id_counter = 0;

// Clock sync: offset from esp_timer (boot time) to wall-clock time, set from
// the server's timestamp on first contact.
static bool clock_synced_from_server = false;
static int64_t boot_to_wall_offset_us = 0;

static inline int64_t get_sync_time_us(void) {
  return esp_timer_get_time() + boot_to_wall_offset_us;
}

// UDP streaming state
#define UDP_SERVER_PORT 1706
#define UDP_RECV_TASK_PRIORITY 8
#define UDP_RECV_TASK_CORE_ID tskNO_AFFINITY
#define UDP_RECV_BUF_SIZE 2048

// Jitter / reorder buffer. Packets are parked here briefly so that late or
// out-of-order datagrams can be placed back into sequence before the Opus
// decoder sees them. The decoder depends on in-order feed for best quality,
// and the player task wants monotonically advancing timestamps.
#define UDP_REORDER_SIZE 6        // 6 slots × ~20ms = 120ms reorder window
#define UDP_MAX_OPUS_LEN 1024     // plenty for typical Opus bitrates up to ~400kbps
#define UDP_JITTER_HOLD_MS 40     // give up waiting for a missing slot after this

typedef struct udp_reorder_slot {
  bool filled;
  uint16_t seq;
  int32_t ts_sec;
  int32_t ts_usec;
  uint32_t opus_len;
  TickType_t arrival;
  uint8_t opus_data[UDP_MAX_OPUS_LEN];
} udp_reorder_slot_t;

static struct netconn *udpNetconn = NULL;
static TaskHandle_t t_udp_recv_task = NULL;
static volatile bool udp_active = false;

static udp_reorder_slot_t *udp_reorder_buf = NULL;  // heap-allocated in task
static uint16_t udp_reorder_next_seq = 0;
static bool udp_reorder_initialized = false;

// Pre-allocated scratch buffer used by the Opus decode helper, sized to
// accommodate the worst-case Opus frame (60ms stereo 16-bit @ 48kHz ≈ 11.5KB).
// Allocated once at task start so the hot path stays free of malloc/free.
#define UDP_DECODE_SCRATCH_BYTES 16384
static opus_int16 *udp_decode_scratch = NULL;

static char udp_client_mac[18];  // MAC address for UDP re-registration

static OpusDecoder *opusDecoder = NULL;

static decoderData_t decoderChunk = {
    .type = SNAPCAST_MESSAGE_INVALID,
    .inData = NULL,
    .timestamp = {0, 0},
    .outData = NULL,
    .bytes = 0,
};

static decoderData_t pcmChunk = {
    .type = SNAPCAST_MESSAGE_INVALID,
    .inData = NULL,
    .timestamp = {0, 0},
    .outData = NULL,
    .bytes = 0,
};

/**
 *
 */
void time_sync_msg_cb(void *args) {
  base_message_t base_message_tx;
  //  struct timeval now;
  int64_t now;
  // time_message_t time_message_tx = {{0, 0}};
  int rc1;

  // causes kernel panic, which shouldn't happen though?
  // Isn't it called from timer task instead of ISR?
  // xSemaphoreGive(timeSyncSemaphoreHandle);

  //  result = gettimeofday(&now, NULL);
  ////  ESP_LOGI(TAG, "time of day: %d", (int32_t)now.tv_sec +
  ///(int32_t)now.tv_usec);
  //  if (result) {
  //    ESP_LOGI(TAG, "Failed to gettimeofday");
  //
  //    return;
  //  }

  uint8_t *p_pkt = (uint8_t *)malloc(BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);
  if (p_pkt == NULL) {
    ESP_LOGW(
        TAG,
        "%s: Failed to get memory for time sync message. Skipping this round.",
        __func__);

    return;
  }

  memset(p_pkt, 0, BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);

  base_message_tx.type = SNAPCAST_MESSAGE_TIME;
  base_message_tx.id = id_counter++;
  base_message_tx.refersTo = 0;
  base_message_tx.received.sec = 0;
  base_message_tx.received.usec = 0;
  now = get_sync_time_us();
  base_message_tx.sent.sec = now / 1000000;
  base_message_tx.sent.usec = now - base_message_tx.sent.sec * 1000000;
  base_message_tx.size = TIME_MESSAGE_SIZE;
  rc1 = base_message_serialize(&base_message_tx, (char *)&p_pkt[0],
                               BASE_MESSAGE_SIZE);
  if (rc1) {
    ESP_LOGE(TAG, "Failed to serialize base message for time");

    return;
  }

  //  memset(&time_message_tx, 0, sizeof(time_message_tx));
  //  result = time_message_serialize(&time_message_tx,
  //  &p_pkt[BASE_MESSAGE_SIZE],
  //                                  TIME_MESSAGE_SIZE);
  //  if (result) {
  //    ESP_LOGI(TAG, "Failed to serialize time message");
  //
  //    return;
  //  }

  rc1 = netconn_write(lwipNetconn, p_pkt, BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE,
                      NETCONN_NOCOPY);
  if (rc1 != ERR_OK) {
    ESP_LOGW(TAG, "error writing timesync msg");

    return;
  }

  free(p_pkt);

  //  ESP_LOGI(TAG, "%s: sent time sync message", __func__);

  //  xSemaphoreGiveFromISR(timeSyncSemaphoreHandle, &xHigherPriorityTaskWoken);
  //  if (xHigherPriorityTaskWoken) {
  //    portYIELD_FROM_ISR();
  //  }
}

/**
 *
 */
static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
    void *client_data) {
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;
  //  decoderData_t *flacData;

  (void)scSet;

  // xQueueReceive(decoderReadQHdl, &flacData, portMAX_DELAY);
  // if (xQueueReceive(decoderReadQHdl, &flacData, pdMS_TO_TICKS(100)))
  if (decoderChunk.inData) {
    //	   ESP_LOGI(TAG, "in flac read cb %ld %p", flacData->bytes,
    // flacData->inData);

    if (decoderChunk.bytes <= 0) {
      //	    free_flac_data(flacData);

      return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    isCachedChunk = false;

    //	  if (flacData->inData == NULL) {
    //	    free_flac_data(flacData);
    //
    //	    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    //	  }

    if (decoderChunk.bytes <= *bytes) {
      memcpy(buffer, decoderChunk.inData, decoderChunk.bytes);
      *bytes = decoderChunk.bytes;

      // ESP_LOGW(TAG, "read all flac inData %d", *bytes);

      free(decoderChunk.inData);
      decoderChunk.inData = NULL;
      decoderChunk.bytes = 0;
    } else {
      memcpy(buffer, decoderChunk.inData, *bytes);

      memmove(decoderChunk.inData, decoderChunk.inData + *bytes,
              decoderChunk.bytes - *bytes);
      decoderChunk.bytes -= *bytes;
      decoderChunk.inData =
          (uint8_t *)realloc(decoderChunk.inData, decoderChunk.bytes);

      // ESP_LOGW(TAG, "didn't read all flac inData %d", *bytes);
      //	    flacData->inData += *bytes;
      //	    flacData->bytes -= *bytes;
    }

    // free_flac_data(flacData);

    // xQueueSend (flacReadQHdl, &flacData, portMAX_DELAY);

    // xSemaphoreGive(decoderReadSemaphore);

    // ESP_LOGE(TAG, "%s: data processed", __func__);

    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  } else {
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
}

/**
 *
 */
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data) {
  size_t i;
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;

  size_t bytes = frame->header.blocksize * frame->header.channels *
                 frame->header.bits_per_sample / 8;

  (void)decoder;

  if (isCachedChunk) {
    cachedBlocks += frame->header.blocksize;
  }

  //  ESP_LOGI(TAG, "in flac write cb %ld %d, pcmChunk.bytes %ld",
  //  frame->header.blocksize, bytes, pcmChunk.bytes);

  if (frame->header.channels != scSet->ch) {
    ESP_LOGE(TAG,
             "ERROR: frame header reports different channel count %ld than "
             "previous metadata block %d",
             frame->header.channels, scSet->ch);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (frame->header.bits_per_sample != scSet->bits) {
    ESP_LOGE(TAG,
             "ERROR: frame header reports different bps %ld than previous "
             "metadata block %d",
             frame->header.bits_per_sample, scSet->bits);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (buffer[0] == NULL) {
    ESP_LOGE(TAG, "ERROR: buffer [0] is NULL");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (buffer[1] == NULL) {
    ESP_LOGE(TAG, "ERROR: buffer [1] is NULL");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  pcmChunk.outData =
      (uint8_t *)realloc(pcmChunk.outData, pcmChunk.bytes + bytes);
  if (!pcmChunk.outData) {
    ESP_LOGE(TAG, "%s, failed to allocate PCM chunk payload", __func__);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  for (i = 0; i < frame->header.blocksize; i++) {
    // write little endian
    pcmChunk.outData[pcmChunk.bytes + 4 * i] = (uint8_t)(buffer[0][i]);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 1] = (uint8_t)(buffer[0][i] >> 8);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 2] = (uint8_t)(buffer[1][i]);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 3] = (uint8_t)(buffer[1][i] >> 8);
  }

  pcmChunk.bytes += bytes;

  scSet->chkInFrames = frame->header.blocksize;

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/**
 *
 */
void metadata_callback(const FLAC__StreamDecoder *decoder,
                       const FLAC__StreamMetadata *metadata,
                       void *client_data) {
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;

  (void)decoder;

  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
    // ESP_LOGI(TAG, "in flac meta cb");

    // save for later
    scSet->sr = metadata->data.stream_info.sample_rate;
    scSet->ch = metadata->data.stream_info.channels;
    scSet->bits = metadata->data.stream_info.bits_per_sample;

    ESP_LOGI(TAG, "fLaC sampleformat: %ld:%d:%d", scSet->sr, scSet->bits,
             scSet->ch);

    // ESP_LOGE(TAG, "%s: data processed", __func__);
  }
}

/**
 *
 */
void error_callback(const FLAC__StreamDecoder *decoder,
                    FLAC__StreamDecoderErrorStatus status, void *client_data) {
  (void)decoder, (void)client_data;

  ESP_LOGE(TAG, "Got error callback: %s\n",
           FLAC__StreamDecoderErrorStatusString[status]);
}

/**
 *
 */
void init_snapcast(QueueHandle_t audioQHdl) {
  audioDACQHdl = audioQHdl;
  audioDACSemaphore = xSemaphoreCreateMutex();
  audioDAC_data.mute = true;
  audioDAC_data.volume = -1; // invalid volume to force update on first set
}

/**
 *
 */
void audio_set_mute(bool mute) {
  xSemaphoreTake(audioDACSemaphore, portMAX_DELAY);
  if (mute != audioDAC_data.mute) {
    audioDAC_data.mute = mute;
    xQueueOverwrite(audioDACQHdl, &audioDAC_data);
  }
  xSemaphoreGive(audioDACSemaphore);
}

/**
 *
 */
void audio_set_volume(int volume) {
  xSemaphoreTake(audioDACSemaphore, portMAX_DELAY);
  if (volume != audioDAC_data.volume) {
    audioDAC_data.volume = volume;
    xQueueOverwrite(audioDACQHdl, &audioDAC_data);
  }
  xSemaphoreGive(audioDACSemaphore);
}

/**
 *
 */
/**
 * Decode one Opus packet and push the resulting PCM chunk (plus any PLC/FEC
 * recovery frames) into the player queue. `scratch` is a caller-owned int16
 * buffer large enough to hold the decoded samples for a single frame; the
 * helper reuses it for PLC, FEC and normal decode paths so no malloc happens
 * in the hot path.
 */
static void udp_decode_and_insert(OpusDecoder *dec,
                                   snapcastSetting_t *scSet,
                                   opus_int16 *scratch,
                                   size_t scratch_bytes,
                                   const uint8_t *opus_data,
                                   uint32_t opus_len,
                                   int32_t ts_sec,
                                   int32_t ts_usec,
                                   uint16_t gap,
                                   uint32_t *pkts_decoded,
                                   uint32_t *pkts_fec_recovered) {
  int samples_per_frame = opus_packet_get_samples_per_frame(opus_data, scSet->sr);
  if (samples_per_frame <= 0) return;

  size_t pcm_bytes = (size_t)samples_per_frame * (scSet->ch * scSet->bits >> 3);
  if (pcm_bytes == 0 || pcm_bytes > scratch_bytes) return;

  int64_t chunk_dur_us = 1000000LL * samples_per_frame / scSet->sr;
  int64_t base_ts = (int64_t)ts_sec * 1000000LL + ts_usec;

  // PLC for packets lost earlier in the gap (all but the most recent).
  // Cap at 3 to avoid runaway recovery on large gaps.
  if (gap > 1) {
    uint16_t plc_frames = gap - 1;
    if (plc_frames > 3) plc_frames = 3;
    for (uint16_t i = 0; i < plc_frames; i++) {
      int plc_size = opus_decode(dec, NULL, 0, scratch, samples_per_frame, 0);
      if (plc_size <= 0) continue;
      pcm_chunk_message_t *plc_chunk = NULL;
      if (allocate_pcm_chunk_memory(&plc_chunk, pcm_bytes) < 0) continue;
      int64_t plc_ts = base_ts - (int64_t)(gap - i) * chunk_dur_us;
      plc_chunk->timestamp.sec = plc_ts / 1000000LL;
      plc_chunk->timestamp.usec = plc_ts % 1000000LL;
      if (plc_chunk->fragment->payload) {
        uint32_t cnt = 0;
        for (int j = 0; j < (int)pcm_bytes; j += 4) {
          volatile uint32_t *sample = (volatile uint32_t *)(&plc_chunk->fragment->payload[j]);
          uint32_t tmpData = (((uint32_t)scratch[cnt] << 16) & 0xFFFF0000) |
                             (((uint32_t)scratch[cnt + 1] << 0) & 0x0000FFFF);
          *sample = (volatile uint32_t)tmpData;
          cnt += 2;
        }
      }
      insert_pcm_chunk(plc_chunk);
    }
  }

  // FEC-decode the most recent lost packet from side information in the
  // current packet (Opus in-band FEC).
  if (gap > 0) {
    int fec_size = opus_decode(dec, opus_data, opus_len, scratch, samples_per_frame, 1);
    if (fec_size > 0) {
      pcm_chunk_message_t *fec_chunk = NULL;
      if (allocate_pcm_chunk_memory(&fec_chunk, pcm_bytes) >= 0) {
        int64_t fec_ts = base_ts - chunk_dur_us;
        fec_chunk->timestamp.sec = fec_ts / 1000000LL;
        fec_chunk->timestamp.usec = fec_ts % 1000000LL;
        if (fec_chunk->fragment->payload) {
          uint32_t cnt = 0;
          for (int j = 0; j < (int)pcm_bytes; j += 4) {
            volatile uint32_t *sample = (volatile uint32_t *)(&fec_chunk->fragment->payload[j]);
            uint32_t tmpData = (((uint32_t)scratch[cnt] << 16) & 0xFFFF0000) |
                               (((uint32_t)scratch[cnt + 1] << 0) & 0x0000FFFF);
            *sample = (volatile uint32_t)tmpData;
            cnt += 2;
          }
        }
        insert_pcm_chunk(fec_chunk);
      }
    }
    if (pkts_fec_recovered) *pkts_fec_recovered += gap;
  }

  // Normal decode of the in-order packet.
  int frame_size = opus_decode(dec, opus_data, opus_len, scratch, samples_per_frame, 0);
  if (frame_size > 0) {
    pcm_chunk_message_t *new_chunk = NULL;
    if (allocate_pcm_chunk_memory(&new_chunk, pcm_bytes) >= 0) {
      new_chunk->timestamp.sec = ts_sec;
      new_chunk->timestamp.usec = ts_usec;
      if (new_chunk->fragment->payload) {
        uint32_t cnt = 0;
        for (int j = 0; j < (int)pcm_bytes; j += 4) {
          volatile uint32_t *sample = (volatile uint32_t *)(&new_chunk->fragment->payload[j]);
          uint32_t tmpData = (((uint32_t)scratch[cnt] << 16) & 0xFFFF0000) |
                             (((uint32_t)scratch[cnt + 1] << 0) & 0x0000FFFF);
          *sample = (volatile uint32_t)tmpData;
          cnt += 2;
        }
      }
#if CONFIG_USE_DSP_PROCESSOR
      if (new_chunk->fragment->payload) {
        dsp_processor_worker(new_chunk->fragment->payload,
                             new_chunk->fragment->size, scSet->sr);
      }
#endif
      insert_pcm_chunk(new_chunk);
      if (pkts_decoded) (*pkts_decoded)++;
    }
  }
}

/**
 * Drain as many in-order packets from the reorder buffer as possible. When
 * `force_flush` is true (or the oldest buffered slot is older than the jitter
 * hold interval), the expected sequence skips forward to the first available
 * slot — the gap is surfaced to the decode helper which fills it with PLC/FEC.
 */
static void udp_reorder_drain(OpusDecoder *dec,
                               snapcastSetting_t *scSet,
                               opus_int16 *scratch,
                               size_t scratch_bytes,
                               bool force_flush,
                               uint32_t *pkts_decoded,
                               uint32_t *pkts_fec_recovered) {
  if (!udp_reorder_buf) return;
  TickType_t now = xTaskGetTickCount();

  while (1) {
    int idx = udp_reorder_next_seq % UDP_REORDER_SIZE;
    udp_reorder_slot_t *slot = &udp_reorder_buf[idx];

    if (slot->filled && slot->seq == udp_reorder_next_seq) {
      udp_decode_and_insert(dec, scSet, scratch, scratch_bytes,
                            slot->opus_data, slot->opus_len,
                            slot->ts_sec, slot->ts_usec, 0,
                            pkts_decoded, pkts_fec_recovered);
      slot->filled = false;
      udp_reorder_next_seq++;
      continue;
    }

    // Expected slot is empty: do we keep waiting, or skip forward?
    bool any_aged = false;
    for (int i = 0; i < UDP_REORDER_SIZE; i++) {
      if (udp_reorder_buf[i].filled &&
          (now - udp_reorder_buf[i].arrival) >= pdMS_TO_TICKS(UDP_JITTER_HOLD_MS)) {
        any_aged = true;
        break;
      }
    }
    if (!force_flush && !any_aged) return;

    // Find the nearest filled slot ahead of next_seq and advance to it.
    uint16_t min_offset = 0xFFFF;
    for (int i = 0; i < UDP_REORDER_SIZE; i++) {
      if (!udp_reorder_buf[i].filled) continue;
      uint16_t off = (uint16_t)(udp_reorder_buf[i].seq - udp_reorder_next_seq);
      if (off < UDP_REORDER_SIZE && off < min_offset) min_offset = off;
    }
    if (min_offset == 0xFFFF) return;  // no forward slot available

    udp_reorder_next_seq += min_offset;
    int idx2 = udp_reorder_next_seq % UDP_REORDER_SIZE;
    udp_reorder_slot_t *s2 = &udp_reorder_buf[idx2];
    if (!s2->filled) return;  // shouldn't happen, but be defensive
    udp_decode_and_insert(dec, scSet, scratch, scratch_bytes,
                          s2->opus_data, s2->opus_len,
                          s2->ts_sec, s2->ts_usec, min_offset,
                          pkts_decoded, pkts_fec_recovered);
    s2->filled = false;
    udp_reorder_next_seq++;
  }
}

/**
 * UDP receive task: receives WireChunk datagrams from the server,
 * stashes them in a jitter/reorder buffer, then drains them in sequence
 * through the Opus decode helper (which handles FEC/PLC).
 */
static void udp_recv_task(void *pvParameters) {
  snapcastSetting_t *scSetPtr = (snapcastSetting_t *)pvParameters;
  struct netbuf *buf = NULL;
  int rc;

  // Stats counters (logged periodically to avoid serial spam)
  uint32_t udp_pkts_received = 0;
  uint32_t udp_pkts_decoded = 0;
  uint32_t udp_pkts_dropped = 0;
  uint32_t udp_pkts_fec_recovered = 0;
  TickType_t last_stats_tick = xTaskGetTickCount();
  const TickType_t stats_interval = pdMS_TO_TICKS(10000);  // log every 10s

  // Create a dedicated Opus decoder for this task (the shared opusDecoder
  // is not thread-safe and may be used concurrently by the TCP task)
  int opus_err = 0;
  OpusDecoder *udpOpusDecoder = opus_decoder_create(scSetPtr->sr, scSetPtr->ch, &opus_err);
  if (opus_err != 0 || udpOpusDecoder == NULL) {
    ESP_LOGE(TAG, "UDP: failed to create opus decoder: %d", opus_err);
    vTaskDelete(NULL);
    return;
  }

  // Allocate reorder buffer + decode scratch once. Freed on task exit.
  udp_reorder_buf = (udp_reorder_slot_t *)calloc(UDP_REORDER_SIZE, sizeof(udp_reorder_slot_t));
  udp_decode_scratch = (opus_int16 *)malloc(UDP_DECODE_SCRATCH_BYTES);
  if (!udp_reorder_buf || !udp_decode_scratch) {
    ESP_LOGE(TAG, "UDP: failed to allocate reorder/scratch buffers");
    if (udp_reorder_buf) { free(udp_reorder_buf); udp_reorder_buf = NULL; }
    if (udp_decode_scratch) { free(udp_decode_scratch); udp_decode_scratch = NULL; }
    opus_decoder_destroy(udpOpusDecoder);
    vTaskDelete(NULL);
    return;
  }
  udp_reorder_initialized = false;
  udp_reorder_next_seq = 0;

  ESP_LOGI(TAG, "UDP recv task started (opus decoder: %ldHz %dch)", scSetPtr->sr, scSetPtr->ch);

  bool settings_sent = false;

  // Set receive timeout so the loop can run re-registration even when
  // no packets are arriving (e.g. after server restart or NAT expiry).
  netconn_set_recvtimeout(udpNetconn, 1000);  // 1 second

  // Periodic re-registration to handle packet loss, NAT timeout, or server restart
  TickType_t last_reg_tick = xTaskGetTickCount();
  const TickType_t reg_interval = pdMS_TO_TICKS(5000);  // re-register every 5s

  while (udp_active) {
    // Periodically re-send registration packet
    TickType_t cur_tick = xTaskGetTickCount();
    if ((cur_tick - last_reg_tick) >= reg_interval) {
      uint16_t mac_len = strlen(udp_client_mac);
      struct netbuf *reg_buf = netbuf_new();
      if (reg_buf) {
        uint8_t reg_pkt[2 + 18];  // 2-byte prefix + max MAC string
        reg_pkt[0] = mac_len & 0xFF;
        reg_pkt[1] = (mac_len >> 8) & 0xFF;
        memcpy(reg_pkt + 2, udp_client_mac, mac_len);
        netbuf_ref(reg_buf, reg_pkt, 2 + mac_len);
        netconn_send(udpNetconn, reg_buf);
        netbuf_delete(reg_buf);
      }
      last_reg_tick = cur_tick;
    }

    rc = netconn_recv(udpNetconn, &buf);
    if (rc != ERR_OK) {
      if (!udp_active) break;
      if (buf) netbuf_delete(buf);
      buf = NULL;
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    void *data;
    uint16_t data_len;
    netbuf_data(buf, &data, &data_len);

    udp_pkts_received++;

    // Periodic stats log
    TickType_t now_tick = xTaskGetTickCount();
    if ((now_tick - last_stats_tick) >= stats_interval) {
      ESP_LOGI(TAG, "UDP stats: recv=%lu decoded=%lu dropped=%lu fec=%lu",
               (unsigned long)udp_pkts_received, (unsigned long)udp_pkts_decoded,
               (unsigned long)udp_pkts_dropped, (unsigned long)udp_pkts_fec_recovered);
      last_stats_tick = now_tick;
    }

    // A UDP datagram must contain at least a base_message header (26 bytes)
    // + WireChunk header (12 bytes: timestamp 8 + size 4)
    if (data_len < BASE_MESSAGE_SIZE + 12) {
      udp_pkts_dropped++;
      netbuf_delete(buf);
      buf = NULL;
      continue;
    }

    uint8_t *pkt = (uint8_t *)data;

    // Parse base_message header (little-endian)
    uint16_t msg_type = pkt[0] | (pkt[1] << 8);
    uint16_t msg_id = pkt[2] | (pkt[3] << 8);  // sequence number

    // We only handle WireChunk (type 2)
    if (msg_type != SNAPCAST_MESSAGE_WIRE_CHUNK) {
      netbuf_delete(buf);
      buf = NULL;
      continue;
    }

    // Parse WireChunk header after base_message (26 bytes)
    uint8_t *wc = pkt + BASE_MESSAGE_SIZE;
    int32_t ts_sec = wc[0] | (wc[1] << 8) | (wc[2] << 16) | (wc[3] << 24);
    int32_t ts_usec = wc[4] | (wc[5] << 8) | (wc[6] << 16) | (wc[7] << 24);
    uint32_t payload_size = wc[8] | (wc[9] << 8) | (wc[10] << 16) | (wc[11] << 24);

    uint8_t *opus_data = wc + 12;
    uint32_t opus_len = payload_size;

    // Bounds check
    if (BASE_MESSAGE_SIZE + 12 + opus_len > data_len) {
      udp_pkts_dropped++;
      netbuf_delete(buf);
      buf = NULL;
      continue;
    }

    if (scSetPtr->sr == 0 || scSetPtr->ch == 0 || scSetPtr->bits == 0) {
      udp_pkts_dropped++;
      netbuf_delete(buf);
      buf = NULL;
      continue;
    }

    // Push settings to the player once we can peek the first valid frame size.
    // (Doesn't depend on a successful decode, so do it before the reorder
    //  buffer possibly parks the packet.)
    if (!settings_sent) {
      int spf = opus_packet_get_samples_per_frame(opus_data, scSetPtr->sr);
      if (spf > 0) {
        scSetPtr->chkInFrames = spf;
        if (player_send_snapcast_setting(scSetPtr) == pdPASS) {
          ESP_LOGI(TAG,
                   "UDP: got settings and notified player_task (chkInFrames=%d)",
                   spf);
        }
        settings_sent = true;
      }
    }

    if (opus_len == 0 || opus_len > UDP_MAX_OPUS_LEN) {
      udp_pkts_dropped++;
      netbuf_delete(buf);
      buf = NULL;
      continue;
    }

    // Reorder buffer bookkeeping.
    if (!udp_reorder_initialized) {
      udp_reorder_next_seq = msg_id;
      udp_reorder_initialized = true;
    }

    int16_t diff = (int16_t)(msg_id - udp_reorder_next_seq);
    if (diff < 0) {
      // Packet arrived after we already skipped past its sequence number.
      udp_pkts_dropped++;
      netbuf_delete(buf);
      buf = NULL;
      continue;
    }
    if (diff >= UDP_REORDER_SIZE) {
      // Packet is beyond the reorder window. Drain anything still parked,
      // then decode the new packet directly with an explicit gap so the
      // helper runs PLC/FEC and keeps the Opus decoder state aligned with
      // the advancing sequence number. Skipping straight to `msg_id` here
      // (as an earlier version did) would desync the decoder and leave a
      // silent hole in the player's timeline.
      udp_reorder_drain(udpOpusDecoder, scSetPtr, udp_decode_scratch,
                        UDP_DECODE_SCRATCH_BYTES, true,
                        &udp_pkts_decoded, &udp_pkts_fec_recovered);

      uint16_t skipped = (uint16_t)(msg_id - udp_reorder_next_seq);
      udp_decode_and_insert(udpOpusDecoder, scSetPtr, udp_decode_scratch,
                            UDP_DECODE_SCRATCH_BYTES,
                            opus_data, opus_len, ts_sec, ts_usec,
                            skipped,
                            &udp_pkts_decoded, &udp_pkts_fec_recovered);
      udp_reorder_next_seq = msg_id + 1;

      netbuf_delete(buf);
      buf = NULL;
      continue;
    }

    udp_reorder_slot_t *slot = &udp_reorder_buf[msg_id % UDP_REORDER_SIZE];
    if (slot->filled && slot->seq == msg_id) {
      // Duplicate — ignore.
      udp_pkts_dropped++;
      netbuf_delete(buf);
      buf = NULL;
      continue;
    }
    slot->filled = true;
    slot->seq = msg_id;
    slot->ts_sec = ts_sec;
    slot->ts_usec = ts_usec;
    slot->opus_len = opus_len;
    slot->arrival = xTaskGetTickCount();
    memcpy(slot->opus_data, opus_data, opus_len);

    udp_reorder_drain(udpOpusDecoder, scSetPtr, udp_decode_scratch,
                      UDP_DECODE_SCRATCH_BYTES, false,
                      &udp_pkts_decoded, &udp_pkts_fec_recovered);

    netbuf_delete(buf);
    buf = NULL;
  }

  // Final drain of any packets still sitting in the reorder buffer so they
  // don't silently disappear on shutdown.
  udp_reorder_drain(udpOpusDecoder, scSetPtr, udp_decode_scratch,
                    UDP_DECODE_SCRATCH_BYTES, true,
                    &udp_pkts_decoded, &udp_pkts_fec_recovered);

  if (udpOpusDecoder) {
    opus_decoder_destroy(udpOpusDecoder);
  }
  if (udp_reorder_buf) {
    free(udp_reorder_buf);
    udp_reorder_buf = NULL;
  }
  if (udp_decode_scratch) {
    free(udp_decode_scratch);
    udp_decode_scratch = NULL;
  }
  udp_reorder_initialized = false;
  ESP_LOGI(TAG, "UDP recv task exiting");
  vTaskDelete(NULL);
}

static void http_get_task(void *pvParameters) {
  char *start;
  base_message_t base_message_rx;
  hello_message_t hello_message;
  wire_chunk_message_t wire_chnk = {{0, 0}, 0, NULL};
  char *hello_message_serialized = NULL;
  int result;
  int64_t now, trx, tdif, ttx;
  time_message_t time_message_rx = {{0, 0}};
  int64_t tmpDiffToServer;
  int64_t lastTimeSync = 0;
  esp_timer_handle_t timeSyncMessageTimer = NULL;
  esp_err_t err = 0;
  server_settings_message_t server_settings_message;
  bool received_header = false;
  mdns_result_t *r;
  codec_type_t codec = NONE;
  snapcastSetting_t scSet;
  pcm_chunk_message_t *pcmData = NULL;
  ip_addr_t remote_ip;
  uint16_t remotePort = 0;
  int rc1 = ERR_OK, rc2 = ERR_OK;
  struct netbuf *firstNetBuf = NULL;
  uint16_t len;
  uint64_t timeout = FAST_SYNC_LATENCY_BUF;
  char *codecString = NULL;
  char *codecPayload = NULL;
  char *serverSettingsString = NULL;

  // create a timer to send time sync messages every x µs
  esp_timer_create(&tSyncArgs, &timeSyncMessageTimer);

#if CONFIG_SNAPCLIENT_USE_MDNS
  ESP_LOGI(TAG, "Enable mdns");
  mdns_init();
#endif

  while (1) {
    // do some house keeping
    {
      received_header = false;
      clock_synced_from_server = false;

      timeout = FAST_SYNC_LATENCY_BUF;

      esp_timer_stop(timeSyncMessageTimer);

      // Stop UDP task FIRST, before destroying the opus decoder it uses
      if (udp_active) {
        udp_active = false;
        vTaskDelay(pdMS_TO_TICKS(100));  // let UDP task exit
      }
      if (t_udp_recv_task) {
        vTaskDelete(t_udp_recv_task);
        t_udp_recv_task = NULL;
      }
      if (udpNetconn) {
        netconn_delete(udpNetconn);
        udpNetconn = NULL;
      }
      udp_reorder_initialized = false;
      udp_reorder_next_seq = 0;

      // Now safe to destroy decoders
      if (opusDecoder != NULL) {
        opus_decoder_destroy(opusDecoder);
        opusDecoder = NULL;
      }

      if (flacDecoder != NULL) {
        FLAC__stream_decoder_finish(flacDecoder);
        FLAC__stream_decoder_delete(flacDecoder);
        flacDecoder = NULL;
      }

      if (decoderChunk.inData) {
        free(decoderChunk.inData);
        decoderChunk.inData = NULL;
      }

      if (decoderChunk.outData) {
        free(decoderChunk.outData);
        decoderChunk.outData = NULL;
      }

      if (codecString) {
        free(codecString);
        codecString = NULL;
      }

      if (codecPayload) {
        free(codecPayload);
        codecPayload = NULL;
      }

      if (codecPayload) {
        free(serverSettingsString);
        serverSettingsString = NULL;
      }
    }

#if SNAPCAST_SERVER_USE_MDNS
    // Find snapcast server
    // Connect to first snapcast server found
    r = NULL;
    err = 0;
    while (!r || err) {
      ESP_LOGI(TAG, "Lookup snapcast service on network");
      esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", 3000, 20, &r);
      if (err) {
        ESP_LOGE(TAG, "Query Failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
      }

      if (!r) {
        ESP_LOGW(TAG, "No results found!");
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    mdns_ip_addr_t *a = r->addr;
    if (a) {
      ip_addr_copy(remote_ip, (a->addr));
      remote_ip.type = a->addr.type;
      remotePort = r->port;
      ESP_LOGI(TAG, "Found %s:%d", ipaddr_ntoa(&remote_ip), remotePort);

      mdns_query_results_free(r);
    } else {
      mdns_query_results_free(r);

      ESP_LOGW(TAG, "No IP found in MDNS query");

      continue;
    }
#else
    // configure a failsafe snapserver according to CONFIG values
    struct sockaddr_in servaddr;

    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET, SNAPCAST_SERVER_HOST, &(servaddr.sin_addr.s_addr));
    servaddr.sin_port = htons(SNAPCAST_SERVER_PORT);

#if LWIP_IPV6
    inet_pton(AF_INET, SNAPCAST_SERVER_HOST, &(remote_ip.u_addr.ip4.addr));
    remote_ip.type = IPADDR_TYPE_V4;
#else
    inet_pton(AF_INET, SNAPCAST_SERVER_HOST, &(remote_ip.addr));
#endif
    remotePort = SNAPCAST_SERVER_PORT;

    ESP_LOGI(TAG, "try connecting to static configuration %s:%d",
             ipaddr_ntoa(&remote_ip), remotePort);
#endif

    if (lwipNetconn != NULL) {
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;
    }

    lwipNetconn = netconn_new(NETCONN_TCP);
    if (lwipNetconn == NULL) {
      ESP_LOGE(TAG, "can't create netconn");

      continue;
    }

    rc1 = netconn_bind(lwipNetconn, IPADDR_ANY, 0);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "can't bind local IP");
    }

    rc2 = netconn_connect(lwipNetconn, &remote_ip, remotePort);
    if (rc2 != ERR_OK) {
      ESP_LOGE(TAG, "can't connect to remote %s:%d, err %d",
               ipaddr_ntoa(&remote_ip), remotePort, rc2);
    }

    if (rc1 != ERR_OK || rc2 != ERR_OK) {
      netconn_close(lwipNetconn);
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;

      continue;
    }

    ESP_LOGI(TAG, "netconn connected");

    if (reset_latency_buffer() < 0) {
      ESP_LOGE(TAG,
               "reset_diff_buffer: couldn't reset median filter long. STOP");
      return;
    }

    char mac_address[18];
    uint8_t base_mac[6];
    // Get MAC address for WiFi station
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    esp_read_mac(base_mac, ESP_MAC_ETH);
#else
    esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
#endif
    sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
            base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);

    now = get_sync_time_us();

    // init base message
    base_message_rx.type = SNAPCAST_MESSAGE_HELLO;
    base_message_rx.id = id_counter++;
    base_message_rx.refersTo = 0x0000;
    base_message_rx.sent.sec = now / 1000000;
    base_message_rx.sent.usec = now - base_message_rx.sent.sec * 1000000;
    base_message_rx.received.sec = 0;
    base_message_rx.received.usec = 0;
    base_message_rx.size = 0x00000000;

    // init hello message
    hello_message.mac = mac_address;
    hello_message.hostname = SNAPCAST_CLIENT_NAME;
    hello_message.version = (char *)VERSION_STRING;
    hello_message.client_name = "libsnapcast";
    hello_message.os = "esp32";
    hello_message.arch = "xtensa";
    hello_message.instance = 1;
    hello_message.id = mac_address;
    hello_message.protocol_version = 2;

    if (hello_message_serialized == NULL) {
      hello_message_serialized = hello_message_serialize(
          &hello_message, (size_t *)&(base_message_rx.size));
      if (!hello_message_serialized) {
        ESP_LOGE(TAG, "Failed to serialize hello message");
        return;
      }
    }

    result = base_message_serialize(&base_message_rx, base_message_serialized,
                                    BASE_MESSAGE_SIZE);
    if (result) {
      ESP_LOGE(TAG, "Failed to serialize base message");
      return;
    }

    rc1 = netconn_write(lwipNetconn, base_message_serialized, BASE_MESSAGE_SIZE,
                        NETCONN_NOCOPY);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "netconn failed to send base message");

      continue;
    }
    rc1 = netconn_write(lwipNetconn, hello_message_serialized,
                        base_message_rx.size, NETCONN_NOCOPY);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "netconn failed to send hello message");

      continue;
    }

    ESP_LOGI(TAG, "netconn sent hello message");

    free(hello_message_serialized);
    hello_message_serialized = NULL;

    // init default setting
    scSet.buf_ms = 500;
    scSet.codec = NONE;
    scSet.bits = 16;
    scSet.ch = 2;
    scSet.sr = 44100;
    scSet.chkInFrames = 0;
    scSet.volume = 0;
    scSet.muted = true;

    uint64_t startTime, endTime;
    //    size_t currentPos = 0;
    size_t typedMsgCurrentPos = 0;
    uint32_t typedMsgLen = 0;
    uint32_t offset = 0;
    uint32_t payloadOffset = 0;
    uint32_t tmpData = 0;
    int32_t payloadDataShift = 0;

#define BASE_MESSAGE_STATE 0
#define TYPED_MESSAGE_STATE 1

    // 0 ... base message, 1 ... typed message
    uint32_t state = BASE_MESSAGE_STATE;
    uint32_t internalState = 0;

    firstNetBuf = NULL;

    while (1) {
      rc2 = netconn_recv(lwipNetconn, &firstNetBuf);
      if (rc2 != ERR_OK) {
        if (firstNetBuf != NULL) {
          netbuf_delete(firstNetBuf);
          firstNetBuf = NULL;
        }

        if (rc2 == ERR_TIMEOUT) {
          continue;  // timeout is recoverable, retry
        }

        // Any connection-level error: reconnect
        ESP_LOGW(TAG, "TCP recv error %d, reconnecting", rc2);
        netconn_close(lwipNetconn);
        break;
      }

      // now parse the data
      netbuf_first(firstNetBuf);
      do {
        // currentPos = 0;

        rc1 = netbuf_data(firstNetBuf, (void **)&start, &len);
        if (rc1 == ERR_OK) {
          // ESP_LOGI (TAG, "netconn rx,"
          // "data len: %d, %d", len, netbuf_len(firstNetBuf) -
          // currentPos);
        } else {
          ESP_LOGE(TAG, "netconn rx, couldn't get data");

          continue;
        }

        while (len > 0) {
          rc1 = ERR_OK;  // probably not necessary

          switch (state) {
            // decode base message
            case BASE_MESSAGE_STATE: {
              switch (internalState) {
                case 0:
                  base_message_rx.type = *start & 0xFF;
                  internalState++;
                  break;

                case 1:
                  base_message_rx.type |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 2:
                  base_message_rx.id = *start & 0xFF;
                  internalState++;
                  break;

                case 3:
                  base_message_rx.id |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 4:
                  base_message_rx.refersTo = *start & 0xFF;
                  internalState++;
                  break;

                case 5:
                  base_message_rx.refersTo |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 6:
                  base_message_rx.sent.sec = *start & 0xFF;
                  internalState++;
                  break;

                case 7:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 8:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 9:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 10:
                  base_message_rx.sent.usec = *start & 0xFF;
                  internalState++;
                  break;

                case 11:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 12:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 13:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 14:
                  base_message_rx.received.sec = *start & 0xFF;
                  internalState++;
                  break;

                case 15:
                  base_message_rx.received.sec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 16:
                  base_message_rx.received.sec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 17:
                  base_message_rx.received.sec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 18:
                  base_message_rx.received.usec = *start & 0xFF;
                  internalState++;
                  break;

                case 19:
                  base_message_rx.received.usec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 20:
                  base_message_rx.received.usec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 21:
                  base_message_rx.received.usec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 22:
                  base_message_rx.size = *start & 0xFF;
                  internalState++;
                  break;

                case 23:
                  base_message_rx.size |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 24:
                  base_message_rx.size |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 25:
                  base_message_rx.size |= (*start & 0xFF) << 24;
                  internalState = 0;

                  now = get_sync_time_us();

                  base_message_rx.received.sec = now / 1000000;
                  base_message_rx.received.usec =
                      now - base_message_rx.received.sec * 1000000;

                  typedMsgCurrentPos = 0;

                  //                   ESP_LOGI(TAG,"BM type %d ts %d.%d",
                  //                   base_message_rx.type,
                  //                   base_message_rx.received.sec,
                  //                   base_message_rx.received.usec);
                  // ESP_LOGI(TAG,"%d, %d.%d", base_message_rx.type,
                  //                   base_message_rx.received.sec,
                  //                   base_message_rx.received.usec);
                  // ESP_LOGI(TAG,"%d, %llu", base_message_rx.type,
                  //		   1000000ULL * base_message_rx.received.sec +
                  // base_message_rx.received.usec);

                  state = TYPED_MESSAGE_STATE;
                  break;
              }

              // currentPos++;++;
              len--;
              start++;

              break;
            }

            // decode typed message
            case TYPED_MESSAGE_STATE: {
              switch (base_message_rx.type) {
                case SNAPCAST_MESSAGE_WIRE_CHUNK: {
                  switch (internalState) {
                    case 0: {
                      wire_chnk.timestamp.sec = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 24;

                      // ESP_LOGI(TAG,
                      // "wire chunk time sec: %d",
                      // wire_chnk.timestamp.sec);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      wire_chnk.timestamp.usec = (*start & 0xFF);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 5: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 6: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 7: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 24;

                      // ESP_LOGI(TAG,
                      // "wire chunk time usec: %d",
                      // wire_chnk.timestamp.usec);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 8: {
                      wire_chnk.size = (*start & 0xFF);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 9: {
                      wire_chnk.size |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 10: {
                      wire_chnk.size |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 11: {
                      wire_chnk.size |= (*start & 0xFF) << 24;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      // TODO: we could use wire chunk directly maybe?
                      decoderChunk.bytes = wire_chnk.size;
                      while (!decoderChunk.inData) {
                        decoderChunk.inData =
                            (uint8_t *)malloc(decoderChunk.bytes);
                        if (!decoderChunk.inData) {
                          ESP_LOGW(TAG,
                                   "malloc decoderChunk.inData failed, wait "
                                   "1ms and try again");

                          vTaskDelay(pdMS_TO_TICKS(1));
                        }
                      }

                      payloadOffset = 0;

#if 0
                       ESP_LOGI(TAG, "chunk with size: %u, at time %ld.%ld",
                    		   	   	   	   	 wire_chnk.size,
                                             wire_chnk.timestamp.sec,
                                             wire_chnk.timestamp.usec);
#endif

                      if (len == 0) {
                        break;
                      }
                    }

                    case 12: {
                      size_t tmp_size;

                      if ((base_message_rx.size - typedMsgCurrentPos) <= len) {
                        tmp_size = base_message_rx.size - typedMsgCurrentPos;
                      } else {
                        tmp_size = len;
                      }

                      if (received_header == true) {
                        switch (codec) {
                          case OPUS:
                          case FLAC: {
                            memcpy(&decoderChunk.inData[payloadOffset], start,
                                   tmp_size);
                            payloadOffset += tmp_size;
                            decoderChunk.outData = NULL;
                            decoderChunk.type = SNAPCAST_MESSAGE_WIRE_CHUNK;

                            break;
                          }

                          case PCM: {
                            size_t _tmp = tmp_size;

                            offset = 0;

                            if (pcmData == NULL) {
                              if (allocate_pcm_chunk_memory(
                                      &pcmData, wire_chnk.size) < 0) {
                                pcmData = NULL;
                              }

                              tmpData = 0;
                              payloadDataShift = 3;
                              payloadOffset = 0;
                            }

                            while (_tmp--) {
                              tmpData |= ((uint32_t)start[offset++]
                                          << (8 * payloadDataShift));

                              payloadDataShift--;
                              if (payloadDataShift < 0) {
                                payloadDataShift = 3;

                                if ((pcmData) && (pcmData->fragment->payload)) {
                                  volatile uint32_t *sample;
                                  uint8_t dummy1;
                                  uint32_t dummy2 = 0;

                                  // TODO: find a more
                                  // clever way to do this,
                                  // best would be to
                                  // actually store it the
                                  // right way in the first
                                  // place
                                  dummy1 = tmpData >> 24;
                                  dummy2 |= (uint32_t)dummy1 << 16;
                                  dummy1 = tmpData >> 16;
                                  dummy2 |= (uint32_t)dummy1 << 24;
                                  dummy1 = tmpData >> 8;
                                  dummy2 |= (uint32_t)dummy1 << 0;
                                  dummy1 = tmpData >> 0;
                                  dummy2 |= (uint32_t)dummy1 << 8;
                                  tmpData = dummy2;

                                  sample = (volatile uint32_t *)(&(
                                      pcmData->fragment
                                          ->payload[payloadOffset]));
                                  *sample = (volatile uint32_t)tmpData;

                                  payloadOffset += 4;
                                }

                                tmpData = 0;
                              }
                            }

                            break;
                          }

                          default: {
                            ESP_LOGE(TAG, "Decoder (1) not supported");

                            return;

                            break;
                          }
                        }
                      }

                      typedMsgCurrentPos += tmp_size;
                      start += tmp_size;
                      // currentPos += tmp_size;
                      len -= tmp_size;

                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        if (received_header == true) {
                          switch (codec) {
                            case OPUS: {
                              // When UDP is active, skip TCP Opus decoding
                              // (audio arrives via UDP instead)
                              if (udp_active) {
                                free(decoderChunk.inData);
                                decoderChunk.inData = NULL;
                                break;
                              }

                              int frame_size = -1;
                              int samples_per_frame;
                              opus_int16 *audio = NULL;

                              samples_per_frame =
                                  opus_packet_get_samples_per_frame(
                                      decoderChunk.inData, scSet.sr);
                              if (samples_per_frame < 0) {
                                ESP_LOGE(TAG,
                                         "couldn't get samples per frame count "
                                         "of packet");
                              }

                              scSet.chkInFrames = samples_per_frame;

                              // ESP_LOGW(TAG, "%d, %llu, %llu",
                              // samples_per_frame, 1000000ULL *
                              // samples_per_frame / scSet.sr,
                              // 1000000ULL *
                              // wire_chnk.timestamp.sec +
                              // wire_chnk.timestamp.usec);

                              // ESP_LOGW(TAG, "got OPUS decoded chunk size: %ld
                              // " "frames from encoded chunk with size %d,
                              // allocated audio buffer %d", scSet.chkInFrames,
                              // wire_chnk.size, samples_per_frame);

                              size_t bytes;
                              do {
                                bytes = samples_per_frame *
                                        (scSet.ch * scSet.bits >> 3);

                                while ((audio = (opus_int16 *)realloc(
                                            audio, bytes)) == NULL) {
                                  ESP_LOGE(TAG,
                                           "couldn't realloc memory for OPUS "
                                           "audio %d",
                                           bytes);

                                  vTaskDelay(pdMS_TO_TICKS(1));
                                }

                                frame_size = opus_decode(
                                    opusDecoder, decoderChunk.inData,
                                    decoderChunk.bytes, (opus_int16 *)audio,
                                    samples_per_frame, 0);

                                samples_per_frame <<= 1;
                              } while (frame_size < 0);

                              free(decoderChunk.inData);
                              decoderChunk.inData = NULL;

                              pcm_chunk_message_t *new_pcmChunk = NULL;

                              // ESP_LOGW(TAG, "OPUS decode: %d", frame_size);

                              if (allocate_pcm_chunk_memory(&new_pcmChunk,
                                                            bytes) < 0) {
                                pcmData = NULL;
                              } else {
                                new_pcmChunk->timestamp = wire_chnk.timestamp;

                                if (new_pcmChunk->fragment->payload) {
                                  volatile uint32_t *sample;
                                  uint32_t tmpData;
                                  uint32_t cnt = 0;

                                  for (int i = 0; i < bytes; i += 4) {
                                    sample = (volatile uint32_t *)(&(
                                        new_pcmChunk->fragment->payload[i]));
                                    tmpData = (((uint32_t)audio[cnt] << 16) &
                                               0xFFFF0000) |
                                              (((uint32_t)audio[cnt + 1] << 0) &
                                               0x0000FFFF);
                                    *sample = (volatile uint32_t)tmpData;

                                    cnt += 2;
                                  }
                                }

                                free(audio);
                                audio = NULL;

#if CONFIG_USE_DSP_PROCESSOR
                                if (new_pcmChunk->fragment->payload) {
                                  dsp_processor_worker(
                                      new_pcmChunk->fragment->payload,
                                      new_pcmChunk->fragment->size, scSet.sr);
                                }
#endif

                                insert_pcm_chunk(new_pcmChunk);
                              }

                              if (player_send_snapcast_setting(&scSet) !=
                                  pdPASS) {
                                ESP_LOGE(TAG,
                                         "Failed to notify "
                                         "sync task about "
                                         "codec. Did you "
                                         "init player?");

                                return;
                              }

                              break;
                            }

                            case FLAC: {
                              isCachedChunk = true;
                              cachedBlocks = 0;

                              while (decoderChunk.bytes > 0) {
                                if (FLAC__stream_decoder_process_single(
                                        flacDecoder) == 0) {
                                  ESP_LOGE(
                                      TAG,
                                      "%s: FLAC__stream_decoder_process_single "
                                      "failed",
                                      __func__);

                                  // TODO: should insert some abort condition?
                                  vTaskDelay(pdMS_TO_TICKS(10));
                                }
                              }

                              // alternating chunk sizes need time stamp repair
                              if ((cachedBlocks > 0) && (scSet.sr != 0)) {
                                uint64_t diffUs =
                                    1000000ULL * cachedBlocks / scSet.sr;

                                uint64_t timestamp =
                                    1000000ULL * wire_chnk.timestamp.sec +
                                    wire_chnk.timestamp.usec;

                                timestamp = timestamp - diffUs;

                                wire_chnk.timestamp.sec =
                                    timestamp / 1000000ULL;
                                wire_chnk.timestamp.usec =
                                    timestamp % 1000000ULL;
                              }

                              pcm_chunk_message_t *new_pcmChunk;
                              int32_t ret = allocate_pcm_chunk_memory(
                                  &new_pcmChunk, pcmChunk.bytes);

                              scSet.chkInFrames =
                                  FLAC__stream_decoder_get_blocksize(
                                      flacDecoder);

                              // ESP_LOGE (TAG, "block size: %ld",
                              // scSet.chkInFrames * scSet.bits / 8 * scSet.ch);
                              // ESP_LOGI(TAG, "new_pcmChunk with size %ld",
                              // new_pcmChunk->totalSize);

                              if (ret == 0) {
                                pcm_chunk_fragment_t *fragment =
                                    new_pcmChunk->fragment;
                                uint32_t fragmentCnt = 0;

                                if (fragment->payload != NULL) {
                                  uint32_t frames =
                                      pcmChunk.bytes /
                                      (scSet.ch * (scSet.bits / 8));

                                  for (int i = 0; i < frames; i++) {
                                    // TODO: for now fragmented payload is not
                                    // supported and the whole chunk is expected
                                    // to be in the first fragment
                                    uint32_t tmpData;
                                    memcpy(&tmpData,
                                           &pcmChunk.outData[fragmentCnt],
                                           (scSet.ch * (scSet.bits / 8)));

                                    if (fragment != NULL) {
                                      volatile uint32_t *test =
                                          (volatile uint32_t *)(&(
                                              fragment->payload[fragmentCnt]));
                                      *test = (volatile uint32_t)tmpData;
                                    }

                                    fragmentCnt +=
                                        (scSet.ch * (scSet.bits / 8));
                                    if (fragmentCnt >= fragment->size) {
                                      fragmentCnt = 0;

                                      fragment = fragment->nextFragment;
                                    }
                                  }
                                }

                                new_pcmChunk->timestamp = wire_chnk.timestamp;

#if CONFIG_USE_DSP_PROCESSOR
                                if (new_pcmChunk->fragment->payload) {
                                  dsp_processor_worker(
                                      new_pcmChunk->fragment->payload,
                                      new_pcmChunk->fragment->size, scSet.sr);
                                }

#endif

                                insert_pcm_chunk(new_pcmChunk);
                              }

                              free(pcmChunk.outData);
                              pcmChunk.outData = NULL;
                              pcmChunk.bytes = 0;

                              if (player_send_snapcast_setting(&scSet) !=
                                  pdPASS) {
                                ESP_LOGE(TAG,
                                         "Failed to "
                                         "notify "
                                         "sync task "
                                         "about "
                                         "codec. Did you "
                                         "init player?");

                                return;
                              }

                              break;
                            }

                            case PCM: {
                              size_t decodedSize = wire_chnk.size;

                              // ESP_LOGW(TAG, "got PCM chunk,"
                              //               "typedMsgCurrentPos %d",
                              //               typedMsgCurrentPos);

                              if (pcmData) {
                                pcmData->timestamp = wire_chnk.timestamp;
                              }

                              scSet.chkInFrames =
                                  decodedSize /
                                  ((size_t)scSet.ch * (size_t)(scSet.bits / 8));

                              // ESP_LOGW(TAG,
                              //          "got PCM decoded chunk size: %ld
                              //          frames", scSet.chkInFrames);

                              if (player_send_snapcast_setting(&scSet) !=
                                  pdPASS) {
                                ESP_LOGE(TAG,
                                         "Failed to notify "
                                         "sync task about "
                                         "codec. Did you "
                                         "init player?");

                                return;
                              }

#if CONFIG_USE_DSP_PROCESSOR
                              if ((pcmData) && (pcmData->fragment->payload)) {
                                dsp_processor_worker(pcmData->fragment->payload,
                                                     pcmData->fragment->size,
                                                     scSet.sr);
                              }
#endif

                              if (pcmData) {
                                insert_pcm_chunk(pcmData);
                              }

                              pcmData = NULL;

                              free(decoderChunk.inData);
                              decoderChunk.inData = NULL;

                              break;
                            }

                            default: {
                              ESP_LOGE(TAG,
                                       "Decoder (2) not "
                                       "supported");

                              return;

                              break;
                            }
                          }
                        }

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        typedMsgCurrentPos = 0;
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "wire chunk decoder "
                               "shouldn't get here");

                      break;
                    }
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_CODEC_HEADER: {
                  switch (internalState) {
                    case 0: {
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      codecString =
                          malloc(typedMsgLen + 1);  // allocate memory for
                                                    // codec string
                      if (codecString == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory "
                                 "for codec string");

                        return;
                      }

                      offset = 0;
                      // ESP_LOGI(TAG,
                      // "codec header string is %d long",
                      // typedMsgLen);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      if (len >= typedMsgLen) {
                        memcpy(&codecString[offset], start, typedMsgLen);

                        offset += typedMsgLen;

                        typedMsgCurrentPos += typedMsgLen;
                        start += typedMsgLen;
                        // currentPos += typedMsgLen;
                        len -= typedMsgLen;
                      } else {
                        memcpy(&codecString[offset], start, typedMsgLen);

                        offset += len;

                        typedMsgCurrentPos += len;
                        start += len;
                        // currentPos += len;
                        len -= len;
                      }

                      if (offset == typedMsgLen) {
                        // NULL terminate string
                        codecString[typedMsgLen] = 0;

                        // ESP_LOGI (TAG, "got codec string: %s", tmp);

                        if (strcmp(codecString, "opus") == 0) {
                          codec = OPUS;
                        } else if (strcmp(codecString, "flac") == 0) {
                          codec = FLAC;
                        } else if (strcmp(codecString, "pcm") == 0) {
                          codec = PCM;
                        } else {
                          codec = NONE;

                          ESP_LOGI(TAG, "Codec : %s not supported",
                                   codecString);
                          ESP_LOGI(TAG,
                                   "Change encoder codec to "
                                   "opus, flac or pcm in "
                                   "/etc/snapserver.conf on "
                                   "server");

                          return;
                        }

                        free(codecString);
                        codecString = NULL;

                        internalState++;
                      }

                      if (len == 0) {
                        break;
                      }
                    }

                    case 5: {
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 6: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 7: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 8: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      codecPayload = malloc(typedMsgLen);  // allocate memory
                                                           // for codec payload
                      if (codecPayload == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory "
                                 "for codec payload");

                        return;
                      }

                      offset = 0;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 9: {
                      if (len >= typedMsgLen) {
                        memcpy(&codecPayload[offset], start, typedMsgLen);

                        offset += typedMsgLen;

                        typedMsgCurrentPos += typedMsgLen;
                        start += typedMsgLen;
                        // currentPos += typedMsgLen;
                        len -= typedMsgLen;
                      } else {
                        memcpy(&codecPayload[offset], start, len);

                        offset += len;

                        typedMsgCurrentPos += len;
                        start += len;
                        // currentPos += len;
                        len -= len;
                      }

                      if (offset == typedMsgLen) {
                        // first ensure everything is set up
                        // correctly and resources are
                        // available

                        if (flacDecoder != NULL) {
                          FLAC__stream_decoder_finish(flacDecoder);
                          FLAC__stream_decoder_delete(flacDecoder);
                          flacDecoder = NULL;
                        }

                        if (opusDecoder != NULL) {
                          opus_decoder_destroy(opusDecoder);
                          opusDecoder = NULL;
                        }

                        if (codec == OPUS) {
                          uint16_t channels;
                          uint32_t rate;
                          uint16_t bits;

                          memcpy(&rate, codecPayload + 4, sizeof(rate));
                          memcpy(&bits, codecPayload + 8, sizeof(bits));
                          memcpy(&channels, codecPayload + 10,
                                 sizeof(channels));

                          scSet.codec = codec;
                          scSet.bits = bits;
                          scSet.ch = channels;
                          scSet.sr = rate;

                          ESP_LOGI(TAG, "Opus sample format: %ld:%d:%d\n", rate,
                                   bits, channels);

                          int error = 0;

                          opusDecoder =
                              opus_decoder_create(scSet.sr, scSet.ch, &error);
                          if (error != 0) {
                            ESP_LOGI(TAG, "Failed to init opus coder");
                            return;
                          }

                          ESP_LOGI(TAG, "Initialized opus Decoder: %d", error);

                          // Set default Opus frame size (20ms) so the player
                          // can create its PCM queue. Needed for UDP path
                          // where TCP decode never sets chkInFrames.
                          // Use 20ms (sr/50) matching the server's typical
                          // chunk_ms=20 config; actual value is overridden
                          // after first decoded packet in both TCP and UDP paths.
                          scSet.chkInFrames = scSet.sr / 50;
                        } else if (codec == FLAC) {
                          decoderChunk.bytes = typedMsgLen;
                          decoderChunk.inData =
                              (uint8_t *)malloc(decoderChunk.bytes);
                          memcpy(decoderChunk.inData, codecPayload,
                                 typedMsgLen);
                          decoderChunk.outData = NULL;
                          decoderChunk.type = SNAPCAST_MESSAGE_CODEC_HEADER;

                          flacDecoder = FLAC__stream_decoder_new();
                          if (flacDecoder == NULL) {
                            ESP_LOGE(TAG, "Failed to init flac decoder");
                            return;
                          }

                          FLAC__StreamDecoderInitStatus init_status =
                              FLAC__stream_decoder_init_stream(
                                  flacDecoder, read_callback, NULL, NULL, NULL,
                                  NULL, write_callback, metadata_callback,
                                  error_callback, &scSet);
                          if (init_status !=
                              FLAC__STREAM_DECODER_INIT_STATUS_OK) {
                            ESP_LOGE(TAG, "ERROR: initializing decoder: %s\n",
                                     FLAC__StreamDecoderInitStatusString
                                         [init_status]);

                            return;
                          }

                          FLAC__stream_decoder_process_until_end_of_metadata(
                              flacDecoder);

                          // ESP_LOGI(TAG, "%s: processed codec header",
                          // __func__);
                        } else if (codec == PCM) {
                          uint16_t channels;
                          uint32_t rate;
                          uint16_t bits;

                          memcpy(&channels, codecPayload + 22,
                                 sizeof(channels));
                          memcpy(&rate, codecPayload + 24, sizeof(rate));
                          memcpy(&bits, codecPayload + 34, sizeof(bits));

                          scSet.codec = codec;
                          scSet.bits = bits;
                          scSet.ch = channels;
                          scSet.sr = rate;

                          ESP_LOGI(TAG, "pcm sampleformat: %ld:%d:%d", scSet.sr,
                                   scSet.bits, scSet.ch);
                        } else {
                          ESP_LOGE(TAG,
                                   "codec header decoder "
                                   "shouldn't get here after "
                                   "codec string was detected");

                          return;
                        }

                        free(codecPayload);
                        codecPayload = NULL;

                        if (player_send_snapcast_setting(&scSet) != pdPASS) {
                          ESP_LOGE(TAG,
                                   "Failed to notify sync task. "
                                   "Did you init player?");

                          return;
                        }

                        // ESP_LOGI(TAG, "done codec header msg");

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        received_header = true;
                        esp_timer_stop(timeSyncMessageTimer);
                        if (!esp_timer_is_active(timeSyncMessageTimer)) {
                          esp_timer_start_periodic(timeSyncMessageTimer,
                                                   timeout);
                        }

                        // Start UDP transport for audio data
                        if (!udp_active && codec == OPUS) {
                          // Clean up any previous UDP state
                          if (t_udp_recv_task) {
                            udp_active = false;
                            vTaskDelay(pdMS_TO_TICKS(50));
                            t_udp_recv_task = NULL;
                          }
                          if (udpNetconn) {
                            netconn_delete(udpNetconn);
                            udpNetconn = NULL;
                          }
                          udp_reorder_initialized = false;
                          udp_reorder_next_seq = 0;

                          udpNetconn = netconn_new(NETCONN_UDP);
                          if (udpNetconn) {
                            int udp_rc = netconn_bind(udpNetconn, IPADDR_ANY, 0);
                            if (udp_rc == ERR_OK) {
                              // Send registration packet to server's UDP port
                              // Format: 2-byte length prefix (LE) + clientId string
                              uint16_t mac_len = strlen(mac_address);
                              uint8_t *reg_pkt = malloc(2 + mac_len);
                              if (reg_pkt) {
                                reg_pkt[0] = mac_len & 0xFF;
                                reg_pkt[1] = (mac_len >> 8) & 0xFF;
                                memcpy(reg_pkt + 2, mac_address, mac_len);

                                int udp_send_rc = netconn_connect(udpNetconn, &remote_ip, UDP_SERVER_PORT);
                                if (udp_send_rc == ERR_OK) {
                                  struct netbuf *reg_buf = netbuf_new();
                                  if (reg_buf) {
                                    netbuf_ref(reg_buf, reg_pkt, 2 + mac_len);
                                    netconn_send(udpNetconn, reg_buf);
                                    netbuf_delete(reg_buf);
                                  }

                                  // Disconnect so we can receive from any source
                                  // (not strictly needed for UDP but cleaner)
                                  // Actually keep connected so recv works as expected

                                  udp_active = true;
                                  udp_reorder_initialized = false;
                                  udp_reorder_next_seq = 0;

                                  // Store MAC for periodic re-registration
                                  strncpy(udp_client_mac, mac_address, sizeof(udp_client_mac) - 1);
                                  udp_client_mac[sizeof(udp_client_mac) - 1] = '\0';

                                  // Create a snapshot of audio settings for the
                                  // UDP task so it doesn't share scSet with the
                                  // TCP task (which may modify volume/mute).
                                  static snapcastSetting_t udpScSet;
                                  udpScSet.sr = scSet.sr;
                                  udpScSet.ch = scSet.ch;
                                  udpScSet.bits = scSet.bits;
                                  udpScSet.codec = scSet.codec;
                                  udpScSet.buf_ms = scSet.buf_ms;
                                  udpScSet.chkInFrames = scSet.chkInFrames;

                                  xTaskCreatePinnedToCore(
                                      udp_recv_task, "udp_recv",
                                      15 * 1024,
                                      &udpScSet, UDP_RECV_TASK_PRIORITY,
                                      &t_udp_recv_task, UDP_RECV_TASK_CORE_ID);

                                  // Free TCP opus decoder — UDP task
                                  // has its own, saves ~30KB of RAM
                                  if (opusDecoder != NULL) {
                                    opus_decoder_destroy(opusDecoder);
                                    opusDecoder = NULL;
                                  }

                                  ESP_LOGI(TAG, "UDP transport started, registered with server");
                                } else {
                                  ESP_LOGW(TAG, "UDP connect failed: %d, falling back to TCP", udp_send_rc);
                                  netconn_delete(udpNetconn);
                                  udpNetconn = NULL;
                                }
                                free(reg_pkt);
                              }
                            } else {
                              ESP_LOGW(TAG, "UDP bind failed: %d, falling back to TCP", udp_rc);
                              netconn_delete(udpNetconn);
                              udpNetconn = NULL;
                            }
                          }
                        }
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "codec header decoder "
                               "shouldn't get here");

                      break;
                    }
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_SERVER_SETTINGS: {
                  switch (internalState) {
                    case 0: {
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      // ESP_LOGI(TAG,"server settings string is %lu"
                      //              " long", typedMsgLen);

                      // now get some memory for server settings
                      // string
                      serverSettingsString = malloc(typedMsgLen + 1);
                      if (serverSettingsString == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory for "
                                 "server settings string");
                      }

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      offset = 0;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      size_t tmpSize =
                          base_message_rx.size - typedMsgCurrentPos;

                      if (len > 0) {
                        if (tmpSize < len) {
                          if (serverSettingsString) {
                            memcpy(&serverSettingsString[offset], start,
                                   tmpSize);
                          }
                          offset += tmpSize;

                          start += tmpSize;
                          // currentPos += tmpSize;  // will be
                          //  incremented by 1
                          //  later so -1 here
                          typedMsgCurrentPos += tmpSize;
                          len -= tmpSize;
                        } else {
                          if (serverSettingsString) {
                            memcpy(&serverSettingsString[offset], start, len);
                          }
                          offset += len;

                          start += len;
                          // currentPos += len;  // will be incremented
                          //  by 1 later so -1
                          //  here
                          typedMsgCurrentPos += len;
                          len = 0;
                        }
                      }

                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        if (serverSettingsString) {
                          // ESP_LOGI(TAG, "done server settings %lu/%lu",
                          //								offset,
                          //								typedMsgLen);

                          // NULL terminate string
                          serverSettingsString[typedMsgLen] = 0;

                          // ESP_LOGI(TAG, "got string: %s",
                          // serverSettingsString);

                          result = server_settings_message_deserialize(
                              &server_settings_message, serverSettingsString);
                          if (result) {
                            ESP_LOGE(TAG,
                                     "Failed to read server "
                                     "settings: %d",
                                     result);
                          } else {
                            // log mute state, buffer, latency
                            ESP_LOGI(TAG, "Buffer length:  %ld",
                                     server_settings_message.buffer_ms);
                            ESP_LOGI(TAG, "Latency:        %ld",
                                     server_settings_message.latency);
                            ESP_LOGI(TAG, "Mute:           %d",
                                     server_settings_message.muted);
                            ESP_LOGI(TAG, "Setting volume: %ld",
                                     server_settings_message.volume);
                          }

                          // Volume setting using ADF HAL
                          // abstraction
                          if (scSet.muted != server_settings_message.muted) {
#if SNAPCAST_USE_SOFT_VOL
                            if (server_settings_message.muted) {
                              dsp_processor_set_volome(0.0);
                            } else {
                              dsp_processor_set_volome(
                                  (double)server_settings_message.volume / 100);
                            }
#endif
                            audio_set_mute(server_settings_message.muted);
                          }

                          if (scSet.volume != server_settings_message.volume) {
#if SNAPCAST_USE_SOFT_VOL
                            if (!server_settings_message.muted) {
                              dsp_processor_set_volome(
                                  (double)server_settings_message.volume / 100);
                            }
#else
                            audio_set_volume(server_settings_message.volume);
#endif
                          }

                          scSet.cDacLat_ms = server_settings_message.latency;
                          scSet.buf_ms = server_settings_message.buffer_ms;
                          scSet.muted = server_settings_message.muted;
                          scSet.volume = server_settings_message.volume;

                          if (player_send_snapcast_setting(&scSet) != pdPASS) {
                            ESP_LOGE(TAG,
                                     "Failed to notify sync task. "
                                     "Did you init player?");

                            return;
                          }

                          free(serverSettingsString);
                          serverSettingsString = NULL;
                        }

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        typedMsgCurrentPos = 0;
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "server settings decoder "
                               "shouldn't get here");

                      break;
                    }
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_STREAM_TAGS: {
                  size_t tmpSize = base_message_rx.size - typedMsgCurrentPos;

                  if (tmpSize < len) {
                    start += tmpSize;
                    // currentPos += tmpSize;
                    typedMsgCurrentPos += tmpSize;
                    len -= tmpSize;
                  } else {
                    start += len;
                    // currentPos += len;

                    typedMsgCurrentPos += len;
                    len = 0;
                  }

                  if (typedMsgCurrentPos >= base_message_rx.size) {
                    // ESP_LOGI(TAG,
                    // "done stream tags with length %d %d %d",
                    // base_message_rx.size, currentPos,
                    // tmpSize);

                    typedMsgCurrentPos = 0;
                    // currentPos = 0;

                    state = BASE_MESSAGE_STATE;
                    internalState = 0;
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_TIME: {
                  switch (internalState) {
                    case 0: {
                      time_message_rx.latency.sec = *start;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      time_message_rx.latency.sec |= (int32_t)*start << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      time_message_rx.latency.sec |= (int32_t)*start << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      time_message_rx.latency.sec |= (int32_t)*start << 24;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      time_message_rx.latency.usec = *start;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 5: {
                      time_message_rx.latency.usec |= (int32_t)*start << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 6: {
                      time_message_rx.latency.usec |= (int32_t)*start << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 7: {
                      time_message_rx.latency.usec |= (int32_t)*start << 24;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;
                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        // ESP_LOGI(TAG, "done time message");

                        typedMsgCurrentPos = 0;

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        trx =
                            (int64_t)base_message_rx.received.sec * 1000000LL +
                            (int64_t)base_message_rx.received.usec;
                        ttx = (int64_t)base_message_rx.sent.sec * 1000000LL +
                              (int64_t)base_message_rx.sent.usec;
                        tdif = trx - ttx;
                        trx = (int64_t)time_message_rx.latency.sec * 1000000LL +
                              (int64_t)time_message_rx.latency.usec;
                        tmpDiffToServer = (trx - tdif) / 2;

                        int64_t diff;

                        // clear diffBuffer if last update is
                        // older than a minute
                        diff = now - lastTimeSync;
                        if (diff > 60000000LL) {
                          ESP_LOGW(TAG,
                                   "Last time sync older "
                                   "than a minute. "
                                   "Clearing time buffer");

                          reset_latency_buffer();

                          timeout = FAST_SYNC_LATENCY_BUF;

                          esp_timer_stop(timeSyncMessageTimer);
                          if (received_header == true) {
                            if (!esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_start_periodic(timeSyncMessageTimer,
                                                       timeout);
                            }
                          }
                        }

                        const int64_t MAX_REASONABLE_DIFF = 30LL * 1000000LL;  // 30s
                        if (tmpDiffToServer > MAX_REASONABLE_DIFF ||
                            tmpDiffToServer < -MAX_REASONABLE_DIFF) {
                          if (!clock_synced_from_server) {
                            // Bootstrap clock using the NTP-style diff which
                            // already accounts for round-trip time.
                            boot_to_wall_offset_us = tmpDiffToServer;
                            clock_synced_from_server = true;
                            player_set_clock_offset(boot_to_wall_offset_us);
                            ESP_LOGI(TAG,
                                     "Clock synced from server, offset: "
                                     "%lldus",
                                     boot_to_wall_offset_us);
                            reset_latency_buffer();
                          } else {
                            ESP_LOGW(TAG,
                                     "Rejecting unreasonable diff to server: "
                                     "%lldus",
                                     tmpDiffToServer);
                          }
                        } else {
                          player_latency_insert(tmpDiffToServer);
                        }

                        // ESP_LOGI(TAG, "Current latency:%lld:",
                        // tmpDiffToServer);

                        // store current time
                        lastTimeSync = now;

                        if (received_header == true) {
                          if (!esp_timer_is_active(timeSyncMessageTimer)) {
                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          }

                          bool is_full = false;
                          latency_buffer_full(&is_full, portMAX_DELAY);
                          if ((is_full == true) &&
                              (timeout < NORMAL_SYNC_LATENCY_BUF)) {
                            timeout = NORMAL_SYNC_LATENCY_BUF;

                            ESP_LOGI(TAG, "latency buffer full");

                            if (esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_stop(timeSyncMessageTimer);
                            }

                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          } else if ((is_full == false) &&
                                     (timeout > FAST_SYNC_LATENCY_BUF)) {
                            timeout = FAST_SYNC_LATENCY_BUF;

                            ESP_LOGI(TAG, "latency buffer not full");

                            if (esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_stop(timeSyncMessageTimer);
                            }

                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          }
                        }
                      } else {
                        ESP_LOGE(TAG,
                                 "error time message, this "
                                 "shouldn't happen! %d %ld",
                                 typedMsgCurrentPos, base_message_rx.size);

                        typedMsgCurrentPos = 0;

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "time message decoder shouldn't "
                               "get here %d %ld %ld",
                               typedMsgCurrentPos, base_message_rx.size,
                               internalState);

                      break;
                    }
                  }

                  break;
                }

                default: {
                  typedMsgCurrentPos++;
                  start++;
                  // currentPos++;
                  len--;

                  if (typedMsgCurrentPos >= base_message_rx.size) {
                    ESP_LOGI(TAG, "done unknown typed message %d",
                             base_message_rx.type);

                    state = BASE_MESSAGE_STATE;
                    internalState = 0;

                    typedMsgCurrentPos = 0;
                  }

                  break;
                }
              }

              break;
            }

            default: {
              break;
            }
          }

          if (rc1 != ERR_OK) {
            break;
          }
        }
      } while (netbuf_next(firstNetBuf) >= 0);

      netbuf_delete(firstNetBuf);

      if (rc1 != ERR_OK) {
        ESP_LOGE(TAG, "Data error, closing netconn");

        netconn_close(lwipNetconn);

        break;
      }
    }
  }
}

/**
 *
 */
void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  esp_log_level_set("*", ESP_LOG_INFO);

  // if enabled these cause a timer srv stack overflow
  esp_log_level_set("HEADPHONE", ESP_LOG_NONE);
  esp_log_level_set("gpio", ESP_LOG_WARN);
  esp_log_level_set("uart", ESP_LOG_WARN);
  // esp_log_level_set("i2s_std", ESP_LOG_DEBUG);
  // esp_log_level_set("i2s_common", ESP_LOG_DEBUG);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi_init", ESP_LOG_WARN);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi_init", ESP_LOG_WARN);

#if !CONFIG_QEMU_MODE
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  // clang-format off
  // nINT/REFCLKO Function Select Configuration Strap
  //  • When nINTSEL is floated or pulled to
  //    VDD2A, nINT is selected for operation on the
  //    nINT/REFCLKO pin (default).
  //  • When nINTSEL is pulled low to VSS, REF-
  //    CLKO is selected for operation on the nINT/
  //    REFCLKO pin.
  //
  // LAN8720 doesn't stop REFCLK while in reset, so we leave the
  // strap floated. It is connected to IO0 on ESP32 so we get nINT
  // function with a HIGH pin value, which is also perfect during boot.
  // Before initializing LAN8720 (which resets the PHY) we pull the
  // strap low and this results in REFCLK enabled which is needed
  // for MAC unit.
  //
  // clang-format on
  gpio_config_t cfg = {.pin_bit_mask = BIT64(GPIO_NUM_5),
                       .mode = GPIO_MODE_DEF_INPUT,
                       .pull_up_en = GPIO_PULLUP_DISABLE,
                       .pull_down_en = GPIO_PULLDOWN_ENABLE,
                       .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&cfg);
#endif
#endif /* !CONFIG_QEMU_MODE */

#if CONFIG_QEMU_MODE
  ESP_LOGI(TAG, "QEMU mode: skipping audio board init, I2S uses stubs");
  i2s_std_gpio_config_t i2s_pin_config0 = {0};
  QueueHandle_t audioQHdl = xQueueCreate(1, sizeof(audioDACdata_t));
  init_snapcast(audioQHdl);
  init_player(i2s_pin_config0, I2S_NUM_0);
#else
  board_i2s_pin_t pin_config0;
  get_i2s_pins(I2S_NUM_0, &pin_config0);

#if CONFIG_AUDIO_BOARD_CUSTOM && CONFIG_DAC_ADAU1961
  // some codecs need i2s mclk for initialization

  i2s_chan_handle_t tx_chan;

  i2s_chan_config_t tx_chan_cfg = {
      .id = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 2,
      .dma_frame_num = 128,
      .auto_clear = true,
  };
  ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));

  i2s_std_clk_config_t i2s_clkcfg = {
      .sample_rate_hz = 44100,
      .clk_src = I2S_CLK_SRC_APLL,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  i2s_std_config_t tx_std_cfg = {
      .clk_cfg = i2s_clkcfg,
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = pin_config0
                          .mck_io_num,  // some codecs may require mclk signal,
                                        // this example doesn't need it
              .bclk = pin_config0.bck_io_num,
              .ws = pin_config0.ws_io_num,
              .dout = pin_config0.data_out_num,
              .din = pin_config0.data_in_num,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
  i2s_channel_enable(tx_chan);
#endif

  ESP_LOGI(TAG, "Start codec chip");
  audio_board_handle_t board_handle = audio_board_init();
  if (board_handle) {
    ESP_LOGI(TAG, "Audio board_init done");
  } else {
    ESP_LOGE(TAG,
             "Audio board couldn't be initialized. Check menuconfig if project "
             "is configured right or check your wiring!");

    vTaskDelay(portMAX_DELAY);
  }

  audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE,
                       AUDIO_HAL_CTRL_START);
  audio_hal_set_mute(board_handle->audio_hal,
                     true);  // ensure no noise is sent after firmware crash

#if CONFIG_AUDIO_BOARD_CUSTOM && CONFIG_DAC_ADAU1961
  if (tx_chan) {
    i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
  }
#endif

  //  ESP_LOGI(TAG, "init player");
  i2s_std_gpio_config_t i2s_pin_config0 =
  {
      .mclk = pin_config0.mck_io_num,
      .bclk = pin_config0.bck_io_num,
      .ws = pin_config0.ws_io_num,
      .dout = pin_config0.data_out_num,
      .din = pin_config0.data_in_num,
      .invert_flags =
          {
#if CONFIG_INVERT_MCLK_LEVEL
              .mclk_inv = true,

#else
              .mclk_inv = false,
#endif

#if CONFIG_INVERT_BCLK_LEVEL
              .bclk_inv = true,
#else
              .bclk_inv = false,
#endif

#if CONFIG_INVERT_WORD_SELECT_LEVEL
              .ws_inv = true,
#else
              .ws_inv = false,
#endif
          },
  };

  QueueHandle_t audioQHdl = xQueueCreate(1, sizeof(audioDACdata_t));

  init_snapcast(audioQHdl);
  init_player(i2s_pin_config0, I2S_NUM_0);
#endif /* !CONFIG_QEMU_MODE */

#if CONFIG_ETH_USE_OPENETH
  /* QEMU OpenCores Ethernet path */
  eth_init();
  init_http_server_task("ETH_DEF");
#elif CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  eth_init();
  // pass "WIFI_STA_DEF", "WIFI_AP_DEF", "ETH_DEF"
  init_http_server_task("ETH_DEF");
#else
  // Enable and setup WIFI in station mode and connect to Access point setup in
  // menu config or set up provisioning mode settable in menuconfig
  wifi_init();
  ESP_LOGI(TAG, "Connected to AP");
  // http server for control operations and user interface
  // pass "WIFI_STA_DEF", "WIFI_AP_DEF", "ETH_DEF"
  init_http_server_task("WIFI_STA_DEF");
#endif

  // Enable websocket server
  //  ESP_LOGI(TAG, "Setup ws server");
  //  websocket_if_start();

  net_mdns_register("snapclient");
#ifdef CONFIG_SNAPCLIENT_SNTP_ENABLE
  set_time_from_sntp();
#endif

#if CONFIG_USE_DSP_PROCESSOR
  dsp_processor_init();
#endif

  xTaskCreatePinnedToCore(&ota_server_task, "ota", 14 * 256, NULL,
                          OTA_TASK_PRIORITY, &t_ota_task, OTA_TASK_CORE_ID);

  xTaskCreatePinnedToCore(&http_get_task, "http", 15 * 1024, NULL,
                          HTTP_TASK_PRIORITY, &t_http_get_task,
                          HTTP_TASK_CORE_ID);

  //  while (1) {
  //    // audio_event_iface_msg_t msg;
  //    vTaskDelay(portMAX_DELAY);  //(pdMS_TO_TICKS(5000));
  //
  //    // ma120_read_error(0x20);
  //
  //    esp_err_t ret = 0;  // audio_event_iface_listen(evt, &msg,
  //    portMAX_DELAY); if (ret != ESP_OK) {
  //      ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
  //      continue;
  //    }
  //  }

  audioDACdata_t dac_data;
  audioDACdata_t dac_data_old = {
    .mute = true,
    .volume = 100,
  };

  while(1) {
    if (xQueueReceive(audioQHdl, &dac_data, portMAX_DELAY) == pdTRUE) {
#if !CONFIG_QEMU_MODE
      if (dac_data.mute != dac_data_old.mute){
        audio_hal_set_mute(board_handle->audio_hal, dac_data.mute);
      }
      if (dac_data.volume != dac_data_old.volume){
        audio_hal_set_volume(board_handle->audio_hal, dac_data.volume);
      }
#endif
      dac_data_old = dac_data;
    }
  }
}
