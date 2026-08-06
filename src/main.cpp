#include <Arduino.h>

// Each PlatformIO environment selects one of the original Arduino examples.
// Keeping the task implementations in examples/ avoids maintaining duplicate
// copies of the experiment code.
#if defined(MOUSEHOUSE_TASK_FIXED_RATIO)
#include "../examples/fixed_ratio/fixed_ratio.ino"
#elif defined(MOUSEHOUSE_TASK_FREE_FEEDING)
#include "../examples/free_feeding/free_feeding.ino"
#elif defined(MOUSEHOUSE_TASK_GO_NOGO_AUTOMATED)
#include "../examples/go_nogo_automated/go_nogo_automated.ino"
#elif defined(MOUSEHOUSE_TASK_SETUP_DEBUG)
#include "../examples/setup_debug/setup_debug.ino"
#else
#error "Select a MouseHouse task environment in platformio.ini"
#endif
