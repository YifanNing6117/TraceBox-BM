"use strict";

const fs = require("node:fs");
const http = require("node:http");
const path = require("node:path");
const { spawn, execFile } = require("node:child_process");

const REPO_ROOT = path.resolve(__dirname, "..", "..", "..");
const FRONTEND_ROOT = path.join(REPO_ROOT, "web-console", "frontend");
const TRACEBOX_CLI = path.join(REPO_ROOT, "tools", "tracebox_cli.py");
const PORT_LIST_TOOL = path.join(REPO_ROOT, "tools", "list_serial_ports.py");
const ARTIFACT_ROOTS = [
  { scope: "exports", directory: path.join(REPO_ROOT, "artifacts", "exports") },
  { scope: "raw", directory: path.join(REPO_ROOT, "artifacts", "raw") },
];
const ALLOWED_ARTIFACT_EXTENSIONS = new Set([
  ".csv",
  ".json",
  ".md",
  ".tbdump",
  ".log",
]);
const ALLOWED_DUMP_BAUDS = new Set([115200, 230400, 460800]);
const SERIAL_ACTIONS = new Set([
  "info",
  "metrics",
  "prepare",
  "record",
  "recover",
  "dump",
  "fault-torn",
  "full",
  "trace-prepare",
  "trace-record",
  "trace-demo",
]);
const STATIC_FILES = new Map([
  ["/", ["index.html", "text/html; charset=utf-8"]],
  ["/index.html", ["index.html", "text/html; charset=utf-8"]],
  ["/src/app.js", [path.join("src", "app.js"), "text/javascript; charset=utf-8"]],
  ["/src/styles.css", [path.join("src", "styles.css"), "text/css; charset=utf-8"]],
]);
const MAX_REQUEST_BYTES = 16 * 1024;
const MAX_JOB_LOG_CHARS = 2 * 1024 * 1024;

class HttpError extends Error {
  constructor(statusCode, message) {
    super(message);
    this.statusCode = statusCode;
  }
}

function requireInteger(value, field, minimum, maximum) {
  if (!Number.isInteger(value) || value < minimum || value > maximum) {
    throw new HttpError(400, `${field} 必须是 ${minimum}..${maximum} 范围内的整数`);
  }
  return value;
}

function requireSerialPort(value) {
  if (typeof value !== "string" || !/^COM[1-9][0-9]{0,2}$/i.test(value)) {
    throw new HttpError(400, "串口名称格式应类似 COM9");
  }
  return value.toUpperCase();
}

function buildCliArguments(payload) {
  if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
    throw new HttpError(400, "请求正文必须是 JSON 对象");
  }
  const action = payload.action;
  if (typeof action !== "string" ||
      (!SERIAL_ACTIONS.has(action) && action !== "bench")) {
    throw new HttpError(400, "不支持的操作");
  }

  const args = ["-u", TRACEBOX_CLI, action];
  if (SERIAL_ACTIONS.has(action)) {
    args.push("--port", requireSerialPort(payload.port));
  }
  if (action === "prepare" || action === "record" || action === "full") {
    args.push(
      "--seconds",
      String(requireInteger(payload.seconds, "seconds", 1, 60)),
    );
  }
  if (action === "trace-prepare" || action === "trace-record" || action === "trace-demo") {
    args.push(
      "--frames",
      String(requireInteger(payload.frames, "frames", action === "trace-demo" ? 2 : 1,
        action === "trace-demo" ? 256 : 10000)),
    );
  }
  if (action === "trace-demo") {
    const testerPort = requireSerialPort(payload.tester_port);
    if (testerPort === requireSerialPort(payload.port)) {
      throw new HttpError(400, "TraceBox 与 Tester 不能使用同一个串口");
    }
    args.push("--tester-port", testerPort);
  }
  if (action === "dump" || action === "full" || action === "trace-demo") {
    const baud = requireInteger(payload.dump_baud, "dump_baud", 115200, 460800);
    if (!ALLOWED_DUMP_BAUDS.has(baud)) {
      throw new HttpError(400, "导出波特率只能是 115200、230400 或 460800");
    }
    args.push("--dump-baud", String(baud));
  }
  return args;
}

function publicJob(job) {
  if (!job) {
    return null;
  }
  return {
    id: job.id,
    action: job.action,
    state: job.state,
    started_at: job.startedAt,
    finished_at: job.finishedAt,
    return_code: job.returnCode,
    log: job.log,
  };
}

class OperationCoordinator {
  constructor(options = {}) {
    this.pythonExecutable = options.pythonExecutable || process.env.TRACEBOX_PYTHON ||
      (process.platform === "win32" ? "python" : "python3");
    this.spawnImpl = options.spawnImpl || spawn;
    this.sequence = 0;
    this.current = null;
    this.currentChild = null;
  }

