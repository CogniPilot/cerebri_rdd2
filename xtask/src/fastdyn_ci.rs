use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail};
use serde_json::{Value, json};

fn repository_root() -> Result<PathBuf> {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .map(Path::to_path_buf)
        .context("xtask must be a direct child of the repository root")
}

fn env_path(name: &str, default: impl FnOnce() -> PathBuf) -> PathBuf {
    env::var_os(name).map(PathBuf::from).unwrap_or_else(default)
}

fn required_env_path(name: &str) -> Result<PathBuf> {
    env::var_os(name)
        .map(PathBuf::from)
        .ok_or_else(|| anyhow!("set {name}"))
}

fn fastdyn_executable(fastdyn_root: &Path) -> PathBuf {
    if let Some(path) = env::var_os("FASTDYN_EXECUTABLE") {
        return path.into();
    }
    let candidate = fastdyn_root
        .join("fastdyn-env")
        .join(if cfg!(windows) { "Scripts" } else { "bin" })
        .join(if cfg!(windows) {
            "fastdyn.exe"
        } else {
            "fastdyn"
        });
    if candidate.is_file() {
        candidate
    } else {
        "fastdyn".into()
    }
}

fn tail(path: &Path, count: usize) -> String {
    fs::read_to_string(path)
        .map(|text| {
            let lines = text.lines().collect::<Vec<_>>();
            lines[lines.len().saturating_sub(count)..].join("\n")
        })
        .unwrap_or_default()
}

fn report_number(report: &Value, name: &str) -> Result<f64> {
    report[name]
        .as_f64()
        .ok_or_else(|| anyhow!("mission report has no numeric {name}"))
}

