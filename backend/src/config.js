const number = (name, fallback) => {
  const parsed = Number(process.env[name]);
  return Number.isFinite(parsed) ? parsed : fallback;
};

export const config = {
  port: number("PORT", 3000),
  mqttUrl: process.env.MQTT_URL || "mqtt://localhost:1883",
  mongoUrl: process.env.MONGODB_URL || "mongodb://localhost:27017",
  mongoDb: process.env.MONGODB_DB || "motor_monitoring",
  corsOrigins: (process.env.CORS_ORIGIN || "http://localhost:5173,http://localhost:8080")
    .split(",")
    .map((value) => value.trim()),
  allowHttpIngest: process.env.ALLOW_HTTP_INGEST !== "false",
  requireMongo: process.env.REQUIRE_MONGO === "true",
  topics: {
    telemetry: "motor/G23/telemetry",
    vibrationRaw: "motor/G23/vibration/raw",
    vibrationFeatures: "motor/G23/vibration/features",
    availability: "motor/G23/availability",
    status: "motor/G23/status",
    alert: "motor/G23/alert",
    command: "motor/G23/command"
  }
};

// Demonstration defaults for the selected 12 V, 300 RPM N20 motor. Replace
// them with measured healthy baselines after the hardware is assembled.
export const defaultThresholds = {
  temperatureC: { warning: 45, abnormal: 52, critical: 60 },
  currentA: { warning: 0.45, abnormal: 0.80, critical: 1.20 },
  vibrationRmsG: { warning: 0.10, abnormal: 0.25, critical: 0.45 },
  rpmDeviationPercent: { warning: 10, abnormal: 20, critical: 40 },
  persistenceWindows: 3,
  recoveryWindows: 5,
  hysteresisPercent: 8,
  nominalRpm: number("NOMINAL_RPM", 300)
};
