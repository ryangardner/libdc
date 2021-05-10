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

// "Read dives"?
#define CMD_GROUP_LOGS     0xC0 // get the logs
#define CMD_GROUP_LOGS_ACK 0xC1 // incremented by one when acked


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

#define EXCURSION_HDR_SIZE	36

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
#define MAX_DATA 160
typedef struct deepsix_command_sentence {
    unsigned char cmd;
    unsigned char sub_command;
    unsigned char byte_order;
    unsigned char data_len;
    unsigned char data[MAX_DATA];
} deepsix_command_sentence;


static char *
write_hex_byte(unsigned char data, char *p)
{
    static const char hex[16] = "0123456789ABCDEF";
    *p++ = hex[data >> 4];
    *p++ = hex[data & 0xf];
    return p;
}

//
// Send a cmd packet.
//
//
static dc_status_t
deepsix_send_cmd(deepsix_device_t *device, const deepsix_command_sentence cmd_sentence)
{
    char buffer[MAX_DATA], *p;
    unsigned char csum;
    int i;

    if (cmd_sentence.data_len > MAX_DATA)
        return DC_STATUS_INVALIDARGS;

    // Calculate packet csum
    csum = cmd_sentence.cmd + cmd_sentence.sub_command + cmd_sentence.byte_order;
    for (i = 0; i < cmd_sentence.data_len; i++)
        csum += cmd_sentence.data[i];
    csum = csum ^ 255;

    // Fill the data buffer
    p = buffer;
    *p++ = cmd_sentence.cmd;
    *p++ = cmd_sentence.sub_command;
    *p++ = cmd_sentence.byte_order;
    *p++ = cmd_sentence.data_len;
    for (i = 0; i < cmd_sentence.data_len; i++)
        *p++ = cmd_sentence.data[i];
    *p++ = csum;

    // .. and send it out
    return dc_iostream_write(device->iostream, buffer, p-buffer, NULL);
}

//
// Receive one 'line' of data
//
// The deepsix BLE protocol is binary and packetized.
//
static dc_status_t
deepsix_recv_bytes(deepsix_device_t *device, unsigned char *buf, size_t size)
{
    while (1) {
        unsigned char buffer[350];
        size_t transferred = 0;
        dc_status_t status;

        status = dc_iostream_read(device->iostream, buffer, sizeof(buffer), &transferred);
        if (status != DC_STATUS_SUCCESS) {
            ERROR(device->base.context, "Failed to receive DeepSix reply packet.");
            return status;
        }
        if (transferred > size) {
            ERROR(device->base.context, "Deep6 reply packet with too much data (got %zu, expected %zu)", transferred, size);
            return DC_STATUS_IO;
        }
        if (!transferred) {
            ERROR(device->base.context, "Empty DeepSix reply packet");
            return DC_STATUS_IO;
        }
        memcpy(buf, buffer, transferred);
        buf += transferred;
        size -= transferred;
        // when do _we_ terminate?
        if (buf[-1] == '\n')
            break;
    }
    buf[-1] = 0;
    return DC_STATUS_SUCCESS;
}

static int
hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int
read_hex_byte(char *p)
{
    // This is negative if either of the nibbles is invalid
    return (hex_nibble(p[0]) << 4) | hex_nibble(p[1]);
}


//
// Receive a reply packet
//
// The reply packet has the same format as the cmd packet we
// send, except the CMD_GROUP is incremented by one to show that it's an ack
static dc_status_t
deepsix_recv_data(deepsix_device_t *device, const unsigned char expected, const unsigned char expected_subcmd, unsigned char *buf, size_t size, size_t *received)
{
    int len, i;
    dc_status_t status;
    char buffer[8+2*MAX_DATA];
    int cmd, csum, ndata;

    status = deepsix_recv_bytes(device, buffer, sizeof(buffer));
    if (status != DC_STATUS_SUCCESS)
        return status;

    // deepsix_recv_line() always zero-terminates the result
    // if it returned success, and has removed the final newline.
    len = strlen(buffer);
    HEXDUMP(device->base.context, DC_LOGLEVEL_DEBUG, "rcv", buffer, len);

    // A valid reply should always be at least 7 characters: the
    // initial '$' and the three header HEX bytes.
    if (len < 8 || buffer[0] != '$') {
        ERROR(device->base.context, "Invalid DeepSix reply packet");
        return DC_STATUS_IO;
    }

    cmd = buffer+1;
    csum = read_hex_byte(buffer+3);
    ndata = read_hex_byte(buffer+5);
    if ((cmd | csum | ndata) < 0) {
        ERROR(device->base.context, "non-hex DeepSix reply packet header");
        return DC_STATUS_IO;
    }

    // Verify the data length: it's the size of the HEX data,
    // and should also match the line length we got (the 7
    // is for the header data we already decoded above).
    if ((ndata & 1) || ndata != len - 7) {
        ERROR(device->base.context, "DeepSix reply packet data length does not match (claimed %d, got %d)", ndata, len-7);
        return DC_STATUS_IO;
    }

    if (ndata >> 1 > size) {
        ERROR(device->base.context, "DeepSix reply packet too big for buffer (ndata=%d, size=%zu)", ndata, size);
        return DC_STATUS_IO;
    }

    csum += cmd + ndata;

    for (i = 7; i < len; i += 2) {
        int byte = read_hex_byte(buffer + i);
        if (byte < 0) {
            ERROR(device->base.context, "DeepSix reply packet data not valid hex");
            return DC_STATUS_IO;
        }
        *buf++ = byte;
        csum += byte;
    }

    if (csum & 255) {
        ERROR(device->base.context, "DeepSix reply packet csum not valid (%x)", csum);
        return DC_STATUS_IO;
    }

    *received = ndata >> 1;
    return DC_STATUS_SUCCESS;
}