pub fn run() -> Result<()> {
    let repository_root = repository_root()?;
    let fastdyn_root = required_env_path("FASTDYN_ROOT")?;
    let workspace_root = required_env_path("RDD2_WORKSPACE_ROOT")?;
    let monitor_elf = env_path("FASTDYN_MONITOR_ELF", || {
        fastdyn_root.join("build/qemu/ws/monitor.elf")
    });
    let qemu = env_path("FASTDYN_QEMU_PATH", || {
        fastdyn_root.join("build/qemu/build/qemu-system-arm")
    });
    let firmware_build = env_path("RDD2_FASTDYN_BUILD_DIR", || {
        repository_root.join("build-mr_vmu_tropic-fastdyn")
    });
    let config = env_path("FASTDYN_RDD2_CONFIG", || {
        repository_root.join("fastdyn/mr_vmu_tropic.toml")
    });
    let work_dir = env_path("FASTDYN_RDD2_WORK_DIR", || {
        repository_root.join("artifacts/bil/work")
    });
    let report_path = env_path("FASTDYN_RDD2_REPORT", || {
        work_dir.join("cerebri_rdd2_mission.json")
    });
    let trajectory = env_path("FASTDYN_RDD2_TRAJECTORY", || {
        work_dir.join("mission-trajectory.csv")
    });
    let log_path = env_path("FASTDYN_RDD2_LOG", || {
        repository_root.join("artifacts/bil/mission.log")
    });
    let timeout = Duration::from_secs(
        env::var("FASTDYN_RDD2_TIMEOUT_SEC")
            .unwrap_or_else(|_| "1200".into())
            .parse()
            .context("FASTDYN_RDD2_TIMEOUT_SEC must be an integer")?,
    );

    fs::create_dir_all(&work_dir)?;
    if let Some(parent) = log_path.parent() {
        fs::create_dir_all(parent)?;
    }
    let log =
        File::create(&log_path).with_context(|| format!("cannot create {}", log_path.display()))?;
    let log_stderr = log.try_clone()?;
    let _ = fs::remove_file(&report_path);
    let _ = fs::remove_file(&trajectory);

    println!("[ci] launching rehosted cerebri_rdd2 mission");
    let start = Instant::now();
    let mut child = Command::new(fastdyn_executable(&fastdyn_root))
        .current_dir(&fastdyn_root)
        .args(["run", "-c"])
        .arg(&config)
        .arg("-o")
        .arg(&work_dir)
        .env("CEREBRI_RDD2_ROOT", &repository_root)
        .env("FASTDYN_INSTALL_ROOT", &fastdyn_root)
        .env("FASTDYN_MONITOR_ELF", &monitor_elf)
        .env("FASTDYN_QEMU_PATH", &qemu)
        .env("RDD2_FASTDYN_BUILD_DIR", &firmware_build)
        .env("RDD2_WORKSPACE_ROOT", &workspace_root)
        .env("RDD2_FASTDYN_REPORT", &report_path)
        .env("RDD2_MISSION_TRAJECTORY", &trajectory)
        .env("FASTDYN_QEMU_MEMORY_DIR", work_dir.join("memory"))
        .env(
            "FASTDYN_QMP_SOCKET",
            work_dir.join("fastdyn-rdd2-ci-qmp.sock"),
        )
        .stdout(Stdio::from(log))
        .stderr(Stdio::from(log_stderr))
        .spawn()
        .context("cannot launch FastDyn")?;

    let deadline = start + timeout;
    let mut stopped_after_report = false;
    let status = 'mission: loop {
        if report_path
            .metadata()
            .is_ok_and(|metadata| metadata.len() > 0)
            && trajectory
                .metadata()
                .is_ok_and(|metadata| metadata.len() > 0)
        {
            stopped_after_report = true;
            let graceful_deadline = Instant::now() + Duration::from_secs(10);
            loop {
                if let Some(status) = child.try_wait()? {
                    break 'mission status;
                }
                if Instant::now() >= graceful_deadline {
                    child
                        .kill()
                        .context("cannot stop FastDyn after mission completion")?;
                    break 'mission child.wait()?;
                }
                thread::sleep(Duration::from_millis(100));
            }
        }
        if let Some(status) = child.try_wait()? {
            break status;
        }
        if Instant::now() >= deadline {
            child.kill().context("cannot stop timed-out FastDyn")?;
            let _ = child.wait();
            bail!(
                "FastDyn mission timed out after {} seconds\n{}",
                timeout.as_secs(),
                tail(&log_path, 200)
            );
        }
        thread::sleep(Duration::from_millis(100));
    };

    if !report_path
        .metadata()
        .is_ok_and(|metadata| metadata.len() > 0)
        || !trajectory
            .metadata()
            .is_ok_and(|metadata| metadata.len() > 0)
    {
        bail!(
            "RDD2 mission did not produce its report and trajectory ({status})\n{}",
            tail(&log_path, 200)
        );
    }
    if !stopped_after_report && !status.success() {
        bail!("FastDyn exited with {status} after producing the mission report");
    }

    let mut report: Value = serde_json::from_slice(&fs::read(&report_path)?)?;
    let simulated = report_number(&report, "simulated_seconds")?;
    let wall = start.elapsed().as_secs_f64();
    let speedup = simulated / wall;
    let object = report
        .as_object_mut()
        .context("mission report root must be a JSON object")?;
    object.insert("overall_wall_seconds".into(), json!(wall));
    object.insert("overall_speedup_over_realtime".into(), json!(speedup));
    fs::write(&report_path, serde_json::to_vec_pretty(&report)?)?;

    let passed = report["passed"]
        .as_bool()
        .context("mission report has no boolean passed field")?;
    let mission_speedup = report_number(&report, "speedup_over_realtime")?;
    let max_altitude = report_number(&report, "max_altitude_m")?;
    println!(
        "[ci] RDD2 mission passed={passed} simulated={simulated:.3}s mission_speedup={mission_speedup:.2}x launch_wall={wall:.3}s overall_speedup={speedup:.2}x max_alt={max_altitude:.2}m"
    );

    if let Some(summary_path) = env::var_os("GITHUB_STEP_SUMMARY") {
        let mut summary = OpenOptions::new()
            .create(true)
            .append(true)
            .open(summary_path)?;
        writeln!(summary, "## FastDyn + cerebri_rdd2 mission\n")?;
        writeln!(
            summary,
            "| Result | Simulated time | Mission speedup | Launch wall time | Overall launch speedup | Max altitude |"
        )?;
        writeln!(summary, "|---|---:|---:|---:|---:|---:|")?;
        writeln!(
            summary,
            "| {passed} | {simulated:.3} s | **{mission_speedup:.2}x** | {wall:.3} s | {speedup:.2}x | {max_altitude:.2} m |"
        )?;
    }

    if !passed {
        let failures = report["failures"]
            .as_array()
            .map(|failures| {
                failures
                    .iter()
                    .filter_map(Value::as_str)
                    .collect::<Vec<_>>()
                    .join("; ")
            })
            .unwrap_or_else(|| "unspecified mission failure".into());
        bail!("RDD2 mission failed: {failures}");
    }
    Ok(())
}
