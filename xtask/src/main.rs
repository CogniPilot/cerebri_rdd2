mod fastdyn_ci;
mod format;
mod physics;
mod protocol;
mod shared_memory;

use std::env;
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail};
use physics::{Plant, radians_to_degrees};
use protocol::{MissionStatusWire, SyntheticGnss, WaypointPlanWire};
use serde::Serialize;
use shared_memory::LockstepOutputs;

/// RDD2's fixed 1600 Hz firmware control-loop period.
const CONTROLLER_DT: f64 = 0.000_625;
const TAKEOFF_ALTITUDE_M: f64 = 1.5;
const MISSION_SIDE_M: f64 = 0.5;
const MISSION_SPEED_M_S: f32 = 0.1;
const MISSION_ARM_DELAY_S: f64 = 0.5;
const MISSION_TAKEOFF_START_S: f64 = 1.0;
const MISSION_TAKEOFF_END_S: f64 = 4.0;
const MISSION_POSITION_START_S: f64 = 8.0;
const MISSION_POSITION_END_S: f64 = 34.0;
const MISSION_DISARM_S: f64 = 40.0;
const DEFAULT_MISSION_DURATION_S: f64 = 44.0;
const MINIMUM_MISSION_DURATION_S: f64 = MISSION_DISARM_S + 2.0;
const PLANNER_CORNER_TOLERANCE_M: f64 = 0.12;
const VEHICLE_CORNER_TOLERANCE_M: f64 = 0.25;

#[derive(Debug)]
struct Options {
    report: PathBuf,
    trajectory: PathBuf,
    duration: f64,
    response_timeout: Duration,
    controller_benchmark: Option<f64>,
    shared_memory: Option<PathBuf>,
    firmware_elf: Option<PathBuf>,
    native_sim: Option<PathBuf>,
    plant_library: PathBuf,
    plant_description: PathBuf,
    plant_dt: f64,
    minimum_speedup: f64,
}

#[derive(Debug, Default, Serialize)]
struct Report {
    passed: bool,
    /// Which providers produced the firmware under test. A pass built from
    /// caller-selected sources is not a qualifying result, and the report
    /// outlives the log that said so. Absent the variable this records
    /// `unknown` rather than implying the repository's pins were used.
    provider_mode: String,
    simulated_seconds: f64,
    wall_seconds: f64,
    speedup_over_realtime: f64,
    minimum_speedup_required: f64,
    plant_step_seconds: f64,
    controller_releases_observed: u64,
    plant_steps: u64,
    motor_messages: u64,
    flight_state_messages: u64,
    max_altitude_m: f64,
    max_tilt_deg: f64,
    max_roll_response_deg: f64,
    max_pitch_response_deg: f64,
    final_altitude_m: f64,
    final_vertical_speed_m_s: f64,
    firmware_armed_observed: bool,
    firmware_disarmed_after_flight: bool,
    firmware_rc_and_imu_healthy: bool,
    firmware_acro_observed: bool,
    firmware_attitude_observed: bool,
    firmware_position_observed: bool,
    gnss_source_ready_observed: bool,
    navigation_origin_observed: bool,
    mission_plan_accepted: bool,
    mission_pending_observed: bool,
    mission_running_observed: bool,
    fresh_planner_reference_observed: bool,
    position_mission_continuous: bool,
    mission_status_current: bool,
    planner_corners_reached: u8,
    vehicle_corners_reached: u8,
    navigation_estimate_finite: bool,
    max_navigation_horizontal_error_m: f64,
    maximum_gnss_generation: u32,
    maximum_plan_generation: u32,
    maximum_reference_generation: u32,
    release_rate_window_observed: bool,
    rate_releases_per_second: u32,
    navigation_releases_per_second: u32,
    guidance_releases_per_second: u32,
    planning_releases_per_second: u32,
    health_releases_per_second: u32,
    failures: Vec<String>,
}

