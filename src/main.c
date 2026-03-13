#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb.h"
#include "usb_descriptors.h"

static const char *TAG = "usb-mic";

// I2S pins for PCM1808
#define I2S_MIC_BCK   5
#define I2S_MIC_DIN   6
#define I2S_MIC_LRCK  7
#define I2S_MIC_MCLK  8

#define SAMPLE_RATE       48000
#define SAMPLES_PER_MS    (SAMPLE_RATE / 1000)  // 48
#define BYTES_PER_FRAME   (SAMPLES_PER_MS * 2)  // 96 bytes per USB frame (16-bit mono)

// I2S reads in chunks matching USB frame size for tight coupling
// 48 stereo frames * 8 bytes = 384 bytes from I2S -> 48 mono 16-bit samples = 96 bytes out
#define I2S_FRAME_COUNT   SAMPLES_PER_MS
#define I2S_BUF_SIZE      (I2S_FRAME_COUNT * 8)  // 8 bytes per stereo frame (2x 32-bit)

static i2s_chan_handle_t s_i2s_rx_handle;

// Audio state
static bool s_muted = false;
static int16_t s_volume = 0;

//--------------------------------------------------------------------
// I2S Setup
//--------------------------------------------------------------------
static void i2s_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = I2S_FRAME_COUNT;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MIC_MCLK,
            .bclk = I2S_MIC_BCK,
            .ws   = I2S_MIC_LRCK,
            .din  = I2S_MIC_DIN,
            .dout = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_rx_handle));
    ESP_LOGI(TAG, "I2S initialized: %d Hz", SAMPLE_RATE);
}

//--------------------------------------------------------------------
// Audio task: reads I2S and writes directly into TinyUSB FIFO
// No ring buffer - direct coupling eliminates periodic glitches
//--------------------------------------------------------------------
static void audio_task(void *arg) {
    uint8_t i2s_buf[I2S_BUF_SIZE];
    int16_t pcm_buf[I2S_FRAME_COUNT];

    while (1) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_i2s_rx_handle, i2s_buf, sizeof(i2s_buf),
                                          &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) {
            continue;
        }

        // Extract left channel from 32-bit stereo to 16-bit mono
        int32_t *samples32 = (int32_t *)i2s_buf;
        size_t num_stereo_frames = bytes_read / 8;

        if (s_muted) {
            memset(pcm_buf, 0, num_stereo_frames * sizeof(int16_t));
        } else {
            for (size_t i = 0; i < num_stereo_frames; i++) {
                int32_t left = samples32[i * 2];
                pcm_buf[i] = (int16_t)(left >> 16);
            }
        }

        // Write directly into TinyUSB's internal EP FIFO
        // tud_audio_write is safe to call from task context
        uint16_t written = 0;
        uint16_t to_write = num_stereo_frames * sizeof(int16_t);
        while (written < to_write) {
            uint16_t n = tud_audio_write((uint8_t *)pcm_buf + written, to_write - written);
            written += n;
            if (n == 0) {
                // FIFO full, wait briefly for USB to drain it
                vTaskDelay(1);
            }
        }
    }
}

//--------------------------------------------------------------------
// TinyUSB Audio Callbacks
//--------------------------------------------------------------------

// TX done ISR - nothing to do, audio_task continuously fills the FIFO
bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id, uint8_t ep_in, uint8_t cur_alt_setting) {
    (void)rhport; (void)n_bytes_sent; (void)func_id; (void)ep_in; (void)cur_alt_setting;
    return true;
}

