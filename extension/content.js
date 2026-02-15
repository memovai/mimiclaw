function cssEscape(value) {
  if (window.CSS && typeof window.CSS.escape === "function") {
    return window.CSS.escape(value);
  }
  return value.replace(/([ #;?%&,.+*~':"!^$\[\]()=>|/@])/g, "\\$1");
}

function elementSelector(el) {
  if (!el || el.nodeType !== Node.ELEMENT_NODE) return "";

  if (el.id) {
    return `#${cssEscape(el.id)}`;
  }

  const testId = el.getAttribute("data-testid") || el.getAttribute("data-test");
  if (testId) {
    return `[data-testid=\"${cssEscape(testId)}\"]`;
  }

  const name = el.getAttribute("name");
  if (name && ["input", "textarea", "select"].includes(el.tagName.toLowerCase())) {
    return `${el.tagName.toLowerCase()}[name=\"${cssEscape(name)}\"]`;
  }

  const className = (el.className || "")
    .toString()
    .split(/\s+/)
    .filter(Boolean)
    .slice(0, 2)
    .map((c) => `.${cssEscape(c)}`)
    .join("");

  if (className) {
    return `${el.tagName.toLowerCase()}${className}`;
  }

  return el.tagName.toLowerCase();
}

function visibleText(maxText = 4000) {
  const text = (document.body?.innerText || "").replace(/\s+/g, " ").trim();
  return text.slice(0, maxText);
}

function elementBrief(el) {
  return {
    tag: el.tagName.toLowerCase(),
    selector: elementSelector(el),
    text: (el.innerText || el.value || "").trim().slice(0, 120),
    href: el.href || null,
    ariaLabel: el.getAttribute("aria-label") || null,
    role: el.getAttribute("role") || null,
    placeholder: el.getAttribute("placeholder") || null,
    contentEditable: Boolean(el.isContentEditable || el.getAttribute("contenteditable") === "true")
  };
}

function getInteractiveElements(maxElements = 100) {
  const selector = [
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
  const nodes = Array.from(document.querySelectorAll(selector));
  return nodes.slice(0, maxElements).map(elementBrief);
}

function domSnapshot(maxText, maxElements) {
  const snapshot = {
    url: location.href,
    title: document.title,
    textSnippet: visibleText(maxText),
    interactiveElements: getInteractiveElements(maxElements)
  };

  if (isTwitterHost()) {
    const composer = findTwitterComposerInput();
    const postBtn =
      document.querySelector("[data-testid='tweetButton']") ||
      document.querySelector("[data-testid='tweetButtonInline']");
    const isDisabled = Boolean(
      postBtn?.disabled ||
        postBtn?.getAttribute("aria-disabled") === "true"
    );
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

function findTwitterPostButton() {
  return (
    document.querySelector("[data-testid='tweetButton']") ||
    document.querySelector("[data-testid='tweetButtonInline']") ||
    Array.from(document.querySelectorAll("button")).find((b) => /^(post|tweet|发布|發佈)$/i.test((b.innerText || "").trim())) ||
    null
  );
}

function clickTwitterPostIfReady() {
  const btn = findTwitterPostButton();
  if (!btn) return false;
  const disabled = Boolean(btn.disabled || btn.getAttribute("aria-disabled") === "true");
  if (disabled) return false;
  btn.click();
  return true;
}

function domSnapshotLegacy(maxText, maxElements) {
  return {
    url: location.href,
    title: document.title,
    textSnippet: visibleText(maxText),
    interactiveElements: getInteractiveElements(maxElements)
  };
}

function findByText(text) {
  if (!text) return null;
  const normalized = text.trim().toLowerCase();
  if (!normalized) return null;

  const candidates = Array.from(
    document.querySelectorAll("button, a[href], [role='button'], input[type='button'], input[type='submit']")
  );

  return candidates.find((el) => {
    const value = (el.innerText || el.value || "").trim().toLowerCase();
    return value === normalized || value.includes(normalized);
  });
}

function isTwitterHost() {
  const host = location.hostname.toLowerCase();
  return host === "x.com" || host.endsWith(".x.com") || host === "twitter.com" || host.endsWith(".twitter.com");
}

function findGenericFillTarget() {
  return (
    document.querySelector("textarea") ||
    document.querySelector("input[type='text']") ||
    document.querySelector("[role='textbox']") ||
    document.querySelector("[contenteditable='true']")
  );
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

function executeAction(action) {
  const name = action?.name;
  if (!name) {
    throw new Error("Missing action.name");
  }

  if (name === "click") {
    const el = action.selector ? document.querySelector(action.selector) : findByText(action.text);
    if (el) {
      el.click();
      return { ok: true, action: "click", selector: action.selector || null, text: action.text || null };
    }
    if (isTwitterHost()) {
      const composer = findTwitterComposerInput();
      if (composer) {
        // Composer already open, treat as successful click step.
        return { ok: true, action: "click", selector: action.selector || null, text: action.text || "compose-already-open" };
      }
      if (openTwitterComposer()) {
        return { ok: true, action: "click", selector: action.selector || null, text: action.text || "open-composer" };
      }
    }
    if (isTwitterHost() && clickTwitterPostIfReady()) {
      return { ok: true, action: "click", selector: action.selector || "[data-testid='tweetButton']", text: action.text || "Post" };
    }
    throw new Error("click target not found");
  }

  if (name === "fill") {
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
      el = findGenericFillTarget();
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
    const behavior = action.behavior || "smooth";
    window.scrollTo({ top, behavior });
    return { ok: true, action: "scroll", top, behavior };
  }

  throw new Error(`Unsupported content action: ${name}`);
}

const OVERLAY_ID = "mimibrowser-plan-overlay";
let overlayHost = null;
let overlayRoot = null;
let autoHideTimer = null;

function ensureOverlay() {
  if (overlayHost && overlayRoot) {
    return overlayRoot;
  }

  overlayHost = document.getElementById(OVERLAY_ID);
  if (!overlayHost) {
    overlayHost = document.createElement("div");
    overlayHost.id = OVERLAY_ID;
    overlayHost.style.position = "fixed";
    overlayHost.style.right = "16px";
    overlayHost.style.top = "16px";
    overlayHost.style.zIndex = "2147483647";
    overlayHost.style.pointerEvents = "auto";
    document.documentElement.appendChild(overlayHost);
  }

  const shadow = overlayHost.shadowRoot || overlayHost.attachShadow({ mode: "open" });
  shadow.innerHTML = `
    <style>
      :host { all: initial; }
      .mb-card {
        width: 360px;
        max-height: 72vh;
        overflow: hidden;
        border: 1px solid #dadce0;
        border-radius: 12px;
        background: #fff;
        box-shadow: 0 6px 20px rgba(60,64,67,0.24);
        color: #202124;
        font: 12px/1.45 "Google Sans", Roboto, Arial, sans-serif;
      }
      .mb-header {
        display: flex;
        justify-content: space-between;
        align-items: center;
        padding: 10px 12px;
        border-bottom: 1px solid #eceff1;
        background: #f8f9fa;
      }
      .mb-title { font-size: 13px; font-weight: 600; }
      .mb-status { font-size: 11px; color: #5f6368; margin-top: 2px; }
      .mb-close {
        border: 1px solid #dadce0;
        background: #fff;
        color: #3c4043;
        border-radius: 8px;
        font-size: 11px;
        padding: 2px 8px;
        cursor: pointer;
      }
      .mb-body {
        max-height: calc(72vh - 48px);
        overflow: auto;
        padding: 10px 12px 12px;
      }
      .mb-goal {
        background: #f1f3f4;
        border: 1px solid #e5e7ea;
        border-radius: 10px;
        color: #3c4043;
        padding: 8px 10px;
        margin-bottom: 10px;
        font-size: 12px;
      }
      .mb-timeline {
        position: relative;
        padding-left: 18px;
      }
      .mb-timeline::before {
        content: "";
        position: absolute;
        left: 6px;
        top: 0;
        bottom: 0;
        width: 2px;
        background: #e0e3e7;
      }
      .mb-step {
        position: relative;
        margin-bottom: 10px;
        padding: 8px 10px;
        border: 1px solid #e7eaee;
        border-radius: 10px;
        background: #fafbfc;
      }
      .mb-step::before {
        content: "";
        position: absolute;
        left: -17px;
        top: 11px;
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: #1a73e8;
        box-shadow: 0 0 0 2px #fff;
      }
      .mb-step-head {
        font-size: 12px;
        font-weight: 600;
        color: #1f2d3d;
      }
      .mb-step-reason {
        margin-top: 4px;
        color: #5f6368;
        font-size: 12px;
      }
      .mb-loading {
        position: relative;
        margin-bottom: 10px;
        padding: 8px 10px;
        border: 1px dashed #d7dce2;
        border-radius: 10px;
        background: #f8fafc;
        color: #5f6368;
        font-size: 12px;
      }
      .mb-loading::before {
        content: "";
        position: absolute;
        left: -17px;
        top: 11px;
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: #1a73e8;
        box-shadow: 0 0 0 2px #fff;
      }
      .mb-dots {
        display: inline-flex;
        gap: 4px;
        margin-left: 6px;
        vertical-align: middle;
      }
      .mb-dots i {
        width: 4px;
        height: 4px;
        border-radius: 50%;
        background: #5f6368;
        animation: mb-bounce 1s infinite ease-in-out;
      }
      .mb-dots i:nth-child(2) { animation-delay: 0.15s; }
      .mb-dots i:nth-child(3) { animation-delay: 0.3s; }
      @keyframes mb-bounce {
        0%, 80%, 100% { transform: scale(0.6); opacity: 0.45; }
        40% { transform: scale(1); opacity: 1; }
      }
      .mb-result {
        margin-top: 4px;
        border-radius: 10px;
        border: 1px solid #d2e3fc;
        background: #e8f0fe;
        color: #174ea6;
        padding: 8px 10px;
        font-size: 12px;
      }
      .mb-result.error {
        border-color: #f6c7c3;
        background: #fce8e6;
        color: #a50e0e;
      }
    </style>
    <div class="mb-card" id="mb-card">
      <div class="mb-header">
        <div>
          <div class="mb-title">mimibrowser Plan</div>
          <div class="mb-status" id="mb-status">Idle</div>
        </div>
        <button class="mb-close" id="mb-close">Close</button>
      </div>
      <div class="mb-body">
        <div class="mb-goal" id="mb-goal">Goal: (waiting)</div>
        <div class="mb-timeline" id="mb-timeline"></div>
        <div class="mb-result" id="mb-result" style="display:none"></div>
      </div>
    </div>
  `;

  overlayRoot = shadow;
  overlayRoot.getElementById("mb-close")?.addEventListener("click", () => {
    overlayHost.style.display = "none";
  });

  return overlayRoot;
}

function renderPlan(plan) {
  const root = ensureOverlay();
  overlayHost.style.display = "block";

  const steps = Array.isArray(plan?.steps) ? plan.steps : [];
  const status = plan?.status || "idle";
  const statusLabel = status === "running" ? "Running" : status === "done" ? "Done" : status === "error" ? "Error" : "Idle";

  root.getElementById("mb-status").textContent = `${statusLabel} · steps ${steps.length}`;
  root.getElementById("mb-goal").textContent = plan?.goal ? `Goal: ${plan.goal}` : "Goal: (waiting)";

  const timeline = root.getElementById("mb-timeline");
  const stepsHtml = steps
    .map(
      (s) => `
      <div class="mb-step">
        <div class="mb-step-head">Step ${s.step || "?"} · ${s.action || "unknown"}</div>
        <div class="mb-step-reason">${s.reason || "(no reason)"}</div>
      </div>
    `
    )
    .join("");
  const loadingHtml =
    status === "running"
      ? `
      <div class="mb-loading">
        Waiting next step
        <span class="mb-dots"><i></i><i></i><i></i></span>
      </div>
    `
      : "";
  timeline.innerHTML = stepsHtml + loadingHtml;

  const resultEl = root.getElementById("mb-result");
  if (plan?.result) {
    resultEl.textContent = plan.result;
    resultEl.className = `mb-result ${status === "error" ? "error" : ""}`;
    resultEl.style.display = "block";
  } else {
    resultEl.style.display = "none";
  }

  if (autoHideTimer) {
    clearTimeout(autoHideTimer);
    autoHideTimer = null;
  }

  if (status === "done" || status === "error") {
    autoHideTimer = setTimeout(() => {
      if (overlayHost) {
        overlayHost.style.display = "none";
      }
    }, 5000);
  }
}

async function maybeShowOverlay(plan) {
  renderPlan(plan || {});
}

async function loadPlanState() {
  try {
    const res = await chrome.runtime.sendMessage({ type: "GET_PLAN_STATE" });
    if (res?.ok && res.payload) {
      maybeShowOverlay(res.payload);
    }
  } catch (_err) {}
}

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  try {
    if (message?.type === "GET_DOM_SNAPSHOT") {
      sendResponse(domSnapshot(message.maxText, message.maxElements));
      return;
    }

    if (message?.type === "EXECUTE_ACTION") {
      const result = executeAction(message.action || {});
      sendResponse(result);
      return;
    }

    if (message?.type === "PLAN_UPDATE") {
      maybeShowOverlay(message.payload || {});
      sendResponse({ ok: true });
      return;
    }

    sendResponse({ ok: false, error: "unknown_message_type" });
  } catch (err) {
    sendResponse({ ok: false, error: err.message || String(err) });
  }
});

loadPlanState();
