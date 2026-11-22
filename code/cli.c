#define _POSIX_C_SOURCE 200809L
#include "cli.h"
#include "runtime.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const struct {
    const char *text;
    CdpCliCommand command;
} commands[] = {
    {"help", CLI_HELP},
    {"?", CLI_HELP},
    {"show cdp", CLI_STATUS},
    {"show cdp neighbors", CLI_NEIGHBORS},
    {"show cdp neighbors detail", CLI_DETAIL},
    {"show cdp traffic", CLI_TRAFFIC},
    {"clear cdp table", CLI_CLEAR_TABLE},
    {"clear cdp counters", CLI_CLEAR_COUNTERS},
    {"quit", CLI_QUIT},
    {"exit", CLI_QUIT},
    {"cdp enable", CLI_ENABLE},
    {"cdp disable", CLI_DISABLE},
    {"mode 1", CLI_MODE_COMMANDS},
    {"mode 2", CLI_MODE_LOGS},
    {"1", CLI_MODE_COMMANDS},
    {"2", CLI_MODE_LOGS}
};

void cli_init(CdpCli *cli)
{
    memset(cli, 0, sizeof(*cli));
    cli->active = true;
    cli->mode = 1;
    cli->interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (cli->interactive)
        puts("Mode 1: commands. Type help; mode 2 watches live logs.");
    cli_prompt(cli);
}

void cli_prompt(const CdpCli *cli)
{
    if (cli->interactive && cli->active && cli->mode == 1) {
        flockfile(stdout);
        fputs("cdp> ", stdout);
        fflush(stdout);
        funlockfile(stdout);
    }
}

static CdpCliCommand parse_line(CdpCli *cli)
{
    cli->value = 0;
    bool discard = cli->discard;
    cli->line[cli->used] = '\0';
    cli->used = 0;
    cli->discard = false;
    if (discard)
        return CLI_INVALID;

    /* Normalize spaces/tabs and remove trailing CR from CRLF input. */
    size_t used = 0;
    bool space = false;
    for (size_t i = 0; cli->line[i] != '\0'; i++) {
        if (isspace((unsigned char)cli->line[i])) {
            space = used != 0;
        } else {
            if (space)
                cli->line[used++] = ' ';
            cli->line[used++] = cli->line[i];
            space = false;
        }
    }
    cli->line[used] = '\0';
    if (used == 0)
        return CLI_NONE;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(cli->line, commands[i].text) == 0)
            return commands[i].command;
    }
    const char *number = NULL;
    CdpCliCommand command = CLI_INVALID;
    if (strncmp(cli->line, "cdp timer ", 10) == 0) {
        number = cli->line + 10;
        command = CLI_TIMER;
    } else if (strncmp(cli->line, "cdp holdtime ", 13) == 0) {
        number = cli->line + 13;
        command = CLI_HOLDTIME;
    }
    if (number != NULL && *number != '\0') {
        for (const char *p = number; *p != '\0'; p++)
            if (!isdigit((unsigned char)*p))
                return CLI_INVALID;
        errno = 0;
        char *end;
        unsigned long value = strtoul(number, &end, 10);
        if (errno == 0 && *end == '\0' && value <= UINT_MAX) {
            cli->value = (unsigned)value;
            return command;
        }
    }
    return CLI_INVALID;
}

CdpCliCommand cli_next(CdpCli *cli, int timeout_ms)
{
    struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
    int ready = poll(cli->active ? &input : NULL, cli->active ? 1 : 0,
                     timeout_ms);
    if (ready < 0) {
        if (errno == EINTR)
            return CLI_NONE;
        perror("CLI input wait");
        return CLI_ERROR;
    }
    if (ready == 0)
        return CLI_NONE;
    if (input.revents & POLLNVAL) {
        cli->active = false;
        return CLI_NONE;
    }
    if (!(input.revents & (POLLIN | POLLHUP | POLLERR)))
        return CLI_NONE;

    /* Keep partial input across polls so the CLI can check for shutdown. */
    unsigned char byte;
    ssize_t length = read(STDIN_FILENO, &byte, 1);
    if (length < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return CLI_NONE;
        perror("CLI input read");
        return CLI_ERROR;
    }
    if (length == 0) {
        /* Keep discovery running after the console closes. */
        cli->active = false;
        return parse_line(cli);
    }
    if (byte == '\n') {
        CdpCliCommand command = parse_line(cli);
        if (command == CLI_NONE)
            cli_prompt(cli);
        return command;
    }
    if (!cli->discard) {
        if (byte == '\0' || cli->used == sizeof(cli->line) - 1)
            cli->discard = true;
        else
            cli->line[cli->used++] = (char)byte;
    }
    return CLI_NONE;
}

