"use client";

import { useCallback, useEffect, useRef, useState } from "react";

const SERVICE_UUID = "7b1e0001-21a8-4f8c-bb2d-4c7d2156f001";
const SENSOR_UUID = "7b1e0002-21a8-4f8c-bb2d-4c7d2156f001";
const COMMAND_UUID = "7b1e0003-21a8-4f8c-bb2d-4c7d2156f001";

const DEFAULT_PROMPT = "우주를 좋아하는 아이가 색깔을 찾고 몸을 움직이는 게임";
const PRESETS = [
  "동물과 함께 1부터 4까지 세는 게임",
  "바닷속에서 동그라미와 세모를 찾는 게임",
  "숲속 친구들의 감정을 알아맞히는 게임",
];

type ConnectionState = "idle" | "connecting" | "connected" | "unsupported" | "error";
type InputSource = "마우스" | "키보드" | "IMU" | "터치" | "조이스틱" | "시스템";
type GameKind = "color" | "count" | "shape" | "emotion";
type ApprovalState = "draft" | "approved";

type PlayContext = {
  ageBand: string;
  space: string;
  goal: string;
  minutes: number;
  exclusions: string;
};

type Choice = {
  id: string;
  label: string;
  visual: string;
  color?: string;
  amount?: number;
};

type Round = {
  instruction: string;
  hint: string;
  answerId: string;
  choices: Choice[];
};

type GameSpec = {
  id: string;
  title: string;
  theme: string;
  kind: GameKind;
  skill: string;
  intro: string;
  context: PlayContext;
  rounds: Round[];
};

type Theme = {
  key: string;
  matches: string[];
  title: string;
  name: string;
  icons: string[];
  nouns: string[];
};

