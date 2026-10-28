#ifndef DSM_SENSORS_DAEMON_H
#define DSM_SENSORS_DAEMON_H

#include "sensors/chips.h"

/* The loop every sensor program runs (docs/m3-plan.md): a START record,
 * then one SAMPLE per period sent to stored. A failed read sends a FAULT;
 * 3 in a row exits with code 3 ("sensor absent"). SIGTERM/SIGINT exit 0.
 * Usage: <program> <stored-socket> [run-number] */
int sensor_main(const struct sensor_ops *ops, int argc, char **argv);

#endif
