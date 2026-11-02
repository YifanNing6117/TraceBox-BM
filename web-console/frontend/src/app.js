"use strict";

const elements = {
  serverStatus: document.querySelector("#server-status"),
  ownerStatus: document.querySelector("#owner-status"),
  resultStatus: document.querySelector("#result-status"),
  serialPort: document.querySelector("#serial-port"),
  testerPort: document.querySelector("#tester-port"),
  seconds: document.querySelector("#seconds"),
  frames: document.querySelector("#frames"),
  dumpBaud: document.querySelector("#dump-baud"),
  refreshPorts: document.querySelector("#refresh-ports"),
  refreshArtifacts: document.querySelector("#refresh-artifacts"),
  operationBadge: document.querySelector("#operation-badge"),
  operationTitle: document.querySelector("#operation-title"),
  operationProgress: document.querySelector("#operation-progress"),
  progressBar: document.querySelector("#progress-bar"),
  terminal: document.querySelector("#terminal"),
  artifactRows: document.querySelector("#artifact-rows"),
  metricPages: document.querySelector("#metric-pages"),
  metricSamples: document.querySelector("#metric-samples"),
  metricEvents: document.querySelector("#metric-events"),
  metricErrors: document.querySelector("#metric-errors"),
  metricHwm: document.querySelector("#metric-hwm"),
  benchPolling: document.querySelector("#bench-polling"),
  benchDma: document.querySelector("#bench-dma"),
  benchStart: document.querySelector("#bench-start"),
  benchForeground: document.querySelector("#bench-foreground"),
  toast: document.querySelector("#toast"),
  actionButtons: [...document.querySelectorAll("[data-action]")],
};

let toastTimer = null;
let lastOperationState = null;

const STATE_LABELS = {
  queued: "排队中",
  running: "运行中",
  succeeded: "成功",
  failed: "失败",
};

const ACTION_LABELS = {
  info: "读取设备信息",
  metrics: "读取运行指标",
  prepare: "预擦除",
  record: "数据记录",
  recover: "恢复扫描",
  dump: "数据导出",
  "fault-torn": "残页故障注入",
  bench: "轮询与 DMA 性能对比",
  full: "完整记录闭环",
  "trace-prepare": "事件区预擦除",
  "trace-record": "SPI 事件记录",
  "trace-demo": "V2 三板一键闭环",
};

async function requestJson(url, options = {}) {
  const response = await fetch(url, options);
  let data;
  try {
    data = await response.json();
  } catch (_error) {
    throw new Error(`HTTP ${response.status}：服务返回内容无效`);
  }
  if (!response.ok) {
    throw new Error(data.error || `HTTP ${response.status}`);
  }
  return data;
}

function showToast(message, error = false) {
  clearTimeout(toastTimer);
  elements.toast.textContent = message;
  elements.toast.className = `toast visible${error ? " error" : ""}`;
  toastTimer = setTimeout(() => {
    elements.toast.className = "toast";
  }, 3500);
}

