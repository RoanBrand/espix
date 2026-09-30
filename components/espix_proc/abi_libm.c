/*
 * libm, published to apps.
 *
 * A game is the first app that needs mathematics, and it cannot carry its own:
 * an app is linked -fPIC -shared and the toolchain's libm.a is not PIC, so ld
 * refuses it -- "relocation R_RISCV_HI20 against 'a local symbol' can not be
 * used when making a shared object". The firmware, linked normally, already has
 * -lm on its link line; naming the calls here is what pulls their objects into
 * the image and makes them reachable by name at load, exactly as abi_libc.c
 * does for the rest of the C library.
 *
 * The S31 has a hardware FPU (SOC_CPU_HAS_FPU), so this is not the compromise
 * it would be on a chip without one: sqrtf, fabsf and floorf are single
 * instructions, and the trig calls are once per entity per frame rather than
 * per pixel. A hand-rolled CORDIC would be slower and less accurate here; if
 * sinf/cosf ever show up hot, a table indexed by a 16-bit angle is the next
 * step, not CORDIC.
 *
 * Everything here is pure computation over the caller's own memory, so it
 * passes the same test abi_libc.c's allowlist applies: no device access, no
 * espix state, nothing an app could misuse that printf() does not already
 * allow.
 */

#include <math.h>

#include "esp_elf.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

static const struct esp_elfsym s_libm_syms[] = {
    /* Trigonometric, single and double: an engine calls both spellings
     * depending on where the code was written. */
    ESP_ELFSYM_EXPORT(sinf),
    ESP_ELFSYM_EXPORT(cosf),
    ESP_ELFSYM_EXPORT(acos),
    ESP_ELFSYM_EXPORT(atan),
    ESP_ELFSYM_EXPORT(tanf),
    ESP_ELFSYM_EXPORT(asinf),
    ESP_ELFSYM_EXPORT(acosf),
    ESP_ELFSYM_EXPORT(atanf),
    ESP_ELFSYM_EXPORT(atan2f),
    ESP_ELFSYM_EXPORT(sin),
    ESP_ELFSYM_EXPORT(cos),
    ESP_ELFSYM_EXPORT(tan),
    ESP_ELFSYM_EXPORT(atan2),

    /* The roots and powers a renderer runs on. */
    ESP_ELFSYM_EXPORT(sqrtf),
    ESP_ELFSYM_EXPORT(sqrt),
    ESP_ELFSYM_EXPORT(powf),
    ESP_ELFSYM_EXPORT(pow),
    ESP_ELFSYM_EXPORT(expf),
    ESP_ELFSYM_EXPORT(exp),
    ESP_ELFSYM_EXPORT(logf),
    ESP_ELFSYM_EXPORT(log),
    ESP_ELFSYM_EXPORT(log10f),
    ESP_ELFSYM_EXPORT(log10),
    ESP_ELFSYM_EXPORT(hypotf),
    ESP_ELFSYM_EXPORT(hypot),

    /* Rounding, sign and the split of a float into its parts. The cheap ones
     * are still names an app cannot resolve without an entry. */
    ESP_ELFSYM_EXPORT(floorf),
    ESP_ELFSYM_EXPORT(floor),
    ESP_ELFSYM_EXPORT(ceilf),
    ESP_ELFSYM_EXPORT(ceil),
    ESP_ELFSYM_EXPORT(truncf),
    ESP_ELFSYM_EXPORT(trunc),
    ESP_ELFSYM_EXPORT(roundf),
    ESP_ELFSYM_EXPORT(round),
    ESP_ELFSYM_EXPORT(fabsf),
    ESP_ELFSYM_EXPORT(fabs),
    ESP_ELFSYM_EXPORT(copysignf),
    ESP_ELFSYM_EXPORT(copysign),
    ESP_ELFSYM_EXPORT(fmodf),
    ESP_ELFSYM_EXPORT(fmod),
    ESP_ELFSYM_EXPORT(fminf),
    ESP_ELFSYM_EXPORT(fmaxf),
    ESP_ELFSYM_EXPORT(fmin),
    ESP_ELFSYM_EXPORT(fmax),
    ESP_ELFSYM_EXPORT(modff),
    ESP_ELFSYM_EXPORT(modf),
    ESP_ELFSYM_EXPORT(ldexpf),
    ESP_ELFSYM_EXPORT(ldexp),
    ESP_ELFSYM_EXPORT(frexpf),
    ESP_ELFSYM_EXPORT(frexp),

    ESP_ELFSYM_END
};

void espix_proc_abi_libm_register(void)
{
    if (esp_elf_register_symbol(s_libm_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not publish libm to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "libm published to apps");
}
