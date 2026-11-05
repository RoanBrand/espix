/*
 * The libsensors API htop uses, and only that: seven calls, two enums and the
 * three structs they hand back. See ../libsensors_espix.c for why this is a
 * shim and not the real library.
 */
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SENSORS_FEATURE_TEMP = 0x0001,
} sensors_feature_type;

typedef enum {
    SENSORS_SUBFEATURE_TEMP_INPUT = 0x0000,
} sensors_subfeature_type;

typedef struct sensors_chip_name {
    char *prefix;
    int   bus;
    int   addr;
    char *path;
} sensors_chip_name;

typedef struct sensors_feature {
    char                *name;
    int                  number;
    sensors_feature_type type;
} sensors_feature;

typedef struct sensors_subfeature {
    char                    *name;
    int                      number;
    sensors_feature_type     type;
    sensors_subfeature_type  subtype;
} sensors_subfeature;

int sensors_init(FILE *input);
void sensors_cleanup(void);
const sensors_chip_name *sensors_get_detected_chips(const sensors_chip_name *match, int *nr);
const sensors_feature *sensors_get_features(const sensors_chip_name *name, int *nr);
const sensors_subfeature *sensors_get_subfeature(const sensors_chip_name *name,
                                                 const sensors_feature *feature,
                                                 sensors_subfeature_type type);
int sensors_get_value(const sensors_chip_name *name, int subfeat_nr, double *value);
char *sensors_get_label(const sensors_chip_name *name, const sensors_feature *feature);

#ifdef __cplusplus
}
#endif
