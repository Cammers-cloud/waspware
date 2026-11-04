#include "stack.h"
#include "../drivers/net/e1000.h"

#define ETH_HEADER_LENGTH 14U
#define ARP_PACKET_LENGTH 28U
#define IPV4_MIN_HEADER_LENGTH 20U
#define UDP_HEADER_LENGTH 8U
#define TCP_MIN_HEADER_LENGTH 20U
#define ARP_HTYPE_ETHERNET 1U
#define ARP_PTYPE_IPV4 0x0800U
#define ARP_OPERATION_REQUEST 1U
#define ARP_OPERATION_REPLY 2U
#define IPV4_VERSION 4U
#define IPV4_FLAG_FRAGMENT_OFFSET 0x3FFFU

typedef struct __attribute__((packed)) {
    uint8_t destination[6];
    uint8_t source[6];
    uint16_t ethertype;
} ethernet_header_t;

typedef struct __attribute__((packed)) {
    uint16_t hardware_type;
    uint16_t protocol_type;
    uint8_t hardware_length;
    uint8_t protocol_length;
    uint16_t operation;
    uint8_t sender_hardware[6];
    uint8_t sender_protocol[4];
    uint8_t target_hardware[6];
    uint8_t target_protocol[4];
} arp_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t source;
    uint32_t destination;
} ipv4_header_t;

typedef struct __attribute__((packed)) {
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t length;
    uint16_t checksum;
} udp_header_t;

typedef struct __attribute__((packed)) {
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint8_t data_offset_reserved;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_pointer;
} tcp_header_t;

static uint8_t local_mac[NET_MAC_LENGTH];
static uint32_t local_ipv4;
static int local_mac_valid;
static int local_ipv4_valid;
static net_arp_entry_t arp_table[NET_ARP_TABLE_SIZE];
static net_flow_entry_t flow_table[NET_FLOW_TABLE_SIZE];
static uint32_t arp_replace;
static uint32_t flow_replace;

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static int bytes_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < len; ++i) {
        difference |= (uint8_t)(a[i] ^ b[i]);
    }
    return difference == 0;
}

static int mac_is_zero(const uint8_t *mac)
{
    uint8_t value = 0;
    for (size_t i = 0; i < NET_MAC_LENGTH; ++i) {
        value |= mac[i];
    }
    return value == 0;
}

static uint16_t checksum_add(uint32_t sum, const uint8_t *data, size_t length)
{
    while (length >= 2) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        length -= 2;
    }

    if (length != 0) {
        sum += (uint32_t)data[0] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }

    return (uint16_t)sum;
}

static int checksum_valid(const uint8_t *data, size_t length)
{
    return checksum_add(0, data, length) == 0xFFFFU;
}

static uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static int arp_update(const uint8_t mac[6], uint32_t ipv4_host)
{
    if (mac_is_zero(mac) || (mac[0] & 1U) != 0) {
        return -1;
    }

    size_t target = NET_ARP_TABLE_SIZE;
    for (size_t i = 0; i < NET_ARP_TABLE_SIZE; ++i) {
        if (arp_table[i].valid &&
            arp_table[i].ipv4_address == ipv4_host) {
            target = i;
            break;
        }
        if (!arp_table[i].valid && target == NET_ARP_TABLE_SIZE) {
            target = i;
        }
    }

    if (target == NET_ARP_TABLE_SIZE) {
        target = arp_replace++ % NET_ARP_TABLE_SIZE;
    }

    for (size_t i = 0; i < NET_MAC_LENGTH; ++i) {
        arp_table[target].address[i] = mac[i];
    }
    arp_table[target].ipv4_address = ipv4_host;
    arp_table[target].valid = 1;
    return 0;
}

