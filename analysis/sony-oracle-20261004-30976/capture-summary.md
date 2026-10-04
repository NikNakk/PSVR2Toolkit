# Second oracle run: experiment reconstruction (PID 30976)

Inputs: `%TEMP%\psvr2-toolkit-optical-30976` and `../sense-research/steamvr-success-3.pcapng.gz`.
The earlier PID 31876 capture was used only to investigate IF8 startup continuity.
No raw capture files were copied or changed. No Monado implementation was modified.

## Reconstructed actions

The six candidate X presses occur at +93.548, +119.760, +151.874, +154.641, +184.873 and +203.225 seconds from the first capture clock pair (about 16:08:59.254 BST). They toggle A1/31 report byte 9 bit 1 on ACL handle 12. The user's recollection confirms only their approximate order; the action labels below come primarily from motion and semantic observations, not exact button timing.

| Approximate interval | Evidence-backed interpretation |
|---|---|
| Before +94 s | Long lead-in with several movements and quieter intervals; first X press is near entry into a clear quiet hold. |
| +94Ã¢â‚¬â€œ119 s | Quiet hold: median position range 2.33 mm per second; orientation range 0.39 degrees; mean/median optical union near 8 LEDs. |
| Around +119Ã¢â‚¬â€œ120 s | Controller rotation/repositioning, followed by another quiet hold. |
| +120Ã¢â‚¬â€œ148 s | Second quiet hold: median position range 2.34 mm per second; orientation range 0.40 degrees; union around 8.3 LEDs. |
| +148.345Ã¢â‚¬â€œ153.469 s | First sustained zero-match interval, consistent with the first cover. The +151.874 X press is **inside** it, not its onset. |
| +154.5Ã¢â‚¬â€œ184.9 s | Quiet hold after optical return: median position range 2.53 mm per second; orientation range 0.43 degrees; union around 8 LEDs. |
| +185Ã¢â‚¬â€œ195.7 s | Clear free movement: median position range ~0.40 m per second and orientation excursion ~86 degrees. |
| +195.724Ã¢â‚¬â€œ199.496 s | Second sustained zero-match interval, consistent with the second cover. |
| +199.5Ã¢â‚¬â€œ203.5 s | Movement resumes after optical return. |
| +203.5Ã¢â‚¬â€œ212.9 s | Settling/repositioning. HMD movement is substantial during roughly +206Ã¢â‚¬â€œ212 s, while the controller's rotational input signal is relatively quiet. |
| +212.858Ã¢â‚¬â€œ248.376 s | Probable teardown/slow-shutdown interval, also rotationally quiet. No right LED matches for 35.517 s; published controller positions eventually freeze. HMD pose updates end at +220.002 s. |

The raw input report supplies an independently observed rotational proxy: the magnitude of three little-endian signed 16-bit fields beginning at report offset 17 correlates with the published right-controller angular-speed magnitude at **r = 0.994907**, across 28,805 time-near samples. Adjacent/alternative offsets perform substantially worse. This strongly supports a gyro-like triplet, but its axis mapping and units are not asserted. The correlation is not an independent pose-accuracy validation: Sony can use those same sensor readings to produce its public angular velocity.

After +220 s, 1,427 full input reports are distinct and the rotational triplet takes 14 values. Thus the final low signal is not simply replay of one identical captured input packet. It supports low rotational motion; it does not by itself prove absence of translational movement or optical tracking. Published pose validity stays true from +2.955 s through the rest of the run, including both covers and the final zero-match interval.

`experiment-timeline.png` plots movement, the wire rotational candidate and semantic union; purple lines mark candidate X presses, red bars mark zero-match intervals. `experiment-seconds.csv` and `reconstructed-intervals.csv` retain the numerical evidence. Position graph peaks are clipped at 0.3 m; numerical CSVs preserve their full values.

## Scheduling findings and comparison with the first run

