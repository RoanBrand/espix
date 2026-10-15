/*
 * JPEG, into a surface.
 *
 * The software path is TJpgDec in the ROM, which the S3 and the S31 both carry
 * -- so the fallback every target needs costs nothing but the call, and the S3
 * (which has no JPEG block at all) decodes in software because that is all there
 * is. The hardware path is the JPEG codec, which the S31 and the P4 have.
 *
 * That is the same split fill and blit make, and the same reason the software
 * path is first-class here rather than a fallback nobody exercises: on the S3 it
 * is the implementation.
 */

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"

#include "rom/tjpgd.h"

#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_dma2d.h"

#if SOC_JPEG_CODEC_SUPPORTED
#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#endif

#define TAG "image"

/* ------------------------------------------------------------------ */
/* Software: TJpgDec in the ROM                                        */
/* ------------------------------------------------------------------ */

/*
 * TJpgDec's working area: the Huffman and quantization tables at prepare time,
 * and the IDCT and MCU buffers at decompress time. The tables are the largest
 * part and are bounded -- 1K of dequantizers and up to 4 Huffman tables of 16
 * bits, 256 codes and 256 symbols -- which puts a 4:2:0 picture at about 6.4K
 * including the 512-byte stream buffer. 8K is that plus a margin, and TJpgDec
 * returns JDR_MEM1 rather than overrunning when it is not enough, so a picture
 * too big for this says so instead of corrupting the heap.
 */
#define JPEG_POOL 8192

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;

    espix_px_t    *px;      /* destination, at the image's own size */
    int            w;
} jpeg_job_t;

/*
 * The stream callback: as much as it asks for, and zero at the end.
 *
 * The one case that is not a read is the reason this comment is here. TJpgDec
 * asks for a NULL buffer when it wants to *skip* a segment it does not consume
 * -- "null pointer specifies to remove data from the stream" -- and the byte
 * count still has to be honoured. Every JPEG any encoder writes has a JFIF APP0
 * segment before the frames, which is not a segment TJpgDec reads, so the skip
 * is the first thing that happens to the very first file: a callback that only
 * handles reads memcpy()s a segment header into address zero.
 */
static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT n)
{
    jpeg_job_t *j = jd->device;
    size_t      take = j->len - j->pos;

    if (take > n) {
        take = n;
    }
    if (take == 0) {
        return 0;
    }
    if (buf != NULL) {
        memcpy(buf, j->data + j->pos, take);
    }
    j->pos += take;
    return (UINT)take;
}

/*
 * And the output callback, one rectangle at a time, in RGB888 -- the ROM's build
 * has JD_FORMAT 0. RGB565 is what everything here holds, so this is where the two
 * meet, and the shift-and-mask needs no rounding because 565 *is* 888 with the
 * low bits dropped.
 */
static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_job_t *j   = jd->device;
    const BYTE *src = bitmap;

    for (WORD y = rect->top; y <= rect->bottom; y++) {
        espix_px_t *dst = j->px + (size_t)y * j->w + rect->left;

        for (WORD x = rect->left; x <= rect->right; x++) {
            const uint8_t r = *src++;
            const uint8_t g = *src++;
            const uint8_t b = *src++;

            *dst++ = (espix_px_t)(((r & 0xF8u) << 8) |
                                  ((g & 0xFCu) << 3) |
                                   (b >> 3));
        }
    }
    return 1;
}

static espix_surface_t *jpeg_sw(const uint8_t *jpg, size_t len)
{
    espix_surface_t *s    = NULL;
    void            *pool = NULL;
    jpeg_job_t      *job  = NULL;
    JDEC             jd;

    if (jpg == NULL || len < 4) {
        return NULL;
    }

    pool = heap_caps_malloc(JPEG_POOL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    job  = calloc(1, sizeof(*job));
    if (pool == NULL || job == NULL) {
        goto done;
    }

    job->data = jpg;
    job->len  = len;

    JRESULT r = jd_prepare(&jd, jpeg_in, pool, JPEG_POOL, job);
    if (r != JDR_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "not a JPEG TJpgDec can read (%d)",
                   (int)r);
        goto done;
    }
    if (jd.width == 0 || jd.height == 0) {
        goto done;
    }

    s = espix_surface_new((int)jd.width, (int)jd.height);
    if (s == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no room for a %ux%u image",
                   (unsigned)jd.width, (unsigned)jd.height);
        goto done;
    }

    job->px = espix_surface_pixels(s);
    job->w  = (int)jd.width;

    r = jd_decomp(&jd, jpeg_out, 0);        /* 0: no descaling */
    if (r != JDR_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "decode failed (%d)", (int)r);
        espix_surface_free(s);
        s = NULL;
    }

