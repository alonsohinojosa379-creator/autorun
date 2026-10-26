//! NRO output: the adapter of the [`Packer`] port that does not build an NSP.
//!
//! Instead of hacBrewPack it writes a folder that is copied onto the SD card as it is:
//!
//! - `switch/wine/drive_c/Games/<folder>/` — the game with the settings files Carafe renders next to the `.exe`;
//! - `switch/Carafe/<Title>.nro` — a small launcher with the game's name and icon that hbmenu lists;
//! - `switch/Carafe/<Title>.argv` — what the launcher starts: the runtime `.nro` and the path to the `.exe`.
//!
//! The runtime itself (`switch/wine/wine-nx-runtime.nro` and Wine's prefix) is not packed: it is installed once.
//! A small index `<Title ID>.carafe` goes into the library so that the app lists, rebuilds and deletes the game.

use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

use carafe_core::BuildRecord;
use carafe_core::cancel::CancelToken;
use carafe_core::icon::IconJpeg;
use carafe_core::package::{GamePackage, ICON_FILE, RECORD_FILE, package};
use carafe_core::pfs0::{self, Pfs0Entry};
use carafe_core::ports::{AdapterError, BuildProgress, BuildStage, PackTarget, Packer, StoredGame};

use crate::copy::Tree;
use crate::{create_dir, io_error, remove_dir, report, service, write};

/// Extension of the index file in the library.
pub const INDEX_EXTENSION: &str = "carafe";

const LAUNCHER_DIR: &str = "switch/Carafe";
const ASSET_MAGIC: &[u8; 4] = b"ASET";
const ASSET_HEADER_SIZE: usize = 0x38;
const NACP_SIZE: usize = 0x4000;

/// Builds a folder for the SD card with an NRO launcher per game.
pub struct NroPacker {
    stub: PathBuf,
    runtime: Option<PathBuf>,
}

impl NroPacker {
    /// `stub` is the launcher program, a homebrew `.nro` without an asset section.
    pub fn new(stub: impl Into<PathBuf>) -> Self {
        Self {
            stub: stub.into(),
            runtime: None,
        }
    }

    /// Also unpacks the runtime from Carafe's `carafe-runtime.bin` into `<library>/runtime-sd` the first time,
    /// so that the folder to copy to the SD card carries `switch/wine/wine-nx-runtime.nro` and Wine's prefix.
    #[must_use]
    pub fn with_runtime(mut self, archive: impl Into<PathBuf>) -> Self {
        self.runtime = Some(archive.into());
        self
    }

    fn unpack_runtime(
        &self,
        library: &Path,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(BuildProgress),
    ) -> Result<(), AdapterError> {
        let dest = library.join("runtime-sd");
        if dest.join("switch/wine/wine-nx-runtime.nro").is_file() {
            return Ok(());
        }
        let Some(archive) = self.runtime.as_ref().filter(|path| path.is_file()) else {
            report(
                progress,
                BuildStage::Runtime,
                100,
                Some("no carafe-runtime.bin found: install the official Carafe or put the file next to this app".to_owned()),
            );
            return Ok(());
        };
        let partial = library.join("runtime-sd.part");
        remove_dir(&partial)?;
        create_dir(&partial)?;
        let runtime = crate::archive::RuntimeArchive::open(archive)?;
        runtime.extract("romfs", &partial, cancel, &mut |percent| {
            report(
                progress,
                BuildStage::Runtime,
                percent,
                Some("runtime → runtime-sd".to_owned()),
            );
        })?;
        remove_dir(&dest)?;
        fs::rename(&partial, &dest).map_err(|error| io_error(&dest, &error))
    }

