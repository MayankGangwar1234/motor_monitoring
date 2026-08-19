#include <Arduino.h>
#include <ArduinoJson.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <PZEM004Tv30.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Wire.h>
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
Adafruit_SSD1306 display(128, 64, &Wire, -1);
OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureBus(&oneWire);
HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, PZEM_RX_PIN, PZEM_TX_PIN);
QueueHandle_t vibrationQueue;

volatile uint32_t rpmPulseCount = 0;
portMUX_TYPE rpmMux = portMUX_INITIALIZER_UNLOCKED;
bool mpuReady = false;
bool displayReady = false;
bool relayTripped = false;
float temperatureC = 0;
float currentA = 0;
float voltageV = 0;
float rpm = 0;
float vibrationRmsG = 0;
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

void setRelay(bool trip, const char *reason) {
  relayTripped = trip;
  digitalWrite(RELAY_PIN, trip ? LOW : HIGH);  // active-low isolated relay module
  digitalWrite(BUZZER_PIN, trip ? HIGH : LOW);
  Serial.printf("[safety] relay=%s reason=%s\n", trip ? "TRIPPED" : "READY", reason);
}

bool readingsAreLocallySafe() {
  return temperatureC < LOCAL_CRITICAL_TEMPERATURE_C &&
         currentA < LOCAL_CRITICAL_CURRENT_A &&
         vibrationRmsG < LOCAL_CRITICAL_VIBRATION_RMS_G;
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  if (String(topic) != TOPIC_COMMAND || length > 256) return;
  JsonDocument command;
  if (deserializeJson(command, payload, length)) return;
  const String action = command["command"] | "";
  if (action == "STOP") setRelay(true, "remote protected stop");
  else if (action == "RESET") {
    if (readingsAreLocallySafe()) setRelay(false, "safe reset accepted");
    else Serial.println("[safety] reset rejected: critical local reading remains");
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[wifi] connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
    delay(250);
    digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
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
    Serial.println("[mqtt] connected");
    mqttClient.subscribe(TOPIC_COMMAND, 1);
    publishAvailability("online");
  } else Serial.printf("[mqtt] failed rc=%d\n", mqttClient.state());
}

float simulatedVibrationSample(uint32_t index) {
  const float seconds = static_cast<float>(index) / VIBRATION_SAMPLE_RATE_HZ;
  return 0.08f * sinf(2.0f * PI * 24.0f * seconds) +
         0.018f * sinf(2.0f * PI * 48.0f * seconds);
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
    xQueueSend(vibrationQueue, &block, 0);  // drop under pressure; never block the sampler
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
    if (reading > -55 && reading < 150) temperatureC = reading;
    temperatureConversionPending = false;
  }

  if (now - lastElectricalMs >= 2000) {
#if SIMULATE_SENSORS
    temperatureC = 45.0f + 2.0f * sinf(now / 9000.0f);
    currentA = 2.1f + 0.05f * sinf(now / 3000.0f);
    voltageV = 230.0f + sinf(now / 4000.0f);
#else
    const float measuredCurrent = pzem.current();
    const float measuredVoltage = pzem.voltage();
    if (!isnan(measuredCurrent)) currentA = measuredCurrent;
    if (!isnan(measuredVoltage)) voltageV = measuredVoltage;
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
    rpm = 1440.0f + 5.0f * sinf(now / 5000.0f);
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
  document["rpm"] = roundf(rpm * 10.0f) / 10.0f;
  document["vibrationRmsG"] = roundf(vibrationRmsG * 100000.0f) / 100000.0f;
  document["relayTripped"] = relayTripped;
  String payload;
  serializeJson(document, payload);
  mqttClient.publish(TOPIC_TELEMETRY, payload.c_str());
}

void updateDisplay() {
  if (!displayReady) return;
  display.clearDisplay();
  display.setCursor(0, 0);
  display.printf("MOTOR G23  %s\n", mqttClient.connected() ? "ONLINE" : "OFFLINE");
  display.printf("Temp: %5.1f C\n", temperatureC);
  display.printf("I/V: %4.2fA %3.0fV\n", currentA, voltageV);
  display.printf("RPM:  %5.0f\n", rpm);
  display.printf("Vib:  %.3f g\n", vibrationRmsG);
  display.printf("Relay: %s", relayTripped ? "TRIPPED" : "READY");
  display.display();
}

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(RPM_SENSOR_PIN, INPUT_PULLUP);
  setRelay(false, "power-on initialization");
  attachInterrupt(digitalPinToInterrupt(RPM_SENSOR_PIN), onRpmPulse, FALLING);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);
  mpuReady = mpu.begin();
  if (mpuReady) {
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setFilterBandwidth(MPU6050_BAND_184_HZ);
    Serial.println("[sensor] MPU6050 ready");
  } else Serial.println("[sensor] MPU6050 not detected");
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

  VibrationBlock block;
  while (xQueueReceive(vibrationQueue, &block, 0) == pdTRUE) {
    vibrationRmsG = block.rmsG;
    publishVibration(block);
  }

#if ENABLE_LOCAL_PROTECTION
  if (!relayTripped && !readingsAreLocallySafe()) setRelay(true, "local critical threshold");
#endif

  if (millis() - lastTelemetryMs >= 2000) {
    publishTelemetry();
    updateDisplay();
    lastTelemetryMs = millis();
  }
  digitalWrite(STATUS_LED_PIN, mqttClient.connected() ? HIGH : (millis() / 300) % 2);
  delay(2);
}
