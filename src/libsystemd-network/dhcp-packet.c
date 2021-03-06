/***
  This file is part of systemd.

  Copyright (C) 2013 Intel Corporation. All rights reserved.
  Copyright (C) 2014 Tom Gundersen

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <net/ethernet.h>
#include <net/if_arp.h>
#include <sys/param.h>

#include "util.h"
#include "list.h"

#include "dhcp-protocol.h"
#include "dhcp-lease-internal.h"
#include "dhcp-internal.h"
#include "sd-dhcp-lease.h"
#include "sd-dhcp-client.h"

#define DHCP_CLIENT_MIN_OPTIONS_SIZE            312

int dhcp_message_init(DHCPMessage *message, uint8_t op, uint32_t xid,
                      uint8_t type, uint8_t **opt, size_t *optlen) {
        int err;

        assert(op == BOOTREQUEST || op == BOOTREPLY);

        message->op = op;
        message->htype = ARPHRD_ETHER;
        message->hlen = ETHER_ADDR_LEN;
        message->xid = htobe32(xid);
        message->magic = htobe32(DHCP_MAGIC_COOKIE);

        *opt = (uint8_t *)(message + 1);

        err = dhcp_option_append(opt, optlen, DHCP_OPTION_MESSAGE_TYPE, 1,
                                 &type);
        if (err < 0)
                return err;

        return 0;
}

uint16_t dhcp_packet_checksum(void *buf, size_t len) {
        uint64_t *buf_64 = buf;
        uint64_t *end_64 = (uint64_t*)buf + (len / sizeof(uint64_t));
        uint32_t *buf_32;
        uint16_t *buf_16;
        uint8_t *buf_8;
        uint64_t sum = 0;

        while (buf_64 < end_64) {
                sum += *buf_64;
                if (sum < *buf_64)
                        sum++;

                buf_64 ++;
        }

        buf_32 = (uint32_t*)buf_64;

        if (len & sizeof(uint32_t)) {
                sum += *buf_32;
                if (sum < *buf_32)
                        sum++;

                buf_32 ++;
        }

        buf_16 = (uint16_t*)buf_32;

        if (len & sizeof(uint16_t)) {
                sum += *buf_16;
                if (sum < *buf_16)
                        sum ++;

                buf_16 ++;
        }

        buf_8 = (uint8_t*)buf_16;

        if (len & sizeof(uint8_t)) {
                sum += *buf_8;
                if (sum < *buf_8)
                        sum++;
        }

        while (sum >> 16)
                sum = (sum & 0xffff) + (sum >> 16);

        return ~sum;
}

void dhcp_packet_append_ip_headers(DHCPPacket *packet, be32_t source_addr,
                                   uint16_t source_port, be32_t destination_addr,
                                   uint16_t destination_port, uint16_t len) {
        packet->ip.version = IPVERSION;
        packet->ip.ihl = DHCP_IP_SIZE / 4;
        packet->ip.tot_len = htobe16(len);

        packet->ip.protocol = IPPROTO_UDP;
        packet->ip.saddr = source_addr;
        packet->ip.daddr = destination_addr;

        packet->udp.source = htobe16(source_port);
        packet->udp.dest = htobe16(destination_port);

        packet->udp.len = htobe16(len - DHCP_IP_SIZE);

        packet->ip.check = packet->udp.len;
        packet->udp.check = dhcp_packet_checksum(&packet->ip.ttl, len - 8);

        packet->ip.ttl = IPDEFTTL;
        packet->ip.check = 0;
        packet->ip.check = dhcp_packet_checksum(&packet->ip, DHCP_IP_SIZE);
}

int dhcp_packet_verify_headers(DHCPPacket *packet, size_t len, bool checksum) {
        size_t hdrlen;

        assert(packet);

        /* IP */

        if (packet->ip.version != IPVERSION) {
                log_debug("ignoring packet: not IPv4");
                return -EINVAL;
        }

        if (packet->ip.ihl < 5) {
                log_debug("ignoring packet: IPv4 IHL (%u words) invalid",
                          packet->ip.ihl);
                return -EINVAL;
        }

        hdrlen = packet->ip.ihl * 4;
        if (hdrlen < 20) {
                log_debug("ignoring packet: IPv4 IHL (%zu bytes) "
                          "smaller than minimum (20 bytes)", hdrlen);
                return -EINVAL;
        }

        if (len < hdrlen) {
                log_debug("ignoring packet: packet (%zu bytes) "
                          "smaller than expected (%zu) by IP header", len,
                          hdrlen);
                return -EINVAL;
        }

        /* UDP */

        if (packet->ip.protocol != IPPROTO_UDP) {
                log_debug("ignoring packet: not UDP");
                return -EINVAL;
        }

        if (len < hdrlen + be16toh(packet->udp.len)) {
                log_debug("ignoring packet: packet (%zu bytes) "
                          "smaller than expected (%zu) by UDP header", len,
                          hdrlen + be16toh(packet->udp.len));
                return -EINVAL;
        }

        if (be16toh(packet->udp.dest) != DHCP_PORT_CLIENT) {
                log_debug("ignoring packet: to port %u, which "
                          "is not the DHCP client port (%u)",
                          be16toh(packet->udp.dest), DHCP_PORT_CLIENT);
                return -EINVAL;
        }

        /* checksums - computing these is relatively expensive, so only do it
           if all the other checks have passed
         */

        if (dhcp_packet_checksum(&packet->ip, hdrlen)) {
                log_debug("ignoring packet: invalid IP checksum");
                return -EINVAL;
        }

        if (checksum && packet->udp.check) {
                packet->ip.check = packet->udp.len;
                packet->ip.ttl = 0;

                if (dhcp_packet_checksum(&packet->ip.ttl,
                                  be16toh(packet->udp.len) + 12)) {
                        log_debug("ignoring packet: invalid UDP checksum");
                        return -EINVAL;
                }
        }

        return 0;
}
