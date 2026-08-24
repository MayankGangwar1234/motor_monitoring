import assert from "node:assert/strict";
import test from "node:test";
import { validateTelemetry, validateVibrationBlock } from "../src/validate.js";

test("accepts a valid 12 V DC motor telemetry payload", () => {
  assert.deepEqual(validateTelemetry({
    deviceId: "motor-G23", timestamp: new Date().toISOString(), sequence: 1,
    temperatureC: 34, currentA: 0.18, voltageV: 12, powerW: 2.16,
    rpm: 300, motorEnabled: true, tripLatched: false
  }), []);
});

test("rejects malformed and out-of-system telemetry ranges", () => {
  const errors = validateTelemetry({ deviceId: "?", timestamp: "today", sequence: -1,
    temperatureC: 1000, currentA: -2, voltageV: 40, rpm: -1,
    motorEnabled: "yes", tripLatched: 0, powerW: 200 });
  assert.ok(errors.length >= 10);
});

test("accepts a 256-sample vibration block", () => {
  assert.deepEqual(validateVibrationBlock({
    deviceId: "motor-G23", timestamp: new Date().toISOString(), sampleRateHz: 256,
    samples: Array(256).fill(0)
  }), []);
});
