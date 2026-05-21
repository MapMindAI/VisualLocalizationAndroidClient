

#include "control/control_callback.h"
// #include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#define TAG "[CONTORL]"

// Motor functions

static void motor_pin_init() {
  gpio_config_t io_conf = {
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = (1ULL << MOTOR_B_BIN2_PIN) | (1ULL << MOTOR_B_BIN1_PIN) |
                      (1ULL << MOTOR_A_BIN1_PIN) | (1ULL << MOTOR_A_BIN2_PIN) |
                      (1ULL << MOTOR_STBY_PIN),
  };
  gpio_config(&io_conf);

  // set direction
  gpio_set_level(MOTOR_A_BIN1_PIN, 1);
  gpio_set_level(MOTOR_A_BIN2_PIN, 0);
  gpio_set_level(MOTOR_B_BIN1_PIN, 1);
  gpio_set_level(MOTOR_B_BIN2_PIN, 0);

  // enable driver
  gpio_set_level(MOTOR_STBY_PIN, 1);
}

static void motor_pwm_init() {
  ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                               .timer_num = LEDC_TIMER_0,
                               .duty_resolution = LEDC_TIMER_8_BIT,
                               .freq_hz = 1000,
                               .clk_cfg = LEDC_AUTO_CLK};

  ledc_timer_config(&timer);

  ledc_channel_config_t chA = {.gpio_num = MOTOR_A_PWMA_PIN,
                               .speed_mode = LEDC_LOW_SPEED_MODE,
                               .channel = LEDC_CHANNEL_0,
                               .timer_sel = LEDC_TIMER_0,
                               .duty = 0};

  ledc_channel_config(&chA);

  ledc_channel_config_t chB = {.gpio_num = MOTOR_B_PWMA_PIN,
                               .speed_mode = LEDC_LOW_SPEED_MODE,
                               .channel = LEDC_CHANNEL_1,
                               .timer_sel = LEDC_TIMER_0,
                               .duty = 0};

  ledc_channel_config(&chB);
}

// static void battery_adc_init() {
//   adc1_config_width(ADC_WIDTH_BIT_12);
//   adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
// }
//
// float get_battery_voltage() {
//   int raw = adc1_get_raw(ADC1_CHANNEL_6);
//
//   float voltage = raw * 0.05371;
//
//   printf("Battery: %.2f V\n", voltage);
//
//   return voltage;
// }

static void motor_set_speed(int speedA, int speedB) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, speedA);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, speedB);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

void InitializeMotor() {
  motor_pin_init();
  motor_pwm_init();

  motor_set_speed(0, 0);
  // battery_adc_init();
}

static void forward(int speed) {
  gpio_set_level(MOTOR_A_BIN1_PIN, 0);
  gpio_set_level(MOTOR_A_BIN2_PIN, 1);

  gpio_set_level(MOTOR_B_BIN1_PIN, 0);
  gpio_set_level(MOTOR_B_BIN2_PIN, 1);

  motor_set_speed(speed, speed);
}

static void backward(int speed) {
  gpio_set_level(MOTOR_A_BIN1_PIN, 1);
  gpio_set_level(MOTOR_A_BIN2_PIN, 0);

  gpio_set_level(MOTOR_B_BIN1_PIN, 1);
  gpio_set_level(MOTOR_B_BIN2_PIN, 0);

  motor_set_speed(speed, speed);
}

static void turn_left(int speed) {
  gpio_set_level(MOTOR_A_BIN1_PIN, 0);
  gpio_set_level(MOTOR_A_BIN2_PIN, 1);

  gpio_set_level(MOTOR_B_BIN1_PIN, 1);
  gpio_set_level(MOTOR_B_BIN2_PIN, 0);

  motor_set_speed(speed, speed);
}

static void turn_right(int speed) {
  gpio_set_level(MOTOR_A_BIN1_PIN, 1);
  gpio_set_level(MOTOR_A_BIN2_PIN, 0);

  gpio_set_level(MOTOR_B_BIN1_PIN, 0);
  gpio_set_level(MOTOR_B_BIN2_PIN, 1);

  motor_set_speed(speed, speed);
}

