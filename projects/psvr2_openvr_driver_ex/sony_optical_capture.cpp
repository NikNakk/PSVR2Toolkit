#include "sony_optical_capture.h"

#include "util.h"
#include "vr_settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace psvr2_toolkit {
namespace {

constexpr uint32_t kPreFramesPerEvent = 2;
constexpr uint32_t kPostFramesPerEvent = 3;
constexpr uint32_t kMaxCameraFrames = 240;
constexpr uint32_t kPeriodicCameraSampleStride = 60; // ~1 Hz at the observed 60 Hz stream.
constexpr size_t kMaxQueuedUsbPackets = 512;
constexpr uint64_t kMaxLedDetectorBytes = 256ULL * 1024ULL * 1024ULL;

#pragma pack(push, 1)
struct ObservableCameraHeader {
  char magic[2];
  uint16_t version;
  uint32_t packetSize;
  uint32_t vtsUs;
  uint32_t sequenceId;
  uint16_t cameraSet;
  uint16_t imageHeight;
  uint16_t activeHeight;
  uint16_t imageWidth;
  uint16_t activeWidth;
  uint16_t unknown2;
};
#pragma pack(pop)

static_assert(sizeof(ObservableCameraHeader) == 28, "Unexpected observable camera header size");

struct FrameSnapshot {
  std::vector<uint8_t> bytes;
  uint64_t hostTimestampUs = 0;
  uint32_t imageTimestamp = 0;
  uint16_t cameraSet = 0;
  uint32_t sequenceId = 0;
  uint16_t imageHeight = 0;
  uint16_t activeHeight = 0;
  uint16_t imageWidth = 0;
  uint16_t activeWidth = 0;
};

struct QueuedFrame {
  FrameSnapshot frame;
  uint64_t eventId = 0;
  int32_t relativeFrame = 0;
  uint32_t captureIndex = 0;
  std::string filename;
};

struct UsbDetectorPacket {
  std::vector<uint8_t> bytes;
  uint64_t hostTimestampUs = 0;
  uint8_t interfaceNumber = 0;
  uint8_t pipeId = 0;
};

struct LedGroundTruthRow {
  uint64_t hostTimestampUs = 0;
  uint64_t frameIndex = 0;
  uint32_t controllerIdx = 0;
  uint8_t cameraIndex = 0;
  uint32_t assignedMask = 0;
  uint32_t matchedMask = 0;
  std::array<int16_t, 17> blobIndices{};
};

#pragma pack(push, 1)
struct UsbDetectorRecordHeader {
  uint32_t magic; // 'ULD8'
  uint16_t version;
  uint8_t interfaceNumber;
  uint8_t pipeId;
  uint64_t hostTimestampUs;
  uint32_t payloadSize;
};
#pragma pack(pop)

struct ActiveEvent {
  uint64_t eventId = 0;
  uint32_t postFramesRemaining = 0;
  int32_t nextPostRelativeIndex = 1;
};

struct CaptureState {
  std::once_flag initOnce;
  std::filesystem::path directory;
  std::ofstream events;
  std::ofstream frames;
  std::ofstream poses;
  std::ofstream clockSync;
  std::ofstream ledGroundTruth;
  std::ofstream ledDetector;

  std::mutex mutex;
  std::condition_variable writerCv;
  std::deque<QueuedFrame> writerQueue;
  std::deque<UsbDetectorPacket> usbWriterQueue;
  std::deque<LedGroundTruthRow> ledGroundTruthQueue;
  std::thread writerThread;
  bool stopWriter = false;

  std::atomic<uint64_t> eventId{0};

  std::array<FrameSnapshot, kPreFramesPerEvent> history;
  size_t historyCount = 0;
  size_t historyNext = 0;
  std::deque<ActiveEvent> activeEvents;

  uint32_t cameraFramesQueued = 0;
  uint64_t cameraFrameOrdinal = 0;
  uint64_t lastClockSyncHostUs = 0;
  bool publishedTrackingStateKnown[3] = {false, false, false};
  bool publishedTrackingValid[3] = {false, false, false};
  int publishedTrackingResult[3] = {0, 0, 0};
  uint64_t ledDetectorBytesQueued = 0;
  uint64_t ledDetectorPacketsDropped = 0;