void cli_help(void)
{
    puts("help or ?                 Show commands\n"
         "show cdp                  Show local CDP status\n"
         "show cdp neighbors        Show current neighbors\n"
         "show cdp neighbors detail Show all stored neighbor fields\n"
         "show cdp traffic          Show decoder and transmission counters\n"
         "clear cdp table           Clear local neighbors\n"
         "clear cdp counters        Reset counters\n"
         "cdp enable / cdp disable  Enable or disable RX and periodic TX\n"
         "cdp timer <5..254>        Set transmission interval in seconds\n"
         "cdp holdtime <10..255>    Set advertised holdtime (above interval)\n"
         "mode 1 / mode 2          Commands / live logs (1 and 2 also work)\n"
         "quit or exit              Stop CDP cleanly");
}

void cli_show_status(const char *interface, bool enabled, unsigned interval,
                     unsigned holdtime, unsigned tx_remaining)
{
    printf("CDP RX/TX %s on %s\nDevice: %s\nVersion: 2\n"
           "TX interval: %us\nAdvertised holdtime: %us\nNext TX countdown: %us\n",
           enabled ? "enabled" : "disabled", interface, APP_DEVICE_ID,
           interval, holdtime, tx_remaining);
}

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

void cli_show_neighbors(const NeighborTable *table, bool detail)
{
    size_t count = 0;
    for (size_t i = 0; i < NEIGHBOR_LIMIT; i++) {
        const Neighbor *entry = &table->entries[i];
        if (!entry->used || entry->remaining_ms == 0)
            continue;
        const CdpInfo *info = &entry->info;
        for (size_t j = 0; j < ETH_MAC_SIZE; j++)
            printf("%02x%s", (unsigned)info->source_mac[j],
                   j + 1 == ETH_MAC_SIZE ? " " : ":");
        print_text(&info->device);
        fputs(" / ", stdout);
        print_text(&info->port);
        uint64_t remaining = entry->remaining_ms;
        printf("  CDPv%u Holdtime=%" PRIu64 "s\n", (unsigned)info->version,
               remaining / 1000 + (remaining % 1000 != 0));
        count++;
        if (!detail)
            continue;
        char address[INET6_ADDRSTRLEN];
        if (info->has_ipv4 && inet_ntop(AF_INET, info->ipv4, address, sizeof(address)))
            printf("  IPv4: %s\n", address);
        if (info->has_ipv6 && inet_ntop(AF_INET6, info->ipv6, address, sizeof(address)))
            printf("  IPv6: %s\n", address);
        if (info->has_capabilities)
            printf("  Capabilities: 0x%08" PRIx32 "\n", info->capabilities);
        if (info->has_native_vlan)
            printf("  Native VLAN: %u\n", (unsigned)info->native_vlan);
        if (info->has_voice_vlan)
            printf("  Voice VLAN: %u (reply data=0x%02x)\n",
                   (unsigned)info->voice_vlan, (unsigned)info->voice_vlan_data);
        fputs("  Software: ", stdout);
        print_text(&info->software);
        fputs("\n  Platform: ", stdout);
        print_text(&info->platform);
        putchar('\n');
    }
    printf("Current neighbors: %zu\n", count);
}

void cli_show_traffic(const uint64_t *rx_status, uint64_t tx_ok, uint64_t tx_errors)
{
    static const char *const labels[] = {
        "Accepted CDP", "Ignored non-CDP", "Bad frame", "Bad CDP version",
        "Bad checksum", "Bad TLV", "Missing identity"
    };
    for (int i = CDP_OK; i <= CDP_MISSING_ID; i++)
        printf("%s: %" PRIu64 "\n", labels[i], rx_status[i]);
    printf("Successful TX: %" PRIu64 "\nTX errors: %" PRIu64 "\n",
           tx_ok, tx_errors);
}
