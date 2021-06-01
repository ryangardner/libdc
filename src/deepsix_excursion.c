/*
 * libdivecomputer
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "deepsix_excursion.h"
#include "context-private.h"
#include "device-private.h"
#include "platform.h"
#include "checksum.h"
#include "array.h"

#define MAXPACKET 255

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

#define FP_SIZE   6
#define FP_OFFSET 12

#define CMD_GROUP_LOGS     0xC0 // get the logs

#define CMD_GROUP_INFO                   0xA0 // info command group
#define COMMAND_INFO_LAST_DIVE_LOG_INDEX 0x04 // get the index of the last dive
#define COMMAND_INFO_SERIAL_NUMBER       0x03 // get the serial number

#define CMD_GROUP_SETTINGS  0xB0 // settings
#define CMD_SETTING_DATE    0x01 // date setting
#define CMD_SETTING_TIME    0x03 // time setting

// sub commands for the log
#define LOG_INFO    0x02
#define LOG_PROFILE 0x03 // the sub command for the dive profile info

typedef struct deepsix_device_t {
    dc_device_t base;
    dc_iostream_t *iostream;
    unsigned char fingerprint[FP_SIZE];
} deepsix_device_t;

static const unsigned char ENDIAN_BIT = 0x01;

static dc_status_t deepsix_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t deepsix_excursion_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t deepsix_excursion_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime);
static dc_status_t deepsix_device_close (dc_device_t *abstract);

static dc_status_t get_last_dive_index(deepsix_device_t *device, unsigned short *dive_number);
static dc_status_t get_serial_number(deepsix_device_t *device, char* serial_number);

static const dc_device_vtable_t deepsix_device_vtable = {
        sizeof(deepsix_device_t),
        DC_FAMILY_DEEPSIX,
        deepsix_device_set_fingerprint, /* set_fingerprint */
        NULL, /* read */
        NULL, /* write */
        NULL, /* dump */
        deepsix_excursion_device_foreach, /* foreach */
        deepsix_excursion_device_timesync, /* timesync */
        deepsix_device_close, /* close */
};




// Maximum data in a command sentence (in bytes)
//
// This is to make it simpler to build up the buffer
// to create and receive the command
// or reply
//
#define MAX_DATA 200
typedef struct deepsix_command_sentence {
    unsigned char cmd;
    unsigned char sub_command;
    unsigned char byte_order;
    unsigned char data_len;
    unsigned char data[MAX_DATA];
    unsigned char csum;
} deepsix_command_sentence;


static unsigned char calculate_sentence_checksum(const deepsix_command_sentence *sentence) {
    unsigned char checksum;
    checksum = (unsigned char)(sentence->cmd + sentence->sub_command + sentence->byte_order);
    if (sentence->data_len > 0) {
        checksum += sentence->data_len;
        for (int i = 0; i < sentence->data_len; i++)
            checksum += sentence->data[i];
    }
    return checksum ^ 255;
}

static dc_status_t
deepsix_excursion_send (deepsix_device_t *device, unsigned char cmd, unsigned char subcmd, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[4 + MAXPACKET + 1];

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (size > MAXPACKET)
		return DC_STATUS_INVALIDARGS;

	// Setup the data packet
	packet[0] = cmd;
	packet[1] = subcmd;
	packet[2] = 0x01;
	packet[3] = size;
	if (size) {
		memcpy(packet + 4, data, size);
	}
	packet[size + 4] = checksum_add_uint8 (packet, size + 4, 0) ^ 0xFF;

	// Send the data packet.
	status = dc_iostream_write (device->iostream, packet, 4 + size + 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return status;
}

static dc_status_t
deepsix_excursion_recv (deepsix_device_t *device, unsigned char cmd, unsigned char subcmd, unsigned char data[], unsigned int size, unsigned int *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[4 + MAXPACKET + 1];
	size_t transferred = 0;

	// Read the packet header.
	status = dc_iostream_read (device->iostream, packet, 4, &transferred);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet header.");
		return status;
	}

	if (transferred < 4) {
		ERROR (abstract->context, "Packet header too short ("DC_PRINTF_SIZE").", transferred);
		return DC_STATUS_PROTOCOL;
	}

	if (packet[0] != cmd /*|| packet[1] != subcmd*/ || packet[2] != 0x01) {
		ERROR (device->base.context, "Unexpected packet header.");
		return DC_STATUS_PROTOCOL;
	}

	unsigned int len = packet[3];
	if (len > MAXPACKET) {
		ERROR (abstract->context, "Packet header length too large (%u).", len);
		return DC_STATUS_PROTOCOL;
	}

	// Read the packet payload and checksum.
	status = dc_iostream_read (device->iostream, packet + 4, len + 1, &transferred);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet data.");
		return status;
	}

	if (transferred < len + 1) {
		ERROR (abstract->context, "Packet data too short ("DC_PRINTF_SIZE").", transferred);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the checksum.
	unsigned char csum = checksum_add_uint8 (packet, len + 4, 0) ^ 0xFF;
	if (packet[len + 4] != csum) {
		ERROR (abstract->context, "Unexpected packet checksum (%02x)", csum);
		return DC_STATUS_PROTOCOL;
	}

	if (len > size) {
		ERROR (abstract->context, "Unexpected packet length (%u).", len);
		return DC_STATUS_PROTOCOL;
	}

	memcpy(data, packet + 4, len);

	if (actual)
		*actual = len;

	return status;
}

