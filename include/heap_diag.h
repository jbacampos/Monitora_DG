#pragma once

#include <Arduino.h>
#include <StackThunk.h>

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

// Diagnostic-only stack-thunk snapshot helper (instrumentation round only).
//
// The ESP8266 core swaps in a heap-allocated secondary stack (the "stack thunk", 6200 B) whenever
// BearSSL code runs. It is allocated by the FIRST SSL server/client that is constructed and freed by
// the LAST one (refcounted in the core, see StackThunk.cpp). This helper prints, with the same
// [millis()] prefix as heapDiag() so both timelines interleave:
//   refcnt   = stack_thunk_get_refcnt()  (pure read: 0 = never allocated, >=1 = N live SSL objects)
//   ptr      = "set" when stack_thunk_ptr != nullptr (the 6200 B block currently exists)
//   free     = ESP.getFreeHeap()
//   maxblock = ESP.getMaxFreeBlockSize()
//
// stack_thunk_get_refcnt() and stack_thunk_ptr are read-only accesses: they never allocate, free or
// otherwise touch the thunk, so calling this helper cannot perturb the accounting it is measuring.
inline void stackThunkDiag(const char *tag) {
   Serial.printf("[%lu] THUNK: %s refcnt=%u ptr=%s free=%u maxblock=%u\n", static_cast<unsigned long>(millis()), tag,
                 static_cast<unsigned>(stack_thunk_get_refcnt()), stack_thunk_ptr != nullptr ? "set" : "null",
                 static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxFreeBlockSize()));
}
