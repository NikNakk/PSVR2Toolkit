# Sony behavioural-oracle capture: 2026-10-04

Analysed locally in `NikNakk/PSVR2Toolkit`, branch `experiment/native-optical-capture`.
Raw input: `%TEMP%\psvr2-toolkit-optical-7164`; Bluetooth: `../sense-research/steamvr-succes-2.pcapng.gz`.
All CSV rows, all 240 VI files, all 6,206 IF8 packets, and all 92,247 ETW enhanced packet blocks were processed. Raw inputs were not edited or copied into this directory. `source-manifest.csv` records source sizes and SHA-256 hashes. The auxiliary `optical-csvs.zip` was not needed because its CSV inputs were available directly.

## Main findings

1. **The recurring BROAD â†’ PRESCAN transition is not explained by pose loss.** Four completed BROAD episodes last 10.001, 10.012, 10.020 and 10.005 s; all four transitions occur with a valid published pose. Three subsequent PRESCAN episodes last about 3.01 s and one lasts 3.924 s. Periodic synchronization/rescan is a stronger candidate than an unconditional loss fallback.
2. **Initial pose acquisition occurs during PRESCAN.** The first semantic matches occur at +1.298 s; the first valid right pose occurs at +38.868 s; BROAD starts at +41.880 s, about 3.012 s later. Early successful matches therefore do not by themselves imply tracking acquisition.
3. **Both pose-validity losses recover within BROAD.** Losses begin at +57.022 and +85.902 s and last 0.802 and 1.696 s. Neither causes an immediate PRESCAN transition. Final pose remains valid even during many zero-match optical frames. Pose validity cannot be substituted for optical-observation validity.
4. **IF8 is a sparse, structured stream rather than a raster image.** Every 36,944-byte payload supports a 64-byte header plus four sections of `4 + 256*36` bytes. Section counts delimit populated records exactly: all unused records are zero. Across 117,423 populated records, four candidate u16 coordinate fields form ordered pairs within 0..508. This is direct wire evidence, not a private tracker layout.
5. **IF8 can be aligned to VI by an exact common counter.** All 240 saved VI rows find an IF8 packet with the same counter. Their device timestamps differ by a median +11.6125 ms (IF8 later); those timestamps must not be treated as identical exposure times.
6. **IF8 section order agrees with Sony's semantic camera order.** All 64,032 matched labels have an in-range zero-based blob index in the corresponding section of the nearest packet. Corresponding section-count/semantic-match-count correlations are 0.933, 0.946, 0.934 and 0.759. Each camera's best section/lag correlation is its own section at lag zero among the tested Â±3 packets. This strongly supports the correspondence; exact optical callback-to-USB frame identity still needs validation.
7. **VI does not yet establish LED visibility or physical LED mapping.** Raw lanes 0/1 show recognizable scene structure, while lanes 2..7 have widespread byte variation. No emitter is confidently identifiable in the reviewed sheets. Neither lane differences nor the hypothetical IF8 coordinate plots are photographs proving LEDs were visibly lit.

## Inventory and integrity

| Stream | Rows/packets | Span | Cadence / interpretation |
|---|---:|---:|---|
| Events | 118 | 102.110 s | 1.146 events/s, predominantly hooks |
| Clock pairs | 104 | 104.374 s | About 1 Hz |
| Ground truth | 23,968 | 102.366 s | 5,992 optical frames Ã— four cameras; median frame interval 16.681 ms |
| Poses, all devices | 130,166 | 102.946 s | Mixed HMD/left/right grain |
| Right poses | 44,484 | 102.708 s | 433.104 Hz overall; median 2.075 ms |
| VI files/CSV rows | 240 | 71.037 s | Event/periodic selection, **not continuous camera coverage** |
| IF8 | 6,206 | 105.575 s | Median 16.684 ms; overall 58.773 Hz |
| CRC-valid A2/31 | 7,325 | 102.771 s | 71.265 Hz overall |
| CRC-valid A1/31 | 12,474 | 102.777 s | 121.360 Hz overall; median 7.5 ms |

All VI filenames exist; header metadata and lengths agree with the CSV; there are no orphan VI files. The 240 rows contain 230 distinct sequences/payloads: ten repeat captures arise when neighbouring events save the same frame. The 240-frame cap is reached about 71 s into the sampled VI stream, leaving the final ~32.5 s of semantic observations without nearby saved imagery.

All IF8 wrappers are complete and valid for interface 8 / endpoint 0x89; no trailing/truncated record was encountered. Payload counters at wire offset 20 advance by one except for jumps of 5, 3, 89 and 36: **129 counter positions are absent**. Device-time increments are consistent with those skipped frames. Source delivery loss, application stalls and capture omissions cannot be distinguished solely from these missing records. A 1.515 s host gap also exists in semantic observations despite contiguous local semantic frame indices; the latter are callback ordinals, not hardware sequence counters. The log separately reports tracker drops (42 total frames at that log point).

