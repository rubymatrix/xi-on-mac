// The FINAL FANTASY XI launcher: accounts and connections (PlayOnline or LandSandBoat), the game's
// settings, and starting host64 with the right arguments. Same program on macOS and Windows.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod config;
mod game;
mod pol;

use config::LauncherConfig;
use game::{LaunchRequest, Paths, Running};
use serde::Serialize;
use std::path::PathBuf;
use std::sync::Arc;
use tauri::{AppHandle, Manager, State};

const KEYCHAIN_SERVICE: &str = "com.rubymatrix.xi-launcher";

fn config_dir(app: &AppHandle) -> Result<PathBuf, String> {
    app.path().app_config_dir().map_err(|e| e.to_string())
}

fn keychain(account_id: &str) -> Result<keyring::Entry, String> {
    keyring::Entry::new(KEYCHAIN_SERVICE, account_id).map_err(|e| e.to_string())
}

/// Passwords read from (or given to) the keychain this run, so a second Play does not ask again.
#[derive(Default)]
struct Passwords(std::sync::Mutex<std::collections::HashMap<String, String>>);

fn stored_password(passwords: &Passwords, account_id: &str) -> Option<String> {
    let mut cache = passwords.0.lock().unwrap();
    if let Some(p) = cache.get(account_id) {
        return Some(p.clone());
    }
    let p = keychain(account_id).ok()?.get_password().ok()?;
    cache.insert(account_id.to_string(), p.clone());
    Some(p)
}

/// host64 when the configuration names none: in the app bundle as its own app (macOS:
/// Contents/Helpers/FINAL FANTASY XI.app, so the game has its own Dock icon and name), beside the
/// launcher (Windows, or a plain build), else the repository's build/ (a development run).
fn default_host() -> PathBuf {
    let name = if cfg!(windows) { "host64.exe" } else { "host64" };
    let mut candidates = Vec::new();
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(|p| p.to_path_buf())) {
        candidates.push(dir.join("../Helpers/FINAL FANTASY XI.app/Contents/MacOS").join(name));
        candidates.push(dir.join(name));
    }
    candidates.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../build").join(name));
    candidates.iter().find(|p| p.exists()).cloned().unwrap_or_else(|| candidates.remove(0))
}

fn default_registry(app: &AppHandle) -> PathBuf {
    app.path()
        .resolve("playonline.reg", tauri::path::BaseDirectory::Resource)
        .ok()
        .filter(|p| p.exists())
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../playonline.reg"))
}

fn paths(app: &AppHandle, cfg: &LauncherConfig) -> Result<Paths, String> {
    Ok(Paths {
        config_dir: config_dir(app)?,
        log_dir: app.path().app_log_dir().map_err(|e| e.to_string())?,
        host: if cfg.host_program.is_empty() { default_host() } else { PathBuf::from(&cfg.host_program) },
        base_registry: if cfg.base_registry.is_empty() { default_registry(app) } else { PathBuf::from(&cfg.base_registry) },
    })
}

#[derive(Serialize)]
struct Defaults {
    host_program: String,
    base_registry: String,
    config_dir: String,
    log_dir: String,
}

#[tauri::command]
fn get_config(app: AppHandle) -> Result<LauncherConfig, String> {
    Ok(config::load(&config_dir(&app)?))
}

#[tauri::command]
fn save_config(app: AppHandle, cfg: LauncherConfig) -> Result<(), String> {
    let dir = config_dir(&app)?;
    // accounts that went away take their stored password with them
    for old in config::load(&dir).accounts {
        if !cfg.accounts.iter().any(|a| a.id == old.id) {
            if let Ok(e) = keychain(&old.id) {
                let _ = e.delete_credential();
            }
        }
    }
    config::save(&dir, &cfg)
}

#[tauri::command]
fn get_defaults(app: AppHandle) -> Result<Defaults, String> {
    let p = paths(&app, &LauncherConfig::default())?;
    Ok(Defaults {
        host_program: p.host.to_string_lossy().into_owned(),
        base_registry: p.base_registry.to_string_lossy().into_owned(),
        config_dir: p.config_dir.to_string_lossy().into_owned(),
        log_dir: p.log_dir.to_string_lossy().into_owned(),
    })
}

#[tauri::command]
fn game_defaults() -> config::GameSettings {
    config::GameSettings::default()
}

#[tauri::command]
fn forget_password(passwords: State<Passwords>, account_id: String) -> Result<(), String> {
    passwords.0.lock().unwrap().remove(&account_id);
    match keychain(&account_id)?.delete_credential() {
        Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
        Err(e) => Err(e.to_string()),
    }
}

/// Starts the game for an account. An empty password uses the stored one; a given one is stored
/// when the account says to remember it. Returns whether the keychain now holds it (the window
/// records that in the account, with last_account, and saves).
#[tauri::command]
fn launch(
    app: AppHandle,
    running: State<Arc<Running>>,
    passwords: State<Passwords>,
    account_id: String,
    password: String,
    otp: String,
) -> Result<bool, String> {
    let cfg = config::load(&config_dir(&app)?);
    let account = cfg
        .accounts
        .iter()
        .find(|a| a.id == account_id)
        .cloned()
        .ok_or("No such account.")?;
    let mut saved = account.password_saved;
    let password = if password.is_empty() {
        let p = stored_password(&passwords, &account.id).ok_or("Enter the password.")?;
        saved = true;
        p
    } else {
        if account.save_password {
            keychain(&account.id)?.set_password(&password).map_err(|e| e.to_string())?;
            saved = true;
        }
        passwords.0.lock().unwrap().insert(account.id.clone(), password.clone());
        password
    };
    let paths = paths(&app, &cfg)?;
    game::launch(app, running.inner().clone(), cfg, paths, LaunchRequest { account, password, otp })?;
    Ok(saved)
}

#[tauri::command]
fn stop(running: State<Arc<Running>>) {
    game::stop(&running);
}

#[tauri::command]
fn is_running(running: State<Arc<Running>>) -> bool {
    game::is_running(&running)
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(Arc::new(Running::default()))
        .manage(Passwords::default())
        .invoke_handler(tauri::generate_handler![
            get_config,
            save_config,
            get_defaults,
            game_defaults,
            forget_password,
            launch,
            stop,
            is_running
        ])
        .build(tauri::generate_context!())
        .expect("error while starting the launcher")
        .run(|app, event| {
            // macOS: clicking the Dock icon brings back the window hidden while the game runs
            #[cfg(target_os = "macos")]
            if let tauri::RunEvent::Reopen { .. } = event {
                if let Some(w) = app.get_webview_window("main") {
                    let _ = w.show();
                    let _ = w.set_focus();
                }
            }
            let _ = (app, event);
        });
}
