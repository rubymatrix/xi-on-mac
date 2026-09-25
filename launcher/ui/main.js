// The launcher window. Everything persistent lives in the Rust side's launcher.json (and the
// keychain, for passwords); this page edits it and saves on every change.
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;
const dialog = window.__TAURI__.dialog;

const $ = (s) => document.querySelector(s);
let cfg = null;
let selected = null; // account id
let running = false;
let gameDefaults = null;

const accountForm = $("#account");
const gameForm = $("#game-settings");
const launcherForm = $("#launcher-settings");

function newId() {
  return "acct-" + Date.now().toString(36) + Math.random().toString(36).slice(2, 7);
}

async function save(form) {
  await invoke("save_config", { cfg });
  const note = form && form.querySelector(".saved");
  if (note) {
    note.hidden = false;
    clearTimeout(note._t);
    note._t = setTimeout(() => (note.hidden = true), 1200);
  }
}

// --- tabs ------------------------------------------------------------------------------------------
document.querySelectorAll(".tab").forEach((t) =>
  t.addEventListener("click", () => {
    document.querySelectorAll(".tab, .page").forEach((e) => e.classList.remove("active"));
    t.classList.add("active");
    $("#" + t.dataset.tab).classList.add("active");
  })
);

// --- accounts --------------------------------------------------------------------------------------
const KIND_LABEL = { pol: "PlayOnline", lsb: "LandSandBoat" };

function account() {
  return cfg.accounts.find((a) => a.id === selected);
}

function renderAccounts() {
  const ul = $("#accounts");
  ul.innerHTML = "";
  for (const a of cfg.accounts) {
    const li = document.createElement("li");
    li.className = a.id === selected ? "selected" : "";
    const name = document.createElement("span");
    name.textContent = a.name || a.login || "New account";
    const sub = document.createElement("small");
    sub.textContent = `${KIND_LABEL[a.kind]} · ${a.server || "no server"}`;
    li.append(name, sub);
    li.addEventListener("click", () => select(a.id));
    ul.append(li);
  }
  $("#no-account").hidden = cfg.accounts.length > 0;
  accountForm.hidden = !account();
}

function showKind(kind) {
  accountForm.querySelectorAll("[data-kind]").forEach((e) => (e.hidden = e.dataset.kind !== kind));
}

async function select(id) {
  selected = id;
  const a = account();
  renderAccounts();
  if (!a) return;
  for (const k of ["name", "kind", "login", "server", "pol_server"]) accountForm.elements[k].value = a[k] ?? "";
  for (const k of ["auth_port", "data_port", "view_port"]) accountForm.elements[k].value = a[k] || "";
  accountForm.elements.save_password.checked = a.save_password;
  accountForm.elements.password.value = "";
  accountForm.elements.otp.value = "";
  accountForm.elements.password.placeholder = a.password_saved ? "stored in the keychain" : "";
  showKind(a.kind);
}

accountForm.addEventListener("change", async (e) => {
  const a = account();
  const name = e.target.name;
  if (!a || name === "password" || name === "otp") return;
  if (name === "save_password") {
    a.save_password = e.target.checked;
    if (!a.save_password) {
      await invoke("forget_password", { accountId: a.id });
      a.password_saved = false;
      accountForm.elements.password.placeholder = "";
    }
  } else if (name.endsWith("_port")) {
    a[name] = parseInt(e.target.value, 10) || 0;
  } else {
    a[name] = e.target.value.trim();
  }
  if (name === "kind") showKind(a.kind);
  await save();
  renderAccounts();
});

$("#add-account").addEventListener("click", async () => {
  const a = {
    id: newId(), name: "", kind: "lsb", login: "", server: "127.0.0.1", pol_server: "",
    save_password: true, password_saved: false, auth_port: 0, data_port: 0, view_port: 0,
  };
  cfg.accounts.push(a);
  await save();
  await select(a.id);
  accountForm.elements.name.focus();
});

$("#delete-account").addEventListener("click", async () => {
  const a = account();
  if (!a || !confirm(`Delete ${a.name || a.login || "this account"}? Its stored password goes too.`)) return;
  cfg.accounts = cfg.accounts.filter((x) => x.id !== a.id);
  await save();
  await select(cfg.accounts[0]?.id ?? null);
});

