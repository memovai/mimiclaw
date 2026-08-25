const OFFSCREEN_PATH = "offscreen.html";
const DEFAULT_WS_URL = "ws://127.0.0.1:8765/ws";

let creatingOffscreen = null;

const planningState = {
  goal: "",
  steps: [],
  status: "idle",
  result: "",
  updatedAt: 0
};

function storageGetSafe(keys) {
  try {
    return chrome.storage.local.get(keys);
  } catch (_err) {
    return Promise.resolve({});
  }
}

function safeError(error) {
  if (!error) return "unknown_error";
  if (typeof error === "string") return error;
  return error.message || JSON.stringify(error);
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function canInject(tab) {
  const url = tab?.url || "";
  return url.startsWith("http://") || url.startsWith("https://");
}

function isAutomatableUrl(url) {
  return typeof url === "string" && (url.startsWith("http://") || url.startsWith("https://"));
}

function captureDomFallback(maxText = 4000, maxElements = 100) {
  function cssEscape(value) {
    if (window.CSS && typeof window.CSS.escape === "function") {
      return window.CSS.escape(value);
    }
    return String(value).replace(/([ #;?%&,.+*~':"!^$\[\]()=>|/@])/g, "\\$1");
  }

  function selectorFor(el) {
    if (!el || el.nodeType !== Node.ELEMENT_NODE) return "";
    if (el.id) return `#${cssEscape(el.id)}`;
    const testId = el.getAttribute("data-testid") || el.getAttribute("data-test");
    if (testId) return `[data-testid="${cssEscape(testId)}"]`;
    return el.tagName.toLowerCase();
  }

  function interactive(maxCount) {
    const query = [
      "a[href]",
      "button",
      "input",
      "textarea",
      "select",
      "[role='button']",
      "[role='textbox']",
      "[contenteditable='true']",
      "[onclick]"
    ].join(",");
    return Array.from(document.querySelectorAll(query)).slice(0, maxCount).map((el) => ({
      tag: el.tagName.toLowerCase(),
      selector: selectorFor(el),
      text: (el.innerText || el.value || "").trim().slice(0, 120),
      href: el.href || null,
      placeholder: el.getAttribute("placeholder") || null,
      contentEditable: Boolean(el.isContentEditable || el.getAttribute("contenteditable") === "true")
    }));
  }

  const text = (document.body?.innerText || "").replace(/\s+/g, " ").trim().slice(0, maxText);
  const snapshot = {
    url: location.href,
    title: document.title,
    textSnippet: text,
    interactiveElements: interactive(maxElements)
  };

  const host = location.hostname.toLowerCase();
  const isTwitter = host === "x.com" || host.endsWith(".x.com") || host === "twitter.com" || host.endsWith(".twitter.com");
  if (isTwitter) {
    const composer =
      document.querySelector("[data-testid='tweetTextarea_0'] [role='textbox']") ||
      document.querySelector("[data-testid='tweetTextarea_0']") ||
      document.querySelector("div[role='textbox'][data-testid='tweetTextarea_0']");
    const postBtn =
      document.querySelector("[data-testid='tweetButton']") ||
      document.querySelector("[data-testid='tweetButtonInline']");
    const isDisabled = Boolean(postBtn?.disabled || postBtn?.getAttribute("aria-disabled") === "true");
    snapshot.twitterCompose = {
      hasComposer: Boolean(composer),
      draftLength: ((composer?.innerText || composer?.textContent || "").trim() || "").length,
      hasPostButton: Boolean(postBtn),
      postButtonEnabled: Boolean(postBtn) && !isDisabled,
      postButtonSelector: postBtn?.getAttribute("data-testid") ? `[data-testid="${postBtn.getAttribute("data-testid")}"]` : null
    };
  }

  return snapshot;
}

function executeActionFallback(action) {
  const name = action?.name;
  if (!name) throw new Error("Missing action.name");

  function findByText(text) {
    const target = (text || "").trim().toLowerCase();
    if (!target) return null;
    const nodes = Array.from(document.querySelectorAll("button, a[href], [role='button'], input[type='button'], input[type='submit']"));
    return nodes.find((el) => {
      const v = (el.innerText || el.value || "").trim().toLowerCase();
      return v === target || v.includes(target);
    });
  }

  if (name === "click") {
    const el = action.selector ? document.querySelector(action.selector) : findByText(action.text);
    if (el) {
      el.click();
      return { ok: true, action: "click", selector: action.selector || null, text: action.text || null };
    }

    const host = location.hostname.toLowerCase();
    const isTwitter = host === "x.com" || host.endsWith(".x.com") || host === "twitter.com" || host.endsWith(".twitter.com");
    if (isTwitter) {
      const composer =
        document.querySelector("[data-testid='tweetTextarea_0'] [role='textbox']") ||
        document.querySelector("[data-testid='tweetTextarea_0']") ||
        document.querySelector("div[role='textbox'][data-testid='tweetTextarea_0']");
      if (composer) {
        return { ok: true, action: "click", selector: action.selector || null, text: action.text || "compose-already-open" };
      }

      const openBtn =
        document.querySelector("[data-testid='SideNav_NewTweet_Button']") ||
        document.querySelector("[data-testid='FloatingActionButtons_AddTweet_Button']") ||
        document.querySelector("[data-testid='tweetButtonInline']") ||
        document.querySelector("a[href='/compose/post']") ||
        document.querySelector("a[href='/compose/tweet']") ||
        Array.from(document.querySelectorAll("a,button,[role='button']")).find((el2) => {
          const t = (el2.innerText || el2.getAttribute("aria-label") || "").trim().toLowerCase();
          return t === "post" || t === "tweet" || t.includes("new post") || t.includes("new tweet");
        });
      if (openBtn) {
        openBtn.click();
        return { ok: true, action: "click", selector: action.selector || null, text: action.text || "open-composer" };
      }

      const postBtn =
        document.querySelector("[data-testid='tweetButton']") ||
        document.querySelector("[data-testid='tweetButtonInline']") ||
        Array.from(document.querySelectorAll("button")).find((b) => /^(post|tweet|发布|發佈)$/i.test((b.innerText || "").trim()));
      if (postBtn && !(postBtn.disabled || postBtn.getAttribute("aria-disabled") === "true")) {
        postBtn.click();
        return { ok: true, action: "click", selector: "[data-testid='tweetButton']", text: "Post" };
      }
    }

    throw new Error("click target not found");
  }

  if (name === "fill") {
    function isTwitterHost() {
      const host = location.hostname.toLowerCase();
      return host === "x.com" || host.endsWith(".x.com") || host === "twitter.com" || host.endsWith(".twitter.com");
    }
    function findTwitterComposerInput() {
      return (
        document.querySelector("[data-testid='tweetTextarea_0'] [role='textbox']") ||
        document.querySelector("[data-testid='tweetTextarea_0']") ||
        document.querySelector("div[role='textbox'][data-testid='tweetTextarea_0']") ||
        document.querySelector("div[role='textbox'][aria-label*='Post']") ||
        document.querySelector("div[role='textbox'][aria-label*='What is happening']") ||
        null
      );
    }
    function openTwitterComposer() {
      const btn =
        document.querySelector("[data-testid='SideNav_NewTweet_Button']") ||
        document.querySelector("[data-testid='FloatingActionButtons_AddTweet_Button']") ||
        document.querySelector("[data-testid='tweetButtonInline']") ||
        document.querySelector("a[href='/compose/post']") ||
        document.querySelector("a[href='/compose/tweet']") ||
        Array.from(document.querySelectorAll("a,button,[role='button']")).find((el) => {
          const t = (el.innerText || el.getAttribute("aria-label") || "").trim().toLowerCase();
          return t === "post" || t === "tweet" || t.includes("new post") || t.includes("new tweet");
        });
      if (btn) {
        btn.click();
        return true;
      }
      return false;
    }

    let el = null;
    if (action.selector) {
      el = document.querySelector(action.selector);
    }
    if (!el && isTwitterHost()) {
      el = findTwitterComposerInput();
      if (!el) {
        openTwitterComposer();
        el = findTwitterComposerInput();
      }
    }
    if (!el) {
      el =
        document.querySelector("textarea") ||
        document.querySelector("input[type='text']") ||
        document.querySelector("[role='textbox']") ||
        document.querySelector("[contenteditable='true']");
    }
    if (!el) throw new Error("fill target not found");
    el.focus();
    const value = action.value || "";
    const writeValue = isTwitterHost() ? value.slice(0, 200) : value;
    if (el.isContentEditable || el.getAttribute("contenteditable") === "true") {
      // Overwrite mode for Draft.js/contenteditable editors.
      const selection = window.getSelection();
      const range = document.createRange();
      range.selectNodeContents(el);
      selection.removeAllRanges();
      selection.addRange(range);

      const replaced = typeof document.execCommand === "function" ? document.execCommand("insertText", false, writeValue) : false;
      if (!replaced) {
        el.textContent = writeValue;
      }

      let current = (el.innerText || el.textContent || "").trim();
      if (writeValue && current.length === 0) {
        el.textContent = "";
        for (const ch of writeValue) {
          el.dispatchEvent(new InputEvent("beforeinput", { bubbles: true, cancelable: true, data: ch, inputType: "insertText" }));
          const ok = typeof document.execCommand === "function" ? document.execCommand("insertText", false, ch) : false;
          if (!ok) el.textContent += ch;
        }
        current = (el.innerText || el.textContent || "").trim();
      }

      el.dispatchEvent(new InputEvent("beforeinput", { bubbles: true, cancelable: true, data: writeValue, inputType: "insertText" }));
      el.dispatchEvent(new InputEvent("input", { bubbles: true, data: writeValue, inputType: "insertText" }));
      el.dispatchEvent(new KeyboardEvent("keyup", { bubbles: true, key: " " }));
      if (isTwitterHost()) {
        el.dispatchEvent(new KeyboardEvent("keydown", { bubbles: true, key: "Backspace" }));
        el.dispatchEvent(new KeyboardEvent("keyup", { bubbles: true, key: "Backspace" }));
      }
    } else {
      el.value = writeValue;
      el.dispatchEvent(new Event("input", { bubbles: true }));
      el.dispatchEvent(new Event("change", { bubbles: true }));
    }
    const finalText = (el.innerText || el.textContent || el.value || "").trim();
    return { ok: true, action: "fill", selector: action.selector, length: finalText.length };
  }

  if (name === "scroll") {
    const top = Number(action.top || 0);
    window.scrollTo({ top, behavior: action.behavior || "smooth" });
    return { ok: true, action: "scroll", top };
  }

  throw new Error(`Unsupported content action: ${name}`);
}

async function runScript(tabId, func, args = []) {
  const injected = await chrome.scripting.executeScript({ target: { tabId }, func, args });
  return injected?.[0]?.result;
}

async function getActiveTab() {
  const activeTabs = await chrome.tabs.query({ active: true, currentWindow: true });
  if (activeTabs && activeTabs[0] && isAutomatableUrl(activeTabs[0].url || "")) {
    return activeTabs[0];
  }

  const currentTabs = await chrome.tabs.query({ currentWindow: true });
  const candidate = (currentTabs || []).find((t) => isAutomatableUrl(t.url || ""));
  if (candidate) {
    await chrome.tabs.update(candidate.id, { active: true });
    return candidate;
  }

  // If user is currently on restricted URLs (chrome://, devtools://),
  // create a normal page so DOM capture and actions can proceed.
  const created = await chrome.tabs.create({ url: "https://www.google.com", active: true });
  await waitForTabComplete(created.id).catch(() => {});
  return created;
}

async function sendMessageToTab(tabId, message) {
  return chrome.tabs.sendMessage(tabId, message);
}

async function waitForTabComplete(tabId, timeoutMs = 12000) {
  const tab = await chrome.tabs.get(tabId);
  if (tab.status === "complete") return;

  await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      chrome.tabs.onUpdated.removeListener(onUpdated);
      reject(new Error("wait_for_tab_complete_timeout"));
    }, timeoutMs);

    function onUpdated(updatedTabId, changeInfo) {
      if (updatedTabId !== tabId) return;
      if (changeInfo.status === "complete") {
        clearTimeout(timeout);
        chrome.tabs.onUpdated.removeListener(onUpdated);
        resolve();
      }
    }

    chrome.tabs.onUpdated.addListener(onUpdated);
  });
}

