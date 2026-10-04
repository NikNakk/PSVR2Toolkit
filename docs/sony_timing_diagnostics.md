# Sony PS VR2 timing diagnostics

This branch adds passive tracing around the existing PSVR2Toolkit OpenVR proxy. It does **not** alter Sony's HMD pose before forwarding it to SteamVR.

The goal is to compare Sony's Windows tracking/presentation timing with the macOS Monado PS VR2 implementation.

## Captured data

The proxy records:

- every HMD `TrackedDevicePoseUpdated` callback, including `poseTimeOffset`, position/orientation, linear velocity, angular velocity, validity and tracking result;
- every `VsyncEvent(vsyncTimeOffsetSeconds)` callback;
- `Prop_ReportsTimeSinceVSync_Bool`, `Prop_SecondsFromVsyncToPhotons_Float` and `Prop_DisplayFrequency_Float` when the HMD activates;
- `TrackedDeviceAdded` events, which are useful on unsupported PCs because they show how far Sony's driver got even if tracking or display setup later fails.

All trace timestamps are derived from `QueryPerformanceCounter` and expressed as monotonic nanoseconds, so the CSV files can be aligned directly.

## Output files

By default the proxy writes to the Windows temporary directory (`%TEMP%`):

```text
psvr2toolkit_<PID>_pose.csv
psvr2toolkit_<PID>_vsync.csv
psvr2toolkit_<PID>_properties.csv
psvr2toolkit_<PID>_events.csv
```

An alternate output directory can be selected by setting `PSVR2TOOLKIT_TRACE_DIR` in the environment inherited by SteamVR. The directory must already exist.

## First diagnostic run

On the Windows PC:

1. Install Steam, SteamVR, the PlayStation VR2 app and PSVR2Toolkit normally.
2. Install the build from this branch in place of the normal PSVR2Toolkit proxy DLL.
3. Connect and power the PS VR2 PC adapter and headset. For the first diagnostic attempt, USB connectivity is the important part; a working DisplayPort path is not required to determine whether Sony's driver emits tracking callbacks on otherwise unsupported hardware.
4. Start SteamVR/the Sony PS VR2 driver path.
5. If tracking starts, move the headset for roughly 20 seconds: first a slow constant yaw, then a slow translation, then hold it still.
6. Exit SteamVR cleanly so buffered pose/vsync rows are flushed, then collect all `psvr2toolkit_<PID>_*.csv` files from `%TEMP%`.

## What success looks like

- `events.csv` only: Sony's proxy loaded and the driver registered at least one device, but activation/tracking may have failed later.
- `properties.csv` contains a data row: HMD activation reached the point where OpenVR timing properties could be queried. The `*_error` columns show whether each Sony property was actually set.
- `pose.csv` contains data rows: this machine is sufficient to observe Sony's tracking output even if video does not work.
- `vsync.csv` contains data rows: Sony's display timing path is active far enough to emit a vsync reference.

The first quantities to compare with Monado are pose callback cadence, `pose_time_offset_s`, linear/angular velocity continuity across tracking corrections, and the relationship between Sony's `VsyncEvent` offset and its reported vsync-to-photons interval.

## Performance impact

Pose and vsync rows are buffered and flushed periodically rather than flushing every callback, to minimise disturbance to the timing being measured. Device and property events are flushed immediately because they are rare.

This branch is intended as a short-lived diagnostic aid rather than a permanent feature.