  start(payload) {
    if (this.current && (this.current.state === "queued" || this.current.state === "running")) {
      throw new HttpError(409, `任务 ${this.current.id} 仍在运行，串口暂不可用`);
    }

    const args = buildCliArguments(payload);
    const id = `${Date.now()}-${++this.sequence}`;
    const job = {
      id,
      action: payload.action,
      state: "queued",
      startedAt: new Date().toISOString(),
      finishedAt: null,
      returnCode: null,
      log: "",
      finished: false,
    };
    this.current = job;

    let child;
    try {
      child = this.spawnImpl(this.pythonExecutable, args, {
        cwd: REPO_ROOT,
        shell: false,
        windowsHide: true,
        stdio: ["ignore", "pipe", "pipe"],
      });
    } catch (error) {
      this.finish(job, null, `无法启动 Python：${error.message}\n`);
      return publicJob(job);
    }

    this.currentChild = child;
    job.state = "running";
    const append = (chunk) => {
      job.log += chunk.toString("utf8");
      if (job.log.length > MAX_JOB_LOG_CHARS) {
        job.log = `[older output truncated]\n${job.log.slice(-MAX_JOB_LOG_CHARS)}`;
      }
    };
    if (child.stdout) {
      child.stdout.on("data", append);
    }
    if (child.stderr) {
      child.stderr.on("data", append);
    }
    child.on("error", (error) => {
      this.finish(job, null, `子进程错误：${error.message}\n`);
    });
    child.on("close", (code, signal) => {
      const suffix = signal ? `子进程被 ${signal} 终止\n` : "";
      this.finish(job, code, suffix);
    });
    return publicJob(job);
  }

  finish(job, returnCode, suffix = "") {
    if (job.finished) {
      return;
    }
    job.finished = true;
    job.returnCode = returnCode;
    job.finishedAt = new Date().toISOString();
    job.log += suffix;
    job.state = returnCode === 0 ? "succeeded" : "failed";
    this.currentChild = null;
  }

  getCurrent() {
    return publicJob(this.current);
  }

  stopActive() {
    const job = this.current;
    const child = this.currentChild;
    if (!job || !child || job.finished) {
      return;
    }
    child.kill();
    this.finish(job, null, "本地服务已停止当前子进程\n");
  }
}

function listArtifacts() {
  const artifacts = [];
  for (const root of ARTIFACT_ROOTS) {
    if (!fs.existsSync(root.directory)) {
      continue;
    }
    for (const entry of fs.readdirSync(root.directory, { withFileTypes: true })) {
      if (!entry.isFile() || !ALLOWED_ARTIFACT_EXTENSIONS.has(path.extname(entry.name).toLowerCase())) {
        continue;
      }
      const fullPath = path.join(root.directory, entry.name);
      const stat = fs.statSync(fullPath);
      artifacts.push({
        name: entry.name,
        scope: root.scope,
        size: stat.size,
        modified_at: stat.mtime.toISOString(),
        download_url: `/api/artifacts/${encodeURIComponent(entry.name)}`,
      });
    }
  }
  artifacts.sort((left, right) => right.modified_at.localeCompare(left.modified_at));
  return artifacts.slice(0, 100);
}

function resolveArtifact(name) {
  if (typeof name !== "string" || name.length === 0 || path.basename(name) !== name ||
      !ALLOWED_ARTIFACT_EXTENSIONS.has(path.extname(name).toLowerCase())) {
    return null;
  }
  for (const root of ARTIFACT_ROOTS) {
    const candidate = path.join(root.directory, name);
    if (fs.existsSync(candidate) && fs.statSync(candidate).isFile()) {
      return candidate;
    }
  }
  return null;
}

function sendJson(response, statusCode, data) {
  const body = Buffer.from(JSON.stringify(data));
  response.writeHead(statusCode, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": body.length,
    "Cache-Control": "no-store",
  });
  response.end(body);
}

function readJson(request) {
  return new Promise((resolve, reject) => {
    let received = 0;
    let rejected = false;
    const chunks = [];
    request.on("data", (chunk) => {
      if (rejected) {
        return;
      }
      received += chunk.length;
      if (received > MAX_REQUEST_BYTES) {
        rejected = true;
        reject(new HttpError(413, "请求正文过大"));
        return;
      }
      chunks.push(chunk);
    });
    request.on("end", () => {
      if (rejected) {
        return;
      }
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}"));
      } catch (_error) {
        reject(new HttpError(400, "JSON 格式无效"));
      }
    });
    request.on("error", reject);
  });
}

