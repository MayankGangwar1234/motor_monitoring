#include <Arduino.h>
#include <ArduinoJson.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_SSD1306.h>
#include <esp_timer.h>
#include <time.h>

#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#warning "Using config.example.h placeholders. Copy it to config.h for hardware use."
#endif

static_assert((VIBRATION_BLOCK_SIZE & (VIBRATION_BLOCK_SIZE - 1)) == 0,
              "VIBRATION_BLOCK_SIZE must be a power of two");

constexpr char TOPIC_TELEMETRY[] = "motor/G23/telemetry";
constexpr char TOPIC_VIBRATION_RAW[] = "motor/G23/vibration/raw";
constexpr char TOPIC_AVAILABILITY[] = "motor/G23/availability";
constexpr char TOPIC_COMMAND[] = "motor/G23/command";
constexpr float STANDARD_GRAVITY = 9.80665f;

struct VibrationBlock {
  float samples[VIBRATION_BLOCK_SIZE];
  uint32_t sequence;
  float rmsG;
};

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Adafruit_MPU6050 mpu;
Adafruit_INA219 ina219;
Adafruit_SSD1306 display(128, 64, &Wire, -1);
OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureBus(&oneWire);
QueueHandle_t vibrationQueue;

volatile uint32_t rpmPulseCount = 0;
portMUX_TYPE rpmMux = portMUX_INITIALIZER_UNLOCKED;
bool mpuReady = false;
bool ina219Ready = false;
bool displayReady = false;
bool temperatureReady = false;
bool motorEnabled = false;
bool tripLatched = false;
bool previousStartPressed = false;
float temperatureC = 0;
float currentA = 0;
float voltageV = 0;
float powerW = 0;
float rpm = 0;
float vibrationRmsG = 0;
String tripReason = "none";
uint32_t telemetrySequence = 0;
uint32_t vibrationSequence = 0;
uint32_t lastTelemetryMs = 0;
uint32_t lastElectricalMs = 0;
uint32_t lastRpmMs = 0;
uint32_t lastTemperatureRequestMs = 0;
uint32_t temperatureRequestedAtMs = 0;
bool temperatureConversionPending = false;

void IRAM_ATTR onRpmPulse() {
  portENTER_CRITICAL_ISR(&rpmMux);
  rpmPulseCount += 1;
  portEXIT_CRITICAL_ISR(&rpmMux);
}

String isoTimestamp() {
  time_t now;
  time(&now);
  struct tm utc;
  gmtime_r(&now, &utc);
  char output[25];
  if (now < 1700000000) {
    snprintf(output, sizeof(output), "1970-01-01T00:%02lu:%02luZ",
             (millis() / 60000UL) % 60UL, (millis() / 1000UL) % 60UL);
  } else {
    strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &utc);
  }
  return String(output);
}

bool stopInputHealthy() {
#if ENABLE_PHYSICAL_BUTTONS
  return digitalRead(STOP_BUTTON_PIN) == LOW;  // NC contact: LOW is healthy
#else
  return true;
#endif
}

bool requiredSensorsReady() {
#if SIMULATE_SENSORS
  return true;
#else
  return mpuReady && ina219Ready && temperatureReady;
#endif
}

bool readingsAreLocallySafe() {
  return requiredSensorsReady() && stopInputHealthy() &&
         temperatureC < LOCAL_CRITICAL_TEMPERATURE_C &&
         currentA < LOCAL_CRITICAL_CURRENT_A &&
         vibrationRmsG < LOCAL_CRITICAL_VIBRATION_RMS_G;
}

void writeMotorOutput(bool enabled) {
  motorEnabled = enabled;
  digitalWrite(MOTOR_SWITCH_PIN, enabled ? HIGH : LOW);  // FR120N input is active HIGH
}

void tripMotor(const char *reason) {
  writeMotorOutput(false);
  tripLatched = true;
  tripReason = reason;
  Serial.printf("[safety] TRIPPED reason=%s\n", reason);
}

