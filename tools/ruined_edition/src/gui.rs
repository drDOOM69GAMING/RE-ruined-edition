// ---------------------------------------------------------------------------
// gui.rs - the launcher window.
//
// One small window, one job: point it at the player's GOG folder, it copies
// the game data in, then it starts the game in line-art / stick-figure mode.
// eframe + egui are pure Rust, so the Linux build still drops into a
// deb/rpm/tarball with no system GUI libraries and the Windows build still
// cross-compiles with mingw alone.
//
// The folder picker is the same pick.rs the terminal menu uses - zenity or
// kdialog on Linux, the OS's own dialog on Windows. If neither helper exists
// the window shows a path field instead, so the import always has a route.
//
// The terminal menu (ui.rs) is still there: run with --tui, or over SSH with
// no DISPLAY, and the launcher falls back to it. Both front ends drive the
// same import and launch code.
// ---------------------------------------------------------------------------

use crate::pick::{ask_folder, Picked};
use crate::App;
use eframe::egui;
use std::sync::mpsc;

#[derive(Clone, Copy, PartialEq)]
enum Import {
    Usa,
    Jpn,
}

pub struct GuiApp {
    app: App,
    /// Result slot for the worker thread doing the copy, if one is running.
    rx: Option<mpsc::Receiver<Result<usize, String>>>,
    /// Which region the running import is for.
    importing: Option<Import>,
    /// Typed path, used when no dialog helper is available on this machine.
    typed: String,
    show_typed: bool,
}

/// True when a window can plausibly open here. The TUI is the fallback for
/// machines with no display (SSH, a bare TTY).
pub fn available() -> bool {
    #[cfg(windows)]
    {
        return true;
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        let set = |k: &str| std::env::var_os(k).map(|v| !v.is_empty()).unwrap_or(false);
        return set("DISPLAY") || set("WAYLAND_DISPLAY");
    }
    #[cfg(not(any(windows, all(unix, not(target_os = "macos")))))]
    {
        false
    }
}

/// Open the window. Returns false when the window could not be created, so
/// the caller can fall back to the terminal menu.
pub fn run(root: std::path::PathBuf) -> bool {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([460.0, 460.0])
            .with_min_inner_size([420.0, 400.0]),
        ..Default::default()
    };
    let app = App::load(root);
    match eframe::run_native(
        "Resident Evil - Ruined Edition",
        options,
        Box::new(move |_cc| Ok(Box::new(GuiApp::new(app)))),
    ) {
        Ok(_) => true,
        Err(e) => {
            eprintln!("ruined-edition: could not open the window ({e}); using the terminal menu");
            false
        }
    }
}

impl GuiApp {
    fn new(app: App) -> Self {
        GuiApp {
            app,
            rx: None,
            importing: None,
            typed: String::new(),
            show_typed: false,
        }
    }

    /// Ask for a source, probe it, then start the copy. A wrong pick never
    /// touches the disk: the probe only reads, and if it does not recognise
    /// the folder nothing is imported.
    fn pick(&mut self, which: Import) {
        let title = match which {
            Import::Usa => "Select your GOG install folder (the one with USA in it)",
            Import::Jpn => "Select your extracted Biohazard JPN folder or disc image",
        };
        match ask_folder(title) {
            Picked::Chosen(p) => self.probe_and_import(which, &p),
            Picked::Cancelled => self.app.message = Some("Cancelled.".into()),
            Picked::Unavailable => {
                self.show_typed = true;
                self.app.message =
                    Some("No dialog helper found - type the path below instead.".into());
            }
        }
    }

    fn probe_and_import(&mut self, which: Import, src: &std::path::Path) {
        let report = match which {
            Import::Usa => crate::probe::probe_usa(src),
            Import::Jpn => crate::probe::probe_jpn(src),
        };
        let expected = match which {
            Import::Usa => {
                matches!(
                    report.kind,
                    crate::probe::Kind::UsaFolder | crate::probe::Kind::GogInstaller
                )
            }
            Import::Jpn => {
                matches!(
                    report.kind,
                    crate::probe::Kind::JpnIso | crate::probe::Kind::JpnFolder
                )
            }
        };
        if !expected {
            self.app.message = Some(format!("{} Nothing was copied.", report.detail));
            return;
        }
        self.start_import(which, src.to_path_buf(), report.detail);
    }

    /// Copy on a worker thread so the window keeps responding; update() polls
    /// for the result and launches the game when it lands.
    fn start_import(&mut self, which: Import, src: std::path::PathBuf, detail: String) {
        let root = self.app.root.clone();
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            let r = match which {
                Import::Usa => crate::inno::import_usa(&root, &src),
                Import::Jpn => crate::iso9660::import_jpn(&root, &src),
            };
            let _ = tx.send(r);
        });
        self.rx = Some(rx);
        self.importing = Some(which);
        self.app.message = Some(format!("{detail} Copying..."));
    }

    fn poll_import(&mut self) {
        let done = match &self.rx {
            Some(rx) => rx.try_recv().ok(),
            None => None,
        };
        let res = match done {
            Some(r) => {
                self.rx = None;
                r
            }
            None => return,
        };
        let which = self.importing.take().unwrap_or(Import::Usa);
        match res {
            Ok(n) => {
                // The worker copied the tree for `which`; keep config.ini's
                // [Assets] Version in step so the "assets in use" line and the
                // game agree with what was just imported.
                self.app.version = match which {
                    Import::Usa => "USA".to_string(),
                    Import::Jpn => "JPN".to_string(),
                };
                self.app.save_config();
                self.app.refresh_regions();
                self.app.message = Some(format!("Game data imported: {n} files. Starting..."));
                // The whole point of the one-button flow: import, then play.
                self.app.launch();
            }
            Err(e) => {
                let tag = match which {
                    Import::Usa => "USA import failed",
                    Import::Jpn => "JPN import failed",
                };
                self.app.message = Some(format!("{tag}: {e}"));
            }
        }
    }

    fn busy(&self) -> bool {
        self.rx.is_some()
    }
}

