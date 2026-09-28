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

#if SOC_JPEG_CODEC_SUPPORTED
#include "driver/jpeg_decode.h"
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
    if (jpeg_decoder_process(eng, &dcfg, jpg, (uint32_t)len, out,
                             (uint32_t)out_cap, &got) != ESP_OK) {
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

    espix_surface_free(sw);
    espix_surface_free(hw);
    return true;
}