done:
    free(job);
    heap_caps_free(pool);
    return s;
}

/* ------------------------------------------------------------------ */
/* Hardware: the JPEG codec                                            */
/* ------------------------------------------------------------------ */

#if SOC_JPEG_CODEC_SUPPORTED

/*
 * The engine is kept rather than acquired per picture. Acquiring one sets up an
 * interrupt, an event queue and a mutex, which is not the cost of decoding a
 * JPEG -- and measurably would be most of the cost of a small one. Created on
 * the first hardware decode and alive after that.
 */
/* How long the codec waits for the 2D-DMA engine before giving up. */
#define JPEG_DMA_WAIT_MS 1000

static jpeg_decoder_handle_t s_engine;
static SemaphoreHandle_t     s_engine_lock;
static portMUX_TYPE          s_engine_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t engine_lock(void)
{
    if (s_engine_lock == NULL) {
        portENTER_CRITICAL(&s_engine_mux);
        if (s_engine_lock == NULL) {
            s_engine_lock = xSemaphoreCreateMutex();
        }
        portEXIT_CRITICAL(&s_engine_mux);
    }
    return s_engine_lock;
}

static jpeg_decoder_handle_t engine_get(void)
{
    if (s_engine != NULL) {
        return s_engine;
    }

    const jpeg_decode_engine_cfg_t cfg = { .timeout_ms = 2000 };
    jpeg_decoder_handle_t          h   = NULL;

    if (jpeg_new_decoder_engine(&cfg, &h) != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no JPEG decode engine");
        return NULL;
    }
    s_engine = h;
    return s_engine;
}

/*
 * The codec works in whole MCUs, so a subsampled picture comes back padded to
 * the MCU in both directions -- 16x16 for 4:2:0, which turns 480x330 into
 * 480x336 -- and the row pitch of the output is that padded width, not the
 * visible one. Rendering the visible width out of each row is the whole of the
 * mapping, and the padded width is read back from the size the codec reports
 * rather than assumed.
 */
static void mcu_size(jpeg_down_sampling_type_t s, int *mx, int *my)
{
    switch (s) {
    case JPEG_DOWN_SAMPLING_YUV420: *mx = 16; *my = 16; break;
    case JPEG_DOWN_SAMPLING_YUV422: *mx = 16; *my = 8;  break;
    default:                        *mx = 8;  *my = 8;  break;
    }
}

