#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "cJSON.h"

static const char *TAG = "ESP32_CONTROLLER";

// WiFi AP Configuration
#define WIFI_SSID "ESP32_Controller"
#define WIFI_PASS "12345678"
#define WIFI_CHANNEL 1
#define MAX_STA_CONN 4

// MQTT Configuration
#define MQTT_BROKER_URI "mqtt://192.168.4.2:1884"
#define MQTT_CLIENT_ID "ESP32_Controller"
#define MQTT_CONNECT_TIMEOUT_MS 60000  // 60 seconds
#define MQTT_RETRY_INTERVAL_MS 10000   // 10 seconds between retries

// MQTT Topics
#define TOPIC_COORDINATES "device/coordinates"
#define TOPIC_START_COMMAND "device/start"
#define TOPIC_STATUS_COMMANDS "device/status/commands"
#define TOPIC_RESULT_DATA "device/status/result"
#define TOPIC_LAST_SAVED "device/status/lastsaved"

// Global variables
static esp_mqtt_client_handle_t mqtt_client;
static bool system_running = false;
static float current_x = 0.0;
static float current_y = 0.0;
static char last_result[256] = "";
static char last_saved_unit[256] = "";
static bool mqtt_client_started = false;

// Task handles
static TaskHandle_t coordinate_task_handle = NULL;
static TaskHandle_t mqtt_publish_task_handle = NULL;
static TaskHandle_t mqtt_connection_task_handle = NULL;

// Event group for synchronization
static EventGroupHandle_t wifi_event_group;
static EventGroupHandle_t mqtt_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define MQTT_CONNECTED_BIT BIT0

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" left, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    
    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started. SSID:%s password:%s channel:%d",
             WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
    
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_get_ip_info(netif, &ip_info);
    ESP_LOGI(TAG, "AP IP Address: " IPSTR, IP2STR(&ip_info.ip));
}

