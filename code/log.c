#define _POSIX_C_SOURCE 200809L
#include "log.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool log_enabled;

void cdp_log_set_enabled(bool enabled)
{
    pthread_mutex_lock(&log_mutex);
    log_enabled = enabled;
    pthread_mutex_unlock(&log_mutex);
}

bool cdp_log_begin(void)
{
    pthread_mutex_lock(&log_mutex);
    if (!log_enabled) {
        pthread_mutex_unlock(&log_mutex);
        return false;
    }
    flockfile(stdout);
    return true;
}

void cdp_log_end(void)
{
    fflush(stdout);
    funlockfile(stdout);
    pthread_mutex_unlock(&log_mutex);
}

void cdp_log(const char *format, ...)
{
    if (!cdp_log_begin())
        return;
    va_list arguments;
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    cdp_log_end();
}