The ETW container has no truncated EPBs. Scanning finds six CRC-rejected and five short A2/31 candidates, and two rejected and seven short A1/31 candidates. These are byte-signature candidates, **not established corrupt HID packets**: without provider/HCI reassembly, incidental signature matches are possible. CRC-valid counts are firm; physical-link packet completeness is not established. CRC scans also do not establish Bluetooth controller identity if multiple controllers were active; this capture's right-only interpretation comes from the experimental context and semantic/event sides.

CSV mixed-stream nonpositive intervals are reported in `stream-summary.csv`; same-tick/interleaved camera and device rows must not be counted as packet corruption. No semantic masks exceed 17 bits.

## Clock alignment and timeline

The fit uses centred integer-origin pairs:

```
unix_us = 1791120384445109 - 217.7913653904
          + 1.0000006660223153 * (qpc_us - 402861483041)
```

Residual RMS = **166.250 us**; p95 absolute = 215.964 us; maximum absolute = 837.298 us, across all 104 pairs. This is clock-pair fit error, not a bound on Bluetooth transport, USB delivery, exposure or hook latency.

`unified-timeline.csv` has one row per semantic camera result and includes all 17 blob labels, corrected side, QPC/Unix time, latest wire setting, nearest event, pose, VI and IF8. Full A2 and IF8 tables retain their other timestamps outside this row grain. Settings are held from the first observed report until the next observed setting; transition edges are interval-censored by reporting/transport latency. Events are generic hook landmarks: no private Sony LED command content is inferred from them.

Median / p95 absolute join distances:

- Right pose: 0.579 / 1.214 ms (50 ms maximum allowed; actual maximum 13.842 ms).
- IF8 host time: 1.341 / 2.417 ms (actual maximum 6.990 ms).
- Saved VI: 312.997 ms / 27.545 s. These sparse nearest images frequently **cannot support a same-frame inference**.
- Event landmark: 249.377 / 483.479 ms. A nearest generic hook does not prove causality.

IF8 offset-8 u32 advances predominantly by 16,683/16,684 units per frame and behaves as a device microsecond timestamp. The full host/device fit has 28.871 ms RMS and 1.497 s maximum residual, dominated by delivery/stall outliers. A clock-like byte heuristic also ranks unaligned counter-overlap offsets highly; only the aligned offset-8 stamp and offset-20 counter have stronger independent evidence. `vi-if8-counter-alignment.csv` uses the exact counter instead: all 240 rows match, device-stamp difference range +9.841..+11.839 ms, same-counter host-time difference range -0.685..+8.383 ms. This supports a common frame-number domain, not simultaneous exposure/read completion.

## LED-pattern comparisons

BROAD has period 42 and `led1..led3=ff`; PRESCAN has period 40. Numeric phases 0, 1, 2 and 5 occur; no numeric phases 3/4 (BG/STABLE in the existing schedule vocabulary) occur. Phase 5 is retained numerically rather than assigned an unsupported state meaning. BROAD cycle_position is zero; cycle_length is 50,050,050 in active schedules. This capture alone does not establish the units/semantics of period or cycle fields.

| BROAD led0 | Setting runs | Optical frames | Mean cross-camera matched union | Right-pose valid fraction |
|---|---:|---:|---:|---:|
| 04 | 1 | 60 | 7.42 | 100% |
| 05 | 1 | 67 | 0.43 | 100% |
| 06 | 7 | 544 | 6.76 | 91.18% |
| 0a | 4 | 239 | 8.17 | 100% |
| 0c | 8 | 529 | 7.75 | 100% |
| 0d | 1 | 59 | 4.07 | 100% |
| 0e | 11 | 1,270 | 4.30 | 92.05% |
| ff | 5 | 104 | 7.13 | 100% |

`a231-runs.csv` separates settings by phase, sequence, period, cycle position/length and all four LED bytes; duration is held to the next observed setting, rather than ending at the last repeated report. `ground-truth-summary.csv` includes assigned/matched-mask distributions and each physical LED's marginal occurrence per camera. `led-pattern-summary.csv` includes complete cross-camera union distributions and empirically observed acquisition/retention denominators. Trials are adjacent optical frames <50 ms apart, with known poses; the exposure key is the earlier frame. These are frame-to-frame empirical proportions, **not independent trials or causal success probabilities**. Most values have no untracked exposure, so acquisition probability is undefined, not zero. Adjacent-frame retention is nearly one but can coexist with long zero-match intervals.

