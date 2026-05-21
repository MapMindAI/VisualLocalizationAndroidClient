#pragma once

#include "config.h"
#include "esp_task_wdt.h"

#define BATTERY_ADC_PIN GPIO_NUM_14


// back right
#define MOTOR_D_BIN2_PIN GPIO_NUM_18
#define MOTOR_D_BIN1_PIN GPIO_NUM_19

// back left
#define MOTOR_C_BIN2_PIN GPIO_NUM_23
#define MOTOR_C_BIN1_PIN GPIO_NUM_22

// front right
#define MOTOR_B_BIN2_PIN GPIO_NUM_26
#define MOTOR_B_BIN1_PIN GPIO_NUM_25

// front left
#define MOTOR_A_BIN2_PIN GPIO_NUM_33
#define MOTOR_A_BIN1_PIN GPIO_NUM_32

#define MOTOR_PWM_PIN GPIO_NUM_27

void SetUpLed();
void UpdateLed(int64_t boottime_ms);

// param->write.value, param->write.len
void ControlMessageCallback(int len, const uint8_t* value);
void InitializeMotor();
void ControlSingleMotor(char motor_id, int dir, int speed);
void StopAllMotors();
