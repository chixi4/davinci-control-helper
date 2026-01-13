import React, { useState, useEffect, useRef, useMemo } from 'react';
import { Mouse, Crosshair, RefreshCw, Minus, X, Loader2, AlertCircle, CheckCircle } from 'lucide-react';
import { motion, AnimatePresence, useMotionValue, animate } from 'framer-motion';

const isTauri = typeof window !== 'undefined' && typeof window.__TAURI_IPC__ === 'function';

async function tauriInvoke(command, args) {
  if (!isTauri) return null;
  const { invoke } = await import('@tauri-apps/api/tauri');
  return invoke(command, args);
}

async function tauriListen(eventName, handler) {
  if (!isTauri) return () => {};
  const { listen } = await import('@tauri-apps/api/event');
  return listen(eventName, handler);
}

async function tauriMinimize() {
  if (!isTauri) return;
  const { appWindow } = await import('@tauri-apps/api/window');
  return appWindow.minimize();
}

async function tauriClose() {
  if (!isTauri) return;
  const { appWindow } = await import('@tauri-apps/api/window');
  return appWindow.close();
}

async function tauriShowAndFocus() {
  if (!isTauri) return;
  const { appWindow } = await import('@tauri-apps/api/window');
  await Promise.allSettled([appWindow.show(), appWindow.unminimize(), appWindow.setFocus()]);
}

async function tauriStartDragging() {
  if (!isTauri) return;
  const { appWindow } = await import('@tauri-apps/api/window');
  return appWindow.startDragging();
}

const WINDOW_WIDTH = 320;
const WINDOW_HEIGHT = 460;
const AMBIENT_SEG1_START = 0.08;
const AMBIENT_SEG1_END = 0.20;
const AMBIENT_SEG2_END = 0.30;
const AMBIENT_SEG3_END = 0.60;
const AMBIENT_BRIGHTNESS_SMOOTH_DEFAULT_MS = 260;
const AMBIENT_BRIGHTNESS_EPSILON = 0.0005;
const AMBIENT_BRIGHTNESS_SMOOTH_MIN = 0;
const AMBIENT_BRIGHTNESS_SMOOTH_MAX = 2000;
const GLASS_TOP_SEG1 = 0.0225;
const GLASS_BOTTOM_SEG1 = 0.045;
const GLASS_TOP_SEG2 = 0.1125;
const GLASS_BOTTOM_SEG2 = 0.15;
const GLASS_TOP_SEG3 = 0.225;
const GLASS_BOTTOM_SEG3 = 0.675;
const GLASS_TOP_SEG4 = 0.375;
const GLASS_BOTTOM_SEG4 = 0.75;
const GLASS_VIGNETTE_SEG1 = 0.80;
const GLASS_VIGNETTE_SEG2 = 0.76;
const GLASS_VIGNETTE_SEG3 = 0.66;
const GLASS_VIGNETTE_SEG4 = 0.55;
const DEFAULT_AMBIENT_CONFIG = {
  seg1: { top: GLASS_TOP_SEG1, bottom: GLASS_BOTTOM_SEG1, vignette: GLASS_VIGNETTE_SEG1 },
  seg2: { top: GLASS_TOP_SEG2, bottom: GLASS_BOTTOM_SEG2, vignette: GLASS_VIGNETTE_SEG2 },
  seg3: { top: GLASS_TOP_SEG3, bottom: GLASS_BOTTOM_SEG3, vignette: GLASS_VIGNETTE_SEG3 },
  seg4: { top: GLASS_TOP_SEG4, bottom: GLASS_BOTTOM_SEG4, vignette: GLASS_VIGNETTE_SEG4 },
};
const AMBIENT_SAMPLE_RING_OFFSETS = [4];
const AMBIENT_SAMPLE_POINTS_PER_SIDE = 5;
const AMBIENT_SAMPLE_FRACTIONS = Array.from(
  { length: AMBIENT_SAMPLE_POINTS_PER_SIDE },
  (_, index) => (index + 1) / (AMBIENT_SAMPLE_POINTS_PER_SIDE + 1)
);
const AMBIENT_SAMPLE_MARKERS = {
  x: AMBIENT_SAMPLE_FRACTIONS,
  y: AMBIENT_SAMPLE_FRACTIONS,
  rings: AMBIENT_SAMPLE_RING_OFFSETS,
};
const AMBIENT_SAMPLE_SOURCE = 48;
const AMBIENT_SAMPLE_POINTS_PER_RING = AMBIENT_SAMPLE_POINTS_PER_SIDE * 4;
const AMBIENT_SAMPLE_TOTAL = AMBIENT_SAMPLE_RING_OFFSETS.length * AMBIENT_SAMPLE_POINTS_PER_RING;
const AMBIENT_SAMPLE_OUTER_MARGIN = Math.max(...AMBIENT_SAMPLE_RING_OFFSETS) + AMBIENT_SAMPLE_SOURCE;
const AMBIENT_SAMPLE_MAP_SCALE = 0.2;
const AMBIENT_SAMPLE_MAP_WIDTH = WINDOW_WIDTH + AMBIENT_SAMPLE_OUTER_MARGIN * 2;
const AMBIENT_SAMPLE_MAP_HEIGHT = WINDOW_HEIGHT + AMBIENT_SAMPLE_OUTER_MARGIN * 2;
const AMBIENT_LOG_FLUSH_INTERVAL = 250;
const AMBIENT_LOG_BUFFER_LIMIT = 240;

const clamp01 = (value) => Math.min(1, Math.max(0, value));
const formatSampleValue = (value) =>
  typeof value === 'number' && Number.isFinite(value) ? value.toFixed(2) : '--';
const getSampleIndex = (ringIndex, side, pointIndex) => {
  const base = ringIndex * AMBIENT_SAMPLE_POINTS_PER_RING;
  const offset = AMBIENT_SAMPLE_POINTS_PER_SIDE * 2;
  if (side === 'top') return base + pointIndex * 2;
  if (side === 'bottom') return base + pointIndex * 2 + 1;
  if (side === 'left') return base + offset + pointIndex * 2;
  return base + offset + pointIndex * 2 + 1;
};
const AMBIENT_CONFIG_STORAGE_KEY = 'rawaccel-ambient-config-v1';
const AMBIENT_SETTINGS_STORAGE_KEY = 'rawaccel-ambient-settings-v1';
const AMBIENT_DEBUG_STORAGE_KEY = 'rawaccel-ambient-debug-ui';
const AMBIENT_DEBUG_HOTKEY = { code: 'KeyD', ctrl: true, alt: true, shift: true };
const IS_DEV = import.meta.env.DEV;
const DEFAULT_AMBIENT_SETTINGS = {
  config: DEFAULT_AMBIENT_CONFIG,
  dropExtremesEnabled: true,
  emaEnabled: false,
  brightnessSmoothMs: AMBIENT_BRIGHTNESS_SMOOTH_DEFAULT_MS,
};

const clampAmbientSmoothMs = (value) => {
  if (!Number.isFinite(value)) return DEFAULT_AMBIENT_SETTINGS.brightnessSmoothMs;
  return Math.min(
    AMBIENT_BRIGHTNESS_SMOOTH_MAX,
    Math.max(AMBIENT_BRIGHTNESS_SMOOTH_MIN, value)
  );
};

const sanitizeAmbientSegment = (segment, fallback) => {
  const next = segment || {};
  const top = Number.isFinite(next.top) ? next.top : fallback.top;
  const bottom = Number.isFinite(next.bottom) ? next.bottom : fallback.bottom;
  const vignette = Number.isFinite(next.vignette) ? next.vignette : fallback.vignette;
  return {
    top: clamp01(top),
    bottom: clamp01(bottom),
    vignette: clamp01(vignette),
  };
};

const sanitizeAmbientConfig = (config) => {
  if (!config || typeof config !== 'object') return DEFAULT_AMBIENT_CONFIG;
  return {
    seg1: sanitizeAmbientSegment(config.seg1, DEFAULT_AMBIENT_CONFIG.seg1),
    seg2: sanitizeAmbientSegment(config.seg2, DEFAULT_AMBIENT_CONFIG.seg2),
    seg3: sanitizeAmbientSegment(config.seg3, DEFAULT_AMBIENT_CONFIG.seg3),
    seg4: sanitizeAmbientSegment(config.seg4, DEFAULT_AMBIENT_CONFIG.seg4),
  };
};

const loadAmbientConfig = () => {
  if (typeof window === 'undefined') return DEFAULT_AMBIENT_CONFIG;
  try {
    const raw = window.localStorage.getItem(AMBIENT_CONFIG_STORAGE_KEY);
    if (!raw) return DEFAULT_AMBIENT_CONFIG;
    const parsed = JSON.parse(raw);
    return sanitizeAmbientConfig(parsed);
  } catch {
    return DEFAULT_AMBIENT_CONFIG;
  }
};

const loadAmbientSettings = () => {
  if (typeof window === 'undefined') return DEFAULT_AMBIENT_SETTINGS;
  try {
    const raw = window.localStorage.getItem(AMBIENT_SETTINGS_STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw);
      if (parsed && typeof parsed === 'object') {
        return {
          config: sanitizeAmbientConfig(parsed.config ?? parsed),
          dropExtremesEnabled:
            typeof parsed.dropExtremesEnabled === 'boolean'
              ? parsed.dropExtremesEnabled
              : DEFAULT_AMBIENT_SETTINGS.dropExtremesEnabled,
          emaEnabled:
            typeof parsed.emaEnabled === 'boolean'
              ? parsed.emaEnabled
              : DEFAULT_AMBIENT_SETTINGS.emaEnabled,
          brightnessSmoothMs: clampAmbientSmoothMs(
            typeof parsed.brightnessSmoothMs === 'number'
              ? parsed.brightnessSmoothMs
              : DEFAULT_AMBIENT_SETTINGS.brightnessSmoothMs
          ),
        };
      }
    }
  } catch {}

  return {
    config: loadAmbientConfig(),
    dropExtremesEnabled: DEFAULT_AMBIENT_SETTINGS.dropExtremesEnabled,
    emaEnabled: DEFAULT_AMBIENT_SETTINGS.emaEnabled,
  };
};

// --- [后端注意] 报错与状态模拟数据 ---
// 这是一个轮播的演示列表，用于展示灵敏度为1.0时的界面状态反馈。
// 后端人员请注意：这里需要替换为实际的硬件/驱动报错信息接口。
// 格式建议保持 "类型:内容" 以便前端解析颜色。
const DEBUG_SEQUENCE = [
  "ERR:未检测到驱动",        
  "OK:配置已保存",            
  "ERR:写入超时",          
  "OK:固件已更新",        
  "ERR:参数无效",       
  "FS:LOST",          
  "FS:CONNECTING",    
  "FS:OFFLINE"        
];

// --- 全屏状态UI配置 ---
// 对应后端连接丢失、正在连接、服务下线等重大状态的视觉反馈
const FULLSCREEN_CONFIG = {
  'LOST': {
    title: "连接已断开",
    subtitle: "正在尝试重连...",
    colorClass: "text-red-500",
    bgClass: "bg-red-500",
    borderClass: "border-red-500",
    shadowColor: "rgba(220,38,38,0.8)",
    pulse: true
  },
  'CONNECTING': {
    title: "正在建立连接",
    subtitle: "握手进行中...",
    colorClass: "text-amber-500",
    bgClass: "bg-amber-500",
    borderClass: "border-amber-500",
    shadowColor: "rgba(245,158,11,0.8)",
    pulse: true
  },
  'DRIVER_MISSING': {
    title: "未检测到驱动",
    subtitle: "需要安装后才能应用灵敏度",
    colorClass: "text-amber-400",
    bgClass: "bg-amber-400",
    borderClass: "border-amber-400",
    shadowColor: "rgba(245,158,11,0.85)",
    pulse: false
  },
  'OFFLINE': {
    title: "后端离线",
    subtitle: "服务不可用",
    colorClass: "text-zinc-300",
    bgClass: "bg-zinc-300",
    borderClass: "border-zinc-300",
    shadowColor: "rgba(255,255,255,0.25)",
    pulse: false 
  }
};

