import { useEffect, useMemo, useState } from "react";
import {
  CategoryScale, Chart as ChartJS, Filler, Legend, LinearScale, LineElement,
  PointElement, Title, Tooltip, BarElement
} from "chart.js";
import { Bar, Line } from "react-chartjs-2";
import { io } from "socket.io-client";
import { API_URL, fetchJson } from "./api.js";

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, BarElement, Filler, Title, Tooltip, Legend);

const DEVICE_ID = "motor-G23";
const statusClass = (status = "UNKNOWN") => `status status--${status.toLowerCase()}`;
const number = (value, digits = 1) => Number.isFinite(Number(value)) ? Number(value).toFixed(digits) : "--";
const time = (value) => value ? new Date(value).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" }) : "--";

const chartOptions = (title, yTitle) => ({
  responsive: true,
  maintainAspectRatio: false,
  interaction: { intersect: false, mode: "index" },
  animation: { duration: 250 },
  plugins: {
    legend: { labels: { color: "#9eb0c6", boxWidth: 10, usePointStyle: true } },
    title: { display: true, text: title, color: "#eef6ff", align: "start", font: { size: 15, weight: 600 } }
  },
  scales: {
    x: { ticks: { color: "#71849a", maxTicksLimit: 8 }, grid: { color: "rgba(135,160,190,.08)" } },
    y: { title: { display: true, text: yTitle, color: "#71849a" }, ticks: { color: "#71849a" }, grid: { color: "rgba(135,160,190,.08)" } }
  }
});

function MetricCard({ label, value, unit, accent, detail }) {
  return (
    <article className="metric" style={{ "--accent": accent }}>
      <div className="metric__label">{label}</div>
      <div className="metric__reading">{value}<span>{unit}</span></div>
      <div className="metric__detail">{detail}</div>
    </article>
  );
}

