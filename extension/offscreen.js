const DEFAULT_WS_URL = "ws://127.0.0.1:8765/ws";
const RECONNECT_MS = 2000;
const HEARTBEAT_MS = 10000;
const HEARTBEAT_TIMEOUT_MS = 30000;
const REREGISTER_MS = 45000;
const MAX_CACHED_RESULTS = 200;

let socket = null;
let reconnectTimer = null;
let heartbeatTimer = null;
let registerTimer = null;
let connected = false;
let lastError = "";
let lastPongAt = 0;
let wsUrl = DEFAULT_WS_URL;
let pendingRequests = new Map();
let completedResults = new Map();
let listenerEnabled = true;

function log(...args) {
  console.log("[mimibrowser-offscreen]", ...args);
}

function storageSetSafe(obj) {
  try {
    const maybePromise = chrome.storage.local.set(obj);
    if (maybePromise && typeof maybePromise.catch === "function") {
      maybePromise.catch(() => {});
    }
  } catch (_err) {}
}

function safeError(error) {
  if (!error) return "unknown_error";
  if (typeof error === "string") return error;
  return error.message || JSON.stringify(error);
}

function setConnected(value, errorText = "") {
  connected = value;
  lastError = errorText || "";
  storageSetSafe({ wsConnected: value, wsLastError: lastError, wsUrl, wsEnabled: listenerEnabled });
}

function sendRaw(payload) {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    throw new Error("WebSocket is not connected");
  }
  socket.send(JSON.stringify(payload));
}

function sendCommandResult(message) {
  try {
    sendRaw(message);
  } catch (err) {
    log("sendCommandResult failed", safeError(err));
  }
}

function pruneCompletedResults() {
  while (completedResults.size > MAX_CACHED_RESULTS) {
    const firstKey = completedResults.keys().next().value;
    if (!firstKey) break;
    completedResults.delete(firstKey);
  }
}

async function runCommandInBackground(data) {
  return chrome.runtime.sendMessage({ __target: "background", type: "RUN_BROWSER_COMMAND", command: data });
}

async function emitAgentStatus(payload) {
  return chrome.runtime.sendMessage({ __target: "background", type: "AGENT_STATUS_BROADCAST", payload });
}

async function executeCommandWithDedup(data) {
  const requestId = data.request_id;
  if (!requestId) {
    throw new Error("missing_request_id");
  }

  if (completedResults.has(requestId)) {
    return completedResults.get(requestId);
  }

  if (pendingRequests.has(requestId)) {
    return pendingRequests.get(requestId);
  }

  const commandPromise = runCommandInBackground(data)
    .catch((err) => ({ type: "command_result", request_id: requestId, ok: false, error: safeError(err) }))
    .finally(() => {
      pendingRequests.delete(requestId);
    });

  pendingRequests.set(requestId, commandPromise);

  const response = await commandPromise;
  completedResults.set(requestId, response);
  pruneCompletedResults();
  return response;
}

function sendRegister() {
  try {
    sendRaw({ type: "register", role: "extension", ua: navigator.userAgent, ts: Date.now() });
  } catch (_err) {}
}

function startHeartbeat() {
  stopHeartbeat();

  heartbeatTimer = setInterval(() => {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      return;
    }

    const now = Date.now();
    if (lastPongAt && now - lastPongAt > HEARTBEAT_TIMEOUT_MS) {
      log("heartbeat timeout, force reconnect");
      socket.close(4000, "heartbeat_timeout");
      return;
    }

    try {
      sendRaw({ type: "ping", ts: now });
    } catch (_err) {}
  }, HEARTBEAT_MS);

  registerTimer = setInterval(() => {
    if (socket && socket.readyState === WebSocket.OPEN) {
      sendRegister();
    }
  }, REREGISTER_MS);
}

function stopHeartbeat() {
  if (heartbeatTimer) {
    clearInterval(heartbeatTimer);
    heartbeatTimer = null;
  }
  if (registerTimer) {
    clearInterval(registerTimer);
    registerTimer = null;
  }
}

function scheduleReconnect() {
  if (!listenerEnabled) return;
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
  }
  reconnectTimer = setTimeout(connect, RECONNECT_MS);
}