async function ensureContentScript(tab) {
  if (!canInject(tab)) return;
  try {
    await chrome.scripting.executeScript({ target: { tabId: tab.id }, files: ["content.js"] });
  } catch (_err) {}
}

async function handleGetDomSnapshot(data) {
  const tab = await getActiveTab();
  const payload = {
    type: "GET_DOM_SNAPSHOT",
    maxText: data.maxText || 4000,
    maxElements: data.maxElements || 100
  };

  try {
    const result = await sendMessageToTab(tab.id, payload);
    return { tabId: tab.id, ...result };
  } catch (err) {
    const msg = safeError(err);
    if (msg.includes("Receiving end does not exist")) {
      try {
        await ensureContentScript(tab);
        await sleep(250);
        const retryResult = await sendMessageToTab(tab.id, payload);
        return { tabId: tab.id, ...retryResult };
      } catch (_retryErr) {
        const fallback = await runScript(tab.id, captureDomFallback, [payload.maxText, payload.maxElements]);
        return { tabId: tab.id, ...fallback };
      }
    }
    throw err;
  }
}

async function handleAction(data) {
  const action = data.action || {};
  const actionName = action.name;
  if (!actionName) {
    throw new Error("Missing action.name");
  }

  const tab = await getActiveTab();

  if (actionName === "navigate") {
    if (!action.url) throw new Error("navigate action needs url");
    await chrome.tabs.update(tab.id, { url: action.url });
    await waitForTabComplete(tab.id).catch(() => {});
    await ensureContentScript(await chrome.tabs.get(tab.id));
    return { ok: true, action: "navigate", url: action.url };
  }

  if (actionName === "back") {
    if (!chrome.tabs.goBack) throw new Error("chrome.tabs.goBack is not available");
    await chrome.tabs.goBack(tab.id);
    await waitForTabComplete(tab.id).catch(() => {});
    await ensureContentScript(await chrome.tabs.get(tab.id));
    return { ok: true, action: "back" };
  }

  if (actionName === "forward") {
    if (!chrome.tabs.goForward) throw new Error("chrome.tabs.goForward is not available");
    await chrome.tabs.goForward(tab.id);
    await waitForTabComplete(tab.id).catch(() => {});
    await ensureContentScript(await chrome.tabs.get(tab.id));
    return { ok: true, action: "forward" };
  }

  const payload = { type: "EXECUTE_ACTION", action };
  try {
    const result = await sendMessageToTab(tab.id, payload);
    return { tabId: tab.id, ...result };
  } catch (err) {
    const msg = safeError(err);
    if (msg.includes("Receiving end does not exist")) {
      try {
        await ensureContentScript(tab);
        await sleep(250);
        const retryResult = await sendMessageToTab(tab.id, payload);
        return { tabId: tab.id, ...retryResult };
      } catch (_retryErr) {
        const fallback = await runScript(tab.id, executeActionFallback, [action]);
        return { tabId: tab.id, ...fallback };
      }
    }
    throw err;
  }
}

