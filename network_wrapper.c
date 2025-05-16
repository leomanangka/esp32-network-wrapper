#include "network_wrapper.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include <string.h>

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_H2E_IDENTIFIER ""
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define ESP_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* FreeRTOS event group signal when connected  */
static EventGroupHandle_t s_wifi_event_group;

/* event group allow multiple bits for each event. for this only need two
 * - connected to the AP with an IP
 * - fail to connect after maximum amount of retries
 * */
#define WIFI_CONNECTED_SUCCESS_BIT BIT0
#define WIFI_CONNECTED_FAIL_BIT BIT1

static const char *WIFI_STA_MODE_TAG = "WiFi STA Mode";
static const char *WIFI_SOFTAP_MODE_TAG = "WiFi SoftAP Mode";
static int s_retry_num = 0;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(WIFI_STA_MODE_TAG, "retry to connect to the AP");
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_FAIL_BIT);
    }
    ESP_LOGI(WIFI_STA_MODE_TAG, "connect to the AP fail");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(WIFI_STA_MODE_TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_SUCCESS_BIT);
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(WIFI_STA_MODE_TAG, "Station " MACSTR " joined, AID=%d",
             MAC2STR(event->mac), event->aid);
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(WIFI_STA_MODE_TAG, "Station " MACSTR " left, AID=%d, reason:%d",
             MAC2STR(event->mac), event->aid, event->reason);
  }
}

static void wifi_init_sta(void) {
  esp_netif_create_default_wifi_sta();

  wifi_config_t wifi_sta_config = {
      .sta = {
          .ssid = CONFIG_ESP_WIFI_STA_SSID,
          .password = CONFIG_ESP_WIFI_STA_PASSWORD,
          /* Authmode threshold resets to WPA2 as default if password matches
           * WPA2 standards (password len => 8). If you want to connect the
           * device to deprecated WEP/WPA networks, Please set the threshold
           * value to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with
           * length and format matching to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK
           * standards.
           */
          .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
          .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
          .sae_h2e_identifier = ESP_H2E_IDENTIFIER,
      }};

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
  ESP_LOGI(WIFI_STA_MODE_TAG, "Init STA mode finished.");
}

static void wifi_init_softap(void) {
  esp_netif_create_default_wifi_ap();

  wifi_config_t wifi_ap_config = {
      .ap = {.ssid = CONFIG_ESP_WIFI_AP_SSID,
             .ssid_len = strlen(CONFIG_ESP_WIFI_AP_SSID),
             .password = CONFIG_ESP_WIFI_AP_PASSWORD,
             .channel = CONFIG_ESP_WIFI_AP_CHANNEL,
             .max_connection = CONFIG_ESP_MAX_STA_CONN_AP,
             .authmode = WIFI_AUTH_WPA2_PSK,
             .pmf_cfg = {.required = false}}};

  if (strlen(CONFIG_ESP_WIFI_AP_PASSWORD) == 0) {
    wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
  ESP_LOGI(WIFI_SOFTAP_MODE_TAG,
           "Init SoftAP mode finished. SSID: %s password: %s channel: %d",
           CONFIG_ESP_WIFI_AP_SSID, CONFIG_ESP_WIFI_AP_PASSWORD,
           CONFIG_ESP_WIFI_AP_CHANNEL);
}

void wifi_start(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  /* Initialize NVS */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  /* Initialize event group */
  s_wifi_event_group = xEventGroupCreate();

  /* Register event handler */
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

  /* Initialize WiFi */
  wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

#if CONFIG_ESP_WIFI_MODE_STA
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  wifi_init_sta();
#elif CONFIG_ESP_WIFI_MODE_SOFTAP
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  wifi_init_softap();
#endif

  /* Start WiFi */
  ESP_ERROR_CHECK(esp_wifi_start());

  /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or
   * connection failed for the maximum number of re-tries (WIFI_FAIL_BIT). The
   * bits are set by event_handler() (see above) */
  EventBits_t event_bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_SUCCESS_BIT | WIFI_CONNECTED_FAIL_BIT,
      pdFALSE, pdFALSE, portMAX_DELAY);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we
   * can test which event actually happened. */
  if (event_bits & WIFI_CONNECTED_SUCCESS_BIT) {
    ESP_LOGI(WIFI_STA_MODE_TAG, "Connected to AP SSID: %s",
             CONFIG_ESP_WIFI_STA_SSID);
  } else if (event_bits & WIFI_CONNECTED_FAIL_BIT) {
    ESP_LOGI(WIFI_STA_MODE_TAG, "Failed to connect to SSID: %s",
             CONFIG_ESP_WIFI_STA_SSID);
  } else {
    ESP_LOGE(WIFI_STA_MODE_TAG, "UNEXPECTED EVENT");
  }
}

bool is_wifi_connected(void) {
  wifi_ap_record_t ap_info;
  esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
  if (err == ESP_OK) {
    ESP_LOGI(WIFI_STA_MODE_TAG, "WiFi is connected to AP SSID: %s",
             ap_info.ssid);
    return true;
  } else {
    ESP_LOGE(WIFI_STA_MODE_TAG, "WiFi is not connected to any AP");
    return false;
  }
}