fn options(args: impl IntoIterator<Item = String>) -> Result<Options> {
    let mut report = PathBuf::from("out/cerebri_rdd2_mission.json");
    let mut trajectory = env::var_os("RDD2_MISSION_TRAJECTORY")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("out/mission-trajectory.csv"));
    let mut duration: f64 = DEFAULT_MISSION_DURATION_S;
    let mut timeout_ms = 2_000_u64;
    let mut controller_benchmark = env::var("RDD2_FASTDYN_CONTROLLER_BENCHMARK_S")
        .ok()
        .map(|value| value.parse())
        .transpose()?;
    let mut shared_memory = env::var_os("RDD2_FASTDYN_SHARED_MEMORY").map(PathBuf::from);
    let mut firmware_elf = env::var_os("RDD2_FASTDYN_FIRMWARE_ELF").map(PathBuf::from);
    let mut native_sim = env::var_os("RDD2_NATIVE_SIM_EXECUTABLE").map(PathBuf::from);
    let mut plant_library = env::var_os("RDD2_RUMOCA_PLANT_LIBRARY").map(PathBuf::from);
    let mut plant_description = env::var_os("RDD2_RUMOCA_PLANT_DESCRIPTION").map(PathBuf::from);
    let mut minimum_speedup: f64 = env::var("RDD2_FASTDYN_MIN_SPEEDUP")
        .ok()
        .map(|value| value.parse())
        .transpose()?
        .unwrap_or(0.0);
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        let value = || anyhow!("{arg} requires a value");
        match arg.as_str() {
            "--report" => report = args.next().ok_or_else(value)?.into(),
            "--trajectory" => trajectory = args.next().ok_or_else(value)?.into(),
            "--duration" => duration = args.next().ok_or_else(value)?.parse()?,
            "--response-timeout-ms" => timeout_ms = args.next().ok_or_else(value)?.parse()?,
            "--controller-benchmark" => {
                controller_benchmark = Some(args.next().ok_or_else(value)?.parse()?)
            }
            "--shared-memory" => shared_memory = Some(args.next().ok_or_else(value)?.into()),
            "--firmware-elf" => firmware_elf = Some(args.next().ok_or_else(value)?.into()),
            "--native-sim" => native_sim = Some(args.next().ok_or_else(value)?.into()),
            "--plant-library" => plant_library = Some(args.next().ok_or_else(value)?.into()),
            "--plant-description" => {
                plant_description = Some(args.next().ok_or_else(value)?.into())
            }
            "--minimum-speedup" => minimum_speedup = args.next().ok_or_else(value)?.parse()?,
            "-h" | "--help" => {
                println!(
                    "cargo xtask fastdyn-mission --shared-memory PATH (--firmware-elf PATH | --native-sim PATH) --plant-library PATH --plant-description PATH [--report PATH] [--trajectory PATH] [--duration SEC] [--controller-benchmark SEC] [--minimum-speedup X]"
                );
                std::process::exit(0);
            }
            _ => bail!("unknown argument: {arg}"),
        }
    }
    if !duration.is_finite() || duration < MINIMUM_MISSION_DURATION_S {
        bail!("--duration must be finite and at least {MINIMUM_MISSION_DURATION_S} seconds");
    }
    if !minimum_speedup.is_finite() || minimum_speedup < 0.0 {
        bail!("--minimum-speedup must be finite and non-negative");
    }
    Ok(Options {
        report,
        trajectory,
        duration,
        response_timeout: Duration::from_millis(timeout_ms),
        controller_benchmark,
        shared_memory,
        firmware_elf,
        native_sim,
        plant_library: plant_library
            .context("--plant-library or RDD2_RUMOCA_PLANT_LIBRARY is required")?,
        plant_description: plant_description
            .context("--plant-description or RDD2_RUMOCA_PLANT_DESCRIPTION is required")?,
        plant_dt: CONTROLLER_DT,
        minimum_speedup,
    })
}

fn desired_altitude(time: f64) -> f64 {
    match time {
        t if t < MISSION_TAKEOFF_START_S => 0.0,
        t if t < MISSION_TAKEOFF_END_S => {
            (t - MISSION_TAKEOFF_START_S) * TAKEOFF_ALTITUDE_M
                / (MISSION_TAKEOFF_END_S - MISSION_TAKEOFF_START_S)
        }
        t if t < MISSION_POSITION_END_S => TAKEOFF_ALTITUDE_M,
        t if t < MISSION_DISARM_S => {
            TAKEOFF_ALTITUDE_M * (MISSION_DISARM_S - t)
                / (MISSION_DISARM_S - MISSION_POSITION_END_S)
        }
        _ => 0.0,
    }
}

fn throttle_must_be_low(time: f64) -> bool {
    !(MISSION_TAKEOFF_START_S..MISSION_DISARM_S).contains(&time)
}

fn rc_channels(mission_time: Option<f64>, plant: &Plant) -> [i32; 16] {
    let mut channels = [1500; 16];
    let Some(time) = mission_time else {
        channels[2] = 1000;
        channels[4] = 1000;
        channels[5] = 1000;
        return channels;
    };
    let arm_requested = (MISSION_ARM_DELAY_S..MISSION_DISARM_S).contains(&time);
    channels[4] = if arm_requested { 2000 } else { 1000 };
    channels[5] = if (MISSION_POSITION_START_S..MISSION_POSITION_END_S).contains(&time) {
        2000
    } else if arm_requested {
        1500
    } else {
        1000
    };
    if throttle_must_be_low(time) {
        channels[2] = 1000;
    } else {
        let error = desired_altitude(time) - plant.altitude();
        let normalized = (0.688 + 0.10 * error - 0.075 * plant.vertical_speed()).clamp(0.38, 0.82);
        channels[2] = (1000.0 + normalized * 1000.0).round() as i32;
    }
    channels
}

