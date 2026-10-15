/*
 * One user of the 2D-DMA engine at a time: the PPA and the JPEG codec share its
 * channels, and on this silicon a channel has been seen to wedge when they ran
 * side by side. Bounded: false means do the work another way.
 */
#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"

bool espix_dma2d_take(TickType_t wait);
void espix_dma2d_give(void);
