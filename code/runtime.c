#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "cli.h"
#include "cdp.h"
#include "net.h"
#include "neighbors.h"
#include "log.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    const char *interface;
    pthread_mutex_t state_mutex; /* stop, configuration, TX countdown/counters */
    pthread_mutex_t table_mutex; /* neighbors and RX counters */
    pthread_cond_t wake_tx;
    bool stopping;
    bool failed;
    bool enabled;
    bool tx_due;
    unsigned interval;
    unsigned holdtime;
    uint32_t tx_remaining_ms;
    uint64_t tx_clock_ms;
    uint64_t table_clock_ms;
    NeighborTable table;
    uint64_t rx_status[CDP_MISSING_ID + 1];
    uint64_t tx_ok;
    uint64_t tx_errors;
} CdpApp;

static bool check_thread_result(int error, const char *operation)
{
    if (error == 0)
        return true;
    fprintf(stderr, "%s: %s\n", operation, strerror(error));
    return false;
}

static bool is_stopping(CdpApp *app)
{
    pthread_mutex_lock(&app->state_mutex);
    bool stopping = app->stopping;
    pthread_mutex_unlock(&app->state_mutex);
    return stopping;
}

static void request_stop(CdpApp *app, bool failed)
{
    pthread_mutex_lock(&app->state_mutex);
    app->stopping = true;
    app->failed = app->failed || failed;
    pthread_cond_broadcast(&app->wake_tx);
    pthread_mutex_unlock(&app->state_mutex);
}

static bool monotonic_ms(uint64_t *now_ms)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        perror("clock_gettime");
        return false;
    }
    *now_ms = (uint64_t)now.tv_sec * APP_MILLISECONDS_PER_SECOND
            + (uint64_t)now.tv_nsec / APP_NANOSECONDS_PER_MILLISECOND;
    return true;
}

/* Hold table_mutex while updating both the table and its clock. */
static bool tick_neighbors_locked(CdpApp *app)
{
    uint64_t now_ms;
    if (!monotonic_ms(&now_ms))
        return false;
    neighbors_tick(&app->table, now_ms - app->table_clock_ms);
    app->table_clock_ms = now_ms;
    return true;
}

/* Hold state_mutex while advancing the TX countdown. */
static bool tick_tx_locked(CdpApp *app)
{
    uint64_t now_ms;
    if (!monotonic_ms(&now_ms))
        return false;
    uint64_t elapsed_ms = now_ms - app->tx_clock_ms;
    app->tx_clock_ms = now_ms;
    if (app->enabled && !app->tx_due && app->tx_remaining_ms != 0) {
        if (elapsed_ms >= app->tx_remaining_ms) {
            app->tx_remaining_ms = 0;
            app->tx_due = true;
            pthread_cond_signal(&app->wake_tx);
        } else {
            app->tx_remaining_ms -= (uint32_t)elapsed_ms;
        }
    }
    return true;
}

static bool process_frame(CdpApp *app, const uint8_t *frame, size_t length)
{
    pthread_mutex_lock(&app->state_mutex);
    bool enabled = app->enabled;
    pthread_mutex_unlock(&app->state_mutex);
    if (!enabled)
        return true;

    CdpInfo info;
    CdpStatus status = cdp_decode(frame, length, &info);
    bool ok = true;
    /* Lock order when both are needed: state, then table. */
    pthread_mutex_lock(&app->state_mutex);
    if (app->enabled && !app->stopping) {
        pthread_mutex_lock(&app->table_mutex);
        app->rx_status[status]++;
        if (status == CDP_OK) {
            ok = tick_neighbors_locked(app);
            if (ok)
                neighbors_update(&app->table, &info);
        } else if (status != CDP_NOT_CDP && status != CDP_BAD_TLV) {
            cdp_log("Dropped CDP: %s\n", cdp_status_string(status));
        }
        pthread_mutex_unlock(&app->table_mutex);
    }
    pthread_mutex_unlock(&app->state_mutex);
    return ok;
}

static void *rx_worker(void *argument)
{
    CdpApp *app = argument;
    uint8_t frame[APP_RX_FRAME_SIZE];
    while (!is_stopping(app)) {
        ssize_t length = net_receive(app->fd, frame, sizeof(frame));
        if (length == -1) {
            perror("recvfrom");
            request_stop(app, true);
            break;
        }
        if (is_stopping(app))
            break;
        if (length > 0 && !process_frame(app, frame, (size_t)length)) {
            request_stop(app, true);
            break;
        }
    }
    return NULL;
}

