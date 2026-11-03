/*
 * libsensors for espix: one chip, one temperature.
 *
 * The real libsensors is a hwmon scanner and a configuration-file parser that
 * exists to describe however many chips a PC has. espix has exactly one
 * sensor -- the ESP32's on-die temperature -- and htop asks for it through
 * seven functions. Pulling the library in would add a parser and a sysfs walk
 * to read one register, so the seven functions are here instead, over the
 * kernel's Linux-shaped thermal zone.
 *
 * htop maps a chip named "coretemp" to the CPU package, and because
 * sensors_get_label() returns NULL it treats temp1 as the package temperature
 * and copies it to every core. That is the truth on this chip: one sensor for
 * the die, not one per hart.
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "sensors/sensors.h"

#define THERMAL_ZONE_TEMP "/sys/class/thermal/thermal_zone0/temp"

static const sensors_chip_name s_chip = {
    .prefix = "coretemp",
};

static const sensors_feature s_feature = {
    .name   = "temp1",
    .number = 0,
    .type   = SENSORS_FEATURE_TEMP,
};

static const sensors_subfeature s_subfeature = {
    .name    = "temp1_input",
    .number  = 0,
    .type    = SENSORS_FEATURE_TEMP,
    .subtype = SENSORS_SUBFEATURE_TEMP_INPUT,
};

int sensors_init(FILE *input)
{
    (void)input;
    if (access(THERMAL_ZONE_TEMP, R_OK) != 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

void sensors_cleanup(void)
{
}

const sensors_chip_name *sensors_get_detected_chips(const sensors_chip_name *match, int *nr)
{
    (void)match;
    if (nr == NULL || *nr != 0) {
        return NULL;
    }
    (*nr)++;
    return &s_chip;
}

const sensors_feature *sensors_get_features(const sensors_chip_name *name, int *nr)
{
    (void)name;
    if (nr == NULL || *nr != 0) {
        return NULL;
    }
    (*nr)++;
    return &s_feature;
}

const sensors_subfeature *sensors_get_subfeature(const sensors_chip_name *name,
                                                 const sensors_feature *feature,
                                                 sensors_subfeature_type type)
{
    (void)name;
    if (feature == NULL || feature->number != s_feature.number ||
        type != SENSORS_SUBFEATURE_TEMP_INPUT) {
        return NULL;
    }
    return &s_subfeature;
}

int sensors_get_value(const sensors_chip_name *name, int subfeat_nr, double *value)
{
    (void)name;
    if (subfeat_nr != s_subfeature.number || value == NULL) {
        return -1;
    }

    FILE *f = fopen(THERMAL_ZONE_TEMP, "r");
    if (f == NULL) {
        return -1;
    }
    long millidegrees;
    const int ok = fscanf(f, "%ld", &millidegrees);
    fclose(f);
    if (ok != 1) {
        return -1;
    }
    *value = (double)millidegrees / 1000.0;
    return 0;
}

char *sensors_get_label(const sensors_chip_name *name, const sensors_feature *feature)
{
    (void)name;
    (void)feature;
    return NULL;
}