static void handle_status_command(const char* command)
{
    if (strcmp(command, "start") == 0) {
        system_running = true;
        ESP_LOGI(TAG, "System STARTED");
    } else if (strcmp(command, "stop") == 0) {
        system_running = false;
        ESP_LOGI(TAG, "System STOPPED");
    } else {
        ESP_LOGI(TAG, "Unknown command: %s", command);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected successfully!");
        xEventGroupSetBits(mqtt_event_group, MQTT_CONNECTED_BIT);
        
        // Subscribe to topics
        esp_mqtt_client_subscribe(client, TOPIC_STATUS_COMMANDS, 1);
        esp_mqtt_client_subscribe(client, TOPIC_RESULT_DATA, 1);
        esp_mqtt_client_subscribe(client, TOPIC_LAST_SAVED, 1);
        
        ESP_LOGI(TAG, "Subscribed to all topics");
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected, will retry...");
        xEventGroupClearBits(mqtt_event_group, MQTT_CONNECTED_BIT);
        break;
        
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT data received");
        ESP_LOGI(TAG, "Topic: %.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "Data: %.*s", event->data_len, event->data);
        
        // Handle different topics
        if (strncmp(event->topic, TOPIC_STATUS_COMMANDS, event->topic_len) == 0) {
            char command[64];
            int len = (event->data_len < 63) ? event->data_len : 63;
            strncpy(command, event->data, len);
            command[len] = '\0';
            handle_status_command(command);
        }
        else if (strncmp(event->topic, TOPIC_RESULT_DATA, event->topic_len) == 0) {
            int len = (event->data_len < 255) ? event->data_len : 255;
            strncpy(last_result, event->data, len);
            last_result[len] = '\0';
            ESP_LOGI(TAG, "Result updated: %s", last_result);
        }
        else if (strncmp(event->topic, TOPIC_LAST_SAVED, event->topic_len) == 0) {
            int len = (event->data_len < 255) ? event->data_len : 255;
            strncpy(last_saved_unit, event->data, len);
            last_saved_unit[len] = '\0';
            ESP_LOGI(TAG, "Last saved unit updated: %s", last_saved_unit);
        }
        break;
        
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT connection error, retrying...");
        break;
        
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "MQTT attempting to connect...");
        break;
        
    default:
        ESP_LOGD(TAG, "MQTT event: %d", event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = MQTT_BROKER_URI,
        .client_id = MQTT_CLIENT_ID,
        .keepalive = 60,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms = MQTT_RETRY_INTERVAL_MS,
        .network_timeout_ms = MQTT_CONNECT_TIMEOUT_MS,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }
    
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_LOGI(TAG, "MQTT client initialized, will start when ready");
    mqtt_client_started = false;  // Don't start immediately
}

static void mqtt_connection_task(void *pvParameters)
{
    ESP_LOGI(TAG, "MQTT connection task started, waiting for stations to connect...");
    
    // Wait a bit for stations to connect to the AP
    vTaskDelay(pdMS_TO_TICKS(15000)); // Wait 15 seconds
    
    ESP_LOGI(TAG, "Starting MQTT client...");
    esp_mqtt_client_start(mqtt_client);
    mqtt_client_started = true;
    
    // Monitor connection and provide status updates
    while (1) {
        if (xEventGroupGetBits(mqtt_event_group) & MQTT_CONNECTED_BIT) {
            ESP_LOGI(TAG, "MQTT is connected and operational");
        } else {
            ESP_LOGI(TAG, "MQTT not connected, waiting...");
        }
        
        vTaskDelay(pdMS_TO_TICKS(30000)); // Check every 30 seconds
    }
}

static void publish_coordinates(float x, float y)
{
    if (!(xEventGroupGetBits(mqtt_event_group) & MQTT_CONNECTED_BIT)) {
        return;
    }
    
    cJSON *json = cJSON_CreateObject();
    cJSON *x_val = cJSON_CreateNumber(x);
    cJSON *y_val = cJSON_CreateNumber(y);
    cJSON *timestamp = cJSON_CreateNumber(esp_timer_get_time() / 1000); // milliseconds
    
    cJSON_AddItemToObject(json, "x", x_val);
    cJSON_AddItemToObject(json, "y", y_val);
    cJSON_AddItemToObject(json, "timestamp", timestamp);
    
    char *json_string = cJSON_Print(json);
    
    esp_mqtt_client_publish(mqtt_client, TOPIC_COORDINATES, json_string, 0, 1, 0);
    ESP_LOGI(TAG, "Published coordinates: %s", json_string);
    
    free(json_string);
    cJSON_Delete(json);
}

static void publish_start_command(void)
{
    if (!(xEventGroupGetBits(mqtt_event_group) & MQTT_CONNECTED_BIT)) {
        return;
    }
    
    cJSON *json = cJSON_CreateObject();
    cJSON *command = cJSON_CreateString("start");
    cJSON *timestamp = cJSON_CreateNumber(esp_timer_get_time() / 1000);
    
    cJSON_AddItemToObject(json, "command", command);
    cJSON_AddItemToObject(json, "timestamp", timestamp);
    
    char *json_string = cJSON_Print(json);
    
    esp_mqtt_client_publish(mqtt_client, TOPIC_START_COMMAND, json_string, 0, 1, 0);
    ESP_LOGI(TAG, "Published start command: %s", json_string);
    
    free(json_string);
    cJSON_Delete(json);
}

static void coordinate_update_task(void *pvParameters)
{
    while (1) {
        // Simulate coordinate changes (replace with actual sensor readings)
        current_x += (esp_random() % 21 - 10) / 10.0; // Random -1.0 to 1.0
        current_y += (esp_random() % 21 - 10) / 10.0;
        
        // Keep coordinates in reasonable range
        if (current_x > 100.0) current_x = 100.0;
        if (current_x < -100.0) current_x = -100.0;
        if (current_y > 100.0) current_y = 100.0;
        if (current_y < -100.0) current_y = -100.0;
        
        vTaskDelay(pdMS_TO_TICKS(2000)); // Update every 2 seconds
    }
}

static void mqtt_publish_task(void *pvParameters)
{
    while (1) {
        if (system_running && (xEventGroupGetBits(mqtt_event_group) & MQTT_CONNECTED_BIT)) {
            publish_coordinates(current_x, current_y);
        }
        vTaskDelay(pdMS_TO_TICKS(3000)); // Publish every 3 seconds
    }
}

static void print_system_info(void)
{
    ESP_LOGI(TAG, "=== System Information ===");
    ESP_LOGI(TAG, "WiFi AP: %s", WIFI_SSID);
    ESP_LOGI(TAG, "MQTT Broker: %s", MQTT_BROKER_URI);
    ESP_LOGI(TAG, "Publish Topics:");
    ESP_LOGI(TAG, "- Coordinates: %s", TOPIC_COORDINATES);
    ESP_LOGI(TAG, "- Start Command: %s", TOPIC_START_COMMAND);
    ESP_LOGI(TAG, "Subscribe Topics:");
    ESP_LOGI(TAG, "- Status Commands: %s", TOPIC_STATUS_COMMANDS);
    ESP_LOGI(TAG, "- Result Data: %s", TOPIC_RESULT_DATA);
    ESP_LOGI(TAG, "- Last Saved: %s", TOPIC_LAST_SAVED);
    ESP_LOGI(TAG, "============================");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32 WiFi AP + MQTT Controller");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Create event groups
    wifi_event_group = xEventGroupCreate();
    mqtt_event_group = xEventGroupCreate();
    
    // Initialize WiFi AP
    wifi_init_softap();
    
    // Start MQTT (but don't connect immediately)
    mqtt_app_start();
    
    // Create tasks
    xTaskCreate(coordinate_update_task, "coord_update", 4096, NULL, 5, &coordinate_task_handle);
    xTaskCreate(mqtt_publish_task, "mqtt_publish", 4096, NULL, 5, &mqtt_publish_task_handle);
    xTaskCreate(mqtt_connection_task, "mqtt_connect", 4096, NULL, 5, &mqtt_connection_task_handle);
    
    print_system_info();
    
    ESP_LOGI(TAG, "System ready! Waiting for clients to connect to WiFi AP...");
    ESP_LOGI(TAG, "MQTT will start connecting after 15 seconds delay");
    
    // Main loop for handling console commands
    while (1) {
        // You can add console input handling here if needed
        // For now, just a simple demonstration
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        // Example: publish start command every 30 seconds for testing
        static int counter = 0;
        if (++counter >= 6) {  // 6 * 5 seconds = 30 seconds
            publish_start_command();
            counter = 0;
        }
    }
}