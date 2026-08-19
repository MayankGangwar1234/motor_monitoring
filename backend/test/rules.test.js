import assert from "node:assert/strict";
import test from "node:test";
import { defaultThresholds } from "../src/config.js";
import { RuleEngine } from "../src/rules.js";

const normal = { temperatureC: 45, currentA: 2, voltageV: 230, rpm: 1440 };

test("warning requires configured persistence", () => {
  const engine = new RuleEngine({ ...defaultThresholds, persistenceWindows: 3 });
  const warning = { ...normal, temperatureC: 65 };
  assert.equal(engine.evaluate(warning, { rmsG: 0.08 }).status, "NORMAL");
  assert.equal(engine.evaluate(warning, { rmsG: 0.08 }).status, "NORMAL");
  const third = engine.evaluate(warning, { rmsG: 0.08 });
  assert.equal(third.status, "WARNING");
  assert.equal(third.reasons[0].code, "TEMP_HIGH");
});

test("critical condition acts immediately", () => {
  const engine = new RuleEngine(defaultThresholds);
  const result = engine.evaluate({ ...normal, currentA: 6 }, { rmsG: 0.08 });
  assert.equal(result.status, "CRITICAL");
  assert.ok(result.reasons.some((reason) => reason.code === "CURRENT_HIGH"));
});

test("recovery requires consecutive healthy windows", () => {
  const engine = new RuleEngine({ ...defaultThresholds, recoveryWindows: 2 });
  engine.evaluate({ ...normal, temperatureC: 95 }, { rmsG: 0.08 });
  assert.equal(engine.evaluate(normal, { rmsG: 0.08 }).status, "CRITICAL");
  assert.equal(engine.evaluate(normal, { rmsG: 0.08 }).status, "NORMAL");
});

test("numeric hysteresis prevents boundary chatter", () => {
  const engine = new RuleEngine({ ...defaultThresholds, persistenceWindows: 1, recoveryWindows: 1, hysteresisPercent: 10 });
  assert.equal(engine.evaluate({ ...normal, temperatureC: 61 }, { rmsG: 0.08 }).status, "WARNING");
  assert.equal(engine.evaluate({ ...normal, temperatureC: 59 }, { rmsG: 0.08 }).status, "WARNING");
  assert.equal(engine.evaluate({ ...normal, temperatureC: 53 }, { rmsG: 0.08 }).status, "NORMAL");
});
