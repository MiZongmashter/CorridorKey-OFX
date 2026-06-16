# OpenFX SDK Ownership

`third_party/openfx` is reserved for the official OpenFX SDK checkout from:

```text
https://github.com/AcademySoftwareFoundation/openfx.git
```

Session 1 owns creating that directory, fetching the SDK, and recording the exact commit. Session 0 intentionally does not create `third_party/openfx`.

## Override

Configure may use `-DOPENFX_SDK_ROOT=/path/to/openfx` when the override points to the same official SDK layout. The override must not point to local shim headers or fake OpenFX API stubs.

## Expected Layout

The build and doctor checks expect the following official headers to exist:

```text
include/ofxCore.h
include/ofxImageEffect.h
include/ofxParam.h
include/ofxProperty.h
Support/include/ofxsImageEffect.h
```

Future sessions may add more required headers as plugin source files are introduced, but they must continue to use the official SDK/support headers.

## SDK Provenance

<!-- BEGIN OPENFX SDK PROVENANCE -->
Status: available
Acquisition: existing git checkout
Recorded: 2026-05-19T17:41:33.406469+00:00
SDK root: third_party/openfx
Expected git commit: cf6cbf978e02475a52ff9a85973c8d8146f5bd23
Header tree checksum: sha256:b4f9d8d2e961ada0881312c57efd3a77596d62cf26760593ffa910f74a0591cb
Git remote: https://github.com/AcademySoftwareFoundation/openfx.git
Git commit: cf6cbf978e02475a52ff9a85973c8d8146f5bd23
<!-- END OPENFX SDK PROVENANCE -->
