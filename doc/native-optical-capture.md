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
- current phase/sequence/period/base time/frame cycle;
- exact bytes supplied to the narrow Sony LED-command hook;
- Sony tracking-state transitions used only as capture landmarks.

All commands are logged. Camera frames are triggered only for commands that can visibly change illumination
(`SET_SYNC_PHASE` and `SET_LEDS_IMMEDIATE`) plus tracking-state transitions. Periodic command type 6 and timing-only
adjustments remain in the CSV but do not consume the image budget.

### camera_frames.csv and camera-*.vi11

Toolkit keeps a rolling two-frame history of the observable type-11 USB camera stream. For each interesting event it
saves:

- two frames immediately before the event (`relative_frame=-2,-1`);
- three frames immediately after it (`relative_frame=+1,+2,+3`).

Capture is capped at 160 frames.

Each `.vi11` file contains the complete native type-11 VI record:

- 256-byte VI header;
- two BC4 `1024x1016` camera textures;
- useful image width 1016 pixels; final 8 columns are texture padding.

This gives a direct before/after record of which IR image features changed when the native controller report changed.

## Decode frames

Dependency-free decoder:

```powershell
py scripts\decode_psvr2_vi11.py "$env:TEMP\psvr2-toolkit-optical-<pid>"
```

It writes left/right 8-bit PGM images beside each captured frame. Use `--gain 2` if needed for viewing faint IR
features.

## Difference LED events automatically

```powershell
py scripts\analyze_psvr2_led_frame_changes.py "$env:TEMP\psvr2-toolkit-optical-<pid>"
```

For every event with both a `-1` and `+1` frame, this writes:

- pre-event PGM;
- post-event PGM;
- absolute-difference PGM;
- `led-frame-diffs\regions.csv` containing connected changed regions, bounding boxes, centroids, area and peak/mean
  luma change.

Useful options:

```powershell
py scripts\analyze_psvr2_led_frame_changes.py "$env:TEMP\psvr2-toolkit-optical-<pid>" --threshold 24 --min-area 2
```

With the controller held still, a `SET_LEDS_IMMEDIATE` event should therefore reveal the physical emitters affected
by values such as `0x01`, `0x03`, `0x06`, `0x07`, `0x0a`, `0x0c`, etc, without depending on Sony internal
tracker data.

Do not assume those values are a literal 17-bit physical-LED bitmap. The successful trace changed only the first byte
of the four-byte field even though the Sense constellation model has 17 LEDs.

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

The camera copy happens after Sony's normal image poll has completed. The new code does not replace Sony's controller
output, pose solver or camera processing. It observes the same camera USB payload Sony receives and uses event timestamps
only to choose which nearby frames to retain.
