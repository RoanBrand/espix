/* A U-mode thread's TLS block: its size and alignment (user/uapp.ld), and its setup (tls.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

extern char __tls_size[], __tls_align[];

static inline size_t _tls_size(void)  { return (size_t)(uintptr_t)__tls_size; }
static inline size_t _tls_align(void) { return (size_t)(uintptr_t)__tls_align; }

void _init_tls(void *tls);
void _set_tls(void *tls);
