import assert from "node:assert/strict";
import test from "node:test";
import { validateTelemetry, validateVibrationBlock } from "../src/validate.js";

test("accepts a valid telemetry payload", () => {
  assert.deepEqual(validateTelemetry({
    deviceId: "motor-G23", timestamp: new Date().toISOString(), sequence: 1,
    temperatureC: 45, currentA: 2.1, voltageV: 230, rpm: 1440
  }), []);
});

test("rejects malformed and unsafe telemetry ranges", () => {
  const errors = validateTelemetry({ deviceId: "?", timestamp: "today", sequence: -1,
    temperatureC: 1000, currentA: -2, voltageV: 900, rpm: -1 });
  assert.ok(errors.length >= 7);
});

test("accepts a 256-sample vibration block", () => {
  assert.deepEqual(validateVibrationBlock({
    deviceId: "motor-G23", timestamp: new Date().toISOString(), sampleRateHz: 512,
    samples: Array(256).fill(0)
  }), []);
});

