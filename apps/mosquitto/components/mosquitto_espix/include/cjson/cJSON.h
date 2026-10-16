/*
 * The registry component exposes cJSON.h at the top of its include directory;
 * mosquitto includes <cjson/cJSON.h>. On a case-insensitive filesystem both are
 * the same path, and on Linux they are not, so this is the difference.
 */
#ifndef ESPIX_SHIM_CJSON_CJSON_H
#define ESPIX_SHIM_CJSON_CJSON_H

#include <cJSON.h>

#endif /* ESPIX_SHIM_CJSON_CJSON_H */
