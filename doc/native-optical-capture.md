# Passive Sony optical/camera capture

Branch: `experiment/native-optical-capture`

This branch is for a future Windows behavioural-oracle session. It is intentionally constrained by `AGENTS.md`:
capture externally observable USB camera data, Bluetooth/HID output, timing and tracking-state changes; derive protocol
facts from those observations; do not transfer Sony internal structs/algorithms into Monado.

The branch therefore does **not** use Sony's internal `OpticalProcessor` working memory as evidence for the
upstream-oriented implementation.

## Provenance boundary

Feature: Sense optical LED identification / scheduling
Source of knowledge: Sony Windows driver used as a behavioural oracle
Method: passive LED/state tracing for event timestamps, raw USB camera/LED-detector capture, simultaneous Bluetooth ETW
capture of the actual A2/31 controller output, plus Sony's permitted 17-LED tracking outputs recorded as semantic
ground-truth labels
Observed protocol/behaviour: phase/period/mask/timing changes and their directly visible effect in camera images
Implementation provenance: any Monado implementation is derived independently from documented protocol facts and test
vectors; no Sony source/decompiled implementation is copied

The passive in-process hooks are used only to timestamp narrow behavioural events so nearby camera frames can be saved.
Any fact intended to inform Monado should, where possible, be cross-checked against the Bluetooth or USB trace itself.

## Capture directory

When `captureSonyOpticalTrace` is enabled (default true on this experimental branch), Toolkit creates:

```
%TEMP%\psvr2-toolkit-optical-<vrserver-pid>\
```

The same path is printed to `vrserver.txt` with the `[Sony Optical Capture]` prefix.

## Files

### events.csv

Records:

- host timestamp;
- event id;
- controller side;
- a generic `led_hook` landmark when the narrow research hook observes a Sony LED-schedule call;
- tracking landmarks derived from changes in pose validity/tracking result that Sony publishes through OpenVR.

The capture deliberately does **not** export Sony's internal LED-sync structure fields or internal command payload
bytes. The hook is only a local trigger for retaining nearby USB frames. Phase/sequence/period/`cycle_position`/mask
facts intended for Monado come from the simultaneous Bluetooth A2/31 wire capture.

The narrow LED hook is retained only as a timing/capture trigger. Camera frames are retained around calls that the
existing research instrumentation classifies as illumination-changing, plus OpenVR-published tracking-state transitions.
The trigger classification is not an upstream protocol fact; the simultaneous A2/31 wire capture determines what
actually changed. Periodic command type 6 and timing-only
adjustments remain in the CSV but do not consume the image budget.

### clock_sync.csv

Records paired QPC microseconds and Unix-wall-clock microseconds about once per second. Wireshark ETW pcapng timestamps
are wall-clock based, while Toolkit's event/camera/pose timestamps use QPC. Fitting these pairs gives a direct affine
mapping between the two clocks, so A2/31 Bluetooth changes can be aligned to camera/IF8/pose data without relying on
vrserver log formatting.

### sony_led_ground_truth.csv

Research-only semantic labels derived from Sony's internal optical-processing **outputs**, which the revised
`AGENTS.md` explicitly permits as behavioural ground truth.

One row is written per processed controller/camera frame with:

- controller side;
- optical frame index;
- camera index;
- 17-bit `assigned_mask`;
- 17-bit `matched_mask`;
- Sony's blob index for each physical LED ID 0..16.

The private Sony working-memory layout and DLL offsets remain confined to Toolkit instrumentation. No raw optical
working-set dump is persisted, and Monado should consume only documented output facts / labelled observations.

This dataset is particularly useful for validating an independently written matcher: for a given raw camera frame,
we can ask whether our detector/matcher assigns the same physical LED IDs Sony reported, without borrowing Sony's
detection or pose-solving algorithm.

### poses.csv

Records the HMD and Sense poses that Sony publishes through the standard OpenVR
`IVRServerDriverHost::TrackedDevicePoseUpdated` interface, before Toolkit applies any controller compatibility
transform. It includes position/orientation, velocity, tracking result, pose time offset, and the published
world/driver/head transforms.

This is useful for a clean-room replacement of the current experimental LED geometry: stereo camera observations can
be paired with the externally published HMD/controller relative pose and the independently obtained camera calibration
to estimate emitter positions.

### camera_frames.csv and camera-*.vi

Toolkit keeps a rolling two-frame history of the observable VI camera stream. The field previously labelled
`image_type=11` in Toolkit is at the same wire offset as Monado's independently parsed `camera_set`; the capture
therefore records it as `camera_set`, together with VI sequence ID and image/active dimensions from the raw header.

