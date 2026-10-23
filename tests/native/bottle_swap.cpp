#include <cassert>
#include "BottleSwapState.h"

int main(int, char**) {
  using S = BottleSwapState;
  const char* a = "0123456789abcdef0123456789abcdef";
  const char* b = "abcdef0123456789abcdef0123456789";
  S s;
  s.reset(10, 0, 0);
  assert(!S::validId("short"));
  assert(!S::validId("0123456789ABCDEF0123456789ABCDEF"));
  assert(s.pause(a, 100, 17, 28) == S::TRANSITION);
  assert(s.state == S::PAUSED && s.left == 17 && s.right == 28);
  assert(s.pause(a, 110, 99, 99) == S::REPEAT);
  assert(s.boundaryUs == 100 && s.left == 17);
  assert(s.pause(b, 120, 17, 28) == S::REJECT);
  assert(s.resume(b, 200, 17, 28) == S::REJECT);
  assert(s.resume(a, 200, 17, 28) == S::TRANSITION);
  assert(s.resume(a, 300, 17, 28) == S::REPEAT);
  assert(s.boundaryUs == 200);
  assert(!s.service(500200, true, true, 17, 28));
  // A touch or failed health check resets continuous release qualification.
  assert(!s.service(700200, true, false, 17, 28));
  assert(!s.service(750200, true, true, 17, 28));
  assert(!s.service(990200, false, true, 17, 28));
  assert(!s.service(1000200, true, true, 17, 28));
  assert(!s.service(1240200, true, true, 17, 28));
  assert(s.service(1250200, true, true, 17, 28));
  assert(s.state == S::ACTIVE && s.left == 17 && s.right == 28);
  assert(s.resume(a, 2000000, 999, 999) == S::REPEAT);
  assert(s.pause(a, 2000000, 999, 999) == S::REPEAT);
  assert(s.state == S::ACTIVE);
  assert(s.left == 17 && s.right == 28 && s.boundaryUs == 1250200);
  // A new swap preserves cumulative counts and permits retry after failure.
  assert(s.pause(b, 3000000, 25, 35) == S::TRANSITION);
  assert(s.state == S::PAUSED && s.left == 25 && s.right == 35);
  assert(s.resume(b, 3000100, 25, 35) == S::TRANSITION);
  assert(s.service(13000100, false, false, 25, 35));
  assert(s.state == S::FAILED);
  assert(s.pause(a, 14000000, 25, 35) == S::REJECT);
  assert(s.resume(b, 14000000, 25, 35) == S::TRANSITION);
  assert(!s.service(14500000, true, true, 25, 35));
  assert(s.service(14750000, true, true, 25, 35));
  s.reset(15000000, 25, 35);
  assert(s.state == S::ACTIVE && s.id[0] == '\0' && s.left == 25);
  assert(s.resume(b, 16000000, 25, 35) == S::REJECT);
}
