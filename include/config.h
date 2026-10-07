#pragma once

#include <Arduino.h>

// -----------------------------------------------------------------------------
// Firmware
// -----------------------------------------------------------------------------

// constexpr char FIRMWARE_VERSION[] = "0.9.0";
constexpr char FIRMWARE_VERSION[] = "1.2.0";
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

// Grace window for a redeDisponivel 1 -> 0 transition. A mains blink shorter
// than this is treated as transient: no FALTA is generated (no ThingsBoard event
// and no Telegram notification). The window delays the CONFIRMATION only: the
// FALTA timestamp remains the original instant the fall was detected.
constexpr uint32_t REDE_FAIL_GRACE_MS = 15000UL; // 15000UL;

constexpr uint32_t TELEGRAM_POLL_MS  = 2000;

// Administrative commands (/reboot, /ota, /who) do not need a fast response, so
// getUpdates() is polled at most once every TELEGRAM_GETUPDATES_INTERVAL_MS. The
// interval is counted from the END of the previous getUpdates() call.
constexpr uint32_t TELEGRAM_GETUPDATES_INTERVAL_MS = 20000UL;
constexpr uint32_t MQTT_RECONNECT_MS = 5000;
constexpr uint32_t WIFI_RETRY_MS     = 10000;

// Bounded wait for the network on the boot that runs a pending /ota update. The OTA client
// needs Wi-Fi; if it does not come up within this window the update is skipped, the failure
// is logged and the normal boot continues (the request is not retried automatically).
constexpr uint32_t OTA_BOOT_WIFI_WAIT_MS = 30000;

// -----------------------------------------------------------------------------
// OTA version check (/ota)
//
// /ota asks GitHub for the latest release tag BEFORE creating an OTA request, so this
// query runs during normal operation, with Telegram and ThingsBoard already connected.
// The TLS client therefore mirrors their profile (4096/512): it is the same buffer
// pair the Telegram poll allocates on every getUpdates() while ThingsBoard is up.
// -----------------------------------------------------------------------------

// Deadline for reading the GitHub reply.
constexpr uint32_t OTA_VERSION_TIMEOUT_MS = 6000UL;

// Bound for the DNS lookup of the API host. Same reason as TELEGRAM_CONNECT_TIMEOUT_MS:
// WiFiClientSecure does its own lookup with a fixed 10 s timeout that ignores setTimeout().
constexpr uint32_t OTA_VERSION_CONNECT_TIMEOUT_MS = 3000;

// Sanity cap for the release JSON (the live reply is ~3.3 kB). A larger body is treated as
// "could not verify" instead of being parsed.
constexpr size_t OTA_VERSION_BODY_MAX = 16384;

constexpr uint32_t ALIVE_INTERVAL_MS = 300000; // local alive, ~5 min
constexpr uint32_t TB_HEARTBEAT_MS   = 60000;  // supervision heartbeat, ~60 s

constexpr uint32_t DIAGNOSTIC_INTERVAL_MS = 60000;

constexpr uint32_t TELEGRAM_CONNECT_TIMEOUT_MS  = 1500;

// Deadline for reading a whole HTTP reply. In the field, Telegram replies were
// observed to arrive up to ~6 s after the request, so the read waits this long
// before giving up. Kept at 6000 ms on purpose: do not go back to 1500 ms.
constexpr uint32_t TELEGRAM_RESPONSE_TIMEOUT_MS = 6000UL;

// Minimum spacing between the END of one Telegram HTTP request and the START of
// the next one when the persistent TLS connection is being REUSED. Applied only
// on reuse: a new connection is never delayed. 0 disables the spacing.
constexpr uint32_t TELEGRAM_REUSE_MIN_GAP_MS = 700;

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