static dc_status_t
deepsix_excursion_transfer (deepsix_device_t *device, unsigned char cmd, unsigned char subcmd, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	status = deepsix_excursion_send (device, cmd, subcmd, command, csize);
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = deepsix_excursion_recv (device, cmd + 1, subcmd, answer, asize, actual);
	if (status != DC_STATUS_SUCCESS)
		return status;

	return status;
}

static dc_status_t
deepsix_excursion_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	deepsix_device_t *device = (deepsix_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	unsigned char rsp_serial[12] = {0};
	status = deepsix_excursion_transfer (device, CMD_GROUP_INFO, COMMAND_INFO_SERIAL_NUMBER, NULL, 0, rsp_serial, sizeof(rsp_serial), NULL);
	if (status != DC_STATUS_SUCCESS)
		return status;

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = array_convert_str2num (rsp_serial + 3, sizeof(rsp_serial) - 3);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	const unsigned char cmd_index[2] = {0};
	unsigned char rsp_index[2] = {0};
	status = deepsix_excursion_transfer (device, CMD_GROUP_INFO, COMMAND_INFO_LAST_DIVE_LOG_INDEX, cmd_index, sizeof(cmd_index), rsp_index, sizeof(rsp_index), NULL);
	if (status != DC_STATUS_SUCCESS)
		return status;

	// Calculate the number of dives.
	unsigned int ndives = array_uint16_le (rsp_index);

	// Update and emit a progress event.
	progress.maximum = ndives * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	dc_buffer_t *buffer = dc_buffer_new(0);
	if (buffer == NULL) {
		return DC_STATUS_NOMEMORY;
	}

	for (unsigned int i = 0; i < ndives; ++i) {
		unsigned int number = ndives - i;

		const unsigned char cmd_header[] = {
			(number     ) & 0xFF,
			(number >> 8) & 0xFF};
		unsigned char rsp_header[EXCURSION_HDR_SIZE] = {0};
		status = deepsix_excursion_transfer (device, CMD_GROUP_LOGS, LOG_INFO, cmd_header, sizeof(cmd_header), rsp_header, sizeof(rsp_header), NULL);
		if (status != DC_STATUS_SUCCESS) {
			dc_buffer_free (buffer);
			return status;
		}

		if (memcmp(rsp_header + FP_OFFSET, device->fingerprint, sizeof(device->fingerprint)) == 0)
			break;

		unsigned int length = array_uint32_le (rsp_header + 8);

		// Update and emit a progress event.
		progress.current = i * NSTEPS + STEP(sizeof(rsp_header), sizeof(rsp_header) + length);
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		dc_buffer_clear(buffer);
		dc_buffer_reserve(buffer, sizeof(rsp_header) + length);

		if (!dc_buffer_append(buffer, rsp_header, sizeof(rsp_header))) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			dc_buffer_free(buffer);
			return DC_STATUS_NOMEMORY;
		}

		unsigned offset = 0;
		while (offset < length) {
			unsigned int len = 0;
			const unsigned char cmd_profile[] = {
				(number     ) & 0xFF,
				(number >> 8) & 0xFF,
				(offset      ) & 0xFF,
				(offset >>  8) & 0xFF,
				(offset >> 16) & 0xFF,
				(offset >> 24) & 0xFF};
			unsigned char rsp_profile[MAXPACKET] = {0};
			status = deepsix_excursion_transfer (device, CMD_GROUP_LOGS, LOG_PROFILE, cmd_profile, sizeof(cmd_profile), rsp_profile, sizeof(rsp_profile), &len);
			if (status != DC_STATUS_SUCCESS) {
				dc_buffer_free (buffer);
				return status;
			}

			unsigned int n = len;
			if (offset + n > length) {
				n = length - offset;
			}

			// Update and emit a progress event.
			progress.current = i * NSTEPS + STEP(sizeof(rsp_header) + offset + n, sizeof(rsp_header) + length);
			device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

			if (!dc_buffer_append(buffer, rsp_profile, n)) {
				ERROR (abstract->context, "Insufficient buffer space available.");
				dc_buffer_free(buffer);
				return DC_STATUS_NOMEMORY;
			}

			offset += n;
		}

		unsigned char *data = dc_buffer_get_data(buffer);
		unsigned int   size = dc_buffer_get_size(buffer);
		if (callback && !callback (data, size, data + FP_OFFSET, sizeof(device->fingerprint), userdata)) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
		}
	}

	dc_buffer_free(buffer);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_excursion_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	deepsix_device_t *device = (deepsix_device_t *) abstract;

	if (datetime == NULL || datetime->year < 2000) {
		ERROR (abstract->context, "Invalid date/time value specified.");
		return DC_STATUS_INVALIDARGS;
	}

	const unsigned char cmd_date[] = {
		datetime->year - 2000,
		datetime->month,
		datetime->day};

	const unsigned char cmd_time[] = {
		datetime->hour,
		datetime->minute,
		datetime->second};

	status = deepsix_excursion_send (device, CMD_GROUP_SETTINGS, CMD_SETTING_DATE, cmd_date, sizeof(cmd_date));
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	status = deepsix_excursion_send (device, CMD_GROUP_SETTINGS, CMD_SETTING_TIME, cmd_time, sizeof(cmd_time));
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	return status;
}


