"use client";

import { useCallback, useEffect, useRef, useState } from "react";

const SERVICE_UUID = "7b1e0001-21a8-4f8c-bb2d-4c7d2156f001";
const SENSOR_UUID = "7b1e0002-21a8-4f8c-bb2d-4c7d2156f001";
const COMMAND_UUID = "7b1e0003-21a8-4f8c-bb2d-4c7d2156f001";

type ConnectionState = "idle" | "connecting" | "connected" | "unsupported" | "error";
type InputSource = "마우스" | "IMU" | "터치" | "시스템";

type RoomState = {
  name: string;
  device: string;
  className: string;
  on: boolean;
};

type SensorData = {
  sequence: number;
  touch: boolean;
  touchMv: number;
  accel: [number, number, number];
  gyro: [number, number, number];
};

type Activity = {
  id: number;
  time: string;
  source: InputSource;
  message: string;
};

interface GattCharacteristic extends EventTarget {
  startNotifications(): Promise<GattCharacteristic>;
  writeValueWithoutResponse?(value: BufferSource): Promise<void>;
  writeValue?(value: BufferSource): Promise<void>;
  value?: DataView;
}

interface GattService {
  getCharacteristic(uuid: string): Promise<GattCharacteristic>;
}

interface GattServer {
  getPrimaryService(uuid: string): Promise<GattService>;
}

interface BluetoothDeviceLike extends EventTarget {
  name?: string;
  gatt?: {
    connected: boolean;
    connect(): Promise<GattServer>;
    disconnect(): void;
  };
}

type BluetoothNavigator = Navigator & {
  bluetooth?: {
    requestDevice(options: {
      filters: Array<{ services: string[] }>;
    }): Promise<BluetoothDeviceLike>;
  };
};

const initialRooms: RoomState[] = [
  { name: "거실", device: "메인 조명", className: "living", on: true },
  { name: "주방", device: "펜던트 조명", className: "kitchen", on: false },
  { name: "침실", device: "무드등", className: "bedroom", on: false },
  { name: "현관", device: "도어 램프", className: "entry", on: true },
];

const emptySensor: SensorData = {
  sequence: 0,
  touch: false,
  touchMv: 0,
  accel: [0, 0, 0],
  gyro: [0, 0, 0],
};

const timeLabel = () =>
  new Intl.DateTimeFormat("ko-KR", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  }).format(new Date());

