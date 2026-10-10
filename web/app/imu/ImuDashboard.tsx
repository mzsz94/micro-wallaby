"use client";

import { memo, useEffect, useMemo, useRef, useState } from "react";
import { boardPose, chartSamples, relativeTilt, unwrapDegrees } from "./rendering";

type Vector = [number, number, number];
type Sample = { t: number; seq: number; accel: Vector; gyro: Vector; corrected: Vector };
type Log = { t: number; level: "info" | "warn" | "error"; text: string };
type GraphicQuality = "low" | "high";
type DeviceMetadata = { board: "nu40" | "nu87" | "unknown"; name: string; mcu: string; pins: { sda: string; scl: string }; baud: number | null };
type OrientationReference = { gravity: Vector; flatGravity?: Vector; label: string };
type HostCalibration = { source: "host"; label: string; gyroBiasDps: Vector };
type Snapshot = {
  device: DeviceMetadata | null;
  orientationReference: OrientationReference | null;
  calibration: HostCalibration | null;
  connection: { state: "connected" | "connecting" | "disconnected" | "error"; port: string | null; firmware: string | null; message: string };
  sensor: { ready: boolean; address: string | null; whoAmI: string | null; profile: string | null; lastError: number | null; stage: string | null; sda: number | null; scl: number | null; fresh: number; errors: number; stale: number; sampleHz: number; biasValid: boolean };
  samples: Sample[];
  logs: Log[];
  updatedAt: number;
};

const EMPTY: Snapshot = {
  device: null,
  orientationReference: null,
  calibration: null,
  connection: { state: "connecting", port: null, firmware: null, message: "로컬 브리지 연결 중" },
  sensor: { ready: false, address: null, whoAmI: null, profile: null, lastError: null, stage: null, sda: null, scl: null, fresh: 0, errors: 0, stale: 0, sampleHz: 0, biasValid: false },
  samples: [], logs: [], updatedAt: 0,
};
const UNKNOWN_DEVICE: DeviceMetadata = { board: "unknown", name: "NU 보드", mcu: "IMU", pins: { sda: "—", scl: "—" }, baud: null };
const LEGACY_NU40: DeviceMetadata = { board: "nu40", name: "NU40-DK", mcu: "nRF52840", pins: { sda: "P0.26", scl: "P0.27" }, baud: 115200 };
const AXIS_COLORS = ["#5be5cd", "#a699ff", "#ffc477"];
const AXES = ["X", "Y", "Z"];
const DEMO_LOGS: Log[] = [{ t: 0, level: "info", text: "[DEMO] 예시 데이터로 화면을 미리 보고 있습니다." }, { t: 0, level: "info", text: "[DEMO] 실제 보드의 상태는 연결 정보에서 확인할 수 있습니다." }];
const ICON_PATHS: Record<string, string> = {
  wave: "M2 12h3l3-8 5 16 3-8h6", download: "M12 3v12m-5-5 5 5 5-5M5 16v5h14v-5",
  pause: "M8 5v14M16 5v14", play: "m8 5 11 7-11 7Z", refresh: "M20 7v5h-5M4 17v-5h5M5.2 8a7 7 0 0 1 11.5-3L20 9M4 15l3.3 4A7 7 0 0 0 18.8 16",
  chip: "M7 7h10v10H7zM9 2v5m6-5v5M9 17v5m6-5v5M2 9h5m-5 6h5m10-6h5m-5 6h5M10 10h4v4h-4z",
  arrow: "M5 12h14m-5-5 5 5-5 5", target: "M12 2v4m0 12v4M2 12h4m12 0h4M8 12a4 4 0 1 0 8 0 4 4 0 1 0-8 0",
  terminal: "m4 6 5 6-5 6m8 0h8", alert: "m12 3 10 18H2L12 3Zm0 6v5m0 3v.1",
  check: "m5 12 4 4L19 6", cable: "M8 3v5m8-5v5M6 8h12v3a6 6 0 0 1-12 0V8Zm6 9v5",
  layers: "m12 3 10 5-10 5L2 8l10-5Zm-9 9 9 5 9-5M3 16l9 5 9-5",
};

function Icon({ name, size = 18 }: { name: string; size?: number }) {
  return <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.65" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d={ICON_PATHS[name] || ICON_PATHS.wave} /></svg>;
}
function fixed(value: number | undefined, digits = 3) { return value === undefined || !Number.isFinite(value) ? "—" : value.toFixed(digits); }
function timeLabel(t: number) { const date = new Date(t); return [date.getHours(), date.getMinutes(), date.getSeconds()].map((value) => String(value).padStart(2, "0")).join(":"); }
function makeDemo(t: number, seq: number): Sample {
  const s = t / 1000;
  const roll = Math.sin(s * .46) * .48;
  const pitch = Math.cos(s * .33) * .3;
  const accel: Vector = [Math.sin(pitch), Math.sin(roll) * Math.cos(pitch), Math.cos(roll) * Math.cos(pitch)];
  const gyro: Vector = [Math.cos(s * .46) * 12.7, -Math.sin(s * .33) * 5.7, Math.sin(s * .61) * 3.2];
  return { t, seq, accel, gyro, corrected: gyro };
}