// Handle GET requests for entities (clock source, feature unit, input terminal)
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);

    // Clock Source (ID=0x04)
    if (entityID == 0x04) {
        if (ctrlSel == AUDIO_CS_CTRL_SAM_FREQ) {
            if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
                audio_control_cur_4_t freq = { .bCur = SAMPLE_RATE };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &freq, sizeof(freq));
            } else if (p_request->bRequest == AUDIO_CS_REQ_RANGE) {
                typedef struct TU_ATTR_PACKED {
                    uint16_t wNumSubRanges;
                    uint32_t bMin;
                    uint32_t bMax;
                    uint32_t bRes;
                } audio_control_range_4_1_t;
                audio_control_range_4_1_t range = {
                    .wNumSubRanges = 1,
                    .bMin = SAMPLE_RATE,
                    .bMax = SAMPLE_RATE,
                    .bRes = 0
                };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range, sizeof(range));
            }
        } else if (ctrlSel == AUDIO_CS_CTRL_CLK_VALID) {
            if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
                audio_control_cur_1_t valid = { .bCur = 1 };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &valid, sizeof(valid));
            }
        }
        return false;
    }

    // Feature Unit (ID=0x02)
    if (entityID == 0x02) {
        if (ctrlSel == AUDIO_FU_CTRL_MUTE) {
            if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
                audio_control_cur_1_t mute = { .bCur = s_muted };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute, sizeof(mute));
            }
        } else if (ctrlSel == AUDIO_FU_CTRL_VOLUME) {
            if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
                audio_control_cur_2_t vol = { .bCur = s_volume };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &vol, sizeof(vol));
            } else if (p_request->bRequest == AUDIO_CS_REQ_RANGE) {
                typedef struct TU_ATTR_PACKED {
                    uint16_t wNumSubRanges;
                    int16_t bMin;
                    int16_t bMax;
                    uint16_t bRes;
                } audio_control_range_2_1_t;
                audio_control_range_2_1_t range = {
                    .wNumSubRanges = 1,
                    .bMin = -90 * 256,
                    .bMax = 0,
                    .bRes = 256
                };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range, sizeof(range));
            }
        }
        return false;
    }

    // Input Terminal (ID=0x01)
    if (entityID == 0x01) {
        if (ctrlSel == AUDIO_TE_CTRL_CONNECTOR) {
            if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
                audio_control_cur_1_t conn = { .bCur = 1 };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &conn, sizeof(conn));
            }
        }
        return false;
    }

    ESP_LOGW(TAG, "Unhandled entity GET: id=%d ctrl=%d", entityID, ctrlSel);
    return false;
}

// Handle SET requests for entities
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
    (void)rhport;
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);

    if (entityID == 0x04) {
        if (ctrlSel == AUDIO_CS_CTRL_SAM_FREQ) return true;
        return false;
    }

    if (entityID == 0x02) {
        if (ctrlSel == AUDIO_FU_CTRL_MUTE) {
            s_muted = ((audio_control_cur_1_t const *)pBuff)->bCur;
            ESP_LOGI(TAG, "Mute %s", s_muted ? "ON" : "OFF");
            return true;
        } else if (ctrlSel == AUDIO_FU_CTRL_VOLUME) {
            s_volume = ((audio_control_cur_2_t const *)pBuff)->bCur;
            ESP_LOGI(TAG, "Volume: %d/256 dB", s_volume);
            return true;
        }
        return false;
    }

    return false;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    uint8_t const itf = TU_U16_LOW(p_request->wIndex);
    uint8_t const alt = TU_U16_LOW(p_request->wValue);
    ESP_LOGI(TAG, "Set interface %d alt %d", itf, alt);
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    (void)p_request;
    return true;
}

//--------------------------------------------------------------------
// Main
//--------------------------------------------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "USB Microphone starting...");

    i2s_init();

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &desc_device,
        .string_descriptor = desc_strings,
        .string_descriptor_count = desc_strings_count,
        .external_phy = false,
        .configuration_descriptor = desc_configuration,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "TinyUSB initialized");

    // Single audio task: I2S read -> TinyUSB write, no intermediate buffer
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, configMAX_PRIORITIES - 1, NULL, 0);

    ESP_LOGI(TAG, "USB Microphone ready");
}
