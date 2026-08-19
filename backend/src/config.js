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

// Demonstration defaults only. Calibrate these values against the motor
// nameplate, healthy recordings and lab-supervisor limits before protection use.
export const defaultThresholds = {
  temperatureC: { warning: 60, abnormal: 75, critical: 90 },
  currentA: { warning: 3.0, abnormal: 4.0, critical: 5.0 },
  vibrationRmsG: { warning: 0.18, abnormal: 0.35, critical: 0.7 },
  rpmDeviationPercent: { warning: 5, abnormal: 10, critical: 20 },
  persistenceWindows: 3,
  recoveryWindows: 5,
  hysteresisPercent: 8,
  nominalRpm: number("NOMINAL_RPM", 1440)
};

