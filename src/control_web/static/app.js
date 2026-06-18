const output = document.getElementById("output");
const datasetRows = document.getElementById("datasetRows");
const toggles = {
  sensor_data: {
    label: "sensor_data",
    start: "start_sensor_data",
    stop: "stop_sensor_data",
  },
  ic_gvins_zmq: {
    label: "ic_gvins_zmq",
    start: "start_ic_gvins_zmq",
    stop: "stop_ic_gvins_zmq",
  },
  sensor_recorder: {
    label: "recorder",
    start: "start_sensor_recorder",
    stop: "stop_sensor_recorder",
  },
};

function formatBytes(bytes) {
  const units = ["B", "KB", "MB", "GB", "TB"];
  let value = Number(bytes || 0);
  let index = 0;
  while (value >= 1024 && index < units.length - 1) {
    value /= 1024;
    index += 1;
  }
  return `${value.toFixed(index === 0 ? 0 : 1)} ${units[index]}`;
}

function setOutput(value) {
  if (typeof value === "string") {
    output.textContent = value;
    return;
  }
  output.textContent = JSON.stringify(value, null, 2);
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    headers: { "Content-Type": "application/json" },
    ...options,
  });
  const text = await response.text();
  const payload = text ? JSON.parse(text) : {};
  if (!response.ok) {
    throw new Error(payload.error || response.statusText);
  }
  return payload;
}

function statusText(process) {
  if (!process) return "unknown";
  if (process.children) {
    return Object.entries(process.children)
      .map(([name, child]) => `${name}:${child.running ? child.pids.join(",") : "off"}`)
      .join(" ");
  }
  return process.running ? `pid ${process.pids.join(",")}` : "stopped";
}

function setDot(id, process) {
  const dot = document.getElementById(id);
  dot.classList.remove("running", "partial");
  if (!process) return;
  if (process.children) {
    const values = Object.values(process.children);
    const running = values.filter((child) => child.running).length;
    if (running === values.length) dot.classList.add("running");
    else if (running > 0) dot.classList.add("partial");
    return;
  }
  if (process.running) dot.classList.add("running");
}

function isProcessActive(process) {
  if (!process) return false;
  if (process.children) {
    return Object.values(process.children).some((child) => child.running);
  }
  return Boolean(process.running);
}

function updateToggle(name, process) {
  const button = document.querySelector(`[data-toggle="${name}"]`);
  if (!button) return;
  const active = isProcessActive(process);
  const config = toggles[name];
  button.dataset.action = active ? config.stop : config.start;
  button.textContent = `${active ? "Stop" : "Start"} ${config.label}`;
  button.classList.toggle("start-toggle", !active);
  button.classList.toggle("stop-toggle", active);
  button.setAttribute("aria-pressed", active ? "true" : "false");
}

async function refreshStatus() {
  const status = await api("/api/status");
  const processes = status.processes;
  document.getElementById("sensorDataStatus").textContent = statusText(processes.sensor_data);
  document.getElementById("gvinsStatus").textContent = statusText(processes.ic_gvins_zmq);
  document.getElementById("recorderStatus").textContent = statusText(processes.sensor_recorder);
  setDot("sensorDataDot", processes.sensor_data);
  setDot("gvinsDot", processes.ic_gvins_zmq);
  setDot("recorderDot", processes.sensor_recorder);
  updateToggle("sensor_data", processes.sensor_data);
  updateToggle("ic_gvins_zmq", processes.ic_gvins_zmq);
  updateToggle("sensor_recorder", processes.sensor_recorder);
  document.getElementById("paths").textContent =
    `data: ${status.paths.data_root} | logs: ${status.paths.log_dir}`;
  document.getElementById("diskInfo").textContent =
    `Disk free ${formatBytes(status.disk.free)} / ${formatBytes(status.disk.total)}`;
}

function renderDatasets(datasets) {
  document.getElementById("datasetCount").textContent = `${datasets.length} datasets`;
  if (datasets.length === 0) {
    datasetRows.innerHTML = '<tr><td colspan="5" class="empty">No data directories.</td></tr>';
    return;
  }
  datasetRows.innerHTML = "";
  for (const row of datasets) {
    const tr = document.createElement("tr");
    const sensors = Object.entries(row.sensors)
      .map(([name, present]) => `<span class="tag ${present ? "present" : ""}">${name}</span>`)
      .join("");
    tr.innerHTML = `
      <td><strong>${row.name}</strong><br><span class="muted">${row.path}</span></td>
      <td>${row.mtime_text}</td>
      <td>${formatBytes(row.size_bytes)}</td>
      <td><div class="sensor-tags">${sensors}</div></td>
      <td>
        <div class="row-actions">
          <button class="secondary sanity-btn" data-name="${row.name}">Sanity Check</button>
          <button class="danger delete-btn" data-name="${row.name}">Delete</button>
        </div>
      </td>
    `;
    datasetRows.appendChild(tr);
  }
}

async function refreshDatasets() {
  const payload = await api("/api/datasets");
  renderDatasets(payload.datasets || []);
}

async function refreshAll() {
  try {
    await Promise.all([refreshStatus(), refreshDatasets()]);
  } catch (error) {
    setOutput(`Refresh failed: ${error.message}`);
  }
}

async function runAction(action, button) {
  button.disabled = true;
  try {
    const payload = await api("/api/actions", {
      method: "POST",
      body: JSON.stringify({ action }),
    });
    setOutput(payload);
    await refreshAll();
  } catch (error) {
    setOutput(`Action failed: ${error.message}`);
  } finally {
    button.disabled = false;
  }
}

async function runSanity(name, button) {
  button.disabled = true;
  setOutput(`Running sanity check for ${name}...`);
  try {
    const payload = await api(`/api/datasets/${encodeURIComponent(name)}/sanity`, {
      method: "POST",
      body: "{}",
    });
    setOutput(payload.output || payload);
  } catch (error) {
    setOutput(`Sanity check failed: ${error.message}`);
  } finally {
    button.disabled = false;
  }
}

async function deleteDataset(name, button) {
  if (!window.confirm(`Delete ${name}? This cannot be undone.`)) return;
  button.disabled = true;
  try {
    const payload = await api(`/api/datasets/${encodeURIComponent(name)}`, {
      method: "DELETE",
    });
    setOutput(payload);
    await refreshDatasets();
  } catch (error) {
    setOutput(`Delete failed: ${error.message}`);
  } finally {
    button.disabled = false;
  }
}

async function loadLog(name) {
  try {
    const payload = await api(`/api/logs/${encodeURIComponent(name)}`);
    setOutput(payload.text || "");
  } catch (error) {
    setOutput(`Load log failed: ${error.message}`);
  }
}

document.querySelectorAll("[data-toggle]").forEach((button) => {
  button.addEventListener("click", () => runAction(button.dataset.action, button));
});

document.querySelectorAll(".log-btn").forEach((button) => {
  button.addEventListener("click", () => loadLog(button.dataset.log));
});

datasetRows.addEventListener("click", (event) => {
  const target = event.target;
  if (!(target instanceof HTMLElement)) return;
  const name = target.dataset.name;
  if (!name) return;
  if (target.classList.contains("sanity-btn")) runSanity(name, target);
  if (target.classList.contains("delete-btn")) deleteDataset(name, target);
});

document.getElementById("refreshBtn").addEventListener("click", refreshAll);

refreshAll();
setInterval(refreshStatus, 3000);
