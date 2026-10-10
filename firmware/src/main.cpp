
#include <Arduino.h>
#include <ArduinoJson.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_INA219.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <math.h>

#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#warning "Using config.example.h. Create config.h with your actual settings."
#endif

static_assert((VIBRATION_BLOCK_SIZE & (VIBRATION_BLOCK_SIZE - 1)) == 0,
              "VIBRATION_BLOCK_SIZE must be a power of two");

constexpr char TOPIC_TELEMETRY[] = "motor/G23/telemetry";
constexpr char TOPIC_VIBRATION_RAW[] = "motor/G23/vibration/raw";
constexpr char TOPIC_AVAILABILITY[] = "motor/G23/availability";
constexpr char TOPIC_COMMAND[] = "motor/G23/command";

constexpr float STANDARD_GRAVITY = 9.80665f;
constexpr float MAX_VALID_CURRENT_A = 2.0f;
constexpr float MAX_VALID_VOLTAGE_V = 16.0f;
constexpr uint32_t STARTUP_DELAY_MS = 5000;

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

volatile uint32_t rpmPulseCount = 0;

bool mpuReady = false;
bool ina219Ready = false;
bool displayReady = false;
bool temperatureReady = false;
bool vibrationReady = false;
bool electricalReady = false;
bool relayTripped = true;
bool temperatureConversionPending = false;

float temperatureC = NAN;
float currentA = NAN;
float voltageV = NAN;
float powerW = NAN;
float rpm = 0.0f;
float vibrationRmsG = NAN;

uint32_t telemetrySequence = 0;
uint32_t vibrationSequence = 0;
uint32_t lastTelemetryMs = 0;
uint32_t lastElectricalMs = 0;
uint32_t lastRpmMs = 0;
uint32_t lastTemperatureRequestMs = 0;
uint32_t temperatureRequestedAtMs = 0;
uint32_t protectionStartMs = 0;

// ------------------------------------------------------------
// RPM interrupt
// ------------------------------------------------------------

void IRAM_ATTR onRpmPulse() {
  rpmPulseCount++;
}

// ------------------------------------------------------------
// Timestamp
// ------------------------------------------------------------

String isoTimestamp() {
  time_t now;
  time(&now);

  char output[25];

  if (now < 1700000000) {
    snprintf(output, sizeof(output), "1970-01-01T00:%02lu:%02luZ",
             (millis() / 60000UL) % 60UL,
             (millis() / 1000UL) % 60UL);
  } else {
    struct tm utc;
    gmtime_r(&now, &utc);
    strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &utc);
  }

  return String(output);
}

// ------------------------------------------------------------
// Motor output and local safety
// ------------------------------------------------------------

void setRelay(bool trip, const char *reason) {
  relayTripped = trip;

  // Confirm actual driver polarity before connecting motor power.
  // This retains the output behavior in the supplied firmware.
  digitalWrite(MOTOR_SWITCH_PIN, trip ? LOW : HIGH);

  digitalWrite(BUZZER_PIN, trip ? HIGH : LOW);

  Serial.printf("[safety] relay=%s reason=%s\n",
                trip ? "TRIPPED" : "READY", reason);
}

// All sensors must be valid for complete telemetry status.
bool sensorReadingsValid() {
  return mpuReady &&
         ina219Ready &&
         temperatureReady &&
         vibrationReady &&
         electricalReady &&
         isfinite(temperatureC) &&
         isfinite(currentA) &&
         isfinite(voltageV) &&
         isfinite(vibrationRmsG);
}

// Protection validity includes temperature.
bool protectionSensorsValid() {
  return mpuReady &&
         ina219Ready &&
         temperatureReady &&
         vibrationReady &&
         electricalReady &&
         isfinite(temperatureC) &&
         isfinite(currentA) &&
         isfinite(voltageV) &&
         isfinite(vibrationRmsG);
}

