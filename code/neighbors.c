#define _POSIX_C_SOURCE 200809L

#include "neighbors.h"
#include "log.h"
#include <arpa/inet.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void print_text(const CdpText *text)
{
    for (size_t i = 0; i < text->length; i++) {
        unsigned int byte = text->bytes[i];
        if (byte >= 32 && byte <= 126 && byte != '\\')
            putchar((int)byte);
        else
            printf("\\x%02x", byte);
    }
}

static void print_neighbor(const char *event, const CdpInfo *info)
{
    if (!cdp_log_begin())
        return;

    printf("%s ", event);
    for (size_t i = 0; i < ETH_MAC_SIZE; i++)
        printf("%02x%s", (unsigned int)info->source_mac[i], i + 1 == ETH_MAC_SIZE ? " " : ":");
    print_text(&info->device);
    printf(" / ");
    print_text(&info->port);
    printf("  CDPv%u TTL=%us\n", (unsigned int)info->version, (unsigned int)info->ttl);
    if (strcmp(event, "ADD") == 0 || strcmp(event, "UPDATE") == 0) {
        char address[INET6_ADDRSTRLEN];
        if (info->has_ipv4 && inet_ntop(AF_INET, info->ipv4, address, sizeof(address)))
            printf("  IPv4: %s\n", address);
        if (info->has_ipv6 && inet_ntop(AF_INET6, info->ipv6, address, sizeof(address)))
            printf("  IPv6: %s\n", address);
        if (info->has_capabilities)
            printf("  Capabilities: 0x%08" PRIx32 "\n", info->capabilities);
        if (info->has_native_vlan)
            printf("  Native VLAN: %u\n", (unsigned int)info->native_vlan);
        if (info->has_voice_vlan)
            printf("  Voice VLAN: %u (reply data=0x%02x)\n",
                (unsigned int)info->voice_vlan, (unsigned int)info->voice_vlan_data);
        printf("  Software: ");
        print_text(&info->software);
        printf("\n  Platform: ");
        print_text(&info->platform);
        putchar('\n');
    }
    cdp_log_end();
}

static bool same_text(const CdpText *a, const CdpText *b)
{
    return a->length == b->length && memcmp(a->bytes, b->bytes, a->length) == 0;
}

static bool same_neighbor(const CdpInfo *a, const CdpInfo *b)
{
    return memcmp(a->source_mac, b->source_mac, ETH_MAC_SIZE) == 0 &&
           same_text(&a->device, &b->device) && same_text(&a->port, &b->port);
}

static void expire_neighbor(Neighbor *entry)
{
    entry->info.ttl = 0;
    print_neighbor("EXPIRE", &entry->info);
    entry->remaining_ms = 0;
    entry->used = false;
}

/* Saturating countdown: delayed ticks cannot underflow a neighbor's TTL. */
void neighbors_tick(NeighborTable *table, uint64_t elapsed_ms)
{
    for (size_t i = 0; i < NEIGHBOR_LIMIT; i++) {
        Neighbor *entry = &table->entries[i];
        if (!entry->used)
            continue;
        if (elapsed_ms >= entry->remaining_ms)
            expire_neighbor(entry);
        else
            entry->remaining_ms -= (uint32_t)elapsed_ms;
    }
}

void neighbors_update(NeighborTable *table, const CdpInfo *info)
{
    Neighbor *match = NULL;
    Neighbor *empty = NULL;
    for (size_t i = 0; i < NEIGHBOR_LIMIT; i++) {
        Neighbor *entry = &table->entries[i];
        if (entry->used && same_neighbor(&entry->info, info))
            match = entry;
        if (!entry->used && empty == NULL)
            empty = entry;
    }
    if (info->ttl == 0) {
        if (match != NULL)
            expire_neighbor(match);
        return;
    }
    Neighbor *entry = match != NULL ? match : empty;
    if (entry == NULL) {
        cdp_log(
                "ERROR: neighbor table full (%d entries); "
                "new neighbor not saved.\n",
                NEIGHBOR_LIMIT);
        return;
    }
    print_neighbor(match != NULL ? "UPDATE" : "ADD", info);
    entry->info = *info;
    entry->remaining_ms = (uint32_t)info->ttl * 1000u;
    entry->used = true;
}
