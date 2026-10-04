#include "sony_optical_capture.h"

#include "util.h"
#include "vr_settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace psvr2_toolkit {
namespace {

constexpr uint32_t kPreFramesPerEvent = 2;
constexpr uint32_t kPostFramesPerEvent = 3;
constexpr uint32_t kMaxCameraFrames = 160;

struct FrameSnapshot {
  std::vector<uint8_t> bytes;
  uint64_t hostTimestampUs = 0;
  uint32_t imageTimestamp = 0;
  uint16_t imageType = 0;
};

struct CaptureState {
  std::once_flag initOnce;
  std::filesystem::path directory;
  std::ofstream events;
  std::ofstream frames;
  std::ofstream poses;
  std::mutex mutex;

  std::atomic<uint64_t> eventId{0};

  std::array<FrameSnapshot, kPreFramesPerEvent> history;
  size_t historyCount = 0;
  size_t historyNext = 0;

  uint64_t activeEventId = 0;
  uint32_t postFramesRemaining = 0;
  int32_t nextPostRelativeIndex = 1;

  uint32_t cameraFramesWritten = 0;
};

CaptureState &
state()
{
  static CaptureState s;
  return s;
}

uint64_t
host_timestamp_us()
{
  static LARGE_INTEGER frequency = {};
  if (frequency.QuadPart == 0) {
    QueryPerformanceFrequency(&frequency);
  }

  LARGE_INTEGER now = {};
  QueryPerformanceCounter(&now);
  return static_cast<uint64_t>(
      (static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart)) * 1e6);
}

std::string
hex_bytes(const void *data, size_t size)
{
  if (data == nullptr || size == 0) {
    return {};
  }

  const auto *bytes = static_cast<const uint8_t *>(data);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (size_t i = 0; i < size; ++i) {
    if (i != 0) {
      out << ' ';
    }
    out << std::setw(2) << static_cast<unsigned>(bytes[i]);
  }
  return out.str();
}

void
initialize_if_needed()
{
  CaptureState &s = state();
  std::call_once(s.initOnce, [&]() {
    try {
      s.directory = std::filesystem::temp_directory_path() /
                    ("psvr2-toolkit-optical-" + std::to_string(GetCurrentProcessId()));
      std::filesystem::create_directories(s.directory);

      s.events.open(s.directory / "events.csv", std::ios::out | std::ios::trunc);
      s.frames.open(s.directory / "camera_frames.csv", std::ios::out | std::ios::trunc);
      s.poses.open(s.directory / "poses.csv", std::ios::out | std::ios::trunc);

      if (s.events) {
        s.events << "host_us,event_id,kind,side,current_phase,current_seq,current_period,base_time,frame_cycle,payload_hex\n";
      }
      if (s.frames) {
        s.frames << "host_us,capture_index,event_id,relative_frame,image_timestamp,image_type,size,filename\n";
      }
      if (s.poses) {
        s.poses
            << "host_us,device,index,pose_time_offset_s,pose_valid,connected,tracking_result,"
               "px,py,pz,qw,qx,qy,qz,vx,vy,vz,avx,avy,avz,"
               "world_qw,world_qx,world_qy,world_qz,world_tx,world_ty,world_tz,"
               "head_qw,head_qx,head_qy,head_qz,head_tx,head_ty,head_tz\n";
      }

      Util::DriverLog(
          "[Sony Optical Capture] directory={} camera-only clean-room capture: {} pre + {} post frames/event, "
          "maxCameraFrames={}",
          s.directory.string(), kPreFramesPerEvent, kPostFramesPerEvent, kMaxCameraFrames);
    } catch (const std::exception &e) {
      Util::DriverLog("[Sony Optical Capture] initialization failed: {}", e.what());
    }
  });
}

uint64_t
new_event_id()
{
  return state().eventId.fetch_add(1) + 1;
}

