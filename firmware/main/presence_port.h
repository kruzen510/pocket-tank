/* presence_port.h — "is somebody there?": the screen follows a time-of-flight
 * sensor (VL53L1X on the board's I2C header). The tank keeps running with the
 * screen dark; only the display (and the sound) go off.
 *
 *   - the screen is wanted while anyone is within the near distance, or for the
 *     hold time after the last sign of anyone (a reading, a touch, BOOT);
 *   - no sensor, or one that stops answering: the screen stays ON (fail open);
 *   - the ranging cadence is fast on USB and while the screen is on, slow on
 *     battery with the screen off: a reading costs ~0.4 mC (about 21 ms at 18 mA),
 *     so it hardly matters next to the tank's own draw, but it sets how soon a
 *     visitor lights it.
 * The sensor is read on a task of its own (a reading blocks ~20-65 ms). */
#ifndef PRESENCE_PORT_H
#define PRESENCE_PORT_H
#include <stdbool.h>
#include "driver/i2c_master.h"

bool presence_port_init(i2c_master_bus_handle_t bus);   /* false = no presence in this build; the screen is always wanted */
void presence_port_activity(void);                       /* a touch / a button: somebody is here, restart the hold */
bool presence_port_screen_wanted(void);                  /* call every frame */
void presence_port_set_battery(bool on_battery);         /* picks the cadence */
int  presence_port_distance_mm(void);                    /* last valid reading, -1 = none / out of range */
void presence_port_command(int argc, char **argv);       /* director `presence ...` (argv excludes the word itself) */
#endif
