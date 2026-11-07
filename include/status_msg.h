#pragma once

// Pure Telegram "/s" (status) reply builder (no Arduino, no network, no allocation), covered by the
// host tests in test/host together with the rest of the pure logic.
//
// The reply is a leading blank line, "Firmware: <FIRMWARE_VERSION>", "Uptime: <d/h/m/s>" and
// "Energia: <four indicators>". The four states come from the SAME PowerState used by the monitoring
// (InputGroup::state()) and by the visual LED line; the version is passed in by the caller (from
// FIRMWARE_VERSION) so this header stays free of the Arduino API and cannot create a second source
// of the version. The uptime is derived from millis() (monotonic, resets at boot) with plain
// unsigned arithmetic, so there is no absolute-time (NTP) dependency.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "types.h"

// Energy indicators. The "on" glyphs are the SAME bytes as the visual LED line (main.cpp), so the
// "Energia" line is the same four-LED picture the group already knows; "off" mirrors its OFF marker.
constexpr char STATUS_GLYPH_REDE[]      = "\xF0\x9F\x9F\xA2"; // 🟢 rede disponivel
constexpr char STATUS_GLYPH_ALIM_REDE[] = "\xF0\x9F\x94\xB5"; // 🔵 alimentacao pela rede
constexpr char STATUS_GLYPH_OFFGRID[]   = "\xF0\x9F\x9F\xA1"; // 🟡 alimentacao pelo offgrid
constexpr char STATUS_GLYPH_GERADOR[]   = "\xF0\x9F\x94\xB4"; // 🔴 alimentacao pelo gerador
constexpr char STATUS_GLYPH_OFF[]       = "\xE2\x9A\xAA";     // ⚪ desligado

// Formats an elapsed time in milliseconds as days / hours / minutes / seconds, e.g. "3d 7h 42m 25s",
// "7h 42m 25s" (no full day) or "42m 25s" (no full hour), WITHOUT leading zeros. The units are
// omitted by MAGNITUDE (never by a zero value): below one hour the days and hours are dropped, below
// one day the days are dropped. Pure unsigned arithmetic: no overflow, no absolute clock.
inline void formatUptime(uint32_t uptimeMs, char *out, size_t outSize) {
   const uint32_t totalSeconds = uptimeMs / 1000UL;
   const uint32_t days         = totalSeconds / (24UL * 60UL * 60UL);
   const uint32_t hours        = (totalSeconds / (60UL * 60UL)) % 24UL;
   const uint32_t minutes      = (totalSeconds / 60UL) % 60UL;
   const uint32_t seconds      = totalSeconds % 60UL;

   if (days > 0) {
      snprintf(out, outSize, "%lud %luh %lum %lus", static_cast<unsigned long>(days), static_cast<unsigned long>(hours),
               static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));
   } else if (hours > 0) {
      snprintf(out, outSize, "%luh %lum %lus", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes),
               static_cast<unsigned long>(seconds));
   } else {
      snprintf(out, outSize, "%lum %lus", static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));
   }
}

// The four channel indicators, in the fixed order (rede, rede, offgrid, gerador), e.g. "🟢 🔵 ⚪ ⚪".
// Each channel shows its own "on" glyph when energised, or the shared "off" glyph otherwise.
inline void energyIndicators(const PowerState &s, char *out, size_t outSize) {
   snprintf(out, outSize, "%s %s %s %s", s.redeDisponivel ? STATUS_GLYPH_REDE : STATUS_GLYPH_OFF,
            s.alimentacaoRede ? STATUS_GLYPH_ALIM_REDE : STATUS_GLYPH_OFF, s.alimentacaoOffgrid ? STATUS_GLYPH_OFFGRID : STATUS_GLYPH_OFF,
            s.alimentacaoGerador ? STATUS_GLYPH_GERADOR : STATUS_GLYPH_OFF);
}

// Builds the whole "/s" reply into `out` (a leading blank line, then the version, the uptime and the
// four energy indicators). `version` is FIRMWARE_VERSION and `uptimeMs` is millis().
inline void buildStatusMessage(const PowerState &s, const char *version, uint32_t uptimeMs, char *out, size_t outSize) {
   char uptime[32];
   formatUptime(uptimeMs, uptime, sizeof(uptime));

   char energy[24];
   energyIndicators(s, energy, sizeof(energy));

   snprintf(out, outSize,
            "\nFirmware: %s\n\n"
            "Uptime: %s\n\n"
            "Energia: %s",
            version, uptime, energy);
}
