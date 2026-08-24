import cors from "cors";
import express from "express";
import { createServer } from "node:http";
import mqtt from "mqtt";
import { Server as SocketServer } from "socket.io";
import { config, defaultThresholds } from "./config.js";
import { analyzeVibration } from "./dsp.js";
import { RuleEngine, rank } from "./rules.js";
import { createStore } from "./store.js";
import { validateTelemetry, validateVibrationBlock } from "./validate.js";

const app = express();
const httpServer = createServer(app);
const io = new SocketServer(httpServer, { cors: { origin: config.corsOrigins } });
const store = await createStore(config);
const mqttClient = mqtt.connect(config.mqttUrl, {
  clientId: `motor-backend-${Math.random().toString(16).slice(2, 10)}`,
  reconnectPeriod: 2000,
  clean: true
});
const engines = new Map();
const latestFeatures = new Map();
const latestSpectra = new Map();
const availability = new Map();

app.use(cors({ origin: config.corsOrigins }));
app.use(express.json({ limit: "3mb" }));

const engineFor = (deviceId) => {
  if (!engines.has(deviceId)) engines.set(deviceId, new RuleEngine(defaultThresholds));
  return engines.get(deviceId);
};

function parseJson(buffer) {
  try {
    return JSON.parse(buffer.toString("utf8"));
  } catch {
    throw new Error("message is not valid JSON");
  }
}

async function processVibration(payload) {
  const errors = validateVibrationBlock(payload);
  if (errors.length) throw new Error(errors.join(", "));
  const features = analyzeVibration(payload.samples, payload.sampleRateHz, payload.rpm || 0);
  latestFeatures.set(payload.deviceId, features);
  latestSpectra.set(payload.deviceId, {
    deviceId: payload.deviceId,
    timestamp: payload.timestamp,
    ...features
  });
  const summary = { ...features };
  delete summary.spectrum;
  const event = { deviceId: payload.deviceId, timestamp: payload.timestamp, ...summary };
  io.emit("vibration", { ...event, spectrum: features.spectrum });
  if (mqttClient.connected) mqttClient.publish(config.topics.vibrationFeatures, JSON.stringify(event), { qos: 0 });
  return { ...event, spectrum: features.spectrum };
}

async function processTelemetry(payload) {
  const errors = validateTelemetry(payload);
  if (errors.length) throw new Error(errors.join(", "));
  const features = latestFeatures.get(payload.deviceId) || {};
  const engine = engineFor(payload.deviceId);
  const previousStatus = engine.state;
  const assessment = engine.evaluate(payload, features);
  const vibration = Object.keys(features).length
    ? {
        rmsG: features.rmsG,
        peakG: features.peakG,
        stdG: features.stdG,
        peakToPeakG: features.peakToPeakG,
        crestFactor: features.crestFactor,
        dominantHz: features.dominantHz,
        dominantAmplitudeG: features.dominantAmplitudeG,
        shaftHz: features.shaftHz,
        oneXEnergy: features.oneXEnergy,
        twoXEnergy: features.twoXEnergy
      }
    : { rmsG: payload.vibrationRmsG ?? null };
  const calculatedPowerW = Number((payload.voltageV * payload.currentA).toFixed(2));
  const record = {
    timestamp: new Date(payload.timestamp),
    meta: {
      deviceId: payload.deviceId,
      experimentId: payload.experimentId || "default"
    },
    sequence: payload.sequence,
    temperatureC: payload.temperatureC,
    currentA: payload.currentA,
    voltageV: payload.voltageV,
    powerW: Number.isFinite(payload.powerW) ? payload.powerW : calculatedPowerW,
    rpm: payload.rpm,
    vibration,
    status: assessment.status,
    rawStatus: assessment.rawStatus,
    reasons: assessment.reasons,
    rpmDeviationPercent: assessment.rpmDeviationPercent,
    motorEnabled: Boolean(payload.motorEnabled),
    tripLatched: Boolean(payload.tripLatched),
    tripReason: typeof payload.tripReason === "string" ? payload.tripReason : "none",
    receivedAt: new Date()
  };
  await store.insertTelemetry(record);

  if (assessment.status !== previousStatus && rank(assessment.status) >= rank("ABNORMAL")) {
    const alert = {
      deviceId: payload.deviceId,
      timestamp: new Date(),
      status: assessment.status,
      reasons: assessment.reasons,
      acknowledged: false
    };
    await store.insertAlert(alert);
    io.emit("alert", alert);
    if (mqttClient.connected) mqttClient.publish(config.topics.alert, JSON.stringify(alert), { qos: 1 });
  }
  const output = { ...record, online: availability.get(payload.deviceId) !== "offline" };
  io.emit("telemetry", output);
  if (mqttClient.connected) {
    mqttClient.publish(config.topics.status, JSON.stringify({
      deviceId: payload.deviceId,
      timestamp: new Date().toISOString(),
      status: assessment.status,
      reasons: assessment.reasons,
      motorEnabled: record.motorEnabled,
      tripLatched: record.tripLatched,
      tripReason: record.tripReason
    }), { qos: 1, retain: true });
  }
  return output;
}