const SignalChart = memo(function SignalChart({ kind, samples, demo, paused, stale, quality, calibrationLabel }: { kind: "accel" | "gyro"; samples: Sample[]; demo: boolean; paused: boolean; stale: boolean; quality: GraphicQuality; calibrationLabel?: string }) {
  const latest = samples.at(-1);
  const data = useMemo(() => {
    const end = samples.at(-1)?.t || 0;
    const start = end - 15000;
    const visible = samples.filter((s) => s.t >= start);
    const property = kind === "accel" ? "accel" : "corrected";
    const plotted = chartSamples(visible, property, quality === "low" ? 120 : 600);
    let max = kind === "accel" ? 1.5 : 20;
    for (const sample of visible) for (const value of sample[property]) max = Math.max(max, Math.abs(value) * 1.12);
    const paths = AXES.map((_, axis) => plotted.map((sample, i) => `${i ? "L" : "M"}${(((sample.t - start) / 15000) * 740 + 43).toFixed(1)},${(75 - sample[property][axis] / max * 57).toFixed(1)}`).join(" "));
    return { paths, max, count: visible.length, points: plotted.length };
  }, [samples, kind, quality]);
  const values = kind === "accel" ? latest?.accel : latest?.corrected;
  return <section className="imu-card imu-signal" aria-label={`${kind === "accel" ? "가속도" : "각속도"} 실시간 차트`}>
    <div className="imu-card-top"><div className="imu-card-title"><span className={`imu-iconbox ${kind}`}><Icon name={kind === "accel" ? "wave" : "target"} /></span><h2>{kind === "accel" ? "가속도" : "각속도"}<span>{kind === "accel" ? "ACCELEROMETER" : "GYROSCOPE"}</span></h2></div><span className="imu-chart-range">{kind === "accel" ? "g" : "°/s"}<i />15초</span></div>
    <div className="imu-metrics">{AXES.map((axis, i) => <div key={axis}><span className="imu-axis" style={{ color: AXIS_COLORS[i] }}>{axis}<i style={{ background: AXIS_COLORS[i] }} /></span><strong>{fixed(values?.[i], kind === "accel" ? 3 : 2)}</strong><small>{kind === "accel" ? "g" : "°/s"}</small></div>)}</div>
    <div className="imu-chart">
      <svg viewBox="0 0 800 164" role="img" data-point-count={data.points} aria-label={data.count ? `최근 15초 ${AXES.join(", ")}축 ${demo ? "데모 예시" : "실측"} 데이터` : "센서 샘플을 기다리는 빈 차트"} preserveAspectRatio="none">
        {[18, 46.5, 75, 103.5, 132].map((y, i) => <g key={y}><line x1="43" y1={y} x2="783" y2={y} stroke={i === 2 ? "#354352" : "#22303e"} strokeDasharray={i === 2 ? undefined : "3 5"} /><text x="33" y={y + 3} textAnchor="end" fill="#778799" fontSize="10">{(data.max * (1 - i / 2)).toFixed(kind === "accel" ? 1 : 0)}</text></g>)}
        {[0, 1, 2, 3, 4, 5].map((v) => <g key={v}><line x1={43 + v * 148} y1="16" x2={43 + v * 148} y2="133" stroke="#1c2a37" /><text x={43 + v * 148} y="157" textAnchor={v === 0 ? "start" : v === 5 ? "end" : "middle"} fill="#778799" fontSize="10">{v === 5 ? "현재" : `${15 - v * 3}초 전`}</text></g>)}
        {data.paths.map((d, i) => d && <path key={i} d={d} fill="none" stroke={AXIS_COLORS[i]} strokeWidth="1.7" vectorEffect="non-scaling-stroke" strokeLinejoin="round" />)}
      </svg>
      {!data.count && <div className="imu-chart-empty"><span className="imu-empty-dot" />센서 데이터를 기다리고 있습니다</div>}
    </div>
    <div className="imu-signal-footer"><span><i className={`imu-dot ${data.count && !stale ? "mint" : "muted"}`} />{demo ? "데모 · 예시 데이터" : paused ? "화면 일시정지" : stale ? "수신 지연 · 마지막 측정값" : data.count ? "실측 데이터" : "아직 수신된 샘플 없음"}</span><span>{kind === "gyro" ? calibrationLabel || "바이어스 보정값" : "중력 가속도 포함"}</span></div>
  </section>;
});