bool readingsAreLocallySafe() {
  if (!protectionSensorsValid()) {
    return false;
  }

  if (temperatureC < -55.0f || temperatureC > 125.0f) {
    return false;
  }

  if (fabsf(currentA) > MAX_VALID_CURRENT_A) {
    return false;
  }

  if (voltageV < 0.0f || voltageV > MAX_VALID_VOLTAGE_V) {
    return false;
  }

  if (vibrationRmsG < 0.0f || vibrationRmsG > 4.0f) {
    return false;
  }

  return temperatureC < LOCAL_CRITICAL_TEMPERATURE_C &&
         fabsf(currentA) < LOCAL_CRITICAL_CURRENT_A &&
         vibrationRmsG < LOCAL_CRITICAL_VIBRATION_RMS_G;
}

void printDebugReadings() {
  Serial.println("------ SENSOR DEBUG ------");

  Serial.printf("MPU6050: %s\n", mpuReady ? "OK" : "FAULT");
  Serial.printf("INA219: %s\n", ina219Ready ? "OK" : "FAULT");
  Serial.printf("DS18B20: %s\n", temperatureReady ? "OK" : "FAULT");
  Serial.printf("Vibration: %s\n", vibrationReady ? "OK" : "FAULT");
  Serial.printf("Electrical: %s\n", electricalReady ? "OK" : "FAULT");

  Serial.printf("Temperature: %.2f C\n", temperatureC);
  Serial.printf("Current: %.3f A\n", currentA);
  Serial.printf("Voltage: %.2f V\n", voltageV);
  Serial.printf("Power: %.2f W\n", powerW);
  Serial.printf("RPM: %.1f\n", rpm);
  Serial.printf("Vibration RMS: %.4f g\n", vibrationRmsG);

  Serial.printf("All readings valid: %s\n",
                sensorReadingsValid() ? "YES" : "NO");
  Serial.printf("Locally safe: %s\n",
                readingsAreLocallySafe() ? "YES" : "NO");

  if (millis() - protectionStartMs < STARTUP_DELAY_MS) {
    Serial.println("Protection: STARTUP DELAY");
  } else {
    Serial.println("Protection: ACTIVE");
  }

  Serial.println("--------------------------");
}

// ------------------------------------------------------------
// MQTT command callback
// ------------------------------------------------------------

void printResetSafetyDiagnostics() {
  Serial.println("[safety] Reset safety check:");

  if (!mpuReady) Serial.println(" - MPU6050 not ready");
  if (!ina219Ready) Serial.println(" - INA219 not ready");
  if (!temperatureReady || !isfinite(temperatureC))
    Serial.println(" - Temperature reading invalid");
  if (!vibrationReady || !isfinite(vibrationRmsG))
    Serial.println(" - Vibration reading invalid");
  if (!electricalReady || !isfinite(currentA) || !isfinite(voltageV))
    Serial.println(" - Electrical reading invalid");

  if (isfinite(temperatureC) &&
      (temperatureC < -55.0f || temperatureC > 125.0f))
    Serial.println(" - Temperature outside valid range");

  if (isfinite(temperatureC) &&
      temperatureC >= LOCAL_CRITICAL_TEMPERATURE_C)
    Serial.println(" - Temperature still at/above critical limit");

  if (isfinite(currentA) && fabsf(currentA) > MAX_VALID_CURRENT_A)
    Serial.println(" - Current outside valid range");

  if (isfinite(currentA) &&
      fabsf(currentA) >= LOCAL_CRITICAL_CURRENT_A)
    Serial.println(" - Current still at/above critical limit");

  if (isfinite(voltageV) &&
      (voltageV < 0.0f || voltageV > MAX_VALID_VOLTAGE_V))
    Serial.println(" - Voltage outside valid range");

  if (isfinite(vibrationRmsG) &&
      (vibrationRmsG < 0.0f || vibrationRmsG > 4.0f))
    Serial.println(" - Vibration outside valid range");

  if (isfinite(vibrationRmsG) &&
      vibrationRmsG >= LOCAL_CRITICAL_VIBRATION_RMS_G)
    Serial.println(" - Vibration still at/above critical limit");

  Serial.printf(
    "[safety] Temp=%.2f C, Current=%.3f A, Voltage=%.2f V, Vib=%.4f g\n",
    temperatureC, currentA, voltageV, vibrationRmsG
  );
}
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  if (String(topic) != TOPIC_COMMAND || length > 256) {
    return;
  }

  JsonDocument command;

  if (deserializeJson(command, payload, length)) {
    Serial.println("[mqtt] invalid command JSON");
    return;
  }

  const String action = command["command"] | "";

  if (action == "STOP") {
    setRelay(true, "remote protected stop");
  } else if (action == "RESET") {
    if (millis() - protectionStartMs < STARTUP_DELAY_MS) {
      Serial.println("[safety] reset rejected: startup delay");
    } else if (readingsAreLocallySafe()) {
      setRelay(false, "safe reset accepted");
  } else {
  Serial.println("[safety] RESET rejected: local readings unsafe");
  printResetSafetyDiagnostics();
}
  }
}