//
// Send a cmd packet.
//
//
static dc_status_t
deepsix_send_cmd(deepsix_device_t *device, const deepsix_command_sentence *cmd_sentence)
{
    char buffer[MAX_DATA], *p;
    unsigned char csum;
    int i;

    if (cmd_sentence->data_len > MAX_DATA)
        return DC_STATUS_INVALIDARGS;

    // Calculate packet csum
    csum = calculate_sentence_checksum(cmd_sentence);

    // Fill the data buffer
    p = buffer;
    *p++ = cmd_sentence->cmd;
    *p++ = cmd_sentence->sub_command;
    *p++ = cmd_sentence->byte_order;
    *p++ = cmd_sentence->data_len;
    for (i = 0; i < cmd_sentence->data_len; i++)
        *p++ = cmd_sentence->data[i];
    *p++ = csum;

    // .. and send it out
    return dc_iostream_write(device->iostream, buffer, p-buffer, NULL);
}


//
// Receive one 'packet' of data
//
// The deepsix BLE protocol is binary and starts with a command
//
static dc_status_t
deepsix_recv_bytes(deepsix_device_t *device, deepsix_command_sentence *response)
{
    unsigned char header[4];
    dc_status_t status;
    size_t header_transferred = 0;

    status = dc_iostream_read(device->iostream, header, sizeof(header), &header_transferred);
    if (status != DC_STATUS_SUCCESS) {
        ERROR(device->base.context, "Failed to receive DeepSix reply packet.");
        return status;
    }
    response->cmd = header[0];
    response->sub_command = header[1];
    response->byte_order  = header[2];
    response->data_len = header[3];
    if (response->data_len > MAX_DATA) {
        ERROR(device->base.context, "Received a response packet with a data length that is too long.");
        return status;
    }

    unsigned char* data_buffer = response->data;

    status = dc_iostream_read(device->iostream, data_buffer, response->data_len+1, NULL);

    if (status != DC_STATUS_SUCCESS) {
        ERROR(device->base.context, "Failed to receive DeepSix reply packet.");
        return status;
    }
    response->csum=response->data[response->data_len];

    return DC_STATUS_SUCCESS;
}


