#include "control/control_callback.h"

#include "driver/gpio.h"
#include "driver/ledc.h"

#define TAG "[CONTORL]"

static int64_t led_updated_time_ = 0;

static inline void set_motor_dir(gpio_num_t bin1, gpio_num_t bin2, int dir) {
  // dir: 1 forward, -1 backward, 0 brake/stop
  if (dir > 0) {
    gpio_set_level(bin1, 0);
    gpio_set_level(bin2, 1);
  } else if (dir < 0) {
    gpio_set_level(bin1, 1);
    gpio_set_level(bin2, 0);
  } else {
    gpio_set_level(bin1, 0);
    gpio_set_level(bin2, 0);
  }
}

static void motor_pin_init() {
  gpio_config_t io_conf = {
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = (1ULL << MOTOR_A_BIN1_PIN) | (1ULL << MOTOR_A_BIN2_PIN) |
                      (1ULL << MOTOR_B_BIN1_PIN) | (1ULL << MOTOR_B_BIN2_PIN) |
                      (1ULL << MOTOR_C_BIN1_PIN) | (1ULL << MOTOR_C_BIN2_PIN) |
                      (1ULL << MOTOR_D_BIN1_PIN) | (1ULL << MOTOR_D_BIN2_PIN),
  };
  gpio_config(&io_conf);

  set_motor_dir(MOTOR_A_BIN1_PIN, MOTOR_A_BIN2_PIN, 0);
  set_motor_dir(MOTOR_B_BIN1_PIN, MOTOR_B_BIN2_PIN, 0);
  set_motor_dir(MOTOR_C_BIN1_PIN, MOTOR_C_BIN2_PIN, 0);
  set_motor_dir(MOTOR_D_BIN1_PIN, MOTOR_D_BIN2_PIN, 0);
}

static void motor_pwm_init() {
  ledc_timer_config_t timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .timer_num = LEDC_TIMER_0,
      .duty_resolution = LEDC_TIMER_8_BIT,
      .freq_hz = 1000,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ledc_timer_config(&timer);

  ledc_channel_config_t ch = {
      .gpio_num = MOTOR_PWM_PIN,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .timer_sel = LEDC_TIMER_0,
      .duty = 0,
      .hpoint = 0,
      .intr_type = LEDC_INTR_DISABLE,
  };
  ledc_channel_config(&ch);
}

static void motor_set_speed(int speed) {
  if (speed < 0) speed = 0;
  if (speed > 255) speed = 255;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, speed);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void InitializeMotor() {
  motor_pin_init();
  motor_pwm_init();
  motor_set_speed(0);
}

static void forward(int speed) {
  set_motor_dir(MOTOR_A_BIN1_PIN, MOTOR_A_BIN2_PIN, 1);
  set_motor_dir(MOTOR_B_BIN1_PIN, MOTOR_B_BIN2_PIN, 1);
  set_motor_dir(MOTOR_C_BIN1_PIN, MOTOR_C_BIN2_PIN, 1);
  set_motor_dir(MOTOR_D_BIN1_PIN, MOTOR_D_BIN2_PIN, 1);
  motor_set_speed(speed);
}

static void backward(int speed) {
  set_motor_dir(MOTOR_A_BIN1_PIN, MOTOR_A_BIN2_PIN, -1);
  set_motor_dir(MOTOR_B_BIN1_PIN, MOTOR_B_BIN2_PIN, -1);
  set_motor_dir(MOTOR_C_BIN1_PIN, MOTOR_C_BIN2_PIN, -1);
  set_motor_dir(MOTOR_D_BIN1_PIN, MOTOR_D_BIN2_PIN, -1);
  motor_set_speed(speed);
}

static void turn_left(int speed) {
  set_motor_dir(MOTOR_A_BIN1_PIN, MOTOR_A_BIN2_PIN, -1);
  set_motor_dir(MOTOR_B_BIN1_PIN, MOTOR_B_BIN2_PIN, 1);
  set_motor_dir(MOTOR_C_BIN1_PIN, MOTOR_C_BIN2_PIN, -1);
  set_motor_dir(MOTOR_D_BIN1_PIN, MOTOR_D_BIN2_PIN, 1);
  motor_set_speed(speed);
}