function enumeratePorts(pythonExecutable) {
  return new Promise((resolve, reject) => {
    execFile(
      pythonExecutable,
      [PORT_LIST_TOOL],
      { cwd: REPO_ROOT, windowsHide: true, timeout: 5000, maxBuffer: 256 * 1024 },
      (error, stdout, stderr) => {
        if (error) {
          reject(new HttpError(503, (stderr || error.message).trim()));
          return;
        }
        try {
          resolve(JSON.parse(stdout));
        } catch (_parseError) {
          reject(new HttpError(502, "串口扫描工具返回了无效 JSON"));
        }
      },
    );
  });
}

function serveStatic(requestPath, response) {
  const descriptor = STATIC_FILES.get(requestPath);
  if (!descriptor) {
    return false;
  }
  const [relativePath, contentType] = descriptor;
  const filePath = path.join(FRONTEND_ROOT, relativePath);
  if (!fs.existsSync(filePath)) {
    throw new HttpError(404, "找不到前端资源");
  }
  const body = fs.readFileSync(filePath);
  response.writeHead(200, {
    "Content-Type": contentType,
    "Content-Length": body.length,
    "Cache-Control": "no-cache",
    "Content-Security-Policy": "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'",
    "X-Content-Type-Options": "nosniff",
  });
  response.end(body);
  return true;
}

function createServer(options = {}) {
  const coordinator = options.coordinator || new OperationCoordinator(options);
  const server = http.createServer(async (request, response) => {
    try {
      const url = new URL(request.url, "http://127.0.0.1");
      if (request.method === "GET" && url.pathname === "/api/health") {
        sendJson(response, 200, {
          status: "ok",
          bind: "127.0.0.1",
          python: coordinator.pythonExecutable,
          cli_exists: fs.existsSync(TRACEBOX_CLI),
          operation: coordinator.getCurrent(),
        });
        return;
      }
      if (request.method === "GET" && url.pathname === "/api/serial/ports") {
        sendJson(response, 200, await enumeratePorts(coordinator.pythonExecutable));
        return;
      }
      if (request.method === "POST" && url.pathname === "/api/operations") {
        sendJson(response, 202, coordinator.start(await readJson(request)));
        return;
      }
      if (request.method === "GET" && url.pathname === "/api/operations/current") {
        sendJson(response, 200, { operation: coordinator.getCurrent() });
        return;
      }
      if (request.method === "GET" && url.pathname === "/api/artifacts") {
        sendJson(response, 200, { artifacts: listArtifacts() });
        return;
      }
      if (request.method === "GET" && url.pathname.startsWith("/api/artifacts/")) {
        const name = decodeURIComponent(url.pathname.slice("/api/artifacts/".length));
        const artifactPath = resolveArtifact(name);
        if (!artifactPath) {
          throw new HttpError(404, "找不到指定产物");
        }
        const stat = fs.statSync(artifactPath);
        response.writeHead(200, {
          "Content-Type": "application/octet-stream",
          "Content-Length": stat.size,
          "Content-Disposition": `attachment; filename*=UTF-8''${encodeURIComponent(name)}`,
          "X-Content-Type-Options": "nosniff",
        });
        fs.createReadStream(artifactPath).pipe(response);
        return;
      }
      if (request.method === "GET" && serveStatic(url.pathname, response)) {
        return;
      }
      throw new HttpError(404, "找不到请求的资源");
    } catch (error) {
      const statusCode = error instanceof HttpError ? error.statusCode : 500;
      if (!response.headersSent) {
        sendJson(response, statusCode, { error: error.message || "服务内部错误" });
      } else {
        response.destroy();
      }
    }
  });
  server.on("close", () => coordinator.stopActive());
  return server;
}

function parseListenPort(argv) {
  const index = argv.indexOf("--port");
  if (index < 0) {
    return 8765;
  }
  return requireInteger(Number(argv[index + 1]), "listen port", 1024, 65535);
}

if (require.main === module) {
  const listenPort = parseListenPort(process.argv.slice(2));
  const server = createServer();
  server.listen(listenPort, "127.0.0.1", () => {
    console.log(`TraceBox console: http://127.0.0.1:${listenPort}`);
    console.log(`Python: ${process.env.TRACEBOX_PYTHON || (process.platform === "win32" ? "python" : "python3")}`);
    console.log("Press Ctrl+C to stop.");
  });
  process.once("SIGINT", () => server.close());
  process.once("SIGTERM", () => server.close());
}

module.exports = {
  HttpError,
  OperationCoordinator,
  buildCliArguments,
  createServer,
  listArtifacts,
  resolveArtifact,
};
