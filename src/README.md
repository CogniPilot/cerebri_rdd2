# Source layout

The application source has four ownership areas:

- `main.c`: composition root only; it starts the four eFMU processes.
- `processes/`: one generated eFMU and one Zephyr execution context per file.
- `interfaces/`: fixed-layout data plus the only driver and ZROS interfaces
  used by those processes.
- `diagnostics/` and `platform/`: optional shell and FastDyn board support,
  kept out of the application/process layer.

Generated eFMU C and headers are build outputs under
`${CMAKE_BINARY_DIR}/generated/rumoca`; there is no generated source directory
in this tree.
