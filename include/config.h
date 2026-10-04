#pragma once

#include <Arduino.h>

// -----------------------------------------------------------------------------
// Firmware
// -----------------------------------------------------------------------------

constexpr char FIRMWARE_VERSION[] = "0.1.0";
constexpr char DEVICE_ID[] = "DG-01";

// -----------------------------------------------------------------------------
// GPIOs - ESP8266 NodeMCU
//
// The optocoupler converter outputs HIGH when the corresponding 220 V signal
// is present. Inputs are therefore read as active HIGH.
// -----------------------------------------------------------------------------

constexpr uint8_t PIN_REDE_DISP       = D1;
constexpr uint8_t PIN_ALIM_REDE       = D2;
constexpr uint8_t PIN_ALIM_OFFGRID    = D5;
constexpr uint8_t PIN_ALIM_GERADOR    = D6;

// -----------------------------------------------------------------------------
// Timing
// -----------------------------------------------------------------------------

constexpr uint32_t INPUT_SAMPLE_MS        = 100;
constexpr uint32_t INPUT_DEBOUNCE_MS      = 150;
constexpr uint32_t TELEGRAM_POLL_MS       = 2000;
constexpr uint32_t MQTT_RECONNECT_MS      = 5000;
constexpr uint32_t TB_HEARTBEAT_MS        = 300000;
constexpr uint32_t WIFI_RETRY_MS          = 10000;

// Telegram HTTP operations are deliberately short so that they cannot
// monopolize the main loop for many seconds.
constexpr uint32_t TELEGRAM_CONNECT_TIMEOUT_MS = 1500;
constexpr uint32_t TELEGRAM_RESPONSE_TIMEOUT_MS = 1500;

// -----------------------------------------------------------------------------
// LittleFS state
// -----------------------------------------------------------------------------

constexpr char STATE_FILE_A[] = "/state_a.bin";
constexpr char STATE_FILE_B[] = "/state_b.bin";

constexpr uint32_t STATE_MAGIC = 0x4D444731; // "MDG1"
constexpr uint16_t STATE_VERSION = 1;

// -----------------------------------------------------------------------------
// ThingsBoard
// -----------------------------------------------------------------------------

constexpr char TB_TELEMETRY_TOPIC[] = "v1/devices/me/telemetry";
constexpr uint8_t TB_QOS = 1;

// -----------------------------------------------------------------------------
// Telemetry keys
// -----------------------------------------------------------------------------

constexpr char KEY_REDE_DISP[]    = "rede_disponivel";
constexpr char KEY_ALIM_REDE[]    = "alimentacao_rede";
constexpr char KEY_ALIM_OFFGRID[] = "alimentacao_offgrid";
constexpr char KEY_ALIM_GERADOR[] = "alimentacao_gerador";
constexpr char KEY_UPTIME[]       = "uptime_s";
constexpr char KEY_WIFI_RSSI[]    = "wifi_rssi";
