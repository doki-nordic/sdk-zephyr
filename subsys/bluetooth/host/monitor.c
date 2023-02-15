/** @file
 *  @brief Custom logging over UART
 */

/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/uart_pipe.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/uart.h>

#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/buf.h>

#include "monitor.h"

#if !defined(CONFIG_BT_MONITOR_LOG_LEVEL)
#define CONFIG_BT_MONITOR_LOG_LEVEL 2
#endif

#define LOG_LEVEL CONFIG_BT_MONITOR_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bt_monitor);

#define HCI_H4_NONE 0x00
#define HCI_H4_CMD  0x01
#define HCI_H4_ACL  0x02
#define HCI_H4_SCO  0x03
#define HCI_H4_EVT  0x04
#define HCI_H4_ISO  0x05

//#define CONFIG_HCI_PACKETS_ARE_H4 0
#define CONFIG_HCI_MONITOR_H4_WITH_PHDR 1
#define CONFIG_HCI_MONITOR_NO_HEADER 1
#define CONFIG_HCI_MONITOR_WAIT_FOR_RTT 1

/* This is the same default priority as for other console handlers,
 * except that we're not exporting it as a Kconfig variable until a
 * clear need arises.
 */
#define MONITOR_INIT_PRIORITY 60

#include <SEGGER_RTT.h>

#define RTT_BUFFER_NAME CONFIG_BT_DEBUG_MONITOR_RTT_BUFFER_NAME
#define RTT_BUF_SIZE CONFIG_BT_DEBUG_MONITOR_RTT_BUFFER_SIZE

/* https://www.tcpdump.org/linktypes.html */
#define DLT_BLUETOOTH_HCI_H4 187
#define DLT_BLUETOOTH_HCI_H4_WITH_PHDR 201

#define PHDR_FROM_CONTROLLER_TO_HOST 0x00
#define PHDR_FROM_HOST_TO_CONTROLLER 0x01

/*
https://wiki.wireshark.org/Bluetooth
https://wiki.wireshark.org/Development/LibpcapFileFormat
*/

static const struct {
	uint32_t magic_number;   /* magic number */
	uint16_t version_major;  /* major version number */
	uint16_t version_minor;  /* minor version number */
	int32_t  thiszone;       /* GMT to local correction */
	uint32_t sigfigs;        /* accuracy of timestamps */
	uint32_t snaplen;        /* max length of captured packets, in octets */
	uint32_t network;        /* data link type */
} file_header = {
	0xa1b2c3d4,
	2,
	4,
	0,
	0,
	65536,
	IS_ENABLED(CONFIG_HCI_MONITOR_H4_WITH_PHDR) ?
		DLT_BLUETOOTH_HCI_H4_WITH_PHDR : DLT_BLUETOOTH_HCI_H4,
};

struct RecordHeader {
	uint32_t ts_sec;         /* timestamp seconds */
	uint32_t ts_usec;        /* timestamp microseconds */
	uint32_t incl_len;       /* number of octets of packet saved in file */
	uint32_t orig_len;       /* actual length of packet */
#if defined(CONFIG_HCI_MONITOR_H4_WITH_PHDR)
	uint32_t phdr;           /* direction flag */
#endif
	uint8_t h4_packet_type[4];
};

uint64_t get_cycles_64() {
	if (IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER)) {
		return k_cycle_get_64();
	} else {
		static atomic_t high_atomic;
		uint32_t high = (uint32_t)atomic_get(&high_atomic);
		uint32_t t = k_cycle_get_32();

		if ((t >> 30) != (high & 3)) {
			if ((t >> 30) == ((high + 1) & 3)) {
				atomic_cas(&high_atomic, high, high + 1);
				high += 1;
			} else if ((t >> 30) == ((high + 2) & 3)) {
				atomic_cas(&high_atomic, high, high + 2);
				high += 2;
			} else {
				high--;
			}
		}

		return ((uint64_t)high << 30) | (uint64_t)t;
	}
}

