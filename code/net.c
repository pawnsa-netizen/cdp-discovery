#define _DEFAULT_SOURCE
#include "net.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <sys/time.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int net_open(const char *interface, const uint8_t multicast_mac[ETH_MAC_SIZE])
{
    unsigned int index = if_nametoindex(interface);
    if (index == 0) {
        perror("if_nametoindex");
        return -1;
    }
    if (index > INT_MAX) {
        fprintf(stderr, "Interface index is out of range: %s\n", interface);
        return -1;
    }

    /* Protocol 0 delays reception until bind selects the interface. */
    int fd = socket(AF_PACKET, SOCK_RAW, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }

    struct timeval timeout = {.tv_sec = NET_IO_TIMEOUT_SECONDS};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("setsockopt timeout");
        close(fd);
        return -1;
    }

    struct sockaddr_ll address = {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(ETH_P_ALL),
        .sll_ifindex = (int)index
    };

    /* The decoder filters the frames received on this interface. */
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        close(fd);
        return -1;
    }

    /* Join the CDP destination on the selected interface. */
    struct packet_mreq membership = {0};
    membership.mr_ifindex = (int)index;
    membership.mr_type = PACKET_MR_MULTICAST;
    membership.mr_alen = ETH_MAC_SIZE;
    memcpy(membership.mr_address, multicast_mac, ETH_MAC_SIZE);

    if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
                   &membership, sizeof(membership)) == -1) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    return fd;
}

ssize_t net_receive(int fd, uint8_t *buffer, size_t capacity)
{
    struct sockaddr_ll source = {0};
    socklen_t address_length = sizeof(source);
    ssize_t length = recvfrom(fd, buffer, capacity, MSG_TRUNC,
                              (struct sockaddr *)&source, &address_length);
    if (length == -1)
        return (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;

    if ((size_t)length > capacity ||
        source.sll_pkttype == PACKET_OUTGOING ||
        source.sll_hatype != ARPHRD_ETHER)
        return 0;

    return length;
}

bool net_get_mac(int fd, uint8_t mac[ETH_MAC_SIZE])
{
    struct sockaddr_ll address = {0};
    socklen_t size = sizeof(address);

    if (getsockname(fd, (struct sockaddr *)&address, &size) == -1) {
        perror("getsockname");
        return false;
    }

    if (address.sll_hatype != ARPHRD_ETHER || address.sll_halen != ETH_MAC_SIZE) {
        fprintf(stderr, "TX requires an Ethernet interface.\n");
        return false;
    }

    memcpy(mac, address.sll_addr, ETH_MAC_SIZE);
    return true;
}

/* Caller supplies a complete Ethernet frame of at least 14 bytes. */
bool net_send(int fd, const uint8_t *frame, size_t length)
{
    struct sockaddr_ll destination = {0};
    socklen_t size = sizeof(destination);

    if (getsockname(fd, (struct sockaddr *)&destination, &size) == -1) {
        perror("getsockname");
        return false;
    }

    destination.sll_protocol = htons(ETH_P_802_2);
    destination.sll_halen = ETH_MAC_SIZE;
    memcpy(destination.sll_addr, frame, ETH_MAC_SIZE);
    destination.sll_hatype = 0;
    destination.sll_pkttype = 0;

    ssize_t sent;

    do {
        sent = sendto(fd, frame, length, 0,
                      (struct sockaddr *)&destination, sizeof(destination));
    } while (sent == -1 && errno == EINTR);

    if (sent == -1) {
        perror("sendto");
        return false;
    }

    if ((size_t)sent != length) {
        fprintf(stderr, "Incomplete Ethernet transmission.\n");
        return false;
    }

    return true;
}


bool net_get_if_addr(const char *interface, CdpTx *local)
{
    struct ifaddrs *list;
    if (getifaddrs(&list) == -1) {
        perror("getifaddrs");
        return false;
    }

    bool found = false;
    bool ipv6_is_link_local = false;
    local->has_ipv4 = false;
    local->has_ipv6 = false;
    memset(local->ipv4, 0, sizeof(local->ipv4));
    memset(local->ipv6, 0, sizeof(local->ipv6));

    for (const struct ifaddrs *item = list; item != NULL; item = item->ifa_next) {
        if (strcmp(item->ifa_name, interface) != 0)
            continue;
        found = true;
        if (item->ifa_addr == NULL)
            continue;
        if (item->ifa_addr->sa_family == AF_INET && !local->has_ipv4) {
            const struct sockaddr_in *address =
                (const struct sockaddr_in *)item->ifa_addr;
            if (address->sin_addr.s_addr != htonl(INADDR_ANY)) {
                memcpy(local->ipv4, &address->sin_addr, sizeof(local->ipv4));
                local->has_ipv4 = true;
            }
        } else if (item->ifa_addr->sa_family == AF_INET6) {
            const struct sockaddr_in6 *address =
                (const struct sockaddr_in6 *)item->ifa_addr;
            if (IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr) ||
                IN6_IS_ADDR_MULTICAST(&address->sin6_addr))
                continue;
            bool link_local = IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr);
            if (!local->has_ipv6 || (ipv6_is_link_local && !link_local)) {
                memcpy(local->ipv6, &address->sin6_addr, sizeof(local->ipv6));
                local->has_ipv6 = true;
                ipv6_is_link_local = link_local;
            }
        }
    }
    freeifaddrs(list);
    if (!found)
        fprintf(stderr, "Interface disappeared: %s\n", interface);
    return found;
}