export default function App() {
  const [latest, setLatest] = useState(null);
  const [history, setHistory] = useState([]);
  const [alerts, setAlerts] = useState([]);
  const [spectrum, setSpectrum] = useState(null);
  const [socketConnected, setSocketConnected] = useState(false);
  const [apiError, setApiError] = useState("");
  const [commandState, setCommandState] = useState("");

  useEffect(() => {
    Promise.all([
      fetchJson(`/api/motor/latest?deviceId=${DEVICE_ID}`),
      fetchJson(`/api/motor/history?deviceId=${DEVICE_ID}&limit=120`),
      fetchJson(`/api/motor/alerts?deviceId=${DEVICE_ID}&limit=20`),
      fetchJson(`/api/motor/spectrum?deviceId=${DEVICE_ID}`)
    ]).then(([latestData, historyData, alertData, spectrumData]) => {
      setLatest(latestData);
      setHistory(historyData);
      setAlerts(alertData);
      setSpectrum(spectrumData);
      setApiError("");
    }).catch((error) => setApiError(error.message));

    const socket = io(API_URL, { transports: ["websocket", "polling"] });
    socket.on("connect", () => setSocketConnected(true));
    socket.on("disconnect", () => setSocketConnected(false));
    socket.on("telemetry", (record) => {
      if (record.meta?.deviceId !== DEVICE_ID) return;
      setLatest(record);
      setHistory((items) => [...items, record].slice(-240));
    });
    socket.on("vibration", (features) => {
      if (features.deviceId === DEVICE_ID) setSpectrum(features);
    });
    socket.on("alert", (alert) => {
      if (alert.deviceId === DEVICE_ID) setAlerts((items) => [alert, ...items].slice(0, 40));
    });
    socket.on("availability", (event) => {
      if ((event.deviceId || DEVICE_ID) === DEVICE_ID) {
        setLatest((current) => current ? { ...current, online: event.state !== "offline" } : current);
      }
    });
    return () => socket.close();
  }, []);

  const trendData = useMemo(() => ({
    labels: history.map((item) => time(item.timestamp)),
    datasets: [
      { label: "Temperature °C", data: history.map((item) => item.temperatureC), borderColor: "#ffb454", backgroundColor: "rgba(255,180,84,.10)", tension: 0.3, pointRadius: 0, fill: true },
      { label: "Vibration RMS g", data: history.map((item) => (item.vibration?.rmsG ?? 0) * 100, borderColor: "#32d6c5", backgroundColor: "transparent", tension: 0.3, pointRadius: 0 }
    ]
  }), [history]);

  const electricalData = useMemo(() => ({
    labels: history.map((item) => time(item.timestamp)),
    datasets: [
      { label: "Current A", data: history.map((item) => item.currentA), borderColor: "#7c8cff", tension: 0.25, pointRadius: 0 },
      { label: "Power ÷ 100 W", data: history.map((item) => (item.powerW || 0) / 100), borderColor: "#dd75ff", tension: 0.25, pointRadius: 0 }
    ]
  }), [history]);

  const spectrumData = useMemo(() => ({
    labels: (spectrum?.spectrum || []).map((bin) => bin.frequencyHz),
    datasets: [{ label: "Amplitude g", data: (spectrum?.spectrum || []).map((bin) => bin.amplitudeG), backgroundColor: "rgba(50,214,197,.62)", borderRadius: 2 }]
  }), [spectrum]);

  const sendCommand = async (command) => {
    if (!window.confirm(command === "STOP" ? "Send a protected STOP request to Motor G23?" : "Request relay reset? Reset is accepted by firmware only when local readings are safe.")) return;
    setCommandState("Sending…");
    try {
      await fetchJson("/api/motor/command", {
        method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ command })
      });
      setCommandState(`${command} accepted`);
    } catch (error) { setCommandState(`Failed: ${error.message}`); }
    window.setTimeout(() => setCommandState(""), 3500);
  };

  const status = latest?.status || "UNKNOWN";
  const reasons = latest?.reasons || [];
  const online = Boolean(latest?.online && socketConnected);

  return (
    <main className="shell">
      <header className="topbar">
        <div>
          <div className="eyebrow">MNNIT · GROUP 23 · LIVE ASSET</div>
          <h1>Induction Motor <span>Condition Monitor</span></h1>
        </div>
        <div className="topbar__state">
          <span className={`connection ${online ? "connection--online" : ""}`}><i />{online ? "LIVE" : "OFFLINE"}</span>
          <span className={statusClass(status)}>{status}</span>
        </div>
      </header>

      {apiError && <div className="notice notice--error">Backend unavailable: {apiError}. Start Docker Compose or the local backend.</div>}
      {!history.length && !apiError && <div className="notice">Waiting for Motor G23 telemetry. Run <code>npm run simulate</code> inside <code>backend/</code> to preview all states.</div>}

      <section className="metrics">
        <MetricCard label="Winding temperature" value={number(latest?.temperatureC)} unit="°C" accent="#ffb454" detail={`Updated ${time(latest?.timestamp)}`} />
        <MetricCard label="Motor current" value={number(latest?.currentA, 2)} unit="A" accent="#7c8cff" detail={`${number(latest?.powerW, 0)} W active estimate`} />
        <MetricCard label="Supply voltage" value={number(latest?.voltageV, 1)} unit="V" accent="#dd75ff" detail="PZEM-004T measurement" />
        <MetricCard label="Shaft speed" value={number(latest?.rpm, 0)} unit="RPM" accent="#5ab0ff" detail={`${number(spectrum?.shaftHz, 1)} Hz shaft frequency`} />
        <MetricCard label="Vibration RMS" value={number(latest?.vibration?.rmsG, 3)} unit="g" accent="#32d6c5" detail={`Peak ${number(latest?.vibration?.peakG, 3)} g`} />
      </section>

      <section className="grid grid--charts">
        <article className="panel panel--wide"><div className="chart"><Line data={trendData} options={chartOptions("Thermal & vibration trend", "°C / RMS × 100")} /></div></article>
        <article className="panel"><div className="chart"><Line data={electricalData} options={chartOptions("Electrical load trend", "A / 100 W")} /></div></article>
        <article className="panel"><div className="chart"><Bar data={spectrumData} options={chartOptions(`FFT spectrum · dominant ${number(spectrum?.dominantHz, 1)} Hz`, "Amplitude g")} /></div></article>
      </section>

      <section className="grid grid--lower">
        <article className="panel diagnosis">
          <div className="panel__heading"><div><div className="eyebrow">RULE ENGINE</div><h2>Condition assessment</h2></div><span className={statusClass(status)}>{status}</span></div>
          <div className="diagnosis__body">
            {reasons.length ? reasons.map((reason) => (
              <div className="reason" key={reason.code}><span>{reason.code.replaceAll("_", " ")}</span><strong>{number(reason.value, 2)}</strong><em>{reason.status}</em></div>
            )) : <div className="healthy"><span>✓</span><div><strong>No active fault reasons</strong><p>All configured measurements are inside their calibrated limits.</p></div></div>}
          </div>
          <div className="controls"><button className="button button--danger" onClick={() => sendCommand("STOP")}>Protected stop</button><button className="button" onClick={() => sendCommand("RESET")}>Reset request</button><span>{commandState}</span></div>
        </article>

        <article className="panel alerts">
          <div className="panel__heading"><div><div className="eyebrow">EVENT LOG</div><h2>Recent alerts</h2></div><span className="count">{alerts.length}</span></div>
          <div className="alerts__list">
            {alerts.length ? alerts.map((alert, index) => (
              <div className="alert-row" key={alert._id || `${alert.timestamp}-${index}`}><i className={`alert-dot alert-dot--${alert.status.toLowerCase()}`} /><div><strong>{alert.status}</strong><p>{(alert.reasons || []).map((reason) => reason.code).join(" · ") || "State transition"}</p></div><time>{time(alert.timestamp)}</time></div>
            )) : <div className="empty">No abnormal or critical transitions recorded.</div>}
          </div>
        </article>
      </section>

      <footer><span>Device: {DEVICE_ID}</span><span>Sequence: {latest?.sequence ?? "--"}</span><span>Database timestamp: {time(latest?.receivedAt)}</span><span>FFT: {spectrum?.sampleCount || "--"} samples @ {spectrum?.sampleRateHz || "--"} Hz</span></footer>
    </main>
  );
}

