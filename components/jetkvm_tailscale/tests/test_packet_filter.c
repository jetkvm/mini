#include "ml_packet_filter.h"
#include <assert.h>
#include <string.h>

static void update(cJSON **filter, const char *json)
{
    cJSON *map = cJSON_Parse(json);
    assert(map && ml_packet_filter_update(filter, map));
    cJSON_Delete(map);
}

void test_packet_filter(void)
{
    cJSON *filter = NULL;
    uint8_t packet[40] = {
        0x45, 0, 0, 40, 0, 0, 0, 0, 64, 6, 0, 0,
        100, 91, 148, 97, 100, 90, 1, 2,
        0xc0, 0x01, 0, 80, 0, 0, 0, 0, 0, 0, 0, 0, 0x50, 2,
    };
    uint32_t local = 0x645a0102;
    assert(!ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    update(&filter, "{\"PacketFilter\":[{\"SrcIPs\":[\"100.91.148.97/32\"],\"DstPorts\":[{\"IP\":\"100.90.1.2\",\"Ports\":{\"First\":80,\"Last\":80}}]}]}");
    assert(ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    packet[23] = 81;
    assert(!ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    packet[23] = 80;
    packet[15] = 98;
    assert(!ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    packet[15] = 97;
    packet[19] = 3;
    assert(!ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    packet[19] = 2;
    packet[6] = 0x20;
    assert(!ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    packet[6] = 0;
    assert(!ml_packet_filter_accept(filter, packet, 30, local));
    update(&filter, "{\"KeepAlive\":true}");
    assert(ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    update(&filter, "{\"PacketFilter\":[]}");
    assert(!ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    update(&filter, "{\"PacketFilters\":{\"grant\":[{\"SrcIPs\":[\"100.91.148.90-100.91.148.99\"],\"IPProto\":[6],\"DstPorts\":[{\"IP\":\"100.64.0.0/10\",\"Ports\":{\"First\":80,\"Last\":90}}]}]}}");
    assert(ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    packet[9] = 17;
    assert(!ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    packet[9] = 6;
    update(&filter, "{\"PacketFilters\":{\"grant\":null}}");
    assert(!ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    update(&filter, "{\"PacketFilter\":[{\"SrcIPs\":[\"*\"],\"DstPorts\":[{\"IP\":\"*\",\"Ports\":{\"First\":0,\"Last\":65535}}]}],\"PacketFilters\":{\"*\":null}}");
    assert(!ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    packet[33] = 0x10;
    assert(ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    packet[33] = 2;
    update(&filter, "{\"PacketFilter\":[{\"SrcIPs\":[\"100.91.148.0\"],\"SrcBits\":[24],\"DstPorts\":[{\"IP\":\"*\",\"Ports\":{\"First\":0,\"Last\":65535}}]}]}");
    assert(ml_packet_filter_accept(filter, packet, sizeof(packet), local));
    cJSON *invalid = cJSON_Parse("{\"PacketFilters\":{\"grant\":42}}");
    assert(!ml_packet_filter_update(&filter, invalid) && !filter);
    cJSON_Delete(invalid);
    assert(!ml_packet_filter_accept(filter, packet, sizeof(packet), local));

    ml_udp_flows_t flows = {0};
    packet[9] = 17;
    packet[3] = 28;
    assert(!ml_packet_filter_udp_reply(&flows, packet, 28, local, 100, false));
    /* Reverse the tuple to represent a locally initiated UDP packet. */
    uint8_t outgoing[28];
    memcpy(outgoing, packet, sizeof(outgoing));
    memcpy(outgoing + 12, packet + 16, 4);
    memcpy(outgoing + 16, packet + 12, 4);
    memcpy(outgoing + 20, packet + 22, 2);
    memcpy(outgoing + 22, packet + 20, 2);
    ml_packet_filter_udp_reply(&flows, outgoing, 28, local, 100, true);
    assert(ml_packet_filter_udp_reply(&flows, packet, 28, local, 101, false));
    packet[15] = 98;
    assert(!ml_packet_filter_udp_reply(&flows, packet, 28, local, 101, false));
    packet[15] = 97;
    packet[23] = 81;
    assert(!ml_packet_filter_udp_reply(&flows, packet, 28, local, 101, false));
    packet[23] = 80;
    assert(!ml_packet_filter_udp_reply(&flows, packet, 28, local, 60101, false));
    cJSON_Delete(filter);
}