export default function HomeLab() {
  const [rooms, setRooms] = useState(initialRooms);
  const [selectedRoom, setSelectedRoom] = useState(0);
  const [sensor, setSensor] = useState(emptySensor);
  const [connection, setConnection] = useState<ConnectionState>("idle");
  const [connectionMessage, setConnectionMessage] = useState("보드 연결 대기");
  const [calibrated, setCalibrated] = useState(false);
  const [activities, setActivities] = useState<Activity[]>([
    { id: 1, time: "—", source: "시스템", message: "가상 주택이 준비됐습니다." },
  ]);

  const roomsRef = useRef(rooms);
  const selectedRoomRef = useRef(selectedRoom);
  const commandRef = useRef<GattCharacteristic | null>(null);
  const sensorRef = useRef<GattCharacteristic | null>(null);
  const deviceRef = useRef<BluetoothDeviceLike | null>(null);
  const notificationHandlerRef = useRef<((event: Event) => void) | null>(null);
  const activityIdRef = useRef(2);
  const previousTouchRef = useRef(false);
  const tiltLatchedRef = useRef(false);
  const lastTouchAtRef = useRef(0);
  const calibrationRef = useRef({ count: 0, x: 0, y: 0, z: 0, baseline: [0, 0, 0] as [number, number, number] });

  useEffect(() => {
    roomsRef.current = rooms;
  }, [rooms]);

  useEffect(() => {
    selectedRoomRef.current = selectedRoom;
  }, [selectedRoom]);

  const addActivity = useCallback((source: InputSource, message: string) => {
    const item: Activity = {
      id: activityIdRef.current++,
      time: timeLabel(),
      source,
      message,
    };
    setActivities((current) => [item, ...current].slice(0, 6));
  }, []);

  const sendHouseCommand = useCallback(async (roomIndex: number, on: boolean) => {
    const characteristic = commandRef.current;
    if (!characteristic) return;

    const payload = new Uint8Array([0x01, roomIndex, on ? 1 : 0]);
    try {
      if (characteristic.writeValueWithoutResponse) {
        await characteristic.writeValueWithoutResponse(payload);
      } else if (characteristic.writeValue) {
        await characteristic.writeValue(payload);
      }
    } catch {
      addActivity("시스템", "보드 LED 동기화에 실패했습니다.");
    }
  }, [addActivity]);

  const setRoomPower = useCallback((roomIndex: number, on: boolean, source: InputSource) => {
    const room = roomsRef.current[roomIndex];
    if (!room || room.on === on) return;

    const nextRooms = roomsRef.current.map((item, index) =>
      index === roomIndex ? { ...item, on } : item,
    );
    roomsRef.current = nextRooms;
    setRooms(nextRooms);
    addActivity(source, `${room.name} ${room.device}을 ${on ? "켰습니다" : "껐습니다"}.`);
    void sendHouseCommand(roomIndex, on);
  }, [addActivity, sendHouseCommand]);

  const toggleRoom = useCallback((roomIndex: number, source: InputSource) => {
    const room = roomsRef.current[roomIndex];
    if (!room) return;
    setRoomPower(roomIndex, !room.on, source);
  }, [setRoomPower]);

  const selectRoom = useCallback((roomIndex: number, source: InputSource) => {
    const normalized = (roomIndex + roomsRef.current.length) % roomsRef.current.length;
    if (normalized === selectedRoomRef.current) return;
    selectedRoomRef.current = normalized;
    setSelectedRoom(normalized);
    addActivity(source, `${roomsRef.current[normalized].name}을 선택했습니다.`);
  }, [addActivity]);

  const resetCalibration = useCallback(() => {
    calibrationRef.current = { count: 0, x: 0, y: 0, z: 0, baseline: [0, 0, 0] };
    tiltLatchedRef.current = false;
    setCalibrated(false);
    addActivity("시스템", "IMU 기준 자세를 다시 측정합니다. 보드를 잠시 고정하세요.");
  }, [addActivity]);

  const processSensorPacket = useCallback((value: DataView) => {
    if (value.byteLength < 18 || value.getUint8(0) !== 1) return;

    const nextSensor: SensorData = {
      sequence: value.getUint16(2, true),
      touch: (value.getUint8(1) & 0x01) !== 0,
      touchMv: value.getUint16(4, true),
      accel: [
        value.getInt16(6, true),
        value.getInt16(8, true),
        value.getInt16(10, true),
      ],
      gyro: [
        value.getInt16(12, true) / 100,
        value.getInt16(14, true) / 100,
        value.getInt16(16, true) / 100,
      ],
    };
    setSensor(nextSensor);

    const calibration = calibrationRef.current;
    if (calibration.count < 12) {
      calibration.x += nextSensor.accel[0];
      calibration.y += nextSensor.accel[1];
      calibration.z += nextSensor.accel[2];
      calibration.count += 1;
      if (calibration.count === 12) {
        calibration.baseline = [
          Math.round(calibration.x / 12),
          Math.round(calibration.y / 12),
          Math.round(calibration.z / 12),
        ];
        setCalibrated(true);
        addActivity("시스템", "IMU 기준 자세 보정이 완료됐습니다.");
      }
    } else {
      const deltaX = nextSensor.accel[0] - calibration.baseline[0];
      if (Math.abs(deltaX) < 220) {
        tiltLatchedRef.current = false;
      } else if (Math.abs(deltaX) > 480 && !tiltLatchedRef.current) {
        tiltLatchedRef.current = true;
        selectRoom(selectedRoomRef.current + (deltaX > 0 ? 1 : -1), "IMU");
      }
    }

    const now = Date.now();
    if (nextSensor.touch && !previousTouchRef.current && now - lastTouchAtRef.current > 350) {
      lastTouchAtRef.current = now;
      toggleRoom(selectedRoomRef.current, "터치");
    }
    previousTouchRef.current = nextSensor.touch;
  }, [addActivity, selectRoom, toggleRoom]);

  const handleDisconnected = useCallback(() => {
    commandRef.current = null;
    sensorRef.current = null;
    setConnection("idle");
    setConnectionMessage("연결 끊김 · 다시 연결 가능");
    setCalibrated(false);
    addActivity("시스템", "Bluetooth 연결이 끊겼습니다.");
  }, [addActivity]);

  const disconnect = useCallback(() => {
    deviceRef.current?.gatt?.disconnect();
    handleDisconnected();
  }, [handleDisconnected]);

  const connect = useCallback(async () => {
    if (connection === "connected") {
      disconnect();
      return;
    }

    const bluetooth = (navigator as BluetoothNavigator).bluetooth;
    if (!bluetooth) {
      setConnection("unsupported");
      setConnectionMessage("Chrome 또는 Edge에서 열어주세요");
      addActivity("시스템", "이 브라우저는 Web Bluetooth를 지원하지 않습니다.");
      return;
    }

    try {
      setConnection("connecting");
      setConnectionMessage("NU40-DK 검색 중");
      const device = await bluetooth.requestDevice({
        filters: [{ services: [SERVICE_UUID] }],
      });
      deviceRef.current = device;
      device.addEventListener("gattserverdisconnected", handleDisconnected);

      const server = await device.gatt?.connect();
      if (!server) throw new Error("GATT server unavailable");
      const service = await server.getPrimaryService(SERVICE_UUID);
      const sensorCharacteristic = await service.getCharacteristic(SENSOR_UUID);
      const commandCharacteristic = await service.getCharacteristic(COMMAND_UUID);

      const notificationHandler = (event: Event) => {
        const characteristic = event.target as GattCharacteristic;
        if (characteristic.value) processSensorPacket(characteristic.value);
      };
      notificationHandlerRef.current = notificationHandler;
      sensorCharacteristic.addEventListener("characteristicvaluechanged", notificationHandler);
      await sensorCharacteristic.startNotifications();

      sensorRef.current = sensorCharacteristic;
      commandRef.current = commandCharacteristic;
      setConnection("connected");
      setConnectionMessage(device.name ?? "NU40 Home Remote");
      resetCalibration();
      addActivity("시스템", `${device.name ?? "NU40 Home Remote"}에 연결했습니다.`);

      for (let index = 0; index < roomsRef.current.length; index += 1) {
        await sendHouseCommand(index, roomsRef.current[index].on);
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : "연결 실패";
      if (message.toLowerCase().includes("cancel")) {
        setConnection("idle");
        setConnectionMessage("연결 취소됨");
      } else {
        setConnection("error");
        setConnectionMessage("연결 실패 · 다시 시도");
        addActivity("시스템", `Bluetooth 연결 실패: ${message}`);
      }
    }
  }, [addActivity, connection, disconnect, handleDisconnected, processSensorPacket, resetCalibration, sendHouseCommand]);

  const turnAllOff = useCallback(() => {
    roomsRef.current.forEach((room, index) => {
      if (room.on) setRoomPower(index, false, "마우스");
    });
  }, [setRoomPower]);

  const relativeX = calibrated ? sensor.accel[0] - calibrationRef.current.baseline[0] : 0;
  const relativeY = calibrated ? sensor.accel[1] - calibrationRef.current.baseline[1] : 0;
  const boardTransform = `perspective(400px) rotateX(${Math.max(-18, Math.min(18, relativeY / 35))}deg) rotateZ(${Math.max(-20, Math.min(20, relativeX / 32))}deg)`;
  const connectionLabel = connection === "connected" ? "연결 해제" : connection === "connecting" ? "연결 중…" : "Bluetooth 연결";

  return (
    <main className="app-shell">
      <header className="topbar">
        <div className="brand-block">
          <span className="brand-mark">NU</span>
          <div><p className="eyebrow">SENSOR REMOTE LAB</p><h1>NU Home</h1></div>
        </div>
        <div className={`connection-block ${connection}`}>
          <span className="status-dot" />
          <span>{connectionMessage}</span>
          <button type="button" className="connect-button" onClick={connect} disabled={connection === "connecting"}>{connectionLabel}</button>
        </div>
      </header>

      <section className="workspace">
        <div className="home-panel">
          <div className="section-heading">
            <div><p className="eyebrow">VIRTUAL HOUSE</p><h2>우리 집 시뮬레이션</h2></div>
            <div className="scene-actions">
              <span className="scene-pill">로컬 시뮬레이션</span>
              <button type="button" className="all-off-button" onClick={turnAllOff}>전체 끄기</button>
            </div>
          </div>

          <div className="house-grid" aria-label="가상 주택 방 목록">
            {rooms.map((room, index) => (
              <article className={`room ${room.className} ${selectedRoom === index ? "selected" : ""}`} key={room.name} onClick={() => selectRoom(index, "마우스")}>
                <div className="room-topline"><span className="room-number">0{index + 1}</span><span className={`device-light ${room.on ? "on" : ""}`} /></div>
                <div className="room-copy"><p>{room.device}</p><h3>{room.name}</h3>{selectedRoom === index && <span className="selected-label">리모컨 선택</span>}</div>
                <button type="button" className={`toggle ${room.on ? "is-on" : ""}`} onClick={(event) => { event.stopPropagation(); toggleRoom(index, "마우스"); }} aria-pressed={room.on} aria-label={`${room.name} ${room.device} ${room.on ? "끄기" : "켜기"}`}>
                  <span /><strong>{room.on ? "켜짐" : "꺼짐"}</strong>
                </button>
              </article>
            ))}
          </div>
        </div>

        <aside className="remote-panel">
          <div className="remote-heading"><p className="eyebrow">LIVE REMOTE</p><span className={`live-badge ${connection === "connected" ? "active" : ""}`}>{connection === "connected" ? "LIVE" : "OFFLINE"}</span></div>
          <h2>센서 리모컨</h2>
          <p className="remote-description">좌우로 기울여 방을 고르고, 터치 센서로 선택한 조명을 전환하세요.</p>

          <div className="imu-visual" aria-label="IMU 기울기 시각화">
            <div className="axis axis-x">X</div><div className="axis axis-y">Y</div><div className="axis axis-z">Z</div>
            <div className="imu-board" style={{ transform: boardTransform }}>NU40</div>
            <span className="selected-room-chip">선택 · {rooms[selectedRoom].name}</span>
          </div>

          <div className="sensor-grid six-axis">
            {sensor.accel.map((value, index) => <div key={`a${index}`}><span>ACC {"XYZ"[index]}</span><strong>{connection === "connected" ? value : "—"}</strong><small>mg</small></div>)}
            {sensor.gyro.map((value, index) => <div key={`g${index}`}><span>GYRO {"XYZ"[index]}</span><strong>{connection === "connected" ? value.toFixed(1) : "—"}</strong><small>°/s</small></div>)}
          </div>

          <div className={`touch-card ${sensor.touch ? "active" : ""}`}>
            <span className="touch-icon">◎</span>
            <div><p>터치 입력</p><strong>{sensor.touch ? "감지됨" : connection === "connected" ? "대기 중" : "연결 필요"}</strong></div>
            <span className="touch-value">{connection === "connected" ? `${sensor.touchMv} mV` : "D0 —"}</span>
          </div>

          <button type="button" className="calibrate-button" onClick={resetCalibration} disabled={connection !== "connected"}>
            {calibrated ? "기준 자세 다시 보정" : connection === "connected" ? "보정 중… 보드를 고정하세요" : "연결 후 자동 보정"}
          </button>
        </aside>
      </section>

      <section className="bottom-grid">
        <div className="gesture-panel">
          <div><p className="eyebrow">HOW TO CONTROL</p><h2>리모컨 사용법</h2></div>
          <ol>
            <li><span>01</span><strong>연결</strong><p>Chrome에서 Bluetooth 연결 버튼을 누릅니다.</p></li>
            <li><span>02</span><strong>선택</strong><p>IMU를 좌우로 기울여 방을 이동합니다.</p></li>
            <li><span>03</span><strong>실행</strong><p>터치 센서에 손을 대 조명을 전환합니다.</p></li>
          </ol>
        </div>
        <div className="activity-panel">
          <div><p className="eyebrow">EVENT LOG</p><h2>최근 동작</h2></div>
          <ul>{activities.map((activity) => <li key={activity.id}><span className="activity-source">{activity.source}</span><p>{activity.message}</p><time>{activity.time}</time></li>)}</ul>
        </div>
      </section>
    </main>
  );
}