fn evaluate(report: &mut Report) {
    if report.speedup_over_realtime < report.minimum_speedup_required {
        report.failures.push(format!(
            "simulation speed {:.3}x is below required {:.3}x",
            report.speedup_over_realtime, report.minimum_speedup_required
        ));
    }
    if report.controller_releases_observed != report.plant_steps {
        report.failures.push(format!(
            "firmware returned {} controller releases for {} lockstep inputs",
            report.controller_releases_observed, report.plant_steps
        ));
    }
    if !report.release_rate_window_observed
        || report.rate_releases_per_second != 1_600
        || report.navigation_releases_per_second != 1_000
        || report.guidance_releases_per_second != 200
        || report.planning_releases_per_second != 50
        || report.health_releases_per_second != 200
    {
        report.failures.push(format!(
            "observed release rates were rate={} navigation={} guidance={} planning={} health={} Hz",
            report.rate_releases_per_second,
            report.navigation_releases_per_second,
            report.guidance_releases_per_second,
            report.planning_releases_per_second,
            report.health_releases_per_second
        ));
    }
    if !report.firmware_armed_observed {
        report.failures.push("firmware never armed".into());
    }
    if !report.firmware_disarmed_after_flight {
        report
            .failures
            .push("firmware did not disarm after landing".into());
    }
    if !report.firmware_rc_and_imu_healthy {
        report
            .failures
            .push("firmware did not report valid RC and IMU state".into());
    }
    if !report.firmware_acro_observed {
        report.failures.push("firmware did not enter ACRO".into());
    }
    if !report.firmware_attitude_observed {
        report
            .failures
            .push("firmware did not enter ATTITUDE".into());
    }
    if !report.firmware_position_observed {
        report
            .failures
            .push("firmware did not enter POSITION".into());
    }
    if !report.gnss_source_ready_observed {
        report
            .failures
            .push("lockstep GNSS source never became ready".into());
    }
    if !report.navigation_origin_observed {
        report
            .failures
            .push("navigation never established a GNSS origin".into());
    }
    if !report.mission_plan_accepted || !report.mission_pending_observed {
        report
            .failures
            .push("firmware never accepted the disarmed waypoint plan".into());
    }
    if !report.mission_running_observed {
        report
            .failures
            .push("waypoint mission never transitioned to RUNNING".into());
    }
    if !report.fresh_planner_reference_observed {
        report
            .failures
            .push("planner never published a current finite reference".into());
    }
    if !report.position_mission_continuous {
        report
            .failures
            .push("GPS/origin/RUNNING capability dropped during POSITION".into());
    }
    if report.planner_corners_reached != 5 {
        report.failures.push(format!(
            "planner visited only {}/5 ordered square corners",
            report.planner_corners_reached
        ));
    }
    if report.vehicle_corners_reached != 5 {
        report.failures.push(format!(
            "vehicle visited only {}/5 ordered square corners",
            report.vehicle_corners_reached
        ));
    }
    if report.maximum_plan_generation != 1 {
        report.failures.push(format!(
            "waypoint plan generation must advance exactly once, observed {}",
            report.maximum_plan_generation
        ));
    }
    let expected_gnss_generations = (report.simulated_seconds * 10.0).floor() as u32 + 1;
    if report.maximum_gnss_generation != expected_gnss_generations {
        report.failures.push(format!(
            "GNSS generation was {}, expected exactly {}",
            report.maximum_gnss_generation, expected_gnss_generations
        ));
    }
    if !report.mission_status_current {
        report
            .failures
            .push("mission status did not match the current control release".into());
    }
    if report.max_navigation_horizontal_error_m > 1.0 {
        report.failures.push(format!(
            "GPS navigation horizontal error {:.3} m exceeded 1.000 m",
            report.max_navigation_horizontal_error_m
        ));
    }
    if !report.navigation_estimate_finite {
        report
            .failures
            .push("GPS navigation estimate became non-finite".into());
    }
    if report.maximum_reference_generation <= 1 {
        report.failures.push(format!(
            "planner reference did not advance beyond pending hold: generation {}",
            report.maximum_reference_generation
        ));
    }
    if report.flight_state_messages < 20 {
        report
            .failures
            .push("too few VehicleHealth messages".into());
    }
    if report.max_altitude_m < 1.0 {
        report
            .failures
            .push("vehicle did not take off above 1 m".into());
    }
    if report.max_tilt_deg > 45.0 {
        report
            .failures
            .push("vehicle exceeded 45 degrees tilt".into());
    }
    if report.final_altitude_m > 0.20 {
        report
            .failures
            .push("vehicle did not land within 0.20 m".into());
    }
    if report.final_vertical_speed_m_s.abs() > 0.50 {
        report
            .failures
            .push("vehicle had excessive vertical speed at completion".into());
    }
    report.passed = report.failures.is_empty();
    report.provider_mode =
        env::var("RDD2_PROVIDER_MODE").unwrap_or_else(|_| "unknown".to_string());
}

fn advance_square_corner(
    progress: &mut u8,
    origin: [f64; 2],
    position: [f64; 2],
    side_m: f64,
    tolerance_m: f64,
) {
    let corners = [
        [0.0, 0.0],
        [side_m, 0.0],
        [side_m, side_m],
        [0.0, side_m],
        [0.0, 0.0],
    ];
    let Some(corner) = corners.get(usize::from(*progress)) else {
        return;
    };
    let error_x = position[0] - origin[0] - corner[0];
    let error_y = position[1] - origin[1] - corner[1];
    if error_x.hypot(error_y) <= tolerance_m {
        *progress += 1;
    }
}

fn write_report(path: &PathBuf, report: &Report) -> Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(path, serde_json::to_vec_pretty(report)?)
        .with_context(|| format!("cannot write report {}", path.display()))
}

fn trajectory_writer(path: &Path) -> Result<BufWriter<File>> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let mut writer = BufWriter::new(
        File::create(path).with_context(|| format!("cannot create {}", path.display()))?,
    );
    writeln!(writer, "time_s,x_m,y_m,z_m,roll_rad,pitch_rad,yaw_rad")?;
    Ok(writer)
}