// ------------------------------------------------------------
// Wi-Fi
// ------------------------------------------------------------

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.printf("[wifi] connecting to %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t started = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < 15000) {
    delay(250);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected, IP=%s\n",
                  WiFi.localIP().toString().c_str());

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println("[wifi] connection timed out");
  }
}

// ------------------------------------------------------------
// MQTT availability and connection
// ------------------------------------------------------------

void publishAvailability(const char *state) {
  if (!mqttClient.connected()) {
    return;
  }

  JsonDocument document;
  document["deviceId"] = DEVICE_ID;
  document["state"] = state;
  document["timestamp"] = isoTimestamp();

  String payload;
  serializeJson(document, payload);

  mqttClient.publish(TOPIC_AVAILABILITY, payload.c_str(), true);
}

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) {
    return;
  }

  const String clientId =
      String(DEVICE_ID) + "-" +
      String((uint32_t)ESP.getChipId(), HEX);

  JsonDocument willDocument;
  willDocument["deviceId"] = DEVICE_ID;
  willDocument["state"] = "offline";

  String willPayload;
  serializeJson(willDocument, willPayload);

  const bool connected = mqttClient.connect(
      clientId.c_str(),
      MQTT_USERNAME,
      MQTT_PASSWORD,
      TOPIC_AVAILABILITY,
      1,
      true,
      willPayload.c_str()
  );

  if (connected) {
    Serial.println("[mqtt] connected");
    mqttClient.subscribe(TOPIC_COMMAND, 1);
    publishAvailability("online");
  } else {
    Serial.printf("[mqtt] failed rc=%d\n", mqttClient.state());
  }
}

// ------------------------------------------------------------
// Optional simulated vibration sample
// ------------------------------------------------------------

float simulatedVibrationSample(uint32_t index) {
  const float seconds =
      static_cast<float>(index) / VIBRATION_SAMPLE_RATE_HZ;

  return 0.08f * sinf(2.0f * PI * 24.0f * seconds) +
         0.018f * sinf(2.0f * PI * 48.0f * seconds);
}

// ------------------------------------------------------------
// Vibration sampling
// ------------------------------------------------------------

void publishVibration(const VibrationBlock &block);

void sampleVibrationBlock() {
  VibrationBlock block{};
  float sumSquares = 0.0f;
  bool blockValid = true;

  for (uint32_t index = 0; index < VIBRATION_BLOCK_SIZE; ++index) {
    float accelerationG = 0.0f;

#if SIMULATE_SENSORS
    accelerationG = simulatedVibrationSample(
        vibrationSequence * VIBRATION_BLOCK_SIZE + index
    );
#else
    if (mpuReady) {
      sensors_event_t acceleration;
      sensors_event_t gyroscope;
      sensors_event_t sensorTemperature;

      mpu.getEvent(&acceleration, &gyroscope, &sensorTemperature);

      const float x = acceleration.acceleration.x / STANDARD_GRAVITY;
      const float y = acceleration.acceleration.y / STANDARD_GRAVITY;
      const float z = acceleration.acceleration.z / STANDARD_GRAVITY;

      // Remove approximate static gravity component.
      accelerationG = sqrtf(x * x + y * y + z * z) - 1.0f;

      if (!isfinite(accelerationG)) {
        blockValid = false;
        accelerationG = 0.0f;
      }
    } else {
      blockValid = false;
    }
#endif

    block.samples[index] = accelerationG;
    sumSquares += accelerationG * accelerationG;

    delayMicroseconds(1000000UL / VIBRATION_SAMPLE_RATE_HZ);
    yield();
  }

  block.sequence = vibrationSequence++;

  if (blockValid && isfinite(sumSquares)) {
    block.rmsG = sqrtf(sumSquares / VIBRATION_BLOCK_SIZE);

    if (isfinite(block.rmsG)) {
      vibrationRmsG = block.rmsG;
      vibrationReady = true;
      publishVibration(block);
    } else {
      vibrationReady = false;
      vibrationRmsG = NAN;
      Serial.println("[sensor] invalid vibration RMS");
    }
  } else {
    vibrationReady = false;
    vibrationRmsG = NAN;
    Serial.println("[sensor] vibration block invalid");
  }

}

