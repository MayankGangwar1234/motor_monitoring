const baseUrl = process.env.API_URL || "http://localhost:3000";
const deviceId = process.env.DEVICE_ID || "motor-G23";
const sampleRateHz = 512;
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
  if (phase === 1) return { temperatureC: 64, currentA: 3.2, rpm: 1390, amplitude: 0.23, label: "WARNING" };
  if (phase === 2) return { temperatureC: 78, currentA: 4.2, rpm: 1270, amplitude: 0.42, label: "ABNORMAL" };
  if (phase === 3) return { temperatureC: 93, currentA: 5.4, rpm: 1080, amplitude: 0.8, label: "CRITICAL" };
  return { temperatureC: 45, currentA: 2.1, rpm: 1440, amplitude: 0.08, label: "NORMAL" };
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
    experimentId: "software-simulation",
    timestamp,
    sequence,
    temperatureC: point.temperatureC + (Math.random() - 0.5),
    currentA: point.currentA + (Math.random() - 0.5) * 0.04,
    voltageV: 230 + (Math.random() - 0.5) * 2,
    rpm: point.rpm + Math.round((Math.random() - 0.5) * 8),
    relayTripped: point.label === "CRITICAL"
  });
  console.log(`${timestamp} simulated=${point.label} classified=${result.status} rpm=${result.rpm}`);
  sequence += 1;
}

console.log(`Publishing simulated motor data to ${baseUrl}. Press Ctrl+C to stop.`);
const startedAt = Date.now();
await tick(startedAt);
setInterval(() => tick(startedAt).catch((error) => console.error(error.message)), 2000);

