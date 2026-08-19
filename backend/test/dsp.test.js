import assert from "node:assert/strict";
import test from "node:test";
import { analyzeVibration } from "../src/dsp.js";

test("FFT detects a known 50 Hz sine wave", () => {
  const sampleRateHz = 1024;
  const samples = Array.from({ length: 1024 }, (_, index) =>
    0.4 * Math.sin(2 * Math.PI * 50 * index / sampleRateHz)
  );
  const result = analyzeVibration(samples, sampleRateHz, 1500);
  assert.ok(Math.abs(result.dominantHz - 50) <= result.binWidthHz);
  assert.ok(Math.abs(result.rmsG - 0.4 / Math.sqrt(2)) < 0.001);
  assert.ok(Math.abs(result.dominantAmplitudeG - 0.4) < 0.002);
  assert.equal(result.shaftHz, 25);
  assert.equal(result.sampleCount, 1024);
});

test("FFT rejects a non-power-of-two sample block", () => {
  assert.throws(() => analyzeVibration([1, 2, 3], 1000), /power-of-two/);
});

test("zero vibration has finite zero-valued features", () => {
  const result = analyzeVibration(Array(256).fill(0), 512, 0);
  assert.equal(result.rmsG, 0);
  assert.equal(result.crestFactor, 0);
  assert.ok(Number.isFinite(result.dominantHz));
});