void
write_frame_locked(CaptureState &s, const FrameSnapshot &frame, uint64_t eventId, int32_t relativeFrame)
{
  if (s.cameraFramesWritten >= kMaxCameraFrames || s.directory.empty() || frame.bytes.empty()) {
    return;
  }

  const uint32_t captureIndex = ++s.cameraFramesWritten;

  std::ostringstream filename;
  filename << "camera-" << std::setfill('0') << std::setw(4) << captureIndex << "-event-" << std::setw(4)
           << eventId << "-rel-" << (relativeFrame >= 0 ? "p" : "m") << std::setw(2)
           << (relativeFrame >= 0 ? relativeFrame : -relativeFrame) << "-ts-" << frame.imageTimestamp << ".vi11";

  const std::filesystem::path path = s.directory / filename.str();
  std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!out) {
    return;
  }

  out.write(reinterpret_cast<const char *>(frame.bytes.data()),
            static_cast<std::streamsize>(frame.bytes.size()));
  out.close();

  if (s.frames) {
    s.frames << frame.hostTimestampUs << ',' << captureIndex << ',' << eventId << ',' << relativeFrame << ','
             << frame.imageTimestamp << ',' << frame.imageType << ',' << frame.bytes.size() << ','
             << filename.str() << "\n";
    s.frames.flush();
  }
}

void
arm_camera_event_locked(CaptureState &s, uint64_t eventId)
{
  /*
   * Dump the two frames immediately preceding the event, oldest first.
   * historyNext points at the slot that will be overwritten next.
   */
  if (s.historyCount > 0) {
    const size_t count = s.historyCount;
    const size_t oldest =
        s.historyCount == kPreFramesPerEvent ? s.historyNext : 0;

    for (size_t i = 0; i < count; ++i) {
      const size_t idx = (oldest + i) % kPreFramesPerEvent;
      const int32_t relative = -static_cast<int32_t>(count - i);
      write_frame_locked(s, s.history[idx], eventId, relative);
    }
  }

  s.activeEventId = eventId;
  s.postFramesRemaining = kPostFramesPerEvent;
  s.nextPostRelativeIndex = 1;
}

void
record_event(bool isLeft,
             const char *kind,
             const void *payload,
             size_t payloadSize,
             uint8_t currentPhase,
             uint8_t currentSequence,
             uint8_t currentPeriod,
             int32_t baseTime,
             uint32_t frameCycle,
             bool captureFrames)
{
  initialize_if_needed();
  CaptureState &s = state();
  const uint64_t id = new_event_id();
  const uint64_t hostUs = host_timestamp_us();

  std::lock_guard<std::mutex> lock(s.mutex);

  if (captureFrames) {
    arm_camera_event_locked(s, id);
  }

  if (s.events) {
    s.events << hostUs << ',' << id << ',' << kind << ',' << (isLeft ? 'L' : 'R') << ','
             << static_cast<unsigned>(currentPhase) << ',' << static_cast<unsigned>(currentSequence) << ','
             << static_cast<unsigned>(currentPeriod) << ',' << baseTime << ',' << frameCycle << ",\""
             << hex_bytes(payload, payloadSize) << "\"\n";
    s.events.flush();
  }
}

} // namespace

bool
SonyOpticalCapture::Enabled()
{
  static const bool enabled =
      VRSettings::GetBool(STEAMVR_SETTINGS_CAPTURE_SONY_OPTICAL_TRACE,
                          SETTING_CAPTURE_SONY_OPTICAL_TRACE_DEFAULT_VALUE);
  return enabled;
}

void
SonyOpticalCapture::NoteLedCommand(bool isLeft,
                                   const void *command,
                                   size_t commandSize,
                                   uint8_t currentPhase,
                                   uint8_t currentSequence,
                                   uint8_t currentPeriod,
                                   int32_t baseTime,
                                   uint32_t frameCycle)
{
  if (!Enabled()) {
    return;
  }

  const auto *rawCommand = static_cast<const uint8_t *>(command);
  const uint8_t commandType = commandSize > 0 && rawCommand ? rawCommand[0] : 0xff;

  /*
   * Save before/after frames only for commands that can visibly change the illumination pattern:
   * SET_SYNC_PHASE (1) and SET_LEDS_IMMEDIATE (2). Timing-only changes and the ~1 Hz maintenance
   * command remain in events.csv but do not consume the bounded frame budget.
   */
  const bool captureFrames = commandType == 1 || commandType == 2;

  record_event(isLeft, "led", command, commandSize, currentPhase, currentSequence, currentPeriod, baseTime,
               frameCycle, captureFrames);
}

