use std::env;
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use anyhow::{Context, Result, bail};

const FORMAT_BATCH_SIZE: usize = 100;
const SUPPORTED_EXTENSIONS: &[&str] = &["c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx"];

fn run_output(command: &mut Command) -> Result<Output> {
    let description = format!("{command:?}");
    let output = command
        .output()
        .with_context(|| format!("failed to run {description}"))?;
    if !output.status.success() {
        bail!(
            "{description} failed:\n{}",
            String::from_utf8_lossy(&output.stderr).trim()
        );
    }
    Ok(output)
}

fn repository_root() -> Result<PathBuf> {
    let output = run_output(Command::new("git").args(["rev-parse", "--show-toplevel"]))?;
    let root =
        String::from_utf8(output.stdout).context("git returned a non-UTF-8 repository path")?;
    Ok(PathBuf::from(root.trim()))
}

fn zephyr_style(root: &Path) -> Result<PathBuf> {
    let candidates = [
        env::var_os("ZEPHYR_BASE").map(PathBuf::from),
        Some(root.join(".devenv/state/west/zephyr")),
    ];
    for base in candidates.into_iter().flatten() {
        let style = base.join(".clang-format");
        if style.is_file() {
            return Ok(style);
        }
    }
    bail!("cannot find Zephyr .clang-format; run cargo xtask fmt from nix develop")
}

fn supported_source(path: &Path) -> bool {
    path.extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| SUPPORTED_EXTENSIONS.contains(&extension))
}

fn tracked_sources(root: &Path, paths: &[OsString]) -> Result<Vec<PathBuf>> {
    let mut command = Command::new("git");
    command.current_dir(root).args(["ls-files", "-z", "--"]);
    if paths.is_empty() {
        command.arg(".");
    } else {
        command.args(paths);
    }
    let output = run_output(&mut command)?;
    let mut sources = output
        .stdout
        .split(|byte| *byte == 0)
        .filter(|entry| !entry.is_empty())
        .map(|entry| {
            String::from_utf8(entry.to_vec())
                .map(|entry| root.join(entry))
                .context("git returned a non-UTF-8 tracked path")
        })
        .collect::<Result<Vec<_>>>()?;
    sources.retain(|path| path.is_file() && supported_source(path));
    sources.sort();
    Ok(sources)
}

pub fn run(args: impl IntoIterator<Item = OsString>) -> Result<()> {
    let mut check = false;
    let mut paths = Vec::new();
    for arg in args {
        if arg == "--check" {
            check = true;
        } else if arg == "-h" || arg == "--help" {
            println!("cargo xtask fmt [--check] [PATH ...]");
            return Ok(());
        } else if arg.to_string_lossy().starts_with('-') {
            bail!("unknown fmt argument: {}", arg.to_string_lossy());
        } else {
            paths.push(arg);
        }
    }

    let root = repository_root()?;
    let style = zephyr_style(&root)?;
    let sources = tracked_sources(&root, &paths)?;
    if sources.is_empty() {
        bail!("no tracked C/C++ files matched");
    }

    println!(
        "{} {} tracked C/C++ files with {}",
        if check { "checking" } else { "formatting" },
        sources.len(),
        style.display()
    );
    for batch in sources.chunks(FORMAT_BATCH_SIZE) {
        let mut command = Command::new("clang-format");
        command.arg(format!("-style=file:{}", style.display()));
        if check {
            command.args(["--dry-run", "--Werror"]);
        } else {
            command.arg("-i");
        }
        command.args(batch);
        let status = command.status().context("failed to run clang-format")?;
        if !status.success() {
            bail!("clang-format reported formatting errors");
        }
    }
    Ok(())
}
