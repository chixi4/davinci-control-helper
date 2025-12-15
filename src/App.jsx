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

async function tauriStartDragging() {
  if (!isTauri) return;
  const { appWindow } = await import('@tauri-apps/api/window');
  return appWindow.startDragging();
}

const WINDOW_WIDTH = 320;
const WINDOW_HEIGHT = 460;

// --- [后端注意] 报错与状态模拟数据 ---
// 这是一个轮播的演示列表，用于展示灵敏度为1.0时的界面状态反馈。
// 后端人员请注意：这里需要替换为实际的硬件/驱动报错信息接口。
// 格式建议保持 "类型:内容" 以便前端解析颜色。
const DEBUG_SEQUENCE = [
  "ERR:DRIVER NOT FOUND",        
  "OK:CONFIG SAVED",            
  "ERR:WRITE TIMEOUT",          
  "OK:FIRMWARE UPDATED",        
  "ERR:INVALID PARAMETER",       
  "FS:LOST",          
  "FS:CONNECTING",    
  "FS:OFFLINE"        
];

// --- 全屏状态UI配置 ---
// 对应后端连接丢失、正在连接、服务下线等重大状态的视觉反馈
const FULLSCREEN_CONFIG = {
  'LOST': {
    title: "CONNECTION LOST",
    subtitle: "ATTEMPTING TO RECONNECT...",
    colorClass: "text-red-500",
    bgClass: "bg-red-500",
    borderClass: "border-red-500",
    shadowColor: "rgba(220,38,38,0.8)",
    pulse: true
  },
  'CONNECTING': {
    title: "ESTABLISHING UPLINK",
    subtitle: "HANDSHAKE IN PROGRESS...",
    colorClass: "text-amber-500",
    bgClass: "bg-amber-500",
    borderClass: "border-amber-500",
    shadowColor: "rgba(245,158,11,0.8)",
    pulse: true
  },
  'OFFLINE': {
    title: "BACKEND OFFLINE",
    subtitle: "SERVICE UNREACHABLE",
    colorClass: "text-zinc-300",
    bgClass: "bg-zinc-300",
    borderClass: "border-zinc-300",
    shadowColor: "rgba(255,255,255,0.25)",
    pulse: false 
  }
};