fn run_controller_benchmark<F>(options: &Options, exchange: &mut F) -> Result<()>
where
    F: FnMut(&protocol::LockstepInputs, Duration) -> Result<LockstepOutputs>,
{
    let benchmark_seconds = options
        .controller_benchmark
        .context("controller benchmark duration is missing")?;
    if !benchmark_seconds.is_finite() || benchmark_seconds <= 0.0 {
        bail!("--controller-benchmark must be a positive finite duration");
    }
    let plant = Plant::open(&options.plant_library, &options.plant_description)?;
    let (gyro, accel) = plant.imu_flu();
    let mut synthetic_gnss = SyntheticGnss::default();
    let mut channels = [1500; 16];
    channels[2] = 1000;
    channels[4] = 1000;
    channels[5] = 1000;
    let warmup_target_ns = 5_000_000_u64;
    let ready_deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let fix = synthetic_gnss.sample(plant.position(), plant.velocity(), warmup_target_ns);
        let inputs = protocol::lockstep_inputs(
            gyro,
            accel,
            channels,
            fix,
            WaypointPlanWire::default(),
            warmup_target_ns,
        );
        if exchange(&inputs, options.response_timeout).is_ok() {
            break;
        }
        if Instant::now() >= ready_deadline {
            bail!("firmware did not become ready for controller benchmark");
        }
    }

    let target_ns = warmup_target_ns + (benchmark_seconds * 1.0e9).round() as u64;
    let fix = synthetic_gnss.sample(plant.position(), plant.velocity(), target_ns);
    let inputs = protocol::lockstep_inputs(
        gyro,
        accel,
        channels,
        fix,
        WaypointPlanWire::default(),
        target_ns,
    );
    let start = Instant::now();
    exchange(&inputs, Duration::from_secs(120))?;
    let wall_seconds = start.elapsed().as_secs_f64();
    println!(
        "RDD2_CONTROLLER_BENCHMARK simulated_s={benchmark_seconds:.6} wall_s={wall_seconds:.6} speedup={:.3}x",
        benchmark_seconds / wall_seconds
    );
    Ok(())
}

