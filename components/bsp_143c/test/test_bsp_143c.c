#include "bsp_143c.h"
#include <assert.h>
#include <stdio.h>

static void test_audio_sample_rate_switching(void)
{
    assert(bsp_audio_init(16000U));
    assert(bsp_audio_get_sample_rate() == 16000U);

    /* Switch to Kokoro 24kHz */
    assert(bsp_audio_set_play_sample_rate(24000U));
    assert(bsp_audio_get_sample_rate() == 24000U);

    /* Switch to MOSS 48kHz */
    assert(bsp_audio_set_play_sample_rate(48000U));
    assert(bsp_audio_get_sample_rate() == 48000U);

    /* Reject unsupported sample rate */
    assert(!bsp_audio_set_play_sample_rate(11025U));

    /* Restore capture 16kHz on barge_in / playback completion */
    assert(bsp_audio_restore_capture_16k());
    assert(bsp_audio_get_sample_rate() == 16000U);
}

static void test_boot_button_gestures(void)
{
    bsp_key_fsm_t fsm;
    bsp_key_init(&fsm);

    /* 1. Single click (press 80ms, release, wait >350ms) -> confirms pairing */
    assert(bsp_key_Step(&fsm, true, false, 100ULL) == BSP_KEY_EVENT_NONE);
    assert(bsp_key_Step(&fsm, false, false, 180ULL) == BSP_KEY_EVENT_NONE);
    assert(bsp_key_Step(&fsm, false, false, 600ULL) == BSP_KEY_EVENT_BOOT_SINGLE_CLICK);

    /* 2. Double click (two short clicks within 350ms) -> toggles Meeting Mode */
    assert(bsp_key_Step(&fsm, true, false, 1000ULL) == BSP_KEY_EVENT_NONE);
    assert(bsp_key_Step(&fsm, false, false, 1080ULL) == BSP_KEY_EVENT_NONE);
    assert(bsp_key_Step(&fsm, true, false, 1200ULL) == BSP_KEY_EVENT_NONE);
    assert(bsp_key_Step(&fsm, false, false, 1280ULL) == BSP_KEY_EVENT_BOOT_DOUBLE_CLICK);

    /* 3. Hold >= 300ms -> Push-to-Talk START, release -> PTT RELEASE */
    assert(bsp_key_Step(&fsm, true, false, 2000ULL) == BSP_KEY_EVENT_NONE);
    assert(bsp_key_Step(&fsm, true, false, 2350ULL) == BSP_KEY_EVENT_BOOT_PRESS_START);
    assert(bsp_key_Step(&fsm, true, false, 3000ULL) == BSP_KEY_EVENT_NONE);
    assert(bsp_key_Step(&fsm, false, false, 3100ULL) == BSP_KEY_EVENT_BOOT_RELEASE);
}

int main(void)
{
    test_audio_sample_rate_switching();
    test_boot_button_gestures();
    printf("[PASS] All bsp_143c unit tests passed!\n");
    return 0;
}