    fn build(
        &self,
        record: &BuildRecord,
        icon: Option<&IconJpeg>,
        package: &GamePackage,
        target: &PackTarget,
        out: &Path,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(BuildProgress),
    ) -> Result<u64, AdapterError> {
        let game_folder = Path::new(&record.source.folder);
        let game = Tree::scan(game_folder)?;
        let available = fs4::available_space(out.parent().unwrap_or(out))
            .map_err(|error| io_error(out, &error))?;
        carafe_core::disk_space::check_space(game.total_bytes(), available)
            .map_err(|error| AdapterError::NoSpace(format!("{}: {error}", target.library_dir)))?;

        report(
            progress,
            BuildStage::Game,
            0,
            Some(format!(
                "game: {} → {}",
                game_folder.display(),
                package.game_dir
            )),
        );
        game.copy_to(&out.join(&package.game_dir), cancel, &mut |percent| {
            report(progress, BuildStage::Game, percent, None);
        })?;

        cancel.check()?;
        report(progress, BuildStage::Metadata, 0, None);
        let mut argv = None;
        for file in &package.romfs_files {
            if file.path == "carafe/argv" {
                argv = Some(file.contents.clone());
                continue;
            }
            // Loading screen, console menu and registry belong to the NSP overlay.
            if file.path.starts_with("carafe/") {
                continue;
            }
            let path = out.join(&file.path);
            if let Some(parent) = path.parent() {
                create_dir(parent)?;
            }
            write(&path, file.contents.as_bytes())?;
        }
        let argv =
            argv.ok_or_else(|| AdapterError::Tool("carafe/argv was not rendered".to_owned()))?;

        let name = launcher_name(&record.metadata.title);
        let dir = out.join(LAUNCHER_DIR);
        create_dir(&dir)?;
        write(
            &dir.join(format!("{name}.argv")),
            format!("{argv}\n").as_bytes(),
        )?;
        let stub = fs::read(&self.stub).map_err(|error| io_error(&self.stub, &error))?;
        let icon_bytes = icon.map(|icon| icon.bytes().to_vec()).unwrap_or_default();
        let nro = with_assets(&stub, &icon_bytes, &package.nacp)?;
        write(&dir.join(format!("{name}.nro")), &nro)?;
        report(
            progress,
            BuildStage::Metadata,
            100,
            Some(format!("{LAUNCHER_DIR}/{name}.nro")),
        );
        Ok(game.total_bytes())
    }
}

impl Packer for NroPacker {
    fn pack(
        &self,
        record: &BuildRecord,
        icon: Option<&IconJpeg>,
        target: &PackTarget,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(BuildProgress),
    ) -> Result<StoredGame, AdapterError> {
        cancel.check()?;
        let package = package(record).map_err(|error| AdapterError::Tool(error.to_string()))?;
        let library = Path::new(&target.library_dir);
        let work = service::build_dir(library, &record.title_id.to_string());
        if service::is_busy(&work) {
            return Err(service::already_running(&work));
        }
        remove_dir(&work)?;
        create_dir(&work)?;
        service::hide(&library.join(service::SERVICE_DIR));
        let lock = service::BuildLock::acquire(&work)?;
        let staging = work.join("sd");
        if let Err(error) = self.unpack_runtime(library, cancel, progress) {
            drop(lock);
            let _ = remove_dir(&work);
            return Err(error);
        }
        let result = self.build(record, icon, &package, target, &staging, cancel, progress);
        let finished = result.and_then(|bytes| {
            cancel.check()?;
            report(progress, BuildStage::Finish, 0, None);
            let final_dir = library.join(format!("{}-sd", record.title_id));
            remove_dir(&final_dir)?;
            fs::rename(&staging, &final_dir).map_err(|error| io_error(&final_dir, &error))?;
            let file_name = format!("{}.{INDEX_EXTENSION}", record.title_id);
            write_index(record, icon, &library.join(&file_name))?;
            report(
                progress,
                BuildStage::Finish,
                100,
                Some(format!(
                    "copy the contents of {} to the SD card",
                    final_dir.display()
                )),
            );
            Ok(StoredGame {
                record: record.clone(),
                file_name,
                size_bytes: bytes,
                icon: icon.cloned(),
            })
        });
        drop(lock);
        let _ = remove_dir(&work);
        finished
    }

    fn clean(&self, library_dir: &str) -> Result<usize, AdapterError> {
        service::clean(Path::new(library_dir))
    }
}