const Board = memo(function Board({ sample, demo, stale, paused, device, orientationReference }: { sample?: Sample; demo: boolean; stale: boolean; paused: boolean; device: DeviceMetadata; orientationReference: OrientationReference | null }) {
  const boardRef = useRef<HTMLDivElement>(null);
  const motionRef = useRef<{ roll: number; arrivedAt: number; demo: boolean; board: string; referenceKey: string } | null>(null);
  const reference = !demo && device.board === "nu87" ? orientationReference : null;
  const hasFrame = Boolean(reference?.flatGravity);
  const referenceKey = reference ? [...reference.gravity, ...(reference.flatGravity || [])].join(",") : "";
  const basePose = hasFrame ? "" : "rotateX(54deg) rotateZ(-31deg)";
  const valid = sample && Math.hypot(...sample.accel) > .02;
  const pose = valid && reference?.flatGravity ? boardPose(sample.accel, reference.flatGravity, reference.gravity) : null;
  const poseTransform = pose ? `matrix3d(${pose.matrix.map((value) => Math.abs(value) < 1e-12 ? 0 : Number(value.toFixed(10))).join(",")})` : null;
  const relative = valid && reference && !hasFrame ? relativeTilt(sample.accel, reference.gravity) : null;
  const roll = hasFrame ? pose?.roll : reference ? relative?.roll : valid ? Math.atan2(sample.accel[1], sample.accel[2]) * 180 / Math.PI : undefined;
  const pitch = hasFrame ? pose?.pitch : reference ? relative?.pitch : valid ? Math.atan2(-sample.accel[0], Math.hypot(sample.accel[1], sample.accel[2])) * 180 / Math.PI : undefined;
  const chip = device.mcu.match(/^([a-zA-Z]+)(.*)$/);
  useEffect(() => {
    const board = boardRef.current;
    if (!board) return;
    if (roll === undefined || pitch === undefined) {
      motionRef.current = null;
      board.style.transitionDuration = "0ms";
      board.style.transform = `${basePose} rotateY(0deg) rotateX(0deg)`;
      return;
    }
    const previous = motionRef.current;
    const arrivedAt = performance.now();
    const sameSource = previous && previous.demo === demo && previous.board === device.board && previous.referenceKey === referenceKey;
    const continuousRoll = sameSource ? unwrapDegrees(previous.roll, roll) : roll;
    const duration = sameSource && !paused && !stale ? Math.max(100, Math.min(140, (arrivedAt - previous.arrivedAt) * 1.1)) : 0;
    // Interpolate only the drawing on the compositor; measurements and CSV stay untouched.
    board.style.transitionDuration = `${duration}ms`;
    board.style.transform = poseTransform || `${basePose} rotateY(${pitch}deg) rotateX(${continuousRoll}deg)`;
    motionRef.current = { roll: continuousRoll, arrivedAt, demo, board: device.board, referenceKey };
  }, [roll, pitch, demo, stale, paused, sample?.t, device.board, referenceKey, basePose, poseTransform]);
  return <section className="imu-card imu-orientation" id="orientation">
    <div className="imu-card-top"><div className="imu-card-title"><span className="imu-iconbox"><Icon name="layers" /></span><h2>보드 자세<span>{hasFrame ? "BOARD TILT" : reference ? "RELATIVE TILT" : "ORIENTATION"}</span></h2></div><span className="imu-small-pill">{demo ? "DEMO" : hasFrame ? "바닥 기준" : "3D VIEW"}</span></div>
    <div className="imu-board-scene" data-physical={hasFrame || undefined} aria-label={sample ? `${hasFrame ? "바닥 기준 보드" : reference ? "저장 자세 대비 상대" : "추정"} 기울기 롤 ${fixed(roll, 1)}도, 피치 ${fixed(pitch, 1)}도` : "보드 자세 미리보기. 센서 샘플 수신 전입니다."}>
      <div className="imu-scene-glow" /><div className="imu-scene-grid" /><div className="imu-orbit orbit-one" /><div className="imu-orbit orbit-two" />
      <span className="imu-scene-label">{device.name} <i />{demo ? "EXAMPLE" : paused ? "PAUSED" : stale ? "LAST SAMPLE" : sample ? "TRACKING" : "STANDBY"}</span>
      <div className="imu-board-camera" data-physical={hasFrame || undefined}>
      <div ref={boardRef} className={`imu-board ${sample ? "has-sample" : ""}`}>
        <span className="imu-board-hole h1" /><span className="imu-board-hole h2" /><span className="imu-board-hole h3" /><span className="imu-board-hole h4" />
        <div className="imu-board-pins left">{Array.from({ length: 12 }, (_, i) => <i key={i} />)}</div><div className="imu-board-pins right">{Array.from({ length: 12 }, (_, i) => <i key={i} />)}</div>
        <div className="imu-board-traces" /><div className="imu-board-antenna" />
        <span className="imu-board-name">{device.board === "unknown" ? "NU" : device.board.toUpperCase()}<span>DEVELOPMENT KIT</span></span><span className="imu-board-soc">{chip?.[1] || device.mcu}<span>{chip?.[2]}</span></span><span className="imu-board-sensor">IMU</span><span className="imu-board-led" /><span className="imu-board-reset" /><span className="imu-board-usb" /><span className="imu-board-cap c1" /><span className="imu-board-cap c2" /><span className="imu-board-cap c3" />
      </div>
      </div>
      <svg className="imu-axis-compass" width="74" height="69" viewBox="0 0 74 69" aria-hidden="true"><path d={hasFrame ? "M31 43 61 35" : "M31 43 60 54"} stroke={AXIS_COLORS[0]} /><path d={hasFrame ? "M31 43 49 57" : "M31 43 10 55"} stroke={AXIS_COLORS[1]} /><path d="M31 43V10" stroke={AXIS_COLORS[2]} /><circle cx="31" cy="43" r="3" fill="#a7b8c9" /><text x="63" y={hasFrame ? "36" : "58"} fill={AXIS_COLORS[0]}>X</text><text x={hasFrame ? "52" : "0"} y={hasFrame ? "63" : "61"} fill={AXIS_COLORS[1]}>Y</text><text x="27" y="8" fill={AXIS_COLORS[2]}>Z</text></svg>
      {!sample && <span className="imu-orientation-wait">샘플 수신 후 자세가 반영됩니다</span>}
    </div>
    <div className="imu-angle-values"><div><span><i style={{ background: AXIS_COLORS[0] }} />ROLL</span><strong>{fixed(roll, 1)}<small>°</small></strong></div><div><span><i style={{ background: AXIS_COLORS[1] }} />PITCH</span><strong>{fixed(pitch, 1)}<small>°</small></strong></div><div><span>크기</span><strong>{sample ? fixed(Math.hypot(...sample.accel), 2) : "—"}<small>g</small></strong></div></div>
    <div className="imu-orientation-note"><Icon name="target" size={15} /><span>{hasFrame ? "바닥 기준 · USB-C 축 정렬" : reference ? "저장 자세 대비 상대 기울기" : "가속도 기반 추정 기울기"}<small>{hasFrame ? "평평한 바닥이 0°입니다. 화면은 고정 시점이며 방위각은 측정하지 않습니다." : reference ? "바닥 기준은 미설정입니다. 움직임의 영향을 받으며 방위각은 측정하지 않습니다." : "정지 상태에서 확인하세요. 움직임의 영향을 받습니다."}</small></span></div>
    <div className="imu-board-caption"><span>6-AXIS INERTIAL SENSOR</span><span>{demo ? "예시 데이터" : paused ? "화면 일시정지" : stale ? "수신 지연 · 마지막 자세" : sample ? "실측 데이터" : "실측 대기"}</span></div>
  </section>;
});

