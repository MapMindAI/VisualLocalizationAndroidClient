#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if ENABLE_UART_MOTOR_CONSOLE
#include "driver/uart.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ble/ble_client.h"
#include "config.h"
#include "control/control_callback.h"

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#define TAG "[MKBOT] "

#if ENABLE_UART_MOTOR_CONSOLE
static void process_console_line(const char* line) {
  if (line == NULL || line[0] == '\0') {
    return;
  }
  ESP_LOGI(TAG, "UART input: %s", line);
  if (strncmp(line, "stop", 4) == 0) {
    StopAllMotors();
    ESP_LOGI(TAG, "All motors stopped.");
    return;
  }

  char motor = 0;
  char action = 0;
  int speed = 125;
  const int matched = sscanf(line, "m %c %c %d", &motor, &action, &speed);
  if (matched >= 2) {
    int dir = 0;
    if (action == 'f' || action == 'F') dir = 1;
    if (action == 'b' || action == 'B') dir = -1;
    if (action == 's' || action == 'S') dir = 0;
    if (speed < 0) speed = 0;
    if (speed > 255) speed = 255;
    if (dir == 0) {
      StopAllMotors();
      ESP_LOGI(TAG, "Motor %c stop", motor);
    } else {
      ControlSingleMotor(motor, dir, speed);
      ESP_LOGI(TAG, "Motor %c dir=%d speed=%d", motor, dir, speed);
    }
    return;
  }
  ESP_LOGW(TAG, "Unknown command: %s", line);
}

static void uart_console_task(void* arg) {
  (void)arg;
  const uart_port_t uart_num = UART_NUM_0;
  uart_driver_install(uart_num, 1024, 0, 0, NULL, 0);
  char line[96];
  int line_len = 0;
  uint8_t ch[10];
  ESP_LOGI(TAG, "UART motor console ready.");
  ESP_LOGI(TAG, "Commands:");
  ESP_LOGI(TAG, "  m <A|B|C|D> <f|b|s> [speed(0-255)]");
  ESP_LOGI(TAG, "  stop");
  ESP_LOGI(TAG, "Examples: m A f 255, m C b 100, m D s, stop");
  printf("motor>\n");
  fflush(stdout);
  while (1) {
    const int n = uart_read_bytes(uart_num, ch, sizeof(ch), pdMS_TO_TICKS(100));
    if (n <= 0) {
      continue;
    }

    for (int i = 0; i < n; i++) {
      if (line_len < (int)sizeof(line) - 1) {
        line[line_len++] = (char)ch[i];
      }
    }

    uint8_t end = ch[n-1];
    if (end == '\r' || end == '\n') {
      if (line_len > 0) {
        line[line_len] = '\0';
        process_console_line(line);
        memset(line, '\0', sizeof(line));
        line_len = 0;
      }
      continue;
    }

  }
}
#endif

void app_main(void) {
  ESP_LOGI(TAG, "Hello from MOKUKU ROBOT %s!", MOKUKU_VERSION);
  SetUpLed();
  InitializeMotor();


  SetBleMessageHandler(ControlMessageCallback);
  ObdBleClientSetup();

#if ENABLE_UART_MOTOR_CONSOLE
  xTaskCreate(uart_console_task, "uart_console_task", 4096, NULL, 5, NULL);
#endif

  while (1) {
    int64_t boottime_ms = esp_timer_get_time();
    UpdateLed(boottime_ms);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