// ------------------------------------------------------------
// Temperature, current, voltage and RPM
// ------------------------------------------------------------

void readSlowSensors() {
  const uint32_t now = millis();

  // DS18B20: request conversion, then read later.
  if (!temperatureConversionPending &&
      now - lastTemperatureRequestMs >= 1000) {
    temperatureBus.requestTemperatures();

    temperatureConversionPending = true;
    temperatureRequestedAtMs = now;
    lastTemperatureRequestMs = now;
  }

  if (temperatureConversionPending &&
      now - temperatureRequestedAtMs >= 200) {
    const float reading = temperatureBus.getTempCByIndex(0);

    if (isfinite(reading) &&
        reading != DEVICE_DISCONNECTED_C &&
        reading >= -55.0f &&
        reading <= 125.0f) {
      temperatureC = reading;
      temperatureReady = true;
    } else {
      temperatureC = NAN;
      temperatureReady = false;
      Serial.println("[sensor] DS18B20 invalid or disconnected");
    }

    temperatureConversionPending = false;
  }

  // INA219 readings
  if (now - lastElectricalMs >= 1000) {
#if SIMULATE_SENSORS
    temperatureC = 34.0f + 1.0f * sinf(now / 9000.0f);
    currentA = 0.18f + 0.015f * sinf(now / 3000.0f);
    voltageV = 12.0f + 0.08f * sinf(now / 4000.0f);
    powerW = currentA * voltageV;

    temperatureReady = true;
    electricalReady = true;
#else
    if (ina219Ready) {
      const float measuredCurrent =
          ina219.getCurrent_mA() / 1000.0f;
      const float measuredVoltage =
          ina219.getBusVoltage_V();
      const float measuredShunt =
          ina219.getShuntVoltage_mV();

      const bool validCurrent =
          isfinite(measuredCurrent) &&
          fabsf(measuredCurrent) <= MAX_VALID_CURRENT_A;

      const bool validVoltage =
          isfinite(measuredVoltage) &&
          measuredVoltage >= 0.0f &&
          measuredVoltage <= MAX_VALID_VOLTAGE_V;

      if (validCurrent && validVoltage) {
        currentA = measuredCurrent;
        voltageV = measuredVoltage;
        powerW = currentA * voltageV;
        electricalReady = isfinite(powerW);

        if (!electricalReady) {
          currentA = NAN;
          voltageV = NAN;
          powerW = NAN;
        }
      } else {
        currentA = NAN;
        voltageV = NAN;
        powerW = NAN;
        electricalReady = false;
      }

      static uint32_t lastPrint = 0;

      if (millis() - lastPrint >= 1000) {
        if (electricalReady) {
          Serial.printf(
            "[INA219] Current: %.3f A, Voltage: %.3f V, "
            "Shunt: %.3f mV\n",
            currentA, voltageV, measuredShunt
          );
        } else {
          Serial.printf(
            "[INA219] Invalid reading: I=%.3f A, "
            "V=%.3f V, Shunt=%.3f mV\n",
            measuredCurrent, measuredVoltage, measuredShunt
          );
        }

        lastPrint = millis();
      }
    } else {
      currentA = NAN;
      voltageV = NAN;
      powerW = NAN;
      electricalReady = false;
    }
#endif

    lastElectricalMs = now;
  }

  // RPM calculation using pulses received in the last interval.
  if (now - lastRpmMs >= 1000) {
    noInterrupts();
    const uint32_t pulses = rpmPulseCount;
    rpmPulseCount = 0;
    interrupts();

    const float elapsedMinutes =
        (now - lastRpmMs) / 60000.0f;

    rpm = elapsedMinutes > 0.0f
        ? pulses / RPM_PULSES_PER_REVOLUTION / elapsedMinutes
        : 0.0f;

#if SIMULATE_SENSORS
    rpm = 300.0f + 3.0f * sinf(now / 5000.0f);
#endif

    lastRpmMs = now;
  }
}

