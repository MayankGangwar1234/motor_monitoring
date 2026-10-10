#pragma once

// =========================
// Wi-Fi / MQTT
// =========================
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

#define MQTT_HOST "10.137.108.116"
#define MQTT_PORT 1883
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""

#define DEVICE_ID "motor-G23"
#define EXPERIMENT_ID "dc-12v-healthy-01"

// =========================
// NodeMCU ESP-12E pins
// =========================
#define I2C_SDA_PIN D2
#define I2C_SCL_PIN D1

#define DS18B20_PIN D5
#define RPM_SENSOR_PIN D6
#define MOTOR_SWITCH_PIN D7
#define BUZZER_PIN D0

// These pins are reserved for status LEDs.
// Avoid connecting them until boot behavior is verified.
#define GREEN_LED_PIN D3
#define YELLOW_LED_PIN D4
#define RED_LED_PIN D8

#define START_BUTTON_PIN D3
#define STOP_BUTTON_PIN D4

// =========================
// Vibration / RPM
// =========================
#define RPM_PULSES_PER_REVOLUTION 1.0f
#define VIBRATION_SAMPLE_RATE_HZ 256
#define VIBRATION_BLOCK_SIZE 256

// =========================
// Safety / operating options
// =========================
#define ENABLE_PHYSICAL_BUTTONS 0
#define ENABLE_LOCAL_PROTECTION 1

#define LOCAL_CRITICAL_TEMPERATURE_C 60.0f
#define LOCAL_CRITICAL_CURRENT_A 1.20f
#define LOCAL_CRITICAL_VIBRATION_RMS_G 0.45f

// Keep this 0 for real sensors.
#define SIMULATE_SENSORS 0
