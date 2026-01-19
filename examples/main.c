#include <esp_log.h>

#include <stdio.h>

#include "caldav_client.h"

static const char *TAG = "CalDAV-Examples";

static void CalDAV_Example_Test_Connection(void)
{
    ESP_LOGI(TAG, "Running CalDAV Connection test...");

    CalDAV_Config_t Config = {
        .ServerURL = "...",
        .Username = "...",
        .Password = "...",
        .CalendarPath = "",
        .TimeoutMs = 5000,
        .UseHTTPS = true,
    };

    CalDAV_Client_t *Client = CalDAV_Client_Init(&Config);
    if (Client == NULL) {
        ESP_LOGE(TAG, "CalDAV client initialization failed!");

        return;
    }

    CalDAV_Error_t err = CalDAV_Test_Connection(Client);
    if (err == CALDAV_ERROR_OK) {
        ESP_LOGI(TAG, "Connection successful!");
    } else {
        ESP_LOGE(TAG, "Connection failed (Error: %d)", err);
    }

    CalDAV_Client_Deinit(Client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Opening WiFi-Verbindung...");

    /* Implement this functions */
    wifi_connect_init();
    while (wifi_is_connected() == false) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "WiFi connected!");

    CalDAV_Example_Test_Connection();

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
