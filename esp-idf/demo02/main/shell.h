/*
 * TCP command shell for the Waveshare ESP32-C6-LCD-1.47 — ESP-IDF (C).
 *
 * Faithful port of demo/demo02/shell.py. A tiny remote shell over a plain
 * TCP socket (telnet <ip> 23). Every command is a _cmd_* function returning
 * text; _execute() dispatches a typed line. A `tick` callback is invoked
 * while waiting for connections/input, so the caller can drive a screensaver
 * without threads.
 */
#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

#define SHELL_PORT 23

typedef void (*shell_tick_fn)(void);

/* Boot timestamp (microseconds since boot) for the 'uptime' command. */
extern int64_t shell_boot_us;

/* Start the shell server; blocks forever. */
void shell_run(uint16_t port, shell_tick_fn tick);

#endif /* SHELL_H */