// ------------------------------------------------------------
// Publish raw vibration block
// ------------------------------------------------------------

void publishVibration(const VibrationBlock &block) {
  if (!mqttClient.connected()) {
    return;
  }

  JsonDocument document;

  document["deviceId"] = DEVICE_ID;
  document["timestamp"] = isoTimestamp();
  document["sequence"] = block.sequence;
  document["sampleRateHz"] = VIBRATION_SAMPLE_RATE_HZ;
  document["rpm"] = isfinite(rpm) ? rpm : 0.0f;

  JsonArray samples = document["samples"].to<JsonArray>();

  for (float sample : block.samples) {
    samples.add(isfinite(sample) ? sample : 0.0f);
  }

  String payload;
  payload.reserve(5000);
  serializeJson(document, payload);

  if (!mqttClient.publish(TOPIC_VIBRATION_RAW, payload.c_str())) {
    Serial.printf("[mqtt] vibration publish failed (%u bytes)\n",
                  payload.length());
  }
}

// ------------------------------------------------------------
// Publish telemetry
// ------------------------------------------------------------

void publishTelemetry() {
  if (!mqttClient.connected()) {
    return;
  }

  JsonDocument document;

  document["deviceId"] = DEVICE_ID;
  document["experimentId"] = EXPERIMENT_ID;
  document["timestamp"] = isoTimestamp();
  document["sequence"] = telemetrySequence++;

  // Never publish NaN or infinity as telemetry numbers.
  // Validity flags indicate whether a measurement is usable.
  document["temperatureC"] =
      isfinite(temperatureC) ? roundf(temperatureC * 100.0f) / 100.0f : 0.0f;

  document["currentA"] =
      isfinite(currentA) ? roundf(currentA * 1000.0f) / 1000.0f : 0.0f;

  document["voltageV"] =
      isfinite(voltageV) ? roundf(voltageV * 100.0f) / 100.0f : 0.0f;

  document["powerW"] =
      isfinite(powerW) ? roundf(powerW * 100.0f) / 100.0f : 0.0f;

  document["rpm"] =
      isfinite(rpm) ? roundf(rpm * 10.0f) / 10.0f : 0.0f;

  document["vibrationRmsG"] =
      isfinite(vibrationRmsG)
        ? roundf(vibrationRmsG * 100000.0f) / 100000.0f
        : 0.0f;

  document["mpuReady"] = mpuReady;
  document["ina219Ready"] = ina219Ready;
  document["temperatureReady"] = temperatureReady;
  document["electricalReady"] = electricalReady;
  document["vibrationReady"] = vibrationReady;
  document["sensorFault"] = !sensorReadingsValid();
  document["relayTripped"] = relayTripped;

  String payload;
  serializeJson(document, payload);

  if (!mqttClient.publish(TOPIC_TELEMETRY, payload.c_str())) {
    Serial.println("[mqtt] telemetry publish failed");
  }
}

// ------------------------------------------------------------
// OLED display
// ------------------------------------------------------------

void updateDisplay() {
  if (!displayReady) {
    return;
  }

  display.clearDisplay();
  display.setCursor(0, 0);

  display.printf("MOTOR G23 %s\n",
                 mqttClient.connected() ? "ONLINE" : "OFFLINE");

  if (temperatureReady && isfinite(temperatureC)) {
    display.printf("Temp: %5.1f C\n", temperatureC);
  } else {
    display.println("Temp: SENSOR ERR");
  }

  if (electricalReady && isfinite(currentA) && isfinite(voltageV)) {
    display.printf("I/V: %4.2fA %3.1fV\n", currentA, voltageV);
    display.printf("Power: %4.1fW\n", powerW);
  } else {
    display.println("INA219: SENSOR ERR");
    display.println("Power: --");
  }

  display.printf("RPM: %5.0f\n", rpm);

  if (vibrationReady && isfinite(vibrationRmsG)) {
    display.printf("Vib: %.3f g\n", vibrationRmsG);
  } else {
    display.println("Vib: SENSOR ERR");
  }

  display.printf("Relay: %s",
                 relayTripped ? "TRIPPED" : "READY");

  display.display();
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MOTOR_SWITCH_PIN, OUTPUT);
  pinMode(RPM_SENSOR_PIN, INPUT_PULLUP);
