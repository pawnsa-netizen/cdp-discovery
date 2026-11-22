#define _POSIX_C_SOURCE 200809L
#include "cdp.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

static uint16_t read16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* The Ethernet payload limit keeps the running sum within 32 bits. */
static uint16_t checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    size_t pos = 0;

    for (; pos + 1 < length; pos += 2)
        sum += read16(data + pos);

    /* Apply CDP’s rule for an unpaired final byte. */
    if (pos < length) {
        uint8_t last = data[pos];
        sum += last < 0x80u ? last : 0xff00u + last - 1u;
    }

    while (sum >> 16)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static bool decode_addresses(const uint8_t *p, size_t length, CdpInfo *info, const char **reason)
{
#define ADDRESS_ERROR(message) do { *reason = (message); return false; } while (0)
    if (length < CDP_ADDRESS_COUNT_SIZE)
        ADDRESS_ERROR("Addresses: missing address count");
    uint32_t count = read32(p);
    size_t pos = CDP_ADDRESS_COUNT_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        if (length - pos < 2)
            ADDRESS_ERROR("Addresses: truncated protocol header");
        uint8_t type = p[pos++];
        size_t protocol_length = p[pos++];
        if (protocol_length == 0 || protocol_length > length - pos)
            ADDRESS_ERROR("Addresses: zero or out-of-bounds protocol length");
        const uint8_t *protocol = p + pos;
        pos += protocol_length;
        if (length - pos < 2)
            ADDRESS_ERROR("Addresses: missing address length");
        size_t address_length = read16(p + pos);
        pos += 2;
        if (address_length == 0 || address_length > length - pos)
            ADDRESS_ERROR("Addresses: zero or out-of-bounds address length");
        if (type == CDP_ADDRESS_NLPID && protocol_length == 1 && protocol[0] == CDP_IPV4_NLPID) {
            if (address_length != CDP_IPV4_SIZE)
                ADDRESS_ERROR("Addresses: IPv4 address length is not 4");
            if (!info->has_ipv4) {
                memcpy(info->ipv4, p + pos, CDP_IPV4_SIZE);
                info->has_ipv4 = true;
            }
        } else if (type == CDP_ADDRESS_SNAP && protocol_length == sizeof(CDP_IPV6_PROTOCOL) &&
                   memcmp(protocol, CDP_IPV6_PROTOCOL, sizeof(CDP_IPV6_PROTOCOL)) == 0) {
            if (address_length != CDP_IPV6_SIZE)
                ADDRESS_ERROR("Addresses: IPv6 address length is not 16");
            if (!info->has_ipv6) {
                memcpy(info->ipv6, p + pos, CDP_IPV6_SIZE);
                info->has_ipv6 = true;
            }
        }
        pos += address_length;
    }
    /* Some switches append zero-filled bytes after the declared entries. */
    while (pos < length) {
        if (p[pos++] != 0)
            ADDRESS_ERROR("Addresses: nonzero trailing bytes after declared entries");
    }
#undef ADDRESS_ERROR
    return true;
}

/* The caller has validated the CDP payload bounds. */
static CdpStatus bad_tlv(const uint8_t *frame, const uint8_t *data,
                         size_t data_length, size_t pos, const char *reason)
{
    if (!cdp_log_begin())
        return CDP_BAD_TLV;
    fprintf(stdout, "Dropped CDP: %s src=%02x:%02x:%02x:%02x:%02x:%02x"
            " cdp_len=%zu offset=%zu remaining=%zu",
            reason, frame[6], frame[7], frame[8], frame[9], frame[10], frame[11],
            data_length, pos, data_length - pos);
    if (data_length - pos >= CDP_TLV_HEADER_SIZE)
        fprintf(stdout, " type=0x%04x tlv_len=%u", read16(data + pos),
                (unsigned)read16(data + pos + 2));
    fputc('\n', stdout);
    cdp_log_end();
    return CDP_BAD_TLV;
}

