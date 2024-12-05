#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "../include/secrets.h"
#define WIFI_CONNECTED_BIT BIT0

#define VOLTAGE_SENSOR_ADC_CHANNEL ADC_CHANNEL_6 // GPIO34
#define CURRENT_SENSOR_ADC_CHANNEL ADC_CHANNEL_7 // GPIO35

static const char* ESP_TAG = "monitor-esp32:MAIN";
static const char* WIFI_TAG = "monitor-esp32:WIFI";
static const char* ADC_TAG = "monitor-esp32:ADC";
static const char* HTTP_TAG = "monitor-esp32:HTTP";

static char mac_str[18];
static EventGroupHandle_t s_wifi_event_group;

static adc_oneshot_unit_handle_t adc_handle;
static uint32_t mv = 0, ma = 0;

static void get_mac_address(uint8_t* mac, char* mac_str);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void wifi_init_sta(void);
static void setup_adc(void);
static void read_adc_values(void);
static esp_err_t http_event_handler(esp_http_client_event_t* evt);
static void send_post_request(void);
static void post_task(void* pvParameters);

void app_main(void) {
	//=
	// * MAC address
	//=
	uint8_t mac[6] = { 0 };
	get_mac_address(mac, mac_str);

	//=
	// * Wi-Fi
	//=
	ESP_ERROR_CHECK(nvs_flash_init());

    wifi_init_sta();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(WIFI_TAG, "Connected to Wi-Fi!");
    } else {
        ESP_LOGI(WIFI_TAG, "Failed to connect to Wi-Fi.");
    }

	//=
	// * ADCs
	//=
	setup_adc();

	//=
	// * HTML POST
	//=
	xTaskCreate(post_task, "post_task", 4096, NULL, 5, NULL);
}

static void get_mac_address(uint8_t* mac, char* mac_str) {
	if (esp_efuse_mac_get_default(mac) == ESP_OK) {
		ESP_LOGI(
			ESP_TAG,
			"Default EFUSE MAC address: %02X:%02X:%02X:%02X:%02X:%02X",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
		);
	} else {
		// TODO: decide what to do on failing to read the MAC address
		ESP_LOGW(ESP_TAG, "Failed to read device tag");
	}
	snprintf(mac_str, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(WIFI_TAG, "Disconnected from Wi-Fi, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(WIFI_TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(WIFI_TAG, "Wi-Fi initialization completed.");
}

static void setup_adc(void) {
	adc_oneshot_unit_init_cfg_t adc_init_config = {
		.unit_id = ADC_UNIT_1,
		.ulp_mode = ADC_ULP_MODE_DISABLE,
	};
	ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_init_config, &adc_handle));

	adc_oneshot_chan_cfg_t adc_channel_config = {
		.bitwidth = ADC_BITWIDTH_DEFAULT,
		.atten = ADC_ATTEN_DB_12
	};
	ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, VOLTAGE_SENSOR_ADC_CHANNEL, &adc_channel_config));
	ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, CURRENT_SENSOR_ADC_CHANNEL, &adc_channel_config));
}

static bool calibrate_adc(void) {
    adc_cali_line_fitting_config_t  cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(ADC_TAG, "ADC calibration initialized successfully.");
        return true;
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(ADC_TAG, "Curve-fitting calibration scheme not supported. ADC readings might be less accurate.");
    } else {
        ESP_LOGI(ADC_TAG, "Failed to initialize ADC calibration.\n");
    }
    return false;
}

static void read_adc_values(void) {
    int raw_voltage = 0;
    int raw_current = 0;

    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, VOLTAGE_SENSOR_ADC_CHANNEL, &raw_voltage));
    ESP_LOGI(ADC_TAG, "Raw Voltage ADC Value: %d", raw_voltage);

    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, CURRENT_SENSOR_ADC_CHANNEL, &raw_current));
    ESP_LOGI(ADC_TAG, "Raw Current ADC Value: %d", raw_current);

    adc_cali_raw_to_voltage(adc_cali_handle, raw_voltage,&mv);

	static const uint16_t current_sensor_zero_offset = 112; // Temporary until we measure the actual offset
	static const double k = 0.04028320312; // (Vref / 4096) / ((Vsupply / 3.3) * (20 / 1000))
    ma = (int)((raw_current - current_sensor_zero_offset) * k * 1000);

    ESP_LOGI(ADC_TAG, "Voltage: %zu mV, Current: %zu mA", mv, ma);
}

static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(HTTP_TAG, "Event: HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(HTTP_TAG, "Event: HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(HTTP_TAG, "Event: HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(HTTP_TAG, "Event: HTTP_EVENT_ON_HEADER, key=%s, value=%s",
                     evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(HTTP_TAG, "Event: HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(HTTP_TAG, "Event: HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(HTTP_TAG, "Event: HTTP_EVENT_DISCONNECTED");
            break;
		default:
			ESP_LOGW(HTTP_TAG, "Unhandled HTTP event");
			return ESP_FAIL;
    }
    return ESP_OK;
}

static void send_post_request(void) {
    esp_http_client_config_t config = {
        .url = API_POST_ENDPOINT,
		.event_handler = http_event_handler
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char post_data[128];
    snprintf(post_data, sizeof(post_data), "{\"mac\":\"%s\",\"mv\":%d,\"ma\":%d}", mac_str, mv, ma);
    ESP_LOGI(HTTP_TAG, "POST: JSON data: %s", post_data);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(HTTP_TAG, "POST: POST successful, Status = %d",
                 esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(HTTP_TAG, "POST: POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

static void post_task(void* pvParameters) {
    while (1) {
		ESP_LOGI(ADC_TAG, "Reading ADC values...");
		read_adc_values();
        ESP_LOGI(HTTP_TAG, "Sending POST request...");
        send_post_request();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
