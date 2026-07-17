use std::collections::HashMap;
use std::ffi::{CString, c_char, c_void};
use std::fs;
use std::path::Path;
use std::ptr;

use anyhow::{Context, Result, bail};
use libloading::Library;

type Instance = *mut c_void;
type ValueReference = u32;
type Status = i32;

type Instantiate = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    bool,
    bool,
    bool,
    bool,
    *const ValueReference,
    usize,
    *mut c_void,
    *mut c_void,
    *mut c_void,
) -> Instance;
type FreeInstance = unsafe extern "C" fn(Instance);
type EnterInitialization = unsafe extern "C" fn(Instance, bool, f64, f64, bool, f64) -> Status;
type ExitInitialization = unsafe extern "C" fn(Instance) -> Status;
type SetFloat64 =
    unsafe extern "C" fn(Instance, *const ValueReference, usize, *const f64, usize) -> Status;
type GetFloat64 =
    unsafe extern "C" fn(Instance, *const ValueReference, usize, *mut f64, usize) -> Status;
type DoStep = unsafe extern "C" fn(
    Instance,
    f64,
    f64,
    bool,
    *mut bool,
    *mut bool,
    *mut bool,
    *mut f64,
) -> Status;

const FMI_WARNING: Status = 1;

struct Api {
    _library: Library,
    free_instance: FreeInstance,
    set_float64: SetFloat64,
    get_float64: GetFloat64,
    do_step: DoStep,
}

pub struct Plant {
    api: Api,
    instance: Instance,
    inputs: [ValueReference; 4],
    outputs: [ValueReference; 14],
    output_values: [f64; 14],
    time: f64,
}

impl Plant {
    pub fn open(library_path: &Path, description_path: &Path) -> Result<Self> {
        let description = fs::read_to_string(description_path).with_context(|| {
            format!(
                "cannot read FMI model description {}",
                description_path.display()
            )
        })?;
        let document = roxmltree::Document::parse(&description)
            .context("cannot parse FMI modelDescription.xml")?;
        let root = document.root_element();
        let token = CString::new(
            root.attribute("instantiationToken")
                .context("FMI model description has no instantiationToken")?,
        )?;
        let references = variable_references(&document)?;
        let inputs = names_to_references(&references, &["motor0", "motor1", "motor2", "motor3"])?;
        let outputs = names_to_references(
            &references,
            &[
                "x_m",
                "y_m",
                "z_m",
                "vz_m_s",
                "roll_rad",
                "pitch_rad",
                "yaw_rad",
                "gyro_x_rad_s",
                "gyro_y_rad_s",
                "gyro_z_rad_s",
                "accel_x_m_s2",
                "accel_y_m_s2",
                "accel_z_m_s2",
                "time_s",
            ],
        )?;

        let library = unsafe { Library::new(library_path) }
            .with_context(|| format!("cannot load FMI plant {}", library_path.display()))?;
        let instantiate = unsafe { *library.get::<Instantiate>(b"fmi3InstantiateCoSimulation\0")? };
        let free_instance = unsafe { *library.get::<FreeInstance>(b"fmi3FreeInstance\0")? };
        let enter_initialization =
            unsafe { *library.get::<EnterInitialization>(b"fmi3EnterInitializationMode\0")? };
        let exit_initialization =
            unsafe { *library.get::<ExitInitialization>(b"fmi3ExitInitializationMode\0")? };
        let set_float64 = unsafe { *library.get::<SetFloat64>(b"fmi3SetFloat64\0")? };
        let get_float64 = unsafe { *library.get::<GetFloat64>(b"fmi3GetFloat64\0")? };
        let do_step = unsafe { *library.get::<DoStep>(b"fmi3DoStep\0")? };

        let instance_name = CString::new("rdd2-plant")?;
        let instance = unsafe {
            instantiate(
                instance_name.as_ptr(),
                token.as_ptr(),
                ptr::null(),
                false,
                false,
                false,
                false,
                ptr::null(),
                0,
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
            )
        };
        if instance.is_null() {
            bail!("FMI plant refused Co-Simulation instantiation");
        }
        check_status(
            unsafe { enter_initialization(instance, false, 0.0, 0.0, false, 0.0) },
            "enter initialization",
        )?;
        check_status(
            unsafe { exit_initialization(instance) },
            "exit initialization",
        )?;

        let api = Api {
            _library: library,
            free_instance,
            set_float64,
            get_float64,
            do_step,
        };
        let mut plant = Self {
            api,
            instance,
            inputs,
            outputs,
            output_values: [0.0; 14],
            time: 0.0,
        };
        plant.read_outputs()?;
        Ok(plant)
    }

