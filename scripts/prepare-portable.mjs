import fs from 'node:fs/promises';
import path from 'node:path';

const repoRoot = process.cwd();
const targetDir = path.join(repoRoot, 'src-tauri', 'target', 'release');
const targetBackendDir = path.join(targetDir, 'backend');

async function ensureDir(dir) {
  await fs.mkdir(dir, { recursive: true });
}

async function copyIfExists(srcPath, destPath) {
  try {
    await fs.copyFile(srcPath, destPath);
    return true;
  } catch (e) {
    if (e && (e.code === 'ENOENT' || e.code === 'ENOTDIR')) return false;
    throw e;
  }
}

async function exists(p) {
  try {
    await fs.access(p);
    return true;
  } catch {
    return false;
  }
}

async function tryReadJson(p) {
  try {
    const raw = await fs.readFile(p, 'utf8');
    return JSON.parse(raw);
  } catch {
    return null;
  }
}

async function main() {
  await ensureDir(targetDir);
  await ensureDir(targetBackendDir);

  const requiredRuntimeFiles = [
    'mouse_monitor.exe',
    'writer.exe',
    'wrapper.dll',
  ];

  const optionalRuntimeFiles = [
    'Newtonsoft.Json.dll',
  ];

  const missing = [];

  for (const file of requiredRuntimeFiles) {
    const srcPath = path.join(repoRoot, file);
    const destPath = path.join(targetBackendDir, file);
    const ok = await copyIfExists(srcPath, destPath);
    if (!ok) missing.push(file);
  }

  for (const file of optionalRuntimeFiles) {
    const srcPath = path.join(repoRoot, file);
    const destPath = path.join(targetBackendDir, file);
    await copyIfExists(srcPath, destPath);
  }

  const settingsDest = path.join(targetBackendDir, 'settings.json');
  const srcSettings = (await exists(path.join(repoRoot, 'settings.json')))
    ? path.join(repoRoot, 'settings.json')
    : path.join(repoRoot, 'settings.example.json');

  const ok = await copyIfExists(srcSettings, settingsDest);
  if (!ok) missing.push('settings.example.json');

  if (missing.length) {
    // Fail the build so users don't end up with a GUI that can't function.
    // eslint-disable-next-line no-console
    console.error(`[portable] Missing required files in repo root: ${missing.join(', ')}`);
    process.exitCode = 1;
    return;
  }

  // Also produce a clean portable folder for "double click and play".
  const tauriConf = await tryReadJson(path.join(repoRoot, 'src-tauri', 'tauri.conf.json'));
  const productExeName = tauriConf?.package?.productName
    ? `${tauriConf.package.productName}.exe`
    : null;

  const productExePath = productExeName ? path.join(targetDir, productExeName) : null;

  const exeEntries = await fs.readdir(targetDir);
  const exeCandidates = [];
  for (const name of exeEntries) {
    if (!name.toLowerCase().endsWith('.exe')) continue;
    if (name.toLowerCase() === 'mouse_monitor.exe') continue;
    if (name.toLowerCase() === 'writer.exe') continue;
    const full = path.join(targetDir, name);
    const stat = await fs.stat(full);
    if (stat.isFile()) exeCandidates.push({ name, full, size: stat.size });
  }

  exeCandidates.sort((a, b) => b.size - a.size);
  const appExe = (productExePath && (await exists(productExePath)))
    ? productExePath
    : exeCandidates[0]?.full;
  const appExeName = productExeName || exeCandidates[0]?.name;

  const portableRoot = path.join(repoRoot, 'dist-portable');
  const portableDir = path.join(portableRoot, 'RawAccel Monitor');
  const portableBackendDir = path.join(portableDir, 'backend');
  const portableDriverDir = path.join(portableDir, 'driver');
  await ensureDir(portableDir);
  await ensureDir(portableBackendDir);
  await ensureDir(portableDriverDir);

  // Cleanup legacy layout (older builds placed rawaccel.sys at the root).
  await fs.rm(path.join(portableDir, 'rawaccel.sys'), { force: true }).catch(() => {});

  const portableFiles = [
    { src: appExe, dest: appExeName },

    // RawAccel driver install/uninstall helpers (optional but recommended).
    { src: path.join(repoRoot, '_archive', 'installer.exe'), dest: '01_Install_RawAccel_Driver.exe', optional: true },
    { src: path.join(repoRoot, '_archive', 'uninstaller.exe'), dest: '02_Uninstall_RawAccel_Driver.exe', optional: true },
    { src: path.join(repoRoot, 'driver', 'rawaccel.sys'), dest: path.join('driver', 'rawaccel.sys'), optional: true },

    // Backend runtime (kept in a subfolder to avoid users clicking the wrong exe).
    { src: path.join(targetBackendDir, 'mouse_monitor.exe'), dest: path.join('backend', 'mouse_monitor.exe') },
    { src: path.join(targetBackendDir, 'writer.exe'), dest: path.join('backend', 'writer.exe') },
    { src: path.join(targetBackendDir, 'wrapper.dll'), dest: path.join('backend', 'wrapper.dll') },
    { src: path.join(targetBackendDir, 'Newtonsoft.Json.dll'), dest: path.join('backend', 'Newtonsoft.Json.dll'), optional: true },
    { src: path.join(targetBackendDir, 'settings.json'), dest: path.join('backend', 'settings.json') },
  ];

  if (!appExe) {
    // eslint-disable-next-line no-console
    console.warn(`[portable] Could not find GUI exe in: ${targetDir}`);
  } else {
    for (const f of portableFiles) {
      if (!f?.src || !f.dest) continue;
      const ok = await copyIfExists(f.src, path.join(portableDir, f.dest));
      if (!ok && !f.optional) missing.push(path.basename(f.src));
    }
  }

  if (missing.length) {
    // eslint-disable-next-line no-console
    console.error(`[portable] Missing required files: ${missing.join(', ')}`);
    process.exitCode = 1;
    return;
  }

  if (appExeName) {
    const readmePath = path.join(portableDir, 'README-使用说明.txt');
    const readme = [
      'RawAccel Monitor 便携版',
      '',
      '首次在新电脑使用：',
      `1) 右键运行 "01_Install_RawAccel_Driver.exe"（以管理员身份运行）`,
      '2) 重启电脑（驱动安装/卸载通常需要重启生效）',
      '',
      '启动：',
      `- 双击 "${appExeName}"`,
      '',
      '卸载驱动：',
      `1) 右键运行 "02_Uninstall_RawAccel_Driver.exe"（以管理员身份运行）`,
      '2) 重启电脑',
      '',
      '注意：',
      '- backend/ 目录内是程序运行所需文件（mouse_monitor.exe / writer.exe / settings.json 等），请勿单独运行或移动。',
      '- 驱动文件在 driver/rawaccel.sys（请勿移动/改名，否则安装器可能提示 “Can\'t find driver binary”。）',
      '- 高级配置文件在 backend/settings.json（如不确定不要改）。',
      '- 若 GUI 无法启动，通常是缺少 WebView2 Runtime（Windows 10/11 一般自带）。',
      '',
    ].join('\r\n');
    await fs.writeFile(readmePath, readme, 'utf8');
  }

  // eslint-disable-next-line no-console
  console.log(`[portable] Copied runtime files to: ${targetBackendDir}`);
  // eslint-disable-next-line no-console
  console.log(`[portable] Portable folder ready: ${portableDir}`);
}

main().catch((e) => {
  // eslint-disable-next-line no-console
  console.error(e);
  process.exitCode = 1;
});
