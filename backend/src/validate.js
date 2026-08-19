const finite = (value) => typeof value === "number" && Number.isFinite(value);
const isoDate = (value) => typeof value === "string" && !Number.isNaN(Date.parse(value));
const powerOfTwo = (value) => value > 1 && (value & (value - 1)) === 0;

export function validateTelemetry(input) {
  const errors = [];
  if (!input || typeof input !== "object" || Array.isArray(input)) return ["payload must be an object"];
  if (typeof input.deviceId !== "string" || !/^[\w-]{3,64}$/.test(input.deviceId)) errors.push("invalid deviceId");
  if (!isoDate(input.timestamp)) errors.push("timestamp must be ISO-8601");
  if (!Number.isInteger(input.sequence) || input.sequence < 0) errors.push("sequence must be a non-negative integer");
  const ranges = {
    temperatureC: [-55, 150],
    currentA: [0, 200],
    voltageV: [0, 600],
    rpm: [0, 100000]
  };
  for (const [field, [min, max]] of Object.entries(ranges)) {
    if (!finite(input[field]) || input[field] < min || input[field] > max) errors.push(`${field} out of range`);
  }
  return errors;
}

export function validateVibrationBlock(input) {
  const errors = [];
  if (!input || typeof input !== "object" || Array.isArray(input)) return ["payload must be an object"];
  if (typeof input.deviceId !== "string" || !/^[\w-]{3,64}$/.test(input.deviceId)) errors.push("invalid deviceId");
  if (!isoDate(input.timestamp)) errors.push("timestamp must be ISO-8601");
  if (!finite(input.sampleRateHz) || input.sampleRateHz < 50 || input.sampleRateHz > 10000) errors.push("sampleRateHz out of range");
  if (!Array.isArray(input.samples) || !powerOfTwo(input.samples.length) || input.samples.length < 64 || input.samples.length > 4096) {
    errors.push("samples length must be a power of two from 64 to 4096");
  } else if (!input.samples.every(finite)) {
    errors.push("samples contain non-finite values");
  }
  return errors;
}