const SerialConsole = memo(function SerialConsole({ logs, demo, connected, port, firmware, device }: { logs: Log[]; demo: boolean; connected: boolean; port: string | null; firmware: string | null; device: DeviceMetadata }) {
  const [autoScroll, setAutoScroll] = useState(true);
  const logsRef = useRef<HTMLDivElement>(null);
  useEffect(() => { if (autoScroll && logsRef.current) logsRef.current.scrollTop = logsRef.current.scrollHeight; }, [logs, autoScroll]);
  return <section className="imu-card imu-console" id="console"><div className="imu-card-top"><div className="imu-card-title"><span className="imu-iconbox"><Icon name="terminal" /></span><h2>시리얼 콘솔<span>{demo ? "DEMO LOG" : "RAW LOG"}</span></h2></div><label className="imu-autoscroll"><input type="checkbox" checked={autoScroll} onChange={(event) => setAutoScroll(event.target.checked)} />자동 스크롤</label></div><div className="imu-console-toolbar"><span><i className={`imu-dot ${connected ? "mint" : "muted"}`} />{demo ? "DEMO OUTPUT" : port || "WAITING FOR DEVICE"}</span><span>{device.baud?.toLocaleString("en-US") || "—"} BAUD</span></div><div className="imu-log-window" ref={logsRef} role="log" aria-label={demo ? "데모 예시 로그" : "보드 시리얼 로그"} aria-live="off">{logs.length ? logs.map((log, i) => <div className={`imu-log-line ${log.level}`} key={`${log.t}-${i}`}><time>{log.t ? timeLabel(log.t) : "--:--:--"}</time><span>{log.level.toUpperCase()}</span><code>{log.text}</code></div>) : <div className="imu-console-empty"><Icon name="terminal" size={23} /><strong>첫 번째 메시지를 기다리고 있어요</strong><span>보드에서 수신한 원본 로그가 여기에 표시됩니다.</span><code><i /> waiting for serial output</code></div>}</div><div className="imu-console-footer"><span>{logs.length} lines<span className="imu-strip-divider" />{demo ? "예시 로그" : "최근 100줄"}</span><span>{firmware ? `FW ${firmware}` : `${device.name} / ${device.mcu}`}</span></div></section>;
});

