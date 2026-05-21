#pragma once

#include "config.h"
#include "esp_task_wdt.h"

#define BATTERY_ADC_PIN GPIO_NUM_14

#define MOTOR_B_PWMA_PIN GPIO_NUM_19
#define MOTOR_B_BIN2_PIN GPIO_NUM_18
#define MOTOR_B_BIN1_PIN GPIO_NUM_32

#define MOTOR_STBY_PIN GPIO_NUM_33

#define MOTOR_A_BIN1_PIN GPIO_NUM_25
#define MOTOR_A_BIN2_PIN GPIO_NUM_26
#define MOTOR_A_PWMA_PIN GPIO_NUM_27

// not working pins : 21
#define UP_SERVO_PIN GPIO_NUM_22
#define DOWN_SERVO_PIN GPIO_NUM_23
#define SERVO_FREQ 100
#define SERVO_RESOLUTION LEDC_TIMER_16_BIT


void SetUpLed();
void UpdateLed(int64_t boottime_ms);

// param->write.value, param->write.len
void ControlMessageCallback(int len, const uint8_t* value);
void CansbusControlMessageCallback(int len, const uint8_t* value);
void InitializeMotor();
void InitCameraServo();

void SetLeftRightAngle(float angle_deg);
void SetUpDownAngle(float angle_deg);





#define MOTOR_A_BIN2_PIN GPIO_NUM_18
#define MOTOR_A_BIN1_PIN GPIO_NUM_19


#define MOTOR_B_BIN2_PIN GPIO_NUM_22
#define MOTOR_B_BIN1_PIN GPIO_NUM_23


#define MOTOR_C_BIN2_PIN GPIO_NUM_25
#define MOTOR_C_BIN1_PIN GPIO_NUM_26


#define MOTOR_D_BIN2_PIN GPIO_NUM_32
#define MOTOR_D_BIN1_PIN GPIO_NUM_33