//
// Receive a reply packet
//
// The reply packet has the same format as the cmd packet we
// send, except the CMD_GROUP is incremented by one to show that it's an ack
static dc_status_t
deepsix_recv_data(deepsix_device_t *device, const unsigned char expected, const unsigned char expected_subcmd, unsigned char *buf, unsigned char *received, unsigned int max_bytes)
{
    int len, i;
    dc_status_t status;
    deepsix_command_sentence response;
    int cmd, csum, ndata;

    status = deepsix_recv_bytes(device, &response);
    if (status != DC_STATUS_SUCCESS)
        return status;

    cmd = response.cmd;
    csum = response.csum;
    ndata = response.data_len;
    if ((cmd | csum | ndata) < 0) {
        ERROR(device->base.context, "non-hex DeepSix reply packet header");
        return DC_STATUS_IO;
    }
    unsigned char calculated_csum = calculate_sentence_checksum(&response);

    if (calculated_csum != response.csum) {
        ERROR(device->base.context, "DeepSix reply packet csum not valid (%x)", csum);
        return DC_STATUS_IO;
    }
    // For the bulk receive, it puts garbage data after the actual profile data
    // so we only want the actual bytes we care about
    if (max_bytes < response.data_len) {
        response.data_len = max_bytes;
    }
    *received = response.data_len;
    memcpy(buf, response.data, response.data_len);

    return DC_STATUS_SUCCESS;
}

// Common communication pattern: send a command, expect data back with the same
// command byte.
static dc_status_t
deepsix_send_recv(deepsix_device_t *device, const deepsix_command_sentence *cmd_sentence,
                  unsigned char *result, unsigned char *result_len, unsigned int max_bytes)
{
    dc_status_t status;

    status = deepsix_send_cmd(device, cmd_sentence);
    if (status != DC_STATUS_SUCCESS)
        return status;
    status = deepsix_recv_data(device, cmd_sentence->cmd+1, cmd_sentence->sub_command, result, result_len, max_bytes);
    if (status != DC_STATUS_SUCCESS)
        return status;
    return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_recv_bulk(deepsix_device_t *device, unsigned short dive_number, unsigned char *buf, unsigned int len)
{
    unsigned int offset = 0;
    deepsix_command_sentence get_profile;

    get_profile.cmd = CMD_GROUP_LOGS;
    get_profile.sub_command = LOG_PROFILE;
    get_profile.byte_order = ENDIAN_BIT;

    while (len) {
        dc_status_t status;
        unsigned char got;

        array_uint16_le_set(get_profile.data, dive_number);
        array_uint32_le_set(&get_profile.data[2], offset);
        get_profile.data_len = 6;

        status = deepsix_send_recv(device, &get_profile, buf, &got, len);
        if (status != DC_STATUS_SUCCESS)
            return status;
        if (got > len) {
            ERROR(device->base.context, "DeepSix bulk receive overflow");
            return DC_STATUS_IO;
        }
        buf += got;
        len -= got;
        offset += got;
    }
    return DC_STATUS_SUCCESS;
}

dc_status_t
deepsix_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	deepsix_device_t *device = NULL;

    if (out == NULL)
        return DC_STATUS_INVALIDARGS;

    // Allocate memory.
    device = (deepsix_device_t *) dc_device_allocate (context, &deepsix_device_vtable);
    if (device == NULL) {
        ERROR (context, "Failed to allocate memory.");
        return DC_STATUS_NOMEMORY;
    }

    // Set the default values.
    device->iostream = iostream;
    memset(device->fingerprint, 0, sizeof(device->fingerprint));

	// Set the serial communication protocol (115200 8N1).
	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		//goto error_free;
	}

	// Set the timeout for receiving data (1000ms).
	status = dc_iostream_set_timeout (device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		//goto error_free;
	}

	// Make sure everything is in a sane state.
	dc_iostream_sleep (device->iostream, 300);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

    *out = (dc_device_t *) device;

    return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
    deepsix_device_t *device = (deepsix_device_t *)abstract;

    HEXDUMP(device->base.context, DC_LOGLEVEL_DEBUG, "set_fingerprint", data, size);

    if (size && size != sizeof (device->fingerprint))
        return DC_STATUS_INVALIDARGS;

    if (size)
        memcpy (device->fingerprint, data, sizeof (device->fingerprint));
    else
        memset (device->fingerprint, 0, sizeof (device->fingerprint));

    return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime)
{
    deepsix_device_t *device = (deepsix_device_t *)abstract;
    unsigned char result[1], date[3], time[3];
    dc_status_t status;
    size_t len;

    // time and date are set in two separate commands
    deepsix_command_sentence date_sync, time_sync;
    date_sync.cmd = CMD_GROUP_SETTINGS;
    date_sync.sub_command = CMD_SETTING_DATE;
    date_sync.byte_order = ENDIAN_BIT;
    date_sync.data[0] = (unsigned char) (datetime->year - 2000);
    date_sync.data[1] = (unsigned char) datetime->month;
    date_sync.data[2] = (unsigned char) datetime->day;
    date_sync.data_len = 3;

    time_sync.cmd = CMD_GROUP_SETTINGS;
    time_sync.sub_command = CMD_SETTING_TIME;
    time_sync.byte_order = ENDIAN_BIT;
    time_sync.data[0] = (unsigned char) datetime->hour;
    time_sync.data[1] = (unsigned char) datetime->minute;
    time_sync.data[2] = (unsigned char) datetime->second;
    time_sync.data_len = 3;

    status = deepsix_send_cmd(device, &date_sync);
    if (status == DC_STATUS_SUCCESS) {
        status = deepsix_send_cmd(device, &time_sync);
    }
    return status;
}

