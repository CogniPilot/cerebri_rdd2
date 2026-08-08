use std::collections::BTreeMap;
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
    motor_command: ValueReference,
    outputs: [ValueReference; 6],
    output_values: [f64; 16],
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
        if root.attribute("fmiVersion") != Some("3.0") {
            bail!("plant is not an FMI 3.0 model description");
        }
        let token = CString::new(
            root.attribute("instantiationToken")
                .context("FMI model description has no instantiationToken")?,
        )?;
        let model_identifier = document
            .descendants()
            .find(|node| node.has_tag_name("CoSimulation"))
            .and_then(|node| node.attribute("modelIdentifier"))
            .context("FMI model description has no CoSimulation modelIdentifier")?;
        if model_identifier.is_empty() {
            bail!("FMI CoSimulation modelIdentifier must not be empty");
        }
        let variables = float64_variables(&document)?;
        let motor_command =
            required_variable(&variables, "commands.motor", "input", 4)?.reference;
        let outputs = required_references(
            &variables,
            &[
                ("truth.positionWorldEnu_m", 3),
                ("truth.velocityWorldEnu_m_s", 3),
                ("truth.eulerRpy_rad", 3),
                ("imu.angularVelocityBodyFlu_rad_s", 3),
                ("imu.specificForceBodyFlu_m_s2", 3),
                ("truth.timestamp_s", 1),
            ],
        )?;

        let library = unsafe { Library::new(library_path) }
            .with_context(|| format!("cannot load FMI plant {}", library_path.display()))?;
        let instantiate =
            unsafe { load_symbol::<Instantiate>(&library, "fmi3InstantiateCoSimulation")? };
        let free_instance = unsafe { load_symbol::<FreeInstance>(&library, "fmi3FreeInstance")? };
        let enter_initialization =
            unsafe { load_symbol::<EnterInitialization>(&library, "fmi3EnterInitializationMode")? };
        let exit_initialization =
            unsafe { load_symbol::<ExitInitialization>(&library, "fmi3ExitInitializationMode")? };
        let set_float64 = unsafe { load_symbol::<SetFloat64>(&library, "fmi3SetFloat64")? };
        let get_float64 = unsafe { load_symbol::<GetFloat64>(&library, "fmi3GetFloat64")? };
        let do_step = unsafe { load_symbol::<DoStep>(&library, "fmi3DoStep")? };

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
            motor_command,
            outputs,
            output_values: [0.0; 16],
            time: 0.0,
        };
        plant.read_outputs()?;
        Ok(plant)
    }

    pub fn altitude(&self) -> f64 {
        self.output_values[2]
    }

    pub fn vertical_speed(&self) -> f64 {
        self.output_values[5]
    }

    pub fn position(&self) -> [f64; 3] {
        [
            self.output_values[0],
            self.output_values[1],
            self.output_values[2],
        ]
    }

    pub fn velocity(&self) -> [f64; 3] {
        [
            self.output_values[3],
            self.output_values[4],
            self.output_values[5],
        ]
    }

    pub fn euler(&self) -> [f64; 3] {
        [
            self.output_values[6],
            self.output_values[7],
            self.output_values[8],
        ]
    }

    pub fn imu_flu(&self) -> ([f32; 3], [f32; 3]) {
        (
            [
                self.output_values[9] as f32,
                self.output_values[10] as f32,
                self.output_values[11] as f32,
            ],
            [
                self.output_values[12] as f32,
                self.output_values[13] as f32,
                self.output_values[14] as f32,
            ],
        )
    }

    pub fn step(&mut self, motor: [f32; 4], dt: f64) -> Result<()> {
        let values = motor.map(|value| f64::from(value.clamp(0.0, 1.0)));
        check_status(
            unsafe {
                (self.api.set_float64)(
                    self.instance,
                    &self.motor_command,
                    1,
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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Float64Variable<'a> {
    reference: ValueReference,
    causality: &'a str,
    scalar_count: usize,
}

fn float64_variables<'a>(
    document: &'a roxmltree::Document<'a>,
) -> Result<BTreeMap<&'a str, Float64Variable<'a>>> {
    let mut variables = BTreeMap::new();
    for node in document
        .descendants()
        .filter(|node| node.has_tag_name("Float64"))
    {
        let Some(name) = node.attribute("name") else {
            continue;
        };
        let reference = node
            .attribute("valueReference")
            .with_context(|| format!("FMI Float64 variable {name} has no valueReference"))?
            .parse()?;
        let causality = node.attribute("causality").unwrap_or("local");
        let scalar_count = node
            .children()
            .filter(|child| child.has_tag_name("Dimension"))
            .try_fold(1_usize, |count, dimension| {
                let extent: usize = dimension
                    .attribute("start")
                    .with_context(|| {
                        format!("FMI Float64 variable {name} has a non-static dimension")
                    })?
                    .parse()?;
                if extent == 0 {
                    bail!("FMI Float64 variable {name} has a zero dimension");
                }
                count
                    .checked_mul(extent)
                    .with_context(|| format!("FMI Float64 variable {name} is too large"))
            })?;
        if variables
            .insert(
                name,
                Float64Variable {
                    reference,
                    causality,
                    scalar_count,
                },
            )
            .is_some()
        {
            bail!("FMI model contains duplicate Float64 variable {name}");
        }
    }
    Ok(variables)
}

fn required_variable<'a>(
    variables: &'a BTreeMap<&str, Float64Variable<'a>>,
    name: &str,
    causality: &str,
    scalar_count: usize,
) -> Result<&'a Float64Variable<'a>> {
    let variable = variables
        .get(name)
        .with_context(|| format!("FMI model has no Float64 variable {name}"))?;
    if variable.causality != causality || variable.scalar_count != scalar_count {
        bail!(
            "FMI Float64 variable {name} must be a {causality} with {scalar_count} scalar values; got {} with {}",
            variable.causality,
            variable.scalar_count
        );
    }
    Ok(variable)
}

fn required_references<const N: usize>(
    variables: &BTreeMap<&str, Float64Variable<'_>>,
    names_and_counts: &[(&str, usize); N],
) -> Result<[ValueReference; N]> {
    let mut result = [0; N];
    for (index, (name, scalar_count)) in names_and_counts.iter().enumerate() {
        result[index] = required_variable(variables, name, "output", *scalar_count)?.reference;
    }
    Ok(result)
}

unsafe fn load_symbol<T: Copy>(library: &Library, function: &str) -> Result<T> {
    let symbol = CString::new(function)?;
    // SAFETY: The caller supplies the standard FMI signature for `function`.
    // Rumoca's build description defines FMI3_OVERRIDE_FUNCTION_PREFIX, so the
    // packaged shared-library ABI uses the unprefixed FMI 3 function names.
    Ok(*unsafe { library.get::<T>(symbol.as_bytes_with_nul()) }?)
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

    const TENSOR_MODEL_DESCRIPTION: &str = r#"
<fmiModelDescription fmiVersion="3.0" instantiationToken="token">
  <CoSimulation modelIdentifier="Rumoca_test"/>
  <ModelVariables>
    <Float64 name="commands.motor" valueReference="1" causality="input"><Dimension start="4"/></Float64>
    <Float64 name="truth.positionWorldEnu_m" valueReference="2" causality="output"><Dimension start="3"/></Float64>
    <Float64 name="truth.timestamp_s" valueReference="3" causality="output"/>
  </ModelVariables>
</fmiModelDescription>
"#;

    #[test]
    fn tensor_boundary_preserves_one_value_reference_per_array() -> Result<()> {
        let document = roxmltree::Document::parse(TENSOR_MODEL_DESCRIPTION)?;
        let variables = float64_variables(&document)?;
        assert_eq!(
            *required_variable(&variables, "commands.motor", "input", 4)?,
            Float64Variable {
                reference: 1,
                causality: "input",
                scalar_count: 4,
            }
        );
        assert_eq!(
            required_variable(&variables, "truth.positionWorldEnu_m", "output", 3)?.reference,
            2
        );
        assert_eq!(
            required_variable(&variables, "truth.timestamp_s", "output", 1)?.reference,
            3
        );
        Ok(())
    }

    #[test]
    fn tensor_boundary_rejects_scalarized_or_wrong_size_variables() -> Result<()> {
        let document = roxmltree::Document::parse(TENSOR_MODEL_DESCRIPTION)?;
        let variables = float64_variables(&document)?;
        assert!(required_variable(&variables, "motor0", "input", 1).is_err());
        assert!(required_variable(&variables, "commands.motor", "input", 3).is_err());
        assert!(required_variable(&variables, "commands.motor", "output", 4).is_err());
        Ok(())
    }

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