type SensorData = {
  touch: boolean;
  touchMv: number;
  accel: [number, number, number];
  joystick: [number, number];
  joystickPressed: boolean;
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

const DEFAULT_CONTEXT: PlayContext = {
  ageBand: "3–5세",
  space: "거실",
  goal: "색·관찰력",
  minutes: 10,
  exclusions: "달리기와 점프 없이",
};

const AGE_BANDS = ["0–1세", "1–2세", "2–3세", "3–5세"];
const PLAY_SPACES = ["거실", "아이 방", "복도", "마당"];
const GROWTH_GOALS = ["대근육·균형", "색·관찰력", "수 감각", "감정·언어", "협동·이야기"];
const PLAY_TIMES = [5, 7, 10, 15];

const THEMES: Theme[] = [
  {
    key: "space",
    matches: ["우주", "별", "로켓", "행성"],
    title: "별빛 우체국",
    name: "우주",
    icons: ["⭐", "🪐", "🚀", "🌍"],
    nouns: ["별", "행성", "로켓", "지구"],
  },
  {
    key: "animals",
    matches: ["동물", "강아지", "고양이", "토끼"],
    title: "숲속 친구 찾기",
    name: "동물",
    icons: ["🐰", "🐻", "🦊", "🐸"],
    nouns: ["토끼", "곰", "여우", "개구리"],
  },
  {
    key: "ocean",
    matches: ["바다", "물고기", "해변", "바닷속"],
    title: "바다 보물 탐험",
    name: "바다",
    icons: ["🐠", "🐙", "🐳", "🦀"],
    nouns: ["물고기", "문어", "고래", "게"],
  },
  {
    key: "forest",
    matches: ["숲", "나무", "꽃", "자연"],
    title: "초록 숲 소풍",
    name: "숲",
    icons: ["🌼", "🌳", "🍄", "🌿"],
    nouns: ["꽃", "나무", "버섯", "잎사귀"],
  },
  {
    key: "dinosaur",
    matches: ["공룡", "화석", "티라노"],
    title: "공룡 발자국 대작전",
    name: "공룡",
    icons: ["🦕", "🦖", "🥚", "🌋"],
    nouns: ["초식 공룡", "티라노", "공룡 알", "화산"],
  },
];

const COLORS = [
  { name: "노란", value: "#f3cf74" },
  { name: "보라", value: "#b5a1d8" },
  { name: "빨간", value: "#ef9c8b" },
  { name: "파란", value: "#87bdd8" },
];

const SHAPES = [
  { name: "동그라미", visual: "●" },
  { name: "세모", visual: "▲" },
  { name: "네모", visual: "■" },
  { name: "별", visual: "★" },
];

const EMOTIONS = [
  { name: "기쁜 얼굴", visual: "😄" },
  { name: "졸린 얼굴", visual: "😴" },
  { name: "슬픈 얼굴", visual: "😢" },
  { name: "놀란 얼굴", visual: "😮" },
];

const todayKey = () =>
  new Intl.DateTimeFormat("sv-SE", { timeZone: "Asia/Seoul" }).format(new Date());

const formatToday = (date: string) =>
  new Intl.DateTimeFormat("ko-KR", {
    timeZone: "Asia/Seoul",
    month: "long",
    day: "numeric",
    weekday: "short",
  }).format(new Date(`${date}T12:00:00+09:00`));

const hashText = (value: string) => {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
};

const randomFrom = (seed: number) => {
  let state = seed || 1;
  return () => {
    state += 0x6d2b79f5;
    let value = state;
    value = Math.imul(value ^ (value >>> 15), value | 1);
    value ^= value + Math.imul(value ^ (value >>> 7), value | 61);
    return ((value ^ (value >>> 14)) >>> 0) / 4294967296;
  };
};

const shuffled = <T,>(items: T[], random: () => number) => {
  const result = [...items];
  for (let index = result.length - 1; index > 0; index -= 1) {
    const swapIndex = Math.floor(random() * (index + 1));
    [result[index], result[swapIndex]] = [result[swapIndex], result[index]];
  }
  return result;
};

export const createDailyGame = (
  prompt: string,
  date: string,
  context: PlayContext = DEFAULT_CONTEXT,
): GameSpec => {
  const normalized = prompt.trim() || DEFAULT_PROMPT;
  const designPrompt = `${normalized} ${context.goal}`;
  const seed = hashText(`${date}:${designPrompt}:${context.ageBand}:${context.space}:${context.minutes}`);
  const random = randomFrom(seed);
  const theme = THEMES.find((item) =>
    item.matches.some((keyword) => designPrompt.includes(keyword)),
  ) ?? THEMES[seed % THEMES.length];

  let kind: GameKind;
  if (/숫자|세기|수학|몇 개|개수|수 감각/.test(designPrompt)) kind = "count";
  else if (/모양|도형|동그라미|세모|네모/.test(designPrompt)) kind = "shape";
  else if (/감정|마음|기분|표정|언어|이야기/.test(designPrompt)) kind = "emotion";
  else if (/색|컬러|무지개|관찰/.test(designPrompt)) kind = "color";
  else kind = (["color", "count", "shape", "emotion"] as GameKind[])[seed % 4];

  const skill = {
    color: "색깔 · 집중력",
    count: "1–4 세기 · 수 감각",
    shape: "모양 · 관찰력",
    emotion: "감정 · 언어 표현",
  }[kind];

  const rounds: Round[] = Array.from({ length: 4 }, (_, roundIndex) => {
    if (kind === "count") {
      const target = roundIndex + 1;
      const icon = theme.icons[roundIndex % theme.icons.length];
      const choices = Array.from({ length: 4 }, (_, index) => ({
        id: `count-${index + 1}`,
        label: `${index + 1}개`,
        visual: icon,
        amount: index + 1,
      }));
      return {
        instruction: `${icon} ${target}개가 있는 카드를 찾아주세요!`,
        hint: "천천히 하나씩 손가락으로 세어봐요.",
        answerId: `count-${target}`,
        choices: shuffled(choices, random),
      };
    }

    if (kind === "shape") {
      const target = SHAPES[roundIndex];
      const choices = SHAPES.map((shape, index) => ({
        id: `shape-${index}`,
        label: shape.name,
        visual: shape.visual,
        color: COLORS[(index + roundIndex) % COLORS.length].value,
      }));
      return {
        instruction: `${target.name} 모양을 찾아주세요!`,
        hint: "모서리와 둥근 부분을 잘 살펴봐요.",
        answerId: `shape-${roundIndex}`,
        choices: shuffled(choices, random),
      };
    }

    if (kind === "emotion") {
      const target = EMOTIONS[roundIndex];
      const choices = EMOTIONS.map((emotion, index) => ({
        id: `emotion-${index}`,
        label: emotion.name,
        visual: emotion.visual,
      }));
      return {
        instruction: `${target.name}을 찾아주세요!`,
        hint: "눈과 입 모양을 보고 같은 표정을 지어봐요.",
        answerId: `emotion-${roundIndex}`,
        choices: shuffled(choices, random),
      };
    }

    const targetIndex = roundIndex % COLORS.length;
    const choices = COLORS.map((color, index) => ({
      id: `color-${index}`,
      label: `${color.name} ${theme.nouns[index]}`,
      visual: theme.icons[index],
      color: color.value,
    }));
    return {
      instruction: `${COLORS[targetIndex].name}색 ${theme.nouns[targetIndex]} 카드를 찾아주세요!`,
      hint: "이름을 소리 내어 말하고 같은 색을 찾아봐요.",
      answerId: `color-${targetIndex}`,
      choices: shuffled(choices, random),
    };
  });

  return {
    id: `${date}-${seed}`,
    title: theme.title,
    theme: theme.name,
    kind,
    skill,
    intro: `${context.ageBand} · ${context.space}에서 ${context.minutes}분 동안 ${theme.name} 친구들과 놀아요.`,
    context: { ...context },
    rounds,
  };
};

const EMPTY_SENSOR: SensorData = {
  touch: false,
  touchMv: 0,
  accel: [0, 0, 0],
  joystick: [127, 127],
  joystickPressed: false,
};

export default function DailyGameLab() {
  const date = todayKey();
  const [prompt, setPrompt] = useState(DEFAULT_PROMPT);
  const [playContext, setPlayContext] = useState<PlayContext>(DEFAULT_CONTEXT);
  const [game, setGame] = useState(() => createDailyGame(DEFAULT_PROMPT, date, DEFAULT_CONTEXT));
  const [approval, setApproval] = useState<ApprovalState>("draft");
  const [roundIndex, setRoundIndex] = useState(0);
  const [selectedChoice, setSelectedChoice] = useState(0);
  const [score, setScore] = useState(0);
  const [complete, setComplete] = useState(false);
  const [feedback, setFeedback] = useState<{ kind: "correct" | "retry"; text: string } | null>(null);
  const [lastInput, setLastInput] = useState<InputSource>("시스템");
  const [connection, setConnection] = useState<ConnectionState>("idle");
  const [connectionMessage, setConnectionMessage] = useState("보드 연결 대기");
  const [sensor, setSensor] = useState(EMPTY_SENSOR);
  const [calibrated, setCalibrated] = useState(false);

  const gameRef = useRef(game);
  const roundIndexRef = useRef(roundIndex);
  const selectedChoiceRef = useRef(selectedChoice);
  const scoreRef = useRef(score);
  const lockedRef = useRef(false);
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const commandRef = useRef<GattCharacteristic | null>(null);
  const deviceRef = useRef<BluetoothDeviceLike | null>(null);
  const previousTouchRef = useRef(false);
  const previousJoystickButtonRef = useRef(false);
  const tiltLatchedRef = useRef(false);
  const joystickLatchedRef = useRef(false);
  const lastTouchAtRef = useRef(0);
  const lastJoystickButtonAtRef = useRef(0);
  const calibrationRef = useRef({
    count: 0,
    x: 0,
    y: 0,
    z: 0,
    joystickX: 0,
    joystickY: 0,
    baseline: [0, 0, 0] as [number, number, number],
    joystickBaseline: [127, 127] as [number, number],
  });

  useEffect(() => { gameRef.current = game; }, [game]);
  useEffect(() => { roundIndexRef.current = roundIndex; }, [roundIndex]);
  useEffect(() => { selectedChoiceRef.current = selectedChoice; }, [selectedChoice]);
  useEffect(() => { scoreRef.current = score; }, [score]);

  const writeLed = useCallback(async (index: number, on: boolean) => {
    const characteristic = commandRef.current;
    if (!characteristic) return;
    const payload = new Uint8Array([0x01, index, on ? 1 : 0]);
    try {
      if (characteristic.writeValueWithoutResponse) {
        await characteristic.writeValueWithoutResponse(payload);
      } else if (characteristic.writeValue) {
        await characteristic.writeValue(payload);
      }
    } catch {
      setConnectionMessage("연결됨 · LED 응답 없음");
    }
  }, []);

  const clearBoardProgress = useCallback(async () => {
    for (let index = 0; index < 4; index += 1) await writeLed(index, false);
  }, [writeLed]);

  const resetPlay = useCallback((nextGame?: GameSpec) => {
    if (timerRef.current) clearTimeout(timerRef.current);
    if (nextGame) {
      gameRef.current = nextGame;
      setGame(nextGame);
    }
    roundIndexRef.current = 0;
    selectedChoiceRef.current = 0;
    scoreRef.current = 0;
    lockedRef.current = false;
    setRoundIndex(0);
    setSelectedChoice(0);
    setScore(0);
    setComplete(false);
    setFeedback(null);
    setLastInput("시스템");
    void clearBoardProgress();
  }, [clearBoardProgress]);

  const moveSelection = useCallback((direction: number, source: InputSource) => {
    if (lockedRef.current || complete || approval !== "approved") return;
    const currentRound = gameRef.current.rounds[roundIndexRef.current];
    const next = (selectedChoiceRef.current + direction + currentRound.choices.length) % currentRound.choices.length;
    selectedChoiceRef.current = next;
    setSelectedChoice(next);
    setLastInput(source);
    if (feedback?.kind === "retry") setFeedback(null);
  }, [approval, complete, feedback]);

  const submitChoice = useCallback((source: InputSource) => {
    if (lockedRef.current || complete || approval !== "approved") return;
    const currentGame = gameRef.current;
    const currentRoundIndex = roundIndexRef.current;
    const currentRound = currentGame.rounds[currentRoundIndex];
    const choice = currentRound.choices[selectedChoiceRef.current];
    setLastInput(source);

    if (choice.id !== currentRound.answerId) {
      setFeedback({ kind: "retry", text: "거의 다 왔어요! 천천히 다시 찾아볼까요?" });
      return;
    }

    lockedRef.current = true;
    const nextScore = scoreRef.current + 1;
    scoreRef.current = nextScore;
    setScore(nextScore);
    setFeedback({ kind: "correct", text: "찾았어요! 멋진 선택이에요." });
    void writeLed(currentRoundIndex, true);

    timerRef.current = setTimeout(() => {
      if (currentRoundIndex === currentGame.rounds.length - 1) {
        setComplete(true);
        lockedRef.current = false;
        return;
      }
      const nextRound = currentRoundIndex + 1;
      roundIndexRef.current = nextRound;
      selectedChoiceRef.current = 0;
      setRoundIndex(nextRound);
      setSelectedChoice(0);
      setFeedback(null);
      lockedRef.current = false;
    }, 850);
  }, [approval, complete, writeLed]);

  const moveSelectionRef = useRef(moveSelection);
  const submitChoiceRef = useRef(submitChoice);
  useEffect(() => { moveSelectionRef.current = moveSelection; }, [moveSelection]);
  useEffect(() => { submitChoiceRef.current = submitChoice; }, [submitChoice]);

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "ArrowLeft") {
        event.preventDefault();
        moveSelectionRef.current(-1, "키보드");
      } else if (event.key === "ArrowRight") {
        event.preventDefault();
        moveSelectionRef.current(1, "키보드");
      } else if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        submitChoiceRef.current("키보드");
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, []);

  useEffect(() => {
    const restoreTimer = window.setTimeout(() => {
      const savedPlan = window.localStorage.getItem(`nu-play-plan:${date}`);
      if (savedPlan) {
        try {
          const restored = JSON.parse(savedPlan) as {
            prompt?: string;
            context?: PlayContext;
            approved?: boolean;
          };
          const restoredPrompt = restored.prompt?.trim() || DEFAULT_PROMPT;
          const restoredContext = restored.context ?? DEFAULT_CONTEXT;
          setPrompt(restoredPrompt);
          setPlayContext(restoredContext);
          resetPlay(createDailyGame(restoredPrompt, date, restoredContext));
          setApproval(restored.approved ? "approved" : "draft");
          return;
        } catch {
          window.localStorage.removeItem(`nu-play-plan:${date}`);
        }
      }

      const savedPrompt = window.localStorage.getItem(`nu-play-prompt:${date}`);
      if (savedPrompt) {
        setPrompt(savedPrompt);
        resetPlay(createDailyGame(savedPrompt, date, DEFAULT_CONTEXT));
      }
    }, 0);
    return () => {
      window.clearTimeout(restoreTimer);
      if (timerRef.current) clearTimeout(timerRef.current);
    };
  }, [date, resetPlay]);

  const generateGame = useCallback(() => {
    const cleanPrompt = prompt.trim() || DEFAULT_PROMPT;
    const nextGame = createDailyGame(cleanPrompt, date, playContext);
    window.localStorage.setItem(`nu-play-prompt:${date}`, cleanPrompt);
    setPrompt(cleanPrompt);
    setApproval("draft");
    resetPlay(nextGame);
  }, [date, playContext, prompt, resetPlay]);

  const approveGame = useCallback(() => {
    window.localStorage.setItem(
      `nu-play-plan:${date}`,
      JSON.stringify({ prompt, context: playContext, approved: true }),
    );
    setApproval("approved");
    resetPlay();
  }, [date, playContext, prompt, resetPlay]);

  const resetCalibration = useCallback(() => {
    calibrationRef.current = {
      count: 0,
      x: 0,
      y: 0,
      z: 0,
      joystickX: 0,
      joystickY: 0,
      baseline: [0, 0, 0],
      joystickBaseline: [127, 127],
    };
    tiltLatchedRef.current = false;
    joystickLatchedRef.current = false;
    previousTouchRef.current = false;
    previousJoystickButtonRef.current = false;
    setCalibrated(false);
  }, []);

  const processSensorPacket = useCallback((value: DataView) => {
    if (value.byteLength < 18) return;
    const packetVersion = value.getUint8(0);
    if (packetVersion !== 1 && packetVersion !== 2) return;
    const flags = value.getUint8(1);
    const nextSensor: SensorData = {
      touch: (flags & 0x01) !== 0,
      touchMv: value.getUint16(4, true),
      accel: [
        value.getInt16(6, true),
        value.getInt16(8, true),
        value.getInt16(10, true),
      ],
      joystick: packetVersion >= 2 && value.byteLength >= 20
        ? [value.getUint8(18), value.getUint8(19)]
        : [127, 127],
      joystickPressed: packetVersion >= 2 && (flags & 0x02) !== 0,
    };
    setSensor(nextSensor);

    const calibration = calibrationRef.current;
    if (calibration.count < 12) {
      calibration.x += nextSensor.accel[0];
      calibration.y += nextSensor.accel[1];
      calibration.z += nextSensor.accel[2];
      calibration.joystickX += nextSensor.joystick[0];
      calibration.joystickY += nextSensor.joystick[1];
      calibration.count += 1;
      if (calibration.count === 12) {
        calibration.baseline = [
          Math.round(calibration.x / 12),
          Math.round(calibration.y / 12),
          Math.round(calibration.z / 12),
        ];
        calibration.joystickBaseline = [
          Math.round(calibration.joystickX / 12),
          Math.round(calibration.joystickY / 12),
        ];
        setCalibrated(true);
      }
    } else {
      const deltaX = nextSensor.accel[0] - calibration.baseline[0];
      if (Math.abs(deltaX) < 220) {
        tiltLatchedRef.current = false;
      } else if (Math.abs(deltaX) > 480 && !tiltLatchedRef.current) {
        tiltLatchedRef.current = true;
        moveSelectionRef.current(deltaX > 0 ? 1 : -1, "IMU");
      }

      const joystickDeltaX = nextSensor.joystick[0] - calibration.joystickBaseline[0];
      if (Math.abs(joystickDeltaX) < 18) {
        joystickLatchedRef.current = false;
      } else if (Math.abs(joystickDeltaX) > 42 && !joystickLatchedRef.current) {
        joystickLatchedRef.current = true;
        moveSelectionRef.current(joystickDeltaX > 0 ? 1 : -1, "조이스틱");
      }
    }

    const now = Date.now();
    if (nextSensor.touch && !previousTouchRef.current && now - lastTouchAtRef.current > 350) {
      lastTouchAtRef.current = now;
      submitChoiceRef.current("터치");
    }
    previousTouchRef.current = nextSensor.touch;

    if (
      nextSensor.joystickPressed &&
      !previousJoystickButtonRef.current &&
      now - lastJoystickButtonAtRef.current > 350
    ) {
      lastJoystickButtonAtRef.current = now;
      submitChoiceRef.current("조이스틱");
    }
    previousJoystickButtonRef.current = nextSensor.joystickPressed;
  }, []);

  const handleDisconnected = useCallback(() => {
    commandRef.current = null;
    setConnection("idle");
    setConnectionMessage("연결 끊김 · 다시 연결 가능");
    setCalibrated(false);
  }, []);

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
      setConnectionMessage("Chrome 또는 Edge가 필요합니다");
      return;
    }

    try {
      setConnection("connecting");
      setConnectionMessage("MZSZ-TOY 검색 중");
      const device = await bluetooth.requestDevice({
        filters: [{ services: [SERVICE_UUID] }],
      });
      deviceRef.current = device;
      device.addEventListener("gattserverdisconnected", handleDisconnected);
      const server = await device.gatt?.connect();
      if (!server) throw new Error("GATT server unavailable");
      const service = await server.getPrimaryService(SERVICE_UUID);
      const sensorCharacteristic = await service.getCharacteristic(SENSOR_UUID);
      commandRef.current = await service.getCharacteristic(COMMAND_UUID);
      sensorCharacteristic.addEventListener("characteristicvaluechanged", (event: Event) => {
        const characteristic = event.target as GattCharacteristic;
        if (characteristic.value) processSensorPacket(characteristic.value);
      });
      await sensorCharacteristic.startNotifications();
      setConnection("connected");
      setConnectionMessage(device.name ?? "MZSZ-TOY");
      resetCalibration();
      await clearBoardProgress();
    } catch (error) {
      const message = error instanceof Error ? error.message : "연결 실패";
      if (message.toLowerCase().includes("cancel")) {
        setConnection("idle");
        setConnectionMessage("연결 취소됨");
      } else {
        setConnection("error");
        setConnectionMessage("연결 실패 · 다시 시도");
      }
    }
  }, [clearBoardProgress, connection, disconnect, handleDisconnected, processSensorPacket, resetCalibration]);

  const currentRound = game.rounds[roundIndex];
  const connectionLabel = connection === "connected"
    ? "연결 해제"
    : connection === "connecting"
      ? "연결 중…"
      : "Bluetooth 연결";

  return (
    <main className="play-shell">
      <header className="play-topbar">
        <div className="play-brand">
          <span className="play-brand-mark">NU</span>
          <div>
            <p className="play-kicker">SENSOR PLAY LAB</p>
            <h1>오늘의 성장 놀이</h1>
          </div>
        </div>
        <div className={`play-connection ${connection}`}>
          <span className="connection-light" />
          <div><small>{formatToday(date)}</small><strong>{connectionMessage}</strong></div>
          <button type="button" className="play-connect-button" onClick={connect} disabled={connection === "connecting"}>{connectionLabel}</button>
        </div>
      </header>

      <section className="play-layout">
        <aside className="parent-card">
          <p className="play-kicker">FOR PARENTS</p>
          <h2>오늘은 어떤 놀이를 할까요?</h2>
          <div className="play-setup-grid">
            <label>
              아이 나이
              <select
                value={playContext.ageBand}
                onChange={(event) => setPlayContext((current) => ({ ...current, ageBand: event.target.value }))}
              >
                {AGE_BANDS.map((age) => <option key={age}>{age}</option>)}
              </select>
            </label>
            <label>
              오늘의 공간
              <select
                value={playContext.space}
                onChange={(event) => setPlayContext((current) => ({ ...current, space: event.target.value }))}
              >
                {PLAY_SPACES.map((space) => <option key={space}>{space}</option>)}
              </select>
            </label>
            <label>
              발달 목표
              <select
                value={playContext.goal}
                onChange={(event) => setPlayContext((current) => ({ ...current, goal: event.target.value }))}
              >
                {GROWTH_GOALS.map((goal) => <option key={goal}>{goal}</option>)}
              </select>
            </label>
            <label>
              놀이 시간
              <select
                value={playContext.minutes}
                onChange={(event) => setPlayContext((current) => ({ ...current, minutes: Number(event.target.value) }))}
              >
                {PLAY_TIMES.map((minutes) => <option key={minutes} value={minutes}>{minutes}분</option>)}
              </select>
            </label>
          </div>

          <label htmlFor="game-exclusions">안전상 피하고 싶은 요소</label>
          <input
            id="game-exclusions"
            className="safety-input"
            value={playContext.exclusions}
            onChange={(event) => setPlayContext((current) => ({ ...current, exclusions: event.target.value.slice(0, 60) }))}
            maxLength={60}
            placeholder="예: 점프, 작은 부품, 큰 소리"
          />

          <label htmlFor="game-prompt">아이의 관심사를 한 문장으로 적어주세요.</label>
          <textarea
            id="game-prompt"
            value={prompt}
            onChange={(event) => setPrompt(event.target.value.slice(0, 120))}
            rows={5}
            maxLength={120}
          />
          <div className="prompt-count">{prompt.length}/120</div>
          <div className="prompt-presets" aria-label="추천 프롬프트">
            {PRESETS.map((preset) => (
              <button type="button" key={preset} onClick={() => setPrompt(preset)}>{preset}</button>
            ))}
          </div>
          <button type="button" className="generate-button" onClick={generateGame}>게임 초안 만들기</button>

          <div className="safe-generator-note">
            <span>✓</span>
            <div><strong>부모 승인 전에는 시작되지 않아요</strong><p>현재는 검증된 로컬 규칙으로 초안을 만들며 외부 콘텐츠를 불러오지 않습니다.</p></div>
          </div>
        </aside>

        <section className="game-card" aria-label="오늘의 게임">
          <div className="game-card-heading">
            <div>
              <p className="play-kicker">TODAY&apos;S GAME · {game.rounds.length} ROUNDS</p>
              <h2>{game.title}</h2>
              <p className="game-intro">{game.intro}</p>
            </div>
            <span className="game-skill-badge">{game.skill}</span>
          </div>

          <div className="approval-workflow" aria-label="게임 준비 단계">
            <div className="workflow-steps">
              <span className="done">1 · 초안</span>
              <span className={approval === "approved" ? "done" : "current"}>2 · 부모 승인</span>
              <span className={connection === "connected" ? "done" : approval === "approved" ? "current" : ""}>3 · BLE 놀이</span>
            </div>
            {approval === "draft" ? (
              <div className="approval-review">
                <div>
                  <strong>부모님 최종 확인</strong>
                  <p>{game.context.ageBand} · {game.context.space} · {game.context.minutes}분 · {game.context.goal}</p>
                  <small>피할 요소: {game.context.exclusions || "없음"}</small>
                </div>
                <button type="button" onClick={approveGame}>승인하고 시작</button>
              </div>
            ) : (
              <div className="local-play-note">
                <span>PLAY MODE</span>
                <strong>승인 완료 · 로컬 BLE 놀이</strong>
                <small>아이에게 화면을 보여주지 않아도 보호자가 미션을 읽어줄 수 있어요.</small>
              </div>
            )}
          </div>

          <div className="round-progress" aria-label={`${roundIndex + 1}/${game.rounds.length} 라운드`}>
            {game.rounds.map((_, index) => (
              <span key={index} className={index < score ? "done" : index === roundIndex && !complete ? "current" : ""}>{index < score ? "✓" : index + 1}</span>
            ))}
          </div>

          {complete ? (
            <div className="completion-card" aria-live="polite">
              <div className="celebration-icons" aria-hidden="true"><span>⭐</span><span>🎉</span><span>⭐</span></div>
              <p className="play-kicker">ALL MISSIONS COMPLETE</p>
              <h3>오늘의 놀이 성공!</h3>
              <p>네 번 모두 끝냈어요. 아이가 고른 방법과 이유를 함께 이야기해 보세요.</p>
              <button type="button" onClick={() => resetPlay()}>한 번 더 하기</button>
            </div>
          ) : (
            <>
              <div className="mission-card">
                <div>
                  <span className="mission-guide">{roundIndex + 1}번째 미션</span>
                  <strong>{currentRound.instruction}</strong>
                  <p>{currentRound.hint}</p>
                </div>
                <span className="mission-mascot" aria-hidden="true">{THEMES.find((item) => item.name === game.theme)?.icons[roundIndex] ?? "⭐"}</span>
              </div>

              <div className="choice-grid" aria-label="정답 선택지">
                {currentRound.choices.map((choice, index) => (
                  <button
                    type="button"
                    className={`choice-card ${selectedChoice === index ? "selected" : ""}`}
                    key={choice.id}
                    disabled={approval !== "approved"}
                    onClick={() => {
                      selectedChoiceRef.current = index;
                      setSelectedChoice(index);
                      setLastInput("마우스");
                      if (feedback?.kind === "retry") setFeedback(null);
                    }}
                    aria-pressed={selectedChoice === index}
                  >
                    {choice.amount ? (
                      <span className="count-visual" aria-hidden="true">{Array.from({ length: choice.amount }, (_, itemIndex) => <i key={itemIndex}>{choice.visual}</i>)}</span>
                    ) : choice.color ? (
                      <span className={`color-visual ${game.kind === "shape" ? "shape-visual" : ""}`} style={{ backgroundColor: choice.color }} aria-hidden="true">{choice.visual}</span>
                    ) : (
                      <span className="emoji-visual" aria-hidden="true">{choice.visual}</span>
                    )}
                    <strong>{choice.label}</strong>
                    {selectedChoice === index && <small>내 선택</small>}
                  </button>
                ))}
              </div>

              <div className="answer-row">
                <div className={`feedback-message ${feedback?.kind ?? ""}`} aria-live="polite">
                  {feedback ? feedback.text : `현재 ${currentRound.choices[selectedChoice].label} 선택 · ${lastInput}`}
                </div>
                <button type="button" className="confirm-choice" onClick={() => submitChoice("마우스")} disabled={approval !== "approved"}>선택하기</button>
              </div>
            </>
          )}
        </section>
      </section>

      <section className="play-info-grid">
        <article className="device-card">
          <div className="info-heading">
            <div><p className="play-kicker">NU40 REMOTE</p><h2>센서 리모컨</h2></div>
            <span className={`live-pill ${connection === "connected" ? "active" : ""}`}>{connection === "connected" ? "LIVE" : "OFFLINE"}</span>
          </div>
          <div className="device-body">
            <div className={`mini-board ${calibrated ? "ready" : ""}`}><span>NU40</span><small>{calibrated ? "READY" : "WAIT"}</small></div>
            <div className="sensor-stats">
              <div><span>기울기 X</span><strong>{connection === "connected" ? `${sensor.accel[0]} mg` : "—"}</strong></div>
              <div><span>터치</span><strong>{connection === "connected" ? (sensor.touch ? "감지됨" : "대기") : "—"}</strong></div>
              <div><span>터치 전압</span><strong>{connection === "connected" ? `${sensor.touchMv} mV` : "—"}</strong></div>
              <div><span>조이스틱 X</span><strong>{connection === "connected" ? sensor.joystick[0] : "—"}</strong></div>
              <div><span>조이스틱 Y</span><strong>{connection === "connected" ? sensor.joystick[1] : "—"}</strong></div>
              <div><span>조이스틱 버튼</span><strong>{connection === "connected" ? (sensor.joystickPressed ? "눌림" : "대기") : "—"}</strong></div>
            </div>
          </div>
          <p className="device-help">IMU 기울기 또는 조이스틱 좌우: 선택 이동 · 터치 또는 조이스틱 버튼: 선택하기</p>
          <div className="input-modules" aria-label="입력 모듈 상태">
            <span className="ready">IMU 사용 가능</span>
            <span className="ready">터치 사용 가능</span>
            <span className="ready">조이스틱 사용 가능</span>
          </div>
        </article>

        <article className="growth-card">
          <p className="play-kicker">GROWTH GOALS</p>
          <h2>오늘 연습하는 힘</h2>
          <ul>
            <li><span>01</span><div><strong>보고 구별하기</strong><p>색·수·모양·표정을 관찰해요.</p></div></li>
            <li><span>02</span><div><strong>몸으로 선택하기</strong><p>기울이고 터치하며 눈과 손을 맞춰요.</p></div></li>
            <li><span>03</span><div><strong>말로 설명하기</strong><p>왜 골랐는지 부모님과 이야기해요.</p></div></li>
          </ul>
        </article>

        <article className="parent-guide-card">
          <p className="play-kicker">PLAY TOGETHER</p>
          <h2>부모님 놀이 팁</h2>
          <p>정답보다 아이가 생각한 과정을 칭찬해 주세요. “어떻게 찾았어?”처럼 열린 질문을 하고 4분 뒤에는 화면에서 벗어나 같은 색이나 모양을 집 안에서 찾아보세요.</p>
          <div className="guardian-note">0–3세는 보호자가 곁에서 함께하고, 케이스 고정과 배선 노출 여부를 먼저 확인해 주세요.</div>
          <div className="device-roadmap"><span>한 기기, 여러 놀이</span><strong>필요한 센서만 단계적으로 확장</strong></div>
        </article>
      </section>
    </main>
  );
}
