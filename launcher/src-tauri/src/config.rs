//! The launcher's persistent configuration: accounts, paths and the game's settings, kept in
//! <app config dir>/launcher.json. Passwords are not in it; they go to the OS keychain (secrets.rs).

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Serialize, Deserialize, Clone, Copy, PartialEq, Eq, Debug)]
#[serde(rename_all = "lowercase")]
pub enum AccountKind {
    /// A server with PlayOnline behind it: the launcher signs in and hands host64 the session value.
    Pol,
    /// A LandSandBoat server (xiloader's protocol): host64 signs in itself.
    Lsb,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(default)]
pub struct Account {
    pub id: String,
    pub name: String,
    pub kind: AccountKind,
    /// The PlayOnline ID (Pol) or the account name (Lsb).
    pub login: String,
    /// Where the game's servers are: the lobby, and the LandSandBoat sign-in.
    pub server: String,
    /// Pol: the PlayOnline sign-in server, when it is not `server`.
    pub pol_server: String,
    /// Remember the password in the OS keychain.
    pub save_password: bool,
    /// One is in the keychain. Kept here so showing the account never reads the keychain (each
    /// read can ask the user for access).
    pub password_saved: bool,
    /// Lsb: LandSandBoat's ports, 0 for xiloader's defaults (54231, 54230, 54001).
    pub auth_port: u16,
    pub data_port: u16,
    pub view_port: u16,
}

impl Default for Account {
    fn default() -> Self {
        Account {
            id: String::new(),
            name: String::new(),
            kind: AccountKind::Lsb,
            login: String::new(),
            server: "127.0.0.1".into(),
            pol_server: String::new(),
            save_password: true,
            password_saved: false,
            auth_port: 0,
            data_port: 0,
            view_port: 0,
        }
    }
}

/// The game's own settings (what FINAL FANTASY XI's config tool writes), in
/// HKLM\SOFTWARE\PlayOnlineUS\SquareEnix\FinalFantasyXI.
#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(default)]
pub struct GameSettings {
    /// 0 full screen, 1 windowed, 2 borderless window, 3 borderless full screen (0034)
    pub window_mode: u32,
    pub window_width: u32, // 0001
    pub window_height: u32, // 0002
    /// 0 x 0 is the window's size
    pub menu_width: u32, // 0037
    pub menu_height: u32, // 0038
    pub background_width: u32, // 0003
    pub background_height: u32, // 0004
    /// 0-6 (0000)
    pub mip_mapping: u32,
    /// 0 high, 1 low, 2 uncompressed (0018)
    pub texture_compression: u32,
    /// 0 compressed, 1 uncompressed (0019)
    pub map_compression: u32,
    /// 0 compressed, 1 uncompressed, 2 high quality (0036)
    pub font: u32,
    /// 0 off, 1 normal, 2 smooth (0011)
    pub environment_animation: u32,
    pub bump_mapping: bool, // 0017
    pub graphics_stabilization: bool, // 0040
    pub hardware_mouse: bool, // 0021
    pub opening_movie: bool, // 0022
    pub simplified_ccs: bool, // 0023
    pub sound: bool, // 0007
    pub sound_in_background: bool, // 0035
    /// 12-20 (0029)
    pub max_sounds: u32,
    /// host64 --fps-divisor: 1 is 60 fps, 2 is 30 fps as shipped
    pub fps_divisor: u32,
    /// FFXI_PROBE=gpu: answer the game's occlusion probe from the GPU (the sun's lens flare hides
    /// behind walls) at a cost of a few ms a frame; off, it always reads fully visible
    pub gpu_probe: bool,
    /// host64 --ui-aspect: the interface's shape, centered in a wider window ("16:9"), or "off"
    /// to stretch it across the window as the game does
    pub ui_aspect: String,
}

impl Default for GameSettings {
    fn default() -> Self {
        GameSettings {
            window_mode: 1,
            window_width: 1920,
            window_height: 1080,
            menu_width: 960,
            menu_height: 540,
            background_width: 4096,
            background_height: 4096,
            mip_mapping: 6,
            texture_compression: 1,
            map_compression: 1,
            font: 0,
            environment_animation: 1,
            bump_mapping: true,
            graphics_stabilization: false,
            hardware_mouse: false,
            opening_movie: true,
            simplified_ccs: false,
            sound: true,
            sound_in_background: true,
            max_sounds: 12,
            fps_divisor: 1,
            gpu_probe: false,
            ui_aspect: "off".into(),
        }
    }
}

impl GameSettings {
    /// The settings as a REGEDIT4 file for host64 --reg-final (loaded after the game's own saves).
    pub fn to_reg(&self) -> String {
        let b = |v: bool| v as u32;
        let values: [(&str, u32); 20] = [
            ("0000", self.mip_mapping.min(6)),
            ("0001", self.window_width),
            ("0002", self.window_height),
            ("0003", self.background_width),
            ("0004", self.background_height),
            ("0007", b(self.sound)),
            ("0011", self.environment_animation.min(2)),
            ("0017", b(self.bump_mapping)),
            ("0018", self.texture_compression.min(2)),
            ("0019", self.map_compression.min(1)),
            ("0021", b(self.hardware_mouse)),
            ("0022", b(self.opening_movie)),
            ("0023", b(self.simplified_ccs)),
            ("0029", self.max_sounds.clamp(12, 20)),
            ("0034", self.window_mode.min(3)),
            ("0035", b(self.sound_in_background)),
            ("0036", self.font.min(2)),
            ("0037", self.menu_width),
            ("0038", self.menu_height),
            ("0040", b(self.graphics_stabilization)),
        ];
        let mut s = String::from("REGEDIT4\r\n\r\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI]\r\n");
        for (name, v) in values {
            s += &format!("\"{name}\"=dword:{v:08x}\r\n");
        }
        s
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
#[serde(default)]
pub struct LauncherConfig {
    /// The FINAL FANTASY XI folder (PlayOnlineViewer beside it).
    pub game_path: String,
    /// host64; empty: the one bundled with the launcher.
    pub host_program: String,
    /// The base registry export; empty: the bundled playonline.reg.
    pub base_registry: String,
    /// DAT overlay folders (host64 --dats), the first wins.
    pub dats: Vec<String>,
    pub accounts: Vec<Account>,
    pub last_account: String,
    pub game: GameSettings,
}

pub fn path(config_dir: &Path) -> PathBuf {
    config_dir.join("launcher.json")
}

pub fn load(config_dir: &Path) -> LauncherConfig {
    fs::read_to_string(path(config_dir))
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

pub fn save(config_dir: &Path, cfg: &LauncherConfig) -> Result<(), String> {
    fs::create_dir_all(config_dir).map_err(|e| e.to_string())?;
    let text = serde_json::to_string_pretty(cfg).map_err(|e| e.to_string())?;
    let tmp = config_dir.join("launcher.json.tmp");
    fs::write(&tmp, text).map_err(|e| e.to_string())?;
    fs::rename(&tmp, path(config_dir)).map_err(|e| e.to_string())
}
