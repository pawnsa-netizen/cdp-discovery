#include <assert.h>
#include <string.h>
#include "cdp.h"
#include "neighbors.h"
int main(void)
{
    CdpTx tx = {.mac = {2, 0, 0, 0, 0, 1}, .ttl = 3, .device = "Switch",
                .port = "p1", .has_ipv4 = true, .ipv4 = {192, 0, 2, 1}};
    uint8_t frame[CDP_TX_FRAME_SIZE];
    CdpInfo rx;
    NeighborTable table = {0};
    for (unsigned extra = 0; extra < 2; ++extra) {
        tx.platform = extra ? "x" : NULL;
        size_t length = cdp_tx_build(frame, sizeof frame, &tx);
        assert(length && cdp_decode(frame, length, &rx) == CDP_OK);
        assert(rx.ttl == 3 && rx.has_ipv4 && rx.ipv4[3] == 1);
        assert(!rx.has_native_vlan && !rx.has_voice_vlan);
        assert(rx.device.length == 6 && memcmp(rx.device.bytes, "Switch", 6) == 0);
        frame[CDP_OFFSET + CDP_CHECKSUM_OFFSET] ^= 1;
        assert(cdp_decode(frame, length, &rx) == CDP_BAD_CHECKSUM);
    }
    neighbors_update(&table, &rx);
    neighbors_tick(&table, 400);
    assert(table.entries[0].remaining_ms == 2600);
    neighbors_update(&table, &rx);
    assert(table.entries[0].remaining_ms == 3000);
    neighbors_tick(&table, 3100);
    assert(!table.entries[0].used);
    neighbors_update(&table, &rx);
    rx.ttl = 0;
    neighbors_update(&table, &rx);
    assert(!table.entries[0].used);
}
