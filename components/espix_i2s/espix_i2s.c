/*
 * The S31 coreboard's ES8311, over I2S and I2C, with the NS4150B PA.
 *
 * This is the first sink that is not Bluetooth. It registers an "es8311" sink,
 * so `play` reaches the speaker with the BT stack off entirely -- and because
 * the engine prefers a connected sink, the speaker is what plays when no A2DP
 * link is up.
 *
 * Nothing is opened at boot: the I2C bus, the I2S channels and the codec are
 * created on the first stream, so a board that never plays pays nothing. The
 * pins default to the S31 function coreboard's, and each is a Kconfig value.
 */
#if CONFIG_ESPIX_I2S

#include <string.h>

#include "espix_audio_sink.h"
#include "espix_i2s.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#define TAG "i2s"

static i2c_master_bus_handle_t       s_bus;
static i2s_chan_handle_t             s_tx;
static i2s_chan_handle_t             s_rx;
static const audio_codec_data_if_t  *s_data;
static esp_codec_dev_handle_t        s_dev;
static bool                          s_open;

static espix_audio_format_t s_fmt = { 44100, 2, 16 };

static bool i2s_connected(void)              { return true; }
static espix_audio_format_t i2s_format(void) { return s_fmt; }

static bool i2s_open(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = CONFIG_ESPIX_I2S_I2C_PORT,
        .sda_io_num                   = CONFIG_ESPIX_I2S_I2C_SDA,
        .scl_io_num                   = CONFIG_ESPIX_I2S_I2C_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus");
        return false;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &s_tx, &s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s channel");
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(s_fmt.rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = CONFIG_ESPIX_I2S_MCLK,
            .bclk = CONFIG_ESPIX_I2S_BCLK,
            .ws   = CONFIG_ESPIX_I2S_WS,
            .dout = CONFIG_ESPIX_I2S_DOUT,
            .din  = CONFIG_ESPIX_I2S_DIN,
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK ||
        i2s_channel_init_std_mode(s_rx, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s std mode");
        return false;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = CONFIG_ESPIX_I2S_I2C_PORT,
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_bus,
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = I2S_NUM_0,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_data = audio_codec_new_i2s_data(&i2s_cfg);

    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if     = ctrl,
        .gpio_if     = audio_codec_new_gpio(),
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = CONFIG_ESPIX_I2S_PA,
        .pa_reverted = false,
        .master_mode = false,       /* the ESP is the I2S master */
        .use_mclk    = true,
        .mclk_div    = 256,
    };
    const audio_codec_if_t *codec = es8311_codec_new(&codec_cfg);
    if (codec == NULL || s_data == NULL) {
        ESP_LOGE(TAG, "codec");
        return false;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec,
        .data_if  = s_data,
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (s_dev == NULL) {
        ESP_LOGE(TAG, "codec dev");
        return false;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 2,
        .sample_rate     = s_fmt.rate,
        .mclk_multiple   = 256,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open");
        return false;
    }
    esp_codec_dev_set_out_vol(s_dev, CONFIG_ESPIX_I2S_DEFAULT_VOL);

    s_open = true;
    ESP_LOGI(TAG, "es8311 open: %u Hz, %u ch, %u bits",
             (unsigned)s_fmt.rate, (unsigned)s_fmt.channels, (unsigned)s_fmt.bits);
    return true;
}

static size_t i2s_write(const void *pcm, size_t len)
{
    if (!s_open && !i2s_open()) {
        return 0;
    }
    if (esp_codec_dev_write(s_dev, (void *)pcm, (int)len) != ESP_CODEC_DEV_OK) {
        return 0;
    }
    return len;
}

static void i2s_start(void)
{
    if (!s_open) {
        i2s_open();
    }
    if (s_open) {
        esp_codec_dev_set_out_mute(s_dev, false);
    }
}

static void i2s_suspend(void)
{
    if (s_open) {
        esp_codec_dev_set_out_mute(s_dev, true);
    }
}

static const espix_audio_sink_ops_t s_es8311 = {
    .name      = "es8311",
    .connected = i2s_connected,
    .format    = i2s_format,
    .write     = i2s_write,
    .start     = i2s_start,
    .suspend   = i2s_suspend,
};

esp_err_t espix_i2s_sink_register(void)
{
    return espix_audio_sink_register(&s_es8311);
}

#endif /* CONFIG_ESPIX_I2S */