export default function ImuDashboard() {
  const [snapshot, setSnapshot] = useState<Snapshot>(EMPTY);
  const [quality, setQuality] = useState<GraphicQuality>("low");
  const [demo, setDemo] = useState(false);
  const [demoSamples, setDemoSamples] = useState<Sample[]>([]);
  const [paused, setPaused] = useState(false);
  const [frozenSamples, setFrozenSamples] = useState<Sample[]>([]);
  const [commandBusy, setCommandBusy] = useState(false);
  const [commandNotice, setCommandNotice] = useState("");
  const [now, setNow] = useState(0);
  const [csvExport, setCsvExport] = useState<{ filename: string; text: string; url: string; rows: number; demo: boolean } | null>(null);
  const [copyNotice, setCopyNotice] = useState("");
  const csvDialogRef = useRef<HTMLDialogElement>(null);
  const csvTextRef = useRef<HTMLTextAreaElement>(null);
  const selectedBoardRef = useRef<string | null>(null);
  const sourceSamples = demo ? demoSamples : snapshot.samples;
  const samples = paused ? frozenSamples : sourceSamples;
  const latest = samples.at(-1);
  const device = snapshot.device || UNKNOWN_DEVICE;
  const supportsBusLevels = device.board === "nu40";
  const supportsCommands = device.board === "nu40";
  const commandUnavailable = supportsCommands ? undefined : "이 펌웨어에서는 보드 명령을 사용할 수 없습니다.";
  const busLevelNote = supportsBusLevels ? undefined : "이 보드에서는 I²C 사용 중 SDA/SCL의 전압 상태를 확인할 수 없습니다.";
  const connected = snapshot.connection.state === "connected";
  const sensorWarning = connected && !snapshot.sensor.ready;
  const lastDeviceSample = snapshot.samples.at(-1);
  const stale = Boolean(!demo && lastDeviceSample && (!connected || now - lastDeviceSample.t > 2500));

  useEffect(() => { const timer = window.setInterval(() => setNow(Date.now()), 1000); return () => window.clearInterval(timer); }, []);

  useEffect(() => {
    let events: EventSource | null = null;
    let reconnectTimer: number | undefined;
    let stopped = false;
    function connect() {
      if (stopped) return;
      const stream = new EventSource("/api/imu/events");
      events = stream;
      stream.onmessage = (event) => {
      try {
        const next = JSON.parse(event.data) as Snapshot;
        if (!next.connection || !next.sensor || !Array.isArray(next.samples) || !Array.isArray(next.logs)) return;
        const incomingDevice = next.device || LEGACY_NU40;
        if (selectedBoardRef.current && selectedBoardRef.current !== incomingDevice.board) {
          setPaused(false);
          setFrozenSamples([]);
        }
        selectedBoardRef.current = incomingDevice.board;
        setSnapshot((previous) => {
          const incoming = next.samples.slice(-1500);
          const logs = next.logs.slice(-100);
          const last = incoming.at(-1);
          const previousLast = previous.samples.at(-1);
          const sameSamples = incomingDevice.board === previous.device?.board && incoming.length === previous.samples.length && last?.t === previousLast?.t && last?.seq === previousLast?.seq && incoming[0]?.t === previous.samples[0]?.t;
          const sameLogs = logs.length === previous.logs.length && logs.every((log, index) => log.t === previous.logs[index].t && log.level === previous.logs[index].level && log.text === previous.logs[index].text);
          const sameDevice = JSON.stringify(incomingDevice) === JSON.stringify(previous.device);
          const incomingReference = next.orientationReference || null;
          const sameReference = JSON.stringify(incomingReference) === JSON.stringify(previous.orientationReference);
          const incomingCalibration = next.calibration || null;
          const sameCalibration = JSON.stringify(incomingCalibration) === JSON.stringify(previous.calibration);
          return { ...next, device: sameDevice ? previous.device : incomingDevice, orientationReference: sameReference ? previous.orientationReference : incomingReference, calibration: sameCalibration ? previous.calibration : incomingCalibration, samples: sameSamples && sameCalibration ? previous.samples : incoming, logs: sameLogs ? previous.logs : logs };
        });
      } catch { /* An interrupted event is retried by EventSource. */ }
      };
      stream.onerror = () => {
        if (stopped || events !== stream) return;
        setSnapshot((previous) => ({ ...previous, connection: { ...previous.connection, state: "disconnected", message: "로컬 브리지를 기다리고 있습니다." } }));
        // HTTP errors can permanently close EventSource; CONNECTING retains native retry.
        if (stream.readyState === EventSource.CLOSED && reconnectTimer === undefined) {
          stream.close();
          reconnectTimer = window.setTimeout(() => {
            reconnectTimer = undefined;
            connect();
          }, 2000);
        }
      };
    }
    connect();
    return () => {
      stopped = true;
      if (reconnectTimer !== undefined) window.clearTimeout(reconnectTimer);
      events?.close();
    };
  }, []);

  useEffect(() => {
    if (!demo) return;
    let seq = 188;
    const interval = window.setInterval(() => setDemoSamples((previous) => [...previous.slice(-1499), makeDemo(Date.now(), seq++)]), 80);
    return () => window.clearInterval(interval);
  }, [demo]);

  useEffect(() => {
    if (!csvExport) return;
    if (!csvDialogRef.current?.open) csvDialogRef.current?.showModal();
    return () => URL.revokeObjectURL(csvExport.url);
  }, [csvExport]);

  function toggleDemo() {
    if (!demo) { const startedAt = Date.now(); setDemoSamples(Array.from({ length: 188 }, (_, i) => makeDemo(startedAt - (187 - i) * 80, i))); }
    setDemo((v) => !v); setPaused(false); setFrozenSamples([]); setCommandNotice("");
  }
  function togglePause() { if (!paused) setFrozenSamples(sourceSamples); setPaused((v) => !v); }
  function downloadCsv() {
    if (!samples.length) return;
    const header = "timestamp,sequence,accel_x_g,accel_y_g,accel_z_g,gyro_x_dps,gyro_y_dps,gyro_z_dps,corrected_x_dps,corrected_y_dps,corrected_z_dps,source,board";
    const rows = samples.map((s) => [new Date(s.t).toISOString(), s.seq, ...s.accel, ...s.gyro, ...s.corrected, demo ? "demo" : "device", device.board].join(","));
    const text = `${header}\n${rows.join("\n")}`;
    const url = URL.createObjectURL(new Blob([text], { type: "text/csv;charset=utf-8" }));
    const filename = `${device.board === "unknown" ? "nu" : device.board}-imu-${demo ? "demo" : "live"}-${new Date().toISOString().replace(/[:.]/g, "-")}.csv`;
    setCsvExport({ filename, text, url, rows: rows.length, demo });
    setCopyNotice("");
  }
  async function copyCsv() {
    if (!csvExport) return;
    try { await navigator.clipboard.writeText(csvExport.text); setCopyNotice("CSV를 복사했습니다. 스프레드시트나 텍스트 파일에 붙여 넣으세요."); }
    catch { csvTextRef.current?.focus(); csvTextRef.current?.select(); setCopyNotice("내용을 선택했습니다. ⌘C 또는 Ctrl+C로 복사해 주세요."); }
  }
  async function sendCommand(command: string) {
    setCommandBusy(true); setCommandNotice("");
    try {
      const response = await fetch("/api/imu/command", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ command }) });
      const result = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(result.error || "명령을 전달하지 못했습니다.");
      setCommandNotice(command === "imu retry" ? "초기화 재시도 명령을 전달했습니다." : "센서 상태를 요청했습니다.");
    } catch (error) { setCommandNotice(error instanceof Error ? error.message : "브리지 연결을 확인해 주세요."); }
    finally { setCommandBusy(false); }
  }
  const sensor = snapshot.sensor;
  const statusText = demo ? "데모 모드" : connected ? sensor.ready ? stale ? "샘플 수신 지연" : snapshot.samples.length ? "센서 수신 중" : "샘플 수신 대기" : "센서 확인 필요" : snapshot.connection.state === "connecting" ? "연결 중" : "연결 대기";
  const visibleLogs = demo ? DEMO_LOGS : snapshot.logs;

  return <main className="imu-dashboard" data-quality={quality}>
    <header className="imu-topbar"><a href="/imu" className="imu-brand" aria-label="NU Motion Lab 홈"><span className="imu-brand-mark">nu<span>·</span></span><span>MOTION<span>LAB</span></span></a><nav aria-label="대시보드 탐색"><a className="active" href="#signals">모니터링</a><a href="#device">장치 정보</a><a href="#console">시리얼 로그</a></nav><div className="imu-local"><span className="imu-dot mint" />LOCAL WORKSPACE<span className="imu-top-divider" /><Icon name="chip" size={15} /><span>{device.name}</span></div></header>
    <div className="imu-content">
      <section className="imu-heading" id="signals"><div><div className="imu-eyebrow"><span />SENSOR WORKSPACE <span className="imu-eyebrow-slash">/</span> 01</div><h1>움직임을, 한눈에<span>.</span></h1><p>{device.name}의 가속도와 각속도를 실시간으로 살펴보세요.</p></div><div className="imu-heading-actions"><label className="imu-quality-control" title="차트의 표시 밀도와 장식 효과를 조절합니다. 측정값과 CSV 원본은 유지됩니다."><span>그래픽 품질</span><select value={quality} onChange={(event) => setQuality(event.target.value as GraphicQuality)} aria-label="그래픽 품질"><option value="low">저부하</option><option value="high">선명</option></select></label><button className={`imu-button imu-demo-button ${demo ? "selected" : ""}`} onClick={toggleDemo}><Icon name={demo ? "cable" : "play"} size={15} />{demo ? "실제 센서로 돌아가기" : "데모 둘러보기"}</button><button className="imu-button primary" onClick={downloadCsv} disabled={!samples.length}><Icon name="download" size={16} />CSV 내보내기</button></div></section>

      <section className={`imu-status-strip ${demo ? "demo" : sensorWarning || stale ? "warning" : ""}`} aria-label="데이터 수신 상태"><div className="imu-status-main"><span className={`imu-dot ${demo ? "violet" : connected && sensor.ready && !stale ? "mint" : sensorWarning || stale ? "amber" : "muted"}`} /><strong>{statusText}</strong><span className="imu-strip-divider" /><span className="imu-status-description">{demo ? "예시 데이터로 시각화를 확인하고 있습니다." : connected ? snapshot.connection.port?.split("/").at(-1) || "USB 시리얼 연결됨" : "USB 연결과 로컬 브리지 상태를 확인하세요"}</span></div><div className="imu-status-details"><span>{demo ? "예시" : "수신"} <strong>{demo ? "12.5" : connected && !stale && sensor.sampleHz ? fixed(sensor.sampleHz, 1) : "—"}</strong> Hz</span><span className="imu-strip-divider" /><span><strong>{samples.length.toLocaleString()}</strong> samples</span><button className="imu-icon-button" onClick={togglePause} aria-label={paused ? "화면 업데이트 다시 시작" : "화면 업데이트 일시정지"} title={paused ? "화면 업데이트 다시 시작" : "화면 업데이트 일시정지"} disabled={!sourceSamples.length}><Icon name={paused ? "play" : "pause"} size={16} /></button></div></section>

      {demo && <div className="imu-demo-notice" role="status"><Icon name="layers" size={16} /><span><strong>데모 · 예시 데이터</strong> 실제 센서 측정값이 아닙니다. 기기 연결 없이 화면을 미리 볼 수 있습니다.</span><button onClick={toggleDemo}>데모 종료 <Icon name="arrow" size={14} /></button></div>}
      {paused && <div className="imu-pause-notice" role="status">화면이 일시정지되었습니다. 장치의 데이터 수신은 계속됩니다.<button onClick={togglePause}>다시 시작</button></div>}

      <div className="imu-main-grid"><Board sample={latest} demo={demo} stale={stale} paused={paused} device={device} orientationReference={snapshot.orientationReference} /><div className="imu-charts"><SignalChart kind="accel" samples={samples} demo={demo} paused={paused} stale={stale} quality={quality} /><SignalChart kind="gyro" samples={samples} demo={demo} paused={paused} stale={stale} quality={quality} calibrationLabel={!demo ? snapshot.calibration?.label : undefined} /></div></div>

      <div className="imu-bottom-grid"><section className="imu-card imu-device" id="device"><div className="imu-card-top"><div className="imu-card-title"><span className="imu-iconbox"><Icon name="chip" /></span><h2>장치 연결<span>DEVICE HEALTH</span></h2></div><span className="imu-small-pill">실제 장치</span></div>
        <div className="imu-health-row"><span>USB 시리얼</span><strong className={connected ? "imu-mint-text" : "imu-muted-text"}><i className={`imu-dot ${connected ? "mint" : "muted"}`} />{connected ? "연결됨" : "연결 대기"}</strong></div><div className="imu-health-row"><span>IMU 센서</span><strong className={connected && sensor.ready ? "imu-mint-text" : sensorWarning ? "imu-amber-text" : "imu-muted-text"}><i className={`imu-dot ${connected && sensor.ready ? "mint" : sensorWarning ? "amber" : "muted"}`} />{connected && sensor.ready ? "정상" : sensorWarning ? "초기화 확인 필요" : "응답 대기"}</strong></div>
        {snapshot.calibration?.source === "host" && <div className="imu-health-row"><span>자이로 보정</span><strong className={connected ? "imu-mint-text" : "imu-muted-text"}>{connected ? snapshot.calibration.label : "맥북 보정 준비"}</strong></div>}
        <div className="imu-bus-grid"><div><span>I²C ADDRESS</span><strong>{connected ? sensor.address || "—" : "—"}</strong></div><div><span>SDA <small>{device.pins.sda}</small></span><strong title={busLevelNote} className={connected && supportsBusLevels && sensor.sda === 0 ? "imu-amber-text" : ""}>{!connected ? "—" : !supportsBusLevels ? "확인 불가" : sensor.sda === null ? "—" : sensor.sda ? "HIGH" : "LOW"}</strong></div><div><span>SCL <small>{device.pins.scl}</small></span><strong title={busLevelNote} className={connected && supportsBusLevels && sensor.scl === 0 ? "imu-amber-text" : ""}>{!connected ? "—" : !supportsBusLevels ? "확인 불가" : sensor.scl === null ? "—" : sensor.scl ? "HIGH" : "LOW"}</strong></div></div>
        <div className={`imu-health-note ${sensorWarning ? "warning" : ""}`}><Icon name={sensorWarning ? "alert" : "cable"} size={16} /><span>{sensorWarning ? supportsBusLevels && sensor.scl === 0 ? `SCL이 LOW 상태입니다. USB 전원을 분리한 뒤 ${device.pins.scl} 연결과 센서 전원을 확인하세요.` : `센서 초기화가 완료되지 않았습니다.${sensor.lastError !== null ? ` 오류 ${sensor.lastError}.` : ""} 연결을 확인하고 다시 시도하세요.` : connected && sensor.ready ? "센서가 응답하고 있습니다. 보드를 천천히 움직여 변화를 확인하세요." : `${device.name}를 USB로 연결하고 해당 보드의 로컬 브리지를 실행해 주세요.`}</span></div>
        <div className="imu-device-actions"><button className="imu-button" title={commandUnavailable} disabled={!connected || !supportsCommands || commandBusy} onClick={() => sendCommand("imu retry")}><Icon name="refresh" size={14} />{commandBusy ? "전송 중…" : "센서 다시 연결"}</button><button className="imu-text-button" title={commandUnavailable} disabled={!connected || !supportsCommands || commandBusy} onClick={() => sendCommand("imu status")}>상태 확인 <Icon name="arrow" size={14} /></button></div><div className="imu-command-notice" role="status">{commandNotice}</div>
      </section>

      <SerialConsole logs={visibleLogs} demo={demo} connected={connected} port={snapshot.connection.port} firmware={snapshot.connection.firmware} device={device} /></div>

      <footer className="imu-footer"><span className="imu-footer-brand">nu<span>·</span></span><span>작은 움직임에서 시작되는 가능성.</span><span className="imu-footer-right"><span className="imu-dot muted" />{demo ? "데모 모드 · 예시 데이터" : snapshot.updatedAt ? `마지막 수신 ${timeLabel(snapshot.updatedAt)}` : "LOCAL SENSOR MONITOR"}</span></footer>
    </div>
    <dialog className="imu-export-dialog" ref={csvDialogRef} onClose={() => setCsvExport(null)} aria-labelledby="imu-export-title" aria-describedby="imu-export-description">
      {csvExport && <><div className="imu-export-heading"><span className="imu-iconbox accel"><Icon name="download" /></span><div><h2 id="imu-export-title">CSV 내보내기</h2><p id="imu-export-description">현재 보관된 샘플을 파일로 저장하거나 복사하세요.</p></div><button className="imu-export-close" onClick={() => csvDialogRef.current?.close()} aria-label="CSV 미리보기 닫기">×</button></div>
        <div className={`imu-export-source ${csvExport.demo ? "demo" : ""}`}><span className={`imu-dot ${csvExport.demo ? "violet" : "mint"}`} /><strong>{csvExport.demo ? "데모 · 예시 데이터" : "실제 센서 측정값"}</strong><span>{csvExport.rows.toLocaleString()} samples</span></div>
        <label className="imu-export-filename" htmlFor="imu-export-csv">{csvExport.filename}</label><textarea id="imu-export-csv" ref={csvTextRef} className="imu-export-text" value={csvExport.text} readOnly spellCheck={false} wrap="off" aria-label="내보낼 CSV 내용" />
        <p className="imu-export-help">파일 저장이 지원되지 않는 브라우저에서는 CSV 복사를 이용하세요.</p><div className="imu-export-actions"><span className="imu-export-notice" role="status">{copyNotice}</span><button className="imu-button" onClick={copyCsv}><Icon name="layers" size={15} />CSV 복사</button><a className="imu-button primary" href={csvExport.url} download={csvExport.filename}><Icon name="download" size={15} />파일 저장</a></div></>}
    </dialog>
  </main>;
}
