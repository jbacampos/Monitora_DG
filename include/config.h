#pragma once

#include <Arduino.h>

// -----------------------------------------------------------------------------
// Firmware
// -----------------------------------------------------------------------------

constexpr char FIRMWARE_VERSION[] = "0.2.0";
constexpr char DEVICE_ID[] = "DG-01";

// -----------------------------------------------------------------------------
// GPIOs - ESP8266 NodeMCU
//
// All four inputs are treated LOGICALLY as ACTIVE-HIGH. The physical wiring may
// be inverted per channel; the rest of the firmware only ever sees the
// normalised logical level.
// -----------------------------------------------------------------------------

constexpr uint8_t PIN_REDE_DISP    = D1;
constexpr uint8_t PIN_ALIM_REDE    = D2;
constexpr uint8_t PIN_ALIM_OFFGRID = D5;
constexpr uint8_t PIN_ALIM_GERADOR = D6;

constexpr bool INPUT_REDE_DISPONIVEL_INVERTED     = false;
constexpr bool INPUT_ALIMENTACAO_REDE_INVERTED    = false;
constexpr bool INPUT_ALIMENTACAO_OFFGRID_INVERTED = false;
constexpr bool INPUT_ALIMENTACAO_GERADOR_INVERTED = false;

// -----------------------------------------------------------------------------
// Timing
// -----------------------------------------------------------------------------

constexpr uint32_t INPUT_SAMPLE_MS    = 100;
constexpr uint32_t INPUT_DEBOUNCE_MS  = 150;
constexpr uint32_t INPUT_STABILIZE_MS = 400;

constexpr uint32_t TELEGRAM_POLL_MS  = 2000;
constexpr uint32_t MQTT_RECONNECT_MS = 5000;
constexpr uint32_t WIFI_RETRY_MS     = 10000;

constexpr uint32_t ALIVE_INTERVAL_MS = 300000; // local alive, ~5 min
constexpr uint32_t TB_HEARTBEAT_MS   = 60000;  // supervision heartbeat, ~60 s

constexpr uint32_t DIAGNOSTIC_INTERVAL_MS = 60000;

constexpr uint32_t TELEGRAM_CONNECT_TIMEOUT_MS  = 1500;
constexpr uint32_t TELEGRAM_RESPONSE_TIMEOUT_MS = 1500;

// -----------------------------------------------------------------------------
// Time source / NTP
// -----------------------------------------------------------------------------

constexpr char NTP_SERVER[]  = "pool.ntp.org";
constexpr char TZ_BRASILIA[] = "<-03>3"; // America/Sao_Paulo (UTC-3, no DST)

// -----------------------------------------------------------------------------
// LittleFS files
// -----------------------------------------------------------------------------

constexpr char STATE_FILE[]   = "/state.bin";
constexpr char STATE_TMP[]    = "/state.tmp";
constexpr char PENDING_FILE[] = "/pending.bin";
constexpr char PENDING_TMP[]  = "/pending.tmp";

// Approximate budget for the ThingsBoard append-only log (~512 KB).
constexpr uint32_t TB_LOG_BUDGET_BYTES = 512UL * 1024UL;

// Persist tbLogHead to Flash at most every N published events (at-least-once).
constexpr uint32_t TB_HEAD_SAVE_EVERY = 16;

// -----------------------------------------------------------------------------
// ThingsBoard
// -----------------------------------------------------------------------------

constexpr char TB_TELEMETRY_TOPIC[]  = "v1/devices/me/telemetry";
constexpr char TB_ATTRIBUTES_TOPIC[] = "v1/devices/me/attributes";

constexpr char KEY_REDE_DISP[]    = "rede_disponivel";
constexpr char KEY_ALIM_REDE[]    = "alimentacao_rede";
constexpr char KEY_ALIM_OFFGRID[] = "alimentacao_offgrid";
constexpr char KEY_ALIM_GERADOR[] = "alimentacao_gerador";
constexpr char KEY_TS[]           = "ts";
constexpr char KEY_TS_UNKNOWN[]   = "ts_unknown";
constexpr char KEY_LAST_ALIVE[]   = "last_alive";
constexpr char KEY_UPTIME[]       = "uptime_s";
constexpr char KEY_WIFI_RSSI[]    = "wifi_rssi";