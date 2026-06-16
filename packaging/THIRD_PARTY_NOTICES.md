# Third-Party Notices

This file records notice inputs for source packaging. It is not a complete legal
review for every release asset.

| Component | Purpose | Notice status |
| --- | --- | --- |
| OpenFX SDK | OpenFX host API headers/support code | Include upstream license from `third_party/openfx/LICENSE.md` in final legal review. |
| CorridorKey | AI keying model/runtime integration source | Record exact checkout/version before release. |
| PyTorch / MLX / NumPy / OpenCV / OpenEXR | Optional sidecar runtime dependencies | Record exact versions and licenses from the configured runtime before release. |
| Python standard library | Sidecar control/runtime scripts | Covered by target Python distribution/license. |

Release asset notices should be generated from the configured runtime and model
package inputs.
