/*
 * The S31 coreboard's ES8311, over I2S and I2C, with the NS4150B PA.
 *
 * Both directions: a DAC sink onto the speaker, and an ADC source from the
 * on-board microphone. This is the first audio that is not Bluetooth, so the
 * board plays and records with the BT stack off entirely.
 *
 * Nothing is opened at boot: the I2C bus, the I2S channels and the two codec
 * devices are created on the first stream in that direction, so a board that
 * neither plays nor records pays nothing. The pins default to the S31 function
 * coreboard's, and each is a Kconfig value.
 */
#if CONFIG_ESPIX_I2S

#include <string.h>

#include "espix_audio_sink.h"
#include "espix_audio_source.h"
#include "espix_i2s.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#define TAG "i2s"

/*
 * The board decides the codec and the address; everything above it is the same.
 * Both `esp_codec_dev_defaults.h` codec headers are present (each is behind a
 * Kconfig of its own, both on by default), so this is a compile-time choice and
 * not a dependency question.
 */
#if CONFIG_ESPIX_I2S_BOARD_KORVO_1
#define CODEC_ADDR   ES8389_CODEC_DEFAULT_ADDR
#define SINK_LABEL   "S31 Korvo-1 speaker"
#define SOURCE_LABEL "S31 Korvo-1 microphone"
#define CODEC_MCLK   ((CONFIG_ESPIX_I2S_MCLK) >= 0)
#else
#define CODEC_ADDR   ES8311_CODEC_DEFAULT_ADDR
#define SINK_LABEL   "S31 coreboard speaker"
#define SOURCE_LABEL "S31 coreboard microphone"
#define CODEC_MCLK   true
#endif

/* One chip: one bus, one pair of I2S channels, one control and data interface,
 * shared by both directions. */
static i2c_master_bus_handle_t      s_bus;
static i2s_chan_handle_t            s_tx;
static i2s_chan_handle_t            s_rx;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_gpio_if_t *s_gpio;
static const audio_codec_data_if_t *s_data;
static bool                         s_hw;

static esp_codec_dev_handle_t s_dac;
static esp_codec_dev_handle_t s_adc;

static espix_audio_format_t s_out_fmt = { 44100, 2, 16 };
static espix_audio_format_t s_in_fmt  = { 44100, 2, 16 };

/* The codec's own output volume, remembered so a set before the first stream
 * still applies when the DAC opens. */
static int s_out_vol = CONFIG_ESPIX_I2S_DEFAULT_VOL;

/*
 * One codec, whichever it is. `dac` says whether the PA pin belongs to this
 * direction (the DAC's does; the ADC's must not be given it), and no_dac_ref
 * silences the reference the right channel would otherwise carry into a
 * capture.
 */
static const audio_codec_if_t *codec_new(esp_codec_dec_work_mode_t mode,
                                         bool dac, bool no_dac_ref)
{
#if CONFIG_ESPIX_I2S_BOARD_KORVO_1
    es8389_codec_cfg_t cfg = {
        .ctrl_if     = s_ctrl,
        .gpio_if     = s_gpio,
        .codec_mode  = mode,
        .pa_pin      = dac ? CONFIG_ESPIX_I2S_PA : -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk    = CODEC_MCLK,
        .no_dac_ref  = no_dac_ref,
        .mclk_div    = 256,
    };
    return es8389_codec_new(&cfg);
#else
    es8311_codec_cfg_t cfg = {
        .ctrl_if     = s_ctrl,
        .gpio_if     = s_gpio,
        .codec_mode  = mode,
        .pa_pin      = dac ? CONFIG_ESPIX_I2S_PA : -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk    = CODEC_MCLK,
        .no_dac_ref  = no_dac_ref,
        .mclk_div    = 256,
    };
    return es8311_codec_new(&cfg);
#endif
}

static bool hw_ensure(void)
{
    if (s_hw) {
        return true;
    }

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

    /* The ES8311 wants MCLK, and both directions run from the same clocks. */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(s_out_fmt.rate),
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
        .addr       = CODEC_ADDR,
        .bus_handle = s_bus,
    };
    s_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    s_gpio = audio_codec_new_gpio();

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = I2S_NUM_0,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_data = audio_codec_new_i2s_data(&i2s_cfg);

    if (s_ctrl == NULL || s_data == NULL) {
        ESP_LOGE(TAG, "codec interfaces");
        return false;
    }
    s_hw = true;
    return true;
}

/* ------------------------------------------------------------------ sink -- */

static bool dac_open(void)
{
    if (s_dac != NULL) {
        return true;
    }
    if (!hw_ensure()) {
        return false;
    }

    const audio_codec_if_t *codec = codec_new(ESP_CODEC_DEV_WORK_MODE_DAC, true, false);
    if (codec == NULL) {
        ESP_LOGE(TAG, "dac codec");
        return false;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec,
        .data_if  = s_data,
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    };
    s_dac = esp_codec_dev_new(&dev_cfg);
    if (s_dac == NULL) {
        ESP_LOGE(TAG, "dac dev");
        return false;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 2,
        .sample_rate     = s_out_fmt.rate,
        .mclk_multiple   = 256,
    };
    if (esp_codec_dev_open(s_dac, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "dac open");
        return false;
    }
    esp_codec_dev_set_out_vol(s_dac, s_out_vol);
    ESP_LOGI(TAG, "es8311 dac: %u Hz, %u ch", (unsigned)s_out_fmt.rate,
             (unsigned)s_out_fmt.channels);
    return true;
}