static espix_surface_t *jpeg_hw(const uint8_t *jpg, size_t len)
{
    jpeg_decode_picture_info_t info;
    espix_surface_t           *s       = NULL;
    uint8_t                   *out     = NULL;
    size_t                     out_cap = 0;
    SemaphoreHandle_t          lock;

    if (jpg == NULL || len < 4) {
        return NULL;
    }
    if (jpeg_decoder_get_info(jpg, (uint32_t)len, &info) != ESP_OK) {
        return NULL;
    }

    const int w = (int)info.width;
    const int h = (int)info.height;
    if (w <= 0 || h <= 0) {
        return NULL;
    }

    int mx, my;
    mcu_size(info.sample_method, &mx, &my);
    const int pw = (w + mx - 1) / mx * mx;
    const int ph = (h + my - 1) / my * my;

    lock = engine_lock();
    if (lock == NULL) {
        return NULL;
    }
    xSemaphoreTake(lock, portMAX_DELAY);

    jpeg_decoder_handle_t eng = engine_get();
    if (eng == NULL) {
        goto done;
    }

    const jpeg_decode_memory_alloc_cfg_t mcfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    out = jpeg_alloc_decoder_mem((size_t)pw * (size_t)ph * sizeof(espix_px_t),
                                 &mcfg, &out_cap);
    if (out == NULL) {
        goto done;
    }

    /*
     * BGR rather than RGB, and the names are the least helpful thing about this
     * driver: what rgb_order selects is the *byte* order the 2D-DMA scrambles
     * the output into, and RGB565 is the one format where the two choices are
     * not an endian swap of each other. RGB is SCRAMBLE_ORDER_BYTE2_0_1 and lays
     * the word down big-endian, which is not what a 16-bit espix_px_t in memory
     * is; BGR is the driver's default order and is. The components are the same
     * either way -- BT.601 decides those -- and the picture is compared against
     * the software path below, so this is measured rather than assumed.
     */
    const jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    uint32_t got = 0;
    if (!espix_dma2d_take(pdMS_TO_TICKS(JPEG_DMA_WAIT_MS))) {
        goto done;
    }
    const esp_err_t derr = jpeg_decoder_process(eng, &dcfg, jpg, (uint32_t)len, out,
                                                (uint32_t)out_cap, &got);
    espix_dma2d_give();
    if (derr != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "codec refused a %dx%d picture", w, h);
        goto done;
    }

    int stride = pw;
    if (got >= (uint32_t)ph * sizeof(espix_px_t)) {
        const int back = (int)(got / ((uint32_t)ph * sizeof(espix_px_t)));
        if (back >= w) {
            stride = back;
        }
    }

    s = espix_surface_new(w, h);
    if (s == NULL) {
        goto done;
    }

    espix_px_t       *dst = espix_surface_pixels(s);
    const espix_px_t *src = (const espix_px_t *)out;

    for (int y = 0; y < h; y++) {
        memcpy(&dst[(size_t)y * w], &src[(size_t)y * stride],
               (size_t)w * sizeof(espix_px_t));
    }

done:
    heap_caps_free(out);            /* jpeg_alloc_decoder_mem is a heap_caps one */
    xSemaphoreGive(lock);
    return s;
}

/*
 * The encoder, beside the decoder and behind the same lock, because it is the
 * same block on the die: one JPEG codec, two directions.
 */
static jpeg_encoder_handle_t s_enc;

static jpeg_encoder_handle_t encoder_get(void)
{
    if (s_enc != NULL) {
        return s_enc;
    }

    const jpeg_encode_engine_cfg_t cfg = { .timeout_ms = 2000 };
    jpeg_encoder_handle_t          h   = NULL;

    if (jpeg_new_encoder_engine(&cfg, &h) != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no JPEG encode engine");
        return NULL;
    }
    s_enc = h;
    return s_enc;
}

/*
 * The frame the encoder reads, and the one it writes.
 *
 * Both from jpeg_alloc_encoder_mem rather than the heap, because the driver
 * builds its DMA descriptors out of them and will not take a buffer of its own
 * choosing -- measured: handed a plain PSRAM block it answers ESP_ERR_INVALID_ARG
 * and produces nothing. The input is handed back to the caller so the pixels go
 * into it once: a staging copy and then a copy into here would be two copies of
 * 600 KiB a frame, which is most of what this is meant to save.
 *
 * The caller owns the frame between the two calls, so one encoder is one
 * caller at a time -- which is the RFB server's single connection.
 */
static uint8_t *s_enc_in;
static size_t   s_enc_in_px;
static uint8_t *s_enc_out;
static size_t   s_enc_out_cap;

espix_px_t *espix_display_jpeg_input(size_t pixels)
{
    if (pixels == 0) {
        return NULL;
    }
    if (encoder_get() == NULL) {
        return NULL;
    }
    if (s_enc_in != NULL && pixels <= s_enc_in_px) {
        return (espix_px_t *)s_enc_in;
    }

    const jpeg_encode_memory_alloc_cfg_t cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    /* Whole cache lines, so the PPA may describe the buffer as a picture. */
    const size_t want = (pixels * sizeof(espix_px_t) + 127) & ~(size_t)127;
    size_t       got  = 0;
    uint8_t     *buf  = jpeg_alloc_encoder_mem(want, &cfg, &got);

    if (buf == NULL) {
        return NULL;
    }
    heap_caps_free(s_enc_in);
    s_enc_in    = buf;
    s_enc_in_px = got / sizeof(espix_px_t);
    return (espix_px_t *)s_enc_in;
}

