import assert from "node:assert/strict";
import test from "node:test";
import { defaultThresholds } from "../src/config.js";
import { RuleEngine } from "../src/rules.js";

const normal = { temperatureC: 34, currentA: 0.18, voltageV: 12, rpm: 300, motorEnabled: true };

test("warning requires configured persistence", () => {
  const engine = new RuleEngine({ ...defaultThresholds, persistenceWindows: 3 });
  const warning = { ...normal, temperatureC: 46 };
  assert.equal(engine.evaluate(warning, { rmsG: 0.03 }).status, "NORMAL");
  assert.equal(engine.evaluate(warning, { rmsG: 0.03 }).status, "NORMAL");
  const third = engine.evaluate(warning, { rmsG: 0.03 });
  assert.equal(third.status, "WARNING");
  assert.equal(third.reasons[0].code, "TEMP_HIGH");
});

test("critical condition acts immediately", () => {
  const engine = new RuleEngine(defaultThresholds);
  const result = engine.evaluate({ ...normal, currentA: 1.25 }, { rmsG: 0.03 });
  assert.equal(result.status, "CRITICAL");
  assert.ok(result.reasons.some((reason) => reason.code === "CURRENT_HIGH"));
});

test("recovery requires consecutive healthy windows", () => {
  const engine = new RuleEngine({ ...defaultThresholds, recoveryWindows: 2 });
  engine.evaluate({ ...normal, temperatureC: 62 }, { rmsG: 0.03 });
  assert.equal(engine.evaluate(normal, { rmsG: 0.03 }).status, "CRITICAL");
  assert.equal(engine.evaluate(normal, { rmsG: 0.03 }).status, "NORMAL");
});

test("numeric hysteresis prevents boundary chatter", () => {
  const engine = new RuleEngine({ ...defaultThresholds, persistenceWindows: 1, recoveryWindows: 1, hysteresisPercent: 10 });
  assert.equal(engine.evaluate({ ...normal, temperatureC: 46 }, { rmsG: 0.03 }).status, "WARNING");
  assert.equal(engine.evaluate({ ...normal, temperatureC: 44 }, { rmsG: 0.03 }).status, "WARNING");
  assert.equal(engine.evaluate({ ...normal, temperatureC: 40 }, { rmsG: 0.03 }).status, "NORMAL");
});

test("RPM deviation is ignored while the motor output is off", () => {
  const engine = new RuleEngine(defaultThresholds);
  const result = engine.evaluate({ ...normal, motorEnabled: false, rpm: 0 }, { rmsG: 0.03 });
  assert.equal(result.status, "NORMAL");
  assert.equal(result.rpmDeviationPercent, 0);
});