async function runBrowserCommand(command) {
  const requestId = command?.request_id;
  const cmd = command?.type;
  if (!requestId || !cmd) {
    return { type: "command_result", request_id: requestId || "", ok: false, error: "invalid_command_payload" };
  }

  try {
    let result;
    if (cmd === "get_dom_snapshot") {
      result = await handleGetDomSnapshot(command);
    } else if (cmd === "execute_action") {
      result = await handleAction(command);
    } else {
      throw new Error(`Unknown command type: ${cmd}`);
    }
    return { type: "command_result", request_id: requestId, ok: true, result };
  } catch (err) {
    return { type: "command_result", request_id: requestId, ok: false, error: safeError(err) };
  }
}

function updatePlanningState(payload) {
  const event = payload?.event;
  if (!event) return;

  planningState.updatedAt = Date.now();

  if (event === "goal") {
    planningState.goal = payload.goal || "";
    planningState.steps = [];
    planningState.status = "running";
    planningState.result = "";
  } else if (event === "step") {
    planningState.status = "running";
    planningState.steps.push({
      step: payload.step || planningState.steps.length + 1,
      action: payload.action || "",
      reason: payload.reason || "",
      at: Date.now()
    });
    if (planningState.steps.length > 30) {
      planningState.steps = planningState.steps.slice(-30);
    }
  } else if (event === "done") {
    planningState.status = payload.ok ? "done" : "error";
    planningState.result = payload.answer || payload.error || "";
  }
}

