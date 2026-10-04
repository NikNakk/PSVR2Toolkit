# AGENTS.md

## Purpose

This fork may be used to investigate PlayStation VR2 hardware and the behaviour of Sony's official Windows software so that independently written open-source support can be developed, including work that may eventually be proposed upstream to Monado.

Treat this repository as a behavioural/protocol research environment, **not** as a source from which Sony implementation code may be translated into Monado.

## Clean-room and provenance rules

The important distinction is how protocol knowledge was obtained, not merely that the resulting code talks to Sony hardware.

### Permitted research methods

Agents may:

- Run legitimately obtained Sony software and drivers and observe their externally visible behaviour.
- Capture and analyse USB, HID, Bluetooth/HCI/L2CAP, ETW, IPC, timing, device-state, camera, tracking, gaze, LED, haptics and other observable I/O.
- Send ordinary device requests and record responses.
- Use the Sony driver/application as a **behavioural oracle**: provide inputs, observe outputs, measure timing/state changes, and derive protocol facts.
- Compare Sony-driver behaviour with independently written implementations.
- Infer packet layouts, constants, state machines, timing relationships, calibration semantics and other interface/protocol facts from observations.
- Consult legitimately published open-source reverse-engineering projects for protocol facts, subject to their licences and provenance.
- Implement discovered behaviour independently using the conventions and architecture of the destination open-source project.

### Do not do these things

Agents must not:

- Copy Sony source code, headers, comments, symbols, tables, structs, algorithms expressed in code, or other copyrightable implementation material.
- Transliterate or mechanically rewrite decompiled/disassembled Sony functions into C/C++/Rust or other source code.
- Treat Sony disassembly or decompiler output as implementation source material.
- Commit, redistribute or embed Sony DLLs, firmware, calibration blobs, certificates, cryptographic keys or other proprietary payloads.
- Use leaked, NDA-restricted or otherwise non-public Sony SDKs, documentation or source material.
- Bypass authentication, secure boot, firmware signing or other technological protection mechanisms as part of work intended for Monado upstream.
- Assume that code from another reverse-engineering project can be copied merely because it is public: verify the exact file/commit licence and provenance first.

If answering a specific interoperability question requires limited inspection of machine code, keep the question narrow, record only the resulting interface/protocol fact, and implement any downstream code independently. Do not reproduce the inspected implementation.

## Monado upstream boundary

Work intended to inform an eventual Monado contribution should remain reproducible from protocol facts and observable hardware/software behaviour alone.

Good candidates include, where discovered through the methods above:

- stock-device USB/HID/Bluetooth protocols;
- Sense controller reports, LEDs, tracking and adaptive-trigger behaviour;
- camera and sensor interfaces;
- eye/gaze interfaces and calibration semantics;
- timing, prediction and synchronisation behaviour;
- device capability and state-machine discovery.

Keep functionality that depends on jailbreaking, defeating authentication/security mechanisms, proprietary Sony payloads, or other circumvention **separate from the upstream-oriented path**.

## Record provenance

For each newly discovered behaviour that may inform another project, record enough provenance to explain how it was learned. Prefer a short note in the relevant research document, commit message or issue using this form:

```text
Feature:
Source of knowledge:
Method:
Observed protocol/behaviour:
Implementation provenance:
```

Example:

```text
Feature: Sense controller LED synchronisation
Source of knowledge: Sony Windows driver used as a behavioural oracle
Method: Bluetooth HCI capture plus controlled state/timing experiments
Observed protocol/behaviour: <protocol facts only>
Implementation provenance: Independently implemented; no Sony source or decompiled implementation copied
```

When evidence comes from another open-source project, also record the repository, commit/file and licence.

## Separation from other projects

Do not copy implementation code from this repository into Monado unless its exact provenance and licence make that appropriate and the contribution is acceptable to Monado upstream.

Prefer transferring **facts and specifications**:

```text
observation -> protocol description/test vector -> independent implementation
```

rather than:

```text
existing implementation -> edited/copied implementation
```

When in doubt, stop before copying implementation material and document the observed behaviour instead.

## Repository-specific caution

The repository currently contains an MIT `LICENSE`, while its README also states non-commercial-use restrictions for the project and for eye-tracking data. Do not make assumptions about the legal status of reusing substantial existing implementation code elsewhere based on the licence badge alone. Verify provenance and applicable terms for the exact material before reuse.

## Default rule

**Sony binaries may be executed, traced and used as behavioural oracles. They must never be used as source material for transliterating implementation code into Monado or another open-source project. Record protocol facts and independently implement them using the destination project's conventions. Do not use leaked/NDA material, redistribute Sony proprietary material, or circumvent firmware/authentication mechanisms in upstream-oriented work.**
