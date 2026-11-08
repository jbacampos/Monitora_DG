#pragma once

// Pure helpers for the environment (test/production) selection and the on-flash environment
// tag. Deliberately NOT <Arduino.h> and NOT secrets.h: this header compiles both in the
// firmware and in the host tests (test/host), so the routing rules are covered without any
// device or credential. Everything here is a pure function of its arguments.
#include <stdint.h>

// -----------------------------------------------------------------------------
// Environment tag
//
// The record stores (pending.bin and the Telegram ids/offsets kept in state.bin) are shared
// between both environments. A 1-byte tag persisted next to them (ENV_TAG_FILE) records which
// environment wrote them, so a store written under one environment is never consumed by the
// other. ENV_TAG_NONE means "never written" (a store from a firmware without this feature): it
// is ADOPTED without a reset, so upgrading never discards existing data.
// -----------------------------------------------------------------------------

constexpr uint8_t ENV_TAG_NONE = 0; // never written / unknown (legacy)
constexpr uint8_t ENV_TAG_PROD = 1;
constexpr uint8_t ENV_TAG_TEST = 2;

// Tag of the environment selected by TEST_MODE.
constexpr uint8_t envTagForMode(bool testMode) {
   return testMode ? ENV_TAG_TEST : ENV_TAG_PROD;
}

// True when a persisted store carries a tag from a DIFFERENT environment and therefore must NOT
// be consumed by the current one. ENV_TAG_NONE is never a conflict (adopt on migration).
constexpr bool envTagConflicts(uint8_t storedTag, uint8_t currentTag) {
   return storedTag != ENV_TAG_NONE && storedTag != currentTag;
}

// -----------------------------------------------------------------------------
// Credential selection
//
// SINGLE, unambiguous choice between the production and the test value. Every communication
// module consumes the selected constant (ACTIVE_*, see credentials.h); none of them decides its
// own environment. Kept as a pure function so the host tests cover both branches with dummy
// values (the real values live in the Git-ignored secrets.h).
// -----------------------------------------------------------------------------

constexpr const char *selectCredential(bool testMode, const char *production, const char *test) {
   return testMode ? test : production;
}