static void stop() { motor_set_speed(0, 0); }

static int64_t led_updated_time_ = 0;

void ControlMessageCallback(int len, const uint8_t* value) {
  uint8_t message_type = value[0];
  switch (message_type) {
    case 66:
      esp_restart();  // Reboots the ESP32
      break;
    case 8: {
      ESP_LOGI(TAG, "move front");
      forward(125);
    } break;
    case 2: {
      ESP_LOGI(TAG, "move back");
      backward(125);
    } break;
    case 4: {
      ESP_LOGI(TAG, "move left");
      turn_left(125);
    } break;
    case 5: {
      ESP_LOGI(TAG, "stop");
      stop();
    } break;
    case 6: {
      ESP_LOGI(TAG, "move right");
      turn_right(125);
    } break;
    case 10: {
      ESP_LOGI(TAG, "move left right");
      uint32_t angle_value = ((uint32_t)value[1]) | ((uint32_t)value[2] << 8) |
                             ((uint32_t)value[3] << 16) | ((uint32_t)value[4] << 24);
      float angle = (float)angle_value / 100.0f - 180.0f;
      SetLeftRightAngle(angle);
    } break;
    case 11: {
      ESP_LOGI(TAG, "move up down");
      uint32_t angle_value = ((uint32_t)value[1]) | ((uint32_t)value[2] << 8) |
                             ((uint32_t)value[3] << 16) | ((uint32_t)value[4] << 24);
      float angle = (float)angle_value / 100.0f - 180.0f;
      SetUpDownAngle(angle);
    } break;
    default: {
      ESP_LOG_BUFFER_HEX(TAG, value, len);
    } break;
  }
  led_updated_time_ = esp_timer_get_time();
  gpio_set_level(LED_PIN, LED_LIGHT_ON);
}

void CansbusControlMessageCallback(int len, const uint8_t* value) {
  uint8_t message_type = value[0];
  switch (message_type) {
    case 66:
      esp_restart();  // Reboots the ESP32
      break;
    case 8: {
      ESP_LOGI(TAG, "move front");
      forward(125);
    } break;
    case 2: {
      ESP_LOGI(TAG, "move back");
      backward(125);
    } break;
    case 4: {
      ESP_LOGI(TAG, "move left");
      turn_left(125);
    } break;
    case 5: {
      ESP_LOGI(TAG, "stop");
      stop();
    } break;
    case 6: {
      ESP_LOGI(TAG, "move right");
      turn_right(125);
    } break;
    case 10: {
      ESP_LOGI(TAG, "move left right");
      uint32_t angle_value = ((uint32_t)value[1]) | ((uint32_t)value[2] << 8) |
                             ((uint32_t)value[3] << 16) | ((uint32_t)value[4] << 24);
      float angle = (float)angle_value / 100.0f - 180.0f;
      SetLeftRightAngle(angle);
    } break;
    case 11: {
      ESP_LOGI(TAG, "move up down");
      uint32_t angle_value = ((uint32_t)value[1]) | ((uint32_t)value[2] << 8) |
                             ((uint32_t)value[3] << 16) | ((uint32_t)value[4] << 24);
      float angle = (float)angle_value / 100.0f - 180.0f;
      SetUpDownAngle(angle);
    } break;
    default: {
      ESP_LOG_BUFFER_HEX(TAG, value, len);
    } break;
  }
  led_updated_time_ = esp_timer_get_time();
  gpio_set_level(LED_PIN, LED_LIGHT_ON);
}