fn run_mission<F>(options: &Options, exchange: &mut F) -> Result<()>
where
    F: FnMut(&protocol::LockstepInputs, Duration) -> Result<LockstepOutputs>,
{
    let mut plant = Plant::open(&options.plant_library, &options.plant_description)?;
    let mut report = Report {
        position_mission_continuous: true,
        mission_status_current: true,
        navigation_estimate_finite: true,
        ..Report::default()
    };
    let mut simulated_time = 0.0_f64;
    let wall_start = Instant::now();
    let mut firmware_ready = false;
    let mut synthetic_gnss = SyntheticGnss::default();
    let mission_plan = protocol::bounded_square_plan(1, MISSION_SIDE_M as f32, MISSION_SPEED_M_S)?;
    let mut plan_sent = false;
    let mut mission_epoch = None;
    let mut last_outputs: Option<LockstepOutputs> = None;
    let mut planner_origin: Option<[f64; 2]> = None;
    let mut vehicle_origin: Option<[f64; 2]> = None;
    let mut navigation_truth_origin: Option<[f64; 2]> = None;
    let mut release_window_start: Option<(u64, MissionStatusWire)> = None;
    let mut motor_generation_origin: Option<u32> = None;
    let mut final_motor_generation = 0_u32;
    let mut health_generation_origin: Option<u32> = None;
    let mut final_health_generation = 0_u32;
    let mut trajectory = trajectory_writer(&options.trajectory)?;

    let mission_steps = (options.duration / options.plant_dt).round() as u64;
    while report.plant_steps < mission_steps {
        if !plan_sent
            && last_outputs.as_ref().is_some_and(|outputs| {
                let required = MissionStatusWire::SOURCE_READY | MissionStatusWire::ORIGIN_VALID;
                outputs.mission_status.flags & required == required
                    && outputs.odometry_estimate.quality_pct() > 0
            })
        {
            plan_sent = true;
        }
        let mission_time = mission_epoch.map(|epoch| simulated_time - epoch);
        let channels = rc_channels(mission_time, &plant);
        let (gyro, accel) = plant.imu_flu();
        let target_time = ((simulated_time + options.plant_dt) * 1.0e9).round() as u64;
        let fix = synthetic_gnss.sample(plant.position(), plant.velocity(), target_time);
        let plan = if plan_sent {
            mission_plan
        } else {
            WaypointPlanWire::default()
        };
        let inputs = protocol::lockstep_inputs(gyro, accel, channels, fix, plan, target_time);
        let outputs = match exchange(&inputs, options.response_timeout) {
            Ok(response) => response,
            Err(error) if !firmware_ready && wall_start.elapsed() < Duration::from_secs(60) => {
                eprintln!("waiting for rehosted RDD2 firmware: {error}");
                continue;
            }
            Err(error) => return Err(error),
        };
        if !firmware_ready {
            firmware_ready = true;
        }

        plant.step(outputs.motor_command.values, options.plant_dt)?;
        simulated_time += options.plant_dt;
        report.plant_steps += 1;
        let state = outputs.flight_state;
        report.firmware_rc_and_imu_healthy |= state.rc_valid && state.imu_ok;
        report.firmware_acro_observed |= state.flight_mode == 0;
        report.firmware_attitude_observed |= state.flight_mode == 1;
        report.firmware_position_observed |= state.flight_mode == 2;
        report.firmware_armed_observed |= state.armed;
        let status = outputs.mission_status;
        motor_generation_origin.get_or_insert(status.motor_generation.saturating_sub(1));
        health_generation_origin.get_or_insert(status.health_generation.saturating_sub(1));
        final_motor_generation = status.motor_generation;
        final_health_generation = status.health_generation;
        report.mission_status_current &= status.timestamp_ns == target_time;
        report.gnss_source_ready_observed |= status.flags & MissionStatusWire::SOURCE_READY != 0;
        report.navigation_origin_observed |= status.flags & MissionStatusWire::ORIGIN_VALID != 0;
        report.mission_plan_accepted |= status.flags & MissionStatusWire::PLAN_ACCEPTED != 0;
        report.mission_pending_observed |= status.mission_state == 1;
        report.mission_running_observed |= status.mission_state == 2;
        report.maximum_gnss_generation = report.maximum_gnss_generation.max(status.gnss_generation);
        report.maximum_plan_generation = report.maximum_plan_generation.max(status.plan_generation);
        report.maximum_reference_generation = report
            .maximum_reference_generation
            .max(status.reference_generation);
        if release_window_start.is_none()
            && status.mission_state == 2
            && state.flight_mode == 2
            && status.guidance_generation > 0
        {
            release_window_start = Some((target_time, status));
        }
        if !report.release_rate_window_observed
            && let Some((window_start_ns, baseline)) = release_window_start
            && target_time - window_start_ns >= 1_000_000_000
        {
            report.rate_releases_per_second = status
                .motor_generation
                .wrapping_sub(baseline.motor_generation);
            report.navigation_releases_per_second = status
                .odometry_generation
                .wrapping_sub(baseline.odometry_generation);
            report.guidance_releases_per_second = status
                .guidance_generation
                .wrapping_sub(baseline.guidance_generation);
            report.planning_releases_per_second = status
                .reference_generation
                .wrapping_sub(baseline.reference_generation);
            report.health_releases_per_second = status
                .health_generation
                .wrapping_sub(baseline.health_generation);
            report.release_rate_window_observed = true;
        }
        let reference = outputs.planner_reference;
        let reference_position = reference.position_enu_m();
        let reference_current = reference.timestamp_ns() != 0
            && reference.timestamp_ns() <= target_time
            && target_time - reference.timestamp_ns() <= 100_000_000
            && reference.type_mask() == 0
            && reference.coordinate_frame() == synapse_fbs::types::LocalFrame::LocalEnu
            && [
                reference_position.x(),
                reference_position.y(),
                reference_position.z(),
                reference.yaw_rad(),
                reference.yaw_rate_rad_s(),
            ]
            .into_iter()
            .all(f32::is_finite);
        report.fresh_planner_reference_observed |= reference_current;
        if mission_time.is_some_and(|time| {
            (MISSION_POSITION_START_S + 0.1..MISSION_POSITION_END_S).contains(&time)
        }) {
            let required = MissionStatusWire::SOURCE_READY
                | MissionStatusWire::ORIGIN_VALID
                | MissionStatusWire::PLAN_ACCEPTED;
            report.position_mission_continuous &= status.flags & required == required
                && status.mission_state == 2
                && reference_current;
        }
        if status.mission_state == 2 && state.flight_mode == 2 && reference_current {
            let planner_position = [
                f64::from(reference_position.x()),
                f64::from(reference_position.y()),
            ];
            let truth_position = plant.position();
            let planner_origin_value = *planner_origin.get_or_insert(planner_position);
            let vehicle_origin_value =
                *vehicle_origin.get_or_insert([truth_position[0], truth_position[1]]);
            advance_square_corner(
                &mut report.planner_corners_reached,
                planner_origin_value,
                planner_position,
                MISSION_SIDE_M,
                PLANNER_CORNER_TOLERANCE_M,
            );
            advance_square_corner(
                &mut report.vehicle_corners_reached,
                vehicle_origin_value,
                [truth_position[0], truth_position[1]],
                MISSION_SIDE_M,
                VEHICLE_CORNER_TOLERANCE_M,
            );
        }
        if status.flags & MissionStatusWire::ORIGIN_VALID != 0
            && outputs.odometry_estimate.quality_pct() > 0
        {
            let truth = plant.position();
            let origin = *navigation_truth_origin.get_or_insert([truth[0], truth[1]]);
            let estimate = outputs.odometry_estimate.position_enu_m();
            let error_e = f64::from(estimate.x()) - (truth[0] - origin[0]);
            let error_n = f64::from(estimate.y()) - (truth[1] - origin[1]);
            if error_e.is_finite() && error_n.is_finite() {
                report.max_navigation_horizontal_error_m = report
                    .max_navigation_horizontal_error_m
                    .max(error_e.hypot(error_n));
            } else {
                report.navigation_estimate_finite = false;
            }
        }
        if mission_epoch.is_none() && status.mission_state == 1 {
            mission_epoch = Some(simulated_time);
        }
        if mission_time.is_some_and(|time| time >= MISSION_DISARM_S + 0.25) && !state.armed {
            report.firmware_disarmed_after_flight = true;
        }

        let euler = plant.euler();
        let position = plant.position();
        writeln!(
            trajectory,
            "{simulated_time:.9},{:.9},{:.9},{:.9},{:.9},{:.9},{:.9}",
            position[0], position[1], position[2], euler[0], euler[1], euler[2]
        )?;
        let roll = radians_to_degrees(euler[0]).abs();
        let pitch = radians_to_degrees(euler[1]).abs();
        report.max_altitude_m = report.max_altitude_m.max(plant.altitude());
        report.max_tilt_deg = report.max_tilt_deg.max(roll.max(pitch));
        if mission_time.is_some_and(|time| (2.0..2.9).contains(&time)) {
            report.max_roll_response_deg = report.max_roll_response_deg.max(roll);
        }
        if mission_time.is_some_and(|time| (2.9..3.8).contains(&time)) {
            report.max_pitch_response_deg = report.max_pitch_response_deg.max(pitch);
        }
        let progress_steps = (1.0 / options.plant_dt).round() as u64;
        if report.plant_steps.is_multiple_of(progress_steps) {
            println!(
                "[rdd2-mission] t={simulated_time:.1}s alt={:.2}m vz={:.2}m/s tilt={:.1}deg armed={} gps_ready={} origin={} mission_state={}",
                plant.altitude(),
                plant.vertical_speed(),
                roll.max(pitch),
                state.armed,
                status.flags & MissionStatusWire::SOURCE_READY != 0,
                status.flags & MissionStatusWire::ORIGIN_VALID != 0,
                status.mission_state,
            );
        }
        last_outputs = Some(outputs);
    }

    report.simulated_seconds = report.plant_steps as f64 * options.plant_dt;
    report.wall_seconds = wall_start.elapsed().as_secs_f64();
    report.speedup_over_realtime = report.simulated_seconds / report.wall_seconds;
    report.minimum_speedup_required = options.minimum_speedup;
    report.plant_step_seconds = options.plant_dt;
    report.controller_releases_observed =
        u64::from(final_motor_generation.wrapping_sub(motor_generation_origin.unwrap_or(0)));
    report.motor_messages = report.controller_releases_observed;
    report.flight_state_messages =
        u64::from(final_health_generation.wrapping_sub(health_generation_origin.unwrap_or(0)));
    report.final_altitude_m = plant.altitude();
    report.final_vertical_speed_m_s = plant.vertical_speed();
    trajectory.flush()?;
    evaluate(&mut report);
    write_report(&options.report, &report)?;
    println!(
        "RDD2_MISSION_RESULT passed={} simulated_s={:.3} wall_s={:.3} speedup={:.2}x max_alt_m={:.2} report={}",
        report.passed,
        report.simulated_seconds,
        report.wall_seconds,
        report.speedup_over_realtime,
        report.max_altitude_m,
        options.report.display(),
    );
    if !report.passed {
        bail!("RDD2 mission failed: {}", report.failures.join("; "));
    }
    Ok(())
}

