/*
 * DeepSix Excursion downloading
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

// TODO: Implement the stuff in here - as of now, this is _literally_ a copy paste of the deepblu.c with the "deepblu" changed to "deepsix"

#include <string.h>
#include <stdlib.h>

#include "deepsix.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"

// "Write state"
#define CMD_SETTIME	0x20	// Send 6 byte date-time, get single-byte 00x00 ack
#define CMD_23		0x23	// Send 00/01 byte, get ack back? Some metric/imperial setting?

//
#define CMD_GROUP_LOGS     0xC0 // get the logs
#define CMD_GROUP_LOGS_ACK 0xC1 // incremented by one when acked

#define CMD_GROUP_INFO                   0xA0 // info command group
#define COMMAND_INFO_LAST_DIVE_LOG_INDEX 0x04 // get the index of the last dive
#define COMMAND_INFO_SERIAL_NUMBER       0x03 // get the serial number
#define SERIAL_NUMBER_LENGTH             12 // the length of the serial number

// sub commands for the log
#define LOG_INFO    0x02
#define LOG_PROFILE 0x03 // the sub command for the dive profile info


#define READ_WATCH = 0
#define WRITE_WATCH = 1

#define CMD_GETDIVENR	0x40	// Send empty byte, get single-byte number of dives back
#define CMD_GETDIVE	0x41	// Send dive number (1-nr) byte, get dive stat length byte back
#define RSP_DIVESTAT	0x42	//  .. followed by packets of dive stat for that dive of that length
#define CMD_GETPROFILE	0x43	// Send dive number (1-nr) byte, get dive profile length BE word back
#define RSP_DIVEPROF  0x44	//  .. followed by packets of dive profile of that length

// "Read state"
#define CMD_GETTIME	0x50	// Send empty byte, get six-byte bcd date-time back
#define CMD_51		0x51	// Send empty byte, get four bytes back (03 dc 00 e3)
#define CMD_52		0x52	// Send empty byte, get two bytes back (bf 8d)
#define CMD_53		0x53	// Send empty byte, get six bytes back (0e 81 00 03 00 00)
#define CMD_54		0x54	// Send empty byte, get byte back (00)
#define CMD_55		0x55	// Send empty byte, get byte back (00)
#define CMD_56		0x56	// Send empty byte, get byte back (00)
#define CMD_57		0x57	// Send empty byte, get byte back (00)
#define CMD_58		0x58	// Send empty byte, get byte back (52)
#define CMD_59		0x59	// Send empty byte, get six bytes back (00 00 07 00 00 00)
//                                     (00 00 00 00 00 00)
#define CMD_5a		0x5a	// Send empty byte, get six bytes back (23 1b 09 d8 37 c0)
#define CMD_5b		0x5b	// Send empty byte, get six bytes back (00 21 00 14 00 01)
//                                     (00 00 00 14 00 01)
#define CMD_5c		0x5c	// Send empty byte, get six bytes back (13 88 00 46 20 00)
//                                     (13 88 00 3c 15 00)
#define CMD_5d		0x5d	// Send empty byte, get six bytes back (19 00 23 0C 02 0E)
//                                     (14 14 14 0c 01 0e)
#define CMD_5f		0x5f	// Send empty byte, get six bytes back (00 00 07 00 00 00)

#define EXCURSION_HDR_SIZE	165

typedef struct deepsix_device_t {
    dc_device_t base;
    dc_iostream_t *iostream;
    unsigned char fingerprint[EXCURSION_HDR_SIZE];
} deepsix_device_t;

static const unsigned char endian_bit = 0x01;

static dc_status_t deepsix_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t deepsix_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t deepsix_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime);
static dc_status_t deepsix_device_close (dc_device_t *abstract);

static const dc_device_vtable_t deepsix_device_vtable = {
        sizeof(deepsix_device_t),
        DC_FAMILY_DEEP6,
        deepsix_device_set_fingerprint, /* set_fingerprint */
        NULL, /* read */
        NULL, /* write */
        NULL, /* dump */
        deepsix_device_foreach, /* foreach */
        deepsix_device_timesync, /* timesync */
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
//    csum = cmd_sentence.cmd + cmd_sentence.sub_command + cmd_sentence.byte_order;
//    if (cmd_sentence.data_len > 0) {
//        csum += cmd_sentence.data_len;
//        for (i = 0; i < cmd_sentence.data_len; i++)
//            csum += cmd_sentence.data[i];
//    }
//    csum = csum ^ 255;

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

    // response header
