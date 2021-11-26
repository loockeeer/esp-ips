#include <sys/cdefs.h>
#include <stdio.h>
#include <esp_gap_ble_api.h>
#include <esp_bt.h>
#include <nvs_flash.h>
#include <esp_websocket_client.h>
#include <mqtt_client.h>
#include <esp_bt_device.h>
#include "esp_bt_main.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "ble.h"
#include "main.h"

#define CARMODE 0
#define ANTENNAMODE_INIT 1
#define ANTENNAMODE_RUN 2
#define IDLE_MODE 3

#define ACK 4
#define PING 5

static esp_mqtt_client_handle_t mqttClient;
static char topic_rssi[23];
static char topic_cc[23];
static char esp_address[18];
static int mode = IDLE_MODE;
static int previous_mode = IDLE_MODE;

static esp_ble_adv_params_t adv_params = {
        .adv_int_min        = 0x20,
        .adv_int_max        = 0x40,
        .adv_type           = ADV_TYPE_IND,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .channel_map        = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};


void mqtt_callback(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    if (event_id != MQTT_EVENT_DATA) return;

    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    printf("topic : %.*s\n", event->topic_len, event->topic);

    // Get event->topic_len first chars of event->topic and store in topic
    char topic[event->topic_len + 1];
    memcpy(topic, event->topic, event->topic_len);

    char data[event->data_len + 1];
    memcpy(data, event->data, event->data_len);

    if (*topic != *topic_cc) return;
    int command = (int) strtol(data, NULL, 10);

    if(command == ACK) return;
    else if (command != PING) {
        previous_mode = mode;
        mode = command;
        printf("Received new mode : %s (%i)\n",
               mode == CARMODE ? "CAR" : mode == ANTENNAMODE_RUN ? "ANTENNA (RUN)" : mode == ANTENNAMODE_INIT ? "ANTENNA (INIT)" : "undefined", mode);
    }

    esp_mqtt_client_publish(mqttClient, topic_cc, "4", 1, 2, 0);

}

_Noreturn void init_mqtt() {
    const esp_mqtt_client_config_t mqtt_config = {
            .host = CONFIG_MQTT_HOST,
            .port = 1883,
            .transport = MQTT_TRANSPORT_OVER_TCP,
    };

    println("Initating mqtt");
    mqttClient = esp_mqtt_client_init(&mqtt_config);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqttClient, MQTT_EVENT_DATA, &mqtt_callback, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqttClient));

    init_ble();

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_mqtt_client_publish(mqttClient, "announce", esp_address, 17, 2, 0);
    esp_mqtt_client_subscribe(mqttClient, topic_cc, 2);
    esp_mqtt_client_subscribe(mqttClient, "cc", 2);
    println("Listening for orders from cc (all and device only)");

    while (true) {
        if ((previous_mode == ANTENNAMODE_INIT && mode != ANTENNAMODE_INIT) || (previous_mode == CARMODE && mode != CARMODE)) {
            esp_ble_gap_stop_advertising();
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
        switch (mode) {
            case CARMODE:
                if (previous_mode != mode) {
                    println("Switching to car mode");
                    ESP_ERROR_CHECK(esp_ble_gap_start_advertising(&adv_params));
                    previous_mode = mode;
                }
                vTaskDelay(500 / portTICK_PERIOD_MS);
                break;
            case ANTENNAMODE_INIT:
                if (previous_mode != mode) {
                    println("Switching to antenna mode (init)");
                    static esp_ble_scan_params_t scan_params = {
                            .own_addr_type          = BLE_ADDR_TYPE_RPA_PUBLIC,
                            .scan_duplicate         = BLE_SCAN_DUPLICATE_ENABLE,
                            .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL
                    };
                    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));
                    ESP_ERROR_CHECK(esp_ble_gap_start_advertising(&adv_params));
                    previous_mode = mode;
                }
                ESP_ERROR_CHECK(esp_ble_gap_start_scanning(5));
                vTaskDelay(5100 / portTICK_PERIOD_MS);
                break;
            case ANTENNAMODE_RUN:
                if (previous_mode != mode) {
                    println("Switching to antenna mode (run)");
                    static esp_ble_scan_params_t scan_params = {
                            .own_addr_type          = BLE_ADDR_TYPE_RPA_PUBLIC,
                            .scan_duplicate         = BLE_SCAN_DUPLICATE_ENABLE,
                            .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL
                    };
                    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    previous_mode = mode;
                }
                ESP_ERROR_CHECK(esp_ble_gap_start_scanning(5));
                vTaskDelay(5100 / portTICK_PERIOD_MS);
                break;
            default:
                previous_mode = mode;
                vTaskDelay(500);
                break;
        }
    }
}

void send_data(char addr_str[18], int rssi) {
    char payload[32];

    int len = sprintf(payload, "%s,%d", addr_str, rssi);

    esp_mqtt_client_publish(mqttClient, topic_rssi, payload, len, 2, 0);
}

void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    if (event == ESP_GAP_BLE_SCAN_RESULT_EVT) {
        int rssi = param->scan_rst.rssi;
        char addr_str[18];
        sprintf(addr_str, "%02x:%02x:%02x:%02x:%02x:%02x", param->scan_rst.bda[0], param->scan_rst.bda[1],
                param->scan_rst.bda[2], param->scan_rst.bda[3], param->scan_rst.bda[4], param->scan_rst.bda[5]);
        printf("Found device: %s, rssi: %d\n", addr_str, rssi);
        send_data(addr_str, rssi);
    }
}


void init_ble() {
    printf("Initiating BLE\n");

    // Initalize all the blueetooth components
    esp_err_t ret;
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    config.mode = ESP_BT_MODE_BLE;

    ESP_ERROR_CHECK(esp_bt_controller_init(&config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    println("Controller initiated");

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    println("Bluedroid initiated");

    ESP_ERROR_CHECK(esp_ble_gap_config_local_privacy(true));

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_callback));
    println("Callback registered");

    ESP_ERROR_CHECK(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9));
    println("TX power set");

    const uint8_t *host_address = esp_bt_dev_get_address();
    sprintf(esp_address, "%02x:%02x:%02x:%02x:%02x:%02x",
            host_address[0], host_address[1], host_address[2],
            host_address[3], host_address[4], host_address[5]);
    sprintf(topic_rssi, "rssi/%s", esp_address);
    sprintf(topic_cc, "cc/%s", esp_address);
}