// Resident Evil: Ruined Edition - asset importer and launcher.
//
// This program ships with the compiled game binary but never with Capcom's
// game data. It asks the player for their own copy of the game and copies the
// assets out of it into this folder, then writes the config.ini the game reads
// and launches it.
//
// Two front ends share the same import and launch code: a window (gui.rs,
// eframe/egui) and a terminal menu (ui.rs). The window is the default; the
// menu takes over with --tui or on machines with no display. The import core
// itself only needs flate2 and lzma-rust2, so it still builds offline and
// drops into a tarball/deb/rpm with nothing else to resolve.

mod embed;
mod gui;
mod iso9660;
mod inno;
mod paths;
mod pick;
mod probe;
mod ui;

use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let exe_dir = paths::exe_dir();
    // Debug hook: materialise the embedded payload and report, no GUI, no game.
    if std::env::args().any(|a| a == "--extract-only") {
        match embed::ensure_extracted(&exe_dir) {
            Some(bin) => {
                println!("game binary: {}", bin.display());
                let lib = embed::lib_dir(&exe_dir);
                let n = std::fs::read_dir(&lib).map(|d| d.count()).unwrap_or(0);
                println!("libraries: {n} files in {}", lib.display());
            }
            None => println!("no embedded payload (dev build)"),
        }
        return;
    }
    let force_tui = std::env::args().any(|a| a == "--tui");
    if !force_tui && gui::available() && gui::run(exe_dir.clone()) {
        return; // the window ran and closed normally
    }
    let mut app = App::load(exe_dir);
    ui::run(&mut app);
}

/// Where a game region's assets are expected to live.
pub struct Region {
    /// "USA" or "JPN" - the folder name the game resolves.
    pub name: &'static str,
    /// Human label for the menus.
    pub label: &'static str,
    /// True once the required subfolders are present.
    pub installed: bool,
}

pub struct App {
    /// The folder the launcher lives in; the game binary sits beside it.
    pub root: PathBuf,
    pub regions: Vec<Region>,
    /// [Display] LineArt in config.ini.
    pub line_art: bool,
    pub line_art_detail: i32,
    /// [Display] StickFigure in config.ini.
    pub stick_figure: bool,
    pub version: String,
    /// Set for one frame to show a result message.
    pub message: Option<String>,
}

impl App {
    fn load(root: PathBuf) -> App {
        let mut app = App {
            root: root.clone(),
            regions: Vec::new(),
            line_art: true,
            line_art_detail: 4,
            stick_figure: true,
            version: "USA".to_string(),
            message: None,
        };
        app.refresh_regions();
        app.load_config();
        app
    }

    fn refresh_regions(&mut self) {
        let mut regions = Vec::new();
        for (name, label) in [("USA", "GOG / retail USA (English)"),
                              ("JPN", "Biohazard JPN (MediaKite)")] {
            let dir = self.root.join(name);
            let installed = region_installed(&dir);
            regions.push(Region { name, label, installed });
        }
        self.regions = regions;
    }


    fn load_config(&mut self) {
        let cfg = self.root.join("config.ini");
        let text = std::fs::read_to_string(&cfg).unwrap_or_default();
        for line in text.lines() {
            let line = line.trim();
            if let Some(v) = line.strip_prefix("LineArt=") {
                self.line_art = v.trim() != "0";
            } else if let Some(v) = line.strip_prefix("LineArtDetail=") {
                self.line_art_detail = v.trim().parse().unwrap_or(3);
            } else if let Some(v) = line.strip_prefix("StickFigure=") {
                self.stick_figure = v.trim() != "0";
            } else if let Some(v) = line.strip_prefix("Version=") {
                self.version = v.trim().to_string();
            }
        }
    }

    /// Rewrite only the [Display] keys this app owns, leaving the rest of the
    /// file (keybinds, saves, player stats) exactly as it was.
    fn save_config(&mut self) {
        let cfg = self.root.join("config.ini");
        let mut text = std::fs::read_to_string(&cfg).unwrap_or_default();
        if text.is_empty() {
            text = default_config();
        }
        text = set_key(&text, "LineArt", &bool_str(self.line_art));
        text = set_key(&text, "LineArtDetail", &self.line_art_detail.to_string());
        text = set_key(&text, "StickFigure", &bool_str(self.stick_figure));
        text = set_key(&text, "Version", &self.version);
        if let Err(e) = std::fs::write(&cfg, text) {
            self.message = Some(format!("Could not write config.ini: {e}"));
        }
    }