//    if (transferred > response->data_len) {
//        ERROR(device->base.context, "Deep6 reply packet with too much data (got %zu, expected %zu)", transferred, size);
//        return DC_STATUS_IO;
//    }

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
deepsix_recv_data(deepsix_device_t *device, const unsigned char expected, const unsigned char expected_subcmd, unsigned char *buf, unsigned char *received)
{
    int len, i;
    dc_status_t status;
    deepsix_command_sentence response;
    int cmd, csum, ndata;

    status = deepsix_recv_bytes(device, &response);
    if (status != DC_STATUS_SUCCESS)
        return status;

    // deepsix_recv_line() always zero-terminates the result
    // if it returned success, and has removed the final newline.
//    len = strlen(buffer);
//    HEXDUMP(device->base.context, DC_LOGLEVEL_DEBUG, "rcv", buffer, len);

    // A valid reply should always be at least 7 characters: the
    // initial '$' and the three header HEX bytes.
//    if (len < 8 || buffer[0] != '$') {
//        ERROR(device->base.context, "Invalid DeepSix reply packet");
//        return DC_STATUS_IO;
//    }

    cmd = response.cmd;
    csum = response.csum;
    ndata = response.data_len;
    if ((cmd | csum | ndata) < 0) {
        ERROR(device->base.context, "non-hex DeepSix reply packet header");
        return DC_STATUS_IO;
    }

//    // Verify the data length: it's the size of the HEX data,
//    // and should also match the line length we got (the 7
//    // is for the header data we already decoded above).
//    if ((ndata & 1) || ndata != len - 7) {
//        ERROR(device->base.context, "DeepSix reply packet data length does not match (claimed %d, got %d)", ndata, len-7);
//        return DC_STATUS_IO;
//    }
//
//    if (ndata >> 1 > size) {
//        ERROR(device->base.context, "DeepSix reply packet too big for buffer (ndata=%d, size=%zu)", ndata, size);
//        return DC_STATUS_IO;
//    }

//    csum += response.cmd + response.sub_command + response.byte_order + response.data_len;
//    for (int i = 0; i < cmd_sentence.data_len; i++)
//        *p++ = cmd_sentence.data[i];
//
//    for (i = 7; i < len; i += 2) {
//        int byte = read_hex_byte(buffer + i);
//        if (byte < 0) {
//            ERROR(device->base.context, "DeepSix reply packet data not valid hex");
//            return DC_STATUS_IO;
//        }
//        *buf++ = byte;
//        csum += byte;
//    }
    unsigned char calculated_csum = calculate_sentence_checksum(&response);

    if (calculated_csum != response.csum) {
        ERROR(device->base.context, "DeepSix reply packet csum not valid (%x)", csum);
        return DC_STATUS_IO;
    }
    memcpy(&received, &response.data_len, sizeof received);
    memcpy(buf, response.data, response.data_len);

    return DC_STATUS_SUCCESS;
}

