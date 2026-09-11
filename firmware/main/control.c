#include "control.h"
#include "generator.h"
#include "harness.h"
#include "metrics.h"
#include "protocol.h"
#include "radio.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

harness_config_t g_config;
volatile harness_state_t g_state = STATE_BOOT;

const char *harness_state_name(harness_state_t state)
{
    static const char *names[] = {"BOOT", "IDLE", "CONFIGURED", "ARMED", "RUNNING", "FINISHED", "ERROR"};
    return state <= STATE_ERROR ? names[state] : "ERROR";
}

static cJSON *response(double id, bool ok)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", ok);
    return root;
}

static void send_json(cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    if (text) {
        fputs(text, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

static void send_error(double id, const char *code, const char *message)
{
    cJSON *root = response(id, false);
    cJSON_AddStringToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message);
    send_json(root);
}

static cJSON *object_item(cJSON *parent, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsObject(item) ? item : NULL;
}

static const char *string_item(cJSON *parent, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static double number_item(cJSON *parent, const char *name, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool bool_item(cJSON *parent, const char *name, bool fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static bool integer_in_range(double value, double min, double max)
{
    return isfinite(value) && value >= min && value <= max && floor(value) == value;
}

static bool parse_mac(const char *text, uint8_t mac[6])
{
    unsigned value[6];
    if (!text || sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &value[0], &value[1], &value[2],
                        &value[3], &value[4], &value[5]) != 6) {
        return false;
    }
    for (size_t i = 0; i < 6; ++i) {
        mac[i] = (uint8_t)value[i];
    }
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool parse_key(const char *text, uint8_t key[HARNESS_KEY_SIZE])
{
    if (!text || strlen(text) != HARNESS_KEY_SIZE * 2) {
        return false;
    }
    for (size_t i = 0; i < HARNESS_KEY_SIZE; ++i) {
        int high = hex_value(text[i * 2]);
        int low = hex_value(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        key[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool channel_valid(bool band_5ghz, uint8_t channel)
{
    if (!band_5ghz) {
        return channel >= 1 && channel <= 14;
    }
    switch (channel) {
    case 36: case 40: case 44: case 48: case 149: case 153: case 157: case 161: case 165:
        return true;
    default:
        return false;
    }
}

static bool phy_valid(const char *phy, bool band_5ghz)
{
    if (!phy || (band_5ghz && strcmp(phy, "1m") == 0)) {
        return false;
    }
    static const char *valid[] = {"1m", "6m", "24m", "54m", "mcs0", "mcs7", "he-mcs0", "he-mcs7"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        if (strcmp(phy, valid[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool parse_config(cJSON *root, char *why, size_t why_capacity)
{
    cJSON *radio = object_item(root, "radio");
    cJSON *traffic = object_item(root, "traffic");
    const char *role = string_item(root, "role");
    const char *peer = string_item(root, "peer_mac");
    double run_id = number_item(root, "run_id", -1);
    double duration = number_item(root, "duration_us", -1);
    if (!radio || !traffic || !role || !peer || !integer_in_range(run_id, 1, UINT32_MAX) ||
        !integer_in_range(duration, 1, UINT64_C(86400000000))) {
        snprintf(why, why_capacity, "invalid run_id, duration_us, role, peer_mac, radio, or traffic");
        return false;
    }

    harness_config_t cfg = {0};
    cfg.run_id = (uint32_t)run_id;
    cfg.duration_us = (uint64_t)duration;
    if (!parse_mac(peer, cfg.peer_mac)) {
        snprintf(why, why_capacity, "peer_mac must be six hexadecimal octets");
        return false;
    }
    if (strcmp(role, "generator") == 0) cfg.role = ROLE_GENERATOR;
    else if (strcmp(role, "sink") == 0) cfg.role = ROLE_SINK;
    else if (strcmp(role, "ping_initiator") == 0) cfg.role = ROLE_PING_INITIATOR;
    else if (strcmp(role, "echo_responder") == 0) cfg.role = ROLE_ECHO_RESPONDER;
    else {
        snprintf(why, why_capacity, "unsupported role");
        return false;
    }

    const char *band = string_item(radio, "band");
    const char *country = string_item(radio, "country");
    const char *phy = string_item(radio, "phy_rate");
    double channel = number_item(radio, "channel", -1);
    double tx_power = number_item(radio, "tx_power_dbm", -1);
    if (!band || (strcmp(band, "2.4ghz") != 0 && strcmp(band, "5ghz") != 0) ||
        !country || strlen(country) != 2 || !isupper((unsigned char)country[0]) ||
        !isupper((unsigned char)country[1]) || !integer_in_range(channel, 1, 255) ||
        !isfinite(tx_power) || tx_power < 2 || tx_power > 20 || floor(tx_power * 4) != tx_power * 4) {
        snprintf(why, why_capacity, "invalid band, country, channel, or tx_power_dbm");
        return false;
    }
    cfg.band_5ghz = strcmp(band, "5ghz") == 0;
    cfg.channel = (uint8_t)channel;
    if (!channel_valid(cfg.band_5ghz, cfg.channel) || !phy_valid(phy, cfg.band_5ghz)) {
        snprintf(why, why_capacity, "unsupported channel or PHY rate for selected band");
        return false;
    }
    memcpy(cfg.country, country, 2);
    cfg.country[2] = '\0';
    snprintf(cfg.phy_rate, sizeof(cfg.phy_rate), "%s", phy);
    cfg.tx_power_qdbm = (int8_t)(tx_power * 4);
    cfg.power_save = bool_item(radio, "power_save", false);
    cfg.encryption = bool_item(radio, "encryption", false);
    if (cfg.encryption && !parse_key(string_item(radio, "key"), cfg.key)) {
        snprintf(why, why_capacity, "key must be 32 hexadecimal digits");
        return false;
    }

    const char *mode = string_item(traffic, "mode");
    if (!mode) {
        snprintf(why, why_capacity, "missing traffic.mode");
        return false;
    }
    if (strcmp(mode, "ping") == 0) cfg.mode = MODE_PING;
    else if (strcmp(mode, "stream") == 0) cfg.mode = MODE_STREAM;
    else if (strcmp(mode, "multi-stream") == 0) cfg.mode = MODE_MULTI_STREAM;
    else if (strcmp(mode, "saturation") == 0) cfg.mode = MODE_SATURATION;
    else if (strcmp(mode, "latency-under-load") == 0) cfg.mode = MODE_LATENCY_LOAD;
    else {
        snprintf(why, why_capacity, "unsupported mode");
        return false;
    }

    double packet_size = number_item(traffic, "packet_size", -1);
    double streams = number_item(traffic, "streams", 1);
    double rate = number_item(traffic, "packet_rate_per_stream", 0);
    double probe_rate = number_item(traffic, "probe_rate", 0);
    double background_size = number_item(traffic, "background_packet_size", 0);
    double background_rate = number_item(traffic, "background_rate", 0);
    if (!integer_in_range(packet_size, BENCH_HEADER_SIZE, HARNESS_MAX_PACKET_SIZE) ||
        !integer_in_range(streams, 1, HARNESS_MAX_STREAMS) || !integer_in_range(rate, 0, UINT32_MAX) ||
        !integer_in_range(probe_rate, 0, UINT32_MAX) ||
        !integer_in_range(background_size, 0, HARNESS_MAX_PACKET_SIZE) ||
        !integer_in_range(background_rate, 0, UINT32_MAX)) {
        snprintf(why, why_capacity, "invalid traffic numeric field");
        return false;
    }
    cfg.packet_size = (uint16_t)packet_size;
    cfg.streams = (uint16_t)streams;
    cfg.rate_per_stream = (uint32_t)rate;
    cfg.probe_rate = (uint32_t)probe_rate;
    cfg.background_packet_size = (uint16_t)background_size;
    cfg.background_rate = (uint32_t)background_rate;
    if (cfg.mode != MODE_SATURATION && cfg.mode != MODE_LATENCY_LOAD && cfg.rate_per_stream == 0) {
        snprintf(why, why_capacity, "packet_rate_per_stream must be positive");
        return false;
    }
    if (cfg.mode == MODE_LATENCY_LOAD &&
        (cfg.probe_rate == 0 || cfg.background_rate == 0 || cfg.background_packet_size < BENCH_HEADER_SIZE)) {
        snprintf(why, why_capacity, "invalid latency-under-load rates or background size");
        return false;
    }
    g_config = cfg;
    return true;
}

static uint32_t boot_id(void)
{
    static uint32_t id;
    if (id == 0) id = esp_random();
    return id;
}

static void send_info(double id)
{
    uint8_t mac_bytes[6];
    esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac_bytes[0], mac_bytes[1],
             mac_bytes[2], mac_bytes[3], mac_bytes[4], mac_bytes[5]);
    cJSON *root = response(id, true);
    cJSON *info = cJSON_AddObjectToObject(root, "info");
    cJSON_AddNumberToObject(info, "protocol_version", HARNESS_CONTROL_VERSION);
    cJSON_AddStringToObject(info, "mac", mac);
    cJSON_AddStringToObject(info, "chip", "ESP32-C5");
    cJSON_AddStringToObject(info, "firmware_version", HARNESS_FW_VERSION);
    cJSON_AddStringToObject(info, "idf_version", esp_get_idf_version());
    cJSON_AddNumberToObject(info, "boot_id", (double)boot_id());
    cJSON *cap = cJSON_AddObjectToObject(info, "capabilities");
    cJSON_AddNumberToObject(cap, "max_packet_size", HARNESS_MAX_PACKET_SIZE);
    cJSON_AddNumberToObject(cap, "max_streams", HARNESS_MAX_STREAMS);
    cJSON *bands = cJSON_AddArrayToObject(cap, "bands");
    cJSON_AddItemToArray(bands, cJSON_CreateString("2.4ghz"));
    cJSON_AddItemToArray(bands, cJSON_CreateString("5ghz"));
    cJSON *rates = cJSON_AddArrayToObject(cap, "phy_rates");
    static const char *rate_names[] = {"1m", "6m", "24m", "54m", "mcs0", "mcs7", "he-mcs0", "he-mcs7"};
    for (size_t i = 0; i < sizeof(rate_names) / sizeof(rate_names[0]); ++i) {
        cJSON_AddItemToArray(rates, cJSON_CreateString(rate_names[i]));
    }
    cJSON_AddBoolToObject(cap, "encryption", true);
    send_json(root);
}

static void send_state(double id)
{
    cJSON *root = response(id, true);
    cJSON_AddStringToObject(root, "state", harness_state_name(g_state));
    send_json(root);
}

static void dispatch(cJSON *root)
{
    double id = number_item(root, "id", 0);
    double version = number_item(root, "version", 0);
    const char *command = string_item(root, "cmd");
    if (!command || !integer_in_range(id, 1, 9007199254740991.0)) {
        send_error(id, "bad_request", "cmd and a positive integer id are required");
        return;
    }
    if (version != HARNESS_CONTROL_VERSION) {
        send_error(id, "incompatible_version", "control protocol version mismatch");
        return;
    }
    if (strcmp(command, "info") == 0) {
        send_info(id);
        return;
    }
    if (strcmp(command, "status") == 0) {
        send_state(id);
        return;
    }
    if (strcmp(command, "configure") == 0) {
        if (g_state == STATE_RUNNING || g_state == STATE_ARMED) {
            send_error(id, "invalid_state", "configure requires IDLE, CONFIGURED, FINISHED, or ERROR");
            return;
        }
        char why[112];
        if (!parse_config(root, why, sizeof(why))) {
            send_error(id, "invalid_config", why);
            return;
        }
        esp_err_t error = radio_configure(&g_config);
        if (error != ESP_OK) {
            g_state = STATE_ERROR;
            send_error(id, "radio_config", esp_err_to_name(error));
            return;
        }
        metrics_reset();
        g_state = STATE_CONFIGURED;
        send_state(id);
        return;
    }

    double requested_run = number_item(root, "run_id", 0);
    if (requested_run != 0 && requested_run != g_config.run_id) {
        send_error(id, "run_id_mismatch", "command does not match configured run");
        return;
    }
    if (strcmp(command, "arm") == 0) {
        if (g_state != STATE_CONFIGURED) {
            send_error(id, "invalid_state", "arm requires CONFIGURED");
            return;
        }
        metrics_reset();
        g_state = STATE_ARMED;
        send_state(id);
        return;
    }
    if (strcmp(command, "start") == 0) {
        if (g_state != STATE_ARMED) {
            send_error(id, "invalid_state", "start requires ARMED");
            return;
        }
        g_metrics.start_us = (uint64_t)esp_timer_get_time();
        g_state = STATE_RUNNING;
        esp_err_t error = generator_start();
        if (error != ESP_OK) {
            g_state = STATE_ERROR;
            send_error(id, "generator_start", esp_err_to_name(error));
            return;
        }
        send_state(id);
        return;
    }
    if (strcmp(command, "stop") == 0) {
        if (g_state == STATE_RUNNING || g_state == STATE_ARMED) {
            g_state = STATE_FINISHED;
            g_metrics.end_us = (uint64_t)esp_timer_get_time();
        }
        send_state(id);
        return;
    }
    if (strcmp(command, "result") == 0) {
        if (g_state != STATE_FINISHED) {
            send_error(id, "invalid_state", "result requires FINISHED");
            return;
        }
        cJSON *reply = response(id, true);
        cJSON_AddItemToObject(reply, "result", metrics_json());
        send_json(reply);
        return;
    }
    send_error(id, "unknown_command", "unsupported command");
}

void control_run(void)
{
    char line[2048];
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (g_state == STATE_BOOT) g_state = STATE_IDLE;
    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        cJSON *root = cJSON_Parse(line);
        if (!root) {
            send_error(0, "invalid_json", "expected one JSON object per line");
            continue;
        }
        dispatch(root);
        cJSON_Delete(root);
    }
}