// Common communication pattern: send a command, expect data back with the same
// command byte.
static dc_status_t
deepsix_send_recv(deepsix_device_t *device, struct deepsix_command_sentence cmd_sentence,
                  unsigned char *result, size_t result_size)
{
    dc_status_t status;
    size_t got;

    status = deepsix_send_cmd(device, cmd_sentence);
    if (status != DC_STATUS_SUCCESS)
        return status;
    status = deepsix_recv_data(device, cmd_sentence.cmd+1, cmd_sentence.sub_command, result, result_size, &got);
    if (status != DC_STATUS_SUCCESS)
        return status;
    if (got != result_size) {
        ERROR(device->base.context, "DeepSix result size didn't match expected (expected %zu, got %zu)",
              result_size, got);
        return DC_STATUS_IO;
    }
    return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_recv_bulk(deepsix_device_t *device, const unsigned char cmd, const unsigned char sub_cmd, unsigned char *buf, size_t len)
{
    while (len) {
        dc_status_t status;
        size_t got;

        status = deepsix_recv_data(device, cmd+1, sub_cmd, buf, len, &got);
        if (status != DC_STATUS_SUCCESS)
            return status;
        if (got > len) {
            ERROR(device->base.context, "DeepSix bulk receive overflow");
            return DC_STATUS_IO;
        }
        buf += got;
        len -= got;
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
deepsix_download_dive(deepsix_device_t *device, unsigned char nr, dc_dive_callback_t callback, void *userdata)
{
    unsigned char header_len;
    unsigned char profilebytes[2];
    unsigned int profile_len;
    dc_status_t status;
    char header[256];
    unsigned char *profile;
// todo - something here
    status = DC_STATUS_UNSUPPORTED;
//    status = deepsix_send_recv(device,  CMD_GETDIVE, 00, &nr, 1, &header_len, 1);
    if (status != DC_STATUS_SUCCESS)
        return status;
//    status = deepsix_recv_bulk(device, RSP_DIVESTAT, 00, header, header_len);
//    if (status != DC_STATUS_SUCCESS)
//        return status;
//    memset(header + header_len, 0, 256 - header_len);

    /* The header is the fingerprint. If we've already seen this header, we're done */
    if (memcmp(header, device->fingerprint, sizeof (device->fingerprint)) == 0)
        return DC_STATUS_DONE;

    // todo - add actual commands
//    status = deepsix_send_recv(device,  CMD_GETPROFILE, 0, &nr, 1, profilebytes, sizeof(profilebytes));
    status = DC_STATUS_UNSUPPORTED;
    if (status != DC_STATUS_SUCCESS)
        return status;
    profile_len = (profilebytes[0] << 8) | profilebytes[1];

    profile = malloc(256 + profile_len);
    if (!profile) {
        ERROR (device->base.context, "Insufficient buffer space available.");
        return DC_STATUS_NOMEMORY;
    }

    // We make the dive data be 256 bytes of header, followed by the profile data
    memcpy(profile, header, 256);

    // todo - update this
    status = deepsix_recv_bulk(device, RSP_DIVEPROF, 0, profile+256, profile_len);
    if (status != DC_STATUS_SUCCESS)
        return status;

    if (callback) {
        if (!callback(profile, profile_len+256, header, header_len, userdata))
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
    int i;

    u_int16_t dive_number = 1;
    deepsix_command_sentence sentence;
    sentence.cmd = CMD_GROUP_LOGS;
    sentence.sub_command = 0x02;
    sentence.byte_order = endian_bit;
    sentence.data_len = 2;
    // put the dive number into the data
    memcpy(sentence.data, &dive_number, 2);


    status = deepsix_send_recv(device, sentence, &nrdives, 1);
    if (status != DC_STATUS_SUCCESS)
        return status;

    if (!nrdives)
        return DC_STATUS_SUCCESS;

    progress.maximum = nrdives;
    progress.current = 0;
    device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

    for (i = 1; i <= nrdives; i++) {
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