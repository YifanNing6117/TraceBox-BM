"use strict";

const assert = require("node:assert/strict");
const { EventEmitter } = require("node:events");
const { PassThrough } = require("node:stream");
const test = require("node:test");

const {
  HttpError,
  OperationCoordinator,
  buildCliArguments,
  createServer,
  listArtifacts,
  resolveArtifact,
} = require("../server/src/server.js");

function expectHttpError(callback, statusCode) {
  assert.throws(callback, (error) => {
    assert.ok(error instanceof HttpError);
    assert.equal(error.statusCode, statusCode);
    return true;
  });
}

test("CLI arguments are whitelisted and bounded", () => {
  const args = buildCliArguments({
    action: "full",
    port: "com9",
    seconds: 60,
    dump_baud: 460800,
  });
  assert.deepEqual(args.slice(-6), [
    "--port", "COM9", "--seconds", "60", "--dump-baud", "460800",
  ]);
  expectHttpError(() => buildCliArguments({ action: "shell", port: "COM9" }), 400);
  expectHttpError(() => buildCliArguments({
    action: "full", port: "COM9 & calc", seconds: 3, dump_baud: 460800,
  }), 400);
  expectHttpError(() => buildCliArguments({
    action: "record", port: "COM9", seconds: 61,
  }), 400);
  expectHttpError(() => buildCliArguments({
    action: "dump", port: "COM9", dump_baud: 921600,
  }), 400);

  const traceArgs = buildCliArguments({
    action: "trace-demo",
    port: "com9",
    tester_port: "com10",
    frames: 64,
    dump_baud: 460800,
  });
  assert.deepEqual(traceArgs.slice(-8), [
    "--port", "COM9", "--frames", "64", "--tester-port", "COM10",
    "--dump-baud", "460800",
  ]);
  expectHttpError(() => buildCliArguments({
    action: "trace-demo", port: "COM9", tester_port: "COM9",
    frames: 8, dump_baud: 460800,
  }), 400);
  expectHttpError(() => buildCliArguments({
    action: "trace-demo", port: "COM9", tester_port: "COM10",
    frames: 257, dump_baud: 460800,
  }), 400);
});

test("coordinator enforces one active operation and captures output", async () => {
  let child;
  const fakeSpawn = () => {
    child = new EventEmitter();
  child.stdout = new PassThrough();
  child.stderr = new PassThrough();
    child.kill = () => child.emit("close", null, "SIGTERM");
    return child;
  };
  const coordinator = new OperationCoordinator({
    pythonExecutable: "python-test",
    spawnImpl: fakeSpawn,
  });
  const first = coordinator.start({ action: "bench" });
  assert.equal(first.state, "running");
  expectHttpError(() => coordinator.start({ action: "bench" }), 409);

  child.stdout.write("benchmark PASS\n");
  child.emit("close", 0, null);
  await new Promise((resolve) => setImmediate(resolve));
  const completed = coordinator.getCurrent();
  assert.equal(completed.state, "succeeded");
  assert.equal(completed.return_code, 0);
  assert.match(completed.log, /benchmark PASS/);
});

test("coordinator stops an active child when the server closes", () => {
  let killed = false;
  const fakeSpawn = () => {
    const child = new EventEmitter();
    child.stdout = new PassThrough();
    child.stderr = new PassThrough();
    child.kill = () => { killed = true; };
    return child;
  };
  const coordinator = new OperationCoordinator({
    pythonExecutable: "python-test",
    spawnImpl: fakeSpawn,
  });
  coordinator.start({ action: "bench" });
  coordinator.stopActive();
  assert.equal(killed, true);
  assert.equal(coordinator.getCurrent().state, "failed");
});

test("artifact resolver rejects traversal and exposes only approved files", () => {
  assert.equal(resolveArtifact("../README.md"), null);
  assert.equal(resolveArtifact("firmware.hex"), null);
  for (const artifact of listArtifacts()) {
    assert.equal(artifact.name, artifact.name.split(/[\\/]/).pop());
    assert.match(artifact.download_url, /^\/api\/artifacts\//);
  }
});

test("HTTP server exposes health, frontend and rejects unknown API", async (context) => {
  const coordinator = new OperationCoordinator({
    pythonExecutable: "python-test",
    spawnImpl: () => { throw new Error("unexpected spawn"); },
  });
  const server = createServer({ coordinator });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  context.after(() => server.close());
  const address = server.address();
  const base = `http://127.0.0.1:${address.port}`;

  const health = await fetch(`${base}/api/health`);
  assert.equal(health.status, 200);
  assert.equal((await health.json()).status, "ok");

  const frontend = await fetch(`${base}/`);
  assert.equal(frontend.status, 200);
  assert.match(await frontend.text(), /TRACEBOX/);
  assert.match(frontend.headers.get("content-security-policy"), /default-src 'self'/);

  const invalid = await fetch(`${base}/api/operations`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ action: "arbitrary-command" }),
  });
  assert.equal(invalid.status, 400);

  const missing = await fetch(`${base}/api/artifacts/%2e%2e%2fREADME.md`);
  assert.equal(missing.status, 404);
});