const generateParticles = (count) => {
  return Array.from({ length: count }).map((_, i) => ({
    id: i,
    x: Math.random() * WINDOW_WIDTH,
    y: Math.random() * WINDOW_HEIGHT,
    size: Math.random() * 2 + 1,
    speed: Math.random() * 0.5 + 0.2,
  }));
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

  // 自动开火状态：当 isCrosshairActive 为 true 且触发逻辑时，变为 true (变绿)
  const [isFiring, setIsFiring] = useState(false);
  
  const [mousePos, setMousePos] = useState({ x: 0, y: 0 });
  const [shakeProgress, setShakeProgress] = useState(0);
  
  const fireTimer = useRef(null);
  const lastMousePos = useRef(null);
  const debounceTimer = useRef(null);
  const syncTimer = useRef(null);
  const isFirstRender = useRef(true);
  const pendingSensitivity = useRef(null);
  const skipNextSensitivitySend = useRef(false);
  const pendingPower = useRef(null);
  const pendingExit = useRef(null);
  const isCrosshairActiveRef = useRef(false);
  const mouseStatusRef = useRef('OFF');
  const phaseRef = useRef(phase);
  
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
            addNotification('success', 'MOUSE REGISTERED');
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
      if (phase === 'DASHBOARD' && e.key.toLowerCase() === 'r') {
        setSensitivity(1.0);
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
      if (e.key === 'CapsLock') {
        const now = Date.now();
        if (now - lastCapsLockTime.current < 300) {
          if (phase === 'DASHBOARD') {
            // 立即重置所有功能开关
            isCrosshairActiveRef.current = false;
            mouseStatusRef.current = 'OFF';
            setIsCrosshairActive(false);
            setIsFiring(false);
            setFullScreenStatus(null);
            setNotifications([]);
            setSensitivity(1.0);
            pendingSensitivity.current = null;
            setIsSyncing(false);

            // 立即触发界面切换，动画与逻辑并行
            setPhase('SCAN');
            setShakeProgress(0);
            setMouseStatus('OFF');
            tauriInvoke('backend_full_reset').catch(() => {});
          }
        }
        lastCapsLockTime.current = now;
      }
    };
    window.addEventListener('keydown', handleKey);
    return () => window.removeEventListener('keydown', handleKey);
  }, [phase]); 

  const particles = useRef(generateParticles(20));
  const sliderPercent = fromSplitScale(sensitivity);

  // --- [后端注意] 左下角鼠标按钮逻辑 ---
  // 开关逻辑：
  // 关 (OFF): 
  //   - 对应 CLI：关闭自动按左键功能。
  //   - 对应 CLI：恢复鼠标灵敏度到 1.0 (或默认值)。
  // 开 (ON):
  //   - 记忆功能：如果上次关机前右边的瞄准镜是开着的，这次开机也要自动打开。
  const handleMouseToggle = () => {
    if (mouseStatus === 'OFF') {
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
      className={`flex items-center justify-center w-full h-screen ${
        isTauri ? 'bg-transparent' : 'bg-gray-900/50'
      }`}
    >
      
      {/* --- [前端交互] 窗口容器 ---
         注意：整个窗口除了特定的按钮和拉条区域外，
         都应该支持拖拽移动 (通过 CSS WebkitAppRegion: 'drag' 实现)。
       */}
       <div 
         style={{ width: WINDOW_WIDTH, height: WINDOW_HEIGHT }}
          className={`relative overflow-hidden bg-zinc-950 text-zinc-200 font-mono select-none transition-all duration-300 shadow-2xl rounded-xl border border-zinc-800
            ${isFiring ? 'cursor-crosshair' : 'cursor-default'}
          `}
          onMouseDown={(e) => {
            if (!isTauri) return;
            if (e.button !== 0) return;

            const target = e.target instanceof Element ? e.target : null;
            if (target) {
              if (target.closest('button, input, textarea, select, option, a, [data-no-drag]')) {
                return;
              }
            }

            tauriStartDragging().catch(() => {});
          }}
        >

        {/* Entire window is draggable; interactive elements opt-out via `button/input/...` or `data-no-drag`. */}

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
                        className="text-2xl font-black tracking-[0.2em] drop-shadow-lg text-center px-4"
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
              onClick={() => {
                if (isClosing) return;
                setIsClosing(true);

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
              }}
          >
            {isClosing ? (
              <Loader2 size={14} className="animate-spin" />
            ) : (
              <X size={14} className="text-zinc-600 group-hover:text-red-500 transition-colors" />
            )}
          </button>
        </div>
        
        {/* 状态发光边框 */}
        <div 
          className={`absolute inset-0 pointer-events-none z-50 border-[6px] transition-all duration-300 rounded-xl
            ${phase === 'SCAN' || phase === 'INIT' ? 'border-transparent' : ''}
            ${phase === 'DASHBOARD' && !isCrosshairActive ? 'border-zinc-800/50' : ''}
            ${phase === 'DASHBOARD' && isCrosshairActive && !isFiring ? 'border-amber-500/60 animate-pulse shadow-[inset_0_0_30px_rgba(245,158,11,0.2)]' : ''}
            ${phase === 'DASHBOARD' && isCrosshairActive && isFiring ? 'border-emerald-500 shadow-[inset_0_0_60px_rgba(16,185,129,0.4)] scale-[0.995]' : ''}
            ${(notifications.length > 0 && notifications[0].type === 'error') || fullScreenStatus === 'LOST' ? 'border-red-500/50 shadow-[inset_0_0_30px_rgba(220,38,38,0.2)]' : ''} 
            ${fullScreenStatus === 'CONNECTING' ? 'border-amber-500/50 shadow-[inset_0_0_30px_rgba(245,158,11,0.2)]' : ''}
            ${fullScreenStatus === 'OFFLINE' ? 'border-zinc-500/50' : ''}
            ${(notifications.length > 0 && notifications[0].type === 'success') ? 'border-emerald-500/50 shadow-[inset_0_0_30px_rgba(16,185,129,0.2)]' : ''} 
          `} 
          style={{ animationDuration: '3s' }}
        />

        {/* 背景粒子效果 */}
        <div className="absolute inset-0 z-0 opacity-20">
           <div className="absolute inset-0" 
                style={{ backgroundImage: 'linear-gradient(#333 1px, transparent 1px), linear-gradient(90deg, #333 1px, transparent 1px)', backgroundSize: '30px 30px' }} />
           {particles.current.map(p => (
             <motion.div
               key={p.id}
               className="absolute bg-zinc-500 rounded-full"
               style={{ width: p.size, height: p.size, left: p.x, top: p.y }}
               animate={{ 
                 y: [p.y, p.y - 1000],
                 opacity: [0, 0.5, 0]
               }}
               transition={{ 
                 duration: (10 / p.speed) / sensitivity, 
                 repeat: Infinity,
                 repeatDelay: Math.random() * 2,
                 ease: "linear"
               }}
             />
           ))}
        </div>

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
                <div className="mt-8 text-zinc-600 text-xs tracking-[0.2em] font-bold">
                  INITIALIZING...
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
                  className="mt-8 text-zinc-600 text-xs tracking-[0.2em] font-bold"
                >
                  {isTauri ? (scanInputReady ? 'SHAKE YOUR MOUSE' : 'INITIALIZING...') : 'INITIALIZING...'}
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
                          ${isSyncing ? 'text-amber-500 drop-shadow-[0_0_15px_rgba(245,158,11,0.5)]' : 'text-white'}
                      `}
                    >
                      {sensitivity.toFixed(2)}
                      
                      {/* 同步指示点：提示后端正在写入数据 */}
                      {isSyncing && (
                        <div className="absolute -right-3 top-1 w-1.5 h-1.5 bg-amber-500 rounded-full animate-ping" />
                      )}

                      <button
                        data-no-drag
                        onClick={() => {
                          if (sensitivity !== 1.0) setSensitivity(1.0);
                        }}
                        className="absolute left-full top-1/2 -translate-y-1/2 ml-3 opacity-0 group-hover:opacity-100 transition-opacity p-1.5 hover:bg-zinc-800 rounded text-zinc-500 hover:text-white"
                        title="Reset to 1.00"
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
                    
                    <div className="w-full h-1 bg-zinc-800 rounded-full overflow-hidden">
                      <motion.div 
                        className={`h-full transition-colors duration-500 ${isSyncing ? 'bg-amber-500' : 'bg-white'}`}
                        style={{ width: `${sliderPercent}%` }}
                      />
                    </div>

                    <div className="absolute top-1/2 -translate-y-1/2 w-0.5 h-3 bg-zinc-600 left-1/2" />
                    <motion.div 
                      className={`absolute h-5 w-1 shadow-[0_0_10px_white] pointer-events-none transition-colors duration-500 ${isSyncing ? 'bg-amber-500' : 'bg-white'}`}
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
                              SYNCING...
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
                          ${mouseStatus === 'OFF' ? 'opacity-50' : 'opacity-100'}
                          ${isMouseActive ? 'scale-110' : ''}
                        `}
                        onMouseDown={(e) => e.nativeEvent.stopImmediatePropagation()} 
                        onClick={handleMouseToggle}
                      >
                          <AnimatePresence>
                            {isMouseActive && (
                              <motion.div
                                initial={{ opacity: 0, scale: 0.5, x: "-50%", y: "-50%" }}
                                animate={{ opacity: 1, scale: 1, x: "-50%", y: "-50%" }}
                                exit={{ opacity: 0, scale: 0.5, x: "-50%", y: "-50%" }}
                                className="absolute left-1/2 top-1/2 w-24 h-24 pointer-events-none z-0"
                                style={{ mixBlendMode: 'plus-lighter' }} 
                              >
                                <div className="w-full h-full bg-[radial-gradient(circle,rgba(255,255,255,0.8)_0%,rgba(59,130,246,0.6)_40%,rgba(59,130,246,0)_70%)] blur-xl" />
                              </motion.div>
                            )}
                          </AnimatePresence>

                        <div className={`relative z-10 p-4 rounded-full border backdrop-blur-md transition-all duration-500
                          ${isMouseActive 
                            ? 'bg-white/10 border-white/80' 
                            : mouseStatus === 'SHUTTING_DOWN' ? 'bg-red-500/10 border-red-500/50' : 'bg-white/5 border-white/10 hover:bg-white/10'}
                        `}>
                          <Mouse size={24} className={`transition-all duration-300 
                            ${isMouseActive ? 'text-white drop-shadow-[0_0_2px_rgba(255,255,255,1)]' : 'text-white/20'}
                            ${mouseStatus === 'BOOTING' ? 'animate-pulse text-blue-400' : ''}
                            ${mouseStatus === 'SHUTTING_DOWN' ? 'text-red-400 opacity-50' : ''}
                          `} />
                        </div>
                      </div>

                      {/* --- [后端注意] 右侧：瞄准/开火按钮 --- */}
                      {/* 对应 CLI：'p' 键（自动按左键功能）。 */}
                      {/* 状态说明： */}
                      {/* 琥珀色: 功能开启，待机状态。 */}
                      {/* 绿色 (isFiring): 触发自动按左键。 */}
                      <div 
                        data-no-drag
                        className={`group relative flex flex-col items-center gap-2 transition-all duration-300 
                          ${isCrosshairActive ? 'scale-110' : 'opacity-50'}
                          ${isMouseActive ? 'cursor-pointer' : 'cursor-not-allowed opacity-20'} 
                        `}
                        onMouseDown={(e) => e.nativeEvent.stopImmediatePropagation()}
                        onClick={() => {
                           if (!isMouseActive) return;
                           const next = !isCrosshairActive;
                           crosshairMemory.current = next;
                           tauriInvoke('ui_save_state', { state: { crosshairMemory: next } }).catch(() => {});
                           isCrosshairActiveRef.current = next;
                           setIsCrosshairActive(next);
                           setIsFiring(false);
                           tauriInvoke('backend_set_feature', { enabled: next }).catch(() => {});
                        }}
                      >
                          <AnimatePresence>
                            {(isFiring || isCrosshairActive) && (
                              <motion.div
                                initial={{ opacity: 0, scale: 0.5, x: "-50%", y: "-50%" }}
                                animate={{ opacity: 1, scale: 1, x: "-50%", y: "-50%" }}
                                exit={{ opacity: 0, scale: 0.5, x: "-50%", y: "-50%" }}
                                className="absolute left-1/2 top-1/2 w-24 h-24 pointer-events-none z-0"
                                style={{ mixBlendMode: 'plus-lighter' }}
                              >
                                <div
                                  className={`w-full h-full blur-xl transition-all duration-500 ${
                                    isFiring
                                      ? 'bg-[radial-gradient(circle,rgba(255,255,255,0.85)_0%,rgba(16,185,129,0.7)_38%,rgba(16,185,129,0)_72%)]'
                                      : 'bg-[radial-gradient(circle,rgba(255,255,255,0.85)_0%,rgba(245,158,11,0.7)_38%,rgba(245,158,11,0)_72%)]'
                                  }`}
                                />
                              </motion.div>
                            )}
                          </AnimatePresence>
                          
                          {/* 按钮主体背景：玻璃拟态 */}
                          <div className={`relative z-10 p-4 rounded-full border backdrop-blur-md transition-all duration-500
                            ${(isFiring || isCrosshairActive) 
                              ? 'bg-white/10 border-white/80' 
                              : 'bg-white/5 border-white/10 hover:bg-white/10'}
                          `}>
                            <Crosshair size={24} className={`transition-colors duration-300 
                                ${(isFiring || isCrosshairActive) ? 'text-white drop-shadow-[0_0_2px_rgba(255,255,255,1)]' : 'text-white/20'}
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
