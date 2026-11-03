/* The three structures capget() takes, as Linux's UAPI defines them, so htop's
 * capget call compiles. syscall() in ../syscall.h refuses it. */
#pragma once

#include <stdint.h>

#define _LINUX_CAPABILITY_VERSION_1 0x19980330
#define _LINUX_CAPABILITY_VERSION_2 0x20071026
#define _LINUX_CAPABILITY_VERSION_3 0x20080522

struct __user_cap_header_struct {
    uint32_t version;
    int      pid;
};

struct __user_cap_data_struct {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

typedef struct __user_cap_header_struct cap_user_header_t;
typedef struct __user_cap_data_struct   cap_user_data_t;
