const statusEl = document.getElementById("status");
const detailEl = document.getElementById("detail");
const wsIpEl = document.getElementById("wsIp");
const reconnectBtn = document.getElementById("reconnect");
const wsEnabledEl = document.getElementById("wsEnabled");
let isToggling = false;

function wsUrlFromIp(ip) {
  const clean = String(ip || "").trim();
  const safeIp = clean || "127.0.0.1";
  return `ws://${safeIp}:8765/ws`;
}

function ipFromWsUrl(wsUrl) {
  const m = String(wsUrl || "").match(/^wss?:\/\/([^/:]+)(?::\d+)?\//i);
  return m?.[1] || "127.0.0.1";
}

async function refreshStatus() {
  let statusData = null;
  try {
    statusData = await chrome.runtime.sendMessage({ type: "GET_WS_STATUS" });
  } catch (_err) {}

  const fallback = await chrome.storage.local.get(["wsConnected", "wsLastError", "wsUrl", "wsUrlConfig", "wsEnabled"]);
  const enabled = Boolean(statusData?.enabled ?? (fallback.wsEnabled !== false));
  const ok = Boolean(statusData?.connected ?? fallback.wsConnected);
  const errorText = statusData?.lastError || fallback.wsLastError || "";
  const currentWs = statusData?.wsUrl || fallback.wsUrl || fallback.wsUrlConfig || "ws://127.0.0.1:8765/ws";
  const configWs = statusData?.wsUrlConfig || fallback.wsUrlConfig || currentWs;

  if (!isToggling) {
    wsEnabledEl.checked = enabled;
  }
  wsIpEl.disabled = !enabled;
  reconnectBtn.disabled = !enabled;

  if (!enabled) {
    statusEl.textContent = "Status: disabled";
    statusEl.style.color = "#6c7a89";
    detailEl.textContent = "Listener is off. No WebSocket listening.";
  } else {
    statusEl.textContent = `Status: ${ok ? "connected" : "disconnected"}`;
    statusEl.style.color = ok ? "#0b8a42" : "#b02a37";
    detailEl.textContent = ok ? `Connected: ${currentWs}` : `Reason: ${errorText || "not_connected_yet"}`;
  }

  if (!wsIpEl.value) {
    wsIpEl.value = ipFromWsUrl(configWs);
  }
}

reconnectBtn.addEventListener("click", async () => {
  const wsUrl = wsUrlFromIp(wsIpEl.value);
  await chrome.runtime.sendMessage({ type: "SET_WS_URL", wsUrl }).catch(() => {});
  await chrome.runtime.sendMessage({ type: "PING_BACKGROUND" }).catch(() => {});
  await refreshStatus();
});

wsEnabledEl.addEventListener("change", async () => {
  isToggling = true;
  const enabled = Boolean(wsEnabledEl.checked);
  await chrome.runtime.sendMessage({ type: "SET_WS_ENABLED", enabled }).catch(() => {});
  if (enabled) {
    await chrome.runtime.sendMessage({ type: "PING_BACKGROUND" }).catch(() => {});
  }
  isToggling = false;
  await refreshStatus();
});

refreshStatus();
setInterval(refreshStatus, 1200);