void bt_monitor_send(uint16_t opcode, const void *data, size_t len)
{
	struct RecordHeader header;
	unsigned int cnt;
	size_t incl_len;
	size_t header_size;
	uint64_t t = get_cycles_64();
	uint64_t per_sec = sys_clock_hw_cycles_per_sec();
	uint32_t sec = (uint32_t)(t / per_sec);
	uint64_t cycles = t % per_sec;
	uint32_t us = (uint32_t)((cycles * 1000000) / per_sec);

	uint8_t hci_h4_type;
	uint32_t phdr = 0;

	switch (opcode) {
	case BT_MONITOR_COMMAND_PKT:
		hci_h4_type = HCI_H4_CMD;
		phdr = PHDR_FROM_CONTROLLER_TO_HOST;
		break;
	case BT_MONITOR_EVENT_PKT:
		hci_h4_type = HCI_H4_EVT;
		phdr = PHDR_FROM_HOST_TO_CONTROLLER;
		break;
	case BT_MONITOR_ACL_TX_PKT:
		hci_h4_type = HCI_H4_ACL;
		phdr = PHDR_FROM_CONTROLLER_TO_HOST;
		break;
	case BT_MONITOR_ACL_RX_PKT:
		hci_h4_type = HCI_H4_ACL;
		phdr = PHDR_FROM_HOST_TO_CONTROLLER;
		break;
	case BT_MONITOR_ISO_TX_PKT:
		hci_h4_type = HCI_H4_ISO;
		phdr = PHDR_FROM_CONTROLLER_TO_HOST;
		break;
	case BT_MONITOR_ISO_RX_PKT:
		hci_h4_type = HCI_H4_ISO;
		phdr = PHDR_FROM_HOST_TO_CONTROLLER;
		break;
	default:
		return;
	}

	incl_len = len;
	header_size = offsetof(struct RecordHeader, h4_packet_type);

	if (IS_ENABLED(CONFIG_HCI_MONITOR_H4_WITH_PHDR)) {
		incl_len += 4;
	}

	if (!IS_ENABLED(CONFIG_HCI_PACKETS_ARE_H4)) {
		incl_len++;
		header_size++;
	}

	header.ts_sec = sys_cpu_to_le32(sec);
	header.ts_usec = sys_cpu_to_le32(us);
	header.incl_len = sys_cpu_to_le32(incl_len);
	header.orig_len = sys_cpu_to_le32(incl_len);
#if defined(CONFIG_HCI_MONITOR_H4_WITH_PHDR)
	header.phdr = sys_cpu_to_be32(phdr);
#endif
	header.h4_packet_type[0] = hci_h4_type;

	SEGGER_RTT_LOCK();
	cnt = SEGGER_RTT_GetAvailWriteSpace(CONFIG_BT_DEBUG_MONITOR_RTT_BUFFER);
	if (cnt >= sizeof(header) + len) {
		SEGGER_RTT_WriteNoLock(CONFIG_BT_DEBUG_MONITOR_RTT_BUFFER,
							&header, header_size);
		SEGGER_RTT_WriteNoLock(CONFIG_BT_DEBUG_MONITOR_RTT_BUFFER,
							data, len);
	}
	SEGGER_RTT_UNLOCK();

	if (cnt < sizeof(header) + len) {
		static int total_lost = 0;
		const uint8_t *buf = (const uint8_t *)data;

		total_lost++;
		LOG_WRN("BT Monitor packet dropped, total=%d, size=%d, type=%d, from_host=%d, %02X %02X %02X %02X ...", total_lost, len, opcode, phdr, buf[0], buf[1], buf[2], buf[3]);
	}
}

void bt_monitor_new_index(uint8_t type, uint8_t bus, const bt_addr_t *addr,
			  const char *name)
{
	/* not supported by pcap format */
}

static const struct RecordHeader dummy_reset_record = {
	.ts_sec = 0,
	.ts_usec = 0,
#if defined(CONFIG_HCI_MONITOR_H4_WITH_PHDR)
	.incl_len = 8,
	.orig_len = 8,
	.phdr = 0,
#else
	.incl_len = 4,
	.orig_len = 4,
#endif
	.h4_packet_type = { 0x01, 0x03, 0x0c, 0x00 },
};

