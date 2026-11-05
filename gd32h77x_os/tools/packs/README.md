# Approved CMSIS-Packs

`GigaDevice.GD32H77x_78x_DFP.0.6.1.pack` is a pinned platform dependency. It is
kept in the repository so a clean checkout can build and flash without network
access or machine-specific environment setup.

- Name: `GigaDevice.GD32H77x_78x_DFP`
- Version: `0.6.1`
- SHA256: `94ed659f9adb58c881114858e9a13dba4a0540765c4d9e4d305a24e13ca5dee6`
- Required pyOCD: `0.46.0`

`platform_prepare.py` verifies the digest before builds. The flash bridge uses
this repository-local file first. `GD32H77X_DFP_PACK` remains available only as
a developer override for testing a different approved pack.
