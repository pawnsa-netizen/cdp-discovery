#ifndef NET_H
#define NET_H

#include "cdp.h"
#include <sys/types.h>

#define NET_IO_TIMEOUT_SECONDS 1

/* Open and bind a packet socket and join its multicast group.
 * The caller owns the returned descriptor. Returns -1 on failure. */
int net_open(const char *interface, const uint8_t multicast_mac[ETH_MAC_SIZE]);

/* Receive a frame. Zero means no usable frame and -1 means an I/O error. */
ssize_t net_receive(int fd, uint8_t *buffer, size_t capacity);

/* Read the Ethernet MAC from the bound socket. */
bool net_get_mac(int fd, uint8_t mac[ETH_MAC_SIZE]);

/* Refresh the outgoing record's IP addresses from the selected interface. */
bool net_get_if_addr(const char *interface, CdpTx *local);

/* Return true when the local socket accepts the entire Ethernet frame. */
bool net_send(int fd, const uint8_t *frame, size_t length);

#endif