  ~CaptureState()
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopWriter = true;
    }
    writerCv.notify_all();
    if (writerThread.joinable()) {
      writerThread.join();
    }
  }
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

uint64_t
unix_timestamp_us()
{
  FILETIME ft = {};
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER ticks = {};
  ticks.LowPart = ft.dwLowDateTime;
  ticks.HighPart = ft.dwHighDateTime;
  constexpr uint64_t kWindowsToUnixEpoch100ns = 116444736000000000ULL;
  return (ticks.QuadPart - kWindowsToUnixEpoch100ns) / 10ULL;
}

void
writer_main(CaptureState *s)
{
  for (;;) {
    QueuedFrame frameItem;
    UsbDetectorPacket usbItem;
    LedGroundTruthRow groundTruthItem;
    bool haveFrame = false;
    bool haveUsb = false;
    bool haveGroundTruth = false;

    {
      std::unique_lock<std::mutex> lock(s->mutex);
      s->writerCv.wait(lock, [s]() {
        return s->stopWriter || !s->writerQueue.empty() || !s->usbWriterQueue.empty() ||
               !s->ledGroundTruthQueue.empty();
      });
      if (s->stopWriter && s->writerQueue.empty() && s->usbWriterQueue.empty() &&
          s->ledGroundTruthQueue.empty()) {
        break;
      }

      if (!s->writerQueue.empty()) {
        frameItem = std::move(s->writerQueue.front());
        s->writerQueue.pop_front();
        haveFrame = true;
      } else if (!s->usbWriterQueue.empty()) {
        usbItem = std::move(s->usbWriterQueue.front());
        s->usbWriterQueue.pop_front();
        haveUsb = true;
      } else if (!s->ledGroundTruthQueue.empty()) {
        groundTruthItem = std::move(s->ledGroundTruthQueue.front());
        s->ledGroundTruthQueue.pop_front();
        haveGroundTruth = true;
      }
    }

    if (haveFrame) {
      const std::filesystem::path path = s->directory / frameItem.filename;
      std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
      if (out) {
        out.write(reinterpret_cast<const char *>(frameItem.frame.bytes.data()),
                  static_cast<std::streamsize>(frameItem.frame.bytes.size()));
        out.close();

        if (s->frames) {
          s->frames << frameItem.frame.hostTimestampUs << ',' << frameItem.captureIndex << ','
                    << frameItem.eventId << ',' << frameItem.relativeFrame << ','
                    << frameItem.frame.imageTimestamp << ',' << frameItem.frame.sequenceId << ','
                    << frameItem.frame.cameraSet << ',' << frameItem.frame.imageWidth << ','
                    << frameItem.frame.imageHeight << ',' << frameItem.frame.activeWidth << ','
                    << frameItem.frame.activeHeight << ',' << frameItem.frame.bytes.size() << ','
                    << frameItem.filename << "\n";
          s->frames.flush();
        }
      }
    } else if (haveUsb && s->ledDetector) {
      UsbDetectorRecordHeader header = {};
      header.magic = 0x38444c55; // 'ULD8' on little-endian Windows.
      header.version = 1;
      header.interfaceNumber = usbItem.interfaceNumber;
      header.pipeId = usbItem.pipeId;
      header.hostTimestampUs = usbItem.hostTimestampUs;
      header.payloadSize = static_cast<uint32_t>(usbItem.bytes.size());
      s->ledDetector.write(reinterpret_cast<const char *>(&header), sizeof(header));
      s->ledDetector.write(reinterpret_cast<const char *>(usbItem.bytes.data()),
                           static_cast<std::streamsize>(usbItem.bytes.size()));
    } else if (haveGroundTruth && s->ledGroundTruth) {
      s->ledGroundTruth << groundTruthItem.hostTimestampUs << ',' << groundTruthItem.frameIndex << ','
                        << (groundTruthItem.controllerIdx == 0 ? 'R' : 'L') << ','
                        << static_cast<unsigned>(groundTruthItem.cameraIndex) << ",0x"
                        << std::hex << groundTruthItem.assignedMask << ",0x" << groundTruthItem.matchedMask
                        << std::dec;
      for (int i = 0; i < 17; ++i) {
        s->ledGroundTruth << ',' << groundTruthItem.blobIndices[static_cast<size_t>(i)];
      }
      s->ledGroundTruth << "\n";
    }
  }
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
      s.clockSync.open(s.directory / "clock_sync.csv", std::ios::out | std::ios::trunc);
      s.ledGroundTruth.open(s.directory / "sony_led_ground_truth.csv", std::ios::out | std::ios::trunc);
      s.ledDetector.open(s.directory / "usb-if8-led-detector.bin",
                         std::ios::out | std::ios::binary | std::ios::trunc);

      if (s.events) {
        s.events << "host_us,event_id,kind,side,detail\n";
      }
      if (s.frames) {
        s.frames << "host_us,capture_index,event_id,relative_frame,vts_us,sequence_id,camera_set,"
                    "image_width,image_height,active_width,active_height,size,filename\n";
      }
      if (s.clockSync) {
        s.clockSync << "qpc_us,unix_us\n";
      }
      if (s.ledGroundTruth) {
        s.ledGroundTruth << "host_us,frame_index,side,camera,assigned_mask,matched_mask,blob_0,blob_1,blob_2,blob_3,blob_4,blob_5,blob_6,blob_7,blob_8,blob_9,blob_10,blob_11,blob_12,blob_13,blob_14,blob_15,blob_16\n";
      }
      if (s.poses) {
        s.poses
            << "host_us,device,index,pose_time_offset_s,pose_valid,connected,tracking_result,"
               "px,py,pz,qw,qx,qy,qz,vx,vy,vz,avx,avy,avz,"
               "world_qw,world_qx,world_qy,world_qz,world_tx,world_ty,world_tz,"
               "head_qw,head_qx,head_qy,head_qz,head_tx,head_ty,head_tz\n";
      }

      s.writerThread = std::thread(writer_main, &s);

      Util::DriverLog(
          "[Sony Optical Capture] directory={} camera-only clean-room capture: {} pre + {} post frames/event, "
          "maxCameraFrames={} with ~1 Hz periodic samples (background writer); observable IF8/0x89 LED-detector "
          "USB capture capped at {} MiB",
          s.directory.string(), kPreFramesPerEvent, kPostFramesPerEvent, kMaxCameraFrames,
          kMaxLedDetectorBytes / (1024 * 1024));
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
queue_frame_locked(CaptureState &s, const FrameSnapshot &frame, uint64_t eventId, int32_t relativeFrame)
{
  if (s.cameraFramesQueued >= kMaxCameraFrames || s.directory.empty() || frame.bytes.empty()) {
    return;
  }

  const uint32_t captureIndex = ++s.cameraFramesQueued;

  std::ostringstream filename;
  filename << "camera-" << std::setfill('0') << std::setw(4) << captureIndex << "-event-" << std::setw(4)
           << eventId << "-rel-" << (relativeFrame >= 0 ? "p" : "m") << std::setw(2)
           << (relativeFrame >= 0 ? relativeFrame : -relativeFrame) << "-set-" << frame.cameraSet
           << "-ts-" << frame.imageTimestamp << ".vi";

  QueuedFrame queued;
  queued.frame = frame; // Memory copy only on the Sony/camera callback; disk I/O is done by writer_main.
  queued.eventId = eventId;
  queued.relativeFrame = relativeFrame;
  queued.captureIndex = captureIndex;
  queued.filename = filename.str();
  s.writerQueue.push_back(std::move(queued));
  s.writerCv.notify_one();
}

void
arm_camera_event_locked(CaptureState &s, uint64_t eventId)
{
  /*
   * Preserve the two observable camera frames immediately preceding the event, oldest first.
   * historyNext points at the slot that will be overwritten next.
   */
  if (s.historyCount > 0) {
    const size_t count = s.historyCount;
    const size_t oldest = s.historyCount == kPreFramesPerEvent ? s.historyNext : 0;

    for (size_t i = 0; i < count; ++i) {
      const size_t idx = (oldest + i) % kPreFramesPerEvent;
      const int32_t relative = -static_cast<int32_t>(count - i);
      queue_frame_locked(s, s.history[idx], eventId, relative);
    }
  }

  s.activeEvents.push_back(
      ActiveEvent{eventId, kPostFramesPerEvent, 1});
}

void
record_event(bool isLeft, const char *kind, bool captureFrames)
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
    s.events << hostUs << ',' << id << ',' << kind << ',' << (isLeft ? 'L' : 'R') << ",\"\"\n";
    s.events.flush();
  }
}

