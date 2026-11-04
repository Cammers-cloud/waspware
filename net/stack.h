#ifndef NET_STACK_H
#define NET_STACK_H

#include <stddef.h>
#include <stdint.h>

#define NET_MAC_LENGTH 6U
#define NET_ARP_TABLE_SIZE 32U
#define NET_FLOW_TABLE_SIZE 64U

#define NET_ETHERTYPE_IPV4 0x0800U
#define NET_ETHERTYPE_ARP  0x0806U
#define NET_IPPROTO_ICMP   1U
#define NET_IPPROTO_TCP    6U
#define NET_IPPROTO_UDP    17U

#define NET_NTOHS(v) \
    ((uint16_t)((((uint16_t)(v) & 0x00FFU) << 8) | \
                 (((uint16_t)(v) & 0xFF00U) >> 8)))

#define NET_NTOHL(v) \
    ((uint32_t)((((uint32_t)(v) & 0x000000FFUL) << 24) | \
                (((uint32_t)(v) & 0x0000FF00UL) << 8)  | \
                (((uint32_t)(v) & 0x00FF0000UL) >> 8)  | \
                (((uint32_t)(v) & 0xFF000000UL) >> 24)))

#define NET_HTONS(v) NET_NTOHS(v)
#define NET_HTONL(v) NET_NTOHL(v)

typedef struct {
    uint8_t address[NET_MAC_LENGTH];
    uint32_t ipv4_address; /* Stored in host byte order. */
    uint8_t valid;
} net_arp_entry_t;

typedef struct {
    uint32_t local_address;
    uint32_t remote_address;
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t protocol;
    uint8_t valid;
} net_flow_entry_t;

/* Synchronously parse a single complete Ethernet frame. Buffer is not retained. */
void net_parse_packet(void *buffer, size_t length);

/* Set local interface addresses used for ARP answer decisions. */
int net_set_local_mac(const uint8_t mac[NET_MAC_LENGTH]);
int net_set_local_ipv4(uint32_t address_host_order);

/* Read-only table snapshots. */
size_t net_arp_snapshot(net_arp_entry_t *out, size_t capacity);
size_t net_flow_snapshot(net_flow_entry_t *out, size_t capacity);

#endif
