#pragma once

#include <cstdio>
#include <cstdlib>

// Minimal check macro -- no external test framework, matching the project's
// hand-rolled ethos. Prints the failing expression and exits non-zero, which
// is all ctest needs to mark a test failed.
#define RTP_CHECK(cond)                                                           \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                               \
    }                                                                             \
  } while (0)