Repeated 06/0e settings can produce **all 17 different LED IDs somewhere during the setting's accumulated observations**. Their matched masks vary extensively. A literal four-byte field-to-17-LED on/off bitmap interpretation is not supported. 04/05/0d occur only once each, so reproducibility of their apparent selectivity cannot be measured. 05 is associated with low counts, chiefly IDs 3/4/5 plus occasional 6/14; 0d frequently retains IDs 5/6. These are correlations, not established command-selected physical groups. 0e produces both strong matches and prolonged zeros across different runs. Controller/head motion, view/occlusion, assignment availability and the scheduler's response to tracking quality confound these distributions.

The strongest current candidate is **a scheduling/control field selecting temporal illumination or scan behavior rather than a direct physical LED bitmap**, within a periodically refreshed synchronization state. Which bits select timing, emitter groups, intensity or policy remains unknown. The low-count odd values are worth reproducing at fixed geometry; one occurrence does not settle their meaning.

## Raw VI and IF8 evidence limits

Every saved VI frame was validated and every lane was profiled; all 34 available -1/+1 event pairs were compared. Summary byte-change regions are retained rather than thousands of images. Representative sheets include acquisition-phase change, return to PRESCAN and low-motion valid-pose changes. Pose motion is suppressed as a stationarity metric whenever either pose is invalid; even a small valid-pose change does not establish a stationary camera/controller relative geometry. The HMD can move, transforms must be considered, and prediction can freeze orientation over short intervals.

Lanes 0/1 show coherent scene appearance and relatively sparse changes; lanes 2..7 have large changes over much of the payload. Those bytes may encode packed information; a lane is **not independently established to be an intensity plane or physical camera view**. Differences thresholded at 24 are byte-change components, not LED detections. `vi-if8-coordinate-tests.csv` additionally tests seven neutral full/half-height and quadrant projections across all 230 unique saved VI frames, comparing a local 5×5 byte peak at candidate labelled coordinates with a shifted same-frame control. Several competing projections have positive contrast; this does not uniquely identify camera packing or establish emitter brightness. The control can land on a different background, and selector/packed-byte peaks are not calibrated intensities. No physical LED ID is defensibly associated to a VI changed region here. A validated observable VI packing/decoder and a stationary experiment are needed. The raw sheets do not visually demonstrate lit LEDs.

IF8 section records begin with a u16 taking values 1, 2, 3, 5, 0x8001 and 0x8002. Its meaning is unresolved. Four u16 fields at record offsets 4/6/8/10 form ordered coordinate-like pairs for all populated records. Six following u32 fields vary; interpreting them as moments/intensity/etc. requires another test. Section counts range from 0 to at most 13 in this run and include non-controller/background observations before any right-controller matches. Thus an IF8 record is not inherently a physical Sense LED ID.

`if8-pattern-offsets.csv` contrasts between-setting byte means with within-setting variation across all packets, without excluding counter/header fields or controlling pose. `if8-section-summary.csv` is more interpretable than arbitrary byte correlations. `if8-candidate-label-boxes.csv` and `if8-candidate-labels.png` use the experimentally supported camera/zero-based-index hypothesis to propose label-associated boxes; their coordinate axes and exact frame mapping remain candidates. The sheet chooses maximum-union examples for legibility, **not representative detection success**. It is a diagram of candidate wire fields, not visible-emitter verification or a Sony tracking algorithm.

## State-machine evidence

**Observed:** initial OFF/phase-5/phase-0 sequence, PRESCAN, acquisition within PRESCAN, BROAD, four approximately ten-second BROAD returns to PRESCAN, then BROAD again; shutdown phase 0. Both brief pose losses/reacquisitions remain in BROAD. BG/STABLE are absent.

**Strong inference:** the repeated ten-second boundary is a maintenance/resynchronization timer or a bounded BROAD lifetime, rather than a rule triggered solely by pose-validity loss. Actual optical associations and published pose validity are distinct states. IF8 exposes four camera-grouped candidate detection lists that are upstream of semantic LED identity.

**Hypotheses:** failure to progress to BG/STABLE might cause periodic retry, or PRESCAN refresh may be normal in this environment. This run cannot establish that either phase is required/expected, nor distinguish timer maintenance from a persistent condition evaluated at a ten-second deadline. No controller packet-bit meaning, timing unit, or illumination duty cycle has been established causally.

## Next targeted Windows capture

Use stock Sony scheduling (`useToolkitSync=false`) and the same successful USB/camera/workaround configuration. Before recording, raise the selective VI budget to at least 1,000 frames and IF8 cap to at least 512 MiB, or use equivalent bounded selective recording covering the full experiment; the current 240/256 MiB caps are inadequate for this protocol. Keep ETW, clock pairs, semantic output and published poses enabled. This task does not change these budgets.