fn run_shared_memory(options: &Options, memory_path: &PathBuf) -> Result<()> {
    let firmware_elf = options.firmware_elf.as_ref().ok_or_else(|| {
        anyhow!("--firmware-elf or RDD2_FASTDYN_FIRMWARE_ELF is required with shared memory")
    })?;
    println!(
        "RDD2 mission runner connected to FastDyn shared memory {}",
        memory_path.display()
    );
    let mut transport =
        shared_memory::Transport::open(memory_path, firmware_elf, Duration::from_secs(60))?;
    let mut exchange =
        |inputs: &protocol::LockstepInputs, timeout: Duration| transport.exchange(inputs, timeout);
    if options.controller_benchmark.is_some() {
        run_controller_benchmark(options, &mut exchange)
    } else {
        run_mission(options, &mut exchange)
    }
}

fn stop_native_sim(child: &mut std::process::Child) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(2);
    while child.try_wait()?.is_none() && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(10));
    }
    if child.try_wait()?.is_none() {
        child.kill()?;
    }
    let status = child.wait()?;
    if !status.success() && status.code().is_some_and(|code| code != 9) {
        bail!("native simulator exited with {status}");
    }
    Ok(())
}

fn run_native_sim(options: &Options, memory_path: &Path, executable: &Path) -> Result<()> {
    if let Some(parent) = memory_path.parent() {
        fs::create_dir_all(parent)?;
    }
    let mut transport = shared_memory::Transport::create_direct(memory_path)?;
    let mut child = Command::new(executable)
        .env("RDD2_LOCKSTEP_SHM", memory_path)
        .spawn()
        .with_context(|| format!("cannot launch native simulator {}", executable.display()))?;
    println!(
        "RDD2 mission runner connected to native simulator {}",
        executable.display()
    );
    let mut exchange =
        |inputs: &protocol::LockstepInputs, timeout: Duration| transport.exchange(inputs, timeout);
    let result = if options.controller_benchmark.is_some() {
        run_controller_benchmark(options, &mut exchange)
    } else {
        run_mission(options, &mut exchange)
    };
    drop(exchange);
    drop(transport);
    let stop_result = stop_native_sim(&mut child);
    result.and(stop_result)
}

fn run_fastdyn_mission(args: impl IntoIterator<Item = String>) -> Result<()> {
    let options = options(args)?;
    let memory_path = options.shared_memory.as_ref().ok_or_else(|| {
        anyhow!("--shared-memory or RDD2_FASTDYN_SHARED_MEMORY is required; lockstep does not use CSyn/ZROS transport")
    })?;
    if let Some(executable) = &options.native_sim {
        run_native_sim(&options, memory_path, executable)
    } else {
        run_shared_memory(&options, memory_path)
    }
}

fn print_help() {
    println!(
        "cargo xtask <command>\n\nCommands:\n  fastdyn-ci       Launch FastDyn and validate the complete CI mission\n  fastdyn-mission  Run the FMI plant and firmware lockstep mission\n  fmt              Format tracked C/C++ sources with Zephyr style"
    );
}

