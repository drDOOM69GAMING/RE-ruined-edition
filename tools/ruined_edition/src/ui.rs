// Terminal menu for the launcher - the fallback front end to the window in
// gui.rs. No GUI toolkit is used here: the menu must still run from a desktop
// icon that opens a terminal, over SSH, or in a plain TTY, and it takes over
// automatically when the window cannot open. ANSI colour is used only when
// stdout is a terminal.

use crate::App;
use std::io::{self, Write};

const BOLD: &str = "\x1b[1m";
const DIM: &str = "\x1b[2m";
const RED: &str = "\x1b[31m";
const GREEN: &str = "\x1b[32m";
const CYAN: &str = "\x1b[36m";
const RESET: &str = "\x1b[0m";

fn colour(code: &str) -> &str {
    // Only colourise a real terminal; piped output stays plain for logs.
    if is_tty() {
        code
    } else {
        ""
    }
}

fn is_tty() -> bool {
    // SAFETY: isatty on fd 1 is always safe.
    unsafe { libc_isatty(1) == 1 }
}

extern "C" {
    #[link_name = "isatty"]
    fn libc_isatty(fd: i32) -> i32;
}

/// One import flow: pick, inspect, confirm, then copy.
///
/// The point of doing it in this order is that a wrong pick never touches the
/// player's disk. The probe only reads, and if it does not recognise the file
/// the import is skipped and the player can pick again.
fn import_flow(app: &mut App, which: Which) {
    let (title, files, folder) = match which {
        Which::Usa => (
            "Select your GOG game data: the USA folder, or your GOG setup .exe",
            &[("GOG setup program", "*.exe")][..],
            "Select your GOG install folder (the one with USA in it)",
        ),
        Which::Jpn => (
            "Select your Biohazard JPN disc image",
            &[("Disc image", "*.iso")][..],
            "Select your extracted Biohazard JPN folder",
        ),
    };

    // A file dialog first, because that is the common case. If the player
    // really wants a folder, or the file dialog is not available, offer the
    // folder dialog and then the typed prompt.
    let chosen = match crate::pick::ask_file(title, files) {
        crate::pick::Picked::Chosen(p) => Some(p),
        crate::pick::Picked::Cancelled => None,
        crate::pick::Picked::Unavailable => {
            // No dialog on this machine: folder dialog, then typed path.
            match crate::pick::ask_folder(folder) {
                crate::pick::Picked::Chosen(p) => Some(p),
                _ => ask_path("Path (type or drag it here)"),
            }
        }
    };

    let path = match chosen {
        Some(p) => p,
        None => {
            app.message = Some("Cancelled.".into());
            return;
        }
    };

    // Inspect before copying anything.
    let report = match which {
        Which::Usa => crate::probe::probe_usa(&path),
        Which::Jpn => crate::probe::probe_jpn(&path),
    };

    use crate::probe::Kind;
    let expected = match which {
        Which::Usa => matches!(report.kind, Kind::UsaFolder | Kind::GogInstaller),
        Which::Jpn => matches!(report.kind, Kind::JpnIso | Kind::JpnFolder),
    };

    println!("  {}", report.detail);
    if !expected {
        println!("  That is not what I was looking for. Nothing was copied.");
        app.message = Some("Wrong file. Nothing was copied; try again.".into());
        return;
    }
    if !confirm("  Import this? [Y/n] ") {
        app.message = Some("Cancelled. Nothing was copied.".into());
        return;
    }

    match which {
        Which::Usa => app.import_usa(&path),
        Which::Jpn => app.import_jpn(&path),
    }
}

#[derive(PartialEq, Clone, Copy)]
enum Which {
    Usa,
    Jpn,
}

