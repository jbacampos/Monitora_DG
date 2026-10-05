#pragma once

#include <Arduino.h>

// Diagnostic-only heap snapshot helper (instrumentation round only).
//
// Prints, with a monotonic [millis()] prefix so the boot/init timeline can be
// correlated against the Telegram/ThingsBoard logs:
//   free     = ESP.getFreeHeap()
//   maxblock = ESP.getMaxFreeBlockSize()   (largest contiguous free block)
//   frag     = ESP.getHeapFragmentation()  (%)
//
// It never changes any behaviour: it only reads the allocator state and writes
// to Serial. "maxblock" is the value that matters for a single large allocation
// such as BearSSL's 16709-byte TLS buffer.
inline void heapDiag(const char *tag) {
   Serial.printf("[%lu] HEAP: %s free=%u maxblock=%u frag=%u%%\n", static_cast<unsigned long>(millis()), tag, static_cast<unsigned>(ESP.getFreeHeap()),
                 static_cast<unsigned>(ESP.getMaxFreeBlockSize()), static_cast<unsigned>(ESP.getHeapFragmentation()));
}
