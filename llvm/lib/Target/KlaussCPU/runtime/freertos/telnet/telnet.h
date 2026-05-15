/*
 * telnet.h — KlaussCPU FreeRTOS telnet server public interface.
 *
 * Single-client telnet server on port 23.  Provides a small interactive
 * command shell over TCP.  Compatible with standard telnet(1) clients and
 * PuTTY in Telnet mode.
 */

#ifndef KLAUSSCPU_TELNET_H
#define KLAUSSCPU_TELNET_H

#include <stdint.h>

#define TELNET_PORT         23
#define TELNET_TASK_STACK   512     /* words × 8 B = 4 KB */
#define TELNET_TASK_PRIO    2

/*
 * Start the telnet server task.
 * Call after tcpip_init() and netif setup (network does not have to be
 * up yet — the task will listen once an IP is assigned).
 *
 * ntp_time_ptr: pointer to a volatile time_t updated by the NTP callback,
 *               or NULL if NTP is not running.  Used by the 'time' command.
 */
void telnet_start(volatile time_t *ntp_time_ptr);

#endif /* KLAUSSCPU_TELNET_H */
