/*
 * Deep6 Excursion parsing
 *
 * Copyright (C) 2020 Ryan Gardner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

// TODO - implement this. It's LITERALLY a copy/paste of deepblue_parser.c with a find / replace on the names

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "deepsix.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"
#include "field-cache.h"

#define C_ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

#define MAXFIELDS 128

struct msg_desc;

typedef struct deepsix_parser_t {
    dc_parser_t base;

    dc_sample_callback_t callback;
    void *userdata;

    // 20 sec for scuba, 1 sec for freedives
    int sample_interval;

    // Common fields
    struct dc_field_cache cache;
} deepsix_parser_t;

static dc_status_t deepsix_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t deepsix_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t deepsix_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t deepsix_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t deepsix_parser_vtable = {
        sizeof(deepsix_parser_t),
        DC_FAMILY_DEEP6,
        deepsix_parser_set_data, /* set_data */
        deepsix_parser_get_datetime, /* datetime */
        deepsix_parser_get_field, /* fields */
        deepsix_parser_samples_foreach, /* samples_foreach */
        NULL /* destroy */
};

dc_status_t
deep6_parser_create (dc_parser_t **out, dc_context_t *context)
{
    deepsix_parser_t *parser = NULL;

    if (out == NULL)
        return DC_STATUS_INVALIDARGS;

    // Allocate memory.
    parser = (deepsix_parser_t *) dc_parser_allocate (context, &deepsix_parser_vtable);
    if (parser == NULL) {
        ERROR (context, "Failed to allocate memory.");
        return DC_STATUS_NOMEMORY;
    }

    *out = (dc_parser_t *) parser;

    return DC_STATUS_SUCCESS;
}

static double
pressure_to_depth(unsigned int mbar)
{
    // Specific weight of seawater (millibar to cm)
    const double specific_weight = 1.024 * 0.980665;

    // Absolute pressure, subtract surface pressure
    if (mbar < 1013)
        return 0.0;
    mbar -= 1013;
    return mbar / specific_weight / 100.0;
}

static dc_status_t
deepsix_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
    deepsix_parser_t *deepsix = (deepsix_parser_t *) abstract;
    const unsigned char *hdr = data;
    const unsigned char *profile = data + EXCURSION_HDR_SIZE;
    unsigned int divetime, maxpressure, sampling_rate, lowest_water_temp, average_pressure;
    dc_gasmix_t gasmix = {0, };

    if (size < EXCURSION_HDR_SIZE)
        return DC_STATUS_IO;

    deepsix->callback = NULL;
    deepsix->userdata = NULL;
    memset(&deepsix->cache, 0, sizeof(deepsix->cache));

    // dive type - scuba = 0
    int divetype = array_uint32_le(&hdr[4]);

    int profile_data_len = array_uint32_le(&data[8]);

    // LE32 at 20 is the dive duration in ms
    divetime = array_uint32_le(&hdr[20]);
    // SCUBA - divetime in ms for everything
    divetime /= 1000;
    // sample rate is in seconds
    deepsix->sample_interval = array_uint32_le(&hdr[24]);
    maxpressure = array_uint32_le(&hdr[28]);
//    lowest_water_temp = array_uint32_le(hdr[32]);
//    average_pressure = array_uint32_le(hdr[36]);


    // Byte at 2 is 'activity type' (2 = scuba, 3 = gauge, 4 = freedive)
    // Byte at 3 is O2 percentage
    switch (divetype) {
        case 0:
            // TODO: is the 02 in the log info somewhere? I can't find it - Maybe O2Ratio?
            gasmix.oxygen = 21 / 100.0;
            DC_ASSIGN_IDX(deepsix->cache, GASMIX, 0, gasmix);
            DC_ASSIGN_FIELD(deepsix->cache, GASMIX_COUNT, 1);
            DC_ASSIGN_FIELD(deepsix->cache, DIVEMODE, DC_DIVEMODE_OC);
            break;
        //todo - validate the other modes
        case 1:
            // GAUGE - divetime in minutes
            DC_ASSIGN_FIELD(deepsix->cache, DIVEMODE, DC_DIVEMODE_GAUGE);
            break;
        case 2:
            // FREEDIVE - divetime in seconds
            DC_ASSIGN_FIELD(deepsix->cache, DIVEMODE, DC_DIVEMODE_FREEDIVE);
            deepsix->sample_interval = 1;
            break;
        default:
            ERROR (abstract->context, "DeepSix: unknown activity type '%02x'", data[2]);
            break;
    }

    DC_ASSIGN_FIELD(deepsix->cache, DIVETIME, divetime);
    DC_ASSIGN_FIELD(deepsix->cache, MAXDEPTH, pressure_to_depth(maxpressure));


    return DC_STATUS_SUCCESS;
}