size_t espix_display_jpeg_encode(int w, int h, int quality, void *out,
                                 size_t out_cap)
{
    jpeg_encoder_handle_t enc = encoder_get();

    if (enc == NULL || s_enc_in == NULL || out == NULL
        || w <= 0 || h <= 0 || out_cap == 0
        || (size_t)w * (size_t)h > s_enc_in_px) {
        return 0;
    }

    /*
     * The output is the driver's too, and grows to the largest frame asked for.
     * An RGB565 frame at quality 100 can be larger than its pixels, so the
     * allowance is two bytes a pixel plus room for the headers -- measured at
     * q80, a 640x480 photograph comes back at about 9 KB, so this is slack.
     */
    const size_t want = (size_t)w * (size_t)h * sizeof(espix_px_t) + 1024;

    if (s_enc_out == NULL || s_enc_out_cap < want) {
        const jpeg_encode_memory_alloc_cfg_t cfg = {
            .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
        };
        size_t       got = 0;
        uint8_t     *buf = jpeg_alloc_encoder_mem(want, &cfg, &got);

        if (buf == NULL) {
            return 0;
        }
        heap_caps_free(s_enc_out);
        s_enc_out     = buf;
        s_enc_out_cap = got;
    }

    SemaphoreHandle_t lock = engine_lock();
    if (lock == NULL) {
        return 0;
    }
    xSemaphoreTake(lock, portMAX_DELAY);

    const jpeg_encode_cfg_t ecfg = {
        .width         = (uint32_t)w,
        .height        = (uint32_t)h,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = (uint32_t)quality,
        .pixel_reverse = false,
    };
    uint32_t got = 0;
    bool     ok  = false;

    if (espix_dma2d_take(pdMS_TO_TICKS(JPEG_DMA_WAIT_MS))) {
        ok = (jpeg_encoder_process(enc, &ecfg, s_enc_in,
                                   (uint32_t)((size_t)w * (size_t)h
                                              * sizeof(espix_px_t)),
                                   s_enc_out,
                                   (uint32_t)(s_enc_out_cap < out_cap
                                              ? s_enc_out_cap : out_cap),
                                   &got) == ESP_OK);
        espix_dma2d_give();
    }
    xSemaphoreGive(lock);

    if (!ok) {
        return 0;
    }
    memcpy(out, s_enc_out, got);
    return got;
}

/*
 * The same decode, into pixels the caller already has.
 *
 * Only when the picture is exactly the size of the surface it is going into.
 * The codec writes whole MCUs and reports the pitch it used afterwards, so a
 * destination that is not the padded geometry would have to be decoded
 * elsewhere and copied; that case is the caller's to detect (a non-multiple of
 * 16), and espix_image_jpeg_into() below falls back to a decode and a copy.
 *
 * The direct write is the point: a video player decodes the same shape hundreds
 * of times, and an allocation and a 600 KiB memcpy per frame is most of what a
 * frame costs.
 */
