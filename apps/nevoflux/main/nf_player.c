#include "nf_player.h"
#include "bsp_143c.h"
#include "nf_proto.h"

#include <string.h>

#include "decoder/impl/esp_opus_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "nf_player";

/* 30 s of 16 kHz mono in PSRAM */
#define NF_PLAYER_SAMPLES   (16000U * 30U)
/*
 * The head synthesises faster than real time and sends each sentence as soon as
 * it is made, so a long answer arrives well ahead of the speaker. Sentences wait
 * here still compressed (~3 KB/s) and are decoded only to keep this much PCM
 * ahead of it; decoding all of them on arrival overran the ring and cut the
 * answer off partway.
 */
#define NF_PLAYER_AHEAD     (16000U * 2U)
#define NF_PLAYER_SENTENCES 64U
#define NF_PLAYER_BLOCK     320U /* 20 ms */
/* Switch the PA off (and its hiss) this long after the last sample */
#define NF_PLAYER_IDLE_MS   300U

static int16_t          *s_ring;
static size_t            s_head;
static size_t            s_count;
static SemaphoreHandle_t s_lock;
static volatile bool     s_drained_edge;
static volatile uint8_t  s_level;

typedef struct {
    uint8_t *buf; /* u16 LE length + Opus packet, repeated */
    size_t   len;
} nf_sentence_t;

static QueueHandle_t s_sentences;
static nf_sentence_t s_cur;      /* the sentence being decoded (player task only) */
static size_t        s_cur_off;
static volatile bool s_flush;    /* set by stop: drop s_cur too */
static void         *s_dec;

/* Decode queued sentences until NF_PLAYER_AHEAD is buffered or nothing is left. */
static void top_up(void)
{
    static int16_t pcm[960]; /* up to 60 ms at 16 kHz; packets here are 20 ms */
    for (;;) {
        if (s_flush) {
            free(s_cur.buf);
            s_cur.buf = NULL;
            s_flush = false;
        }
        if (s_count >= NF_PLAYER_AHEAD) {
            return;
        }
        if (!s_cur.buf) {
            if (xQueueReceive(s_sentences, &s_cur, 0) != pdTRUE) {
                return;
            }
            s_cur_off = 0U;
            /* Each sentence was encoded from a fresh encoder */
            (void)esp_opus_dec_reset(s_dec);
        }
        const uint8_t *pkt = NULL;
        size_t pkt_len = 0U;
        if (!nf_opus_next_packet(s_cur.buf, s_cur.len, &s_cur_off, &pkt, &pkt_len)) {
            if (s_cur_off != s_cur.len) {
                ESP_LOGW(TAG, "voice_audio opus: %u of %u bytes used", (unsigned)s_cur_off,
                         (unsigned)s_cur.len);
            }
            free(s_cur.buf);
            s_cur.buf = NULL;
            continue;
        }
        esp_audio_dec_in_raw_t raw = {.buffer = (uint8_t *)pkt, .len = (uint32_t)pkt_len};
        esp_audio_dec_out_frame_t out = {.buffer = (uint8_t *)pcm, .len = sizeof(pcm)};
        esp_audio_dec_info_t info = {0};
        if (esp_opus_dec_decode(s_dec, &raw, &out, &info) == ESP_AUDIO_ERR_OK && !s_flush) {
            (void)nf_player_push(pcm, out.decoded_size / sizeof(int16_t));
        }
    }
}

