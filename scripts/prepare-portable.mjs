import fs from 'node:fs/promises';
import path from 'node:path';

const repoRoot = process.cwd();
const targetDir = path.join(repoRoot, 'src-tauri', 'target', 'release');

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

async function main() {
  await ensureDir(targetDir);

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
    const destPath = path.join(targetDir, file);
    const ok = await copyIfExists(srcPath, destPath);
    if (!ok) missing.push(file);
  }

  for (const file of optionalRuntimeFiles) {
    const srcPath = path.join(repoRoot, file);
    const destPath = path.join(targetDir, file);
    await copyIfExists(srcPath, destPath);
  }

  const settingsDest = path.join(targetDir, 'settings.json');
  if (!(await exists(settingsDest))) {
    const srcSettings = (await exists(path.join(repoRoot, 'settings.json')))
      ? path.join(repoRoot, 'settings.json')
      : path.join(repoRoot, 'settings.example.json');

    const ok = await copyIfExists(srcSettings, settingsDest);
    if (!ok) missing.push('settings.example.json');
  }

  if (missing.length) {
    // Fail the build so users don't end up with a GUI that can't function.
    // eslint-disable-next-line no-console
    console.error(`[portable] Missing required files in repo root: ${missing.join(', ')}`);
    process.exitCode = 1;
    return;
  }

  // Also produce a clean portable folder for "double click and play".
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
  const appExe = exeCandidates[0]?.full;
  const appExeName = exeCandidates[0]?.name;

  const portableRoot = path.join(repoRoot, 'dist-portable');
  const portableDir = path.join(portableRoot, 'RawAccel Monitor');
  await ensureDir(portableDir);

  const portableFiles = [
    { src: appExe, dest: appExeName },
    { src: path.join(targetDir, 'mouse_monitor.exe'), dest: 'mouse_monitor.exe' },
    { src: path.join(targetDir, 'writer.exe'), dest: 'writer.exe' },
    { src: path.join(targetDir, 'wrapper.dll'), dest: 'wrapper.dll' },
    { src: path.join(targetDir, 'Newtonsoft.Json.dll'), dest: 'Newtonsoft.Json.dll', optional: true },
    { src: path.join(targetDir, 'settings.json'), dest: 'settings.json' },
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

  // eslint-disable-next-line no-console
  console.log(`[portable] Copied runtime files to: ${targetDir}`);
  // eslint-disable-next-line no-console
  console.log(`[portable] Portable folder ready: ${portableDir}`);
}

main().catch((e) => {
  // eslint-disable-next-line no-console
  console.error(e);
  process.exitCode = 1;
});
