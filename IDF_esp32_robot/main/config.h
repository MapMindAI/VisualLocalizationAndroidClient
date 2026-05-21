#pragma once

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#define MOKUKU_VERSION "v0.0.1"

#define DEBUG_MODE

#if defined(DEBUG_MODE)
#define DMLOG(...) printf(__VA_ARGS__)
#else
#define DMLOG(...) ((void)0)  // do nothing
#endif

#define LED_PIN GPIO_NUM_2
#define LED_LIGHT_OFF 0
#define LED_LIGHT_ON 1