- Handle 12 is identified as the right-controller candidate by the user's right-only marker presses and the controller connection ordering; handle 13 has no corresponding presses and connects later, consistent with the left controller. The ACL handle itself is a connection identity, not an intrinsic left/right identifier. All accepted handle assignments validate adjacent ACL length 83 and L2CAP length 79. CID is 0x42.
- Both controllers have separate command streams. The complete trace contains 16,846 valid A2/31 reports for handle 12 and 16,492 for handle 13, plus 28,839/28,412 valid A1/31 reports respectively. The analysis does **not** interleave their schedule runs. The parser and semantic tools now reject ambiguous multi-controller analysis unless a selector is supplied.
- Right-controller acquisition is already achieved at +2.955 s during PRESCAN; initial BROAD starts at +5.577 s. Acquisition is much quicker than in PID 7164.
- Quiet holds still include the recurring approximately ten-second BROAD/PRESCAN cycle. Most right BROAD episodes are 9.997Ã¢â‚¬â€œ10.027 s, with one shorter 9.097 s episode. The ten-second maintenance interpretation is strengthened, but an exact universal ten-second law would overstate the evidence.
- PRESCAN usually lasts about 3.01Ã¢â‚¬â€œ3.03 s. The interval spanning the first cover lasts **10.604 s** (+145.883Ã¢â‚¬â€œ156.487); semantic matches resume at +153.469, about three seconds before BROAD. This is consistent with reacquisition/maintenance completion taking longer during that disturbance, but does not establish the exact completion criterion.
- During the second cover, BROAD begins at +196.484 while semantic matches are still zero (they resume at +199.496). This counterexample prevents adopting a universal rule that BROAD requires three uninterrupted seconds of semantic LED matches.
- The final zero-match interval starts at +212.858 while still in BROAD. PRESCAN begins at +218.617 and persists for 29.752 s until shutdown. There is no BG/STABLE phase on either handle in the saved trace, including during the earlier quiet, well-observed holds.
- Right BROAD uses 05, 0a, 0c, 0e, **11** and ff. Value 11 is additional evidence beyond the previous low-nibble examples. Left BROAD also has 06, 07, 09, 0b and 0d. These are wire values, not proven physical-LED masks.
- The apparent sparse result for right `05` is not reproduced: mean cross-camera matched union is **8.695** in this run versus **0.433** previously. Visibility, orientation, temporal behavior and tracking context remain confounders. This does not exclude every possible emitter-group model, but it invalidates assigning the previously observed sparse subset to 05 as an established rule.

## Coverage, validation and IF8

The complete local CSVs, every saved VI record, complete IF8 stream, and all 124,321 ETW enhanced packet blocks were processed. ETW framing has no truncated EPBs. Clock fit over 249 pairs is 256.076 us RMS, maximum absolute residual 1.445 ms. Host-clock fit error is separate from transport/exposure latency.

The capture has 118,076 semantic rows (59,096 right, 58,980 left) and 285,412 published poses. Right semantic output comprises 14,774 frames. The unified right timeline includes all right rows, separate wire settings, pose and USB association distances, event side, and marker associations. The base table is a generated intermediate excluded from the commit; `unified-timeline-with-markers.csv` is the final enriched table. Near an optical transition, sparse VI/IF8 associations must be filtered by their residuals rather than treated as same-frame matches.

The native budgets were still **240 VI frames and 256 MiB IF8**. All 240 VI files validate (214 distinct sequences), but saved imagery ends at +55.000 s. IF8 ends at +121.001 s. Consequently, none of the marked quiet holds/cover events has saved VI imagery; only the first marked quiet hold has substantial IF8 coverage. The later optical evidence is semantic output, not contemporaneously saved raw detector/image data.

All 7,266 IF8 payloads validate the previously inferred `64 + 4*(4 + 256*36)` structure. All 333,259 populated record coordinate candidates have ordered bounds in 0..508, with zero unused tails. All 240 VI rows have common-counter IF8 matches. All 129,900 time-near right semantic matched labels fall within their same-numbered section's zero-based count bounds. IF8 is shared between both visible controllers and background detections: its aggregate count differences cannot be attributed solely to the right schedule.