async function broadcastPlanningState() {
  const tabs = await chrome.tabs.query({});
  await Promise.all(
    tabs.map((tab) => chrome.tabs.sendMessage(tab.id, { type: "PLAN_UPDATE", payload: planningState }).catch(() => {}))
  );
}

async function hasOffscreenDocument() {
  if (!chrome.runtime.getContexts) return false;
  const offscreenUrl = chrome.runtime.getURL(OFFSCREEN_PATH);
  const contexts = await chrome.runtime.getContexts({
    contextTypes: ["OFFSCREEN_DOCUMENT"],
    documentUrls: [offscreenUrl]
  });
  return contexts.length > 0;
}

async function ensureOffscreenDocument() {
  if (await hasOffscreenDocument()) return;
  if (creatingOffscreen) {
    await creatingOffscreen;
    return;
  }

  creatingOffscreen = chrome.offscreen.createDocument({
    url: OFFSCREEN_PATH,
    reasons: ["WORKERS"],
    justification: "Keep a persistent WebSocket bridge independent from service worker lifecycle."
  });

  try {
    await creatingOffscreen;
  } finally {
    creatingOffscreen = null;
  }
}

async function sendToOffscreen(message) {
  await ensureOffscreenDocument();
  return chrome.runtime.sendMessage({ __target: "offscreen", ...message });
}