static int bt_monitor_init(const struct device *d)
{
	ARG_UNUSED(d);
	static uint8_t rtt_up_buf[RTT_BUF_SIZE];
	SEGGER_RTT_ConfigUpBuffer(CONFIG_BT_DEBUG_MONITOR_RTT_BUFFER,
				  RTT_BUFFER_NAME, rtt_up_buf, RTT_BUF_SIZE,
				  SEGGER_RTT_MODE_NO_BLOCK_SKIP);
	if (!IS_ENABLED(CONFIG_HCI_MONITOR_NO_HEADER)) {
		SEGGER_RTT_WriteNoLock(CONFIG_BT_DEBUG_MONITOR_RTT_BUFFER,
							&file_header, sizeof(file_header));
	} else if (IS_ENABLED(CONFIG_HCI_MONITOR_WAIT_FOR_RTT)) {
		SEGGER_RTT_WriteNoLock(CONFIG_BT_DEBUG_MONITOR_RTT_BUFFER,
							&dummy_reset_record,
							sizeof(dummy_reset_record));
	}

	if (IS_ENABLED(CONFIG_HCI_MONITOR_WAIT_FOR_RTT)) {
		while (SEGGER_RTT_GetBytesInBuffer(CONFIG_BT_DEBUG_MONITOR_RTT_BUFFER)) {
			/* Empty loop waiting until JLink consumes the first packet. */
		}
	}
	return 0;
}

SYS_INIT(bt_monitor_init, PRE_KERNEL_1, MONITOR_INIT_PRIORITY);

/*

Use case 1: Capture to a file (single run - resetting your device will end capturing).

1. Disable CONFIG_HCI_MONITOR_NO_HEADER
2. Flash and start your application.
3. Use JLinkRTTLogger to capture packets to a file, e.g.:
   JLinkRTTLogger -device NRF52840_XXAA -if swd -speed 4000 -rttchannel 1 -usb 683149425 hci.pcap
4. Press Ctrl-C to exit JLinkRTTLogger
5. Open your file in Wireshark

Use case 2: Capture to a file (multiple runs - resetting your device will not end capturing).

1. Prepare header file:
   echo d4c3b2a102000400000000000000000000000100c9000000 | xxd -r -p - header.pcap
2. Enable CONFIG_HCI_MONITOR_NO_HEADER
3. Flash and start your application.
4. Use JLinkRTTLogger to capture packets to a file, e.g.:
   JLinkRTTLogger -device NRF52840_XXAA -if swd -speed 4000 -rttchannel 1 -usb 683149425 hci.pcap-no-head
5. Press Ctrl-C to exit JLinkRTTLogger
7. Add header to your capture:
   cat header.pcap hci.pcap-no-head > hci.pcap
6. Open your file in Wireshark

Use case 3: Live capture

1. Prepare header file:
   echo d4c3b2a102000400000000000000000000000100c9000000 | xxd -r -p - header.pcap
2. Make two FIFOs:
   mkfifo /tmp/a
   mkfifo /tmp/b
3. Combine header and one FIFO and send them to the second FIFO:
   cat header.pcap /tmp/a > /tmp/b &
4. Start listening with Wireshark:
   wireshark -k -i /tmp/b &
5. Enable CONFIG_HCI_MONITOR_NO_HEADER
6. Flash and start your application.
7. Use JLinkRTTLogger to transfer packets to the FIFO, e.g.:
   JLinkRTTLogger -device NRF52840_XXAA -if swd -speed 4000 -rttchannel 1 -usb 683149425 /tmp/a
8. Press Ctrl-C to exit JLinkRTTLoger and stop live capture. Do not use "Stop capturing"
   button in Wireshark.
9. You can delete FIFOs if they are not needed anymore or reuse them if you want to repeat
   the process.

NOTE 1:
   Enable CONFIG_HCI_MONITOR_WAIT_FOR_RTT if you want to stop you application startup
   until you connect with RTT. Otherwise, you have to make sure that you are able to connect
   before RTT buffer overflows, e.g by increasing CONFIG_BT_DEBUG_MONITOR_RTT_BUFFER_SIZE.

NOTE 2:
   During device reset, the capture output may be corrupted. Probability of that is very small
   and increases if more data is captured just before the reset. It is because how the RTT works.

NOTE 3:
   If you enable CONFIG_HCI_MONITOR_WAIT_FOR_RTT and CONFIG_HCI_MONITOR_NO_HEADER, you will see
   one additional reset packet just after reset at time 0. It is not actual packet that was send
   by the host, but it is dummy packet needed to detect the RTT connection.

TIP 1:
   If you want to do multiple live captures, you can:
   * put "cat" command in a loop. The loop will end when you delete FIFOs:
     while [ -e /tmp/a ]; do cat header.pcap /tmp/a > /tmp/b ; done &
   * reuse the same wireshark window by starting again the capture. Wireshark will reuse
     the same FIFO. Make sure that you started capturing in Wireshark BEFORE starting
     JLinkRTTLogger again.

*/