static bool transmit(CdpApp *app, const CdpTx *local)
{
    uint8_t frame[CDP_TX_FRAME_SIZE];
    size_t length = cdp_tx_build(frame, sizeof(frame), local);
    bool sent = length != 0 && net_send(app->fd, frame, length);
    pthread_mutex_lock(&app->state_mutex);
    if (sent)
        app->tx_ok++;
    else
        app->tx_errors++;
    pthread_mutex_unlock(&app->state_mutex);
    if (!sent) {
        if (length == 0)
            fprintf(stderr, "Cannot build CDP advertisement.\n");
        return false;
    }
    cdp_log("TX CDPv2 TTL=%us IPv4=%s IPv6=%s%s\n",
            (unsigned)local->ttl, local->has_ipv4 ? "present" : "absent",
            local->has_ipv6 ? "present" : "absent",
            local->ttl == 0 ? " (withdrawal)" : "");
    return true;
}

static void *tx_worker(void *argument)
{
    CdpApp *app = argument;
    CdpTx local = {
        .device = APP_DEVICE_ID, .port = app->interface,
        .has_capabilities = true, .capabilities = CDP_CAP_HOST,
        .software = APP_SOFTWARE, .platform = APP_PLATFORM,
        .has_native_vlan = APP_ADVERTISE_NATIVE_VLAN, .native_vlan = APP_NATIVE_VLAN,
        .has_voice_vlan = APP_ADVERTISE_VOICE_VLAN,
        .voice_vlan_data = APP_VOICE_VLAN_DATA, .voice_vlan = APP_VOICE_VLAN
    };
    CdpTx last = {0};
    bool advertised = false;
    for (;;) {
        pthread_mutex_lock(&app->state_mutex);
        while (!app->stopping && !app->tx_due && (app->enabled || !advertised)) {
            int error = pthread_cond_wait(&app->wake_tx, &app->state_mutex);
            if (!check_thread_result(error, "wait for TX")) {
                app->stopping = true;
                app->failed = true;
            }
        }
        bool stopping = app->stopping;
        bool send = !stopping && app->enabled && app->tx_due;
        if (send)
            app->tx_due = false;
        pthread_mutex_unlock(&app->state_mutex);

        if (!send) {
            if (advertised) {
                last.ttl = 0;
                if (!transmit(app, &last))
                    request_stop(app, true);
                advertised = false;
            }
            if (stopping || is_stopping(app))
                break;
            continue; /* Disabled: wait for enable or stop. */
        }
        if (!net_get_mac(app->fd, local.mac) ||
            !net_get_if_addr(app->interface, &local)) {
            request_stop(app, true);
            continue;
        }
        pthread_mutex_lock(&app->state_mutex);
        bool allowed = app->enabled && !app->stopping;
        local.ttl = (uint8_t)app->holdtime;
        pthread_mutex_unlock(&app->state_mutex);
        if (!allowed)
            continue;
        if (!transmit(app, &local)) {
            request_stop(app, true);
            continue;
        }
        advertised = true;
        last = local;
        pthread_mutex_lock(&app->state_mutex);
        bool ok = monotonic_ms(&app->tx_clock_ms);
        app->tx_remaining_ms = app->enabled && !app->tx_due
                            ? app->interval * APP_MILLISECONDS_PER_SECOND : 0;
        pthread_mutex_unlock(&app->state_mutex);
        if (!ok)
            request_stop(app, true);
    }
    return NULL;
}