void
capture_observable_camera_packet(const void *imageData, size_t imageSize)
{
  if (imageData == nullptr || imageSize < sizeof(ObservableCameraHeader)) {
    return;
  }

  initialize_if_needed();
  CaptureState &s = state();
  const auto *bytes = static_cast<const uint8_t *>(imageData);

  std::lock_guard<std::mutex> lock(s.mutex);

  /*
   * Reuse the two ring-buffer allocations instead of allocating a new ~1 MiB vector at 60 Hz.
   */
  FrameSnapshot &current = s.history[s.historyNext];
  current.hostTimestampUs = host_timestamp_us();
  current.imageTimestamp = 0;
  current.cameraSet = 0;
  current.sequenceId = 0;
  current.imageHeight = 0;
  current.activeHeight = 0;
  current.imageWidth = 0;
  current.activeWidth = 0;
  if (imageSize >= sizeof(ObservableCameraHeader)) {
    ObservableCameraHeader header = {};
    std::memcpy(&header, bytes, sizeof(header));
    if (header.magic[0] == 'V' && header.magic[1] == 'I') {
      current.imageTimestamp = header.vtsUs;
      current.sequenceId = header.sequenceId;
      current.cameraSet = header.cameraSet;
      current.imageHeight = header.imageHeight;
      current.activeHeight = header.activeHeight;
      current.imageWidth = header.imageWidth;
      current.activeWidth = header.activeWidth;
    }
  }
  current.bytes.resize(imageSize);
  std::memcpy(current.bytes.data(), bytes, imageSize);

  ++s.cameraFrameOrdinal;
  if (s.lastClockSyncHostUs == 0 ||
      current.hostTimestampUs - s.lastClockSyncHostUs >= 1000000ULL) {
    s.lastClockSyncHostUs = current.hostTimestampUs;
    if (s.clockSync) {
      s.clockSync << current.hostTimestampUs << ',' << unix_timestamp_us() << "\n";
      s.clockSync.flush();
    }
  }

  if (s.cameraFrameOrdinal % kPeriodicCameraSampleStride == 0) {
    // event_id=0 / relative_frame=0 denotes a periodic clean-room geometry sample.
    queue_frame_locked(s, current, 0, 0);
  }

  for (auto it = s.activeEvents.begin(); it != s.activeEvents.end();) {
    queue_frame_locked(s, current, it->eventId, it->nextPostRelativeIndex);
    ++it->nextPostRelativeIndex;
    if (--it->postFramesRemaining == 0) {
      it = s.activeEvents.erase(it);
    } else {
      ++it;
    }
  }

  s.historyNext = (s.historyNext + 1) % kPreFramesPerEvent;
  s.historyCount = std::min(s.historyCount + 1, static_cast<size_t>(kPreFramesPerEvent));
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
                                   size_t commandSize)
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

  record_event(isLeft, "led_hook", captureFrames);
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

  int stateSlot = -1;
  if (std::strcmp(deviceLabel, "HMD") == 0) {
    stateSlot = 0;
  } else if (std::strcmp(deviceLabel, "L") == 0) {
    stateSlot = 1;
  } else if (std::strcmp(deviceLabel, "R") == 0) {
    stateSlot = 2;
  }

  if (stateSlot >= 0) {
    const bool valid = pose.poseIsValid;
    const int result = static_cast<int>(pose.result);
    if (!s.publishedTrackingStateKnown[stateSlot] ||
        s.publishedTrackingValid[stateSlot] != valid ||
        s.publishedTrackingResult[stateSlot] != result) {
      s.publishedTrackingStateKnown[stateSlot] = true;
      s.publishedTrackingValid[stateSlot] = valid;
      s.publishedTrackingResult[stateSlot] = result;

      const uint64_t id = new_event_id();
      arm_camera_event_locked(s, id);
      if (s.events) {
        std::ostringstream payload;
        payload << "pose_valid=" << (valid ? 1 : 0) << " tracking_result=" << result;
        s.events << hostUs << ',' << id << ",openvr_tracking," << deviceLabel << ",\""
                 << payload.str() << "\"\n";
        s.events.flush();
      }
    }
  }

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

