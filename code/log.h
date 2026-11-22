#ifndef CDP_LOG_H
#define CDP_LOG_H

#include <stdbool.h>

void cdp_log_set_enabled(bool enabled);
/* A successful begin holds the output lock until cdp_log_end(). */
bool cdp_log_begin(void);
void cdp_log_end(void);
void cdp_log(const char *format, ...);

#endif