async function getWsConfigUrl() {
  const data = await storageGetSafe(["wsUrlConfig"]);
  return data.wsUrlConfig || DEFAULT_WS_URL;
}

async function getWsEnabled() {
  const data = await storageGetSafe(["wsEnabled"]);
  return data.wsEnabled !== false;
}

chrome.runtime.onInstalled.addListener(async () => {
  const wsUrl = await getWsConfigUrl();
  const enabled = await getWsEnabled();
  ensureOffscreenDocument().then(() => sendToOffscreen({ type: "OFFSCREEN_CONNECT", wsUrl, enabled })).catch(() => {});
});

chrome.runtime.onStartup.addListener(async () => {
  const wsUrl = await getWsConfigUrl();
  const enabled = await getWsEnabled();
  ensureOffscreenDocument().then(() => sendToOffscreen({ type: "OFFSCREEN_CONNECT", wsUrl, enabled })).catch(() => {});
});

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.__target === "offscreen") return false;

  if (message?.__target === "background" && message?.type === "RUN_BROWSER_COMMAND") {
    runBrowserCommand(message.command || {})
      .then((result) => sendResponse(result))
      .catch((err) => sendResponse({ type: "command_result", request_id: message?.command?.request_id || "", ok: false, error: safeError(err) }));
    return true;
  }

  if (message?.__target === "background" && message?.type === "AGENT_STATUS_BROADCAST") {
    updatePlanningState(message.payload || {});
    broadcastPlanningState().then(() => sendResponse({ ok: true })).catch(() => sendResponse({ ok: false }));
    return true;
  }

  if (message?.type === "GET_PLAN_STATE") {
    sendResponse({ ok: true, payload: planningState });
    return true;
  }

  if (message?.type === "CLEAR_PLAN_STATE") {
    planningState.goal = "";
    planningState.steps = [];
    planningState.status = "idle";
    planningState.result = "";
    planningState.updatedAt = Date.now();
    broadcastPlanningState().then(() => sendResponse({ ok: true })).catch(() => sendResponse({ ok: false }));
    return true;
  }

  if (message?.type === "SET_WS_URL") {
    const wsUrl = (message.wsUrl || "").trim() || DEFAULT_WS_URL;
    chrome.storage.local
      .set({ wsUrlConfig: wsUrl })
      .then(() => sendToOffscreen({ type: "OFFSCREEN_SET_URL", wsUrl }))
      .then((status) => sendResponse({ ok: true, ...status }))
      .catch((err) => sendResponse({ ok: false, error: safeError(err) }));
    return true;
  }

  if (message?.type === "SET_WS_ENABLED") {
    const enabled = Boolean(message.enabled);
    chrome.storage.local
      .set({ wsEnabled: enabled })
      .then(() => sendToOffscreen({ type: "OFFSCREEN_SET_ENABLED", enabled }))
      .then((status) => sendResponse({ ok: true, ...status }))
      .catch((err) => sendResponse({ ok: false, error: safeError(err) }));
    return true;
  }

  if (message?.type === "PING_BACKGROUND") {
    Promise.all([getWsConfigUrl(), getWsEnabled()])
      .then(([wsUrl, enabled]) => sendToOffscreen({ type: "OFFSCREEN_CONNECT", wsUrl, enabled }))
      .then(() => sendToOffscreen({ type: "OFFSCREEN_STATUS" }))
      .then((status) => sendResponse({ ok: true, ...status }))
      .catch(async () => {
        const fallback = await storageGetSafe(["wsConnected", "wsLastError", "wsUrl", "wsUrlConfig", "wsEnabled"]);
        sendResponse({
          ok: true,
          connected: Boolean(fallback.wsConnected),
          lastError: fallback.wsLastError || "offscreen_unavailable",
          wsUrl: fallback.wsUrl || fallback.wsUrlConfig || DEFAULT_WS_URL,
          wsUrlConfig: fallback.wsUrlConfig || DEFAULT_WS_URL,
          enabled: fallback.wsEnabled !== false
        });
      });
    return true;
  }

  if (message?.type === "GET_WS_STATUS") {
    sendToOffscreen({ type: "OFFSCREEN_STATUS" })
      .then(async (status) => {
        const cfg = await storageGetSafe(["wsUrlConfig"]);
        sendResponse({ ok: true, ...status, wsUrlConfig: cfg.wsUrlConfig || DEFAULT_WS_URL });
      })
      .catch(async () => {
        const fallback = await storageGetSafe(["wsConnected", "wsLastError", "wsUrl", "wsUrlConfig", "wsEnabled"]);
        sendResponse({
          ok: true,
          connected: Boolean(fallback.wsConnected),
          lastError: fallback.wsLastError || "offscreen_unavailable",
          wsUrl: fallback.wsUrl || fallback.wsUrlConfig || DEFAULT_WS_URL,
          wsUrlConfig: fallback.wsUrlConfig || DEFAULT_WS_URL,
          enabled: fallback.wsEnabled !== false
        });
      });
    return true;
  }

  return false;
});

Promise.all([getWsConfigUrl(), getWsEnabled()])
  .then(([wsUrl, enabled]) => ensureOffscreenDocument().then(() => sendToOffscreen({ type: "OFFSCREEN_CONNECT", wsUrl, enabled })))
  .catch(() => {});