Toolkit keeps a rolling two-frame history of this stream. For each interesting event it
saves:

- two frames immediately before the event (`relative_frame=-2,-1`);
- three frames immediately after it (`relative_frame=+1,+2,+3`).

In addition, one raw VI frame is sampled approximately once per second with `event_id=0` / `relative_frame=0` so
a normal run contains controller viewpoints that are not tied to an LED transition. This is intended for the independent
multi-view LED-geometry reconstruction.

Capture is capped at 240 frames. The completed raw IF6/0x87 USB read is the sole source of saved VI packets. Selected packets are copied into a bounded memory queue; the actual file I/O runs on a background writer thread so disk latency does not block the native LED state machine.

Each `.vi` file contains the complete raw IF6/0x87 USB VI record. We deliberately no longer label the payload as BC4:
Toolkit's old camera-conversion path made that assumption, while Monado's independent observable USB analysis exposes a
different 8-byte-per-sample packing for the same 1,040,640-byte packet size.

For the mode-0x10-sized packet, the bytes after the 256-byte header fit exactly as:

```
508 rows Ã— (254 samples Ã— 8 byte lanes + 16 bytes row padding)
```

The physical/semantic meaning of those lanes is intentionally left open until a new capture verifies the VI header and
before/after image behaviour.

## Inspect/extract observable VI lanes

```powershell
py scripts\extract_psvr2_vi_lanes.py "$env:TEMP\psvr2-toolkit-optical-<pid>"
```

This prints each frame's raw VI header metadata and writes lanes 0..7 as semantics-free 254Ã—508 PGM images. It does
not claim that any particular lane is a named camera or colour channel.

## Difference LED events automatically

```powershell
py scripts\analyze_psvr2_led_frame_changes.py "$env:TEMP\psvr2-toolkit-optical-<pid>"
```

For every event with both a `-1` and `+1` frame, this compares each of the eight observable byte lanes and writes:

- pre-event lane PGM;
- post-event lane PGM;
- absolute-difference lane PGM;
- `led-frame-diffs\regions.csv` containing lane number, changed-region bounding boxes, centroids, area and peak/mean
  byte change.

Useful options:

```powershell
py scripts\analyze_psvr2_led_frame_changes.py "$env:TEMP\psvr2-toolkit-optical-<pid>" --threshold 24 --min-area 2
```

With the controller held still, a `SET_LEDS_IMMEDIATE` event should therefore reveal the physical emitters affected
by values such as `0x01`, `0x03`, `0x06`, `0x07`, `0x0a`, `0x0c`, etc, without depending on Sony internal
tracker data.

Do not assume those values are a literal 17-bit physical-LED bitmap. The successful trace changed only the first byte
of the four-byte field even though the Sense constellation model has 17 LEDs.

### usb-if8-led-detector.bin

The successful Windows run also showed interface 8 / endpoint `0x89` delivering 36,944 bytes at 60 Hz. Monado's
independently written PSVR2 driver identifies the corresponding endpoint as the **LED Detector** stream. Because this is
externally observable USB data, it is within the clean-room boundary.

The capture branch records the raw IF8/0x89 packets into `usb-if8-led-detector.bin`, each preceded by a small local
record header containing host QPC timestamp, interface, pipe and payload length. Capture is capped at 256 MiB and uses
the same background writer queue. No semantic structure is assumed yet; we can correlate packet changes against
Bluetooth A2/31 schedule changes and camera frames offline.

## Analyse Bluetooth ETW capture

```powershell
py scripts\analyze_sony_sense_etw_pcap.py steamvr-success.pcapng.gz
```

Optional CSV:

```powershell
py scripts\analyze_sony_sense_etw_pcap.py steamvr-success.pcapng.gz --csv a231.csv --runs-csv schedule-runs.csv
```

The parser accepts only CRC-valid native A2/31 reports. This is the preferred source for facts about what actually went
over the controller link.

## Recommended next oracle run

Use the same stock-Sony configuration that produced successful right-controller 6DoF:

- `useToolkitSync=false`;
- passive Sony LED trace;
- corrected camera report `0x0b` with `wValue=0x0b`;
- camera/HMD/proximity/display workarounds from the successful experiment;
- simultaneous Bluetooth ETW capture.

A 60-90 second right-controller run should be enough.

For the clearest LED-mapping evidence:

1. acquire full 6DoF;
2. hold the controller as still as practical for ~15-20 s while Sony changes native LED patterns;
3. slowly rotate to a second static orientation so LEDs hidden in the first view become visible;
4. deliberately occlude once until tracking is lost, then expose it again;
5. if possible remain tracked long enough for Sony to enter BG.