static void process_command(CdpApp *app, CdpCliCommand command, unsigned value)
{
    if (command == CLI_NONE)
        return;
    if (command == CLI_QUIT || command == CLI_ERROR) {
        request_stop(app, command == CLI_ERROR);
        return;
    }
    pthread_mutex_lock(&app->state_mutex);
    pthread_mutex_lock(&app->table_mutex);
    /* Bring the countdowns up to date before showing or changing them. */
    if (!tick_neighbors_locked(app) || !tick_tx_locked(app)) {
        pthread_mutex_unlock(&app->table_mutex);
        pthread_mutex_unlock(&app->state_mutex);
        request_stop(app, true);
        return;
    }
    flockfile(stdout);
    switch (command) {
    case CLI_HELP:
        cli_help();
        break;
    case CLI_STATUS:
        cli_show_status(app->interface, app->enabled, app->interval, app->holdtime,
                        (app->tx_remaining_ms + APP_MILLISECONDS_PER_SECOND - 1u) / APP_MILLISECONDS_PER_SECOND);
        break;
    case CLI_NEIGHBORS:
    case CLI_DETAIL:
        cli_show_neighbors(&app->table, command == CLI_DETAIL);
        break;
    case CLI_TRAFFIC:
        cli_show_traffic(app->rx_status, app->tx_ok, app->tx_errors);
        break;
    case CLI_CLEAR_TABLE:
        for (size_t i = 0; i < NEIGHBOR_LIMIT; i++)
            app->table.entries[i].used = false;
        puts("Local neighbor table cleared.");
        break;
    case CLI_CLEAR_COUNTERS:
        memset(app->rx_status, 0, sizeof(app->rx_status));
        app->tx_ok = 0;
        app->tx_errors = 0;
        puts("CDP counters cleared.");
        break;
    case CLI_ENABLE:
        if (!app->enabled) {
            app->enabled = true;
            app->tx_due = true;
            app->tx_remaining_ms = 0;
            pthread_cond_signal(&app->wake_tx);
        }
        puts("CDP enabled.");
        break;
    case CLI_DISABLE:
        app->enabled = false;
        app->tx_due = false;
        app->tx_remaining_ms = 0;
        pthread_cond_signal(&app->wake_tx);
        puts("CDP disabled; RX updates and periodic TX paused. Neighbors still age.");
        break;
    case CLI_TIMER:
        if (value < APP_TIMER_MIN_SECONDS || value > APP_TIMER_MAX_SECONDS ||
            value >= app->holdtime) {
            puts("Timer must be 5..254 seconds and less than holdtime.");
            break;
        }
        app->interval = value;
        app->tx_due = app->enabled;
        app->tx_remaining_ms = 0;
        pthread_cond_signal(&app->wake_tx);
        printf("CDP timer: %us.\n", value);
        break;
    case CLI_HOLDTIME:
        if (value < APP_HOLDTIME_MIN_SECONDS || value > APP_HOLDTIME_MAX_SECONDS ||
            value <= app->interval) {
            puts("Holdtime must be 10..255 seconds and greater than timer.");
            break;
        }
        app->holdtime = value;
        if (app->enabled) {
            app->tx_due = true;
            app->tx_remaining_ms = 0;
            pthread_cond_signal(&app->wake_tx);
        }
        printf("CDP holdtime: %us.\n", value);
        break;
    default:
        puts("Invalid command or input too long. Type help.");
        break;
    }
    fflush(stdout);
    funlockfile(stdout);
    pthread_mutex_unlock(&app->table_mutex);
    pthread_mutex_unlock(&app->state_mutex);
}

static void *cli_worker(void *argument)
{
    CdpApp *app = argument;
    CdpCli cli;
    cli_init(&cli);
    while (!is_stopping(app) && cli.active) {
        /* A short input timeout lets the CLI check for shutdown. */
        CdpCliCommand command = cli_next(&cli, 1000);
        if (is_stopping(app))
            break;
        if (command == CLI_MODE_COMMANDS || command == CLI_MODE_LOGS) {
            cli.mode = command == CLI_MODE_COMMANDS ? 1 : 2;
            cdp_log_set_enabled(cli.mode == 2);
            puts(cli.mode == 1 ? "Mode 1: commands."
                              : "Mode 2: live logs. Type mode 1 to return; quit stops CDP.");
        } else if (cli.mode == 2 && command != CLI_NONE && command != CLI_QUIT &&
                   command != CLI_ERROR) {
            puts("Type mode 1 to enter commands.");
        } else {
            process_command(app, command, cli.value);
        }
        if (command != CLI_NONE) {
            fflush(stdout);
            if (!is_stopping(app))
                cli_prompt(&cli);
        }
    }
    return NULL;
}