// Common communication pattern: send a command, expect data back with the same
// command byte.
static dc_status_t
deepsix_send_recv(deepsix_device_t *device, const deepsix_command_sentence *cmd_sentence,
                  unsigned char *result, unsigned char *result_len)
{
    dc_status_t status;

    status = deepsix_send_cmd(device, cmd_sentence);
    if (status != DC_STATUS_SUCCESS)
        return status;
    status = deepsix_recv_data(device, cmd_sentence->cmd+1, cmd_sentence->sub_command, result, result_len);
    if (status != DC_STATUS_SUCCESS)
        return status;
    return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_recv_bulk(deepsix_device_t *device, u_int16_t dive_number, unsigned char *buf, unsigned int len)
{
    unsigned int offset = 0;
    deepsix_command_sentence get_profile;

    get_profile.cmd = CMD_GROUP_LOGS;
    get_profile.sub_command = LOG_INFO;
    get_profile.byte_order = endian_bit;

    while (len) {
        dc_status_t status;
        unsigned char got;

        array_uint16_le_set(get_profile.data, dive_number);
        array_uint32_le_set(&get_profile.data[2], offset);
        get_profile.data_len = 6;

        status = deepsix_send_recv(device, &get_profile, buf, &got);
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
deep6_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
    deepsix_device_t *device;

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

static unsigned char bcd(int val)
{
    if (val >= 0 && val < 100) {
        int high = val / 10;
        int low = val % 10;
        return (high << 4) | low;
    }
    return 0;
}

// todo: is this needed? or just for the comsiq?
static dc_status_t
deepsix_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime)
{
    deepsix_device_t *device = (deepsix_device_t *)abstract;
    unsigned char result[1], data[6];
    dc_status_t status;
    size_t len;

    data[0] = bcd(datetime->year - 2000);
    data[1] = bcd(datetime->month);
    data[2] = bcd(datetime->day);
    data[3] = bcd(datetime->hour);
    data[4] = bcd(datetime->minute);
    data[5] = bcd(datetime->second);

    // Maybe also check that we received one zero byte (ack?)
//    return deepsix_send_recv(device, CMD_SETTIME,
//                             data, sizeof(data),
//                             result, sizeof(result));
    return -1;
}

static dc_status_t
deepsix_device_close (dc_device_t *abstract)
{
    deepsix_device_t *device = (deepsix_device_t *) abstract;

    return DC_STATUS_SUCCESS;
}

static const char zero[MAX_DATA];

static dc_status_t
deepsix_download_dive(deepsix_device_t *device, u_int16_t nr, dc_dive_callback_t callback, void *userdata)
{
    dc_status_t status;
    unsigned char dive_info_bytes[EXCURSION_HDR_SIZE];
    unsigned char dive_info_len;
    unsigned char *profile;
    unsigned int profile_len;
    status = DC_STATUS_UNSUPPORTED;

    deepsix_command_sentence get_dive_info;

    get_dive_info.cmd = CMD_GROUP_LOGS;
    get_dive_info.sub_command = LOG_INFO;
    get_dive_info.byte_order = endian_bit;
    memcpy(get_dive_info.data, &nr, sizeof(nr));
    get_dive_info.data_len = sizeof(nr);




    status = deepsix_send_recv(device, &get_dive_info, &dive_info_bytes, &dive_info_len);

    if (status != DC_STATUS_SUCCESS)
        return status;

//    memset(dive_info_bytes + dive_info_len, 0, EXCURSION_HDR_SIZE - dive_info_len);
//    if (memcmp(dive_info_bytes, device->fingerprint, sizeof (device->fingerprint)) == 0)
//        return DC_STATUS_DONE;

    //status = DC_STATUS_UNSUPPORTED;
    if (status != DC_STATUS_SUCCESS)
        return status;

    unsigned int starting_offset = array_uint32_le(&dive_info_bytes[44]);
    unsigned int ending_offset = array_uint32_le(&dive_info_bytes[48]);

    profile_len = ending_offset - starting_offset;
    profile = malloc(EXCURSION_HDR_SIZE + profile_len);
    if (!profile) {
        ERROR (device->base.context, "Insufficient buffer space available.");
        return DC_STATUS_NOMEMORY;
    }
    // the dive profile is the info part (HDR_SIZE bytes) and then the actual profile
    memcpy(profile, dive_info_bytes, EXCURSION_HDR_SIZE);

    status = deepsix_recv_bulk(device, nr, profile+EXCURSION_HDR_SIZE, profile_len);

    //status = deepsix_send_recv(device, &get_dive_info, &dive_info_bytes, &result_size);
//    status = deepsix_recv_bulk(device, RSP_DIVESTAT, 00, header, header_len);
//    if (status != DC_STATUS_SUCCESS)
//        return status;
//    memset(header + header_len, 0, 256 - header_len);
//
//    /* The header is the fingerprint. If we've already seen this header, we're done */
//    if (memcmp(header, device->fingerprint, sizeof (device->fingerprint)) == 0)
//        return DC_STATUS_DONE;
//
//    // todo - add actual commands
////    status = deepsix_send_recv(device,  CMD_GETPROFILE, 0, &nr, 1, profilebytes, sizeof(profilebytes));
//    status = DC_STATUS_UNSUPPORTED;
//    if (status != DC_STATUS_SUCCESS)
//        return status;
//    profile_len = (profilebytes[0] << 8) | profilebytes[1];
//
//    profile = malloc(256 + profile_len);
//    if (!profile) {
//        ERROR (device->base.context, "Insufficient buffer space available.");
//        return DC_STATUS_NOMEMORY;
//    }
//
//    // We make the dive data be 256 bytes of header, followed by the profile data
//    memcpy(profile, header, 256);
//
//    // todo - update this
//    status = deepsix_recv_bulk(device, RSP_DIVEPROF, 0, profile+256, profile_len);
//    if (status != DC_STATUS_SUCCESS)
//        return status;
//
//    header_len = 0;
//    if (callback) {
//        if (!callback(profile, profile_len+256, header, header_len, userdata))
//            return DC_STATUS_DONE;
//    }
//    free(profile);
    return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
    dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
    deepsix_device_t *device = (deepsix_device_t *) abstract;
    unsigned char nrdives, val;
    dc_status_t status;
    u_int16_t i;

    u_int16_t dive_number = 0;
    deepsix_command_sentence sentence;
    sentence.cmd = CMD_GROUP_INFO;
    sentence.sub_command = COMMAND_INFO_LAST_DIVE_LOG_INDEX;
    sentence.byte_order = endian_bit;
//    sentence.data_len = 2;
//    // put the dive number into the data
//    memcpy(sentence.data, &dive_number, 2);
    array_uint16_le_set(sentence.data, dive_number);
    sentence.data_len = 2;
    char dive_number_buff[2];
    // get the last dive number
    status = deepsix_send_recv(device, &sentence, &dive_number_buff, 2);
    dive_number = array_uint16_le(dive_number_buff);

    if (status != DC_STATUS_SUCCESS)
        return status;

    if (!dive_number)
        return DC_STATUS_SUCCESS;

    progress.maximum = dive_number;
    progress.current = 0;
    device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

    for (i = 1; i <= dive_number; i++) {
        if (device_is_cancelled(abstract)) {
            dc_status_set_error(&status, DC_STATUS_CANCELLED);
            break;
        }

        status = deepsix_download_dive(device, i, callback, userdata);
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