static esp_err_t jpeg_hw_into(espix_surface_t *dst, const uint8_t *jpg, size_t len)
{
    jpeg_decode_picture_info_t info;
    SemaphoreHandle_t          lock;

    if (jpeg_decoder_get_info(jpg, (uint32_t)len, &info) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    const int w = (int)info.width;
    const int h = (int)info.height;

    int mx, my;
    mcu_size(info.sample_method, &mx, &my);

    if (w <= 0 || h <= 0
        || ((w + mx - 1) / mx * mx) != espix_surface_width(dst)
        || ((h + my - 1) / my * my) != espix_surface_height(dst)) {
        return ESP_ERR_INVALID_SIZE;
    }

    lock = engine_lock();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(lock, portMAX_DELAY);

    esp_err_t   err = ESP_FAIL;
    jpeg_decoder_handle_t eng = engine_get();

    if (eng == NULL) {
        goto done;
    }

    const jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    const uint32_t bytes = (uint32_t)((size_t)espix_surface_width(dst)
                                      * (size_t)espix_surface_height(dst)
                                      * sizeof(espix_px_t));
    uint32_t got = 0;

    if (espix_dma2d_take(pdMS_TO_TICKS(JPEG_DMA_WAIT_MS))) {
        if (jpeg_decoder_process(eng, &dcfg, jpg, (uint32_t)len,
                                 (uint8_t *)espix_surface_pixels(dst), bytes,
                                 &got) == ESP_OK) {
            err = ESP_OK;
        }
        espix_dma2d_give();
    }

done:
    xSemaphoreGive(lock);
    return err;
}

static const char *jpeg_hw_name(void)
{
    return "JPEG codec";
}

#else   /* no codec, so no hardware path to name */

static espix_surface_t *jpeg_hw(const uint8_t *jpg, size_t len)
{
    (void)jpg; (void)len;
    return NULL;
}

static esp_err_t jpeg_hw_into(espix_surface_t *dst, const uint8_t *jpg, size_t len)
{
    (void)dst; (void)jpg; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

espix_px_t *espix_display_jpeg_input(size_t pixels)
{
    (void)pixels;
    return NULL;
}

size_t espix_display_jpeg_encode(int w, int h, int quality, void *out,
                                 size_t out_cap)
{
    (void)w; (void)h; (void)quality; (void)out; (void)out_cap;
    return 0;
}

static const char *jpeg_hw_name(void)
{
    return NULL;
}

#endif

/* ------------------------------------------------------------------ */
/* What the rest of espix uses                                         */
/* ------------------------------------------------------------------ */

espix_surface_t *espix_image_jpeg(const uint8_t *jpg, size_t len)
{
    espix_surface_t *s = jpeg_hw(jpg, len);

    if (s != NULL) {
        return s;
    }
    return jpeg_sw(jpg, len);
}

/*
 * Decode into a surface the caller already has, which must be exactly the
 * picture's size. True when the surface now holds it.
 *
 * The hardware path is a codec write straight into those pixels when the
 * geometry is MCU-aligned, which every video frame at a multiple of 16 is; the
 * software path, and any picture that is not, decodes into a surface of its own
 * and copies. Either way the caller does not have to know which one happened.
 */
esp_err_t espix_image_jpeg_into(espix_surface_t *dst, const uint8_t *jpg, size_t len)
{
    if (dst == NULL || jpg == NULL || len < 4) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t hw = jpeg_hw_into(dst, jpg, len);
    if (hw == ESP_OK) {
        return ESP_OK;
    }

    espix_surface_t *tmp = jpeg_sw(jpg, len);
    if (tmp == NULL) {
        return ESP_FAIL;
    }

    const int w = espix_surface_width(tmp);
    const int h = espix_surface_height(tmp);

    if (w != espix_surface_width(dst) || h != espix_surface_height(dst)) {
        espix_surface_free(tmp);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(espix_surface_pixels(dst), espix_surface_pixels(tmp),
           (size_t)w * (size_t)h * sizeof(espix_px_t));
    espix_surface_free(tmp);
    return ESP_OK;
}

/*
 * How much of the codec's picture the software path agrees with: the percentage
 * of pixels whose three channels are each within JPEG_TOL of the software
 * path's, or -1 when the two cannot be compared at all -- which is the case that
 * matters most, because a different size means the row pitch or the visible
 * width was taken from the padded one.
 *
 * Not equality, and not an average. The two decoders do not produce the same
 * picture: they differ in IDCT rounding and in how chroma is upsampled, which on
 * a photograph is a few steps on a tenth of the pixels, and an average absorbs
 * exactly that. A count above a threshold that both correct decoders reach
 * catches the mistakes that still look like a picture -- a wrong byte order, a
 * wrong stride, a width off by the padding -- without calling a correct codec
 * wrong. Measured: the codec agrees with a host decoder on 92% of the pixels of
 * the test photograph, and the same output with the bytes the other way round
 * agrees on almost none.
 */
static int jpeg_agreement(espix_surface_t *a, espix_surface_t *b)
{
    static const int TOL = 16;          /* of 255, per channel: about six percent */

    if (a == NULL || b == NULL) {
        return -1;
    }

    const int w = espix_surface_width(a);
    const int h = espix_surface_height(a);
    if (w != espix_surface_width(b) || h != espix_surface_height(b)) {
        return -1;
    }

    const espix_px_t *pa = espix_surface_pixels(a);
    const espix_px_t *pb = espix_surface_pixels(b);
    if (pa == NULL || pb == NULL) {
        return -1;
    }

    const size_t n = (size_t)w * (size_t)h;
    uint32_t     ok = 0;

    for (size_t i = 0; i < n; i++) {
        const uint32_t x = pa[i];
        const uint32_t y = pb[i];

        /* Compared at 8 bits a channel, which is how a tolerance is worth
         * stating; the surface is 5/6/5 and the shift is the whole conversion
         * because 565 is 888 with the low bits dropped. */
        if (abs((int)(((x >> 11) & 0x1F) << 3) - (int)(((y >> 11) & 0x1F) << 3)) > TOL ||
            abs((int)(((x >>  5) & 0x3F) << 2) - (int)(((y >>  5) & 0x3F) << 2)) > TOL ||
            abs((int)( (x        & 0x1F) << 3) - (int)( (y        & 0x1F) << 3)) > TOL) {
            continue;
        }
        ok++;
    }
    return (int)(100u * ok / n);
}

bool espix_image_bench(espix_display_bench_t *row, const uint8_t *jpg, size_t len)
{
    if (row == NULL || jpg == NULL || len < 4) {
        return false;
    }

    const int64_t    t0 = esp_timer_get_time();
    espix_surface_t *sw = jpeg_sw(jpg, len);
    const int64_t    t1 = esp_timer_get_time();

    if (sw == NULL) {
        return false;
    }

    memset(row, 0, sizeof(*row));

    row->op          = "jpeg";
    row->w           = espix_surface_width(sw);
    row->h           = espix_surface_height(sw);
    row->iters       = 1;
    row->px_per_iter = (uint64_t)row->w * (uint64_t)row->h;
    row->us_sw       = (uint32_t)(t1 - t0);
    row->hw          = jpeg_hw_name();

    espix_surface_t *hw = jpeg_hw(jpg, len);
    const int64_t    t2 = esp_timer_get_time();

    if (hw != NULL) {
        const int agree = jpeg_agreement(sw, hw);

        row->us_hw = (uint32_t)(t2 - t1);
        row->hw_ok = agree >= 80;

        espix_klog(ESPIX_KLOG_INFO, TAG,
                   "codec against TJpgDec on %dx%d: %d%% of pixels agree%s",
                   row->w, row->h, agree, row->hw_ok ? "" : "  (MISMATCH)");
    }

    /*
     * And the same picture back through the encoder. The numbers a Tight+JPEG
     * update needs are bytes and milliseconds, and the round trip is the check
     * that the encoder's pixel order is the decoder's: a picture that does not
     * decode back to itself is a byte order nobody would notice until a client
     * showed it.
     */
    espix_surface_t *src = (hw != NULL) ? hw : sw;
    const int        ew  = espix_surface_width(src);
    const int        eh  = espix_surface_height(src);
    const size_t     cap = (size_t)ew * (size_t)eh * sizeof(espix_px_t) + 1024;
    void            *jbuf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (jbuf != NULL) {
        const int64_t t3 = esp_timer_get_time();

        espix_px_t *in = espix_display_jpeg_input((size_t)ew * (size_t)eh);
        size_t      n  = 0;

        if (in != NULL) {
            memcpy(in, espix_surface_pixels(src),
                   (size_t)ew * (size_t)eh * sizeof(espix_px_t));
            n = espix_display_jpeg_encode(ew, eh, 80, jbuf, cap);
        }
        const int64_t t4 = esp_timer_get_time();

        if (n > 0) {
            espix_surface_t *back  = jpeg_hw((const uint8_t *)jbuf, n);
            const int        agree = jpeg_agreement(src, back);

            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "codec encoded %dx%d at q80: %u bytes in %d ms, "
                       "%d%% back%s",
                       ew, eh, (unsigned)n, (int)((t4 - t3) / 1000), agree,
                       agree >= 80 ? "" : "  (MISMATCH)");
            espix_surface_free(back);
        } else {
            espix_klog(ESPIX_KLOG_WARN, TAG, "the encoder produced nothing");
        }
        heap_caps_free(jbuf);
    }

    espix_surface_free(sw);
    espix_surface_free(hw);
    return true;
}
