/* major(), minor() and makedev(), in Linux's 32-bit dev_t encoding: what /proc's tty_nr is in. */
#pragma once

#include <sys/types.h>

static inline unsigned int major(dev_t dev)
{
    return ((unsigned int)dev >> 8) & 0xfffu;
}

static inline unsigned int minor(dev_t dev)
{
    return ((unsigned int)dev & 0xffu) | (((unsigned int)dev >> 12) & 0xfff00u);
}

static inline dev_t makedev(unsigned int maj, unsigned int min)
{
    return (dev_t)(((maj & 0xfffu) << 8) | (min & 0xffu) | ((min & 0xfff00u) << 12));
}
