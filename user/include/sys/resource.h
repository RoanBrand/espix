/* getpriority()/setpriority() for U-mode apps. picolibc declares neither, and
 * espix has no process priority a Unix nice value maps onto yet; a call is
 * refused rather than silently pretending to have changed one. */
#pragma once

#include_next <sys/resource.h>

#include <sys/types.h>

#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);
