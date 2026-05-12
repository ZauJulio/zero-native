var PAGE_WEBVIEW_LABEL = "page";

var form = document.querySelector("#browser-form");
var addressInput = document.querySelector("#address");
var addressBar = document.querySelector("#address-bar");
var addressIcon = document.querySelector("#address-icon");
var suggestions = document.querySelector("#suggestions");
var backButton = document.querySelector("#back");
var forwardButton = document.querySelector("#forward");
var reloadButton = document.querySelector("#reload");
var statusBar = document.querySelector("#status");
var statusText = document.querySelector("#status-text");
var emptyState = document.querySelector("#empty-state");
var errorState = document.querySelector("#error-state");
var errorTitle = document.querySelector("#error-title");
var errorDetail = document.querySelector("#error-detail");
var errorRetry = document.querySelector("#error-retry");
var toolbar = document.querySelector("#toolbar");

var pageWebView = null;
var currentUrl = "";
var navHistory = [];
var historyIndex = -1;
var visitedUrls = [];
var resizeHandle = 0;
var isLoading = false;
var statusTimer = null;
var zoomLevel = 1.0;
var ZOOM_STEP = 0.1;
var ZOOM_MIN = 0.25;
var ZOOM_MAX = 5.0;
var suggestActive = -1;
var suggestFiltered = [];

function setStatus(message, autohide) {
  clearTimeout(statusTimer);
  statusText.textContent = message;
  statusBar.classList.remove("hidden");
  if (autohide) {
    statusTimer = setTimeout(function () {
      statusBar.classList.add("hidden");
    }, autohide);
  }
}

function setLoading(loading) {
  isLoading = loading;
  if (loading) {
    addressBar.classList.remove("load-complete");
    addressBar.classList.add("loading");
  } else {
    addressBar.classList.remove("loading");
    addressBar.classList.add("load-complete");
    setTimeout(function () {
      addressBar.classList.remove("load-complete");
    }, 600);
  }
}

function updateSecurityIndicator(url) {
  if (url.startsWith("https://")) {
    addressBar.classList.add("secure");
  } else {
    addressBar.classList.remove("secure");
  }
}