static bool sink_connected(void) { return true; }
static espix_audio_format_t sink_format(void) { return s_out_fmt; }

static size_t i2s_write(const void *pcm, size_t len)
{
    if (s_dac == NULL && !dac_open()) {
        return 0;
    }
    if (esp_codec_dev_write(s_dac, (void *)pcm, (int)len) != ESP_CODEC_DEV_OK) {
        return 0;
    }
    return len;
}

static void sink_start(void)
{
    if (dac_open()) {
        esp_codec_dev_set_out_mute(s_dac, false);
    }
}

static void sink_suspend(void)
{
    if (s_dac != NULL) {
        esp_codec_dev_set_out_mute(s_dac, true);
    }
}

static esp_err_t sink_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    s_out_vol = percent;
    if (s_dac != NULL) {
        esp_codec_dev_set_out_vol(s_dac, percent);
    }
    return ESP_OK;
}

static int sink_get_volume(void)
{
    return s_out_vol;
}

static const espix_audio_sink_ops_t s_es8311_sink = {
    .name       = "es8311",
    .label      = SINK_LABEL,
    .connected  = sink_connected,
    .format     = sink_format,
    .write      = i2s_write,
    .start      = sink_start,
    .suspend    = sink_suspend,
    .set_volume = sink_set_volume,
    .get_volume = sink_get_volume,
};

/* ---------------------------------------------------------------- source -- */

static bool adc_open(void)
{
    if (s_adc != NULL) {
        return true;
    }
    if (!hw_ensure()) {
        return false;
    }

    const audio_codec_if_t *codec = codec_new(ESP_CODEC_DEV_WORK_MODE_ADC, false, true);
    if (codec == NULL) {
        ESP_LOGE(TAG, "adc codec");
        return false;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec,
        .data_if  = s_data,
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
    };
    s_adc = esp_codec_dev_new(&dev_cfg);
    if (s_adc == NULL) {
        ESP_LOGE(TAG, "adc dev");
        return false;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 2,
        .sample_rate     = s_in_fmt.rate,
        .mclk_multiple   = 256,
    };
    if (esp_codec_dev_open(s_adc, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "adc open");
        return false;
    }
    esp_codec_dev_set_in_gain(s_adc, (float)CONFIG_ESPIX_I2S_INPUT_GAIN_DB);

#if CONFIG_ESPIX_I2S_ADC_ALC
    /*
     * The ES8311's ALC, which esp_codec_dev does not expose. Register 0x18 is
     * the enable (bit 7) and the window size (bits 3:0); 0x19 is the max level
     * (bits 7:4) and the min level (bits 3:0), each a 4-bit index into -30.1 dB
     * .. -6.0 dB. A quiet, distant source is the normal case here and the PGA
     * is already at its 42 dB maximum, so this is what lifts it.
     */
    esp_codec_dev_write_reg(s_adc, 0x18,
                            0x80 | (CONFIG_ESPIX_I2S_ADC_ALC_WINSIZE & 0x0F));
    esp_codec_dev_write_reg(s_adc, 0x19,
                            ((CONFIG_ESPIX_I2S_ADC_ALC_MAXLEVEL & 0x0F) << 4) |
                             (CONFIG_ESPIX_I2S_ADC_ALC_MINLEVEL & 0x0F));
    ESP_LOGI(TAG, "es8311 alc: win %d, max %d, min %d",
             CONFIG_ESPIX_I2S_ADC_ALC_WINSIZE, CONFIG_ESPIX_I2S_ADC_ALC_MAXLEVEL,
             CONFIG_ESPIX_I2S_ADC_ALC_MINLEVEL);
#endif
    ESP_LOGI(TAG, "es8311 adc: %u Hz, %u ch", (unsigned)s_in_fmt.rate,
             (unsigned)s_in_fmt.channels);
    return true;
}

static bool source_connected(void) { return true; }
static espix_audio_format_t source_format(void) { return s_in_fmt; }

static size_t i2s_read(void *pcm, size_t len)
{
    if (s_adc == NULL && !adc_open()) {
        return 0;
    }
    if (esp_codec_dev_read(s_adc, pcm, (int)len) != ESP_CODEC_DEV_OK) {
        return 0;
    }
    return len;
}

static void source_start(void)
{
    if (adc_open()) {
        esp_codec_dev_set_in_mute(s_adc, false);
    }
}

static void source_stop(void)
{
    if (s_adc != NULL) {
        esp_codec_dev_set_in_mute(s_adc, true);
    }
}

static const espix_audio_source_ops_t s_es8311_source = {
    .name      = "es8311",
    .label     = SOURCE_LABEL,
    .connected = source_connected,
    .format    = source_format,
    .read      = i2s_read,
    .start     = source_start,
    .stop      = source_stop,
};

esp_err_t espix_i2s_sink_register(void)
{
    return espix_audio_sink_register(&s_es8311_sink);
}

esp_err_t espix_i2s_source_register(void)
{
    return espix_audio_source_register(&s_es8311_source);
}

#endif /* CONFIG_ESPIX_I2S */