CdpStatus cdp_decode(const uint8_t *frame, size_t length, CdpInfo *info)
{
    if (frame == NULL || info == NULL)
        return CDP_BAD_FRAME;
    if (length < ETH_HEADER_SIZE || memcmp(frame, CDP_MULTICAST_MAC, ETH_MAC_SIZE) != 0)
        return CDP_NOT_CDP;
    size_t ethernet_length = ETH_HEADER_SIZE;
    uint16_t field = read16(frame + ETH_LENGTH_OFFSET);
    for (int tags = 0; field == ETH_TPID_8021Q || field == ETH_TPID_8021AD; tags++) {
        if (tags == ETH_MAX_VLAN_TAGS || length - ethernet_length < ETH_VLAN_TAG_SIZE)
            return CDP_BAD_FRAME;
        field = read16(frame + ethernet_length + 2);
        ethernet_length += ETH_VLAN_TAG_SIZE;
    }
    if (field > ETH_PAYLOAD_MAX)
        return CDP_NOT_CDP;
    if (field < CDP_SNAP_SIZE + CDP_HEADER_SIZE || field > length - ethernet_length)
        return CDP_BAD_FRAME;
    if (memcmp(frame + ethernet_length, CDP_SNAP, CDP_SNAP_SIZE) != 0)
        return CDP_NOT_CDP;
    const uint8_t *data = frame + ethernet_length + CDP_SNAP_SIZE;
    size_t data_length = field - CDP_SNAP_SIZE;
    if (data[0] != CDP_VERSION_1 && data[0] != CDP_VERSION_2)
        return CDP_BAD_VERSION;
    if (checksum(data, data_length) != 0)
        return CDP_BAD_CHECKSUM;

    CdpInfo parsed = {0};
    memcpy(parsed.source_mac, frame + ETH_MAC_SIZE, ETH_MAC_SIZE);
    parsed.version = data[0];
    parsed.ttl = data[1];
    size_t pos = CDP_HEADER_SIZE;
    while (pos < data_length) {
        if (data_length - pos < CDP_TLV_HEADER_SIZE)
            return bad_tlv(frame, data, data_length, pos, "incomplete TLV header");
        uint16_t type = read16(data + pos);
        size_t total = read16(data + pos + 2);
        if (total < CDP_TLV_HEADER_SIZE || total > data_length - pos)
            return bad_tlv(frame, data, data_length, pos, "TLV length outside payload bounds");
        const uint8_t *value = data + pos + CDP_TLV_HEADER_SIZE;
        size_t value_length = total - CDP_TLV_HEADER_SIZE;
        CdpText *text = NULL;
        switch (type) {
        case CDP_TLV_DEVICE_ID: text = &parsed.device; break;
        case CDP_TLV_ADDRESSES: {
            const char *reason = NULL;
            if (!decode_addresses(value, value_length, &parsed, &reason))
                return bad_tlv(frame, data, data_length, pos, reason);
            break;
        }
        case CDP_TLV_PORT_ID: text = &parsed.port; break;
        case CDP_TLV_CAPABILITIES:
            if (value_length != CDP_CAPABILITIES_SIZE)
                return bad_tlv(frame, data, data_length, pos, "Capabilities value length must be 4");
            parsed.capabilities = read32(value);
            parsed.has_capabilities = true;
            break;
        case CDP_TLV_SOFTWARE: text = &parsed.software; break;
        case CDP_TLV_PLATFORM: text = &parsed.platform; break;
        case CDP_TLV_NATIVE_VLAN:
            if (value_length != CDP_NATIVE_VLAN_SIZE)
                return bad_tlv(frame, data, data_length, pos, "Native VLAN value length must be 2");

            parsed.native_vlan = read16(value);
            parsed.has_native_vlan = true;
            break;

        case CDP_TLV_VOICE_VLAN_REPLY:
            if (value_length != CDP_VOICE_VLAN_SIZE)
                return bad_tlv(frame, data, data_length, pos, "Voice VLAN Reply value length must be 3");

            parsed.voice_vlan_data = value[0];
            parsed.voice_vlan = read16(value + 1);
            parsed.has_voice_vlan = true;
            break;
        default:
            break; /* Skip unknown TLVs after validating their lengths. */
        }
        if (text != NULL) {
            if (value_length > sizeof(text->bytes))
                return bad_tlv(frame, data, data_length, pos, "text value exceeds local storage");
            text->type = (CdpTlvType)type;
            text->length = value_length;
            memcpy(text->bytes, value, value_length);
        }
        pos += total;
    }
    if (parsed.device.length == 0 || parsed.port.length == 0)
        return CDP_MISSING_ID;
    *info = parsed;
    return CDP_OK;
}

const char *cdp_status_string(CdpStatus status)
{
    switch (status) {
    case CDP_OK: return "accepted";
    case CDP_NOT_CDP: return "not CDP";
    case CDP_BAD_FRAME: return "invalid or truncated Ethernet frame";
    case CDP_BAD_VERSION: return "unsupported CDP version";
    case CDP_BAD_CHECKSUM: return "bad CDP checksum";
    case CDP_BAD_TLV: return "invalid supported TLV";
    case CDP_MISSING_ID: return "missing Device ID or Port ID";
    default: return "unknown status";
    }
}

static void write16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static bool add_tlv(uint8_t *frame, size_t limit, size_t *used,
                    CdpTlvType type, const void *value, size_t length)
{
    if (*used > limit || limit - *used < CDP_TLV_HEADER_SIZE ||
        length > limit - *used - CDP_TLV_HEADER_SIZE)
        return false;

    write16(frame + *used, (uint16_t)type);
    write16(frame + *used + 2,
            (uint16_t)(length + CDP_TLV_HEADER_SIZE));

    memcpy(frame + *used + CDP_TLV_HEADER_SIZE, value, length);
    *used += CDP_TLV_HEADER_SIZE + length;
    return true;
}