static void arp_send_reply(const uint8_t *request_frame,
                           const arp_packet_t *request)
{
    uint8_t reply[ETH_HEADER_LENGTH + ARP_PACKET_LENGTH];

    for (size_t i = 0; i < 6; ++i) {
        reply[i] = request->sender_hardware[i];
        reply[6 + i] = local_mac[i];
    }
    write_be16(reply + 12, NET_ETHERTYPE_ARP);

    uint8_t *arp = reply + ETH_HEADER_LENGTH;
    write_be16(arp + 0, ARP_HTYPE_ETHERNET);
    write_be16(arp + 2, ARP_PTYPE_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    write_be16(arp + 6, ARP_OPERATION_REPLY);

    for (size_t i = 0; i < 6; ++i) {
        arp[8 + i] = local_mac[i];
        arp[18 + i] = request->sender_hardware[i];
    }

    write_be32(arp + 14, local_ipv4);
    for (size_t i = 0; i < 4; ++i) {
        arp[24 + i] = request->sender_protocol[i];
    }

    (void)request_frame;
    (void)ethernet_send(reply, sizeof(reply));
}

static void parse_arp(const uint8_t *frame, size_t length)
{
    if (length < ETH_HEADER_LENGTH + ARP_PACKET_LENGTH) {
        return;
    }

    const arp_packet_t *arp =
        (const arp_packet_t *)(const void *)(frame + ETH_HEADER_LENGTH);

    if (read_be16((const uint8_t *)&arp->hardware_type) != ARP_HTYPE_ETHERNET ||
        read_be16((const uint8_t *)&arp->protocol_type) != ARP_PTYPE_IPV4 ||
        arp->hardware_length != 6 ||
        arp->protocol_length != 4) {
        return;
    }

    uint16_t operation = read_be16((const uint8_t *)&arp->operation);
    uint32_t sender_ipv4 = read_be32(arp->sender_protocol);
    uint32_t target_ipv4 = read_be32(arp->target_protocol);

    if (operation != ARP_OPERATION_REQUEST &&
        operation != ARP_OPERATION_REPLY) {
        return;
    }

    (void)arp_update(arp->sender_hardware, sender_ipv4);

    if (operation == ARP_OPERATION_REQUEST &&
        local_mac_valid && local_ipv4_valid &&
        target_ipv4 == local_ipv4) {
        arp_send_reply(frame, arp);
    }
}

static void flow_record(uint8_t protocol, uint32_t source,
                        uint32_t destination, uint16_t source_port,
                        uint16_t destination_port)
{
    size_t target = NET_FLOW_TABLE_SIZE;

    for (size_t i = 0; i < NET_FLOW_TABLE_SIZE; ++i) {
        net_flow_entry_t *entry = &flow_table[i];
        if (entry->valid &&
            entry->protocol == protocol &&
            entry->local_address == source &&
            entry->remote_address == destination &&
            entry->local_port == source_port &&
            entry->remote_port == destination_port) {
            return;
        }
        if (!entry->valid && target == NET_FLOW_TABLE_SIZE) {
            target = i;
        }
    }

    if (target == NET_FLOW_TABLE_SIZE) {
        target = flow_replace++ % NET_FLOW_TABLE_SIZE;
    }

    flow_table[target].protocol = protocol;
    flow_table[target].local_address = source;
    flow_table[target].remote_address = destination;
    flow_table[target].local_port = source_port;
    flow_table[target].remote_port = destination_port;
    flow_table[target].valid = 1;
}

static void parse_transport(uint8_t protocol, const uint8_t *packet,
                            size_t packet_length, uint32_t source,
                            uint32_t destination)
{
    if (protocol == NET_IPPROTO_UDP) {
        if (packet_length < UDP_HEADER_LENGTH) {
            return;
        }

        uint16_t udp_length = read_be16(packet + 4);
        if (udp_length < UDP_HEADER_LENGTH || udp_length > packet_length) {
            return;
        }

        flow_record(protocol, source, destination,
                    read_be16(packet), read_be16(packet + 2));
        return;
    }

    if (protocol == NET_IPPROTO_TCP) {
        if (packet_length < TCP_MIN_HEADER_LENGTH) {
            return;
        }

        uint8_t header_length = (uint8_t)((packet[12] >> 4) * 4U);
        if (header_length < TCP_MIN_HEADER_LENGTH ||
            header_length > packet_length) {
            return;
        }

        flow_record(protocol, source, destination,
                    read_be16(packet), read_be16(packet + 2));
    }
}

static void parse_ipv4(const uint8_t *frame, size_t length)
{
    if (length < ETH_HEADER_LENGTH + IPV4_MIN_HEADER_LENGTH) {
        return;
    }

    const uint8_t *ip = frame + ETH_HEADER_LENGTH;
    uint8_t version = ip[0] >> 4;
    uint8_t ihl = (uint8_t)((ip[0] & 0x0FU) * 4U);

    if (version != IPV4_VERSION ||
        ihl < IPV4_MIN_HEADER_LENGTH ||
        ihl > length - ETH_HEADER_LENGTH) {
        return;
    }

    uint16_t total_length = read_be16(ip + 2);
    if (total_length < ihl ||
        total_length > length - ETH_HEADER_LENGTH ||
        !checksum_valid(ip, ihl)) {
        return;
    }

    uint16_t fragment = read_be16(ip + 6);
    if ((fragment & IPV4_FLAG_FRAGMENT_OFFSET) != 0) {
        return;
    }

    uint32_t source = read_be32(ip + 12);
    uint32_t destination = read_be32(ip + 16);
    parse_transport(ip[9], ip + ihl, total_length - ihl,
                    source, destination);
}

void net_parse_packet(void *buffer, size_t length)
{
    if (buffer == NULL || length < ETH_HEADER_LENGTH) {
        return;
    }

    const uint8_t *frame = (const uint8_t *)buffer;
    uint16_t ethertype = read_be16(frame + 12);

    if (ethertype == NET_ETHERTYPE_ARP) {
        parse_arp(frame, length);
    } else if (ethertype == NET_ETHERTYPE_IPV4) {
        parse_ipv4(frame, length);
    }
}

int net_set_local_mac(const uint8_t mac[NET_MAC_LENGTH])
{
    if (mac == NULL || mac_is_zero(mac) || (mac[0] & 1U) != 0) {
        return -1;
    }

    for (size_t i = 0; i < NET_MAC_LENGTH; ++i) {
        local_mac[i] = mac[i];
    }
    local_mac_valid = 1;
    return 0;
}

int net_set_local_ipv4(uint32_t address_host_order)
{
    if (address_host_order == 0 || address_host_order == 0xFFFFFFFFU) {
        return -1;
    }

    local_ipv4 = address_host_order;
    local_ipv4_valid = 1;
    return 0;
}

size_t net_arp_snapshot(net_arp_entry_t *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }

    size_t copied = 0;
    for (size_t i = 0; i < NET_ARP_TABLE_SIZE && copied < capacity; ++i) {
        if (!arp_table[i].valid) {
            continue;
        }
        out[copied++] = arp_table[i];
    }
    return copied;
}

size_t net_flow_snapshot(net_flow_entry_t *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }

    size_t copied = 0;
    for (size_t i = 0; i < NET_FLOW_TABLE_SIZE && copied < capacity; ++i) {
        if (!flow_table[i].valid) {
            continue;
        }
        out[copied++] = flow_table[i];
    }
    return copied;
}