function connect() {
  if (!listenerEnabled) {
    setConnected(false, "listener_disabled");
    return;
  }
  if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) {
    return;
  }

  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }

  socket = new WebSocket(wsUrl);

  socket.addEventListener("open", () => {
    lastPongAt = Date.now();
    setConnected(true, "");
    sendRegister();
    startHeartbeat();
    log("Connected", wsUrl);
  });

  socket.addEventListener("message", async (event) => {
    let data;
    try {
      data = JSON.parse(event.data);
    } catch (_err) {
      return;
    }

    if (data.type === "pong") {
      lastPongAt = Date.now();
      storageSetSafe({ wsLastPongAt: lastPongAt });
      return;
    }

    if (data.type === "ping") {
      sendRaw({ type: "pong", ts: Date.now() });
      return;
    }

    if (data.type === "agent_status") {
      await emitAgentStatus(data).catch(() => {});
      return;
    }

    if (!data.request_id || !data.type) {
      return;
    }

    if (!listenerEnabled) {
      sendCommandResult({
        type: "command_result",
        request_id: data.request_id,
        ok: false,
        error: "listener_disabled"
      });
      return;
    }

    const response = await executeCommandWithDedup(data);
    sendCommandResult(response);
  });

  socket.addEventListener("close", (event) => {
    stopHeartbeat();
    const reason = `closed(code=${event.code}, reason=${event.reason || "none"})`;
    setConnected(false, reason);
    scheduleReconnect();
    log("Socket closed", reason);
  });

  socket.addEventListener("error", () => {
    setConnected(false, "socket_error");
  });
}

function reconnect() {
  try {
    if (socket && socket.readyState <= WebSocket.OPEN) {
      socket.close(4100, "reconfigure");
    }
  } catch (_err) {}
  stopHeartbeat();
  scheduleReconnect();
}

function closeSocket(reason = "listener_disabled") {
  stopHeartbeat();
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }
  try {
    if (socket && socket.readyState <= WebSocket.OPEN) {
      socket.close(4101, reason);
    }
  } catch (_err) {}
  socket = null;
  setConnected(false, reason);
}

async function loadInitialConfig() {
  const data = await chrome.storage.local.get(["wsUrlConfig", "wsEnabled"]);
  wsUrl = (data.wsUrlConfig || DEFAULT_WS_URL).trim();
  listenerEnabled = data.wsEnabled !== false;
  storageSetSafe({ wsUrl, wsEnabled: listenerEnabled });
}

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.__target !== "offscreen") {
    return false;
  }

  if (message?.type === "OFFSCREEN_CONNECT") {
    if (typeof message.enabled === "boolean") {
      listenerEnabled = message.enabled;
      storageSetSafe({ wsEnabled: listenerEnabled });
    }
    if (message.wsUrl && typeof message.wsUrl === "string") {
      wsUrl = message.wsUrl.trim() || DEFAULT_WS_URL;
      storageSetSafe({ wsUrlConfig: wsUrl, wsUrl });
    }
    if (!listenerEnabled) {
      closeSocket("listener_disabled");
    } else {
      connect();
    }
    sendResponse({ connected, lastError, wsUrl, lastPongAt, enabled: listenerEnabled });
    return true;
  }

  if (message?.type === "OFFSCREEN_SET_URL") {
    wsUrl = (message.wsUrl || "").trim() || DEFAULT_WS_URL;
    storageSetSafe({ wsUrlConfig: wsUrl, wsUrl });
    if (listenerEnabled) {
      reconnect();
    } else {
      closeSocket("listener_disabled");
    }
    sendResponse({ connected, lastError, wsUrl, lastPongAt, enabled: listenerEnabled });
    return true;
  }

  if (message?.type === "OFFSCREEN_SET_ENABLED") {
    listenerEnabled = Boolean(message.enabled);
    storageSetSafe({ wsEnabled: listenerEnabled });
    if (!listenerEnabled) {
      closeSocket("listener_disabled");
    } else {
      connect();
    }
    sendResponse({ connected, lastError, wsUrl, lastPongAt, enabled: listenerEnabled });
    return true;
  }

  if (message?.type === "OFFSCREEN_STATUS") {
    sendResponse({ connected, lastError, wsUrl, lastPongAt, enabled: listenerEnabled });
    return true;
  }

  sendResponse({ connected, lastError, wsUrl, lastPongAt, enabled: listenerEnabled });
  return true;
});

loadInitialConfig()
  .then(() => {
    if (listenerEnabled) connect();
    else closeSocket("listener_disabled");
  })
  .catch(connect);
