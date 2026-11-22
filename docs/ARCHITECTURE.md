# Architecture

| Module | Responsibility |
|---|---|
| `main.c` | Command-line entry point |
| `runtime.c` | Starts workers, manages timers and shared state, and handles shutdown |
| `net.c` | Linux Ethernet socket, frame I/O, interface MAC and IP addresses |
| `cdp.c` | CDP frame encoding, checksum, and TLV decoding |
| `neighbors.c` | Fixed neighbor table, refresh, expiry, and neighbor event logs |
| `cli.c` | Command parsing and display |
| `log.c` | Live log mode and synchronized output |

## Receive path

RX receives an Ethernet frame and decodes it into a temporary record. After the frame passes validation, RX updates the neighbor table. Each slot owns its copy of the decoded fields, so RX can reuse its frame buffer.

The checksum adds network-order 16-bit words, applies the CDP rule for an unpaired final byte, folds the carry, and complements the result.

## Transmit path

Main advances the send countdown. When it expires, main sets a shared flag and signals the TX condition variable. TX reads current interface addresses, builds an advertisement, sends it, and reloads the interval. Settings changes and shutdown can also wake TX.

## Shared data and shutdown

`state_mutex` protects runtime settings, stop flags, and TX timers and counters. `table_mutex` protects neighbor entries and RX counters. Code taking both acquires state before table. Logging takes its own lock before the standard-output lock.

Main handles SIGINT and SIGTERM. A stop request wakes TX, and main joins the workers before closing the socket and destroying synchronization objects.