void clearTrip(const char *reason) {
  if (!readingsAreLocallySafe()) {
    Serial.println("[safety] reset rejected: unsafe reading, missing sensor, or STOP open");
    return;
  }
  writeMotorOutput(false);
  tripLatched = false;
  tripReason = "none";
  Serial.printf("[safety] READY reason=%s\n", reason);
}

void startMotor(const char *reason) {
  if (tripLatched || !readingsAreLocallySafe()) {
    Serial.println("[safety] start rejected: reset trip and verify sensors/STOP first");
    return;
  }
  writeMotorOutput(true);
  Serial.printf("[motor] ON reason=%s\n", reason);
}

void updateLocalOutputs() {
  digitalWrite(GREEN_LED_PIN, motorEnabled && !tripLatched ? HIGH : LOW);
  digitalWrite(YELLOW_LED_PIN, !motorEnabled && !tripLatched ? HIGH : LOW);
  digitalWrite(RED_LED_PIN, tripLatched ? HIGH : LOW);
  const bool buzzerOn = tripLatched && ((millis() / 250UL) % 2UL == 0);
  digitalWrite(BUZZER_PIN, buzzerOn ? HIGH : LOW);
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  if (String(topic) != TOPIC_COMMAND || length > 256) return;
  JsonDocument command;
  if (deserializeJson(command, payload, length)) return;
  const String action = command["command"] | "";
  if (action == "START") startMotor("remote START");
  else if (action == "STOP") tripMotor("remote protected STOP");
  else if (action == "RESET") clearTrip("remote RESET accepted");
}

void handlePhysicalButtons() {
#if ENABLE_PHYSICAL_BUTTONS
  if (!stopInputHealthy()) {
    if (!tripLatched) tripMotor("physical STOP or open STOP wire");
    previousStartPressed = false;
    return;
  }
  const bool startPressed = digitalRead(START_BUTTON_PIN) == LOW;
  if (startPressed && !previousStartPressed) startMotor("physical START");
  previousStartPressed = startPressed;
#endif
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[wifi] connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
    delay(250);
    digitalWrite(YELLOW_LED_PIN, !digitalRead(YELLOW_LED_PIN));
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected, IP=%s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  } else Serial.println("[wifi] connection timed out; retrying in loop");
}

void publishAvailability(const char *state) {
  JsonDocument document;
  document["deviceId"] = DEVICE_ID;
  document["state"] = state;
  document["timestamp"] = isoTimestamp();
  String payload;
  serializeJson(document, payload);
  mqttClient.publish(TOPIC_AVAILABILITY, payload.c_str(), true);
}

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;
  const String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  JsonDocument willDocument;
  willDocument["deviceId"] = DEVICE_ID;
  willDocument["state"] = "offline";
  String willPayload;
  serializeJson(willDocument, willPayload);
  const bool connected = mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD,
    TOPIC_AVAILABILITY, 1, true, willPayload.c_str());
  if (connected) {
    mqttClient.subscribe(TOPIC_COMMAND, 1);
    publishAvailability("online");
    Serial.println("[mqtt] connected");
  } else Serial.printf("[mqtt] failed rc=%d\n", mqttClient.state());
}

float simulatedVibrationSample(uint32_t index) {
  const float seconds = static_cast<float>(index) / VIBRATION_SAMPLE_RATE_HZ;
  return 0.03f * sinf(2.0f * PI * 5.0f * seconds) +
         0.008f * sinf(2.0f * PI * 10.0f * seconds);
}