static void player_task(void *arg)
{
    (void)arg;
    int16_t block[NF_PLAYER_BLOCK];
    bool playing = false;
    uint32_t idle_ms = 0U;
    uint32_t failed = 0U;
    for (;;) {
        top_up();
        size_t n = 0U;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_count > 0U) {
            n = s_count < NF_PLAYER_BLOCK ? s_count : NF_PLAYER_BLOCK;
            for (size_t i = 0; i < n; ++i) {
                block[i] = s_ring[(s_head + i) % NF_PLAYER_SAMPLES];
            }
            s_head = (s_head + n) % NF_PLAYER_SAMPLES;
            s_count -= n;
        }
        xSemaphoreGive(s_lock);

        if (n > 0U) {
            if (n < NF_PLAYER_BLOCK) {
                memset(block + n, 0, (NF_PLAYER_BLOCK - n) * sizeof(int16_t));
            }
            int32_t sum = 0;
            for (size_t i = 0; i < NF_PLAYER_BLOCK; ++i) {
                sum += block[i] < 0 ? -block[i] : block[i];
            }
            const int32_t v = (sum / (int32_t)NF_PLAYER_BLOCK - 150) * 100 / 4000;
            s_level = (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
            size_t written = 0U;
            if (bsp_audio_write_spk(block, NF_PLAYER_BLOCK, &written, 100U)) {
                if (failed > 0U) {
                    ESP_LOGW(TAG, "speaker back after %u failed blocks", (unsigned)failed);
                    failed = 0U;
                }
            } else {
                /* Keep time so the answer does not race ahead; say so once a burst */
                if (failed++ == 0U) {
                    ESP_LOGE(TAG, "speaker write failed; resetting the audio path");
                    (void)bsp_audio_restore_capture_16k();
                }
                vTaskDelay(pdMS_TO_TICKS(NF_PLAYER_BLOCK / 16U));
            }
            playing = true;
            idle_ms = 0U;
            continue;
        }
        s_level = 0U;
        if (playing && !s_cur.buf && uxQueueMessagesWaiting(s_sentences) == 0U) {
            vTaskDelay(pdMS_TO_TICKS(20));
            idle_ms += 20U;
            if (idle_ms >= NF_PLAYER_IDLE_MS) {
                (void)bsp_audio_spk_idle(); /* PA off; the codec stays open at 16 kHz */
                playing = false;
                s_drained_edge = true;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

bool nf_player_init(void)
{
    s_ring = heap_caps_malloc(NF_PLAYER_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = xSemaphoreCreateMutex();
    s_sentences = xQueueCreate(NF_PLAYER_SENTENCES, sizeof(nf_sentence_t));
    if (!s_ring || !s_lock || !s_sentences) {
        ESP_LOGE(TAG, "no memory for the playback buffer");
        return false;
    }
    esp_opus_dec_cfg_t cfg = {
        .sample_rate = 16000,
        .channel = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
        .self_delimited = false,
    };
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &s_dec) != ESP_AUDIO_ERR_OK) {
        s_dec = NULL;
        ESP_LOGE(TAG, "no Opus decoder");
    }
    /* The Opus decoder needs more stack than the speaker loop alone */
    return xTaskCreatePinnedToCoreWithCaps(player_task, "nf_player", 16384, NULL, 9, NULL, 1,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

bool nf_player_queue_opus(uint8_t *framed, size_t len)
{
    if (!s_sentences || !s_dec || !framed) {
        free(framed);
        return false;
    }
    nf_sentence_t sn = {.buf = framed, .len = len};
    if (xQueueSend(s_sentences, &sn, 0) != pdTRUE) {
        ESP_LOGW(TAG, "too many sentences queued; one dropped");
        free(framed);
        return false;
    }
    s_drained_edge = false;
    return true;
}

size_t nf_player_push(const int16_t *pcm, size_t samples)
{
    if (!s_ring || !pcm) {
        return 0U;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t room = NF_PLAYER_SAMPLES - s_count;
    size_t n = samples < room ? samples : room;
    size_t tail = (s_head + s_count) % NF_PLAYER_SAMPLES;
    for (size_t i = 0; i < n; ++i) {
        s_ring[(tail + i) % NF_PLAYER_SAMPLES] = pcm[i];
    }
    s_count += n;
    s_drained_edge = false;
    xSemaphoreGive(s_lock);
    if (n < samples) {
        ESP_LOGW(TAG, "playback buffer full; %u samples dropped", (unsigned)(samples - n));
    }
    return n;
}

void nf_player_stop(void)
{
    if (!s_lock) {
        return;
    }
    nf_sentence_t sn;
    while (s_sentences && xQueueReceive(s_sentences, &sn, 0) == pdTRUE) {
        free(sn.buf);
    }
    s_flush = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0U;
    xSemaphoreGive(s_lock);
}

bool nf_player_take_drained(void)
{
    if (s_drained_edge) {
        s_drained_edge = false;
        return true;
    }
    return false;
}

uint8_t nf_player_level(void)
{
    return s_level;
}
