const LEVELS = ["NORMAL", "WARNING", "ABNORMAL", "CRITICAL"];
const rank = (status) => LEVELS.indexOf(status);

function levelForHigh(value, limits, recoveryFactor = 1) {
  if (!Number.isFinite(value)) return "NORMAL";
  if (value >= limits.critical * recoveryFactor) return "CRITICAL";
  if (value >= limits.abnormal * recoveryFactor) return "ABNORMAL";
  if (value >= limits.warning * recoveryFactor) return "WARNING";
  return "NORMAL";
}

export class RuleEngine {
  constructor(thresholds) {
    this.thresholds = structuredClone(thresholds);
    this.state = "NORMAL";
    this.candidate = "NORMAL";
    this.candidateCount = 0;
    this.recoveryCount = 0;
  }

  rawEvaluation(telemetry, features = {}) {
    const recoveryFactor = this.state === "NORMAL"
      ? 1
      : 1 - this.thresholds.hysteresisPercent / 100;
    const nominalRpm = telemetry.nominalRpm || this.thresholds.nominalRpm;
    const rpmDeviationPercent = nominalRpm > 0 && Number.isFinite(telemetry.rpm)
      ? (Math.abs(telemetry.rpm - nominalRpm) / nominalRpm) * 100
      : 0;
    const checks = [
      ["TEMP_HIGH", telemetry.temperatureC, this.thresholds.temperatureC],
      ["CURRENT_HIGH", telemetry.currentA, this.thresholds.currentA],
      ["VIBRATION_HIGH", features.rmsG ?? telemetry.vibrationRmsG, this.thresholds.vibrationRmsG],
      ["RPM_DEVIATION", rpmDeviationPercent, this.thresholds.rpmDeviationPercent]
    ].map(([code, value, limits]) => ({
      code,
      value,
      status: levelForHigh(value, limits, recoveryFactor)
    }));
    const status = checks.reduce(
      (worst, check) => (rank(check.status) > rank(worst) ? check.status : worst),
      "NORMAL"
    );
    return {
      status,
      reasons: checks.filter((check) => check.status !== "NORMAL"),
      rpmDeviationPercent: Number(rpmDeviationPercent.toFixed(2))
    };
  }

  evaluate(telemetry, features = {}) {
    const raw = this.rawEvaluation(telemetry, features);
    const currentRank = rank(this.state);
    const rawRank = rank(raw.status);

    if (raw.status === "CRITICAL") {
      this.state = "CRITICAL";
      this.candidate = "CRITICAL";
      this.candidateCount = 0;
      this.recoveryCount = 0;
    } else if (rawRank > currentRank) {
      if (this.candidate !== raw.status) {
        this.candidate = raw.status;
        this.candidateCount = 1;
      } else {
        this.candidateCount += 1;
      }
      if (this.candidateCount >= this.thresholds.persistenceWindows) {
        this.state = raw.status;
        this.candidateCount = 0;
      }
      this.recoveryCount = 0;
    } else if (rawRank < currentRank) {
      this.recoveryCount += 1;
      if (this.recoveryCount >= this.thresholds.recoveryWindows) {
        this.state = raw.status;
        this.candidate = raw.status;
        this.recoveryCount = 0;
      }
    } else {
      this.candidate = raw.status;
      this.candidateCount = 0;
      this.recoveryCount = 0;
    }

    return { ...raw, status: this.state, rawStatus: raw.status };
  }
}

export { LEVELS, rank, levelForHigh };