The static periods are important: image differencing then isolates emitter changes rather than controller motion.

## Non-interference

IF6 camera and IF8 LED-detector data are copied directly from completed observable USB reads, before any semantic interpretation by the capture code. Disk writes run on a background writer thread. The new code does not replace Sony's controller output, pose
solver or camera processing; it observes USB payloads and poses Sony publishes through OpenVR, and uses event timestamps
only to choose which nearby camera frames to retain.

## Complete successful oracle capture analysed (2026-10-04)

The complete capture `%TEMP%\psvr2-toolkit-optical-7164` and simultaneous
`../sense-research/steamvr-succes-2.pcapng.gz` were analysed locally. See
`../analysis/sony-oracle-20261004/capture-summary.md` for validation, all-stream
statistics, qualified interpretations, reproducible commands and the next experiment.
No raw captures or Sony proprietary payloads are included in the analysis directory.

### Instrumentation side correction

This capture predates a corrected semantic CSV label: the research hook indexes
right as 0 and left as 1. The writer had reversed those labels. The writer now emits
R for index 0 and L for index 1. The original capture remains unchanged; analysis
uses an explicit `--correct-reversed-ground-truth-side` switch and retains the
original label. Do not apply this switch to post-fix captures.

### Directly observed scheduling and tracking facts

- The full ETW scan accepts 7,325 CRC-valid A2/31 and 12,474 CRC-valid A1/31 reports;
  A2/31 spans 102.771457 seconds.
- BROAD/phase 2 uses period 42 and led0 values 04, 05, 06, 0a, 0c, 0d, 0e **and ff**;
  led1..led3 remain ff. PRESCAN/phase 1 uses period 40. Numeric phases 0 and 5 also
  appear; phases 3/4 do not occur in this capture.
- Initial right-pose validity is acquired during PRESCAN, approximately 3.012 s
  before the first BROAD command.
- Four completed BROAD episodes last 10.001â€“10.020 s before returning to PRESCAN.
  The right pose remains valid at all four transitions. Two brief pose-validity
  losses (0.802 and 1.696 s) recover within BROAD, without an intervening PRESCAN.
  These observations do not establish the cause of the periodic transitions.
- Matched physical LED IDs/masks vary within a setting. Across repeated settings,
  both 06 and 0e are associated with all 17 physical IDs somewhere in the run.
  Published pose validity can remain true when no LED is semantically matched.
  A literal 17-bit bitmap interpretation is not established by these observations.

### Directly observed IF8 wire structure and VI alignment

All 6,206 captured IF8/0x89 payloads are 36,944 bytes. Repeated wire structure is:

```
64-byte header
4 sections, each:
    little-endian u32 populated-record count
    256 record slots Ã— 36 bytes
```

The four sections start at payload offsets `64 + section*9220`. All slots after
that section's populated count are zero in every packet. All 117,423 populated
records have u16 fields at record offsets 4/6 and 8/10 forming ordered pairs in
0..508. These are observable coordinate-like fields; their exact geometric and
other record-field semantics remain under investigation. Counts range from 0 to
13 across sections in this capture.

The header starts with `LD`; its little-endian u32 at offset 4 equals payload
length. A u32 at offset 8 advances predominantly by 16,683/16,684 per packet.
The u32 at offset 20 usually advances by one; four jumps leave 129 missing counter
positions. All 240 saved VI rows have an IF8 record with an equal counter.
Same-counter IF8 and VI device timestamps are not equal: IF8 minus VI is
+9.841..+11.839 ms (median +11.6125 ms). This documents counter/timestamp
relationships, not identical exposure times.

When semantic camera rows are associated to the nearest IF8 host timestamp, all
64,032 matched Sony blob indices are valid zero-based indices in the same-numbered
IF8 section. Same-numbered section counts best correlate with each camera's
semantic match count among sections and packet lags -3..+3 tested. This is evidence
for camera/record correspondence; exact callback-to-USB frame identity and physical
camera ordering still require validation. IF8 records also exist without controller
matches, so detector records must not be assumed to inherently encode physical LED IDs.

### Timing and imagery limitations

The 104 QPC/Unix clock pairs fit with 0.166 ms RMS residual and 0.837 ms maximum
absolute residual. This is host-clock alignment accuracy, not USB/Bluetooth
transport or exposure latency. Use the affine clock fit, and retain join distances.