void
SonyOpticalCapture::NoteTracking(bool isLeft, int oldFlag, int newFlag)
{
  if (!Enabled()) {
    return;
  }

  std::ostringstream payload;
  payload << "flag " << oldFlag << " -> " << newFlag;
  const std::string text = payload.str();

  record_event(isLeft, "tracking", text.data(), text.size(), 0xff, 0xff, 0xff, 0, 0, true);
}

void
SonyOpticalCapture::CaptureTrackingImage(const void *imageData,
                                         size_t imageSize,
                                         uint32_t imageTimestamp,
                                         uint16_t imageType)
{
  if (!Enabled() || imageType != 11 || imageData == nullptr || imageSize == 0) {
    return;
  }

  initialize_if_needed();

  FrameSnapshot current;
  current.hostTimestampUs = host_timestamp_us();
  current.imageTimestamp = imageTimestamp;
  current.imageType = imageType;
  const auto *bytes = static_cast<const uint8_t *>(imageData);
  current.bytes.assign(bytes, bytes + imageSize);

  CaptureState &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);

  if (s.postFramesRemaining > 0 && s.activeEventId != 0) {
    write_frame_locked(s, current, s.activeEventId, s.nextPostRelativeIndex);
    --s.postFramesRemaining;
    ++s.nextPostRelativeIndex;
  }

  s.history[s.historyNext] = std::move(current);
  s.historyNext = (s.historyNext + 1) % kPreFramesPerEvent;
  s.historyCount = std::min(s.historyCount + 1, static_cast<size_t>(kPreFramesPerEvent));
}

void
SonyOpticalCapture::CapturePublishedPose(const char *deviceLabel,
                                         uint32_t deviceIndex,
                                         const vr::DriverPose_t &pose)
{
  if (!Enabled() || deviceLabel == nullptr) {
    return;
  }

  initialize_if_needed();
  CaptureState &s = state();
  const uint64_t hostUs = host_timestamp_us();

  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.poses) {
    return;
  }

  s.poses << hostUs << ',' << deviceLabel << ',' << deviceIndex << ',' << pose.poseTimeOffset << ','
          << (pose.poseIsValid ? 1 : 0) << ',' << (pose.deviceIsConnected ? 1 : 0) << ','
          << static_cast<int>(pose.result) << ','
          << pose.vecPosition[0] << ',' << pose.vecPosition[1] << ',' << pose.vecPosition[2] << ','
          << pose.qRotation.w << ',' << pose.qRotation.x << ',' << pose.qRotation.y << ',' << pose.qRotation.z << ','
          << pose.vecVelocity[0] << ',' << pose.vecVelocity[1] << ',' << pose.vecVelocity[2] << ','
          << pose.vecAngularVelocity[0] << ',' << pose.vecAngularVelocity[1] << ',' << pose.vecAngularVelocity[2] << ','
          << pose.qWorldFromDriverRotation.w << ',' << pose.qWorldFromDriverRotation.x << ','
          << pose.qWorldFromDriverRotation.y << ',' << pose.qWorldFromDriverRotation.z << ','
          << pose.vecWorldFromDriverTranslation[0] << ',' << pose.vecWorldFromDriverTranslation[1] << ','
          << pose.vecWorldFromDriverTranslation[2] << ','
          << pose.qDriverFromHeadRotation.w << ',' << pose.qDriverFromHeadRotation.x << ','
          << pose.qDriverFromHeadRotation.y << ',' << pose.qDriverFromHeadRotation.z << ','
          << pose.vecDriverFromHeadTranslation[0] << ',' << pose.vecDriverFromHeadTranslation[1] << ','
          << pose.vecDriverFromHeadTranslation[2] << "\n";
}

} // namespace psvr2_toolkit