static dc_status_t
deepsix_device_close (dc_device_t *abstract)
{
    deepsix_device_t *device = (deepsix_device_t *) abstract;

    return DC_STATUS_SUCCESS;
}

static const char zero[MAX_DATA];

static dc_status_t
deepsix_download_dive(deepsix_device_t *device, unsigned short nr, dc_dive_callback_t callback, const char* serial_number, void *userdata)
{
    unsigned char header_len;
    char header[256];

    dc_status_t status;
    unsigned char dive_info_bytes[EXCURSION_HDR_SIZE];
    unsigned char dive_info_len = 0;
    unsigned char *profile;
    unsigned int profile_len;
    status = DC_STATUS_UNSUPPORTED;

    deepsix_command_sentence get_dive_info;

    get_dive_info.cmd = CMD_GROUP_LOGS;
    get_dive_info.sub_command = LOG_INFO;
    get_dive_info.byte_order = ENDIAN_BIT;
    memcpy(get_dive_info.data, &nr, sizeof(nr));
    get_dive_info.data_len = sizeof(nr);

    status = deepsix_send_recv(device, &get_dive_info, &dive_info_bytes, &dive_info_len, MAX_DATA);

    if (status != DC_STATUS_SUCCESS)
        return status;

    memset(dive_info_bytes + dive_info_len, 0, EXCURSION_HDR_SIZE - dive_info_len);
    if (memcmp(dive_info_bytes, device->fingerprint, sizeof (device->fingerprint)) == 0)
        return DC_STATUS_DONE;

    //status = DC_STATUS_UNSUPPORTED;
    if (status != DC_STATUS_SUCCESS)
        return status;

    unsigned int starting_offset = array_uint32_le(&dive_info_bytes[40]);
    unsigned int ending_offset = array_uint32_le(&dive_info_bytes[44]);

    profile_len = ending_offset - starting_offset;
    profile = malloc(EXCURSION_HDR_SIZE + EXCURSION_SERIAL_NUMBER_LEN + profile_len);
    if (!profile) {
        ERROR (device->base.context, "Insufficient buffer space available.");
        return DC_STATUS_NOMEMORY;
    }
    // the dive profile is the info part (HDR_SIZE bytes) and then the actual profile
    memcpy(profile, dive_info_bytes, EXCURSION_HDR_SIZE);
    memcpy(profile+EXCURSION_HDR_SIZE, serial_number, EXCURSION_SERIAL_NUMBER_LEN);

    status = deepsix_recv_bulk(device, nr, profile+EXCURSION_HDR_SIZE+EXCURSION_SERIAL_NUMBER_LEN, profile_len);
    memset(header + header_len, 0, 256 - header_len);
    memcpy(header, profile, EXCURSION_HDR_SIZE);

    /* The header is the fingerprint. If we've already seen this header, we're done */
    if (memcmp(header, device->fingerprint, sizeof (device->fingerprint)) == 0)
        return DC_STATUS_DONE;

    char divehdr[25];
    sprintf(divehdr, "Dive #%2d header: ", nr);
    HEXDUMP(device->base.context, DC_LOGLEVEL_INFO, divehdr, (const unsigned char *) dive_info_bytes, dive_info_len);
    char diveprofile[30];
    sprintf(diveprofile, "Dive #%2d profile: ", nr);
    HEXDUMP(device->base.context, DC_LOGLEVEL_INFO, diveprofile, (const unsigned char *) profile+EXCURSION_HDR_SIZE, profile_len);
    char divecombined[30];
    sprintf(divecombined, "Dive #%2d combined: ", nr);
    HEXDUMP(device->base.context, DC_LOGLEVEL_INFO, divecombined, (const unsigned char*)profile, dive_info_len+EXCURSION_SERIAL_NUMBER_LEN+profile_len);

    header_len = 0;
    if (callback) {
        // typedef int (*dc_dive_callback_t) (const unsigned char *data, unsigned int size, const unsigned char *fingerprint, unsigned int fsize, void *userdata);
        if (!callback(profile, profile_len+EXCURSION_HDR_SIZE+EXCURSION_SERIAL_NUMBER_LEN, header, header_len, userdata))
            return DC_STATUS_DONE;
    }
    free(profile);
    return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
    dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
    deepsix_device_t *device = (deepsix_device_t *) abstract;
    unsigned char nrdives, val;
    dc_status_t status;
    unsigned short i;

    unsigned short dive_number;
    status = get_last_dive_index(device, &dive_number);
    if (status != DC_STATUS_SUCCESS)
        return status;
    char serial_number[12];
    status = get_serial_number(device, serial_number);

    if (status != DC_STATUS_SUCCESS)
        return status;

    if (!dive_number)
        return DC_STATUS_SUCCESS;

    progress.maximum = dive_number;
    progress.current = 0;
    device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

    for (i = dive_number; i > 0; i--) {
        if (device_is_cancelled(abstract)) {
            dc_status_set_error(&status, DC_STATUS_CANCELLED);
            break;
        }

        status = deepsix_download_dive(device, i, callback, serial_number, userdata);
        switch (status) {
            case DC_STATUS_DONE:
                i = nrdives;
                break;
            case DC_STATUS_SUCCESS:
                break;
            default:
                return status;
        }
        progress.current = i;
        device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);
    }

    return DC_STATUS_SUCCESS;
}

