/* presence_port_stub.c — no presence sensor in this build: the screen is always wanted. */
#include "presence_port.h"
#include "esp_log.h"

bool presence_port_init(i2c_master_bus_handle_t bus) { (void)bus; return false; }
void presence_port_activity(void) {}
bool presence_port_screen_wanted(void) { return true; }
void presence_port_set_battery(bool on_battery) { (void)on_battery; }
int  presence_port_distance_mm(void) { return -1; }
void presence_port_command(int argc, char **argv) { (void)argc; (void)argv; ESP_LOGI("presence", "no presence sensor in this build (POCKET_TANK_PRESENCE_VL53L1X)"); }
