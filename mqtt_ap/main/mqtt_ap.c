#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "driver/gpio.h"

static const char *TAG = "MQTT_AP";

/* Wi-Fi SoftAP parameters */
#define AP_SSID       "MyESP32_AP"
#define AP_PASS       "password123"
#define AP_MAX_CONN   4

/* MQTT topics */
#define TOPIC_COORDS  "/device/coordinates"
#define TOPIC_CONFIG  "/device/config"
#define TOPIC_STATUS  "/device/status"

/* Commands */
#define CMD_START     0x01
#define CMD_STOP      0x02
#define CMD_RESULT    0x03  // + int16_t
#define CMD_LASTUNIT  0x04  // + int16_t

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool     running         = false;
static int16_t  last_result     = 0;
static int16_t  last_saved_unit = 0;

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid          = AP_SSID,
            .ssid_len      = strlen(AP_SSID),
            .password      = AP_PASS,
            .max_connection= AP_MAX_CONN,
            .authmode      = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(AP_PASS) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Soft-AP up: SSID=%s PASS=%s", AP_SSID, AP_PASS);
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;  // if you ever need it

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        esp_mqtt_client_subscribe(client, TOPIC_CONFIG, 0);
        esp_mqtt_client_publish(client,
                                TOPIC_STATUS,
                                "OK", 2, 1, 0);
        break;

    case MQTT_EVENT_DATA:
        // sanity checks
        if (event->data == NULL || event->data_len == 0) {
            ESP_LOGW(TAG, "MQTT_EVENT_DATA with empty payload");
            break;
        }
        if (event->topic == NULL || event->topic_len == 0) {
            ESP_LOGW(TAG, "MQTT_EVENT_DATA with empty topic");
            break;
        }

        // only handle config topic
        if (strncmp(event->topic, TOPIC_CONFIG, event->topic_len) != 0) {
            ESP_LOGI(TAG, "Ignoring topic %.*s", event->topic_len, event->topic);
            break;
        }

        // safe to deref now
        {
            uint8_t *p   = (uint8_t*)event->data;
            int      len = event->data_len;
            uint8_t  cmd = p[0];

            switch (cmd) {
            case CMD_START:
                running = true;
                ESP_LOGI(TAG, "→ CMD_START");
                break;

            case CMD_STOP:
                running = false;
                ESP_LOGI(TAG, "→ CMD_STOP");
                break;

            case CMD_RESULT:
                if (len >= 3) {
                    last_result = (int16_t)(p[1] | (p[2] << 8));
                    ESP_LOGI(TAG, "→ CMD_RESULT: %d", last_result);
                } else {
                    ESP_LOGW(TAG, "CMD_RESULT payload too short (%d bytes)", len);
                }
                break;

            case CMD_LASTUNIT:
                if (len >= 3) {
                    last_saved_unit = (int16_t)(p[1] | (p[2] << 8));
                    ESP_LOGI(TAG, "→ CMD_LASTUNIT: %d", last_saved_unit);
                } else {
                    ESP_LOGW(TAG, "CMD_LASTUNIT payload too short (%d bytes)", len);
                }
                break;

            default:
                ESP_LOGW(TAG, "→ Unknown cmd: 0x%02X", cmd);
            }
        }
        break;

    default:
        // ignore all other events
        break;
    }

    return ESP_OK;
}

static void coord_publisher_task(void *pv)
{
    int16_t x = 0, y = 0;
    uint8_t buf[4];
    while (1) {
        if (running) {
            x += 1; y += 2;
            buf[0] =  x & 0xFF;
            buf[1] = (x >> 8) & 0xFF;
            buf[2] =  y & 0xFF;
            buf[3] = (y >> 8) & 0xFF;
            esp_mqtt_client_publish(mqtt_client,
                                    TOPIC_COORDS,
                                    (char*)buf, 4, 1, 0);
            ESP_LOGI(TAG, "Published (%d,%d)", x, y);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t cfg = { .uri = "mqtt://192.168.4.1" };
    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt_client,
        ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    vTaskDelay(pdMS_TO_TICKS(2000));
    xTaskCreate(coord_publisher_task,
                "coord_pub", 4096, NULL, 5, NULL);
}

void app_main(void)
{
    wifi_init_softap();
    //mqtt_app_start();
    gpio_reset_pin(GPIO_NUM_2);
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    while (1) {
        gpio_set_level(GPIO_NUM_2, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(GPIO_NUM_2, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