    /// Import the USA assets from a GOG install folder or an offline installer.
    fn import_usa(&mut self, src: &Path) {
        self.message = Some(match inno::import_usa(&self.root, src) {
            Ok(n) => format!("USA imported: {n} files from {}", src.display()),
            Err(e) => format!("USA import failed: {e}"),
        });
        // The import just laid down the USA tree; make the config say so, so
        // the "assets in use" line and the game agree.
        self.version = "USA".to_string();
        self.save_config();
        self.refresh_regions();
    }

    /// Import the JPN assets from a Biohazard disc image or a folder.
    fn import_jpn(&mut self, src: &Path) {
        self.message = Some(match iso9660::import_jpn(&self.root, src) {
            Ok(n) => format!("JPN imported: {n} files."),
            Err(e) => format!("JPN import failed: {e}"),
        });
        self.version = "JPN".to_string();
        self.save_config();
        self.refresh_regions();
    }


    fn launch(&mut self) {
        self.save_config();
        // The compiled game binary and its 32-bit libraries are packed inside
        // this launcher; materialise them beside it on first run (the OS loader
        // needs real files to exec). Capcom's ripped assets already live in
        // self.root and are untouched.
        let bin = match embed::ensure_extracted(&self.root) {
            Some(b) => b,
            None => self.root.join("residentevil"),
        };
        if !bin.exists() {
            self.message = Some(format!("Game binary not found: {}", bin.display()));
            return;
        }
        let mut cmd = Command::new(&bin);
        cmd.current_dir(&self.root);
        // Point the dynamic loader at the bundled libraries. Unix only - on
        // Windows the DLLs sit beside the extracted exe and the loader finds
        // them there without any environment help.
        #[cfg(unix)]
        {
            let lib = embed::lib_dir(&self.root);
            if lib.is_dir() {
                let mut val = std::ffi::OsString::from(&lib);
                if let Some(old) = std::env::var_os("LD_LIBRARY_PATH") {
                    if !old.is_empty() {
                        val.push(":");
                        val.push(old);
                    }
                }
                cmd.env("LD_LIBRARY_PATH", val);
            }
        }
        // Detach so the launcher window is not tied to the game's lifetime.
        match cmd.spawn() {
            Ok(_) => self.message = Some("Game launched.".to_string()),
            Err(e) => self.message = Some(format!("Could not launch: {e}")),
        }
    }
}

fn region_installed(dir: &Path) -> bool {
    // The game resolves USA/Stage1 and USA/Data on every boot; those two are
    // the smallest reliable signal that a region was unpacked.
    dir.join("Stage1").is_dir() && dir.join("Data").is_dir()
}



fn bool_str(b: bool) -> String {
    if b { "1".to_string() } else { "0".to_string() }
}

/// What the player knows a version tag as. "USA" is the GOG/retail release,
/// "JPN" is the Japanese MediaKite release; the menus say that outright
/// instead of the folder name, so it is obvious which game is loaded.
pub fn version_label(v: &str) -> &'static str {
    match v {
        "JPN" => "MediaKite (JPN)",
        _     => "GOG (USA)",
    }
}

/// Replace `Key=...` in an ini text, adding it under [Display] if absent.
fn set_key(text: &str, key: &str, value: &str) -> String {
    let mut out = String::with_capacity(text.len() + 32);
    let mut replaced = false;
    let mut in_display = false;
    for line in text.lines() {
        let t = line.trim();
        if t.starts_with('[') {
            in_display = t.eq_ignore_ascii_case("[Display]");
        }
        if t.starts_with(&format!("{key}=")) {
            out.push_str(&format!("{key}={value}"));
            replaced = true;
        } else {
            out.push_str(line);
        }
        out.push('\n');
    }
    if !replaced {
        // Append under [Display] if we can find it, else at the end.
        if in_display || text.contains("[Display]") {
            let mut rebuilt = String::new();
            for line in out.lines() {
                rebuilt.push_str(line);
                rebuilt.push('\n');
                if line.trim().eq_ignore_ascii_case("[Display]") {
                    rebuilt.push_str(&format!("{key}={value}\n"));
                }
            }
            return rebuilt;
        }
        out.push_str(&format!("[Display]\n{key}={value}\n"));
    }
    out
}

/// The defaults match the config the game ships with, so a fresh install
/// behaves the same whether or not the launcher has been opened yet.
fn default_config() -> String {
    "[Display]\nFullScreen=1\nWidth=1280\nHeight=960\nBitDepth=32\nVSync=1\n\
     LineArt=1\nLineArtDetail=4\nStickFigure=1\n\
     [Assets]\nPath=\nVersion=USA\n[Save]\nPath=\n[Game]\nMode=OG\n\
     Ps1EndingCredits=0\n"
        .to_string()
}
