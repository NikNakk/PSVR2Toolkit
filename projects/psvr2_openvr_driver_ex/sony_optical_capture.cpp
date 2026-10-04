#include "sony_optical_capture.h"

#include "sense_controller.h"
#include "util.h"
#include "vr_settings.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <windows.h>

namespace psvr2_toolkit {
namespace {

constexpr uint32_t kFramesPerEvent = 3;
constexpr uint32_t kMaxCameraFrames = 96;
constexpr uint64_t kMaxOpticalBytes = 256ULL * 1024ULL * 1024ULL;
constexpr size_t kControllerOpticalBytes = 0x5A44;
constexpr size_t kCameraStride = 0x1688;
constexpr size_t kLedMapOffset = 0x1438;
constexpr size_t kBlobBaseOffset = 0x40;
constexpr size_t kBlobStride = 0x14;
constexpr int kLedCount = 17;

#pragma pack(push, 1)
struct OpticalRecordHeader {
  uint32_t magic; // 'SOPT'
  uint16_t version;
  uint16_t controller;
  uint64_t hostTimestampUs;
  uint64_t frameIndex;
  uint32_t payloadSize;
  uint32_t reserved;
};
#pragma pack(pop)

struct CaptureState {
  std::once_flag initOnce;
  std::filesystem::path directory;
  std::ofstream events;
  std::ofstream frames;
  std::ofstream opticalSummary;
  std::ofstream opticalRaw[2];
  std::mutex mutex;
  std::atomic<uint64_t> eventId{0};
  std::atomic<uint64_t> currentEventId{0};
  std::atomic<uint32_t> framesRemaining{0};
  uint32_t cameraFramesWritten = 0;
  uint64_t opticalBytesWritten[2] = {0, 0};
};

CaptureState &
state()
{
  static CaptureState s;
  return s;
}

std::string
hex_bytes(const void *data, size_t size)
{
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
      s.opticalSummary.open(s.directory / "optical_summary.csv", std::ios::out | std::ios::trunc);
      s.opticalRaw[0].open(s.directory / "optical-L.bin", std::ios::out | std::ios::binary | std::ios::trunc);
      s.opticalRaw[1].open(s.directory / "optical-R.bin", std::ios::out | std::ios::binary | std::ios::trunc);

      if (s.events) {
        s.events << "host_us,event_id,kind,side,current_phase,current_seq,current_period,base_time,frame_cycle,payload_hex\n";
      }
      if (s.frames) {
        s.frames << "host_us,capture_index,event_id,image_timestamp,image_type,size,filename\n";
      }
      if (s.opticalSummary) {
        s.opticalSummary << "host_us,frame_index,side,camera,assigned_led_mask,matched_led_mask,matched_count\n";
      }

      Util::DriverLog("[Sony Optical Capture] directory={} maxCameraFrames={} maxOpticalMiBPerController={}",
                      s.directory.string(), kMaxCameraFrames, kMaxOpticalBytes / (1024 * 1024));
    } catch (const std::exception &e) {
      Util::DriverLog("[Sony Optical Capture] initialization failed: {}", e.what());
    }
  });
}

void
arm_camera_frames(uint64_t eventId)
{
  CaptureState &s = state();
  s.currentEventId.store(eventId);
  uint32_t existing = s.framesRemaining.load();
  while (existing < kFramesPerEvent &&
         !s.framesRemaining.compare_exchange_weak(existing, kFramesPerEvent)) {
  }
}

uint64_t
new_event_id()
{
  return state().eventId.fetch_add(1) + 1;
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
  initialize_if_needed();
  CaptureState &s = state();
  const uint64_t id = new_event_id();
  const uint64_t hostUs = GetHostTimestamp();

  // Command 6 is a ~1 Hz Sony maintenance command and would otherwise consume the bounded camera-frame budget
  // during long PRESCAN intervals. Keep every command in events.csv, but save images only around state/timing
  // mutations that can change what the cameras see.
  const auto *rawCommand = static_cast<const uint8_t *>(command);
  const uint8_t commandType = commandSize > 0 && rawCommand ? rawCommand[0] : 0xff;
  const bool visuallyInteresting =
      commandType == 1 || // SET_SYNC_PHASE
      commandType == 2 || // SET_LEDS_IMMEDIATE
      commandType == 3 || // ADJUST_FRAME_CYCLE
      commandType == 4 || // ADJUST_BASE_TIME
      commandType == 5;   // ADJUST_TIME_AND_CYCLE
  if (visuallyInteresting) {
    arm_camera_frames(id);
  }

  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.events) {
    s.events << hostUs << ',' << id << ",led," << (isLeft ? 'L' : 'R') << ','
             << static_cast<unsigned>(currentPhase) << ',' << static_cast<unsigned>(currentSequence) << ','
             << static_cast<unsigned>(currentPeriod) << ',' << baseTime << ',' << frameCycle << ",\""
             << hex_bytes(command, commandSize) << "\"\n";
    s.events.flush();
  }
}

