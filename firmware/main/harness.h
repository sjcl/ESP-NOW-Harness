#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HARNESS_CONTROL_VERSION 1
#define HARNESS_FW_VERSION "0.2.0"
#define HARNESS_MAX_STREAMS 16
#define HARNESS_MAX_PACKET_SIZE 250
#define HARNESS_KEY_SIZE 16
#define HARNESS_MAX_TX_WINDOW 32

typedef enum { STATE_BOOT, STATE_IDLE, STATE_CONFIGURED, STATE_ARMED, STATE_RUNNING, STATE_FINISHED, STATE_ERROR } harness_state_t;
typedef enum { ROLE_NONE, ROLE_GENERATOR, ROLE_SINK, ROLE_PING_INITIATOR, ROLE_ECHO_RESPONDER } harness_role_t;
typedef enum { MODE_PING, MODE_STREAM, MODE_MULTI_STREAM, MODE_SATURATION, MODE_LATENCY_LOAD } harness_mode_t;

typedef struct {
    uint32_t run_id;
    harness_role_t role;
    harness_mode_t mode;
    uint8_t peer_mac[6];
    bool band_5ghz;
    char country[3];
    uint8_t channel;
    char phy_rate[16];
    int8_t tx_power_qdbm;
    bool power_save;
    bool encryption;
    uint8_t key[HARNESS_KEY_SIZE];
    uint64_t duration_us;
    uint16_t packet_size;
    uint16_t streams;
    uint16_t tx_window;
    uint32_t rate_per_stream;
    uint32_t probe_rate;
    uint16_t background_packet_size;
    uint32_t background_rate;
} harness_config_t;

extern harness_config_t g_config;
extern volatile harness_state_t g_state;
const char *harness_state_name(harness_state_t state);
