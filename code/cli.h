#ifndef CLI_H
#define CLI_H

#include "neighbors.h"

typedef enum {
    CLI_NONE,
    CLI_HELP,
    CLI_STATUS,
    CLI_NEIGHBORS,
    CLI_DETAIL,
    CLI_TRAFFIC,
    CLI_CLEAR_TABLE,
    CLI_CLEAR_COUNTERS,
    CLI_QUIT,
    CLI_ENABLE,
    CLI_DISABLE,
    CLI_TIMER,
    CLI_HOLDTIME,
    CLI_MODE_COMMANDS,
    CLI_MODE_LOGS,
    CLI_INVALID,
    CLI_ERROR
} CdpCliCommand;

typedef struct {
    char line[128];
    size_t used;
    bool discard;
    bool active;
    bool interactive;
    unsigned mode;
    unsigned value;
} CdpCli;

void cli_init(CdpCli *cli);
void cli_prompt(const CdpCli *cli);
CdpCliCommand cli_next(CdpCli *cli, int timeout_ms);
void cli_help(void);
void cli_show_status(const char *interface, bool enabled, unsigned interval,
                     unsigned holdtime, unsigned tx_remaining);
void cli_show_neighbors(const NeighborTable *table, bool detail);
void cli_show_traffic(const uint64_t *rx_status, uint64_t tx_ok, uint64_t tx_errors);

#endif