// --- playing ---------------------------------------------------------------------------------------
function setStatus(text, error) {
  $("#status").textContent = text;
  $("#status").className = error ? "error" : "";
}

function setRunning(r) {
  running = r;
  $("#play-button").disabled = r;
  $("#stop").hidden = !r;
}

accountForm.addEventListener("submit", async (e) => {
  e.preventDefault();
  const a = account();
  if (!a || running) return;
  $("#log").textContent = "";
  setStatus("Starting…");
  setRunning(true);
  try {
    a.password_saved = await invoke("launch", {
      accountId: a.id,
      password: accountForm.elements.password.value,
      otp: accountForm.elements.otp.value.trim(),
    });
    cfg.last_account = a.id;
    await save();
    accountForm.elements.password.value = "";
    accountForm.elements.otp.value = "";
    accountForm.elements.password.placeholder = a.password_saved ? "stored in the keychain" : "";
  } catch (err) {
    setRunning(false);
    setStatus(String(err), true);
  }
});

$("#stop").addEventListener("click", () => invoke("stop"));

listen("game-state", ({ payload }) => {
  setStatus(payload.message, payload.state === "error");
  setRunning(payload.state === "signing-in" || payload.state === "running");
});

const log = $("#log");
listen("game-log", ({ payload }) => {
  const atEnd = log.scrollTop + log.clientHeight >= log.scrollHeight - 4;
  log.append(payload + "\n");
  // keep the view bounded: the game can log a lot
  if (log.textContent.length > 400000) log.textContent = log.textContent.slice(-300000);
  if (atEnd) log.scrollTop = log.scrollHeight;
});

// --- game settings ---------------------------------------------------------------------------------
function fillSettings(form, obj) {
  for (const el of form.elements) {
    if (!el.name || !(el.name in obj)) continue;
    if (el.type === "checkbox") el.checked = !!obj[el.name];
    else el.value = obj[el.name];
  }
}

gameForm.addEventListener("change", async (e) => {
  const el = e.target;
  if (!el.name) return;
  cfg.game[el.name] = el.type === "checkbox" ? el.checked : "num" in el.dataset ? parseInt(el.value, 10) || 0 : el.value;
  await save(gameForm);
});

$("#game-defaults").addEventListener("click", async () => {
  cfg.game = { ...gameDefaults };
  fillSettings(gameForm, cfg.game);
  await save(gameForm);
});

// --- launcher settings -----------------------------------------------------------------------------
function fillLauncher() {
  fillSettings(launcherForm, cfg);
  launcherForm.elements.dats.value = cfg.dats.join("\n");
}

launcherForm.addEventListener("change", async (e) => {
  const el = e.target;
  if (!el.name) return;
  if (el.name === "dats") cfg.dats = el.value.split("\n").map((s) => s.trim()).filter(Boolean);
  else cfg[el.name] = el.value.trim();
  await save(launcherForm);
});

launcherForm.querySelectorAll("[data-pick]").forEach((b) =>
  b.addEventListener("click", async () => {
    const path = await dialog.open({ directory: b.dataset.pick === "folder", multiple: false });
    if (!path) return;
    if (b.dataset.append) {
      const ta = launcherForm.elements[b.dataset.append];
      ta.value = (ta.value.trim() ? ta.value.trim() + "\n" : "") + path;
      ta.dispatchEvent(new Event("change", { bubbles: true }));
    } else {
      const input = launcherForm.elements[b.dataset.for];
      input.value = path;
      input.dispatchEvent(new Event("change", { bubbles: true }));
    }
  })
);

// --- start -----------------------------------------------------------------------------------------
(async () => {
  cfg = await invoke("get_config");
  const d = await invoke("get_defaults");
  launcherForm.elements.host_program.placeholder = d.host_program;
  launcherForm.elements.base_registry.placeholder = d.base_registry;
  $("#paths").textContent = `Settings: ${d.config_dir} · Logs: ${d.log_dir}`;
  gameDefaults = await invoke("game_defaults");
  fillSettings(gameForm, cfg.game);
  fillLauncher();
  setRunning(await invoke("is_running"));
  await select(cfg.accounts.some((a) => a.id === cfg.last_account) ? cfg.last_account : cfg.accounts[0]?.id ?? null);
  if (!cfg.game_path) document.querySelector('.tab[data-tab="launcher"]').click();
})();