function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MiB`;
}

function formatDate(isoDate) {
  return new Intl.DateTimeFormat("zh-CN", {
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  }).format(new Date(isoDate));
}

function latestMatch(text, pattern) {
  const matches = [...text.matchAll(pattern)];
  return matches.length ? matches[matches.length - 1] : null;
}

function calculateProgress(operation) {
  if (!operation) return 0;
  if (operation.state === "succeeded" || operation.state === "failed") return 100;
  const log = operation.log || "";
  const dump = latestMatch(log, /dump_progress=(\d+)%/g);
  if (dump) return Math.max(0, Math.min(100, Number(dump[1])));
  const target = latestMatch(log, /target_(?:pages|frames)=(\d+)/g);
  const page = latestMatch(log, /P state=\d+ packed=(\d+)/g);
  if (target && page && Number(target[1]) > 0) {
    return Math.max(1, Math.min(99, Math.floor(Number(page[1]) * 100 / Number(target[1]))));
  }
  if (["prepare", "trace-prepare"].includes(operation.action)) return 15;
  return 4;
}

function updateEvidenceMetrics(log) {
  const committed = latestMatch(log, /pages_committed=(\d+)/g);
  const pageCount = latestMatch(log, /"page_count"\s*:\s*(\d+)/g);
  const samples = latestMatch(log, /"sample_count"\s*:\s*(\d+)/g);
  const events = latestMatch(log, /"event_count"\s*:\s*(\d+)/g);
  const adcBlocks = latestMatch(log, /adc_blocks=(\d+)/g);
  const hwm = latestMatch(log, /page_pool_hwm=(\d+)/g);
  const errors = latestMatch(
    log,
    /sequence_errors=(\d+) page_crc_errors=(\d+) flash_errors=(\d+)/g,
  );
  if (pageCount || committed) {
    elements.metricPages.textContent = Number((pageCount || committed)[1]).toLocaleString("en-US");
  }
  if (samples) {
    elements.metricSamples.textContent = Number(samples[1]).toLocaleString("en-US");
  } else if (adcBlocks) {
    elements.metricSamples.textContent = (Number(adcBlocks[1]) * 96).toLocaleString("en-US");
  }
  if (events) {
    elements.metricEvents.textContent = Number(events[1]).toLocaleString("en-US");
  }
  if (hwm) elements.metricHwm.textContent = hwm[1];
  if (errors) {
    elements.metricErrors.textContent = String(
      Number(errors[1]) + Number(errors[2]) + Number(errors[3]),
    );
  }
  const benchmarkRows = [...log.matchAll(
    /"mode": "(polling|dma)"[\s\S]*?"start_api_cycles": "(\d+)"[\s\S]*?"throughput_bytes_per_s": "(\d+)"[\s\S]*?"foreground_iterations": "(\d+)"/g,
  )];
  for (const row of benchmarkRows) {
    if (row[1] === "polling") {
      elements.benchPolling.textContent = `${Number(row[3]).toLocaleString("en-US")} B/s`;
    } else {
      elements.benchDma.textContent = `${Number(row[3]).toLocaleString("en-US")} B/s`;
      elements.benchStart.textContent = `${Number(row[2]).toLocaleString("en-US")} 周期`;
      elements.benchForeground.textContent = Number(row[4]).toLocaleString("en-US");
    }
  }
}

function setBusy(isBusy) {
  for (const button of elements.actionButtons) {
    button.disabled = isBusy;
  }
  elements.refreshPorts.disabled = isBusy;
}

function renderOperation(operation) {
  if (!operation) {
    elements.ownerStatus.textContent = "空闲";
    elements.operationBadge.textContent = "空闲";
    elements.operationBadge.className = "badge idle";
    setBusy(false);
    return;
  }

  const running = operation.state === "queued" || operation.state === "running";
  const progress = calculateProgress(operation);
  const actionLabel = ACTION_LABELS[operation.action] || operation.action;
  const stateLabel = STATE_LABELS[operation.state] || operation.state;
  elements.ownerStatus.textContent = running ? `${actionLabel}占用中` : "空闲";
  elements.operationBadge.textContent = stateLabel;
  elements.operationBadge.className = `badge ${operation.state}`;
  elements.operationTitle.textContent = `${actionLabel} · ${operation.id}`;
  elements.operationProgress.textContent = `${progress}%`;
  elements.progressBar.style.width = `${progress}%`;
  elements.terminal.textContent = operation.log || "任务已经启动…";
  elements.terminal.scrollTop = elements.terminal.scrollHeight;
  elements.resultStatus.textContent = running ? "运行中" : stateLabel;
  elements.resultStatus.className = `status-value ${operation.state === "succeeded" ? "pass" : operation.state === "failed" ? "fail" : "pending"}`;
  setBusy(running);
  updateEvidenceMetrics(operation.log || "");

  if (lastOperationState && lastOperationState !== operation.state && !running) {
    showToast(
      operation.state === "succeeded" ? `${actionLabel}已完成` : `${actionLabel}执行失败`,
      operation.state !== "succeeded",
    );
    loadArtifacts();
  }
  lastOperationState = operation.state;
}

async function loadHealth() {
  try {
    const data = await requestJson("/api/health");
    elements.serverStatus.textContent = "在线";
    elements.serverStatus.className = "status-value pass";
    renderOperation(data.operation);
  } catch (error) {
    elements.serverStatus.textContent = "离线";
    elements.serverStatus.className = "status-value fail";
    showToast(error.message, true);
  }
}

async function loadPorts() {
  const previous = elements.serialPort.value || localStorage.getItem("tracebox-port") || "COM9";
  const previousTester = elements.testerPort.value || localStorage.getItem("tester-port") || "COM10";
  elements.refreshPorts.disabled = true;
  try {
    const data = await requestJson("/api/serial/ports");
    const ports = data.ports || [];
    populatePortSelect(elements.serialPort, ports, previous, "TraceBox");
    populatePortSelect(elements.testerPort, ports, previousTester, "Tester");
  } catch (error) {
    elements.serialPort.replaceChildren(new Option("串口扫描失败", ""));
    elements.testerPort.replaceChildren(new Option("串口扫描失败", ""));
    showToast(error.message, true);
  } finally {
    elements.refreshPorts.disabled = false;
  }
}

function populatePortSelect(select, ports, preferred, role) {
  select.replaceChildren();
  for (const port of ports) {
    const option = document.createElement("option");
    option.value = port.device;
    option.textContent = `${port.device} · ${port.description || "串口设备"}`;
    select.append(option);
  }
  if (!select.options.length) {
    select.append(new Option("未发现串口（硬件可稍后连接）", ""));
    return;
  }
  const exact = [...select.options].find((option) => option.value === preferred);
  if (exact) {
    select.value = exact.value;
    return;
  }
  const placeholder = new Option(`请选择 ${role} 串口`, "");
  select.prepend(placeholder);
  select.value = "";
}

async function loadArtifacts() {
  try {
    const data = await requestJson("/api/artifacts");
    elements.artifactRows.replaceChildren();
    const artifacts = (data.artifacts || []).slice(0, 24);
    if (!artifacts.length) {
      const row = document.createElement("tr");
      const cell = document.createElement("td");
      cell.colSpan = 5;
      cell.className = "empty";
      cell.textContent = "还没有本地产物";
      row.append(cell);
      elements.artifactRows.append(row);
      return;
    }
    for (const artifact of artifacts) {
      const row = document.createElement("tr");
      const name = document.createElement("td");
      name.textContent = artifact.name;
      const scope = document.createElement("td");
      scope.textContent = artifact.scope === "exports" ? "导出结果" : "原始证据";
      scope.className = "file-kind";
      const size = document.createElement("td");
      size.textContent = formatBytes(artifact.size);
      const modified = document.createElement("td");
      modified.textContent = formatDate(artifact.modified_at);
      const action = document.createElement("td");
      const link = document.createElement("a");
      link.href = artifact.download_url;
      link.className = "download";
      link.textContent = "下载 ↗";
      action.append(link);
      row.append(name, scope, size, modified, action);
      elements.artifactRows.append(row);
    }
  } catch (error) {
    showToast(error.message, true);
  }
}

function operationPayload(action) {
  const payload = { action };
  if (action !== "bench") {
    if (!elements.serialPort.value) {
      throw new Error("当前没有可用串口；连接下位机后先扫描串口");
    }
    payload.port = elements.serialPort.value;
    localStorage.setItem("tracebox-port", payload.port);
  }
  if (action === "trace-demo") {
    if (!elements.testerPort.value) {
      throw new Error("请选择 Mini-DCM Tester 串口");
    }
    if (elements.testerPort.value === payload.port) {
      throw new Error("TraceBox 与 Tester 不能选择同一个串口");
    }
    payload.tester_port = elements.testerPort.value;
    localStorage.setItem("tester-port", payload.tester_port);
  }
  if (["prepare", "record", "full"].includes(action)) {
    payload.seconds = Number(elements.seconds.value);
  }
  if (["dump", "full"].includes(action)) {
    payload.dump_baud = Number(elements.dumpBaud.value);
  }
  if (["trace-prepare", "trace-record", "trace-demo"].includes(action)) {
    payload.frames = Number(elements.frames.value);
  }
  if (action === "trace-demo") {
    payload.dump_baud = Number(elements.dumpBaud.value);
  }
  return payload;
}

async function startOperation(action) {
  if (["prepare", "full", "trace-prepare", "trace-demo"].includes(action) &&
      !window.confirm("该操作会擦除 TraceBox 数据区，确定继续吗？")) return;
  if (action === "fault-torn" &&
      !window.confirm("将故意写入一个无 commit 的尾页，确定执行故障注入吗？")) return;

  try {
    const payload = operationPayload(action);
    const operation = await requestJson("/api/operations", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
    lastOperationState = operation.state;
    renderOperation(operation);
    showToast(`${ACTION_LABELS[action] || action}已启动`);
  } catch (error) {
    showToast(error.message, true);
  }
}

async function pollOperation() {
  try {
    const data = await requestJson("/api/operations/current");
    renderOperation(data.operation);
  } catch (_error) {
    elements.serverStatus.textContent = "离线";
    elements.serverStatus.className = "status-value fail";
  }
}

elements.refreshPorts.addEventListener("click", loadPorts);
elements.refreshArtifacts.addEventListener("click", loadArtifacts);
for (const button of elements.actionButtons) {
  button.addEventListener("click", () => startOperation(button.dataset.action));
}

loadHealth();
loadPorts();
loadArtifacts();
setInterval(pollOperation, 650);
