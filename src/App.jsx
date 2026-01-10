import React, { useState, useEffect, useRef } from 'react';
import { Mouse, Crosshair, RefreshCw, Minus, X, Loader2, AlertCircle, CheckCircle } from 'lucide-react';
import { motion, AnimatePresence } from 'framer-motion';

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
const AMBIENT_RANGE_MIN = 0.05;
const AMBIENT_RANGE_MAX = 0.2;
const AMBIENT_LERP = 0.06;
const GLASS_TOP_DARK = 0.20;
const GLASS_TOP_LIGHT = 0.30;
const GLASS_BOTTOM_DARK = 0.40;
const GLASS_BOTTOM_LIGHT = 0.90;
const GLASS_VIGNETTE_ALPHA = 0.25;

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
  const ambientTarget = useRef(1.0);
  const ambientSmooth = useRef(1.0);
  const ambientRaf = useRef(0);
  
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

  const applyAmbientStyle = (value) => {
    const range = AMBIENT_RANGE_MAX - AMBIENT_RANGE_MIN;
    const t = range <= 0 ? 1 : Math.max(0, Math.min(1, (value - AMBIENT_RANGE_MIN) / range));
    const top = GLASS_TOP_DARK + (GLASS_TOP_LIGHT - GLASS_TOP_DARK) * t;
    const bottom = GLASS_BOTTOM_DARK + (GLASS_BOTTOM_LIGHT - GLASS_BOTTOM_DARK) * t;
    const el = containerRef.current;
    if (!el) return;
    el.style.setProperty('--glass-top-alpha', top.toFixed(3));
    el.style.setProperty('--glass-bottom-alpha', bottom.toFixed(3));
    el.style.setProperty('--glass-vignette-alpha', GLASS_VIGNETTE_ALPHA.toFixed(3));
  };

  const stepAmbient = () => {
    const target = ambientTarget.current;
    const current = ambientSmooth.current;
    const next = current + (target - current) * AMBIENT_LERP;
    ambientSmooth.current = next;
    applyAmbientStyle(next);
    if (Math.abs(target - next) > 0.0005) {
      ambientRaf.current = requestAnimationFrame(stepAmbient);
      return;
    }
    ambientSmooth.current = target;
    applyAmbientStyle(target);
    ambientRaf.current = 0;
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

          if (kind === 'NOTIFY') return;
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
    applyAmbientStyle(ambientSmooth.current);
  }, []);

  useEffect(() => {
    if (!isTauri) return;

    let unlisten = null;

    (async () => {
      try {
        unlisten = await tauriListen('ambient-brightness', (event) => {
          const raw = event?.payload;
          const value = typeof raw === 'number' ? raw : Number.parseFloat(raw);
          if (!Number.isFinite(value)) return;
          ambientTarget.current = Math.max(0, Math.min(1, value));
          if (!ambientRaf.current) {
            ambientRaf.current = requestAnimationFrame(stepAmbient);
          }
        });
      } catch {}
    })();

    return () => {
      if (unlisten) unlisten();
      if (ambientRaf.current) {
        cancelAnimationFrame(ambientRaf.current);
        ambientRaf.current = 0;
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
             '--glass-top-alpha': GLASS_TOP_LIGHT.toFixed(2),
             '--glass-bottom-alpha': GLASS_BOTTOM_LIGHT.toFixed(2),
             '--glass-vignette-alpha': GLASS_VIGNETTE_ALPHA.toFixed(2),
           }}
              className={`relative overflow-hidden bg-zinc-950/10 text-zinc-200 font-mono select-none transition-all duration-300 shadow-2xl rounded-xl border border-white/10
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
              className="absolute inset-0 rounded-xl pointer-events-none"
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
              data-no-drag
              className={`absolute inset-0 z-[300] bg-zinc-950/80 flex flex-col items-center justify-center cursor-pointer ${FULLSCREEN_CONFIG[fullScreenStatus].colorClass}`}
              onClick={() => setFullScreenStatus(null)}
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
                 </div>
              </motion.div>
            </motion.div>
          )}
        </AnimatePresence>

         {/* 顶部通知列表 */}
         <div className="absolute top-8 left-0 w-full flex justify-center z-[400] pointer-events-none">
            <AnimatePresence mode='popLayout'>
                {notifications.map((notif, index) => {
                    const isError = notif.type === 'error';
                    const styleConfig = isError ? {
                        bg: "bg-red-950/80",
                        border: "border-red-500/20",
                        text: "text-red-200",
                        iconBg: "bg-red-500/20",
                        iconColor: "text-red-500",
                        shadow: "shadow-[0_4px_20px_rgba(220,38,38,0.2)]",
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
            className="group p-1.5 rounded hover:bg-zinc-800 transition-colors"
            onClick={() => tauriMinimize().catch(() => {})}
          >
            <Minus size={14} className="text-zinc-600 group-hover:text-zinc-200 transition-colors" />
          </button>
          
            <button 
                className={`group p-1.5 rounded transition-colors flex items-center justify-center
                  ${isClosing ? 'bg-red-500/20 text-red-500' : 'hover:bg-red-500/10'}
                `}
                onClick={requestClose}
            >
              {isClosing ? (
              <span className="flex items-center justify-center w-[14px] h-[14px]">
                <Loader2 className="animate-spin w-full h-full block" />
              </span>
             ) : (
               <X size={14} className="text-zinc-600 group-hover:text-red-500 transition-colors" />
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
