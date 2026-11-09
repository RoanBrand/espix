/* Where an M-mode app's fault resumes (app_fault.c): its own function's window, a2-a4 the arguments. */
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ARCH_XTENSA

    .section .iram1.espix_app_fault, "ax"
    .global espix_app_fault_tramp
    .type   espix_app_fault_tramp, @function
    .align  4
espix_app_fault_tramp:
    mov     a8, a4
    mov     a7, a3
    mov     a6, a2
    call4   espix_app_fault
1:  j       1b
    .size   espix_app_fault_tramp, . - espix_app_fault_tramp

#endif
