#ifndef CDP_H
#define CDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ETH_MAC_SIZE 6u
#define ETH_LENGTH_SIZE 2u
#define ETH_HEADER_SIZE (2u * ETH_MAC_SIZE + ETH_LENGTH_SIZE)
#define ETH_LENGTH_OFFSET (2u * ETH_MAC_SIZE)
#define ETH_PAYLOAD_MAX 1500u
#define ETH_FRAME_MIN 60u
#define ETH_VLAN_TAG_SIZE 4u

#define CDP_SNAP_SIZE 8u
#define CDP_HEADER_SIZE 4u
#define CDP_TLV_HEADER_SIZE 4u

#define CDP_OFFSET (ETH_HEADER_SIZE + CDP_SNAP_SIZE)
#define CDP_TLV_OFFSET (CDP_OFFSET + CDP_HEADER_SIZE)
#define CDP_CHECKSUM_OFFSET 2u

#define CDP_IPV4_SIZE 4u
#define CDP_IPV6_SIZE 16u
#define CDP_IPV6_PROTOCOL_SIZE 8u
#define CDP_ADDRESS_COUNT_SIZE 4u
#define CDP_CAPABILITIES_SIZE 4u
#define CDP_NATIVE_VLAN_SIZE 2u
#define CDP_VOICE_VLAN_SIZE 3u

#define CDP_TEXT_SIZE ETH_PAYLOAD_MAX

#define CDP_TX_FRAME_SIZE (ETH_HEADER_SIZE + ETH_PAYLOAD_MAX)
#define CDP_TX_INTERVAL_MS 60000u
#define CDP_TX_TTL 180u
#define CDP_CAP_HOST 0x00000010u


/* TLV identifiers and decoded records. */

typedef enum {
    CDP_TLV_UNSPECIFIED = 0,
    CDP_TLV_DEVICE_ID = 0x0001,
    CDP_TLV_ADDRESSES = 0x0002,
    CDP_TLV_PORT_ID = 0x0003,
    CDP_TLV_CAPABILITIES = 0x0004,
    CDP_TLV_SOFTWARE = 0x0005,
    CDP_TLV_PLATFORM = 0x0006,
    CDP_TLV_NATIVE_VLAN = 0x000a,
    CDP_TLV_VOICE_VLAN_REPLY = 0x000e
} CdpTlvType;


typedef struct {
    CdpTlvType type;
    size_t length;
    uint8_t bytes[CDP_TEXT_SIZE]; /* In-place storage survives RX buffer reuse. */
} CdpText;

typedef struct {
    uint8_t source_mac[ETH_MAC_SIZE];
    uint8_t version;
    uint8_t ttl;
    CdpText device;
    CdpText port;
    CdpText software;
    CdpText platform;
    bool has_capabilities;
    uint32_t capabilities;
    bool has_ipv4;
    uint8_t ipv4[CDP_IPV4_SIZE];
    bool has_ipv6;
    uint8_t ipv6[CDP_IPV6_SIZE];
    bool has_native_vlan;
    uint16_t native_vlan;
    bool has_voice_vlan;
    uint8_t voice_vlan_data;
    uint16_t voice_vlan;
} CdpInfo;

typedef enum {
    CDP_OK,
    CDP_NOT_CDP,
    CDP_BAD_FRAME,
    CDP_BAD_VERSION,
    CDP_BAD_CHECKSUM,
    CDP_BAD_TLV,
    CDP_MISSING_ID
} CdpStatus;


/* Advertisement fields supplied by TX. */

typedef struct {
    uint8_t mac[ETH_MAC_SIZE];
    const char *device;
    const char *port;
    uint8_t ttl;

    bool has_capabilities;
    uint32_t capabilities;

    const char *software;
    const char *platform;

    bool has_ipv4;
    uint8_t ipv4[CDP_IPV4_SIZE];

    bool has_ipv6;
    uint8_t ipv6[CDP_IPV6_SIZE];

    bool has_native_vlan;
    uint16_t native_vlan;

    bool has_voice_vlan;
    uint8_t voice_vlan_data;
    uint16_t voice_vlan;
} CdpTx;

#define CDP_MULTICAST_MAC ((const uint8_t[ETH_MAC_SIZE]){ \
    0x01, 0x00, 0x0c, 0xcc, 0xcc, 0xcc })
#define CDP_SNAP ((const uint8_t[CDP_SNAP_SIZE]){ \
    0xaa, 0xaa, 0x03, 0x00, 0x00, 0x0c, 0x20, 0x00 })
#define CDP_IPV6_PROTOCOL ((const uint8_t[CDP_IPV6_PROTOCOL_SIZE]){ \
    0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x86, 0xdd })

#define CDP_VERSION_1 1u
#define CDP_VERSION_2 2u
#define CDP_IPV4_NLPID 0xccu
#define ETH_TPID_8021Q 0x8100u
#define ETH_TPID_8021AD 0x88a8u
#define ETH_MAX_VLAN_TAGS 2

/* Protocol identifiers inside Addresses TLV entries. */
typedef enum {
    CDP_ADDRESS_NLPID = 1,
    CDP_ADDRESS_SNAP = 2
} CdpAddressProtocol;

/* Write the decoded record only when the entire frame passes validation. */
CdpStatus cdp_decode(const uint8_t *frame, size_t length, CdpInfo *info);
/* Return a static human-readable description. */
const char *cdp_status_string(CdpStatus status);

/* Return the encoded frame length, or zero if the frame cannot be built. */
size_t cdp_tx_build(uint8_t *frame, size_t capacity, const CdpTx *local);

#endif
