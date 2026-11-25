import { constants, createReadStream, createWriteStream } from "node:fs";
import { randomUUID } from "node:crypto";
import fs from "node:fs/promises";
import path from "node:path";
import { performance } from "node:perf_hooks";
import { pipeline } from "node:stream/promises";
import { fileURLToPath } from "node:url";
import { CadRefusal } from "./refusal.js";
import type { CadProcessOptions, CadSystem } from "./process.js";

const WINDOWS_CAD_ASSETS = ["worker.py", "geometry.py", "scan_touchup.py", "scan_measure.py", "project3mf.py", "purpose.py"];
const WINDOWS_HELPER = fileURLToPath(new URL("../../temp/windows-native/Release/bambu-cad-sandbox.exe", import.meta.url));

export async function windowsCadHelper(): Promise<string> {
  const helper = process.env.CAD_WINDOWS_HELPER?.trim() || WINDOWS_HELPER;
  if (path.isAbsolute(helper) && /\.exe$/i.test(helper) && await fs.stat(helper).then(s => s.isFile(), () => false)) return helper;
  throw new CadRefusal("cad-confinement-unavailable", "Build bambu-cad-sandbox.exe and set CAD_WINDOWS_HELPER to its absolute path. CAD never runs without confinement.");
}

export async function windowsCadRuntime(data: string) {
  const venv = path.join(data, "runtime");
  const cfg = await fs.readFile(path.join(venv, "pyvenv.cfg"), "utf8");
  const home = cfg.match(/^home\s*=\s*(.+)$/m)?.[1]?.trim() ?? "";
  if (!path.isAbsolute(home)) throw new Error("Windows CAD needs a managed Python installation.");
  const [managed, base] = await Promise.all([fs.realpath(path.join(data, "python")), fs.realpath(home)]);
  const relative = path.relative(managed, base);
  if (!relative || relative.startsWith(".." + path.sep) || relative === ".." || path.isAbsolute(relative)) {
    throw new Error("Windows CAD's interpreter must be under its private managed Python directory.");
  }
  return { base, python: path.join(base, "python.exe"), site: path.join(venv, "Lib", "site-packages"), assets: path.join(data, "windows-assets") };
}

export async function prepareWindowsCad(system: CadSystem, data: string, source: string, opts: CadProcessOptions) {
  const [helper, runtime] = await Promise.all([windowsCadHelper(), windowsCadRuntime(data)]);
  await fs.mkdir(runtime.assets, { recursive: true });
  await Promise.all(WINDOWS_CAD_ASSETS.map(file => fs.copyFile(path.join(source, file), path.join(runtime.assets, file))));
  await system.run(helper, ["--prepare-runtime", runtime.base, runtime.site, runtime.assets], { ...opts, timeoutMs: 120_000 });
}

type WorkerRequest = {
  output?: string;
  script?: string;
  parameters?: string;
  model?: string;
  reference?: string;
  inputs?: Record<string, string>;
};

async function transferWork(from: string, to: string, excluded: ReadonlySet<string>, check: () => void, signal?: AbortSignal) {
  let count = 0, bytes = 0;
  async function copy(source: string, destination: string, top: boolean) {
    for await (const entry of await fs.opendir(source)) {
      check();
      if (top && excluded.has(entry.name.toLowerCase())) continue;
      if (++count > 4096) throw new Error("CAD work directory contains too many entries.");
      const file = path.join(source, entry.name);
      const target = path.join(destination, entry.name);
      const stat = await fs.lstat(file);
      if (stat.isDirectory() && !stat.isSymbolicLink()) {
        await fs.mkdir(target);
        await copy(file, target, false);
        continue;
      }
      bytes += stat.size;
      if (!stat.isFile() || stat.nlink !== 1 || stat.size > 128 * 1024**2 || bytes > 512 * 1024**2) {
        throw new Error("CAD work directory contains an unsafe or oversized artifact.");
      }
      // A byte stream publishes only the primary file contents, never NTFS alternate data streams.
      await pipeline(createReadStream(file), createWriteStream(target, { flags: "wx" }), { signal });
    }
  }
  await copy(from, to, true);
}