static void turn_right(int speed) {
  set_motor_dir(MOTOR_A_BIN1_PIN, MOTOR_A_BIN2_PIN, 1);
  set_motor_dir(MOTOR_B_BIN1_PIN, MOTOR_B_BIN2_PIN, -1);
  set_motor_dir(MOTOR_C_BIN1_PIN, MOTOR_C_BIN2_PIN, 1);
  set_motor_dir(MOTOR_D_BIN1_PIN, MOTOR_D_BIN2_PIN, -1);
  motor_set_speed(speed);
}

static void stop() {
  motor_set_speed(0);
  set_motor_dir(MOTOR_A_BIN1_PIN, MOTOR_A_BIN2_PIN, 0);
  set_motor_dir(MOTOR_B_BIN1_PIN, MOTOR_B_BIN2_PIN, 0);
  set_motor_dir(MOTOR_C_BIN1_PIN, MOTOR_C_BIN2_PIN, 0);
  set_motor_dir(MOTOR_D_BIN1_PIN, MOTOR_D_BIN2_PIN, 0);
}

void StopAllMotors() { stop(); }

void ControlSingleMotor(char motor_id, int dir, int speed) {
  stop();
  if (dir > 0) dir = 1;
  if (dir < 0) dir = -1;
  if (dir == 0) return;

  switch (motor_id) {
    case 'A':
    case 'a':
      set_motor_dir(MOTOR_A_BIN1_PIN, MOTOR_A_BIN2_PIN, dir);
      break;
    case 'B':
    case 'b':
      set_motor_dir(MOTOR_B_BIN1_PIN, MOTOR_B_BIN2_PIN, dir);
      break;
    case 'C':
    case 'c':
      set_motor_dir(MOTOR_C_BIN1_PIN, MOTOR_C_BIN2_PIN, dir);
      break;
    case 'D':
    case 'd':
      set_motor_dir(MOTOR_D_BIN1_PIN, MOTOR_D_BIN2_PIN, dir);
      break;
    default:
      ESP_LOGW(TAG, "Unknown motor id: %c", motor_id);
      return;
  }
  motor_set_speed(speed);
  led_updated_time_ = esp_timer_get_time();
  gpio_set_level(LED_PIN, LED_LIGHT_ON);
}

void ControlMessageCallback(int len, const uint8_t* value) {
  uint8_t message_type = value[0];
  int speed = 255;
  switch (message_type) {
    case 66:
      esp_restart();
      break;
    case 8:
      ESP_LOGI(TAG, "move front");
      forward(speed);
      break;
    case 2:
      ESP_LOGI(TAG, "move back");
      backward(speed);
      break;
    case 4:
      ESP_LOGI(TAG, "move left");
      turn_left(speed);
      break;
    case 5:
      ESP_LOGI(TAG, "stop");
      stop();
      break;
    case 6:
      ESP_LOGI(TAG, "move right");
      turn_right(speed);
      break;
    default:
      ESP_LOG_BUFFER_HEX(TAG, value, len);
      break;
  }
  led_updated_time_ = esp_timer_get_time();
  gpio_set_level(LED_PIN, LED_LIGHT_ON);
}

void SetUpLed() {
  gpio_config_t io_conf = {
      .intr_type = GPIO_INTR_DISABLE,
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = 1ULL << LED_PIN,
      .pull_down_en = 0,
      .pull_up_en = 0,
  };
  gpio_config(&io_conf);
  gpio_set_level(LED_PIN, LED_LIGHT_OFF);
}

void UpdateLed(int64_t boottime_ms) {
  if (boottime_ms - led_updated_time_ > 500000) {
    gpio_set_level(LED_PIN, LED_LIGHT_OFF);
    led_updated_time_ = boottime_ms;
    stop();
  }
}