static size_t encode_addresses(uint8_t *value, const CdpTx *local)
{
    size_t used = CDP_ADDRESS_COUNT_SIZE;
    uint32_t count = 0;

    if (local->has_ipv4) {
        value[used++] = CDP_ADDRESS_NLPID;
        value[used++] = 1;
        value[used++] = CDP_IPV4_NLPID;

        write16(value + used, CDP_IPV4_SIZE);
        used += 2;

        memcpy(value + used, local->ipv4, CDP_IPV4_SIZE);
        used += CDP_IPV4_SIZE;
        count++;
    }

    if (local->has_ipv6) {
        value[used++] = CDP_ADDRESS_SNAP;
        value[used++] = sizeof(CDP_IPV6_PROTOCOL);

        memcpy(value + used, CDP_IPV6_PROTOCOL,
               sizeof(CDP_IPV6_PROTOCOL));
        used += sizeof(CDP_IPV6_PROTOCOL);

        write16(value + used, CDP_IPV6_SIZE);
        used += 2;

        memcpy(value + used, local->ipv6, CDP_IPV6_SIZE);
        used += CDP_IPV6_SIZE;
        count++;
    }

    write32(value, count);
    return used;
}

size_t cdp_tx_build(uint8_t *frame, size_t capacity, const CdpTx *local)
{
    size_t limit = capacity < CDP_TX_FRAME_SIZE
                 ? capacity : CDP_TX_FRAME_SIZE;

    if (frame == NULL || local == NULL || limit < ETH_FRAME_MIN ||
        local->device == NULL || local->port == NULL ||
        local->device[0] == '\0' || local->port[0] == '\0')
        return 0;

    memset(frame, 0, CDP_TLV_OFFSET);

    memcpy(frame, CDP_MULTICAST_MAC, ETH_MAC_SIZE);
    memcpy(frame + ETH_MAC_SIZE, local->mac, ETH_MAC_SIZE);
    memcpy(frame + ETH_HEADER_SIZE, CDP_SNAP, sizeof(CDP_SNAP));

    frame[CDP_OFFSET] = CDP_VERSION_2;
    frame[CDP_OFFSET + 1] = local->ttl;

    size_t used = CDP_TLV_OFFSET;

    uint8_t addresses[
        CDP_ADDRESS_COUNT_SIZE +
        (1u + 1u + 1u + 2u + CDP_IPV4_SIZE) +
        (1u + 1u + CDP_IPV6_PROTOCOL_SIZE + 2u + CDP_IPV6_SIZE)
    ];

    size_t address_length = encode_addresses(addresses, local);

    if (!add_tlv(frame, limit, &used, CDP_TLV_DEVICE_ID,
                 local->device, strlen(local->device)) ||
        !add_tlv(frame, limit, &used, CDP_TLV_ADDRESSES,
                 addresses, address_length) ||
        !add_tlv(frame, limit, &used, CDP_TLV_PORT_ID,
                 local->port, strlen(local->port)))
        return 0;

    if (local->has_capabilities) {
        uint8_t value[CDP_CAPABILITIES_SIZE];
        write32(value, local->capabilities);

        if (!add_tlv(frame, limit, &used, CDP_TLV_CAPABILITIES,
                     value, sizeof(value)))
            return 0;
    }

    if (local->software != NULL &&
        !add_tlv(frame, limit, &used, CDP_TLV_SOFTWARE,
                 local->software, strlen(local->software)))
        return 0;

    if (local->platform != NULL &&
        !add_tlv(frame, limit, &used, CDP_TLV_PLATFORM,
                 local->platform, strlen(local->platform)))
        return 0;

    if (local->has_native_vlan) {
        uint8_t value[CDP_NATIVE_VLAN_SIZE];
        write16(value, local->native_vlan);

        if (!add_tlv(frame, limit, &used, CDP_TLV_NATIVE_VLAN,
                     value, sizeof(value)))
            return 0;
    }

    if (local->has_voice_vlan) {
        uint8_t value[CDP_VOICE_VLAN_SIZE];
        value[0] = local->voice_vlan_data;
        write16(value + 1, local->voice_vlan);

        if (!add_tlv(frame, limit, &used, CDP_TLV_VOICE_VLAN_REPLY,
                     value, sizeof(value)))
            return 0;
    }

    write16(frame + ETH_LENGTH_OFFSET,
            (uint16_t)(used - ETH_HEADER_SIZE));

    write16(frame + CDP_OFFSET + CDP_CHECKSUM_OFFSET,
            checksum(frame + CDP_OFFSET, used - CDP_OFFSET));

    if (used < ETH_FRAME_MIN) {
        memset(frame + used, 0, ETH_FRAME_MIN - used);
        used = ETH_FRAME_MIN;
    }

    return used;
}