mqttClient.on("connect", () => {
  console.log(`[mqtt] connected to ${config.mqttUrl}`);
  mqttClient.subscribe([
    { topic: config.topics.telemetry, qos: 0 },
    { topic: config.topics.vibrationRaw, qos: 0 },
    { topic: config.topics.availability, qos: 1 }
  ]);
});
mqttClient.on("reconnect", () => console.log("[mqtt] reconnecting"));
mqttClient.on("error", (error) => console.warn(`[mqtt] ${error.message}`));
mqttClient.on("message", async (topic, buffer) => {
  try {
    if (topic === config.topics.telemetry) await processTelemetry(parseJson(buffer));
    else if (topic === config.topics.vibrationRaw) await processVibration(parseJson(buffer));
    else if (topic === config.topics.availability) {
      const text = buffer.toString("utf8");
      let event;
      try { event = JSON.parse(text); } catch { event = { deviceId: "motor-G23", state: text }; }
      availability.set(event.deviceId || "motor-G23", event.state);
      io.emit("availability", event);
    }
  } catch (error) {
    console.warn(`[mqtt] rejected ${topic}: ${error.message}`);
  }
});

app.get("/health", (_request, response) => response.json({
  ok: true,
  database: store.mode,
  mqttConnected: mqttClient.connected,
  timestamp: new Date().toISOString()
}));

app.get("/api/motor/latest", async (request, response, next) => {
  try {
    const deviceId = request.query.deviceId || "motor-G23";
    const latest = await store.latest(deviceId);
    response.json(latest ? { ...latest, online: availability.get(deviceId) !== "offline" } : null);
  } catch (error) { next(error); }
});

app.get("/api/motor/history", async (request, response, next) => {
  try {
    const parseDate = (value) => value && !Number.isNaN(Date.parse(value)) ? new Date(value) : undefined;
    const limit = Math.min(Math.max(Number(request.query.limit) || 500, 1), 5000);
    response.json(await store.history({
      deviceId: request.query.deviceId || "motor-G23",
      from: parseDate(request.query.from),
      to: parseDate(request.query.to),
      limit
    }));
  } catch (error) { next(error); }
});

app.get("/api/motor/alerts", async (request, response, next) => {
  try {
    const limit = Math.min(Math.max(Number(request.query.limit) || 100, 1), 1000);
    response.json(await store.alerts({ deviceId: request.query.deviceId || "motor-G23", limit }));
  } catch (error) { next(error); }
});

app.get("/api/motor/status", async (request, response, next) => {
  try {
    const deviceId = request.query.deviceId || "motor-G23";
    const latest = await store.latest(deviceId);
    response.json({
      deviceId,
      online: availability.get(deviceId) !== "offline",
      status: latest?.status || "UNKNOWN",
      reasons: latest?.reasons || [],
      motorEnabled: latest?.motorEnabled || false,
      tripLatched: latest?.tripLatched || false,
      tripReason: latest?.tripReason || "none",
      timestamp: latest?.timestamp || null
    });
  } catch (error) { next(error); }
});

app.get("/api/motor/spectrum", (request, response) => {
  response.json(latestSpectra.get(request.query.deviceId || "motor-G23") || null);
});

app.get("/api/config/thresholds", (_request, response) => response.json(defaultThresholds));

app.post("/api/motor/command", (request, response) => {
  const command = String(request.body?.command || "").toUpperCase();
  if (!["START", "STOP", "RESET"].includes(command)) {
    return response.status(400).json({ error: "command must be START, STOP or RESET" });
  }
  if (!mqttClient.connected) return response.status(503).json({ error: "MQTT broker is not connected" });
  const message = JSON.stringify({ command, requestedAt: new Date().toISOString(), source: "dashboard" });
  mqttClient.publish(config.topics.command, message, { qos: 1 }, (error) => {
    if (error) return response.status(503).json({ error: error.message });
    response.status(202).json({ accepted: true, command });
  });
});

if (config.allowHttpIngest) {
  app.post("/api/dev/telemetry", async (request, response) => {
    try { response.status(202).json(await processTelemetry(request.body)); }
    catch (error) { response.status(400).json({ error: error.message }); }
  });
  app.post("/api/dev/vibration", async (request, response) => {
    try { response.status(202).json(await processVibration(request.body)); }
    catch (error) { response.status(400).json({ error: error.message }); }
  });
}

app.use((error, _request, response, _next) => {
  console.error(error);
  response.status(500).json({ error: "internal server error" });
});

io.on("connection", (socket) => console.log(`[socket] client ${socket.id} connected`));

httpServer.listen(config.port, "0.0.0.0", () => {
  console.log(`[api] listening on http://0.0.0.0:${config.port}`);
});

async function shutdown(signal) {
  console.log(`[system] ${signal}; shutting down`);
  mqttClient.end(true);
  io.close();
  httpServer.close();
  await store.close();
  process.exit(0);
}
process.on("SIGINT", () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));
