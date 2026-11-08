#pragma once

// Central selection of the ACTIVE credentials.
//
// This is the ONLY place that picks between production and test routing. It derives everything
// from the single TEST_MODE flag (config.h) and exposes three constants that the communication
// modules consume. Telegram (telegram.cpp) and ThingsBoard (telemetry.cpp) MUST use these and
// MUST NOT reference the production/test values directly, so no module can pick an environment
// on its own.
//
// The raw values live in the Git-ignored secrets.h (never versioned). The GitHub token used by
// /ota (TELEGRAM_REPO_TOKEN) is INDEPENDENT of TEST_MODE and is referenced directly by ota.cpp.
#include "config.h"
#include "env_tag.h"
#include "secrets.h"

constexpr const char *ACTIVE_TELEGRAM_BOT_TOKEN = selectCredential(TEST_MODE, TELEGRAM_BOT_TOKEN_PROD, TELEGRAM_BOT_TOKEN_TEST);
constexpr const char *ACTIVE_TELEGRAM_CHAT_ID   = selectCredential(TEST_MODE, TELEGRAM_CHAT_ID_PROD, TELEGRAM_CHAT_ID_TEST);
constexpr const char *ACTIVE_TB_ACCESS_TOKEN    = selectCredential(TEST_MODE, TB_ACCESS_TOKEN_PROD, TB_ACCESS_TOKEN_TEST);

// Tag of the active environment, persisted in ENV_TAG_FILE so a store written under the other
// environment is never consumed here (see env_tag.h).
constexpr uint8_t ACTIVE_ENV_TAG = envTagForMode(TEST_MODE);