pub fn run(app: &mut App) {
    clear();
    loop {
        draw(app);
        let choice = prompt("> ");
        match choice.trim() {
            "1" => import_flow(app, Which::Usa),
            "2" => import_flow(app, Which::Jpn),
            "3" => {
                app.line_art = !app.line_art;
                app.save_config();
            }
            "4" => {
                app.line_art_detail = if app.line_art_detail >= 5 { 1 } else { app.line_art_detail + 1 };
                app.save_config();
            }
            "5" => {
                app.stick_figure = !app.stick_figure;
                app.save_config();
            }
            "6" => {
                app.version = if app.version == "USA" { "JPN".into() } else { "USA".into() };
                app.save_config();
            }
            "s" | "S" => {
                app.save_config();
                app.message = Some("Settings saved to config.ini".into());
            }
            "q" | "Q" | "quit" | "exit" => {
                clear();
                println!("Launching Resident Evil...");
                app.launch();
                if let Some(m) = &app.message {
                    println!("{m}");
                }
                // Give the player a moment to read a failure before the
                // terminal closes under a desktop icon.
                if app.message.as_deref().map(|m| m.starts_with("Could not launch")).unwrap_or(false) {
                    let _ = prompt("Press Enter to close");
                }
                return;
            }
            "" => {
                clear();
                println!("Launching Resident Evil...");
                app.launch();
                if let Some(m) = &app.message {
                    println!("{m}");
                }
                return;
            }
            _ => app.message = Some("Unknown choice - type a number, or q to play.".into()),
        }
    }
}

fn draw(app: &App) {
    clear();
    let b = colour(BOLD);
    let d = colour(DIM);
    let g = colour(GREEN);
    let r = colour(RED);
    let c = colour(CYAN);
    let z = colour(RESET);

    println!("{b}{c}  Resident Evil - Ruined Edition{z}");
    println!("{d}  stick-figure cast, line-art mode{z}");
    println!();
    println!("{b}  Game data{z}");
    for reg in &app.regions {
        let mark = if reg.installed {
            format!("{g}[ ready ]{z}")
        } else {
            format!("{r}[ missing ]{z}")
        };
        println!("    {mark} {:<32} {}", reg.label, reg.name);
    }
    println!();
    println!("{b}  Display settings{z}  {d}(edit any time, saved to config.ini){z}");
    println!(
        "    3. Line-art mode ............ {}",
        on_off(app.line_art)
    );
    println!(
        "    4. Line-art detail ......... {} / 5",
        app.line_art_detail
    );
    println!(
        "    5. Stick-figure cast ....... {}",
        on_off(app.stick_figure)
    );
    println!(
        "    6. Game version ............ {}",
        crate::version_label(&app.version)
    );
    println!("{d}    in game: F7 line-art, Shift+F7 more detail,{z}");
    println!("{d}             Ctrl+Shift+F7 less, Ctrl+R reset,{z}");
    println!("{d}             F1 cast, F10 quit{z}");
    println!();
    println!("{b}  Import your own copy of the game{z}");
    println!("    1. Import USA assets ....... opens a file picker");
    println!("    2. Import JPN assets ....... opens a file picker");
    println!();
    println!("    s. Save settings");
    println!("    q. Play  {d}(or just press Enter){z}");
    println!();
    println!("{d}  This app ships without Capcom's game data. Point it at your own{z}");
    println!("{d}  legally owned copy above and it will set everything up.{z}");
    println!();

    if let Some(m) = &app.message {
        let m = m.replace('\n', "\n     ");
        println!("  {b}*{z} {m}\n");
    }
}

fn on_off(b: bool) -> String {
    if b {
        format!("{}ON{}", colour(GREEN), colour(RESET))
    } else {
        format!("{}OFF{}", colour(DIM), colour(RESET))
    }
}

fn prompt(label: &str) -> String {
    print!("{label}");
    let _ = io::stdout().flush();
    let mut s = String::new();
    match io::stdin().read_line(&mut s) {
        Ok(0) => "q".to_string(), // EOF closes the window
        Ok(_) => s,
        Err(_) => "q".to_string(),
    }
}

/// Yes/no, defaulting to yes on a bare Enter.
fn confirm(label: &str) -> bool {
    let a = prompt(label);
    let t = a.trim().to_ascii_lowercase();
    t.is_empty() || t == "y" || t == "yes"
}

fn ask_path(question: &str) -> Option<std::path::PathBuf> {
    println!("  {question}");
    println!("  (drag the file or folder here, then press Enter; blank cancels)");
    let raw = prompt("  path> ");
    let trimmed = raw.trim().trim_matches('"').trim_matches('\'');
    if trimmed.is_empty() {
        None
    } else {
        Some(std::path::PathBuf::from(trimmed))
    }
}

fn clear() {
    if is_tty() {
        print!("\x1b[2J\x1b[H");
        let _ = io::stdout().flush();
    }
}