void
SonyOpticalCapture::CaptureLedGroundTruth(uint32_t controllerIdx,
                                         uint64_t frameIndex,
                                         uint8_t cameraIndex,
                                         uint32_t assignedMask,
                                         uint32_t matchedMask,
                                         const int16_t blobIndices[17])
{
  if (!Enabled() || controllerIdx >= 2 || cameraIndex >= 4 || blobIndices == nullptr) {
    return;
  }

  initialize_if_needed();
  CaptureState &s = state();

  LedGroundTruthRow row;
  row.hostTimestampUs = host_timestamp_us();
  row.frameIndex = frameIndex;
  row.controllerIdx = controllerIdx;
  row.cameraIndex = cameraIndex;
  row.assignedMask = assignedMask;
  row.matchedMask = matchedMask;
  for (size_t i = 0; i < row.blobIndices.size(); ++i) {
    row.blobIndices[i] = blobIndices[i];
  }

  std::lock_guard<std::mutex> lock(s.mutex);
  // Four rows per processed controller frame at ~60 Hz is small, but keep a bounded queue
  // so research logging can never grow without limit if disk stalls.
  constexpr size_t kMaxGroundTruthRowsQueued = 16384;
  if (s.ledGroundTruthQueue.size() >= kMaxGroundTruthRowsQueued) {
    return;
  }
  s.ledGroundTruthQueue.push_back(std::move(row));
  s.writerCv.notify_one();
}

