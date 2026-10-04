# Passive Sony optical capture

Branch: experiment/native-optical-capture

This branch is for the next Windows oracle session. It leaves Sony's controller/optical implementation in control and captures enough state to correlate native LED commands, raw tracking-camera frames, and Sony's own internal controller-to-blob associations.

## Capture directory

When captureSonyOpticalTrace is enabled (default true on this experimental branch), Toolkit creates:

    %TEMP%\\psvr2-toolkit-optical-<vrserver-pid>\\

The same path is printed to vrserver.txt with the [Sony Optical Capture] prefix.

## Files

- events.csv: host timestamp, event id, LED command or tracking-state transition, controller side, current phase/sequence/period/base time/frame cycle, and exact raw command bytes. Unknown command type 6 is therefore captured byte-for-byte.
- camera_frames.csv: saved type-11 tracking frames and the event that triggered them.
- camera-....vi11: complete native type-11 VI records. Each is a 256-byte VI header followed by two BC4 1024x1016 camera textures. Three frames are saved after phase/mask/timing mutations and tracking-state transitions, capped at 96 frames. Periodic command type 6 is logged but deliberately does not consume the image budget.
- optical_summary.csv: for each Sony optical-processing frame, controller, and camera, the 17-bit set of LED IDs with assigned blobs, the subset Sony marks matched, and matched count.
- optical-L.bin / optical-R.bin: raw post-OpticalProcessor::process Sony controller optical working sets. Each fixed 0x5A44-byte payload has a small SOPT header with timestamp and frame index. Capture is capped at 256 MiB per controller.

The raw optical blocks are retained because only a few field offsets are currently known. They let us identify blob coordinates/scores later without repeating the native run.

## Decode camera frames

Use the dependency-free decoder:

    py scripts\\decode_psvr2_vi11.py "$env:TEMP\\psvr2-toolkit-optical-<pid>"

It writes left/right 8-bit PGM images beside each .vi11 file. The useful image width is 1016 pixels; the 1024-wide BC4 texture contains 8 columns of padding. Use --gain 2 if the IR blobs are faint.

Do not assume the four-byte Sony leds[] field is a literal 32-bit physical-LED mask. In the successful trace only the first byte changed (01, 03, 06, 07, 0a, 0c, 0e, and others) even though Sense has 17 model LEDs. The raw frames plus Sony's 17-LED association table are intended to establish what that field means physically.

Monado's 17-LED controller model was also extracted from Sony's PC driver and uses IDs 0..16. Sony's optical working set contains exactly 17 LED-to-blob entries per camera. The obvious hypothesis is that these are the same IDs/order; do not assume it silently, but the capture should let us verify it directly against the decoded camera spots and Monado's known 3D LED coordinates.

## Analyse Bluetooth ETW capture

    py scripts\\analyze_sony_sense_etw_pcap.py steamvr-success.pcapng.gz

Optional CSV:

    py scripts\\analyze_sony_sense_etw_pcap.py steamvr-success.pcapng.gz --csv a231.csv --runs-csv schedule-runs.csv

The parser accepts only CRC-valid Sony A2/31 reports, avoiding false byte matches inside ETW headers or incoming payloads.

## Recommended next oracle run

Use the same stock-Sony configuration that produced successful right-controller 6DoF: useToolkitSync=false, passive LED trace, corrected camera report 0x0b with wValue=0x0b, plus the camera/HMD/proximity/display workarounds already used on the successful branch.

A 60-90 second right-controller run is enough. Keep it visible until full 6DoF, rotate it slowly so different ring sectors face the cameras, deliberately occlude it once until tracking is lost, then expose it again, and keep it tracked long enough for Sony to enter BG if possible. Capture Bluetooth ETW simultaneously.

## Non-interference

The new hooks copy/log data and then return to Sony. The camera frame is copied only after the normal Sony image poll completes. The optical working set is copied after OpticalProcessor::process returns, so the captured LED/blob association table is Sony's processed state, not a replacement tracker.