void vibrationSamplingTask(void *) {
  VibrationBlock block{};
  int64_t nextSampleUs = esp_timer_get_time();
  const int64_t periodUs = 1000000LL / VIBRATION_SAMPLE_RATE_HZ;
  while (true) {
    float sumSquares = 0;
    for (uint32_t index = 0; index < VIBRATION_BLOCK_SIZE; index += 1) {
      float accelerationG = 0;
#if SIMULATE_SENSORS
      accelerationG = simulatedVibrationSample(vibrationSequence * VIBRATION_BLOCK_SIZE + index);
#else
      if (mpuReady) {
        sensors_event_t acceleration, gyroscope, sensorTemperature;
        mpu.getEvent(&acceleration, &gyroscope, &sensorTemperature);
        const float x = acceleration.acceleration.x / STANDARD_GRAVITY;
        const float y = acceleration.acceleration.y / STANDARD_GRAVITY;
        const float z = acceleration.acceleration.z / STANDARD_GRAVITY;
        accelerationG = sqrtf(x * x + y * y + z * z) - 1.0f;
      }
#endif
      block.samples[index] = accelerationG;
      sumSquares += accelerationG * accelerationG;
      nextSampleUs += periodUs;
      while (esp_timer_get_time() < nextSampleUs) {
        const int64_t remaining = nextSampleUs - esp_timer_get_time();
        if (remaining > 1200) vTaskDelay(1);
        else if (remaining > 30) delayMicroseconds(static_cast<uint32_t>(remaining - 20));
      }
    }
    block.sequence = vibrationSequence++;
    block.rmsG = sqrtf(sumSquares / VIBRATION_BLOCK_SIZE);
    xQueueSend(vibrationQueue, &block, 0);
  }
}

void readSlowSensors() {
  const uint32_t now = millis();
  if (!temperatureConversionPending && now - lastTemperatureRequestMs >= 1000) {
    temperatureBus.requestTemperatures();
    temperatureConversionPending = true;
    temperatureRequestedAtMs = now;
    lastTemperatureRequestMs = now;
  }
  if (temperatureConversionPending && now - temperatureRequestedAtMs >= 200) {
    const float reading = temperatureBus.getTempCByIndex(0);
    if (reading > -55 && reading < 125) {
      temperatureC = reading;
      temperatureReady = true;
    }
    temperatureConversionPending = false;
  }

  if (now - lastElectricalMs >= 1000) {
#if SIMULATE_SENSORS
    temperatureC = 34.0f + 1.0f * sinf(now / 9000.0f);
    currentA = 0.18f + 0.015f * sinf(now / 3000.0f);
    voltageV = 12.0f + 0.08f * sinf(now / 4000.0f);
    powerW = voltageV * currentA;
    temperatureReady = true;
#else
    if (ina219Ready) {
      voltageV = ina219.getBusVoltage_V();
      currentA = fabsf(ina219.getCurrent_mA() / 1000.0f);
      powerW = fabsf(ina219.getPower_mW() / 1000.0f);
    }
#endif
    lastElectricalMs = now;
  }

  if (now - lastRpmMs >= 1000) {
    portENTER_CRITICAL(&rpmMux);
    const uint32_t pulses = rpmPulseCount;
    rpmPulseCount = 0;
    portEXIT_CRITICAL(&rpmMux);
    const float elapsedMinutes = (now - lastRpmMs) / 60000.0f;
    rpm = elapsedMinutes > 0 ? pulses / RPM_PULSES_PER_REVOLUTION / elapsedMinutes : 0;
#if SIMULATE_SENSORS
    rpm = motorEnabled ? 300.0f + 3.0f * sinf(now / 5000.0f) : 0.0f;
#endif
    lastRpmMs = now;
  }
}

void publishVibration(const VibrationBlock &block) {
  if (!mqttClient.connected()) return;
  JsonDocument document;
  document["deviceId"] = DEVICE_ID;
  document["timestamp"] = isoTimestamp();
  document["sequence"] = block.sequence;
  document["sampleRateHz"] = VIBRATION_SAMPLE_RATE_HZ;
  document["rpm"] = rpm;
  JsonArray samples = document["samples"].to<JsonArray>();
  for (float sample : block.samples) samples.add(sample);
  String payload;
  payload.reserve(5000);
  serializeJson(document, payload);
  if (!mqttClient.publish(TOPIC_VIBRATION_RAW, payload.c_str())) {
    Serial.printf("[mqtt] vibration publish failed (%u bytes)\n", payload.length());
  }
}