void
SonyOpticalCapture::NoteTracking(bool isLeft, int oldFlag, int newFlag)
{
  if (!Enabled()) {
    return;
  }
  initialize_if_needed();
  CaptureState &s = state();
  const uint64_t id = new_event_id();
  const uint64_t hostUs = GetHostTimestamp();
  arm_camera_frames(id);

  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.events) {
    s.events << hostUs << ',' << id << ",tracking," << (isLeft ? 'L' : 'R')
             << ",,,,,,\"flag " << oldFlag << " -> " << newFlag << "\"\n";
    s.events.flush();
  }
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

  CaptureState &s = state();
  uint32_t remaining = s.framesRemaining.load();
  while (remaining > 0 && !s.framesRemaining.compare_exchange_weak(remaining, remaining - 1)) {
  }
  if (remaining == 0) {
    return;
  }

  initialize_if_needed();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.cameraFramesWritten >= kMaxCameraFrames || s.directory.empty()) {
    return;
  }

  const uint64_t eventId = s.currentEventId.load();
  const uint64_t hostUs = GetHostTimestamp();
  const uint32_t captureIndex = ++s.cameraFramesWritten;

  std::ostringstream filename;
  filename << "camera-" << std::setfill('0') << std::setw(4) << captureIndex << "-event-" << std::setw(4)
           << eventId << "-ts-" << imageTimestamp << ".vi11";

  const std::filesystem::path path = s.directory / filename.str();
  std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
  if (out) {
    out.write(static_cast<const char *>(imageData), static_cast<std::streamsize>(imageSize));
    out.close();
    if (s.frames) {
      s.frames << hostUs << ',' << captureIndex << ',' << eventId << ',' << imageTimestamp << ',' << imageType
               << ',' << imageSize << ',' << filename.str() << "\n";
      s.frames.flush();
    }
  }
}

void
SonyOpticalCapture::CaptureOpticalData(uint32_t controllerIdx,
                                       uint64_t frameIndex,
                                       const void *controllerData,
                                       size_t controllerDataSize)
{
  if (!Enabled() || controllerIdx >= 2 || controllerData == nullptr ||
      controllerDataSize < kControllerOpticalBytes) {
    return;
  }

  initialize_if_needed();
  CaptureState &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);

  const uint64_t recordBytes = sizeof(OpticalRecordHeader) + kControllerOpticalBytes;
  if (s.opticalBytesWritten[controllerIdx] + recordBytes > kMaxOpticalBytes) {
    return;
  }

  const uint64_t hostUs = GetHostTimestamp();
  OpticalRecordHeader header = {};
  header.magic = 0x54504f53; // SOPT on little-endian Windows
  header.version = 1;
  header.controller = static_cast<uint16_t>(controllerIdx);
  header.hostTimestampUs = hostUs;
  header.frameIndex = frameIndex;
  header.payloadSize = static_cast<uint32_t>(kControllerOpticalBytes);

  if (s.opticalRaw[controllerIdx]) {
    s.opticalRaw[controllerIdx].write(reinterpret_cast<const char *>(&header), sizeof(header));
    s.opticalRaw[controllerIdx].write(static_cast<const char *>(controllerData),
                                      static_cast<std::streamsize>(kControllerOpticalBytes));
    s.opticalBytesWritten[controllerIdx] += recordBytes;
  }

  if (!s.opticalSummary) {
    return;
  }

  const auto *bytes = static_cast<const uint8_t *>(controllerData);
  for (int cam = 0; cam < 4; ++cam) {
    const uint8_t *camData = bytes + cam * kCameraStride;
    uint32_t assignedMask = 0;
    uint32_t matchedMask = 0;
    uint32_t matchedCount = 0;

    for (int ledId = 0; ledId < kLedCount; ++ledId) {
      int16_t blobIndex = 0;
      std::memcpy(&blobIndex, camData + kLedMapOffset + ledId * sizeof(int16_t), sizeof(blobIndex));
      if (blobIndex < 0) {
        continue;
      }
      assignedMask |= (1u << ledId);

      const size_t blobOffset = kBlobBaseOffset + static_cast<size_t>(blobIndex) * kBlobStride;
      if (blobOffset + 10 > kLedMapOffset) {
        continue;
      }

      int16_t isMatched = 0;
      std::memcpy(&isMatched, camData + blobOffset + 8, sizeof(isMatched));
      if (isMatched == 1) {
        matchedMask |= (1u << ledId);
        ++matchedCount;
      }
    }

    s.opticalSummary << hostUs << ',' << frameIndex << ',' << (controllerIdx == 0 ? 'L' : 'R') << ',' << cam
                     << ",0x" << std::hex << assignedMask << ",0x" << matchedMask << std::dec << ',' << matchedCount
                     << "\n";
  }
}

} // namespace psvr2_toolkit
