/* major()/minor()/makedev(), with Linux's 32-bit dev_t encoding: 8 bits of
 * major in the low word, 12 bits below it. espix's dev_t is 32 bits, so there
 * is no high-major half to encode. */
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
    return (dev_t)(((maj & 0xfffu) << 8) | (min & 0xffu) |
                   ((min & 0xfff00u) << 12));
}
