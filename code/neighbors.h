#ifndef NEIGHBORS_H
#define NEIGHBORS_H

#include "cdp.h"

#define NEIGHBOR_LIMIT 32

typedef struct {
    bool used;
    CdpInfo info;
    uint32_t remaining_ms;
} Neighbor;

typedef struct {
    Neighbor entries[NEIGHBOR_LIMIT];
} NeighborTable;

void neighbors_tick(NeighborTable *table, uint64_t elapsed_ms);
void neighbors_update(NeighborTable *table, const CdpInfo *info);

#endif
