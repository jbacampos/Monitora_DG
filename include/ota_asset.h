#pragma once

// Pure selection of the release asset used by /ota (no Arduino, no network, no allocation), covered
// by the host tests in test/host together with the rest of the pure logic.
//
// /releases/latest lists its assets; /ota must pick EXCLUSIVELY the one whose name matches the
// expected firmware asset. To never download the wrong or an ambiguous asset, the match is exact and
// the selection succeeds only when EXACTLY ONE asset carries that name.

#include <stddef.h>
#include <string.h>

// Looks for `wanted` among the parallel `names`/`urls` arrays (count entries each). On success it
// copies the matching URL into `out` (which must hold outSize bytes, NUL included) and returns true.
// Returns false when the asset is absent, the name is ambiguous (more than one match), or the URL is
// empty or does not fit `out` — the caller must then abort the update with an explicit error.
inline bool selectAssetUrl(
   const char *const *names, const char *const *urls, size_t count, const char *wanted, char *out, size_t outSize
) {
   if (names == nullptr || urls == nullptr || wanted == nullptr || out == nullptr || outSize == 0) {
      return false;
   }

   const char *found = nullptr;
   size_t matches = 0;

   for (size_t i = 0; i < count; ++i) {
      const char *name = names[i];
      const char *url = urls[i];

      if (name != nullptr && url != nullptr && url[0] != '\0' && strcmp(name, wanted) == 0) {
         found = url;
         ++matches;
      }
   }

   if (matches != 1 || found == nullptr) {
      return false; // absent or ambiguous
   }

   const size_t length = strlen(found);

   if (length + 1 > outSize) {
      return false; // would not fit (outSize includes the terminating NUL)
   }

   memcpy(out, found, length + 1);
   return true;
}