// The layout of the header in the 'data' is
//  0: LE16 dive number
//  2: dive type byte?
//  3: O2 percentage byte
//  4: unknown
//  5: unknown
//  6: LE16 year
//  8: day of month
//  9: month
// 10: minute
// 11: hour
// 12: LE16 dive time
// 14: LE16 ??
// 16: LE16 surface pressure?
// 18: LE16 ??
// 20: LE16 ??
// 22: LE16 max depth pressure
// 24: LE16 water temp
// 26: LE16 ??
// 28: LE16 ??
// 30: LE16 ??
// 32: LE16 ??
// 34: LE16 ??
static dc_status_t
deepsix_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
    deepsix_parser_t *deepsix = (deepsix_parser_t *) abstract;
    const unsigned char *data = deepsix->base.data;
    int len = deepsix->base.size;

    if (len < 256)
        return DC_STATUS_IO;
    datetime->year = data[12] + 2000;
    datetime->month = data[13];
    datetime->day = data[14];
    datetime->hour = data[15];
    datetime->minute = data[16];
    datetime->second = data[17];
    datetime->timezone = DC_TIMEZONE_NONE;

    return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
    deepsix_parser_t *deepsix = (deepsix_parser_t *) abstract;

    if (!value)
        return DC_STATUS_INVALIDARGS;

    /* This whole sequence should be standardized */
    if (!(deepsix->cache.initialized & (1 << type)))
        return DC_STATUS_UNSUPPORTED;

    switch (type) {
        case DC_FIELD_DIVETIME:
            return DC_FIELD_VALUE(deepsix->cache, value, DIVETIME);
        case DC_FIELD_MAXDEPTH:
            return DC_FIELD_VALUE(deepsix->cache, value, MAXDEPTH);
        case DC_FIELD_AVGDEPTH:
            return DC_FIELD_VALUE(deepsix->cache, value, AVGDEPTH);
        case DC_FIELD_GASMIX_COUNT:
        case DC_FIELD_TANK_COUNT:
            return DC_FIELD_VALUE(deepsix->cache, value, GASMIX_COUNT);
        case DC_FIELD_GASMIX:
            if (flags >= MAXGASES)
                return DC_STATUS_UNSUPPORTED;
            return DC_FIELD_INDEX(deepsix->cache, value, GASMIX, flags);
        case DC_FIELD_SALINITY:
            return DC_FIELD_VALUE(deepsix->cache, value, SALINITY);
        case DC_FIELD_ATMOSPHERIC:
            return DC_FIELD_VALUE(deepsix->cache, value, ATMOSPHERIC);
        case DC_FIELD_DIVEMODE:
            return DC_FIELD_VALUE(deepsix->cache, value, DIVEMODE);
        case DC_FIELD_TANK:
            return DC_STATUS_UNSUPPORTED;
        case DC_FIELD_STRING:
            return dc_field_get_string(&deepsix->cache, flags, (dc_field_string_t *)value);
        default:
            return DC_STATUS_UNSUPPORTED;
    }
    return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
    deepsix_parser_t *deepsix = (deepsix_parser_t *) abstract;
    const unsigned char *data = deepsix->base.data;
    int len = deepsix->base.size, i;

    deepsix->callback = callback;
    deepsix->userdata = userdata;

    // Skip the header information
    if (len < EXCURSION_HDR_SIZE)
        return DC_STATUS_IO;
    data += EXCURSION_HDR_SIZE;
    len -= EXCURSION_HDR_SIZE;

    // The rest should be samples every 20s with temperature and depth
    for (i = 0; i < len/6; i++) {
        dc_sample_value_t sample = {0};
        data += 2;
        unsigned int pressure = array_uint16_le(&data);
        unsigned int temp = array_uint16_le(&data+2);
        data += 4;

        sample.time = (i+1)*deepsix->sample_interval;
        if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

        sample.depth = pressure_to_depth(pressure);
        if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

        sample.temperature = temp / 10.0;
        if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);
    }

    return DC_STATUS_SUCCESS;
}