    pub fn altitude(&self) -> f64 {
        self.output_values[2]
    }

    pub fn vertical_speed(&self) -> f64 {
        self.output_values[3]
    }

    pub fn position(&self) -> [f64; 3] {
        [
            self.output_values[0],
            self.output_values[1],
            self.output_values[2],
        ]
    }

    pub fn euler(&self) -> [f64; 3] {
        [
            self.output_values[4],
            self.output_values[5],
            self.output_values[6],
        ]
    }

    pub fn imu_flu(&self) -> ([f32; 3], [f32; 3]) {
        (
            [
                self.output_values[7] as f32,
                self.output_values[8] as f32,
                self.output_values[9] as f32,
            ],
            [
                self.output_values[10] as f32,
                self.output_values[11] as f32,
                self.output_values[12] as f32,
            ],
        )
    }

    pub fn step(&mut self, motor: [f32; 4], dt: f64) -> Result<()> {
        let values = motor.map(|value| f64::from(value.clamp(0.0, 1.0)));
        check_status(
            unsafe {
                (self.api.set_float64)(
                    self.instance,
                    self.inputs.as_ptr(),
                    self.inputs.len(),
                    values.as_ptr(),
                    values.len(),
                )
            },
            "set motor inputs",
        )?;
        let mut event_handling_needed = false;
        let mut terminate_simulation = false;
        let mut early_return = false;
        let mut last_successful_time = self.time;
        check_status(
            unsafe {
                (self.api.do_step)(
                    self.instance,
                    self.time,
                    dt,
                    true,
                    &mut event_handling_needed,
                    &mut terminate_simulation,
                    &mut early_return,
                    &mut last_successful_time,
                )
            },
            "advance plant",
        )?;
        if event_handling_needed || early_return || terminate_simulation {
            bail!(
                "FMI plant requested unsupported external event handling: event={event_handling_needed} early={early_return} terminate={terminate_simulation}"
            );
        }
        self.time = last_successful_time;
        self.read_outputs()
    }

    fn read_outputs(&mut self) -> Result<()> {
        check_status(
            unsafe {
                (self.api.get_float64)(
                    self.instance,
                    self.outputs.as_ptr(),
                    self.outputs.len(),
                    self.output_values.as_mut_ptr(),
                    self.output_values.len(),
                )
            },
            "read plant outputs",
        )
    }
}

impl Drop for Plant {
    fn drop(&mut self) {
        unsafe { (self.api.free_instance)(self.instance) };
    }
}

fn variable_references(document: &roxmltree::Document<'_>) -> Result<HashMap<String, u32>> {
    let mut references = HashMap::new();
    for node in document
        .descendants()
        .filter(|node| node.has_tag_name("Float64"))
    {
        let Some(name) = node.attribute("name") else {
            continue;
        };
        let Some(reference) = node.attribute("valueReference") else {
            continue;
        };
        references.insert(name.to_owned(), reference.parse()?);
    }
    Ok(references)
}

fn names_to_references<const N: usize>(
    references: &HashMap<String, u32>,
    names: &[&str; N],
) -> Result<[u32; N]> {
    let mut result = [0; N];
    for (index, name) in names.iter().enumerate() {
        result[index] = *references
            .get(*name)
            .with_context(|| format!("FMI model has no Float64 variable {name}"))?;
    }
    Ok(result)
}

fn check_status(status: Status, operation: &str) -> Result<()> {
    if status > FMI_WARNING {
        bail!("FMI {operation} failed with status {status}");
    }
    Ok(())
}

pub fn radians_to_degrees(value: f64) -> f64 {
    value.to_degrees()
}

#[cfg(test)]
mod tests {
    use std::env;

    use super::*;

    #[test]
    fn configured_fmi_plant_steps_through_landing_contact() -> Result<()> {
        let Some(library) = env::var_os("RDD2_RUMOCA_PLANT_LIBRARY") else {
            return Ok(());
        };
        let description = env::var_os("RDD2_RUMOCA_PLANT_DESCRIPTION")
            .context("RDD2_RUMOCA_PLANT_DESCRIPTION must accompany the plant library")?;
        let mut plant = Plant::open(Path::new(&library), Path::new(&description))?;
        let initial_altitude = plant.altitude();
        for _ in 0..600 {
            plant.step([0.0; 4], 0.005)?;
        }
        assert!(plant.altitude().is_finite());
        assert!(plant.altitude() < initial_altitude);
        assert!(plant.altitude() >= 0.0);
        assert!(plant.vertical_speed().abs() < 0.1);
        Ok(())
    }
}