function relocatePaths(value: unknown, from: string, to: string): unknown {
  if (typeof value === "string" && path.isAbsolute(value)) {
    const relative = path.relative(from, value);
    if (relative !== ".." && !relative.startsWith(".." + path.sep) && !path.isAbsolute(relative)) return path.join(to, relative);
  } else if (Array.isArray(value)) {
    return value.map(item => relocatePaths(item, from, to));
  } else if (value && typeof value === "object") {
    return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, relocatePaths(item, from, to)]));
  }
  return value;
}

/** Stage per-call copies so the OS never grants an executed design access to its saved checkpoints. */
export async function runWindowsCad(
  system: CadSystem, data: string, mode: string, sourceRequest: string, job: string,
  protectedFiles: string[], opts: CadProcessOptions,
): Promise<{ stdout: string; stderr?: string }> {
  const started = performance.now();
  const check = () => {
    opts.signal?.throwIfAborted();
    const remaining = Math.ceil(opts.timeoutMs - (performance.now() - started));
    if (remaining <= 0) throw new Error("CAD operation timed out while staging Windows work.");
    return remaining;
  };
  const [helper, runtime] = await Promise.all([windowsCadHelper(), windowsCadRuntime(data)]);
  const stage = await fs.mkdtemp(path.join(data, "windows-job-"));
  try {
    const work = path.join(stage, "work"), input = path.join(stage, "input"), tmp = path.join(work, "tmp");
    await fs.mkdir(input);
    await fs.mkdir(work);
    const request: WorkerRequest = JSON.parse(await fs.readFile(sourceRequest, "utf8"));
    if (request.output !== undefined) request.output = work;
    for (const field of ["script", "parameters", "model", "reference"] as const) {
      const file = request[field];
      if (!file) continue;
      check();
      const target = path.join(input, path.basename(file));
      await fs.copyFile(file, target, constants.COPYFILE_EXCL);
      request[field] = target;
    }
    if (request.inputs && Object.keys(request.inputs).length) {
      const directory = path.join(input, "assets");
      await fs.mkdir(directory);
      const copied = new Map<string, string>();
      for (const [alias, file] of Object.entries(request.inputs)) {
        check();
        const known = copied.get(file);
        const target = known ?? path.join(directory, path.basename(file));
        if (!known) await fs.copyFile(file, target, constants.COPYFILE_EXCL);
        copied.set(file, target);
        request.inputs[alias] = target;
      }
    }
    const workerRequest = path.join(input, "worker-request.json");
    const denied = path.join(stage, "private-proof.txt");
    if (mode === "smoke") await fs.writeFile(denied, "Windows CAD isolation proof\n");
    await fs.writeFile(workerRequest, JSON.stringify({ ...request, ...(mode === "smoke" && { windows_private_proof: denied }) }));
    const excluded = new Set(["tmp", "build.log", ...protectedFiles.map(file => path.basename(file).toLowerCase())]);
    if (mode === "inspect") await transferWork(job, work, excluded, check, opts.signal);
    const original = new Set(["tmp", ...(await fs.readdir(work)).map(file => file.toLowerCase())]);
    const profile = randomUUID();
    const launch = {
      ...opts, cwd: stage, timeoutMs: check(),
      env: { ...opts.env, PATH: path.join(opts.env.SystemRoot ?? "C:\\Windows", "System32"),
        HOME: work, USERPROFILE: work, TMPDIR: tmp, TMP: tmp, TEMP: tmp, MPLCONFIGDIR: path.join(tmp, "mpl"),
        UV_CACHE_DIR: undefined, UV_PYTHON_INSTALL_DIR: undefined, CAD_WINDOWS_SITE_PACKAGES: runtime.site },
    };
    const result = await system.run(helper, ["--run", profile, input, work, runtime.python, "-I", "-B", "-S", "-X", "utf8",
      path.join(runtime.assets, "worker.py"), mode, workerRequest], launch).catch(async error => {
      // If the deadline killed the launcher, Windows closed its Job Object but
      // the launcher could not remove the per-operation AppContainer profile.
      await system.run(helper, ["--cleanup-profile", profile], { ...launch, signal: undefined, timeoutMs: 5000 }).catch(() => {});
      throw error;
    });
    await transferWork(work, job, original, check, opts.signal);
    if (mode === "build") return result;
    return { ...result, stdout: JSON.stringify(relocatePaths(JSON.parse(result.stdout), work, job)) };
  } finally {
    await fs.rm(stage, { recursive: true, force: true, maxRetries: 3, retryDelay: 100 });
  }
}
