#include <natives-dsl.h>

// so we dont conflict with the jdk/internal version
DECLARE_NATIVE_OVERLOADED("sun/misc", VM, initialize, "()V", 1) { return value_null(); }

// Instant.now() / Clock: nanoseconds of the wall clock since args[0] seconds after 1970; -1 when that difference does
// not fit in 2^32 seconds (the JDK's contract, as HotSpot). Was a constant 0, so Instant.now() never moved
// (Shiklo fork, 2026-10-28).
extern s64 wall_micros(void);
DECLARE_NATIVE("jdk/internal/misc", VM, getNanoTimeAdjustment, "(J)J") {
  s64 now = wall_micros();
  s64 diff = now / 1000000 - args[0].l;
  if (diff > ((s64)1 << 32) || diff < -((s64)1 << 32)) return (stack_value){.l = -1};
  return (stack_value){.l = diff * 1000000000LL + (now % 1000000) * 1000};
}