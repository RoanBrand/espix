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

#include "rom/tjpgd.h"

#include "espix_display.h"
#include "espix_kernel.h"

#define TAG "image"

/*
 * TJpgDec's working area: the Huffman and quantization tables at prepare time,
 * and the IDCT and MCU buffers at decompress time. Its own documentation puts the
 * minimum near 3.1K plus two MCU rows, so 8K is comfortable for a 4:2:0 image
 * with 16x16 MCUs. Internal RAM, and freed the moment the decode ends.
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

espix_surface_t *espix_image_jpeg(const uint8_t *jpg, size_t len)
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
    } else {
        espix_klog(ESPIX_KLOG_INFO, TAG, "decoded %ux%u",
                   (unsigned)jd.width, (unsigned)jd.height);
    }

done:
    free(job);
    heap_caps_free(pool);
    return s;
}