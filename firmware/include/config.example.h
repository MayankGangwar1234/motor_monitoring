#pragma once

// Copy this file to config.h and replace the network values. config.h is
// ignored by Git so credentials are never committed. The firmware falls back
// to these placeholders for compile-only CI verification.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define MQTT_HOST "192.168.1.10"
#define MQTT_PORT 1883
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""

#define DEVICE_ID "motor-G23"
#define EXPERIMENT_ID "healthy-load-01"

// ESP32 DevKit V1 pin assignment.
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define DS18B20_PIN 4
#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17
#define RPM_SENSOR_PIN 27
#define RELAY_PIN 26
#define BUZZER_PIN 25
#define STATUS_LED_PIN 2

#define RPM_PULSES_PER_REVOLUTION 1.0f
#define VIBRATION_SAMPLE_RATE_HZ 512
#define VIBRATION_BLOCK_SIZE 256

// Protection values are placeholders, not universal motor limits. Confirm the
// motor nameplate and obtain supervisor approval before enabling protection.
#define ENABLE_LOCAL_PROTECTION 0
#define LOCAL_CRITICAL_TEMPERATURE_C 90.0f
#define LOCAL_CRITICAL_CURRENT_A 5.0f
#define LOCAL_CRITICAL_VIBRATION_RMS_G 0.70f

// Set to 1 for a complete no-hardware demo through the real MQTT path.
#define SIMULATE_SENSORS 0