impl eframe::App for GuiApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_import();
        if self.busy() {
            // The worker has no progress channel yet; keep repainting so the
            // spinner runs until try_recv succeeds.
            ctx.request_repaint_after(std::time::Duration::from_millis(100));
        }

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.add_space(4.0);
            ui.heading("Resident Evil - Ruined Edition");
            ui.label(egui::RichText::new("stick-figure cast, line-art mode").weak());
            ui.add_space(8.0);

            // --- game data -------------------------------------------------
            ui.label(egui::RichText::new("Game data").strong());
            for reg in &self.app.regions {
                ui.horizontal(|ui| {
                    let (text, colour) = if reg.installed {
                        ("[ ready ]", egui::Color32::from_rgb(0x2e, 0xa0, 0x43))
                    } else {
                        ("[ missing ]", egui::Color32::from_rgb(0xb0, 0x30, 0x30))
                    };
                    ui.colored_label(colour, text);
                    ui.label(reg.label);
                    if reg.name == self.app.version {
                        ui.colored_label(
                            egui::Color32::from_rgb(0x2e, 0xa0, 0x43),
                            "<- in use",
                        );
                    }
                });
            }
            ui.label(
                egui::RichText::new(format!(
                    "Game version in use: {}",
                    crate::version_label(&self.app.version)
                ))
                .strong(),
            );
            ui.add_space(6.0);

            // --- import ----------------------------------------------------
            ui.label(egui::RichText::new("Import your own copy").strong());
            ui.horizontal(|ui| {
                let b = ui.add_enabled(
                    !self.busy(),
                    egui::Button::new("GOG dir...").min_size(egui::vec2(110.0, 0.0)),
                );
                if b.clicked() {
                    self.pick(Import::Usa);
                }
                let b = ui.add_enabled(
                    !self.busy(),
                    egui::Button::new("JPN disc...").min_size(egui::vec2(110.0, 0.0)),
                );
                if b.clicked() {
                    self.pick(Import::Jpn);
                }
            });
            if self.show_typed {
                ui.horizontal(|ui| {
                    ui.label("path:");
                    let e = ui.add(
                        egui::TextEdit::singleline(&mut self.typed)
                            .desired_width(f32::INFINITY),
                    );
                    let enter = e.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter));
                    let go = ui.button("Import").clicked() || enter;
                    if go && !self.typed.trim().is_empty() {
                        let p = std::path::PathBuf::from(self.typed.trim().trim_matches('"'));
                        let which = if p.extension().map(|e| e.eq_ignore_ascii_case("iso"))
                            == Some(true)
                        {
                            Import::Jpn
                        } else {
                            Import::Usa
                        };
                        self.probe_and_import(which, &p);
                    }
                });
            }
            ui.add_space(6.0);

            // --- display settings ------------------------------------------
            ui.label(egui::RichText::new("Display").strong());
            ui.horizontal(|ui| {
                let switch = |ui: &mut egui::Ui, on: bool| {
                    let label = if on { "ON" } else { "OFF" };
                    ui.selectable_label(on, label).clicked()
                };
                ui.label("Line art:");
                if switch(ui, self.app.line_art) {
                    self.app.line_art = !self.app.line_art;
                    self.app.save_config();
                }
                ui.label("Stick figure:");
                if switch(ui, self.app.stick_figure) {
                    self.app.stick_figure = !self.app.stick_figure;
                    self.app.save_config();
                }
            });
            ui.horizontal(|ui| {
                ui.label("Detail:");
                if ui
                    .add(egui::Slider::new(&mut self.app.line_art_detail, 1..=5))
                    .changed()
                {
                    self.app.save_config();
                }
            });
            ui.add_space(10.0);

            // --- play ------------------------------------------------------
            let usa_ready = self.app.regions.iter().any(|r| r.name == "USA" && r.installed);
            let label = if usa_ready { "PLAY" } else { "PLAY (import your GOG copy first)" };
            let b = ui.add_enabled(
                !self.busy() && usa_ready,
                egui::Button::new(egui::RichText::new(label).size(20.0))
                    .min_size(egui::vec2(0.0, 44.0)),
            );
            if b.clicked() {
                self.app.launch();
            }
            ui.add_space(6.0);

            // --- status ----------------------------------------------------
            if let Some(m) = &self.app.message {
                if self.busy() {
                    ui.horizontal(|ui| {
                        ui.spinner();
                        ui.label(m);
                    });
                } else {
                    ui.label(m);
                }
            }
            ui.with_layout(egui::Layout::bottom_up(egui::Align::LEFT), |ui| {
                ui.label(
                    egui::RichText::new(
                        "This app ships without Capcom's game data. Point it at your own \
                         legally owned copy and it will set everything up.",
                    )
                    .weak(),
                );
            });
        });
    }
}