void camera_test_task(void* param) {
  // 3. 舵机角度对应的占空比
  uint32_t duty_min = (uint32_t)((1.0 / 20.0) * 65535);  // 1ms / 20ms
  uint32_t duty_mid = (uint32_t)((1.5 / 20.0) * 65535);  // 1.5ms / 20ms
  uint32_t duty_max = (uint32_t)((2.0 / 20.0) * 65535);  // 2ms / 20ms

  while(1) {
      printf("test\n");
      // 旋转到 0°
      SetLeftRightAngle(0);
      // ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, duty_min);
      // ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
      // ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, duty_min);
      // ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
      vTaskDelay(pdMS_TO_TICKS(1000));

      // 旋转到 90°
      SetLeftRightAngle(90);

      // ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, duty_mid);
      // ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
      // ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, duty_mid);
      // ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
      vTaskDelay(pdMS_TO_TICKS(1000));

      // 旋转到 180°
      SetLeftRightAngle(180);

      // ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, duty_max);
      // ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
      // ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, duty_max);
      // ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
      vTaskDelay(pdMS_TO_TICKS(2000));
  }

  // Notify main task of deletion
  xTaskNotifyGive((TaskHandle_t)param);
  vTaskDelete(NULL);
}

void SetLeftRightAngle(float angle_deg) {
  printf("set left right %f\n", angle_deg);
  float offset = 1.0;
  float value = angle_deg / 180.0 + offset;
  uint32_t duty_min = (uint32_t)((value / 20.0) * 65535);
  ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_2, duty_min);
  ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_2);
}

void SetUpDownAngle(float angle_deg) {
  printf("set up down %f\n", angle_deg);
  float offset = 1.0;
  float value = angle_deg / 180.0 + offset;
  uint32_t duty_min = (uint32_t)((value / 20.0) * 65535);
  ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_3, duty_min);
  ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_3);
}

// PWM motor functions
void InitCameraServo() {
  // 1. 配置 PWM 定时器
  ledc_timer_config_t ledc_timer = {
      .speed_mode = LEDC_HIGH_SPEED_MODE,
      .timer_num = LEDC_TIMER_1,
      .duty_resolution = SERVO_RESOLUTION,
      .freq_hz = SERVO_FREQ,
      .clk_cfg = LEDC_AUTO_CLK
  };
  ledc_timer_config(&ledc_timer);

  // 2. 配置 PWM 通道
  ledc_channel_config_t ledc_channel_1 = {
      .speed_mode = LEDC_HIGH_SPEED_MODE,
      .channel = LEDC_CHANNEL_2,
      .timer_sel = LEDC_TIMER_1,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = UP_SERVO_PIN,
      .duty = 0, // 初始占空比
      .hpoint = 0
  };
  ledc_channel_config(&ledc_channel_1);

  // 2. 配置 PWM 通道
  ledc_channel_config_t ledc_channel_2 = {
      .speed_mode = LEDC_HIGH_SPEED_MODE,
      .channel = LEDC_CHANNEL_3,
      .timer_sel = LEDC_TIMER_1,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = DOWN_SERVO_PIN,
      .duty = 0, // 初始占空比
      .hpoint = 0
  };
  ledc_channel_config(&ledc_channel_2);
  // xTaskCreatePinnedToCore(camera_test_task, "camera_test_task", 4096, NULL, 1, NULL, 0);
}

// LED CONTROL
void SetUpLed() {
  gpio_config_t io_conf = {
      .intr_type = GPIO_INTR_DISABLE,  // disable interrupt
      .mode = GPIO_MODE_OUTPUT,        // set as output mode
      .pin_bit_mask = 1ULL << LED_PIN,
      .pull_down_en = 0,  // disable pull-down mode
      .pull_up_en = 0,    // disable pull-up mode
  };
  // configure GPIO with the given settings
  gpio_config(&io_conf);
  gpio_set_level(LED_PIN, LED_LIGHT_OFF);
}

void UpdateLed(int64_t boottime_ms) {
  if (boottime_ms - led_updated_time_ > 500000) {
    printf("force stop\n");
    gpio_set_level(LED_PIN, LED_LIGHT_OFF);
    led_updated_time_ = boottime_ms;
    stop();
  }
}
