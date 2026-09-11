#include "radio.h"
#include "metrics.h"
#include "receiver.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static SemaphoreHandle_t tx_ready;
static StaticSemaphore_t tx_ready_storage;

static void tx_callback(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    xSemaphoreGive(tx_ready);
    receiver_post_tx(status == ESP_NOW_SEND_SUCCESS);
}

static void rx_callback(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    if (!info || !info->rx_ctrl || !data || length < 0 || length > HARNESS_MAX_PACKET_SIZE) {
        receiver_post_drop(false, true);
        return;
    }
    receiver_post_rx(data, (size_t)length, info->rx_ctrl->rssi, (uint64_t)esp_timer_get_time());
}

esp_err_t radio_init(void)
{
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    if ((error = esp_wifi_init(&wifi)) != ESP_OK) return error;
    if ((error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return error;
    if ((error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return error;
    if ((error = esp_wifi_start()) != ESP_OK) return error;
    if ((error = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) return error;
    if ((error = esp_now_init()) != ESP_OK) return error;
    tx_ready = xSemaphoreCreateBinaryStatic(&tx_ready_storage);
    xSemaphoreGive(tx_ready);
    if ((error = esp_now_register_send_cb(tx_callback)) != ESP_OK) return error;
    return esp_now_register_recv_cb(rx_callback);
}

static bool make_rate_config(const harness_config_t *config, esp_now_rate_config_t *rate)
{
    memset(rate, 0, sizeof(*rate));
    const char *name = config->phy_rate;
    if (strcmp(name, "1m") == 0 && !config->band_5ghz) {
        rate->phymode = WIFI_PHY_MODE_11B;
        rate->rate = WIFI_PHY_RATE_1M_L;
    } else if (strcmp(name, "6m") == 0 || strcmp(name, "24m") == 0 || strcmp(name, "54m") == 0) {
        rate->phymode = config->band_5ghz ? WIFI_PHY_MODE_11A : WIFI_PHY_MODE_11G;
        rate->rate = strcmp(name, "6m") == 0 ? WIFI_PHY_RATE_6M :
                     strcmp(name, "24m") == 0 ? WIFI_PHY_RATE_24M : WIFI_PHY_RATE_54M;
    } else if (strcmp(name, "mcs0") == 0 || strcmp(name, "mcs7") == 0) {
        rate->phymode = WIFI_PHY_MODE_HT20;
        rate->rate = strcmp(name, "mcs0") == 0 ? WIFI_PHY_RATE_MCS0_LGI : WIFI_PHY_RATE_MCS7_LGI;
    } else if (strcmp(name, "he-mcs0") == 0 || strcmp(name, "he-mcs7") == 0) {
        rate->phymode = WIFI_PHY_MODE_HE20;
        rate->rate = strcmp(name, "he-mcs0") == 0 ? WIFI_PHY_RATE_MCS0_LGI : WIFI_PHY_RATE_MCS7_LGI;
    } else {
        return false;
    }
    rate->ersu = false;
    rate->dcm = false;
    return true;
}

esp_err_t radio_configure(const harness_config_t *config)
{
    esp_now_peer_info_t existing;
    while (esp_now_fetch_peer(true, &existing) == ESP_OK) {
        esp_now_del_peer(existing.peer_addr);
    }
    esp_err_t error = esp_wifi_set_country_code(config->country, false);
    if (error != ESP_OK) return error;
    error = esp_wifi_set_band_mode(config->band_5ghz ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_2G_ONLY);
    if (error != ESP_OK) return error;
    error = esp_wifi_set_channel(config->channel, WIFI_SECOND_CHAN_NONE);
    if (error != ESP_OK) return error;
    error = esp_wifi_set_max_tx_power(config->tx_power_qdbm);
    if (error != ESP_OK) return error;
    error = esp_wifi_set_ps(config->power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (error != ESP_OK) return error;
    if (config->encryption && (error = esp_now_set_pmk(config->key)) != ESP_OK) return error;

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, config->peer_mac, ESP_NOW_ETH_ALEN);
    peer.channel = config->channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = config->encryption;
    if (config->encryption) memcpy(peer.lmk, config->key, ESP_NOW_KEY_LEN);
    if ((error = esp_now_add_peer(&peer)) != ESP_OK) return error;
    esp_now_rate_config_t rate;
    if (!make_rate_config(config, &rate)) return ESP_ERR_NOT_SUPPORTED;
    return esp_now_set_peer_rate_config(config->peer_mac, &rate);
}

esp_err_t radio_send(const uint8_t *data, size_t length, uint32_t timeout_ms)
{
    if (xSemaphoreTake(tx_ready, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = esp_now_send(g_config.peer_mac, data, length);
    if (error != ESP_OK) xSemaphoreGive(tx_ready);
    return error;
}