function normalizeUrl(value) {
  var trimmed = value.trim();
  if (!trimmed) return "https://example.com";
  if (/^[a-z][a-z0-9+.-]*:\/\//i.test(trimmed)) return trimmed;
  if (/^[a-z0-9]([a-z0-9-]*\.)+[a-z]{2,}/i.test(trimmed)) return "https://" + trimmed;
  return "https://" + trimmed;
}

function pageFrame() {
  var rect = toolbar.getBoundingClientRect();
  var top = Math.ceil(rect.height);
  top = Math.min(top, Math.max(0, window.innerHeight - 1));
  return {
    x: 0,
    y: top,
    width: Math.max(1, Math.floor(window.innerWidth)),
    height: Math.max(1, Math.floor(window.innerHeight - top)),
  };
}

function chromeFrame() {
  var toolbarRect = toolbar.getBoundingClientRect();
  var height = Math.ceil(toolbarRect.height);
  if (!suggestions.hidden) {
    var suggestionsRect = suggestions.getBoundingClientRect();
    height = Math.max(height, Math.ceil(suggestionsRect.bottom + 8));
  }
  return {
    x: 0,
    y: 0,
    width: Math.max(1, Math.floor(window.innerWidth)),
    height: Math.max(1, Math.min(height, window.innerHeight)),
  };
}

function updateHistoryButtons() {
  backButton.disabled = historyIndex <= 0;
  forwardButton.disabled = historyIndex < 0 || historyIndex >= navHistory.length - 1;
}

function remember(url) {
  if (navHistory[historyIndex] === url) return;
  navHistory = navHistory.slice(0, historyIndex + 1);
  navHistory.push(url);
  historyIndex = navHistory.length - 1;
  updateHistoryButtons();
}

function trackVisited(url) {
  for (var i = 0; i < visitedUrls.length; i++) {
    if (visitedUrls[i] === url) return;
  }
  visitedUrls.unshift(url);
  if (visitedUrls.length > 200) visitedUrls.length = 200;
}

function hostFromUrl(url) {
  try {
    return new URL(url).hostname;
  } catch (_) {
    return url;
  }
}

// ── Suggestions ──

function escapeHtml(str) {
  return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function highlightMatch(text, query) {
  if (!query) return escapeHtml(text);
  var lower = text.toLowerCase();
  var qLower = query.toLowerCase();
  var idx = lower.indexOf(qLower);
  if (idx === -1) return escapeHtml(text);
  var before = text.slice(0, idx);
  var match = text.slice(idx, idx + query.length);
  var after = text.slice(idx + query.length);
  return escapeHtml(before) + "<mark>" + escapeHtml(match) + "</mark>" + escapeHtml(after);
}

function filterSuggestions(query) {
  if (!query) return [];
  var q = query.toLowerCase();
  var results = [];
  for (var i = 0; i < visitedUrls.length; i++) {
    if (visitedUrls[i].toLowerCase().indexOf(q) !== -1) {
      results.push(visitedUrls[i]);
      if (results.length >= 8) break;
    }
  }
  return results;
}

function renderSuggestions(query) {
  suggestFiltered = filterSuggestions(query);
  suggestActive = -1;
  if (suggestFiltered.length === 0) {
    suggestions.hidden = true;
    scheduleResize();
    return;
  }
  var html = "";
  for (var i = 0; i < suggestFiltered.length; i++) {
    var url = suggestFiltered[i];
    var host = hostFromUrl(url);
    html +=
      '<div class="suggestion" data-index="' + i + '">' +
        '<div class="suggestion-icon">' +
          '<svg width="14" height="14" viewBox="0 0 14 14" fill="none"><circle cx="7" cy="7" r="5.5" stroke="currentColor" stroke-width="1.1"/><path d="M5 5.5h4M5 7h4M5 8.5h2.5" stroke="currentColor" stroke-width="0.9" stroke-linecap="round"/></svg>' +
        '</div>' +
        '<span class="suggestion-url">' + highlightMatch(url, query) + '</span>' +
        '<span class="suggestion-host">' + escapeHtml(host) + '</span>' +
      '</div>';
  }
  suggestions.innerHTML = html;
  suggestions.hidden = false;
  scheduleResize();
}

function hideSuggestions() {
  var wasVisible = !suggestions.hidden;
  suggestions.hidden = true;
  suggestActive = -1;
  suggestFiltered = [];
  if (wasVisible) scheduleResize();
}

function setSuggestActive(index) {
  var items = suggestions.querySelectorAll(".suggestion");
  for (var i = 0; i < items.length; i++) {
    items[i].classList.toggle("active", i === index);
  }
  suggestActive = index;
  if (index >= 0 && index < suggestFiltered.length) {
    addressInput.value = suggestFiltered[index];
  }
}

suggestions.addEventListener("mousedown", function (event) {
  event.preventDefault();
  var item = event.target.closest(".suggestion");
  if (!item) return;
  var idx = parseInt(item.getAttribute("data-index"), 10);
  if (idx >= 0 && idx < suggestFiltered.length) {
    hideSuggestions();
    navigateTo(suggestFiltered[idx]);
  }
});

// ── Error state ──

function showError(url, message) {
  var host = hostFromUrl(url);
  errorTitle.textContent = "Can\u2019t connect to " + host;
  errorDetail.textContent = message || "The site could not be reached. Check the address or try again.";
  errorState.hidden = false;
  emptyState.hidden = true;
}

function hideError() {
  errorState.hidden = true;
}

async function closeOverlay() {
  if (!pageWebView) return;
  try {
    await pageWebView.close();
  } catch (error) {
    console.error("Failed to close page WebView", error);
    setStatus(error && error.message ? error.message : "Failed to close page WebView", 3000);
  }
  pageWebView = null;
}

// ── Navigation ──

async function probeUrl(url) {
  var controller = new AbortController();
  var timeout = setTimeout(function () { controller.abort(); }, 6000);
  try {
    await fetch(url, { method: "HEAD", mode: "no-cors", signal: controller.signal });
  } finally {
    clearTimeout(timeout);
  }
}

async function ensureOverlay(url) {
  var frame = pageFrame();
  if (!pageWebView) {
    await window.zero.webviews.setFrame({ label: "main", frame: chromeFrame() });
    pageWebView = await window.zero.webviews.create({
      label: PAGE_WEBVIEW_LABEL,
      url: url,
      frame: frame,
      layer: 0,
      transparent: false,
      bridge: false,
    });
    emptyState.hidden = true;
  } else {
    await pageWebView.setFrame(frame);
    await pageWebView.navigate(url);
  }
}

async function navigateTo(url, options) {
  options = options || {};
  var target = normalizeUrl(url);
  addressInput.value = target;
  updateSecurityIndicator(target);
  hideError();
  setLoading(true);
  setStatus("Loading " + hostFromUrl(target) + "\u2026");
  try {
    await probeUrl(target);
  } catch (err) {
    setLoading(false);
    await closeOverlay();
    var reason = err.name === "AbortError"
      ? "The connection timed out. The server may be down or unreachable."
      : "The site can\u2019t be reached. Check the address or your connection.";
    showError(target, reason);
    setStatus("Failed to load " + hostFromUrl(target));
    if (options.record !== false) remember(target);
    currentUrl = target;
    return;
  }
  try {
    await ensureOverlay(target);
    currentUrl = target;
    trackVisited(target);
    if (options.record !== false) remember(target);
    setLoading(false);
    setStatus(hostFromUrl(target), 2000);
  } catch (error) {
    setLoading(false);
    await closeOverlay();
    var msg = error && error.message ? error.message : "Navigation failed";
    showError(target, msg);
    setStatus("Failed to load " + hostFromUrl(target));
  }
}

function scheduleResize() {
  if (resizeHandle) cancelAnimationFrame(resizeHandle);
  resizeHandle = requestAnimationFrame(async function () {
    resizeHandle = 0;
    if (!pageWebView) return;
    try {
      await window.zero.webviews.setFrame({ label: "main", frame: chromeFrame() });
      await pageWebView.setFrame(pageFrame());
    } catch (error) {
      setStatus(error.message || "Failed to resize page WebView");
    }
  });
}

// ── Event listeners ──

form.addEventListener("submit", function (event) {
  event.preventDefault();
  hideSuggestions();
  navigateTo(addressInput.value);
});

addressInput.addEventListener("focus", function () {
  addressBar.classList.add("focused");
  addressInput.select();
  renderSuggestions(addressInput.value);
});

addressInput.addEventListener("blur", function () {
  addressBar.classList.remove("focused");
  hideSuggestions();
});

addressInput.addEventListener("input", function () {
  renderSuggestions(addressInput.value);
});

addressInput.addEventListener("keydown", function (event) {
  if (event.key === "Escape") {
    if (!suggestions.hidden) {
      hideSuggestions();
      addressInput.value = currentUrl || addressInput.value;
      return;
    }
    addressInput.value = currentUrl || addressInput.value;
    addressInput.blur();
    return;
  }

  if (suggestions.hidden || suggestFiltered.length === 0) return;

  if (event.key === "ArrowDown") {
    event.preventDefault();
    var next = suggestActive + 1;
    if (next >= suggestFiltered.length) next = 0;
    setSuggestActive(next);
  } else if (event.key === "ArrowUp") {
    event.preventDefault();
    var prev = suggestActive - 1;
    if (prev < 0) prev = suggestFiltered.length - 1;
    setSuggestActive(prev);
  } else if (event.key === "Enter" && suggestActive >= 0) {
    event.preventDefault();
    var url = suggestFiltered[suggestActive];
    hideSuggestions();
    navigateTo(url);
  }
});

backButton.addEventListener("click", function () {
  if (historyIndex <= 0) return;
  historyIndex -= 1;
  updateHistoryButtons();
  navigateTo(navHistory[historyIndex], { record: false });
});

forwardButton.addEventListener("click", function () {
  if (historyIndex >= navHistory.length - 1) return;
  historyIndex += 1;
  updateHistoryButtons();
  navigateTo(navHistory[historyIndex], { record: false });
});

reloadButton.addEventListener("click", function () {
  if (isLoading) {
    setLoading(false);
    setStatus("Stopped", 2000);
    return;
  }
  navigateTo(currentUrl || addressInput.value, { record: false });
});

errorRetry.addEventListener("click", function () {
  navigateTo(currentUrl || addressInput.value, { record: false });
});

async function applyZoom(level) {
  zoomLevel = Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, Math.round(level * 100) / 100));
  if (!pageWebView) return;
  try {
    await pageWebView.setZoom(zoomLevel);
    var pct = Math.round(zoomLevel * 100);
    setStatus(pct + "%", 1200);
  } catch (error) {
    console.error("Failed to zoom page WebView", error);
    setStatus(error && error.message ? error.message : "Failed to zoom page WebView", 3000);
  }
}

window.addEventListener("keydown", function (event) {
  var isMod = event.metaKey || event.ctrlKey;
  if (!isMod) return;

  if (event.key === "=" || event.key === "+") {
    event.preventDefault();
    applyZoom(zoomLevel + ZOOM_STEP);
  } else if (event.key === "-") {
    event.preventDefault();
    applyZoom(zoomLevel - ZOOM_STEP);
  } else if (event.key === "0") {
    event.preventDefault();
    applyZoom(1.0);
  }
});

window.addEventListener("resize", scheduleResize);
window.addEventListener("DOMContentLoaded", function () {
  navigateTo(addressInput.value);
});
