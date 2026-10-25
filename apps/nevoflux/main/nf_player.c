#include "nf_player.h"
#include "bsp_143c.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "nf_player";

/* 30 s of 16 kHz mono in PSRAM: a whole answer is queued as it arrives */
#define NF_PLAYER_SAMPLES   (16000U * 30U)
#define NF_PLAYER_BLOCK     320U /* 20 ms */
/* Close the speaker (and its PA hiss) this long after the last sample */
#define NF_PLAYER_IDLE_MS   300U

static int16_t          *s_ring;
static size_t            s_head;
static size_t            s_count;
static SemaphoreHandle_t s_lock;
static volatile bool     s_drained_edge;

static void player_task(void *arg)
{
    (void)arg;
    int16_t block[NF_PLAYER_BLOCK];
    bool playing = false;
    uint32_t idle_ms = 0U;
    for (;;) {
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
            size_t written = 0U;
            (void)bsp_audio_write_spk(block, NF_PLAYER_BLOCK, &written, 100U);
            playing = true;
            idle_ms = 0U;
            continue;
        }
        if (playing) {
            vTaskDelay(pdMS_TO_TICKS(20));
            idle_ms += 20U;
            if (idle_ms >= NF_PLAYER_IDLE_MS) {
                (void)bsp_audio_restore_capture_16k(); /* closes the speaker, mic keeps running */
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
    if (!s_ring || !s_lock) {
        ESP_LOGE(TAG, "no memory for the playback buffer");
        return false;
    }
    return xTaskCreatePinnedToCore(player_task, "nf_player", 4096, NULL, 9, NULL, 1) == pdPASS;
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