// 灵敏度数值转换工具（保持非线性手感）
const toSplitScale = (position) => {
  if (position <= 50) {
    return 0.01 + (position / 50) * (1.0 - 0.01);
  } else {
    return 1.0 + ((position - 50) / 50) * (5.0 - 1.0);
  }
};

const fromSplitScale = (value) => {
  if (value <= 1.0) {
    return ((value - 0.01) / (1.0 - 0.01)) * 50;
  } else {
    return 50 + ((value - 1.0) / (5.0 - 1.0)) * 50;
  }
};

export default function App() {
  // --- 核心状态 ---
  // INIT: Tauri 启动阶段（等待后端快照，避免已注册设备时闪过 SCAN）
  // SCAN: 注册/绑定鼠标界面
  // DASHBOARD: 主控制界面
  const [phase, setPhase] = useState(isTauri ? 'INIT' : 'SCAN');
  const [scanInputReady, setScanInputReady] = useState(false);
  
  // 灵敏度：对应 CLI 中的 'l' 命令设置的值
  const [sensitivity, setSensitivity] = useState(1.0);
  
  // 同步状态：用于展示调节灵敏度时的 1秒 等待动画
  const [isSyncing, setIsSyncing] = useState(false);
  const [resetPulse, setResetPulse] = useState(false);
  const [shutdownPulse, setShutdownPulse] = useState(false);
  
  // 瞄准镜状态：对应 CLI 中的 'p' 键（自动按左键功能开关）
  const [isCrosshairActive, setIsCrosshairActive] = useState(false);
  
  // 鼠标开关状态：控制整个功能的启停
  // OFF: 关机（恢复默认灵敏度，关闭功能）
  // ON: 开机（应用设置）
  // BOOTING/SHUTTING_DOWN: 过渡动画状态
  const [mouseStatus, setMouseStatus] = useState('OFF'); 
  
  const [notifications, setNotifications] = useState([]);
  const [fullScreenStatus, setFullScreenStatus] = useState(null);
  const [driverNoticeActive, setDriverNoticeActive] = useState(false);
  const [driverNoticePillDismissed, setDriverNoticePillDismissed] = useState(false);
  const driverNoticeOverlayShown = useRef(false);
  const driverPillDragging = useRef(false);
  const overlayPointerStart = useRef(null);
  const overlayPointerDragged = useRef(false);
  const driverNoticePillY = useMotionValue(0);
  const driverNoticeVisible = driverNoticeActive && !driverNoticePillDismissed && !fullScreenStatus;
  
  // 退出状态：用于处理点击关闭按钮后的延迟逻辑
  const [isClosing, setIsClosing] = useState(false);
  const [contextMenu, setContextMenu] = useState(null);

  // 自动开火状态：当 isCrosshairActive 为 true 且触发逻辑时，变为 true (变绿)
  const [isFiring, setIsFiring] = useState(false);
  
  const [mousePos, setMousePos] = useState({ x: 0, y: 0 });
  const [shakeProgress, setShakeProgress] = useState(0);
  
  const fireTimer = useRef(null);
  const lastMousePos = useRef(null);
  const containerRef = useRef(null);
  const dragArmed = useRef(false);
  const dragStart = useRef(null);
  const lastWindowClick = useRef(0);
  const lastWindowClickPos = useRef(null);
  const debounceTimer = useRef(null);
  const syncTimer = useRef(null);
  const resetPulseTimer = useRef(null);
  const shutdownPulseTimer = useRef(null);
  const isFirstRender = useRef(true);
  const pendingSensitivity = useRef(null);
  const skipNextSensitivitySend = useRef(false);
  const pendingPower = useRef(null);
  const pendingExit = useRef(null);
  const closeRequested = useRef(false);
  const isCrosshairActiveRef = useRef(false);
  const mouseStatusRef = useRef('OFF');
  const phaseRef = useRef(phase);
  const initialAmbientSettings = useMemo(() => loadAmbientSettings(), []);
  const [ambientConfig, setAmbientConfig] = useState(() => initialAmbientSettings.config);
  const ambientConfigRef = useRef(ambientConfig);
  const ambientTarget = useRef(1.0);
  const ambientMeasured = useRef(1.0);
  const [ambientPreviewEnabled, setAmbientPreviewEnabled] = useState(false);
  const ambientPreviewEnabledRef = useRef(false);
  const [ambientPreviewBrightness, setAmbientPreviewBrightness] = useState(0.2);
  const [ambientTunerOpen, setAmbientTunerOpen] = useState(false);
  const [showAmbientSamplePoints, setShowAmbientSamplePoints] = useState(false);
  const [showAmbientDebugPanel, setShowAmbientDebugPanel] = useState(() => {
    if (IS_DEV) return true;
    try {
      return window.localStorage.getItem(AMBIENT_DEBUG_STORAGE_KEY) === '1';
    } catch {
      return false;
    }
  });
  const [dropExtremesEnabled, setDropExtremesEnabled] = useState(
    () => initialAmbientSettings.dropExtremesEnabled
  );
  const [emaEnabled, setEmaEnabled] = useState(() => initialAmbientSettings.emaEnabled);
  const [ambientSmoothMs, setAmbientSmoothMs] = useState(
    () => initialAmbientSettings.brightnessSmoothMs
  );
  const ambientSmoothMsRef = useRef(ambientSmoothMs);
  const [ambientLogging, setAmbientLogging] = useState(false);
  const ambientLoggingRef = useRef(false);
  const ambientLogBuffer = useRef([]);
  const ambientLogFlushTimer = useRef(null);
  const ambientLogRaf = useRef(0);
  const ambientLogFsRef = useRef(null);
  const ambientLogPathRef = useRef('');
  const ambientLogFlushBusy = useRef(false);
  const ambientLogError = useRef(false);
  const ambientSamplesLast = useRef(0);
  const [ambientSamples, setAmbientSamples] = useState(() => Array(AMBIENT_SAMPLE_TOTAL).fill(null));
  const ambientSamplesRef = useRef(ambientSamples);
  const ambientSmoothed = useRef(1.0);
  const ambientStepTime = useRef(null);
  const ambientTop = useRef(ambientConfigRef.current.seg4.top);
  const ambientBottom = useRef(ambientConfigRef.current.seg4.bottom);
  const ambientVignette = useRef(ambientConfigRef.current.seg4.vignette);
  const ambientRaf = useRef(0);
  const ambientDebugLast = useRef(0);
  const [ambientDebug, setAmbientDebug] = useState(() => ({
    brightness: ambientSmoothed.current,
    measured: ambientMeasured.current,
    top: ambientTop.current,
    bottom: ambientBottom.current,
    vignette: ambientVignette.current,
  }));
  
  // 记忆功能：用于在重新开启鼠标开关时，恢复上次的瞄准镜状态
  const crosshairMemory = useRef(false);
  
  // CapsLock 双击检测计时器
  const lastCapsLockTime = useRef(0);

  const addNotification = (type, msg) => {
    const id = Date.now();
    setNotifications(prev => [
      { id, type, msg }, 
      ...prev
    ]);

    setTimeout(() => {
      setNotifications(prev => prev.filter(n => n.id !== id));
    }, 3000);
  };

  useEffect(() => {
    if (!driverNoticeVisible) return;
    driverNoticePillY.set(-16);
    const controls = animate(driverNoticePillY, 0, {
      type: "spring",
      stiffness: 500,
      damping: 35,
    });
    return () => controls.stop();
  }, [driverNoticePillY, driverNoticeVisible]);

  const triggerDriverNotice = () => {
    setDriverNoticeActive(true);
    if (!driverNoticeOverlayShown.current) {
      driverNoticeOverlayShown.current = true;
      setFullScreenStatus('DRIVER_MISSING');
    }
  };

  const isDriverMissingError = (err) => {
    const normalized = String(err ?? '').trim();
    if (!normalized) return false;
    const upper = normalized.toUpperCase();
    if (upper === 'RAWACCEL_NOT_INSTALLED' || upper === 'DRIVER_MISSING') return true;
    if (normalized.includes('未检测到驱动')) return true;
    return false;
  };

  const updateAmbientDebug = (force = false) => {
    const now = typeof performance !== 'undefined' ? performance.now() : Date.now();
    if (!force && now - ambientDebugLast.current < 80) return;
    ambientDebugLast.current = now;
    setAmbientDebug({
      brightness: ambientSmoothed.current,
      measured: ambientMeasured.current,
      top: ambientTop.current,
      bottom: ambientBottom.current,
      vignette: ambientVignette.current,
    });
  };

  const updateAmbientSamples = (samples, force = false) => {
    if (!Array.isArray(samples)) return;
    ambientSamplesRef.current = samples;
    const now = typeof performance !== 'undefined' ? performance.now() : Date.now();
    if (!force && now - ambientSamplesLast.current < 80) return;
    ambientSamplesLast.current = now;
    setAmbientSamples(samples);
  };

  const setAmbientConfigValue = (segment, key, raw) => {
    const next = Number.parseFloat(raw);
    if (!Number.isFinite(next)) return;
    setAmbientConfig((prev) => ({
      ...prev,
      [segment]: {
        ...prev[segment],
        [key]: clamp01(next),
      },
    }));
  };

  const setPreviewBrightnessValue = (value) => {
    const next = clamp01(value);
    setAmbientPreviewBrightness(next);
  };

  const setAmbientSmoothMsValue = (raw) => {
    const next = Number.parseFloat(raw);
    if (!Number.isFinite(next)) return;
    setAmbientSmoothMs(clampAmbientSmoothMs(next));
  };

  useEffect(() => {
    ambientSmoothMsRef.current = ambientSmoothMs;
    if (!ambientRaf.current) {
      ambientStepTime.current = null;
      ambientRaf.current = requestAnimationFrame(stepAmbient);
    }
  }, [ambientSmoothMs]);

  useEffect(() => {
    if (!isTauri) return;
    tauriInvoke('backend_set_drop_extremes', { enabled: dropExtremesEnabled }).catch(() => {});
  }, [dropExtremesEnabled]);

  useEffect(() => {
    if (!isTauri) return;
    tauriInvoke('backend_set_ema_enabled', { enabled: emaEnabled }).catch(() => {});
  }, [emaEnabled]);

  useEffect(() => {
    const onKeyDown = (event) => {
      if (!event) return;
      if (
        event.code === AMBIENT_DEBUG_HOTKEY.code &&
        event.ctrlKey === AMBIENT_DEBUG_HOTKEY.ctrl &&
        event.altKey === AMBIENT_DEBUG_HOTKEY.alt &&
        event.shiftKey === AMBIENT_DEBUG_HOTKEY.shift
      ) {
        event.preventDefault();
        setShowAmbientDebugPanel((prev) => {
          const next = !prev;
          try {
            window.localStorage.setItem(AMBIENT_DEBUG_STORAGE_KEY, next ? '1' : '0');
          } catch {}
          return next;
        });
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, []);

  const ambientSampleRects = useMemo(() => {
    const source = AMBIENT_SAMPLE_SOURCE;
    const xPoints = AMBIENT_SAMPLE_MARKERS.x.map((fraction) => fraction * WINDOW_WIDTH);
    const yPoints = AMBIENT_SAMPLE_MARKERS.y.map((fraction) => fraction * WINDOW_HEIGHT);
    const rects = [];

    AMBIENT_SAMPLE_RING_OFFSETS.forEach((margin, ringIndex) => {
      xPoints.forEach((x, index) => {
        rects.push({
          index: getSampleIndex(ringIndex, 'top', index),
          left: x - source / 2,
          top: -margin - source,
          width: source,
          height: source,
        });
        rects.push({
          index: getSampleIndex(ringIndex, 'bottom', index),
          left: x - source / 2,
          top: WINDOW_HEIGHT + margin,
          width: source,
          height: source,
        });
      });
      yPoints.forEach((y, index) => {
        rects.push({
          index: getSampleIndex(ringIndex, 'left', index),
          left: -margin - source,
          top: y - source / 2,
          width: source,
          height: source,
        });
        rects.push({
          index: getSampleIndex(ringIndex, 'right', index),
          left: WINDOW_WIDTH + margin,
          top: y - source / 2,
          width: source,
          height: source,
        });
      });
    });

    return rects;
  }, []);

  const applyAmbientConfig = () => {
    if (typeof window === 'undefined') return;
    try {
      window.localStorage.setItem(
        AMBIENT_SETTINGS_STORAGE_KEY,
        JSON.stringify({
          config: ambientConfig,
          dropExtremesEnabled,
          emaEnabled,
          brightnessSmoothMs: ambientSmoothMs,
        })
      );
      window.localStorage.setItem(
        AMBIENT_CONFIG_STORAGE_KEY,
        JSON.stringify(ambientConfig)
      );
    } catch {}
  };

  const buildAmbientLogPayload = () => ({
    timestamp: new Date().toISOString(),
    measured: ambientMeasured.current,
    target: ambientSmoothed.current,
    top: ambientTop.current,
    bottom: ambientBottom.current,
    vignette: ambientVignette.current,
    previewEnabled: ambientPreviewEnabledRef.current,
    previewBrightness: ambientPreviewBrightness,
    dropExtremesEnabled,
    emaEnabled,
    brightnessSmoothMs: ambientSmoothMsRef.current,
    config: ambientConfigRef.current,
    samples: ambientSamplesRef.current,
  });

  const queueAmbientLog = () => {
    const payload = buildAmbientLogPayload();
    ambientLogBuffer.current.push(JSON.stringify(payload));
    if (ambientLogBuffer.current.length >= AMBIENT_LOG_BUFFER_LIMIT) {
      flushAmbientLogBuffer();
    }
  };

  const flushAmbientLogBuffer = async (silent = false) => {
    if (!isTauri) return;
    if (ambientLogFlushBusy.current) return;
    const fs = ambientLogFsRef.current;
    if (!fs || ambientLogBuffer.current.length === 0) return;
    ambientLogFlushBusy.current = true;
    const lines = ambientLogBuffer.current.splice(0, ambientLogBuffer.current.length);
    try {
      await fs.writeTextFile(
        `${fs.logDir}/${fs.logFile}`,
        `${lines.join('\n')}\n`,
        { dir: fs.BaseDirectory.AppData, append: true }
      );
    } catch (error) {
      ambientLogBuffer.current.unshift(...lines);
      if (!silent && !ambientLogError.current) {
        ambientLogError.current = true;
        console.error(error);
        addNotification('error', '日志写入失败');
      }
    } finally {
      ambientLogFlushBusy.current = false;
    }
  };

  const startAmbientLogging = async () => {
    if (!isTauri) {
      addNotification('error', '日志仅支持桌面应用');
      return;
    }
    if (ambientLoggingRef.current) return;
    ambientLogError.current = false;
    ambientLogBuffer.current = [];
    try {
      const { writeTextFile, createDir, BaseDirectory } = await import('@tauri-apps/api/fs');
      const { appDataDir, join } = await import('@tauri-apps/api/path');
      const logDir = 'ambient-logs';
      const stamp = new Date().toISOString().replace(/[:.]/g, '-');
      const logFile = `ambient-log-${stamp}.jsonl`;
      await createDir(logDir, { dir: BaseDirectory.AppData, recursive: true });
      const base = await appDataDir();
      const fullPath = await join(base, logDir, logFile);
      ambientLogFsRef.current = { writeTextFile, BaseDirectory, logDir, logFile };
      ambientLogPathRef.current = fullPath;
      ambientLoggingRef.current = true;
      setAmbientLogging(true);
      queueAmbientLog();
      if (ambientLogFlushTimer.current) clearInterval(ambientLogFlushTimer.current);
      ambientLogFlushTimer.current = setInterval(() => {
        flushAmbientLogBuffer(true);
      }, AMBIENT_LOG_FLUSH_INTERVAL);
      const tick = () => {
        if (!ambientLoggingRef.current) return;
        queueAmbientLog();
        ambientLogRaf.current = requestAnimationFrame(tick);
      };
      ambientLogRaf.current = requestAnimationFrame(tick);
      addNotification('success', `开始记录 ${fullPath}`);
    } catch (error) {
      console.error(error);
      ambientLoggingRef.current = false;
      setAmbientLogging(false);
      addNotification('error', '日志写入失败');
    }
  };

  const stopAmbientLogging = async (silent = false) => {
    if (!ambientLoggingRef.current) return;
    ambientLoggingRef.current = false;
    setAmbientLogging(false);
    if (ambientLogRaf.current) {
      cancelAnimationFrame(ambientLogRaf.current);
      ambientLogRaf.current = 0;
    }
    if (ambientLogFlushTimer.current) {
      clearInterval(ambientLogFlushTimer.current);
      ambientLogFlushTimer.current = null;
    }
    await flushAmbientLogBuffer(true);
    if (!silent) {
      const target = ambientLogPathRef.current;
      addNotification('success', target ? `已停止记录 ${target}` : '已停止记录');
    }
  };

  const toggleAmbientLogging = () => {
    if (ambientLoggingRef.current) {
      stopAmbientLogging();
      return;
    }
    startAmbientLogging();
  };

  useEffect(() => {
    return () => {
      ambientLoggingRef.current = false;
      if (ambientLogRaf.current) {
        cancelAnimationFrame(ambientLogRaf.current);
        ambientLogRaf.current = 0;
      }
      if (ambientLogFlushTimer.current) {
        clearInterval(ambientLogFlushTimer.current);
        ambientLogFlushTimer.current = null;
      }
      flushAmbientLogBuffer(true);
    };
  }, []);

  const getAmbientTargets = (value) => {
    const v = Math.max(AMBIENT_SEG1_START, Math.min(AMBIENT_SEG3_END, value));
    const { seg1, seg2, seg3, seg4 } = ambientConfigRef.current;
    const lerp = (start, end, t) => start + (end - start) * t;
    const blend = (fromTop, fromBottom, fromVignette, toTop, toBottom, toVignette, start, end) => {
      if (end <= start) return { top: toTop, bottom: toBottom, vignette: toVignette };
      const t = Math.max(0, Math.min(1, (v - start) / (end - start)));
      return {
        top: lerp(fromTop, toTop, t),
        bottom: lerp(fromBottom, toBottom, t),
        vignette: lerp(fromVignette, toVignette, t),
      };
    };

    if (v <= AMBIENT_SEG1_END) {
      return blend(
        seg1.top,
        seg1.bottom,
        seg1.vignette,
        seg2.top,
        seg2.bottom,
        seg2.vignette,
        AMBIENT_SEG1_START,
        AMBIENT_SEG1_END
      );
    }
    if (v <= AMBIENT_SEG2_END) {
      return blend(
        seg2.top,
        seg2.bottom,
        seg2.vignette,
        seg3.top,
        seg3.bottom,
        seg3.vignette,
        AMBIENT_SEG1_END,
        AMBIENT_SEG2_END
      );
    }
    return blend(
      seg3.top,
      seg3.bottom,
      seg3.vignette,
      seg4.top,
      seg4.bottom,
      seg4.vignette,
      AMBIENT_SEG2_END,
      AMBIENT_SEG3_END
    );
  };

  const applyAmbientStyle = (top, bottom, vignette) => {
    const el = containerRef.current;
    if (!el) return;
    el.style.setProperty('--glass-top-alpha', top.toFixed(3));
    el.style.setProperty('--glass-bottom-alpha', bottom.toFixed(3));
    el.style.setProperty('--glass-vignette-alpha', vignette.toFixed(3));
  };

  const stepAmbient = (timestamp) => {
    const now = typeof timestamp === 'number' ? timestamp : performance.now();
    const last = ambientStepTime.current;
    const dt = last == null ? 0 : Math.min(64, Math.max(0, now - last));
    ambientStepTime.current = now;

    const targetBrightness = ambientTarget.current;
    const currentBrightness = ambientSmoothed.current;
    const smoothMs = Math.max(0, ambientSmoothMsRef.current ?? 0);
    const alpha =
      smoothMs > 0
        ? 1 - Math.exp(-dt / smoothMs)
        : 1;
    const nextBrightness =
      currentBrightness + (targetBrightness - currentBrightness) * alpha;
    ambientSmoothed.current = nextBrightness;

    const { top: nextTop, bottom: nextBottom, vignette: nextVignette } =
      getAmbientTargets(nextBrightness);
    ambientTop.current = nextTop;
    ambientBottom.current = nextBottom;
    ambientVignette.current = nextVignette;
    applyAmbientStyle(nextTop, nextBottom, nextVignette);
    updateAmbientDebug();

    if (Math.abs(targetBrightness - nextBrightness) > AMBIENT_BRIGHTNESS_EPSILON) {
      ambientRaf.current = requestAnimationFrame(stepAmbient);
      return;
    }

    ambientSmoothed.current = targetBrightness;
    const { top, bottom, vignette } = getAmbientTargets(targetBrightness);
    ambientTop.current = top;
    ambientBottom.current = bottom;
    ambientVignette.current = vignette;
    applyAmbientStyle(top, bottom, vignette);
    updateAmbientDebug(true);
    ambientRaf.current = 0;
    ambientStepTime.current = null;
  };

  const closeContextMenu = () => setContextMenu(null);

  const requestClose = () => {
    if (isClosing || closeRequested.current) return;
    closeRequested.current = true;
    closeContextMenu();

    if (!isTauri) {
      window.close?.();
      return;
    }

    tauriInvoke('ui_save_state', { state: { crosshairMemory: crosshairMemory.current } }).catch(() => {});

    let finished = false;
    const timeoutId = window.setTimeout(() => {
      if (finished) return;
      finished = true;
      pendingExit.current = null;
      tauriClose().catch(() => {});
    }, 8000);

    pendingExit.current = () => {
      if (finished) return;
      finished = true;
      window.clearTimeout(timeoutId);
      tauriClose().catch(() => {});
    };

    tauriInvoke('backend_quit').catch(() => {
      const done = pendingExit.current;
      pendingExit.current = null;
      if (typeof done === 'function') done();
    });
  };

  const requestCrosshairToggle = () => {
    if (!isMouseActive) return;

    const next = !isCrosshairActive;
    crosshairMemory.current = next;
    tauriInvoke('ui_save_state', { state: { crosshairMemory: next } }).catch(() => {});
    isCrosshairActiveRef.current = next;
    setIsCrosshairActive(next);
    setIsFiring(false);
    tauriInvoke('backend_set_feature', { enabled: next }).catch(() => {});
  };

  const applyUiReset = () => {
    setIsCrosshairActive(false);
    setIsFiring(false);
    isCrosshairActiveRef.current = false;
    setFullScreenStatus(null);
    setNotifications([]);
    setPhase('SCAN');
    setShakeProgress(0);
    setMouseStatus('OFF');
    mouseStatusRef.current = 'OFF';
    setSensitivity(1.0);
    pendingSensitivity.current = null;
    pendingPower.current = null;
    setIsSyncing(false);
    tauriShowAndFocus().catch(() => {});
  };

  const requestUnbindMouse = () => {
    closeContextMenu();
    applyUiReset();
    if (isTauri) {
      tauriInvoke('backend_full_reset').catch(() => {});
    }
  };

  useEffect(() => {
    if (!contextMenu) return;

    const handleMouseDown = () => setContextMenu(null);
    const handleKeyDown = (e) => {
      if (e.key === 'Escape') setContextMenu(null);
    };

    window.addEventListener('mousedown', handleMouseDown);
    window.addEventListener('keydown', handleKeyDown);
    return () => {
      window.removeEventListener('mousedown', handleMouseDown);
      window.removeEventListener('keydown', handleKeyDown);
    };
  }, [contextMenu]);

  useEffect(() => {
    isCrosshairActiveRef.current = isCrosshairActive;
  }, [isCrosshairActive]);

  useEffect(() => {
    mouseStatusRef.current = mouseStatus;
  }, [mouseStatus]);

  useEffect(() => {
    phaseRef.current = phase;
  }, [phase]);

  // Tauri 后端事件桥接：用本地 backend 驱动 UI 状态（扫描/开火/报错等）
  useEffect(() => {
    if (!isTauri) return;

    let unlisten = null;

    (async () => {
      try {
        try {
          const uiState = await tauriInvoke('ui_load_state');
          if (uiState && typeof uiState.crosshairMemory === 'boolean') {
            crosshairMemory.current = uiState.crosshairMemory;
          }
        } catch {}

        unlisten = await tauriListen('backend_event', (event) => {
          const payload = event?.payload || {};
          const kind = payload.kind;
          const raw = payload?.data?.raw || '';

          if (!kind) return;

          if (kind === 'SCAN_PROGRESS') {
            const v = Number.parseFloat(raw);
            if (!Number.isFinite(v)) return;
            const next = Math.max(0, Math.min(100, v));
            setShakeProgress(next);
            if (phaseRef.current === 'INIT' && next < 100) {
              setPhase('SCAN');
            }
            return;
          }

          if (kind === 'INPUT_READY') {
            setScanInputReady(true);
            return;
          }

          if (kind === 'REGISTERED') {
            setFullScreenStatus(null);
            addNotification('success', '鼠标已绑定');
            setShakeProgress(100);
            setPhase('DASHBOARD');
            return;
          }

          if (kind === 'FIRING') {
            const on = raw.trim().toUpperCase().startsWith('ON');
            if (!on) {
              setIsFiring(false);
              return;
            }

            if (!isCrosshairActiveRef.current || mouseStatusRef.current !== 'ON') {
              setIsFiring(false);
              return;
            }

            setIsFiring(true);
            return;
          }

          if (kind === 'EXITING') {
            setIsClosing(true);
            return;
          }

          if (kind === 'EXITED') {
            const done = pendingExit.current;
            pendingExit.current = null;
            if (typeof done === 'function') done();
            return;
          }

          if (kind === 'POWER_APPLIED') {
            const on = raw.trim().toUpperCase().startsWith('ON');
            pendingPower.current = null;
            mouseStatusRef.current = on ? 'ON' : 'OFF';
            if (on) {
              setMouseStatus('ON');
              if (crosshairMemory.current) {
                isCrosshairActiveRef.current = true;
                setIsCrosshairActive(true);
                tauriInvoke('backend_set_feature', { enabled: true }).catch(() => {});
              }
            } else {
              setMouseStatus('OFF');
              isCrosshairActiveRef.current = false;
              setIsCrosshairActive(false);
              setIsFiring(false);
            }
            return;
          }

          if (kind === 'FEATURE') {
            const on = raw.trim().toUpperCase().startsWith('ON');
            isCrosshairActiveRef.current = on;
            setIsCrosshairActive(on);
            if (!on) setIsFiring(false);
            return;
          }

          if (kind === 'RESET') {
            applyUiReset();
            return;
          }

          if (kind === 'SENS_APPLIED') {
            const v = Number.parseFloat(raw);
            if (!Number.isFinite(v)) return;

            const pending = pendingSensitivity.current;
            if (pending == null) {
              const next = Math.max(0.01, Math.min(5.0, v));
              skipNextSensitivitySend.current = true;
              setSensitivity(next);
              setIsSyncing(false);
              return;
            }

            if (Math.abs(v - pending) < 0.02) {
              pendingSensitivity.current = null;
              setIsSyncing(false);
            }
            return;
          }

          if (kind === 'NOTIFY') {
            const msg = String(raw ?? '').trim();
            if (!msg) return;

            if (msg.startsWith('FS:')) {
              const status = msg.slice(3).trim().toUpperCase();
              if (status) setFullScreenStatus(status);
              return;
            }

            if (msg.startsWith('ERR:')) {
              const err = msg.slice(4).trim();
              if (isDriverMissingError(err)) {
                triggerDriverNotice();
                return;
              }
              addNotification('error', err || '未知错误');
              return;
            }

            if (msg.startsWith('OK:')) {
              const ok = msg.slice(3).trim();
              addNotification('success', ok || 'OK');
              return;
            }

            addNotification('warn', msg);
            return;
          }
        });
        await tauriInvoke('backend_init');
      } catch (e) {
        console.error(e);
      }
    })();

    return () => {
      if (unlisten) unlisten();
    };
  }, []);

  useEffect(() => {
    applyAmbientStyle(ambientTop.current, ambientBottom.current, ambientVignette.current);
  }, []);

  useEffect(() => {
    ambientConfigRef.current = ambientConfig;
    updateAmbientDebug(true);
    if (!ambientRaf.current) {
      ambientStepTime.current = null;
      ambientRaf.current = requestAnimationFrame(stepAmbient);
    }
  }, [ambientConfig]);

  useEffect(() => {
    ambientPreviewEnabledRef.current = ambientPreviewEnabled;
  }, [ambientPreviewEnabled]);

  useEffect(() => {
    if (ambientPreviewEnabled) {
      ambientTarget.current = clamp01(ambientPreviewBrightness);
    } else {
      ambientTarget.current = ambientMeasured.current;
    }
    updateAmbientDebug(true);
    if (!ambientRaf.current) {
      ambientStepTime.current = null;
      ambientRaf.current = requestAnimationFrame(stepAmbient);
    }
  }, [ambientPreviewEnabled, ambientPreviewBrightness]);

  useEffect(() => {
    if (!isTauri) return;

    let unlisten = null;
    let unlistenSamples = null;

    (async () => {
      try {
        unlisten = await tauriListen('ambient-brightness', (event) => {
          const raw = event?.payload;
          const value = typeof raw === 'number' ? raw : Number.parseFloat(raw);
          if (!Number.isFinite(value)) return;
          ambientMeasured.current = clamp01(value);
          if (!ambientPreviewEnabledRef.current) {
            ambientTarget.current = ambientMeasured.current;
            if (!ambientRaf.current) {
              ambientStepTime.current = null;
              ambientRaf.current = requestAnimationFrame(stepAmbient);
            }
          }
          updateAmbientDebug(true);
        });
        unlistenSamples = await tauriListen('ambient-samples', (event) => {
          const payload = event?.payload;
          const samples = Array.isArray(payload) ? payload : payload?.samples;
          if (!Array.isArray(samples)) return;
          const next = Array.from({ length: AMBIENT_SAMPLE_TOTAL }, (_, index) => {
            const value = samples[index];
            return typeof value === 'number' && Number.isFinite(value) ? clamp01(value) : null;
          });
          updateAmbientSamples(next, true);
        });
      } catch {}
    })();

    return () => {
      if (unlisten) unlisten();
      if (unlistenSamples) unlistenSamples();
      if (ambientRaf.current) {
        cancelAnimationFrame(ambientRaf.current);
        ambientRaf.current = 0;
        ambientStepTime.current = null;
      }
    };
  }, []);

  // 模拟开机启动时间
  useEffect(() => {
    if (phase === 'DASHBOARD') {
      setMouseStatus('BOOTING');
      const bootTimer = setTimeout(() => {
        if (isTauri) {
          pendingPower.current = 'ON';
          tauriInvoke('backend_set_power', { enabled: true }).catch(() => {});
          return;
        }
        setMouseStatus('ON');
      }, 600);
      return () => clearTimeout(bootTimer);
    }
  }, [phase]);

  // 扫描阶段逻辑：进度条满后跳转
  useEffect(() => {
    if (isTauri) return;
    if (phase === 'SCAN' && shakeProgress >= 100) {
      const timer = setTimeout(() => {
        setPhase('DASHBOARD');
      }, 400); 
      return () => clearTimeout(timer);
    }
  }, [shakeProgress, phase]);

  // --- [后端注意] 灵敏度调节同步 ---
  // 对应 CLI 的 'l' 命令。
  // 注意：后端调整灵敏度有耗时（写文件 + writer.exe），前端用 SYNCING 动画等待后端确认。
  // 只有在鼠标开关打开 (ON) 时，调节灵敏度才会有同步动画反馈。
  useEffect(() => {
    if (isFirstRender.current) {
      isFirstRender.current = false;
      return;
    }

    if (skipNextSensitivitySend.current) {
      skipNextSensitivitySend.current = false;
      return;
    }

    if (mouseStatus === 'ON') {
      setIsSyncing(true);
    }
     
    // 写入后端（拖动时防抖，避免频繁写 settings.json）
    if (debounceTimer.current) clearTimeout(debounceTimer.current);
    debounceTimer.current = setTimeout(() => {
      pendingSensitivity.current = sensitivity;
      tauriInvoke('backend_set_sensitivity', { value: sensitivity }).catch(() => {});

      // Preview mode: simulate backend latency.
      if (!isTauri && mouseStatus === 'ON') {
        if (syncTimer.current) clearTimeout(syncTimer.current);
        syncTimer.current = setTimeout(() => {
          setIsSyncing(false);
        }, 1000);
      }
    }, 250);
     
    return () => {
      clearTimeout(debounceTimer.current);
      clearTimeout(syncTimer.current);
    };
  }, [sensitivity]);

  useEffect(() => {
    if (mouseStatus === 'ON') return;
    pendingSensitivity.current = null;
    setIsSyncing(false);
  }, [mouseStatus]);

  useEffect(() => {
    return () => {
      if (resetPulseTimer.current) {
        clearTimeout(resetPulseTimer.current);
      }
      if (shutdownPulseTimer.current) {
        clearTimeout(shutdownPulseTimer.current);
      }
    };
  }, []);

  // --- 鼠标移动监听逻辑 ---
  useEffect(() => {
    if (isTauri) return;
    const handleMove = (e) => {
      const current = { x: e.clientX, y: e.clientY };
      setMousePos(current);

      if (lastMousePos.current === null) {
        lastMousePos.current = current;
        return;
      }

      const dx = Math.abs(current.x - lastMousePos.current.x);
      const dy = Math.abs(current.y - lastMousePos.current.y);
      const dist = Math.sqrt(dx * dx + dy * dy);

      // --- [后端注意] 注册界面逻辑 ---
      // 逻辑：所有鼠标移动数据累积。
      // 意图：哪个鼠标最终让进度条到达 100%，就注册/绑定哪个鼠标。
      if (phase === 'SCAN') {
        if (dist > 5) {
          setShakeProgress(prev => {
            if (prev >= 100) return 100;
            const next = prev + (dist / 15); 
            return next >= 100 ? 100 : next;
          });
        }
      }

      // 模拟自动开火逻辑（仅演示用）
      if (phase === 'DASHBOARD' && isCrosshairActive) {
         setIsFiring(false);
         clearTimeout(fireTimer.current);
         fireTimer.current = setTimeout(() => {
            setIsFiring(true);
         }, 100); 
      }

      lastMousePos.current = current;
    };

    window.addEventListener('mousemove', handleMove);
    return () => {
      window.removeEventListener('mousemove', handleMove);
      clearTimeout(fireTimer.current);
    };
  }, [phase, isCrosshairActive]);

  // --- 键盘事件监听 ---
  useEffect(() => {
    const handleKey = (e) => {
      const key = e.key.toLowerCase();
      const isResetKey = e.code === 'KeyR' || key === 'r';

      if (isTauri && e.ctrlKey && key === 'r') {
        e.preventDefault();
        window.location.reload();
        return;
      }

      if (isTauri && e.ctrlKey && key === 'm') {
        e.preventDefault();
        tauriMinimize().catch(() => {});
        return;
      }

      if (isTauri && e.ctrlKey && key === 'q') {
        e.preventDefault();
        requestClose();
        return;
      }

      if (phase === 'DASHBOARD' && isResetKey && !e.ctrlKey && !e.metaKey && !e.altKey) {
        if (sensitivity !== 1.0) {
          setSensitivity(1.0);
        } else {
          triggerResetPulse();
        }
      }

      // --- [后端注意] CapsLock 双击逻辑 ---
      // 对应 CLI 逻辑：
      // 1. 彻底恢复灵敏度 (重置为默认)。
      // 2. 关闭自动按钮功能 (isCrosshairActive = false)。
      // 3. 解绑鼠标，回到注册界面 (Phase -> SCAN)。
      // [修改说明] 为实现“并行处理”，此处移除了所有的 setTimeout 延迟。
      // 交互逻辑：双击后立即触发界面切换动画（Dashboard退场 -> Scan进场）。
      // 意图：利用转场动画本身的时间（约0.5-0.8秒）来掩盖后端重置所需的1秒耗时。
      // 后端请在收到此信号后，在后台异步执行重置操作。
      // Tauri 模式下改为后端全局监听（即使窗口在后台也生效）
      if (!isTauri && (e.code === 'CapsLock' || e.key === 'CapsLock')) {
        const now = Date.now();
        if (now - lastCapsLockTime.current < 300) {
          if (phase === 'DASHBOARD') {
            applyUiReset();
          }
        }
        lastCapsLockTime.current = now;
      }
    };
    window.addEventListener('keydown', handleKey, true);
    return () => window.removeEventListener('keydown', handleKey, true);
  }, [phase, sensitivity]); 

  const sliderPercent = fromSplitScale(sensitivity);
  const syncActive = isSyncing || resetPulse;

  const triggerResetPulse = () => {
    if (resetPulseTimer.current) {
      clearTimeout(resetPulseTimer.current);
    }
    setResetPulse(true);
    resetPulseTimer.current = setTimeout(() => {
      setResetPulse(false);
    }, 600);
  };

  const triggerShutdownPulse = () => {
    if (shutdownPulseTimer.current) {
      clearTimeout(shutdownPulseTimer.current);
    }
    setShutdownPulse(true);
    shutdownPulseTimer.current = setTimeout(() => {
      setShutdownPulse(false);
    }, 700);
  };

  // --- [后端注意] 左下角鼠标按钮逻辑 ---
  // 开关逻辑：
  // 关 (OFF): 
  //   - 对应 CLI：关闭自动按左键功能。
  //   - 对应 CLI：恢复鼠标灵敏度到 1.0 (或默认值)。
  // 开 (ON):
  //   - 记忆功能：如果上次关机前右边的瞄准镜是开着的，这次开机也要自动打开。
  const handleMouseToggle = () => {
    if (mouseStatus === 'OFF') {
      if (shutdownPulseTimer.current) {
        clearTimeout(shutdownPulseTimer.current);
      }
      setShutdownPulse(false);
      mouseStatusRef.current = 'BOOTING';
      setMouseStatus('BOOTING');
      if (isTauri) {
        pendingPower.current = 'ON';
        tauriInvoke('backend_set_power', { enabled: true }).catch(() => {});
        return;
      }
      setTimeout(() => {
        mouseStatusRef.current = 'ON';
        setMouseStatus('ON');
        if (crosshairMemory.current) {
          isCrosshairActiveRef.current = true;
          setIsCrosshairActive(true);
        }
      }, 1000);
    } else if (mouseStatus === 'ON') {
      triggerShutdownPulse();
      crosshairMemory.current = isCrosshairActive;
      tauriInvoke('ui_save_state', { state: { crosshairMemory: crosshairMemory.current } }).catch(() => {});

      mouseStatusRef.current = 'SHUTTING_DOWN';
      setMouseStatus('SHUTTING_DOWN');
      isCrosshairActiveRef.current = false;
      setIsCrosshairActive(false);
      setIsFiring(false);
      if (isTauri) {
        tauriInvoke('backend_set_feature', { enabled: false }).catch(() => {});
        pendingPower.current = 'OFF';
        tauriInvoke('backend_set_power', { enabled: false }).catch(() => {});
        return;
      }
      setTimeout(() => {
        mouseStatusRef.current = 'OFF';
        setMouseStatus('OFF');
      }, 1000);
    }
  };

  const isMouseActive = mouseStatus === 'ON'; 
  const isProcessing = mouseStatus === 'BOOTING' || mouseStatus === 'SHUTTING_DOWN';
  const titleControlIconStyle = useMemo(() => {
    const value = clamp01(ambientDebug.brightness);
    const t = clamp01((value - 0.2) / 0.6);
    const brightness = 1 + t * 1.2;
    const opacity = 0.65 + t * 0.35;
    return { filter: `brightness(${brightness})`, opacity };
  }, [ambientDebug.brightness]);
  const titleControlHoverStyle = useMemo(() => {
    const value = clamp01(ambientDebug.brightness);
    const mix = clamp01((value - 0.15) / 0.7);
    const channel = Math.round(255 * (1 - mix));
    const alpha = 0.14 + (0.1 - 0.14) * mix;
    return { '--title-hover-bg': `rgba(${channel}, ${channel}, ${channel}, ${alpha.toFixed(3)})` };
  }, [ambientDebug.brightness]);

  return (
    <div
      className={`dark flex items-center justify-center w-full h-screen ${
        isTauri ? 'bg-transparent' : 'bg-gray-900/50'
      }`}
    >
      
      {/* --- [前端交互] 窗口容器 ---
         注意：整个窗口除了特定的按钮和拉条区域外，
         都应该支持拖拽移动 (通过 CSS WebkitAppRegion: 'drag' 实现)。
       */}
        <div
           ref={containerRef}
           style={{
             width: WINDOW_WIDTH,
             height: WINDOW_HEIGHT,
             '--glass-top-alpha': ambientConfig.seg4.top.toFixed(2),
             '--glass-bottom-alpha': ambientConfig.seg4.bottom.toFixed(2),
             '--glass-vignette-alpha': ambientConfig.seg4.vignette.toFixed(2),
           }}
              className={`relative overflow-hidden bg-zinc-950/10 text-zinc-200 font-mono select-none transition-all duration-300 shadow-2xl rounded-[8px] border border-white/10
              ${isFiring ? 'cursor-crosshair' : 'cursor-default'}
            `}
            onMouseDown={(e) => {
             if (!isTauri) return;
             if (contextMenu) {
               setContextMenu(null);
               return;
             }
             if (e.button !== 0) return;
 
             const target = e.target instanceof Element ? e.target : null;
             if (target) {
               if (target.closest('button, input, textarea, select, option, a, [data-no-drag]')) {
                 return;
               }
             }

             const now = Date.now();
             const last = lastWindowClick.current;
             const lastPos = lastWindowClickPos.current;
             const sameSpot = lastPos
               ? Math.hypot(e.clientX - lastPos.x, e.clientY - lastPos.y) <= 6
               : false;

             if (last && now - last < 320 && sameSpot) {
               lastWindowClick.current = 0;
               lastWindowClickPos.current = null;
               dragArmed.current = false;
               dragStart.current = null;
               tauriMinimize().catch(() => {});
               return;
             }

             lastWindowClick.current = now;
             lastWindowClickPos.current = { x: e.clientX, y: e.clientY };
             dragArmed.current = true;
             dragStart.current = { x: e.clientX, y: e.clientY };
           }}
            onMouseMove={(e) => {
             if (!isTauri) return;
             if (!dragArmed.current) return;
             if ((e.buttons & 1) !== 1) {
               dragArmed.current = false;
               dragStart.current = null;
               return;
             }
             const start = dragStart.current;
             if (!start) return;
             const distance = Math.hypot(e.clientX - start.x, e.clientY - start.y);
             if (distance < 4) return;

             dragArmed.current = false;
             dragStart.current = null;
             tauriStartDragging().catch(() => {});
           }}
            onMouseUp={() => {
             dragArmed.current = false;
             dragStart.current = null;
           }}
            onMouseLeave={() => {
             dragArmed.current = false;
             dragStart.current = null;
            }}
            onContextMenu={(e) => {
              e.preventDefault();
              e.stopPropagation();

              const rect = containerRef.current?.getBoundingClientRect?.();
              const baseX = rect ? e.clientX - rect.left : e.clientX;
              const baseY = rect ? e.clientY - rect.top : e.clientY;

              const menuWidth = 224;
              const menuHeight = 300;
              const margin = 8;

              const x = Math.max(margin, Math.min(baseX, WINDOW_WIDTH - menuWidth - margin));
              const y = Math.max(margin, Math.min(baseY, WINDOW_HEIGHT - menuHeight - margin));

              setContextMenu({ x, y });
            }}
         >

         {/* Entire window is draggable; interactive elements opt-out via `button/input/...` or `data-no-drag`. */}

         {showAmbientDebugPanel && (
           <div
              data-no-drag
              className="absolute top-3 left-3 z-20 pointer-events-auto w-[230px] rounded-md border border-white/10 bg-black/45 px-2 py-1 text-[10px] leading-4 text-zinc-200"
            >
              <div className="flex items-center justify-between gap-2">
                <div className="font-semibold tracking-wide">亮度调参</div>
                <button
                  type="button"
                  data-no-drag
                  className="rounded px-1 text-[10px] text-zinc-300 hover:text-white"
                  onClick={() => setAmbientTunerOpen((v) => !v)}
                >
                  {ambientTunerOpen ? '收起' : '展开'}
                </button>
              </div>
              <div className="mt-1 space-y-0.5">
                <div>实时亮度 {ambientDebug.measured.toFixed(3)}</div>
                <div>生效亮度 {ambientDebug.brightness.toFixed(3)}</div>
                <div>渐变 {ambientDebug.top.toFixed(3)} / {ambientDebug.bottom.toFixed(3)}</div>
                <div>暗角 {ambientDebug.vignette.toFixed(3)}</div>
              </div>
              {ambientTunerOpen && (
                <div className="mt-2 space-y-2">
                  <label className="flex items-center gap-2">
                    <input
                      data-no-drag
                      type="checkbox"
                      checked={showAmbientSamplePoints}
                      onChange={(e) => setShowAmbientSamplePoints(e.target.checked)}
                      className="h-3 w-3 accent-white"
                    />
                    <span>显示采样点</span>
                  </label>
                  <label className="flex items-center gap-2">
                    <input
                      data-no-drag
                      type="checkbox"
                      checked={dropExtremesEnabled}
                      onChange={(e) => setDropExtremesEnabled(e.target.checked)}
                      className="h-3 w-3 accent-white"
                    />
                    <span>丢极值</span>
                  </label>
                  <label className="flex items-center gap-2">
                    <input
                      data-no-drag
                      type="checkbox"
                      checked={emaEnabled}
                      onChange={(e) => setEmaEnabled(e.target.checked)}
                      className="h-3 w-3 accent-white"
                    />
                    <span>EMA 平滑</span>
                  </label>
                  <div className="space-y-1">
                    <div className="flex items-center justify-between text-[9px] text-zinc-300">
                      <span>亮度平滑 (ms)</span>
                      <span className="text-zinc-400">{Math.round(ambientSmoothMs)}</span>
                    </div>
                    <input
                      data-no-drag
                      type="range"
                      min={AMBIENT_BRIGHTNESS_SMOOTH_MIN}
                      max={AMBIENT_BRIGHTNESS_SMOOTH_MAX}
                      step="10"
                      value={ambientSmoothMs}
                      onChange={(e) => setAmbientSmoothMsValue(e.target.value)}
                      className="w-full"
                    />
                    <input
                      data-no-drag
                      className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                      type="number"
                      min={AMBIENT_BRIGHTNESS_SMOOTH_MIN}
                      max={AMBIENT_BRIGHTNESS_SMOOTH_MAX}
                      step="10"
                      value={ambientSmoothMs}
                      onChange={(e) => setAmbientSmoothMsValue(e.target.value)}
                    />
                  </div>
                  {showAmbientSamplePoints && (
                    <div className="rounded border border-white/10 bg-black/30 p-2">
                      <div className="mb-1 text-[9px] text-zinc-400">采样区域示意</div>
                      <div
                        className="relative"
                        style={{
                          width: AMBIENT_SAMPLE_MAP_WIDTH * AMBIENT_SAMPLE_MAP_SCALE,
                          height: AMBIENT_SAMPLE_MAP_HEIGHT * AMBIENT_SAMPLE_MAP_SCALE,
                        }}
                      >
                        <div
                          className="absolute rounded border border-white/20"
                          style={{
                            left: AMBIENT_SAMPLE_OUTER_MARGIN * AMBIENT_SAMPLE_MAP_SCALE,
                            top: AMBIENT_SAMPLE_OUTER_MARGIN * AMBIENT_SAMPLE_MAP_SCALE,
                            width: WINDOW_WIDTH * AMBIENT_SAMPLE_MAP_SCALE,
                            height: WINDOW_HEIGHT * AMBIENT_SAMPLE_MAP_SCALE,
                          }}
                        />
                        {ambientSampleRects.map((rect) => (
                          <div
                            key={`rect-${rect.index}-${rect.left}-${rect.top}`}
                            className="absolute rounded-[2px] border border-white/15"
                            style={{
                              left: (AMBIENT_SAMPLE_OUTER_MARGIN + rect.left) * AMBIENT_SAMPLE_MAP_SCALE,
                              top: (AMBIENT_SAMPLE_OUTER_MARGIN + rect.top) * AMBIENT_SAMPLE_MAP_SCALE,
                              width: rect.width * AMBIENT_SAMPLE_MAP_SCALE,
                              height: rect.height * AMBIENT_SAMPLE_MAP_SCALE,
                            }}
                          >
                            <div className="absolute bottom-0 left-1/2 -translate-x-1/2 text-[8px] text-zinc-200/80">
                              {formatSampleValue(ambientSamples[rect.index])}
                            </div>
                          </div>
                        ))}
                      </div>
                    </div>
                  )}
                  <div className="grid grid-cols-[34px_1fr_1fr_1fr] items-center gap-1 text-[9px] text-zinc-300">
                  <div className="text-center text-zinc-400">亮度</div>
                  <div className="text-center">Top</div>
                  <div className="text-center">Bottom</div>
                  <div className="text-center">Vig</div>
                  <div className="text-center text-zinc-400">{AMBIENT_SEG1_START.toFixed(2)}</div>
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg1.top}
                    onChange={(e) => setAmbientConfigValue('seg1', 'top', e.target.value)}
                  />
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg1.bottom}
                    onChange={(e) => setAmbientConfigValue('seg1', 'bottom', e.target.value)}
                  />
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg1.vignette}
                    onChange={(e) => setAmbientConfigValue('seg1', 'vignette', e.target.value)}
                  />
                  <div className="text-center text-zinc-400">{AMBIENT_SEG1_END.toFixed(2)}</div>
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg2.top}
                    onChange={(e) => setAmbientConfigValue('seg2', 'top', e.target.value)}
                  />
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg2.bottom}
                    onChange={(e) => setAmbientConfigValue('seg2', 'bottom', e.target.value)}
                  />
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg2.vignette}
                    onChange={(e) => setAmbientConfigValue('seg2', 'vignette', e.target.value)}
                  />
                  <div className="text-center text-zinc-400">{AMBIENT_SEG2_END.toFixed(2)}</div>
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg3.top}
                    onChange={(e) => setAmbientConfigValue('seg3', 'top', e.target.value)}
                  />
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg3.bottom}
                    onChange={(e) => setAmbientConfigValue('seg3', 'bottom', e.target.value)}
                  />
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg3.vignette}
                    onChange={(e) => setAmbientConfigValue('seg3', 'vignette', e.target.value)}
                  />
                  <div className="text-center text-zinc-400">{AMBIENT_SEG3_END.toFixed(2)}</div>
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg4.top}
                    onChange={(e) => setAmbientConfigValue('seg4', 'top', e.target.value)}
                  />
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg4.bottom}
                    onChange={(e) => setAmbientConfigValue('seg4', 'bottom', e.target.value)}
                  />
                  <input
                    data-no-drag
                    className="w-full rounded border border-white/10 bg-black/30 px-1 py-0.5 text-[9px] text-zinc-100"
                    type="number"
                    min="0"
                    max="1"
                    step="0.01"
                    value={ambientConfig.seg4.vignette}
                    onChange={(e) => setAmbientConfigValue('seg4', 'vignette', e.target.value)}
                  />
                </div>
                <div className="flex items-center justify-end gap-2 pt-1">
                  <button
                    type="button"
                    data-no-drag
                    onClick={toggleAmbientLogging}
                    className={`rounded border px-2 py-1 text-[10px] ${
                      ambientLogging
                        ? 'border-red-500/40 bg-red-500/15 text-red-100 hover:bg-red-500/25'
                        : 'border-white/10 bg-white/5 text-zinc-200 hover:bg-white/10'
                    }`}
                  >
                    {ambientLogging ? '停止记录' : '开始记录'}
                  </button>
                  <button
                    type="button"
                    data-no-drag
                    onClick={applyAmbientConfig}
                    className="rounded border border-white/10 bg-white/10 px-2 py-1 text-[10px] text-zinc-100 hover:bg-white/20"
                  >
                    应用参数
                  </button>
                </div>
              </div>
            )}
         </div>
         )}

         {showAmbientSamplePoints && (
          <div className="absolute inset-0 z-10 pointer-events-none">
            {AMBIENT_SAMPLE_MARKERS.rings.map((offset, ringIndex) =>
              AMBIENT_SAMPLE_MARKERS.x.map((x, index) => {
                const value = ambientSamples[getSampleIndex(ringIndex, 'top', index)];
                return (
                  <div
                    key={`top-${ringIndex}-${x}`}
                    className="absolute -translate-x-1/2"
                    style={{ left: `${x * 100}%`, top: offset }}
                  >
                    <div className="flex flex-col items-center gap-0.5">
                      <div className="h-2 w-2 rounded-full bg-white/60 ring-1 ring-white/20" />
                      <div className="text-[8px] text-zinc-200/80">
                        {formatSampleValue(value)}
                      </div>
                    </div>
                  </div>
                );
              })
            )}
            {AMBIENT_SAMPLE_MARKERS.rings.map((offset, ringIndex) =>
              AMBIENT_SAMPLE_MARKERS.x.map((x, index) => {
                const value = ambientSamples[getSampleIndex(ringIndex, 'bottom', index)];
                return (
                  <div
                    key={`bottom-${ringIndex}-${x}`}
                    className="absolute -translate-x-1/2"
                    style={{ left: `${x * 100}%`, bottom: offset }}
                  >
                    <div className="flex flex-col items-center gap-0.5">
                      <div className="h-2 w-2 rounded-full bg-white/60 ring-1 ring-white/20" />
                      <div className="text-[8px] text-zinc-200/80">
                        {formatSampleValue(value)}
                      </div>
                    </div>
                  </div>
                );
              })
            )}
            {AMBIENT_SAMPLE_MARKERS.rings.map((offset, ringIndex) =>
              AMBIENT_SAMPLE_MARKERS.y.map((y, index) => {
                const value = ambientSamples[getSampleIndex(ringIndex, 'left', index)];
                return (
                  <div
                    key={`left-${ringIndex}-${y}`}
                    className="absolute -translate-y-1/2"
                    style={{ top: `${y * 100}%`, left: offset }}
                  >
                    <div className="flex flex-col items-center gap-0.5">
                      <div className="h-2 w-2 rounded-full bg-white/60 ring-1 ring-white/20" />
                      <div className="text-[8px] text-zinc-200/80">
                        {formatSampleValue(value)}
                      </div>
                    </div>
                  </div>
                );
              })
            )}
            {AMBIENT_SAMPLE_MARKERS.rings.map((offset, ringIndex) =>
              AMBIENT_SAMPLE_MARKERS.y.map((y, index) => {
                const value = ambientSamples[getSampleIndex(ringIndex, 'right', index)];
                return (
                  <div
                    key={`right-${ringIndex}-${y}`}
                    className="absolute -translate-y-1/2"
                    style={{ top: `${y * 100}%`, right: offset }}
                  >
                    <div className="flex flex-col items-center gap-0.5">
                      <div className="h-2 w-2 rounded-full bg-white/60 ring-1 ring-white/20" />
                      <div className="text-[8px] text-zinc-200/80">
                        {formatSampleValue(value)}
                      </div>
                    </div>
                  </div>
                );
              })
            )}
          </div>
         )}

         {/* bento-grid 风格背景（偏黑白，带轻微冷暖色偏移） */}
          <div className="absolute inset-0 z-0 pointer-events-none">
            <div
              className="absolute inset-0"
              style={{
                backgroundImage:
                  'linear-gradient(180deg, rgba(0,0,0,var(--glass-top-alpha)) 0%, rgba(0,0,0,var(--glass-bottom-alpha)) 100%)',
              }}
            />
            <div
              className="absolute inset-0 rounded-[8px] pointer-events-none"
              style={{ boxShadow: 'inset 0 0 96px rgba(0,0,0,var(--glass-vignette-alpha))' }}
            />
          </div>

          {/* 全屏 Overlay (报错/状态显示) */}
          <AnimatePresence>
            {fullScreenStatus && FULLSCREEN_CONFIG[fullScreenStatus] && (
              <motion.div
                key="fullscreen-overlay"
                initial={{ opacity: 0, backdropFilter: "blur(0px)" }}
                animate={{ opacity: 1, backdropFilter: "blur(8px)" }}
                exit={{ opacity: 0, backdropFilter: "blur(0px)" }}
                transition={{ duration: 0.3 }}
                className={`absolute inset-0 z-[300] bg-zinc-950/80 flex flex-col items-center justify-center cursor-default ${FULLSCREEN_CONFIG[fullScreenStatus].colorClass}`}
                onMouseDown={(e) => {
                  if (e.button !== 0) return;
                  overlayPointerStart.current = { x: e.clientX, y: e.clientY };
                  overlayPointerDragged.current = false;
                }}
                onMouseMove={(e) => {
                  const start = overlayPointerStart.current;
                  if (!start) return;
                  if (overlayPointerDragged.current) return;
                  const distance = Math.hypot(e.clientX - start.x, e.clientY - start.y);
                  if (distance >= 4) {
                    overlayPointerDragged.current = true;
                  }
                }}
                onMouseUp={() => {
                  if (!overlayPointerStart.current) return;
                  const dragged = overlayPointerDragged.current;
                  overlayPointerStart.current = null;
                  overlayPointerDragged.current = false;
                  if (dragged) return;
                  setFullScreenStatus(null);
                }}
                onMouseLeave={() => {
                  overlayPointerStart.current = null;
                  overlayPointerDragged.current = false;
                }}
              >
                <motion.div
                initial={{ scale: 0.9, opacity: 0 }}
                animate={{ scale: 1, opacity: 1 }}
                exit={{ scale: 0.9, opacity: 0 }}
                transition={{ delay: 0.1, type: "spring" }}
                className="flex flex-col items-center"
              >
                  <div className="flex flex-col items-center gap-4">
                     <h2 
                         className="text-2xl font-black tracking-[0.12em] drop-shadow-lg text-center px-4"
                         style={{ textShadow: `0 0 15px ${FULLSCREEN_CONFIG[fullScreenStatus].shadowColor}` }}
                     >
                         {FULLSCREEN_CONFIG[fullScreenStatus].title}
                     </h2>
                     
                     <div className={`flex items-center gap-2.5 px-4 py-1.5 rounded-full border bg-opacity-10 
                         ${FULLSCREEN_CONFIG[fullScreenStatus].bgClass} 
                         ${FULLSCREEN_CONFIG[fullScreenStatus].borderClass}
                         border-opacity-20 bg-opacity-10
                     `}>
                       <div className="relative flex items-center justify-center w-2 h-2">
                          {FULLSCREEN_CONFIG[fullScreenStatus].pulse && (
                              <div className={`absolute w-full h-full rounded-full animate-ping opacity-75 ${FULLSCREEN_CONFIG[fullScreenStatus].bgClass}`} />
                          )}
                          <div className={`relative w-1.5 h-1.5 rounded-full ${FULLSCREEN_CONFIG[fullScreenStatus].bgClass}`} />
                       </div>
                        <span className={`text-[10px] font-bold tracking-widest opacity-80`}>
                           {FULLSCREEN_CONFIG[fullScreenStatus].subtitle}
                        </span>
                     </div>

                    {fullScreenStatus === 'DRIVER_MISSING' && (
                      <div className="mt-2 w-[280px] rounded-2xl border border-white/10 bg-zinc-950/40 px-4 py-3 backdrop-blur-md text-[12px] leading-relaxed text-zinc-200/90 font-sans">
                        <div className="text-[12px] font-medium tracking-[0.04em] text-zinc-200/90 font-sans">需要做什么</div>
                        <div className="mt-2 space-y-1 text-zinc-300/90">
                          <div>1) 在便携版目录右键运行 “01_Install_RawAccel_Driver.exe”</div>
                          <div>2) 安装后重启电脑</div>
                          <div>3) 重启后再打开本软件</div>
                        </div>
                        <div className="mt-3 text-[11px] text-zinc-400/90">
                          自动点击功能仍可用，但调整灵敏度无效。
                        </div>
                      </div>
                    )}
                  </div>
                </motion.div>
             </motion.div>
           )}
         </AnimatePresence>

         {/* 常驻驱动提示胶囊（上拉关闭，点击打开详情） */}
         <AnimatePresence>
           {driverNoticeVisible && (
             <motion.div
               key="driver-notice-pill"
                initial={{ opacity: 0, scale: 0.96 }}
                animate={{ opacity: 1, scale: 1 }}
                exit={{ opacity: 0, scale: 0.96 }}
                transition={{ type: "spring", stiffness: 500, damping: 35 }}
                className="absolute top-4 left-0 w-full flex justify-center z-[410]"
                style={{ y: driverNoticePillY }}
                drag="y"
                dragConstraints={{ top: -80, bottom: 0 }}
                dragMomentum={false}
                dragElastic={0}
                onDragStart={() => {
                  driverPillDragging.current = true;
                }}
                onDragEnd={(_, info) => {
                  const shouldDismiss = info.offset.y < -32 || info.velocity.y < -700;
                  if (shouldDismiss) {
                    driverPillDragging.current = false;
                    setDriverNoticePillDismissed(true);
                    return;
                  }
                  animate(driverNoticePillY, 0, {
                    type: "spring",
                    stiffness: 500,
                    damping: 35,
                  });
                  window.setTimeout(() => {
                    driverPillDragging.current = false;
                  }, 0);
                }}
               onClick={() => {
                 if (driverPillDragging.current) return;
                 setFullScreenStatus('DRIVER_MISSING');
               }}
               data-no-drag
             >
               <div className="bg-amber-950/80 backdrop-blur-md border border-amber-500/20 text-amber-200 pl-1 pr-3 py-1 rounded-full shadow-[0_4px_20px_rgba(245,158,11,0.18)] flex items-center gap-2 whitespace-nowrap">
                 <div className="w-6 h-6 rounded-full bg-amber-500/20 flex items-center justify-center shrink-0">
                   <AlertCircle size={14} className="text-amber-400" />
                 </div>
                <span className="text-[10px] font-bold tracking-widest opacity-90">
                  未检测到驱动
                </span>
              </div>
            </motion.div>
          )}
        </AnimatePresence>

         {/* 顶部通知列表 */}
          <div className="absolute top-8 left-0 w-full flex justify-center z-[400] pointer-events-none">
             <AnimatePresence mode='popLayout'>
                 {notifications.map((notif, index) => {
                    const styleConfig = notif.type === 'error' ? {
                         bg: "bg-red-950/80",
                         border: "border-red-500/20",
                         text: "text-red-200",
                         iconBg: "bg-red-500/20",
                         iconColor: "text-red-500",
                         shadow: "shadow-[0_4px_20px_rgba(220,38,38,0.2)]",
                         Icon: AlertCircle
                    } : notif.type === 'warn' ? {
                        bg: "bg-amber-950/80",
                        border: "border-amber-500/20",
                        text: "text-amber-200",
                        iconBg: "bg-amber-500/20",
                        iconColor: "text-amber-400",
                        shadow: "shadow-[0_4px_20px_rgba(245,158,11,0.18)]",
                        Icon: AlertCircle
                     } : {
                         bg: "bg-emerald-950/80",
                         border: "border-emerald-500/20",
                         text: "text-emerald-200",
                         iconBg: "bg-emerald-500/20",
                         iconColor: "text-emerald-500",
                         shadow: "shadow-[0_4px_20px_rgba(16,185,129,0.2)]",
                         Icon: CheckCircle
                     };

                    return (
                        <motion.div
                            key={notif.id}
                            layout 
                            initial={{ opacity: 0, y: -20, scale: 0.8 }}
                            animate={{ 
                                opacity: 1, 
                                y: index * 42, 
                                scale: 1, 
                                zIndex: 100 - index 
                            }}
                            exit={{ opacity: 0, scale: 0.9, transition: { duration: 0.2 } }}
                            transition={{ type: "spring", stiffness: 500, damping: 30 }}
                            className="absolute top-0 origin-top" 
                        >
                            <div className={`${styleConfig.bg} backdrop-blur-md border ${styleConfig.border} ${styleConfig.text} pl-1 pr-3 py-1 rounded-full ${styleConfig.shadow} flex items-center gap-2 whitespace-nowrap`}>
                                <div className={`w-6 h-6 rounded-full ${styleConfig.iconBg} flex items-center justify-center shrink-0`}>
                                    <styleConfig.Icon size={14} className={styleConfig.iconColor} />
                                </div>
                                <span className="text-[10px] font-bold tracking-widest uppercase opacity-90">{notif.msg}</span>
                            </div>
                        </motion.div>
                    );
                })}
            </AnimatePresence>
         </div>

         {/* 右键菜单（替换 WebView 默认菜单） */}
         <AnimatePresence>
           {contextMenu && (
             <motion.div
               key="context-menu"
               initial={{ opacity: 0, scale: 0.98, y: 4 }}
               animate={{ opacity: 1, scale: 1, y: 0 }}
               exit={{ opacity: 0, scale: 0.98, y: 4 }}
               transition={{ duration: 0.12, ease: 'easeOut' }}
               data-no-drag
               onMouseDown={(e) => e.stopPropagation()}
               onContextMenu={(e) => {
                 e.preventDefault();
                 e.stopPropagation();
               }}
               style={{
                 left: contextMenu.x,
                 top: contextMenu.y,
                 boxShadow: "inset 0 0 24px rgba(0,0,0,0.45), 0 10px 24px rgba(0,0,0,0.35)",
               }}
               className="absolute z-[500] w-56 rounded-xl border border-white/10 bg-zinc-900/65 backdrop-blur-md p-1 font-sans"
             >
               <button
                 data-no-drag
                 className="w-full flex items-center gap-2 px-3 py-2 rounded-lg hover:bg-white/5 text-[12px] font-medium tracking-[0.04em] text-zinc-200"
                 onClick={() => {
                   if (sensitivity !== 1.0) {
                     setSensitivity(1.0);
                   } else {
                     triggerResetPulse();
                   }
                   closeContextMenu();
                 }}
               >
                 <RefreshCw size={14} className="text-amber-400" />
                 <span className="flex-1 text-left">重置为 1.00</span>
                 <span className="ml-auto text-[10px] text-zinc-500 tracking-[0.1em]">R</span>
               </button>

               <button
                 data-no-drag
                 className="w-full flex items-center gap-2 px-3 py-2 rounded-lg hover:bg-white/5 text-[12px] font-medium tracking-[0.04em] text-zinc-200"
                 onClick={() => {
                   window.location.reload();
                   closeContextMenu();
                 }}
               >
                 <RefreshCw size={14} className="text-zinc-400" />
                 <span className="flex-1 text-left">刷新</span>
                 <span className="ml-auto text-[10px] text-zinc-500 tracking-[0.1em]">Ctrl+R</span>
               </button>

               <div className="my-1 h-px bg-white/10" />

               <button
                 data-no-drag
                 disabled={isProcessing}
                 className={`w-full flex items-center gap-2 px-3 py-2 rounded-lg text-[12px] font-medium tracking-[0.04em]
                   ${isProcessing ? 'opacity-50 cursor-not-allowed' : 'hover:bg-white/5'}
                   text-zinc-200
                 `}
                 onClick={() => {
                   handleMouseToggle();
                   closeContextMenu();
                 }}
                >
                  <Mouse size={14} className={isMouseActive ? 'text-blue-400' : 'text-zinc-400'} />
                  <span className="flex-1 text-left">鼠标接管</span>
                  <span className={`text-zinc-500 ${isMouseActive ? 'text-blue-400' : ''}`}>{isMouseActive ? '开' : '关'}</span>
                </button>

               <button
                 data-no-drag
                 disabled={!isMouseActive}
                 className={`w-full flex items-center gap-2 px-3 py-2 rounded-lg text-[12px] font-medium tracking-[0.04em]
                   ${isMouseActive ? 'hover:bg-white/5' : 'opacity-50 cursor-not-allowed'}
                   text-zinc-200
                 `}
                 onClick={() => {
                   requestCrosshairToggle();
                   closeContextMenu();
                 }}
                >
                  <Crosshair
                    size={14}
                    className={isFiring ? 'text-emerald-400' : isCrosshairActive ? 'text-amber-400' : 'text-zinc-400'}
                  />
                  <span className="flex-1 text-left">自动按键</span>
                  <span className={`text-zinc-500 ${isCrosshairActive ? 'text-amber-400' : ''}`}>{isCrosshairActive ? '开' : '关'}</span>
                </button>

               <button
                 data-no-drag
                 className="w-full flex items-center gap-2 px-3 py-2 rounded-lg hover:bg-white/5 text-[12px] font-medium tracking-[0.04em] text-zinc-200"
                 onClick={requestUnbindMouse}
               >
                 <Mouse size={14} className="text-red-400/80" />
                 <span className="flex-1 text-left">解绑鼠标</span>
                 <span className="ml-auto text-[10px] text-red-300/80 tracking-[0.1em]">Caps x2</span>
               </button>

               <div className="my-1 h-px bg-white/10" />

               <button
                 data-no-drag
                 className="w-full flex items-center gap-2 px-3 py-2 rounded-lg hover:bg-white/5 text-[12px] font-medium tracking-[0.04em] text-zinc-200"
                 onClick={() => {
                   tauriMinimize().catch(() => {});
                   closeContextMenu();
                 }}
               >
                 <Minus size={14} className="text-zinc-400" />
                 <span className="flex-1 text-left">最小化</span>
                 <span className="ml-auto text-[10px] text-zinc-500 tracking-[0.1em]">Ctrl+M</span>
               </button>

               <button
                 data-no-drag
                 className="w-full flex items-center gap-2 px-3 py-2 rounded-lg hover:bg-red-500/10 text-[12px] font-medium tracking-[0.04em] text-zinc-200"
                 onClick={requestClose}
               >
                 <X size={14} className="text-red-400" />
                 <span className="flex-1 text-left">退出</span>
                 <span className="ml-auto text-[10px] text-red-300/80 tracking-[0.1em]">Ctrl+Q</span>
               </button>
             </motion.div>
           )}
         </AnimatePresence>

         {/* --- [后端注意] 右上角关闭按钮 --- */}
         {/* 对应 CLI 逻辑：'q' 命令，关闭程序。 */}
         {/* 重要：需要等待后端恢复灵敏度完成后再关闭窗口（避免关闭后鼠标还“卡”一会）。 */}
        <div 
          className="absolute top-0 right-0 z-[101] flex p-2 gap-1"
          style={{ WebkitAppRegion: 'no-drag' }}
        >
          <button
            className="group p-1.5 rounded transition-colors hover:bg-[color:var(--title-hover-bg)]"
            onClick={() => tauriMinimize().catch(() => {})}
            style={titleControlHoverStyle}
          >
            <Minus
              size={14}
              className="text-zinc-600 group-hover:text-zinc-200 transition-colors"
              style={titleControlIconStyle}
            />
          </button>
          
            <button 
                className={`group p-1.5 rounded transition-colors flex items-center justify-center
                  ${isClosing ? 'bg-red-500/35 text-red-500' : 'hover:bg-red-500/20'}
                `}
                onClick={requestClose}
            >
              {isClosing ? (
              <span className="flex items-center justify-center w-[14px] h-[14px]">
                <Loader2 className="animate-spin w-full h-full block" style={titleControlIconStyle} />
              </span>
             ) : (
               <X
                 size={14}
                 className="text-zinc-600 group-hover:text-red-500 transition-colors"
                 style={titleControlIconStyle}
               />
             )}
           </button>
         </div>
        
        {/* bento-grid 风格：不使用“发光边框 / 粒子”，整体更克制 */}

        {/* 主内容区域 */}
        <div className="relative z-10 w-full h-full flex items-center justify-center p-4">
          <AnimatePresence mode="wait">
            
            {/* --- Phase 0: INIT (避免已注册时闪过注册界面) --- */}
            {phase === 'INIT' && (
              <motion.div
                key="init"
                initial={{ opacity: 0, scale: 0.95, filter: "blur(8px)" }}
                animate={{ opacity: 1, scale: 1, filter: "blur(0px)" }}
                exit={{ opacity: 0, scale: 0.95, filter: "blur(8px)" }}
                transition={{ duration: 0.25, ease: "easeOut" }}
                className="relative flex flex-col items-center"
              >
                <Loader2 size={44} className="text-zinc-500 animate-spin" />
                <div className="mt-8 text-zinc-600 text-xs tracking-[0.12em] font-bold">
                  正在初始化...
                </div>
              </motion.div>
            )}

            {/* --- Phase 1: 扫描/注册界面 --- */}
            {/* 逻辑：等待鼠标移动数据累积到 100% */}
            {phase === 'SCAN' && (
              <motion.div 
                key="scan"
                initial={{ scale: 0.8, opacity: 0, filter: "blur(10px)" }} // 新增入场状态
                animate={{ scale: 1, opacity: 1, filter: "blur(0px)" }}    // 新增目标状态
                exit={{ scale: 0.8, opacity: 0, filter: "blur(10px)" }}
                transition={{ duration: 0.5, ease: "easeOut" }}            // 新增过渡配置
                className="relative flex flex-col items-center"
              >
                <div className="relative w-48 h-48 flex items-center justify-center">
                   <div className="absolute inset-0 border-2 border-zinc-800 rounded-full" />
                   {/* 进度环 */}
                   <svg className="absolute inset-0 w-full h-full -rotate-90" viewBox="0 0 256 256">
                     <motion.circle 
                       cx="128" cy="128" r="126" 
                       fill="none" 
                       strokeWidth="4"
                       strokeDasharray="792"
                       initial={{ strokeDashoffset: 792, stroke: "#71717a" }}
                       animate={{ 
                         strokeDashoffset: 792 - (792 * shakeProgress / 100),
                         stroke: shakeProgress > 50 ? "#3b82f6" : "#71717a"
                       }}
                       transition={{ type: "tween", duration: 0.03, ease: "linear" }}
                     />
                   </svg>
                   <motion.div 
                     animate={{ scale: [1, 1.1, 1] }}
                     transition={{ duration: 2, repeat: Infinity }}
                   >
                     <Mouse size={48} className="text-zinc-500" />
                   </motion.div>
                </div>
                <motion.div 
                  initial={{ opacity: 0 }} 
                  animate={{ opacity: 1 }}
                  className="mt-8 text-zinc-600 text-xs tracking-[0.12em] font-bold"
                >
                  {isTauri ? (scanInputReady ? '请摇动鼠标' : '正在初始化...') : '正在初始化...'}
                </motion.div>
              </motion.div>
            )}

            {/* --- Phase 2: 主控制台 (DASHBOARD) --- */}
            {phase === 'DASHBOARD' && (
              <motion.div 
                key="dashboard"
                initial={{ opacity: 0, scale: 1.2 }}
                animate={{ opacity: 1, scale: 1 }}
                exit={{ opacity: 0, scale: 0.9, filter: "blur(10px)", transition: { duration: 0.3 } }} // 新增退场动画
                className="w-full h-full flex flex-col items-center"
              >
                {/* 顶部数字区域 */}
                <div className="flex-1 w-full flex flex-col items-center justify-center">
                  <div className="group relative translate-y-3">
                    <motion.div 
                      key={sensitivity}
                      initial={{ y: 15, opacity: 0.5, filter: 'blur(2px)' }} 
                      animate={{ y: 0, opacity: 1, filter: 'blur(0px)' }}
                      className={`relative text-7xl font-black tracking-tighter tabular-nums flex items-baseline 
                          ${syncActive ? 'text-amber-500' : 'text-white'}
                      `}
                    >
                      {sensitivity.toFixed(2)}
                      
                      {/* 同步指示点：提示后端正在写入数据 */}
                      {syncActive && (
                        <div className="absolute -right-3 top-1 w-1.5 h-1.5 bg-amber-500 rounded-full animate-ping" />
                      )}

                      <button
                        data-no-drag
                        onClick={() => {
                          if (sensitivity !== 1.0) {
                            setSensitivity(1.0);
                            return;
                          }
                          triggerResetPulse();
                        }}
                        className="absolute left-full top-1/2 -translate-y-1/2 ml-3 opacity-0 group-hover:opacity-100 transition-opacity p-1.5 hover:bg-white/5 rounded text-zinc-500 hover:text-white"
                        title="重置为 1.00"
                      >
                        <RefreshCw size={12} />
                      </button>

                      {/* --- [后端注意] 轮播报错演示 --- */}
                      {/* 仅在灵敏度为 1.0 时出现，用于演示报错 UI。 */}
                      {/* 后端请根据实际情况，将此处的轮播逻辑替换为真实的错误监听。 */}
                    </motion.div>
                  </div>
                </div>

                {/* --- [后端注意] 灵敏度拉条 --- */}
                {/* 对应 CLI：'l' 命令设置灵敏度。 */}
                {/* 交互说明：拖动时触发 'SYNCING' 状态，模拟后端 1秒 的写入耗时。 */}
                {/* 只有在鼠标开关为 ON 时，拉条才生效 (变色反馈)。 */}
                <div className="shrink-0 w-full relative z-20">
                  <div className="relative h-12 flex items-center justify-center w-10/12 mx-auto">
                    <input 
                      type="range" 
                      min="0" max="100" step="any"
                      value={sliderPercent}
                      onChange={(e) => {
                        const rawVal = toSplitScale(parseFloat(e.target.value));
                        const roundedVal = Math.round(rawVal * 100) / 100;
                        if (roundedVal !== sensitivity) {
                            setSensitivity(roundedVal);
                        }
                      }}
                      className="absolute inset-0 z-20 w-full opacity-0 cursor-ew-resize"
                    />
                     
                    <div className="w-full h-1 bg-white/10 rounded-full overflow-hidden">
                      <motion.div 
                        className={`h-full transition-colors duration-500 ${syncActive ? 'bg-amber-500' : 'bg-white/80'}`}
                        style={{ width: `${sliderPercent}%` }}
                      />
                    </div>
 
                    <div className="absolute top-1/2 -translate-y-1/2 w-0.5 h-3 bg-white/15 left-1/2" />
                    <motion.div 
                      className={`absolute h-5 w-1 shadow-[0_1px_3px_rgba(0,0,0,0.55)] pointer-events-none transition-colors duration-500 ${syncActive ? 'bg-amber-500' : 'bg-white/90'}`}
                      style={{ left: `${sliderPercent}%` }}
                    />

                    <div className="absolute top-full left-0 w-full flex justify-center mt-2 pointer-events-none">
                        <AnimatePresence>
                          {isSyncing && (
                            <motion.span 
                              initial={{ opacity: 0, y: -5 }} 
                              animate={{ opacity: 1, y: 0 }} 
                              exit={{ opacity: 0, y: -5 }}
                              className="text-[10px] font-mono text-amber-500 tracking-widest scale-90"
                            >
                              同步中...
                            </motion.span>
                         )}
                        </AnimatePresence>
                    </div>
                  </div>
                </div>

                {/* 底部按钮区域 */}
                <div className="flex-1 w-full flex flex-col items-center justify-center">
                  <div className="flex items-center gap-16 text-zinc-600 -translate-y-2">
                    {/* --- [后端注意] 左侧：鼠标开关按钮 --- */}
                    {/* 功能：控制整个辅助功能的总开关。 */}
                    {/* 灭 (OFF): 对应关闭自动按左键功能，恢复灵敏度。 */}
                    {/* 亮 (ON): 恢复上次记忆的瞄准镜状态。 */}
                    <div 
                      data-no-drag
                      className={`group relative flex flex-col items-center gap-2 transition-all duration-300 
                        ${isProcessing ? 'cursor-wait' : 'cursor-pointer'}
                        ${mouseStatus === 'OFF' && !shutdownPulse ? 'opacity-50' : 'opacity-100'}
                        ${isMouseActive ? 'scale-110' : ''}
                      `}
                      onMouseDown={(e) => e.nativeEvent.stopImmediatePropagation()} 
                      onClick={handleMouseToggle}
                    >
                      <div className={`relative z-10 p-4 rounded-full border backdrop-blur-md transition-all duration-500
                        ${(isMouseActive || mouseStatus === 'BOOTING')
                          ? 'bg-white/10 border-white/35' 
                          : mouseStatus === 'SHUTTING_DOWN' ? 'bg-white/5 border-white/10' : 'bg-white/5 border-white/10 hover:bg-white/10'}
                      `}>
                        <Mouse size={24} className={`transition-all duration-300 
                          ${(shutdownPulse || mouseStatus === 'SHUTTING_DOWN')
                            ? 'text-red-400/50'
                            : (isMouseActive || mouseStatus === 'BOOTING') ? 'text-blue-400' : 'text-white/20'}
                          ${mouseStatus === 'BOOTING' ? 'animate-pulse' : ''}
                        `} />
                      </div>
                    </div>

                    {/* --- [后端注意] 右侧：瞄准/开火按钮 --- */}
                    {/* 对应 CLI：'p' 键（自动按左键功能）。 */}
                    {/* 状态说明： */}
                    {/* 琥珀色: 功能开启，待机状态。 */}
                    <div 
                      data-no-drag
                      className={`group relative flex flex-col items-center gap-2 transition-all duration-300 
                        ${isCrosshairActive ? 'scale-110' : 'opacity-50'}
                        ${isMouseActive ? 'cursor-pointer' : 'cursor-not-allowed opacity-20'} 
                      `}
                      onMouseDown={(e) => e.nativeEvent.stopImmediatePropagation()}
                      onClick={requestCrosshairToggle}
                    >
                      <div className={`relative z-10 p-4 rounded-full border backdrop-blur-md transition-all duration-500
                        ${(isFiring || isCrosshairActive) 
                          ? 'bg-white/10 border-white/40' 
                          : 'bg-white/5 border-white/10 hover:bg-white/10'}
                      `}>
                        <Crosshair size={24} className={`transition-colors duration-300 
                            ${isFiring ? 'text-emerald-400' : isCrosshairActive ? 'text-amber-400' : 'text-white/20'}
                          `} 
                        />
                      </div>
                    </div>
                  </div>
                </div>
              </motion.div>
            )}

          </AnimatePresence>
        </div>
      </div>
    </div>
  );
}
