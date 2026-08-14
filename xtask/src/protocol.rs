use anyhow::{Context, Result, bail};
use synapse_fbs::{
    topic,
    types::{GnssFixType, TimeStatus, Vec3f},
};

const GNSS_PERIOD_NS: u64 = 100_000_000;
const GNSS_ORIGIN_LATITUDE_DEG: f64 = 40.4237;
const GNSS_ORIGIN_LONGITUDE_DEG: f64 = -86.9212;
const GNSS_ORIGIN_ALTITUDE_MSL_M: f64 = 200.0;
const EARTH_RADIUS_M: f64 = 6_378_137.0;

pub const RDD2_MAX_WAYPOINTS: usize = 16;
const MISSION_WAYPOINT_COUNT: i32 = 5;
const MISSION_SIDE_MIN_M: f32 = 0.5;
const MISSION_SIDE_MAX_M: f32 = 3.0;
const MISSION_SPEED_MIN_M_S: f32 = 0.1;
const MISSION_SPEED_MAX_M_S: f32 = 0.5;
const MISSION_MIN_SEGMENT_DURATION_S: f32 = 2.0;

#[derive(Clone, Copy, Debug, Default)]
pub struct MotorCommand {
    pub values: [f32; 4],
}

#[derive(Clone, Copy, Debug, Default)]
pub struct FlightState {
    pub armed: bool,
    pub rc_valid: bool,
    pub imu_ok: bool,
    pub flight_mode: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WaypointPlanWire {
    pub sequence: i32,
    pub waypoint_count: i32,
    pub origin_geodetic: [f32; 3],
    pub waypoint: [[f32; 3]; RDD2_MAX_WAYPOINTS],
    pub velocity_enu: [[f32; 3]; RDD2_MAX_WAYPOINTS],
    pub yaw: [f32; RDD2_MAX_WAYPOINTS],
    pub nominal_speed: f32,
    pub min_segment_duration: f32,
    pub valid: u8,
    pub global_frame: u8,
    padding: [u8; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MissionStatusWire {
    pub timestamp_ns: u64,
    pub plan_sequence: i32,
    pub gnss_generation: u32,
    pub plan_generation: u32,
    pub reference_generation: u32,
    pub odometry_generation: u32,
    pub guidance_generation: u32,
    pub motor_generation: u32,
    pub health_generation: u32,
    pub mission_state: u8,
    pub flags: u8,
    pub reserved: [u8; 6],
}

impl MissionStatusWire {
    pub const SOURCE_READY: u8 = 1;
    pub const ORIGIN_VALID: u8 = 2;
    pub const PLAN_ACCEPTED: u8 = 4;
}

impl Default for WaypointPlanWire {
    fn default() -> Self {
        Self {
            sequence: 0,
            waypoint_count: 0,
            origin_geodetic: [0.0; 3],
            waypoint: [[0.0; 3]; RDD2_MAX_WAYPOINTS],
            velocity_enu: [[0.0; 3]; RDD2_MAX_WAYPOINTS],
            yaw: [0.0; RDD2_MAX_WAYPOINTS],
            nominal_speed: 0.0,
            min_segment_duration: 0.0,
            valid: 0,
            global_frame: 0,
            padding: [0; 2],
        }
    }
}

pub fn bounded_square_plan(sequence: i32, side_m: f32, speed_m_s: f32) -> Result<WaypointPlanWire> {
    if sequence <= 0 {
        bail!("waypoint plan sequence must be positive");
    }
    if !side_m.is_finite() || !(MISSION_SIDE_MIN_M..=MISSION_SIDE_MAX_M).contains(&side_m) {
        bail!("waypoint square side must be finite and within 0.5..=3.0 m");
    }
    if !speed_m_s.is_finite()
        || !(MISSION_SPEED_MIN_M_S..=MISSION_SPEED_MAX_M_S).contains(&speed_m_s)
    {
        bail!("waypoint speed must be finite and within 0.1..=0.5 m/s");
    }

    let mut plan = WaypointPlanWire {
        sequence,
        waypoint_count: MISSION_WAYPOINT_COUNT,
        nominal_speed: speed_m_s,
        min_segment_duration: MISSION_MIN_SEGMENT_DURATION_S,
        valid: 1,
        ..Default::default()
    };
    plan.waypoint[1][0] = side_m;
    plan.waypoint[2][0] = side_m;
    plan.waypoint[2][1] = side_m;
    plan.waypoint[3][1] = side_m;
    Ok(plan)
}

#[derive(Clone, Copy, Debug)]
pub struct SyntheticGnss {
    latest: topic::GnssFixData,
}

impl Default for SyntheticGnss {
    fn default() -> Self {
        Self {
            latest: unusable_gnss_fix(0),
        }
    }
}

impl SyntheticGnss {
    pub fn sample(
        &mut self,
        position_enu_m: [f64; 3],
        velocity_enu_m_s: [f64; 3],
        target_boot_time_ns: u64,
    ) -> topic::GnssFixData {
        let timestamp_ns = target_boot_time_ns / GNSS_PERIOD_NS * GNSS_PERIOD_NS;
        if timestamp_ns != 0 && timestamp_ns > self.latest.timestamp_ns() {
            self.latest = if synthetic_gnss_values_are_usable(position_enu_m, velocity_enu_m_s) {
                make_gnss_fix(position_enu_m, velocity_enu_m_s, timestamp_ns)
            } else {
                unusable_gnss_fix(timestamp_ns)
            };
        }
        self.latest
    }
}

pub struct LockstepInputs {
    pub manual_control: topic::ManualControlData,
    pub inertial_sample: topic::InertialSampleData,
    pub gnss_fix: topic::GnssFixData,
    pub waypoint_plan: WaypointPlanWire,
}

pub fn lockstep_inputs(
    gyro_flu: [f32; 3],
    accel_flu: [f32; 3],
    channels: [i32; 16],
    gnss_fix: topic::GnssFixData,
    waypoint_plan: WaypointPlanWire,
    target_boot_time_ns: u64,
) -> LockstepInputs {
    let timestamp_ns = target_boot_time_ns;
    // synapse_fbs v0.9 standardizes inertial vectors as FLU, matching the
    // plant's body frame, so the sample passes through unconverted.
    let gyro_flu = Vec3f::new(gyro_flu[0], gyro_flu[1], gyro_flu[2]);
    let accel_flu = Vec3f::new(accel_flu[0], accel_flu[1], accel_flu[2]);
    let inertial_flags =
        (topic::InertialFieldFlags::Accel | topic::InertialFieldFlags::Gyro).bits();
    let inertial_sample = topic::InertialSampleData::new(
        timestamp_ns,
        &accel_flu,
        &gyro_flu,
        0.0,
        inertial_flags,
        TimeStatus::LocalFreerun,
        0,
    );

    let axes = (topic::ManualControlAxes::Pitch
        | topic::ManualControlAxes::Roll
        | topic::ManualControlAxes::Throttle
        | topic::ManualControlAxes::Yaw)
        .bits();
    let mut flags = topic::ManualControlFlags::Valid | topic::ManualControlFlags::Active;
    if channels[4] >= 1500 {
        flags |= topic::ManualControlFlags::ArmSwitch;
    }
    let centered_milli = |channel: i32| ((channel - 1500) * 2).clamp(-1000, 1000) as i16;
    let throttle_milli = (channels[2] - 1000).clamp(0, 1000) as i16;
    let manual_control = topic::ManualControlData::new(
        timestamp_ns,
        0,
        axes,
        centered_milli(channels[1]),
        centered_milli(channels[0]),
        throttle_milli,
        -centered_milli(channels[3]),
        0,
        0,
        0,
        0,
        0,
        0,
        if channels[5] < 1333 {
            0
        } else if channels[5] < 1667 {
            1
        } else {
            2
        },
        flags.bits(),
        TimeStatus::LocalFreerun,
    );

    LockstepInputs {
        manual_control,
        inertial_sample,
        gnss_fix,
        waypoint_plan,
    }
}

fn synthetic_gnss_values_are_usable(position_enu_m: [f64; 3], velocity_enu_m_s: [f64; 3]) -> bool {
    position_enu_m
        .iter()
        .chain(velocity_enu_m_s.iter())
        .all(|value| value.is_finite())
        && position_enu_m.iter().all(|value| value.abs() <= 10_000.0)
        && velocity_enu_m_s.iter().all(|value| value.abs() <= 300.0)
        && (-1_000.0..=20_000.0).contains(&(GNSS_ORIGIN_ALTITUDE_MSL_M + position_enu_m[2]))
}

fn unusable_gnss_fix(timestamp_ns: u64) -> topic::GnssFixData {
    topic::GnssFixData::new(
        timestamp_ns,
        0,
        0,
        0,
        0,
        0,
        u16::MAX,
        u16::MAX,
        u16::MAX,
        u16::MAX,
        u16::MAX,
        u16::MAX,
        0,
        0,
        0,
        0,
        0,
        GnssFixType::NoFix,
        0,
        0,
        TimeStatus::LocalFreerun,
        0,
    )
}

fn make_gnss_fix(
    position_enu_m: [f64; 3],
    velocity_enu_m_s: [f64; 3],
    timestamp_ns: u64,
) -> topic::GnssFixData {
    let east_m = position_enu_m[0];
    let north_m = position_enu_m[1];
    let distance_m = east_m.hypot(north_m);
    let angular_distance = distance_m / EARTH_RADIUS_M;
    let bearing_rad = east_m.atan2(north_m);
    let latitude_0_rad = GNSS_ORIGIN_LATITUDE_DEG.to_radians();
    let longitude_0_rad = GNSS_ORIGIN_LONGITUDE_DEG.to_radians();
    let sin_latitude_0 = latitude_0_rad.sin();
    let cos_latitude_0 = latitude_0_rad.cos();
    let sin_angular_distance = angular_distance.sin();
    let cos_angular_distance = angular_distance.cos();
    let latitude_rad = (sin_latitude_0 * cos_angular_distance
        + cos_latitude_0 * sin_angular_distance * bearing_rad.cos())
    .asin();
    let longitude_rad = longitude_0_rad
        + (bearing_rad.sin() * sin_angular_distance * cos_latitude_0)
            .atan2(cos_angular_distance - sin_latitude_0 * latitude_rad.sin());
    let horizontal_speed_m_s = velocity_enu_m_s[0].hypot(velocity_enu_m_s[1]);
    let course_cdeg = (velocity_enu_m_s[0]
        .atan2(velocity_enu_m_s[1])
        .to_degrees()
        .rem_euclid(360.0)
        * 100.0)
        .round() as u16;
    let mut flags = topic::GnssFixFlags::VelocityUpValid;
    if horizontal_speed_m_s > 0.01 {
        flags |= topic::GnssFixFlags::CourseValid;
    }

    topic::GnssFixData::new(
        timestamp_ns,
        0,
        (latitude_rad.to_degrees() * 1.0e7).round() as i32,
        (longitude_rad.to_degrees() * 1.0e7).round() as i32,
        ((GNSS_ORIGIN_ALTITUDE_MSL_M + position_enu_m[2]) * 1000.0).round() as i32,
        ((GNSS_ORIGIN_ALTITUDE_MSL_M + position_enu_m[2]) * 1000.0).round() as i32,
        400,
        700,
        50,
        0,
        100,
        150,
        (horizontal_speed_m_s * 100.0).round() as u16,
        course_cdeg % 36_000,
        0,
        (velocity_enu_m_s[2] * 100.0).round() as i16,
        flags.bits(),
        GnssFixType::Fix3d,
        16,
        20,
        TimeStatus::LocalFreerun,
        0,
    )
}

const _: () = assert!(size_of::<WaypointPlanWire>() == 480);
const _: () = assert!(align_of::<WaypointPlanWire>() == 4);
const _: () = assert!(size_of::<MissionStatusWire>() == 48);
const _: () = assert!(align_of::<MissionStatusWire>() == 8);

pub fn motor_output(payload: &[u8]) -> Result<MotorCommand> {
    let bytes = payload
        .try_into()
        .context("PwmSignalOutputsData payload has the wrong generated size")?;
    let output = topic::PwmSignalOutputsData(bytes);
    let normalized = |pulse_us: u16| ((f32::from(pulse_us) - 1000.0) / 1000.0).clamp(0.0, 1.0);
    Ok(MotorCommand {
        values: [
            normalized(output.output0_us()),
            normalized(output.output1_us()),
            normalized(output.output2_us()),
            normalized(output.output3_us()),
        ],
    })
}

pub fn flight_state(payload: &[u8]) -> Result<FlightState> {
    let bytes = payload
        .try_into()
        .context("VehicleHealthData payload has the wrong generated size")?;
    let health = topic::VehicleHealthData(bytes);
    let flags = topic::VehicleHealthFlags::from_bits_retain(health.flags());
    let sensors = topic::SensorComponentFlags::from_bits_retain(health.sensors_health());
    Ok(FlightState {
        armed: flags.contains(topic::VehicleHealthFlags::Armed),
        rc_valid: sensors.contains(topic::SensorComponentFlags::RadioControl),
        imu_ok: sensors.contains(topic::SensorComponentFlags::Gyro)
            && sensors.contains(topic::SensorComponentFlags::Accel),
        flight_mode: health.flight_mode(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::offset_of;

    #[test]
    fn native_wire_layouts_match_the_firmware_contract() {
        assert_eq!(size_of::<WaypointPlanWire>(), 480);
        assert_eq!(align_of::<WaypointPlanWire>(), 4);
        assert_eq!(offset_of!(WaypointPlanWire, sequence), 0);
        assert_eq!(offset_of!(WaypointPlanWire, waypoint_count), 4);
        assert_eq!(offset_of!(WaypointPlanWire, origin_geodetic), 8);
        assert_eq!(offset_of!(WaypointPlanWire, waypoint), 20);
        assert_eq!(offset_of!(WaypointPlanWire, velocity_enu), 212);
        assert_eq!(offset_of!(WaypointPlanWire, yaw), 404);
        assert_eq!(offset_of!(WaypointPlanWire, nominal_speed), 468);
        assert_eq!(offset_of!(WaypointPlanWire, min_segment_duration), 472);
        assert_eq!(offset_of!(WaypointPlanWire, valid), 476);
        assert_eq!(offset_of!(WaypointPlanWire, global_frame), 477);

        assert_eq!(size_of::<MissionStatusWire>(), 48);
        assert_eq!(align_of::<MissionStatusWire>(), 8);
        assert_eq!(offset_of!(MissionStatusWire, timestamp_ns), 0);
        assert_eq!(offset_of!(MissionStatusWire, plan_sequence), 8);
        assert_eq!(offset_of!(MissionStatusWire, gnss_generation), 12);
        assert_eq!(offset_of!(MissionStatusWire, plan_generation), 16);
        assert_eq!(offset_of!(MissionStatusWire, reference_generation), 20);
        assert_eq!(offset_of!(MissionStatusWire, odometry_generation), 24);
        assert_eq!(offset_of!(MissionStatusWire, guidance_generation), 28);
        assert_eq!(offset_of!(MissionStatusWire, motor_generation), 32);
        assert_eq!(offset_of!(MissionStatusWire, health_generation), 36);
        assert_eq!(offset_of!(MissionStatusWire, mission_state), 40);
        assert_eq!(offset_of!(MissionStatusWire, flags), 41);
        assert_eq!(offset_of!(MissionStatusWire, reserved), 42);
    }

    #[test]
    fn inputs_use_gnss_and_bounded_plan_boundary() {
        let mut channels = [1500; 16];
        channels[2] = 1250;
        channels[4] = 2000;
        channels[5] = 2000;
        let mut synthetic_gnss = SyntheticGnss::default();
        let gnss_fix = synthetic_gnss.sample([1.0, 2.0, 3.0], [4.0, 0.0, 0.25], GNSS_PERIOD_NS);
        let plan = bounded_square_plan(1, 2.0, 0.3).unwrap();
        let inputs = lockstep_inputs(
            [1.0, 2.0, 3.0],
            [4.0, 5.0, -9.8],
            channels,
            gnss_fix,
            plan,
            5_000_000,
        );
        assert_eq!(inputs.inertial_sample.timestamp_ns(), 5_000_000);
        assert_eq!(inputs.inertial_sample.gyro_flu_rad_s().y(), 2.0);
        assert_eq!(inputs.manual_control.throttle_milli(), 250);
        assert_eq!(inputs.manual_control.flight_mode(), 2);
        assert_eq!(inputs.gnss_fix.timestamp_ns(), GNSS_PERIOD_NS);
        assert_eq!(inputs.gnss_fix.altitude_msl_mm(), 203_000);
        assert_eq!(inputs.waypoint_plan, plan);
        assert!(
            topic::ManualControlFlags::from_bits_retain(inputs.manual_control.flags())
                .contains(topic::ManualControlFlags::ArmSwitch)
        );
    }

    #[test]
    fn synthetic_gnss_updates_at_exactly_ten_hertz() {
        let mut gnss = SyntheticGnss::default();
        let before_first_period = gnss.sample([0.0; 3], [0.0; 3], GNSS_PERIOD_NS - 1);
        assert_eq!(before_first_period.timestamp_ns(), 0);
        assert_eq!(before_first_period.fix_type(), GnssFixType::NoFix);
        assert_eq!(before_first_period.horizontal_accuracy_mm(), u16::MAX);
        assert_eq!(before_first_period.vertical_accuracy_mm(), u16::MAX);
        assert_eq!(before_first_period.velocity_accuracy_mm_s(), u16::MAX);
        assert_eq!(before_first_period.yaw_accuracy_cdeg(), u16::MAX);
        assert_eq!(before_first_period.time_status(), TimeStatus::LocalFreerun);

        let first = gnss.sample([1.0, 2.0, 3.0], [2.0, 0.0, 0.25], GNSS_PERIOD_NS);
        let repeated = gnss.sample(
            [99.0, 99.0, 99.0],
            [99.0, 99.0, 99.0],
            GNSS_PERIOD_NS + GNSS_PERIOD_NS / 2,
        );
        let second = gnss.sample([2.0, 4.0, 6.0], [0.0; 3], 2 * GNSS_PERIOD_NS);

        assert_eq!(first, repeated);
        assert_eq!(first.timestamp_ns(), GNSS_PERIOD_NS);
        assert_eq!(second.timestamp_ns() - first.timestamp_ns(), GNSS_PERIOD_NS);
        assert_eq!(first.fix_type(), GnssFixType::Fix3d);
        assert_eq!(first.ground_speed_cm_s(), 200);
        assert_eq!(first.course_over_ground_cdeg(), 9_000);
        assert_eq!(first.velocity_up_cm_s(), 25);
        assert!(first.latitude_deg_e7() > 404_237_000);
        assert!(first.longitude_deg_e7() > -869_212_000);
        assert_eq!(first.altitude_msl_mm(), 203_000);
        assert_eq!(first.horizontal_accuracy_mm(), 400);
        assert_eq!(first.vertical_accuracy_mm(), 700);
    }

    #[test]
    fn synthetic_geodesy_round_trips_the_mission_square() {
        let origin = make_gnss_fix([0.0; 3], [0.0; 3], GNSS_PERIOD_NS);
        for expected in [
            [0.0, 0.0, 0.0],
            [1.0, 0.0, 1.5],
            [1.0, 1.0, 1.5],
            [0.0, 1.0, 1.5],
        ] {
            let fix = make_gnss_fix(expected, [0.0; 3], 2 * GNSS_PERIOD_NS);
            let latitude_0 = origin.latitude_deg_e7() as f32 * 1.0e-7_f32.to_radians();
            let delta_latitude = (i64::from(fix.latitude_deg_e7())
                - i64::from(origin.latitude_deg_e7())) as f32
                * 1.0e-7_f32.to_radians();
            let delta_longitude = (i64::from(fix.longitude_deg_e7())
                - i64::from(origin.longitude_deg_e7())) as f32
                * 1.0e-7_f32.to_radians();
            let latitude = latitude_0 + delta_latitude;
            let haversine = (delta_latitude * 0.5).sin().powi(2)
                + latitude_0.cos() * latitude.cos() * (delta_longitude * 0.5).sin().powi(2);
            let central_angle = 2.0 * haversine.clamp(0.0, 1.0).sqrt().asin();
            let bearing = (delta_longitude.sin() * latitude.cos()).atan2(
                latitude_0.sin()
                    * latitude_0.cos()
                    * delta_latitude.cos()
                    * (1.0 - delta_longitude.cos())
                    + (latitude_0.cos().powi(2) + latitude_0.sin().powi(2) * delta_longitude.cos())
                        * delta_latitude.sin(),
            );
            let distance = EARTH_RADIUS_M as f32 * central_angle;
            let projected = [
                f64::from(distance * bearing.sin()),
                f64::from(distance * bearing.cos()),
                f64::from(fix.altitude_msl_mm() - origin.altitude_msl_mm()) * 1.0e-3,
            ];
            for axis in 0..3 {
                assert!(
                    (projected[axis] - expected[axis]).abs() < 0.03,
                    "axis {axis}: projected {:?}, expected {:?}",
                    projected,
                    expected
                );
            }
        }
    }

    #[test]
    fn synthetic_gnss_marks_nonfinite_input_unusable() {
        let mut gnss = SyntheticGnss::default();
        let fix = gnss.sample([f64::NAN, 0.0, 0.0], [0.0; 3], GNSS_PERIOD_NS);
        assert_eq!(fix.timestamp_ns(), GNSS_PERIOD_NS);
        assert_eq!(fix.fix_type(), GnssFixType::NoFix);
        assert_eq!(fix.horizontal_accuracy_mm(), u16::MAX);
        assert_eq!(fix.vertical_accuracy_mm(), u16::MAX);
        assert_eq!(fix.velocity_accuracy_mm_s(), u16::MAX);
    }

    #[test]
    fn bounded_square_plan_is_fixed_capacity_and_fail_closed() {
        let plan = bounded_square_plan(7, 2.5, 0.4).unwrap();
        assert_eq!(size_of::<WaypointPlanWire>(), 480);
        assert_eq!(plan.sequence, 7);
        assert_eq!(plan.waypoint_count, 5);
        assert_eq!(plan.waypoint[0], [0.0, 0.0, 0.0]);
        assert_eq!(plan.waypoint[1], [2.5, 0.0, 0.0]);
        assert_eq!(plan.waypoint[2], [2.5, 2.5, 0.0]);
        assert_eq!(plan.waypoint[3], [0.0, 2.5, 0.0]);
        assert_eq!(plan.waypoint[4], [0.0, 0.0, 0.0]);
        assert_eq!(plan.valid, 1);
        assert_eq!(plan.global_frame, 0);
        assert!(bounded_square_plan(0, 2.0, 0.3).is_err());
        assert!(bounded_square_plan(1, 0.49, 0.3).is_err());
        assert!(bounded_square_plan(1, 2.0, 0.51).is_err());
        assert!(bounded_square_plan(1, f32::NAN, 0.3).is_err());
    }

    #[test]
    fn outputs_use_generated_v09_accessors() {
        let mut pwm = topic::PwmSignalOutputsData::default();
        pwm.set_output0_us(1250);
        pwm.set_output1_us(1500);
        let command = motor_output(&pwm.0).unwrap();
        assert_eq!(command.values[0], 0.25);
        assert_eq!(command.values[1], 0.5);

        let mut health = topic::VehicleHealthData::default();
        health.set_flags(topic::VehicleHealthFlags::Armed.bits());
        health.set_sensors_health(
            (topic::SensorComponentFlags::Gyro
                | topic::SensorComponentFlags::Accel
                | topic::SensorComponentFlags::RadioControl)
                .bits(),
        );
        let state = flight_state(&health.0).unwrap();
        assert!(state.armed && state.rc_valid && state.imu_ok);
    }
}
