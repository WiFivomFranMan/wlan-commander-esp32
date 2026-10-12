// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>

#include "harness.h"

namespace t {
int g_failures = 0;
static int g_case_failures = 0;

std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

void fail(const char* file, int line, const std::string& msg) {
  ++g_failures;
  ++g_case_failures;
  std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, msg.c_str());
}
}  // namespace t

int main(int argc, char** argv) {
  const char* only = argc > 1 ? argv[1] : nullptr;
  int run = 0, failed = 0;
  for (const auto& c : t::registry()) {
    if (only && std::strstr(c.name, only) == nullptr) continue;
    t::g_case_failures = 0;
    c.fn();
    ++run;
    if (t::g_case_failures) {
      ++failed;
      std::fprintf(stderr, "FAILED %s\n", c.name);
    }
  }
  std::printf("unit: %d tests, %d failed, %d checks failed\n", run, failed, t::g_failures);
  return failed ? 1 : 0;
}
