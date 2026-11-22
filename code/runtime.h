#ifndef RUNTIME_H
#define RUNTIME_H

/* Receive buffer and control-loop interval. */
#define APP_RX_FRAME_SIZE 2048u
#define APP_CONTROL_TICK_SECONDS 1

/* Runtime also requires holdtime to exceed the send interval. */
#define APP_TIMER_MIN_SECONDS 5u
#define APP_TIMER_MAX_SECONDS 254u
#define APP_HOLDTIME_MIN_SECONDS 10u
#define APP_HOLDTIME_MAX_SECONDS 255u

#define APP_MILLISECONDS_PER_SECOND 1000u
#define APP_NANOSECONDS_PER_MILLISECOND 1000000L

/* Local identity and optional VLAN advertisements. */
#define APP_DEVICE_ID "cdp-discovery"
#define APP_SOFTWARE "cdp-discovery"
#define APP_PLATFORM "Linux"
#define APP_ADVERTISE_NATIVE_VLAN false
#define APP_NATIVE_VLAN 1u
#define APP_ADVERTISE_VOICE_VLAN false
#define APP_VOICE_VLAN_DATA 0x01u
#define APP_VOICE_VLAN 1u

int cdp_run(const char *interface);

#endif