/// A file name for the launcher: the title without characters the SD card does not take.
fn launcher_name(title: &str) -> String {
    let cleaned: String = title
        .chars()
        .map(|c| {
            if c.is_control() || "<>:\"/\\|?*".contains(c) {
                '_'
            } else {
                c
            }
        })
        .collect();
    let trimmed = cleaned.trim().trim_end_matches('.');
    if trimmed.is_empty() {
        "Game".to_owned()
    } else {
        trimmed.to_owned()
    }
}

/// Appends the asset section that hbmenu reads for the name and the icon: `ASET`, a version,
/// then offset and size of the icon, the NACP and RomFS, each from the start of the section.
fn with_assets(stub: &[u8], icon: &[u8], nacp: &[u8]) -> Result<Vec<u8>, AdapterError> {
    if stub.len() < 0x14 || &stub[0x10..0x14] != b"NRO0" {
        return Err(AdapterError::Tool(
            "the launcher is not an NRO file".to_owned(),
        ));
    }
    if nacp.len() != NACP_SIZE {
        return Err(AdapterError::Tool(format!(
            "control.nacp is {} bytes, expected {NACP_SIZE}",
            nacp.len()
        )));
    }
    let mut out = stub.to_vec();
    while out.len() % 0x10 != 0 {
        out.push(0);
    }
    let icon_offset = ASSET_HEADER_SIZE as u64;
    let nacp_offset = icon_offset + icon.len() as u64;
    let romfs_offset = nacp_offset + nacp.len() as u64;
    out.extend_from_slice(ASSET_MAGIC);
    out.extend_from_slice(&0u32.to_le_bytes());
    for (offset, size) in [
        (icon_offset, icon.len() as u64),
        (nacp_offset, nacp.len() as u64),
        (romfs_offset, 0),
    ] {
        out.extend_from_slice(&offset.to_le_bytes());
        out.extend_from_slice(&size.to_le_bytes());
    }
    out.extend_from_slice(icon);
    out.extend_from_slice(nacp);
    Ok(out)
}

/// Writes the library index: a PFS0 with the build record and the icon, and no NCA.
fn write_index(
    record: &BuildRecord,
    icon: Option<&IconJpeg>,
    path: &Path,
) -> Result<(), AdapterError> {
    let json = serde_json::to_vec_pretty(record)
        .map_err(|error| AdapterError::Io(format!("{RECORD_FILE}: {error}")))?;
    let mut extras: Vec<(&str, &[u8])> = vec![(RECORD_FILE, &json)];
    if let Some(icon) = icon {
        extras.push((ICON_FILE, icon.bytes()));
    }
    let entries: Vec<Pfs0Entry> = extras
        .iter()
        .map(|(name, bytes)| Pfs0Entry {
            name: (*name).to_owned(),
            size: bytes.len() as u64,
        })
        .collect();
    let header = pfs0::header(&entries).map_err(|error| AdapterError::Tool(error.to_string()))?;
    let partial = path.with_extension("carafe.part");
    let failed = |error: std::io::Error| io_error(path, &error);
    let mut out = BufWriter::new(File::create(&partial).map_err(failed)?);
    out.write_all(&header).map_err(failed)?;
    for (_, bytes) in &extras {
        out.write_all(bytes).map_err(failed)?;
    }
    out.flush().map_err(failed)?;
    drop(out);
    fs::rename(&partial, path).map_err(failed)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_asset_section_follows_the_image() {
        let mut stub = vec![0u8; 0x80];
        stub[0x10..0x14].copy_from_slice(b"NRO0");
        let nro = with_assets(&stub, &[1, 2, 3], &vec![9u8; NACP_SIZE]).unwrap();
        assert_eq!(&nro[0x80..0x84], b"ASET");
        assert_eq!(
            u64::from_le_bytes(nro[0x88..0x90].try_into().unwrap()),
            0x38
        );
        assert_eq!(u64::from_le_bytes(nro[0x90..0x98].try_into().unwrap()), 3);
        assert_eq!(nro.len(), 0x80 + 0x38 + 3 + NACP_SIZE);
    }

    #[test]
    fn a_title_becomes_a_safe_file_name() {
        assert_eq!(launcher_name("Medal: of/Honor."), "Medal_ of_Honor");
        assert_eq!(launcher_name("  "), "Game");
    }
}