void
SonyOpticalCapture::CaptureObservableUsbRead(uint8_t interfaceNumber,
                                             uint8_t pipeId,
                                             const void *data,
                                             size_t size)
{
  if (!Enabled() || data == nullptr || size == 0) {
    return;
  }

  // Camera: capture the completed raw IF6/0x87 USB packet itself. This is the clean-room source of VI frames.
  if (interfaceNumber == 6 && pipeId == 0x87) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    if (size >= 2 && bytes[0] == 'V' && bytes[1] == 'I') {
      capture_observable_camera_packet(data, size);
    }
    return;
  }

  // LED detector: preserve the raw externally observable IF8/0x89 payload for black-box offline analysis.
  if (interfaceNumber != 8 || pipeId != 0x89) {
    return;
  }

  initialize_if_needed();
  CaptureState &s = state();

  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.ledDetectorBytesQueued + size > kMaxLedDetectorBytes ||
      s.usbWriterQueue.size() >= kMaxQueuedUsbPackets) {
    ++s.ledDetectorPacketsDropped;
    return;
  }

  UsbDetectorPacket packet;
  packet.hostTimestampUs = host_timestamp_us();
  packet.interfaceNumber = interfaceNumber;
  packet.pipeId = pipeId;
  const auto *bytes = static_cast<const uint8_t *>(data);
  packet.bytes.assign(bytes, bytes + size);
  s.ledDetectorBytesQueued += size;
  s.usbWriterQueue.push_back(std::move(packet));
  s.writerCv.notify_one();
}

} // namespace psvr2_toolkit