static dc_status_t
get_last_dive_index(deepsix_device_t *device, unsigned short *dive_number) {
    dc_status_t status;
    deepsix_command_sentence get_last_dive_cmd;
    get_last_dive_cmd.cmd = CMD_GROUP_INFO;
    get_last_dive_cmd.sub_command = COMMAND_INFO_LAST_DIVE_LOG_INDEX;
    get_last_dive_cmd.byte_order = ENDIAN_BIT;

    array_uint16_le_set(get_last_dive_cmd.data, (*dive_number));
    get_last_dive_cmd.data_len = 2;
    char dive_number_buff[2];
    // get the last dive number
    unsigned char data_len;
    status = deepsix_send_recv(device, &get_last_dive_cmd, &dive_number_buff, &data_len, MAX_DATA);
    (*dive_number) = array_uint16_le(dive_number_buff);
}

static dc_status_t
get_serial_number(deepsix_device_t *device, char* serial_number) {
    dc_status_t status;
    deepsix_command_sentence get_serial_number;
    get_serial_number.cmd = CMD_GROUP_INFO;
    get_serial_number.sub_command = COMMAND_INFO_SERIAL_NUMBER;
    get_serial_number.byte_order = ENDIAN_BIT;
    get_serial_number.data_len = 0;
    // get the last dive number
    unsigned char data_len;
    status = deepsix_send_recv(device, &get_serial_number, serial_number, &data_len, 12);
    return status;
}
