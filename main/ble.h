#include <sys/cdefs.h>

#ifndef APP_TEMPLATE_BLE_H
#define APP_TEMPLATE_BLE_H

_Noreturn void ble_station_run();

void send_data(char addr_str[18], int rssi);

_Noreturn void init_mqtt();

void init_ble();

#endif //APP_TEMPLATE_BLE_H