fn main() -> Result<()> {
    let mut args = env::args_os().skip(1);
    let Some(command) = args.next() else {
        print_help();
        return Ok(());
    };

    match command.to_string_lossy().as_ref() {
        "fastdyn-ci" => fastdyn_ci::run(),
        "fastdyn-mission" => {
            let args = args
                .map(|arg| {
                    arg.into_string()
                        .map_err(|_| anyhow!("fastdyn-mission arguments must be valid UTF-8"))
                })
                .collect::<Result<Vec<_>>>()?;
            run_fastdyn_mission(args)
        }
        "fmt" => format::run(args),
        "-h" | "--help" | "help" => {
            print_help();
            Ok(())
        }
        command => bail!("unknown xtask command: {command}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Stdio;

    fn nominal_report() -> Report {
        Report {
            speedup_over_realtime: 12.0,
            minimum_speedup_required: 10.0,
            firmware_armed_observed: true,
            firmware_disarmed_after_flight: true,
            firmware_rc_and_imu_healthy: true,
            firmware_acro_observed: true,
            firmware_attitude_observed: true,
            firmware_position_observed: true,
            gnss_source_ready_observed: true,
            navigation_origin_observed: true,
            mission_plan_accepted: true,
            mission_pending_observed: true,
            mission_running_observed: true,
            fresh_planner_reference_observed: true,
            position_mission_continuous: true,
            mission_status_current: true,
            planner_corners_reached: 5,
            vehicle_corners_reached: 5,
            navigation_estimate_finite: true,
            max_navigation_horizontal_error_m: 0.1,
            simulated_seconds: DEFAULT_MISSION_DURATION_S,
            maximum_gnss_generation: 441,
            maximum_reference_generation: 100,
            maximum_plan_generation: 1,
            release_rate_window_observed: true,
            rate_releases_per_second: 1_600,
            navigation_releases_per_second: 1_000,
            guidance_releases_per_second: 200,
            planning_releases_per_second: 50,
            health_releases_per_second: 200,
            controller_releases_observed: 70_400,
            plant_steps: 70_400,
            motor_messages: 70_400,
            flight_state_messages: 100,
            max_altitude_m: 2.0,
            max_roll_response_deg: 5.0,
            max_pitch_response_deg: 5.0,
            max_tilt_deg: 20.0,
            final_altitude_m: 0.05,
            final_vertical_speed_m_s: 0.0,
            ..Report::default()
        }
    }

    #[test]
    fn evaluate_passes_a_nominal_mission() {
        let mut report = nominal_report();
        evaluate(&mut report);
        assert!(report.passed, "unexpected failures: {:?}", report.failures);
        assert!(report.failures.is_empty());
    }

    #[test]
    fn takeoff_schedule_preserves_a_low_throttle_arming_window() {
        assert!(MISSION_ARM_DELAY_S < MISSION_TAKEOFF_START_S);
        assert!(MISSION_TAKEOFF_END_S < MISSION_POSITION_START_S);
        let arming_time = (MISSION_ARM_DELAY_S + MISSION_TAKEOFF_START_S) * 0.5;
        assert!(throttle_must_be_low(arming_time));
        assert_eq!(desired_altitude(arming_time), 0.0);
        assert!(!throttle_must_be_low(MISSION_TAKEOFF_START_S));
        assert_eq!(desired_altitude(MISSION_TAKEOFF_END_S), TAKEOFF_ALTITUDE_M);
        assert_eq!(
            desired_altitude(MISSION_POSITION_START_S),
            TAKEOFF_ALTITUDE_M
        );
    }

    #[test]
    fn conservative_square_oracle_requires_real_motion() {
        let origin = [2.0, -3.0];
        let mut progress = 0;

        advance_square_corner(
            &mut progress,
            origin,
            origin,
            MISSION_SIDE_M,
            VEHICLE_CORNER_TOLERANCE_M,
        );
        assert_eq!(progress, 1);
        advance_square_corner(
            &mut progress,
            origin,
            origin,
            MISSION_SIDE_M,
            VEHICLE_CORNER_TOLERANCE_M,
        );
        assert_eq!(progress, 1, "the origin must not satisfy the next corner");
        advance_square_corner(
            &mut progress,
            origin,
            [origin[0] + MISSION_SIDE_M, origin[1]],
            MISSION_SIDE_M,
            VEHICLE_CORNER_TOLERANCE_M,
        );
        assert_eq!(progress, 2);
    }

    #[test]
    fn evaluate_flags_each_violated_requirement() {
        let mut report = nominal_report();
        report.speedup_over_realtime = 9.0;
        report.firmware_armed_observed = false;
        report.max_tilt_deg = 50.0;
        report.final_altitude_m = 0.5;
        evaluate(&mut report);
        assert!(!report.passed);
        assert_eq!(report.failures.len(), 4, "failures: {:?}", report.failures);
    }

    #[test]
    fn evaluate_rejects_position_capability_dropout() {
        let mut report = nominal_report();
        report.position_mission_continuous = false;
        evaluate(&mut report);
        assert_eq!(report.failures.len(), 1, "failures: {:?}", report.failures);
        assert!(report.failures[0].contains("dropped during POSITION"));
    }

    #[test]
    fn evaluate_rejects_frozen_gnss_and_pending_only_reference() {
        let mut report = nominal_report();
        report.maximum_gnss_generation = 319;
        report.maximum_reference_generation = 1;
        evaluate(&mut report);
        assert_eq!(report.failures.len(), 2, "failures: {:?}", report.failures);
        assert!(
            report
                .failures
                .iter()
                .any(|failure| failure.contains("expected exactly"))
        );
        assert!(
            report
                .failures
                .iter()
                .any(|failure| failure.contains("pending hold"))
        );
    }

    #[test]
    fn evaluate_rejects_running_without_square_motion() {
        let mut report = nominal_report();
        report.planner_corners_reached = 1;
        report.vehicle_corners_reached = 1;
        evaluate(&mut report);
        assert_eq!(report.failures.len(), 2, "failures: {:?}", report.failures);
        assert!(
            report
                .failures
                .iter()
                .all(|failure| failure.contains("square corners"))
        );
    }

    #[test]
    fn evaluate_rejects_replayed_or_republished_plan() {
        let mut report = nominal_report();
        report.maximum_plan_generation = 2;
        evaluate(&mut report);
        assert_eq!(report.failures.len(), 1, "failures: {:?}", report.failures);
        assert!(report.failures[0].contains("exactly once"));
    }

    #[test]
    fn evaluate_rejects_stale_status_and_navigation_divergence() {
        let mut report = nominal_report();
        report.mission_status_current = false;
        report.max_navigation_horizontal_error_m = 1.01;
        evaluate(&mut report);
        assert_eq!(report.failures.len(), 2, "failures: {:?}", report.failures);
        assert!(
            report
                .failures
                .iter()
                .any(|failure| failure.contains("control release"))
        );
        assert!(
            report
                .failures
                .iter()
                .any(|failure| failure.contains("horizontal error"))
        );
    }

    #[test]
    #[ignore = "requires a built direct-lockstep native firmware executable"]
    fn native_firmware_exposes_odometry_before_the_one_shot_plan() -> Result<()> {
        let executable = env::var_os("RDD2_NATIVE_SIM_EXECUTABLE")
            .context("RDD2_NATIVE_SIM_EXECUTABLE is required for this integration test")?;
        let memory_path = env::temp_dir().join(format!(
            "rdd2-gps-ingress-test-{}-{}.bin",
            std::process::id(),
            Instant::now().elapsed().as_nanos()
        ));
        let mut transport = shared_memory::Transport::create_direct(&memory_path)?;
        let mut child = Command::new(&executable)
            .env("RDD2_LOCKSTEP_SHM", &memory_path)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .with_context(|| format!("cannot launch {}", PathBuf::from(executable).display()))?;
        let mut gnss = SyntheticGnss::default();
        let mut channels = [1500; 16];
        channels[2] = 1000;
        channels[4] = 1000;
        channels[5] = 1000;
        let mut target_ns = 0_u64;
        let mut empty_odometry_observed = false;

        let result = (|| -> Result<()> {
            for _ in 0..4_000 {
                target_ns += 625_000;
                let fix = gnss.sample([0.0; 3], [0.0; 3], target_ns);
                let inputs = protocol::lockstep_inputs(
                    [0.0; 3],
                    [0.0, 0.0, 9.806_65],
                    channels,
                    fix,
                    WaypointPlanWire::default(),
                    target_ns,
                );
                let outputs = transport.exchange(&inputs, Duration::from_secs(2))?;
                let required = MissionStatusWire::SOURCE_READY | MissionStatusWire::ORIGIN_VALID;
                if outputs.mission_status.flags & required == required
                    && outputs.odometry_estimate.quality_pct() > 0
                {
                    assert_eq!(outputs.mission_status.mission_state, 0);
                    assert_eq!(outputs.mission_status.plan_generation, 0);
                    assert_eq!(outputs.mission_status.reference_generation, 0);
                    assert_eq!(outputs.planner_reference.timestamp_ns(), 0);
                    empty_odometry_observed = true;
                    break;
                }
            }
            assert!(
                empty_odometry_observed,
                "EMPTY mission state never exposed initialized GPS odometry"
            );

            let plan = protocol::bounded_square_plan(1, 1.0, 0.3)?;
            let mut pending_observed = false;
            let mut last_status = MissionStatusWire::default();
            let mut last_quality = 0_i8;
            for _ in 0..320 {
                target_ns += 625_000;
                let fix = gnss.sample([0.0; 3], [0.0; 3], target_ns);
                let inputs = protocol::lockstep_inputs(
                    [0.0; 3],
                    [0.0, 0.0, 9.806_65],
                    channels,
                    fix,
                    plan,
                    target_ns,
                );
                let outputs = transport.exchange(&inputs, Duration::from_secs(2))?;
                last_status = outputs.mission_status;
                last_quality = outputs.odometry_estimate.quality_pct();
                if outputs.mission_status.mission_state == 1 {
                    assert_eq!(outputs.mission_status.plan_generation, 1);
                    assert!(outputs.mission_status.reference_generation > 0);
                    pending_observed = true;
                    break;
                }
            }
            assert!(
                pending_observed,
                "one-shot plan never reached PENDING: status={last_status:?} quality={last_quality}"
            );

            let baseline = last_status;
            let mut last_motor_values = [0.0_f32; 4];
            let mut last_flight_armed = false;
            for _ in 0..1_600 {
                target_ns += 625_000;
                let fix = gnss.sample([0.0; 3], [0.0; 3], target_ns);
                let inputs = protocol::lockstep_inputs(
                    [0.0; 3],
                    [0.0, 0.0, 9.806_65],
                    channels,
                    fix,
                    plan,
                    target_ns,
                );
                let outputs = transport.exchange(&inputs, Duration::from_secs(2))?;
                last_status = outputs.mission_status;
                last_motor_values = outputs.motor_command.values;
                last_flight_armed = outputs.flight_state.armed;
                assert_eq!(last_status.timestamp_ns, target_ns);
            }
            assert_eq!(last_status.gnss_generation - baseline.gnss_generation, 10);
            assert_eq!(last_status.plan_generation - baseline.plan_generation, 0);
            assert_eq!(
                last_status.reference_generation - baseline.reference_generation,
                50
            );
            assert_eq!(
                last_status.odometry_generation - baseline.odometry_generation,
                1_000
            );
            assert_eq!(
                last_status.guidance_generation - baseline.guidance_generation,
                200,
                "guidance must publish safe commands at its exact release rate"
            );
            assert_eq!(
                last_status.motor_generation - baseline.motor_generation,
                1_600
            );
            assert_eq!(
                last_status.health_generation - baseline.health_generation,
                200
            );
            assert!(!last_flight_armed, "PENDING window unexpectedly armed");
            assert_eq!(
                last_motor_values, [0.0; 4],
                "disarmed PENDING must keep every motor output at zero"
            );
            Ok(())
        })();
        drop(transport);
        let stop_result = stop_native_sim(&mut child);
        let _ = fs::remove_file(memory_path);
        result.and(stop_result)
    }
}