Two counter jumps have 935 absent positions in total, **but 932 occur at startup**. The first new IF8 record is counter 6455 / device stamp 113,115,674. The previous capture ends at counter 6454 / stamp 113,098,991, only one device-frame interval earlier although host time is 15.594 s earlier. The second new record jumps 933 device frames in 697 host microseconds. This strongly suggests an old/delayed first packet crossing the restart boundary. It must not be reported as 932 dropped frames during the four-minute experiment. The later jump of four leaves three absent counter positions. Exact buffering/drop location remains unresolved; `restart-if8-check.json` preserves the comparison.

No confident visible LED or physical LED-to-VI-region mapping is established. Coordinate projections and raw-lane difference sheets remain candidate wire-byte visualizations. This dataset adds much better motion/occlusion segmentation but does not fix the missing late raw-camera coverage.

## Remaining questions and next step

The first and second covers can be separated from movement without relying on exact X timing. The user subsequently reported that the end may be taking the equipment off before stopping SteamVR, which is slow to close. This fits matches ending near +213 s, HMD updates ending near +220 s and controller reports continuing to +248 s. Treat this tail as **probable teardown/slow shutdown**, not the intended final tracking hold. Low rotational input, valid/frozen controller pose and absent semantic LEDs remain distinct observations. Its specific cause (viewpoint loss, headset/camera inactivity, occlusion or another condition) cannot be selected from these saved raw streams.

Before another image-oriented capture, increase the actual compiled VI/IF8 budgets and verify the new values in the startup log. Use at least 1,000 selective VI frames and 512 MiB IF8 for a 180-second run; this run lasted about 247 s, so 768 MiB IF8 provides margin. Mark both cover onset and removal, retain headset camera activity, and keep the final controller hold inside verified camera view with continuing semantic matches. Another run is not needed merely to recover this experiment's main action sequence.

## Reproduce

Use Python 3 with NumPy and Pillow:

```powershell
python scripts/analyze_optical_capture_inventory.py "$env:TEMP/psvr2-toolkit-optical-30976" --out analysis/sony-oracle-20261004-30976
python scripts/analyze_sense_button_markers.py ../sense-research/steamvr-success-3.pcapng.gz --inventory analysis/sony-oracle-20261004-30976/capture-inventory.json --out analysis/sony-oracle-20261004-30976
python scripts/analyze_sony_optical_capture.py "$env:TEMP/psvr2-toolkit-optical-30976" ../sense-research/steamvr-success-3.pcapng.gz --out analysis/sony-oracle-20261004-30976 --controller-side R --bt-handle 12
python scripts/analyze_psvr2_if8_sections.py "$env:TEMP/psvr2-toolkit-optical-30976" --out analysis/sony-oracle-20261004-30976 --controller-side R
python scripts/analyze_vi_if8_coordinates.py "$env:TEMP/psvr2-toolkit-optical-30976" --out analysis/sony-oracle-20261004-30976 --controller-side R
python scripts/reconstruct_sony_oracle_experiment.py "$env:TEMP/psvr2-toolkit-optical-30976" ../sense-research/steamvr-success-3.pcapng.gz --out analysis/sony-oracle-20261004-30976 --handle 12
python scripts/test_optical_capture_parsers.py
```

Do **not** apply the reversed-side correction to this capture. Quaternion motion comparisons normalize the published quaternion components before computing angular differences. The raw-input motion candidate is tested across every byte offset 12..64, without consulting a private Sony tracker/IMU layout.

Feature: Sense experiment markers, motion/occlusion reconstruction and scheduling during quiet holds
Source of knowledge: externally observable Bluetooth ACL/HID and USB capture; Sony's permitted semantic LED output and public OpenVR poses; user's approximate experiment recollection
Method: CRC/framing-validated handle demultiplexing, clock alignment, button-edge extraction, pose/rotational-sensor correlation, explicit-side semantic aggregation and complete wire-stream validation
Observed protocol/behaviour: observations above; inferred action labels and candidate sensor fields explicitly qualified
Implementation provenance: independent local analysis of observable reports and permitted output labels; no Sony private implementation/structures or proprietary geometry copied, and no downstream Monado code implemented
