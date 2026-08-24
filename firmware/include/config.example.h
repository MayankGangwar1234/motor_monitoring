#pragma once

// Copy this file to config.h and replace the network values. config.h is
// ignored by Git so credentials are never committed. These placeholders are
// used by CI for compile-only verification.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define MQTT_HOST "192.168.1.10"
#define MQTT_PORT 1883
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""

#define DEVICE_ID "motor-G23"
#define EXPERIMENT_ID "dc-12v-healthy-01"

// ESP32 DevKit V1 pin assignment for the finalized 12 V DC motor model.
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define DS18B20_PIN 18
#define RPM_SENSOR_PIN 27
#define MOTOR_SWITCH_PIN 23
#define BUZZER_PIN 25
#define GREEN_LED_PIN 32
#define YELLOW_LED_PIN 33
#define RED_LED_PIN 26
#define START_BUTTON_PIN 13
#define STOP_BUTTON_PIN 14

#define RPM_PULSES_PER_REVOLUTION 1.0f
#define VIBRATION_SAMPLE_RATE_HZ 256
#define VIBRATION_BLOCK_SIZE 256

// The optional green NO START and red NC STOP buttons are disabled unless
// they are physically installed exactly as documented.
#define ENABLE_PHYSICAL_BUTTONS 0

// Short demonstration defaults for the selected N20 model. Calibrate against
// healthy readings after assembly. The 2 A fuse and latching E-stop remain
// independent physical protection.
#define ENABLE_LOCAL_PROTECTION 1
#define LOCAL_CRITICAL_TEMPERATURE_C 60.0f
#define LOCAL_CRITICAL_CURRENT_A 1.20f
#define LOCAL_CRITICAL_VIBRATION_RMS_G 0.45f

// Set to 1 for a complete no-hardware MQTT demonstration.
#define SIMULATE_SENSORS 0