Record approximately **180 seconds**, with explicit QPC experiment markers:

1. Acquire a valid right pose; allow up to 60 s and mark acquisition. Mount/fix both HMD and controller relative to the room, avoiding hand/head motion.
2. Keep that geometry fixed for 35 s, spanning at least two BROAD/PRESCAN maintenance boundaries. Do not change LED commands manually; observe Sony's native values.
3. Rotate only the controller to a second fixed orientation and hold for another 35 s; mark the movement and static endpoints.
4. With both devices fixed, cover the controller optically for 3 s, remove the cover, then hold for at least 35 s. Mark cover/uncover and verify pose validity and semantic match counts separately.
5. If 05/0d do not occur, repeat the fixed-orientation trial rather than interpreting their single occurrence in this dataset. Prefer a dark/nonreflective background to separate background IF8 detections. Preserve neighboring VI/IF8 packets by common counter and select representatives at 0, +1, +2, +3 and +6 frames after actual wire-setting changes; host hook timing alone may precede the optical effect.

The decisive comparisons are: fixed geometry across 10 s boundaries; optical occlusion versus the periodic boundary; same physical IDs across repeated 05/0d settings; VI decoder output versus IF8 coordinate candidates; and whether stable prolonged high-quality tracking ever reaches BG/STABLE. Do not copy internal Sony geometry or tracker tables to resolve these questions.

## Safe independent-implementation boundary

Safe facts/specifications: observed A2/31 values and sequence timings; affine host-clock alignment; VI packet/header dimensions already independently observable; IF8 framing/count/record repetition, counters and coordinate-like fields with their evidence qualifications; semantic IDs as experimental test labels; separation between optical observation and published pose validity. An independent Monado parser/matcher may be designed from these wire facts and labelled tests.

Not established/safe to hard-code as behavior: a 17-bit LED bitmap interpretation; a deterministic physical emitter-group map; mandatory ten-second maintenance as a universal firmware rule; BG/STABLE prerequisites; physical camera identity from byte-lane number; an exact exposure-time relationship; Sony private offsets/layouts/hooks/algorithms or canonical LED geometry. No Monado files were modified.

Feature: Sense scheduling, optical labels and IF8/VI relationships.
Source of knowledge: stock Sony Windows software as a behavioural oracle, externally observable Bluetooth ETW and raw USB capture; Sony semantic 17-LED outputs used only as ground truth.
Method: complete streamed framing/CRC validation, QPC/UTC affine fit, empirical per-setting/run statistics, neutral raw-lane differences, and repeated wire-record/label correlation.
Observed protocol/behaviour: the directly observed results above, with inferences and hypotheses separately identified.
Implementation provenance: new local analysis code written independently from captured I/O and exported semantic labels; no Sony machine-code inspection, private tracking layout or algorithm used by analysis, and no proprietary binaries/calibration/model payloads redistributed.

## Reproduce and outputs

Use Python 3 with NumPy and Pillow (the bundled Codex Python has both):

```powershell
python scripts/analyze_sony_optical_capture.py "$env:TEMP/psvr2-toolkit-optical-7164" ../sense-research/steamvr-succes-2.pcapng.gz --correct-reversed-ground-truth-side --vrserver 'C:/Program Files (x86)/Steam/logs/vrserver.txt'
python scripts/analyze_psvr2_if8_sections.py "$env:TEMP/psvr2-toolkit-optical-7164"
python scripts/analyze_vi_if8_coordinates.py "$env:TEMP/psvr2-toolkit-optical-7164"
python scripts/test_optical_capture_parsers.py
```

The side correction is explicit and specific to this pre-fix capture; omit it for newly captured data. `original_side` remains available in the timeline. The C++ writer now maps index 0 to R and index 1 to L, consistent with the hook's controller indexing. Raw source CSVs remain unchanged.

Main audit files: `validation.json`, `stream-summary.csv`, `source-manifest.csv`, `clock-residuals.csv`, `phase-episodes.csv`, `tracking-episodes.csv`, `if8-structure-validation.json`, `if8-gaps.csv`, `vi-validation.csv`, `vi-if8-counter-alignment.csv`. Analysis tables and representative PNGs are in this directory. Full-byte IF8 statistics use streamed records and bounded per-offset accumulators; neither the 229 MB IF8 file nor the decompressed ETW capture is loaded in full. The timeline intentionally retains host-join residuals so it can be filtered to an appropriate tolerance.

Validation: complete end-to-end runs of both analysis scripts; framing/CRC/timestamp-resolution synthetic regression checks (four passing tests); Python compilation; output row/count/counter reconciliation; representative image inspection; `git diff --check`. The one-line C++ instrumentation label fix was checked against its indexing call site; no native driver build or new live hardware capture was performed.