All 240 VI files validate, but they contain only 230 distinct frames and hit the
capture cap after 71.037 s. They cannot supply same-frame imagery for the entire
102 s semantic trace. All available pre/post pairs were compared as raw byte lanes.
No lit LED or physical LED-to-VI-region assignment is confidently established from
those sheets. A lane is not yet a validated intensity plane or named camera view.
IF8 candidate-coordinate diagrams are not evidence that emitters are visually
identifiable in the saved VI images.

Feature: Sense A2/31 scheduling and externally observable IF8/VI relationships
Source of knowledge: Sony Windows driver used as a behavioural oracle; Bluetooth
ETW and raw USB data, plus permitted semantic 17-LED output labels
Method: complete streamed packet analysis, affine clock fit, per-setting statistics,
wire counter comparison and semantic label/index correlation
Observed protocol/behaviour: the directly observed facts above
Implementation provenance: independently written local analysis of captured I/O;
no private tracker layout/algorithm or Sony machine-code inspection used, and no
Monado implementation or proprietary model/geometry data transferred

## Second run: motion and cover reconstruction (PID 30976)

The later capture and `../sense-research/steamvr-success-3.pcapng.gz` were analysed
with explicit controller-side and Bluetooth ACL-handle selection. Full results:
`../analysis/sony-oracle-20261004-30976/capture-summary.md`.

Direct observations:

- The trace has two separate validated ACL streams (handles 12 and 13, CID 0x42).
  Six brief byte-9/bit-1 input presses occur on handle 12, consistent with the user's
  right-controller X markers. Handle identity is connection-specific; it must not
  be hard-coded as right/left in another implementation.
- A candidate signed three-axis rotational field at A1/31 report offset 17 has a
  magnitude correlation of 0.994907 with published right-controller angular speed
  across 28,805 time-near samples. Scale and axis meanings remain unassigned.
  No private Sony sensor/tracker layout was consulted for this statistical test.
- The right semantic union is zero continuously at +148.345â€“153.469 s and
  +195.724â€“199.496 s, consistent with the two reported covers. Published right-pose
  validity remains true through both intervals and the remainder of the run.
- Quiet pose/input intervals retain approximately ten-second BROAD/PRESCAN cycles.
  Most BROAD intervals are 9.997â€“10.027 s; one is shorter at 9.097 s. The first
  cover coincides with a 10.604 s PRESCAN interval. During the second cover, BROAD
  starts before semantic matches resume. No universal optical-match prerequisite
  for entering BROAD is established. Neither controller reaches BG/STABLE.
- Right BROAD includes led0=0x11. Value 05 has a mean matched cross-camera union of
  8.695 here, compared with 0.433 previously. The earlier sparse observation must
  not be turned into an established command-to-emitter-subset rule.
- The final right zero-match interval lasts +212.858â€“248.376 s. Controller input
  reports continue with distinct data and a low rotational signal. HMD pose updates
  stop at about +220.002 s. The user reports this tail may be equipment removal followed by slow SteamVR
  shutdown. Treat it as probable teardown, not the intended final hold. A
  valid/frozen controller pose is not evidence of sustained optical tracking.
- All 7,266 IF8 packets repeat the previously observed structure: 333,259 populated
  records pass ordered candidate coordinate bounds, all unused tails are zero,
  and all 240 VI rows have common-counter IF8 matches. IF8 includes both controllers
  and background detections; aggregate counts cannot be attributed exclusively to
  one controller's scheduling values.
- The first new IF8 packet is one device frame after the preceding capture's final
  packet, although host timestamps differ by 15.594 s. The next packet jumps 933
  device frames in only 697 host microseconds. This strongly supports startup
  delivery of an old/delayed packet; the 932 absent startup counter positions must
  be distinguished from the three absent positions at a later in-run jump.

The unchanged native budgets still limit VI imagery to +55.000 s and IF8 to
+121.001 s. Neither cover has saved contemporaneous VI/IF8 evidence, and no marked
quiet hold has saved VI imagery. The new semantic/pose/Bluetooth reconstruction
therefore does not establish visible LEDs or VI camera/lane mapping.

Feature: multi-controller wire separation and experiment reconstruction
Source of knowledge: Bluetooth/USB wire observations, public OpenVR poses,
permitted semantic LED outputs and approximate user experiment markers
Method: validated ACL/HID framing and CRC, explicit side selection, affine clocks,
button-edge and pose/sensor differential analysis
Observed protocol/behaviour: the observations above; causes/axis mappings qualified
Implementation provenance: independent analysis of captured I/O and output labels;
no private Sony tracking implementation or canonical model geometry transferred