pinMode(YELLOW_LED_PIN, OUTPUT);
pinMode(RED_LED_PIN, OUTPUT);

digitalWrite(YELLOW_LED_PIN, LOW);
digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // Startup safety hold. Verify the real driver's OFF polarity.
  setRelay(true, "startup safety hold");

  attachInterrupt(
      digitalPinToInterrupt(RPM_SENSOR_PIN),
      onRpmPulse,
      FALLING
  );

  Serial.println();
  Serial.println("[system] ESP-12E motor monitor starting");
  Serial.println("[sensor] TCRT5000 RPM input initialized");

  // I2C bus: D2 = SDA, D1 = SCL
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  // MPU6050
  mpuReady = mpu.begin();

  if (mpuReady) {
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setFilterBandwidth(MPU6050_BAND_184_HZ);
    Serial.println("[sensor] MPU6050 ready");
  } else {
    Serial.println("[sensor] MPU6050 not detected");
  }

  // INA219
  ina219Ready = ina219.begin();

  if (ina219Ready) {
    ina219.setCalibration_32V_2A();
    Serial.println("[sensor] INA219 ready");
  } else {
    Serial.println("[sensor] INA219 not detected");
  }

  // SSD1306 OLED
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  if (displayReady) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    Serial.println("[display] SSD1306 ready");
  } else {
    Serial.println("[display] SSD1306 not detected");
  }

  // DS18B20
  temperatureBus.begin();
  temperatureBus.setResolution(10);
  temperatureBus.setWaitForConversion(false);

  const int temperatureDeviceCount =
      temperatureBus.getDeviceCount();

  Serial.printf("[sensor] DS18B20 devices: %d\n",
                temperatureDeviceCount);

  if (temperatureDeviceCount == 0) {
    temperatureReady = false;
    Serial.println("[sensor] DS18B20 not detected");
  }

  // MQTT
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(12288);

  connectWiFi();
  connectMqtt();

  lastRpmMs = millis();
  lastElectricalMs = millis();
  lastTemperatureRequestMs = millis() - 1000;

  // Start the delay after setup and sensor initialization.
  protectionStartMs = millis();
  Serial.println("[safety] 5-second startup delay started");

  Serial.println("[system] setup complete");
}

// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqttClient.connected()) {
    connectMqtt();
  }

  mqttClient.loop();

  readSlowSensors();

  // Sample one vibration block at the configured rate.
  static uint32_t lastVibrationBlockMs = 0;

  const uint32_t vibrationBlockPeriodMs =
      (1000UL * VIBRATION_BLOCK_SIZE) / VIBRATION_SAMPLE_RATE_HZ;

  if (millis() - lastVibrationBlockMs >= vibrationBlockPeriodMs) {
    lastVibrationBlockMs = millis();
    sampleVibrationBlock();
  }

  // Debug readings every 2 seconds.
  static uint32_t lastDebugMs = 0;

  if (millis() - lastDebugMs >= 2000) {
    printDebugReadings();
    lastDebugMs = millis();
  }

  // Safety check starts after the 5-second startup delay.
#if ENABLE_LOCAL_PROTECTION
  if (millis() - protectionStartMs >= STARTUP_DELAY_MS) {
    if (!relayTripped && !readingsAreLocallySafe()) {
      setRelay(true, "critical reading or sensor fault");
    }
  }
#endif

  if (millis() - lastTelemetryMs >= 1000) {
    publishTelemetry();
    updateDisplay();
    lastTelemetryMs = millis();
  }

 // Green: MQTT connection
digitalWrite(
    GREEN_LED_PIN,
    mqttClient.connected() ? HIGH : ((millis() / 300) % 2)
);

// Yellow: sensor or local safety issue
digitalWrite(
    YELLOW_LED_PIN,
    readingsAreLocallySafe() ? LOW : HIGH
);

// Red: protection trip is latched
digitalWrite(
    RED_LED_PIN,
    relayTripped ? HIGH : LOW
);
}