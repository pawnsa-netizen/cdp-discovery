# Validation

The Linux host build passed with GCC using C11 and `-Wall -Wextra -Wpedantic -Werror`. The executable printed usage with `--help` and returned errors for missing arguments and a nonexistent interface.

The offline smoke test builds and decodes CDPv2 frames of both CDP payload parities, checks decoded identity and IPv4 data, checks rejection of a corrupted checksum, and exercises neighbor countdown, refresh, expiry, and TTL-zero removal.

The smoke test exits silently on success. The strict build and smoke test were rerun after the source readability pass. It also passed under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection disabled because the test environment does not support it.

The refactored checksum matched the previous implementation for deterministic sample messages at each length from 4 through 1,492 bytes.

The presentation includes anonymized evidence from earlier lab runs. Live Ethernet exchange with a peer was not rerun for this package.
