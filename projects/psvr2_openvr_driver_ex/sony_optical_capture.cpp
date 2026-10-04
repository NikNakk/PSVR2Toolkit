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
constexpr uint32_t kMaxCameraFrames = 160;
constexpr size_t kMaxQueuedUsbPackets = 512;
constexpr uint64_t kMaxLedDetectorBytes = 256ULL * 1024ULL * 1024ULL;

struct FrameSnapshot {
  std::vector<uint8_t> bytes;
  uint64_t hostTimestampUs = 0;
  uint32_t imageTimestamp = 0;
  uint16_t imageType = 0;
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
  std::ofstream ledDetector;

  std::mutex mutex;
  std::condition_variable writerCv;
  std::deque<QueuedFrame> writerQueue;
  std::deque<UsbDetectorPacket> usbWriterQueue;
  std::thread writerThread;
  bool stopWriter = false;

  std::atomic<uint64_t> eventId{0};

  std::array<FrameSnapshot, kPreFramesPerEvent> history;
  size_t historyCount = 0;
  size_t historyNext = 0;
  std::deque<ActiveEvent> activeEvents;

  uint32_t cameraFramesQueued = 0;
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
writer_main(CaptureState *s)
{
  for (;;) {
    QueuedFrame frameItem;
    UsbDetectorPacket usbItem;
    bool haveFrame = false;
    bool haveUsb = false;

    {
      std::unique_lock<std::mutex> lock(s->mutex);
      s->writerCv.wait(lock, [s]() {
        return s->stopWriter || !s->writerQueue.empty() || !s->usbWriterQueue.empty();
      });
      if (s->stopWriter && s->writerQueue.empty() && s->usbWriterQueue.empty()) {
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
                    << frameItem.frame.imageTimestamp << ',' << frameItem.frame.imageType << ','
                    << frameItem.frame.bytes.size() << ',' << frameItem.filename << "\n";
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
      s.ledDetector.open(s.directory / "usb-if8-led-detector.bin",
                         std::ios::out | std::ios::binary | std::ios::trunc);

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

      s.writerThread = std::thread(writer_main, &s);

      Util::DriverLog(
          "[Sony Optical Capture] directory={} camera-only clean-room capture: {} pre + {} post frames/event, "
          "maxCameraFrames={} (background writer); observable IF8/0x89 LED-detector USB capture capped at {} MiB",
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
           << (relativeFrame >= 0 ? relativeFrame : -relativeFrame) << "-ts-" << frame.imageTimestamp << ".vi11";

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
  CaptureState &s = state();
  const auto *bytes = static_cast<const uint8_t *>(imageData);

  std::lock_guard<std::mutex> lock(s.mutex);

  /*
   * Reuse the two ring-buffer allocations instead of allocating a new ~1 MiB vector at 60 Hz.
   */
  FrameSnapshot &current = s.history[s.historyNext];
  current.hostTimestampUs = host_timestamp_us();
  current.imageTimestamp = imageTimestamp;
  current.imageType = imageType;
  current.bytes.resize(imageSize);
  std::memcpy(current.bytes.data(), bytes, imageSize);

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

void
SonyOpticalCapture::CaptureObservableUsbRead(uint8_t interfaceNumber,
                                             uint8_t pipeId,
                                             const void *data,
                                             size_t size)
{
  if (!Enabled() || interfaceNumber != 8 || pipeId != 0x89 || data == nullptr || size == 0) {
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
