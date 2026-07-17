Generated control artifacts no longer live in this directory.

Rules:
- Do not commit generated C/H artifacts here.
- Keep controller equations in `Vehicles/Rdd2/Controller.mo` in the shared
  `modelica_models` West project.
- Let CMake run pinned Rumoca and write generated eFMI output under
  `${CMAKE_BINARY_DIR}/generated/rumoca`.
- If generated behavior needs to change, update the Modelica source instead of
  patching generated output.

Current status:
- `Vehicles_Rdd2_Controller` is generated from
  `Vehicles.Rdd2.Controller` with Rumoca's `galec-production` target.
- The active flight stack calls generated eFMI controller code through thin
  handwritten wrappers; controller equation changes belong in Modelica.
