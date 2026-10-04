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
Method: passive LED/state tracing for event timestamps, raw USB type-11 camera capture, and simultaneous Bluetooth ETW
capture of the actual A2/31 controller output
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
508 rows × (254 samples × 8 byte lanes + 16 bytes row padding)
```

The physical/semantic meaning of those lanes is intentionally left open until a new capture verifies the VI header and
before/after image behaviour.

## Inspect/extract observable VI lanes

```powershell
py scripts\extract_psvr2_vi_lanes.py "$env:TEMP\psvr2-toolkit-optical-<pid>"
```

This prints each frame's raw VI header metadata and writes lanes 0..7 as semantics-free 254×508 PGM images. It does
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