static bool init_sync(CdpApp *app)
{
    if (!check_thread_result(pthread_mutex_init(&app->state_mutex, NULL),
                             "initialize state mutex"))
        return false;
    if (!check_thread_result(pthread_mutex_init(&app->table_mutex, NULL),
                             "initialize table mutex")) {
        pthread_mutex_destroy(&app->state_mutex);
        return false;
    }
    if (!check_thread_result(pthread_cond_init(&app->wake_tx, NULL),
                             "initialize TX condition")) {
        pthread_mutex_destroy(&app->table_mutex);
        pthread_mutex_destroy(&app->state_mutex);
        return false;
    }
    return true;
}

/* Main advances timers between checks for shutdown signals. */
static void run_control(CdpApp *app, const sigset_t *signals)
{
    while (!is_stopping(app)) {
        pthread_mutex_lock(&app->state_mutex);
        bool ok = tick_tx_locked(app);
        unsigned wait_ms = APP_CONTROL_TICK_SECONDS * APP_MILLISECONDS_PER_SECOND;
        /* Wake sooner if the next send is less than one tick away. */
        if (app->enabled && !app->tx_due && app->tx_remaining_ms != 0 &&
            app->tx_remaining_ms < wait_ms)
            wait_ms = app->tx_remaining_ms;
        pthread_mutex_unlock(&app->state_mutex);
        pthread_mutex_lock(&app->table_mutex);
        ok = tick_neighbors_locked(app) && ok;
        pthread_mutex_unlock(&app->table_mutex);
        if (!ok) {
            request_stop(app, true);
            break;
        }
        struct timespec timeout = {
            .tv_sec = wait_ms / APP_MILLISECONDS_PER_SECOND,
            .tv_nsec = (long)(wait_ms % APP_MILLISECONDS_PER_SECOND) * APP_NANOSECONDS_PER_MILLISECOND
        };
        int signal_number = sigtimedwait(signals, NULL, &timeout);
        if (signal_number == SIGINT || signal_number == SIGTERM) {
            request_stop(app, false);
            break;
        }
        if (signal_number == -1 && errno != EAGAIN && errno != EINTR) {
            perror("sigtimedwait");
            request_stop(app, true);
            break;
        }
    }
}

int cdp_run(const char *interface)
{
    CdpApp app = {
        .fd = -1, .interface = interface, .enabled = true, .tx_due = true,
        .interval = CDP_TX_INTERVAL_MS / APP_MILLISECONDS_PER_SECOND, .holdtime = CDP_TX_TTL
    };
    if (!init_sync(&app))
        return 1;
    int result = 1;
    sigset_t signals, old_mask;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (!check_thread_result(pthread_sigmask(SIG_BLOCK, &signals, &old_mask),
                             "block shutdown signals"))
        goto destroy_sync;
    app.fd = net_open(interface, CDP_MULTICAST_MAC);
    if (app.fd == -1)
        goto restore_signals;
    uint8_t interface_mac[ETH_MAC_SIZE];
    if (!net_get_mac(app.fd, interface_mac) || !monotonic_ms(&app.tx_clock_ms))
        goto close_socket;
    app.table_clock_ms = app.tx_clock_ms;
    cdp_log_set_enabled(false);
    printf("CDP RX/TX on %s as %s (RX, TX, CLI workers)...\n", interface, APP_DEVICE_ID);
    fflush(stdout);

    pthread_t workers[3];
    void *(*entries[])(void *) = {rx_worker, tx_worker, cli_worker};
    const char *names[] = {"create RX thread", "create TX thread", "create CLI thread"};
    size_t started = 0;
    for (; started < 3; started++) {
        if (!check_thread_result(pthread_create(&workers[started], NULL,
                                                entries[started], &app), names[started])) {
            request_stop(&app, true);
            break;
        }
    }
    if (started == 3)
        run_control(&app, &signals);
    /* Join only the workers that started successfully. */
    for (size_t i = 0; i < started; i++)
        if (!check_thread_result(pthread_join(workers[i], NULL), "join worker"))
            abort(); /* A live worker may still hold a pointer to app. */
    result = app.failed ? 1 : 0;

close_socket:
    close(app.fd);
restore_signals:
    if (!check_thread_result(pthread_sigmask(SIG_SETMASK, &old_mask, NULL),
                             "restore shutdown signals"))
        result = 1;
destroy_sync:
    pthread_cond_destroy(&app.wake_tx);
    pthread_mutex_destroy(&app.table_mutex);
    pthread_mutex_destroy(&app.state_mutex);
    return result;
}