void publishTelemetry() {
  if (!mqttClient.connected()) return;
  JsonDocument document;
  document["deviceId"] = DEVICE_ID;
  document["experimentId"] = EXPERIMENT_ID;
  document["timestamp"] = isoTimestamp();
  document["sequence"] = telemetrySequence++;
  document["temperatureC"] = roundf(temperatureC * 100.0f) / 100.0f;
  document["currentA"] = roundf(currentA * 1000.0f) / 1000.0f;
  document["voltageV"] = roundf(voltageV * 100.0f) / 100.0f;
  document["powerW"] = roundf(powerW * 100.0f) / 100.0f;
  document["rpm"] = roundf(rpm * 10.0f) / 10.0f;
  document["vibrationRmsG"] = roundf(vibrationRmsG * 100000.0f) / 100000.0f;
  document["motorEnabled"] = motorEnabled;
  document["tripLatched"] = tripLatched;
  document["tripReason"] = tripReason;
  String payload;
  serializeJson(document, payload);
  mqttClient.publish(TOPIC_TELEMETRY, payload.c_str());
}

void updateDisplay() {
  if (!displayReady) return;
  display.clearDisplay();
  display.setCursor(0, 0);
  display.printf("12V MOTOR  %s\n", mqttClient.connected() ? "ONLINE" : "OFFLINE");
  display.printf("Temp: %5.1f C\n", temperatureC);
  display.printf("I/V: %.2fA %.1fV\n", currentA, voltageV);
  display.printf("RPM:  %5.0f\n", rpm);
  display.printf("Vib:  %.3f g\n", vibrationRmsG);
  display.printf("State: %s", tripLatched ? "TRIPPED" : (motorEnabled ? "RUN" : "READY"));
  display.display();
}

void setup() {
  Serial.begin(115200);
  pinMode(MOTOR_SWITCH_PIN, OUTPUT);
  writeMotorOutput(false);  // safe state before all network/sensor initialization
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(RPM_SENSOR_PIN, INPUT_PULLUP);
#if ENABLE_PHYSICAL_BUTTONS
  pinMode(START_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STOP_BUTTON_PIN, INPUT_PULLUP);
#endif
  updateLocalOutputs();
  attachInterrupt(digitalPinToInterrupt(RPM_SENSOR_PIN), onRpmPulse, FALLING);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);
  mpuReady = mpu.begin();
  if (mpuReady) {
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setFilterBandwidth(MPU6050_BAND_184_HZ);
    Serial.println("[sensor] MPU6050 ready at 0x68");
  } else Serial.println("[sensor] MPU6050 not detected");
  ina219Ready = ina219.begin();
  if (ina219Ready) {
    ina219.setCalibration_32V_2A();
    Serial.println("[sensor] INA219 ready at 0x40");
  } else Serial.println("[sensor] INA219 not detected");
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (displayReady) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
  }
  temperatureBus.begin();
  temperatureBus.setResolution(10);
  temperatureBus.setWaitForConversion(false);

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(12288);
  vibrationQueue = xQueueCreate(2, sizeof(VibrationBlock));
  xTaskCreatePinnedToCore(vibrationSamplingTask, "vibration", 4096, nullptr, 3, nullptr, 1);
  connectWiFi();
  connectMqtt();
  lastRpmMs = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqttClient.connected()) connectMqtt();
  mqttClient.loop();
  readSlowSensors();
  handlePhysicalButtons();

  VibrationBlock block;
  while (xQueueReceive(vibrationQueue, &block, 0) == pdTRUE) {
    vibrationRmsG = block.rmsG;
    publishVibration(block);
  }

#if ENABLE_LOCAL_PROTECTION
  if (motorEnabled && !readingsAreLocallySafe()) tripMotor("local critical threshold or sensor fault");
#endif

  if (millis() - lastTelemetryMs >= 2000) {
    publishTelemetry();
    updateDisplay();
    lastTelemetryMs = millis();
  }
  updateLocalOutputs();
  delay(2);
}
