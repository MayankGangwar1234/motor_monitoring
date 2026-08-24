const baseUrl = process.env.API_URL || "http://localhost:3000";
const deviceId = process.env.DEVICE_ID || "motor-G23";
const sampleRateHz = 256;
const sampleCount = 256;
let sequence = 0;

const post = async (path, body) => {
  const response = await fetch(`${baseUrl}${path}`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body)
  });
  if (!response.ok) throw new Error(`${path}: ${response.status} ${await response.text()}`);
  return response.json();
};

function operatingPoint(elapsedSeconds) {
  const phase = Math.floor(elapsedSeconds / 25) % 4;
  if (phase === 1) return { temperatureC: 46, currentA: 0.48, rpm: 270, amplitude: 0.12, label: "WARNING" };
  if (phase === 2) return { temperatureC: 54, currentA: 0.85, rpm: 225, amplitude: 0.28, label: "ABNORMAL" };
  if (phase === 3) return { temperatureC: 62, currentA: 1.25, rpm: 150, amplitude: 0.52, label: "CRITICAL" };
  return { temperatureC: 34, currentA: 0.18, rpm: 300, amplitude: 0.03, label: "NORMAL" };
}

async function tick(startedAt) {
  const elapsed = (Date.now() - startedAt) / 1000;
  const point = operatingPoint(elapsed);
  const shaftHz = point.rpm / 60;
  const samples = Array.from({ length: sampleCount }, (_, index) => {
    const time = index / sampleRateHz;
    const fundamental = point.amplitude * Math.sin(2 * Math.PI * shaftHz * time);
    const secondHarmonic = point.amplitude * 0.24 * Math.sin(2 * Math.PI * shaftHz * 2 * time);
    const noise = (Math.random() - 0.5) * point.amplitude * 0.08;
    return Number((fundamental + secondHarmonic + noise).toFixed(6));
  });
  const timestamp = new Date().toISOString();
  await post("/api/dev/vibration", { deviceId, timestamp, sequence, sampleRateHz, rpm: point.rpm, samples });
  const result = await post("/api/dev/telemetry", {
    deviceId,
    experimentId: "dc-12v-software-simulation",
    timestamp,
    sequence,
    temperatureC: point.temperatureC + (Math.random() - 0.5),
    currentA: point.currentA + (Math.random() - 0.5) * 0.02,
    voltageV: 12 + (Math.random() - 0.5) * 0.16,
    powerW: 12 * point.currentA,
    rpm: point.rpm + Math.round((Math.random() - 0.5) * 4),
    motorEnabled: point.label !== "CRITICAL",
    tripLatched: point.label === "CRITICAL",
    tripReason: point.label === "CRITICAL" ? "simulated local trip" : "none"
  });
  console.log(`${timestamp} simulated=${point.label} classified=${result.status} rpm=${result.rpm}`);
  sequence += 1;
}

console.log(`Publishing simulated 12 V DC motor data to ${baseUrl}. Press Ctrl+C to stop.`);
const startedAt = Date.now();
await tick(startedAt);
setInterval(() => tick(startedAt).catch((error) => console.error(error.message)), 2000);
