use anyhow::{Context, Result};
use synapse_fbs::{
    topic,
    types::{LocalFrame, Quaternionf, RateTriplet, Vec3f},
};

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

#[derive(Clone, Copy, Debug)]
pub struct NavigationSample {
    pub position_enu_m: [f32; 3],
    pub velocity_enu_m_s: [f32; 3],
    pub quaternion_world_body: [f32; 4],
}

impl Default for NavigationSample {
    fn default() -> Self {
        Self {
            position_enu_m: [0.0; 3],
            velocity_enu_m_s: [0.0; 3],
            quaternion_world_body: [1.0, 0.0, 0.0, 0.0],
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct TrajectoryReference {
    pub position_enu_m: [f32; 3],
    pub velocity_enu_m_s: [f32; 3],
    pub acceleration_enu_m_s2: [f32; 3],
    pub yaw_rad: f32,
}

pub struct LockstepInputs {
    pub manual_control: topic::ManualControlData,
    pub inertial_sample: topic::InertialSampleData,
    pub external_odometry: topic::ExternalOdometryData,
    pub local_position_command: topic::LocalPositionCommandData,
}

pub fn lockstep_inputs(
    gyro_flu: [f32; 3],
    accel_flu: [f32; 3],
    channels: [i32; 16],
    navigation: NavigationSample,
    reference: TrajectoryReference,
    target_boot_time_ns: u64,
) -> LockstepInputs {
    let timestamp_us = target_boot_time_ns / 1_000;
    // synapse_fbs v0.7 standardizes inertial vectors as FLU, matching the
    // plant's body frame, so the sample passes through unconverted.
    let gyro_flu = Vec3f::new(gyro_flu[0], gyro_flu[1], gyro_flu[2]);
    let accel_flu = Vec3f::new(accel_flu[0], accel_flu[1], accel_flu[2]);
    let zero = Vec3f::new(0.0, 0.0, 0.0);
    let inertial_flags =
        (topic::InertialFieldFlags::Accel | topic::InertialFieldFlags::Gyro).bits();
    let inertial_sample = topic::InertialSampleData::new(
        timestamp_us,
        &accel_flu,
        &gyro_flu,
        &zero,
        0.0,
        0.0,
        inertial_flags,
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
        timestamp_us,
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
        u8::from(channels[5] >= 1500),
        flags.bits(),
    );

    let position = Vec3f::new(
        navigation.position_enu_m[0],
        navigation.position_enu_m[1],
        navigation.position_enu_m[2],
    );
    let velocity = Vec3f::new(
        navigation.velocity_enu_m_s[0],
        navigation.velocity_enu_m_s[1],
        navigation.velocity_enu_m_s[2],
    );
    let quaternion = Quaternionf::new(
        navigation.quaternion_world_body[0],
        navigation.quaternion_world_body[1],
        navigation.quaternion_world_body[2],
        navigation.quaternion_world_body[3],
    );
    let angular_velocity = RateTriplet::new(gyro_flu.x(), gyro_flu.y(), gyro_flu.z());
    let odometry_flags = topic::ExternalOdometryFlags::PositionValid
        | topic::ExternalOdometryFlags::AttitudeValid
        | topic::ExternalOdometryFlags::LinearVelocityValid
        | topic::ExternalOdometryFlags::AngularVelocityValid;
    let external_odometry = topic::ExternalOdometryData::new(
        timestamp_us,
        &position,
        &quaternion,
        &velocity,
        &angular_velocity,
        odometry_flags,
        topic::ExternalOdometryStatus::Filtered,
        0,
        0,
    );

    let reference_position = Vec3f::new(
        reference.position_enu_m[0],
        reference.position_enu_m[1],
        reference.position_enu_m[2],
    );
    let reference_velocity = Vec3f::new(
        reference.velocity_enu_m_s[0],
        reference.velocity_enu_m_s[1],
        reference.velocity_enu_m_s[2],
    );
    let reference_acceleration = Vec3f::new(
        reference.acceleration_enu_m_s2[0],
        reference.acceleration_enu_m_s2[1],
        reference.acceleration_enu_m_s2[2],
    );
    let command_mask = topic::LocalPositionCommandMask::IgnoreYawRate;
    let local_position_command = topic::LocalPositionCommandData::new(
        timestamp_us,
        &reference_position,
        &reference_velocity,
        &reference_acceleration,
        reference.yaw_rad,
        0.0,
        command_mask.bits(),
        LocalFrame::LocalEnu,
    );

    LockstepInputs {
        manual_control,
        inertial_sample,
        external_odometry,
        local_position_command,
    }
}

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

    #[test]
    fn inputs_use_generated_v07_structs() {
        let mut channels = [1500; 16];
        channels[2] = 1250;
        channels[4] = 2000;
        channels[5] = 2000;
        let navigation = NavigationSample {
            position_enu_m: [1.0, 2.0, 3.0],
            velocity_enu_m_s: [4.0, 5.0, 6.0],
            quaternion_world_body: [1.0, 0.0, 0.0, 0.0],
        };
        let reference = TrajectoryReference {
            position_enu_m: [7.0, 8.0, 9.0],
            ..Default::default()
        };
        let inputs = lockstep_inputs(
            [1.0, 2.0, 3.0],
            [4.0, 5.0, -9.8],
            channels,
            navigation,
            reference,
            5_000_000,
        );
        assert_eq!(inputs.inertial_sample.timestamp_us(), 5_000);
        assert_eq!(inputs.inertial_sample.gyro_flu_rad_s().y(), 2.0);
        assert_eq!(inputs.manual_control.throttle_milli(), 250);
        assert_eq!(inputs.external_odometry.position_enu_m().z(), 3.0);
        assert_eq!(inputs.local_position_command.position_enu_m().x(), 7.0);
        assert!(
            topic::ManualControlFlags::from_bits_retain(inputs.manual_control.flags())
                .contains(topic::ManualControlFlags::ArmSwitch)
        );
    }

    #[test]
    fn outputs_use_generated_v07_accessors() {
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
