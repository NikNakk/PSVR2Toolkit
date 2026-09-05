#include "driver_host_proxy.h"

#include "hmd_math.h"
#include "hmd_types.h"
#include "util.h"
#include "vr_settings.h"

#include <windows.h>

#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace psvr2_toolkit {

namespace {

class DiagnosticTrace {
public:
  static DiagnosticTrace &Instance() {
    static DiagnosticTrace trace;
    return trace;
  }

  void WriteProxyInitialised() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_events.is_open()) {
      return;
    }
    m_events << NowNs() << ",proxy_initialised,0,driver_host_attached\n";
    m_events.flush();
  }

  void WritePose(uint32_t deviceIndex, uint32_t poseStructSize, const vr::DriverPose_t &pose) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_pose.is_open()) {
      return;
    }

    m_pose << NowNs() << ',' << deviceIndex << ',' << poseStructSize << ',' << pose.poseTimeOffset << ','
           << pose.qWorldFromDriverRotation.w << ',' << pose.qWorldFromDriverRotation.x << ','
           << pose.qWorldFromDriverRotation.y << ',' << pose.qWorldFromDriverRotation.z << ','
           << pose.vecWorldFromDriverTranslation[0] << ',' << pose.vecWorldFromDriverTranslation[1] << ','
           << pose.vecWorldFromDriverTranslation[2] << ',' << pose.qDriverFromHeadRotation.w << ','
           << pose.qDriverFromHeadRotation.x << ',' << pose.qDriverFromHeadRotation.y << ','
           << pose.qDriverFromHeadRotation.z << ',' << pose.vecDriverFromHeadTranslation[0] << ','
           << pose.vecDriverFromHeadTranslation[1] << ',' << pose.vecDriverFromHeadTranslation[2] << ','
           << pose.vecPosition[0] << ',' << pose.vecPosition[1] << ',' << pose.vecPosition[2] << ','
           << pose.qRotation.w << ',' << pose.qRotation.x << ',' << pose.qRotation.y << ',' << pose.qRotation.z << ','
           << pose.vecVelocity[0] << ',' << pose.vecVelocity[1] << ',' << pose.vecVelocity[2] << ','
           << pose.vecAngularVelocity[0] << ',' << pose.vecAngularVelocity[1] << ',' << pose.vecAngularVelocity[2] << ','
           << static_cast<int>(pose.result) << ',' << (pose.poseIsValid ? 1 : 0) << ',' << (pose.willDriftInYaw ? 1 : 0)
           << ',' << (pose.shouldApplyHeadModel ? 1 : 0) << ',' << (pose.deviceIsConnected ? 1 : 0) << '\n';

    if (++m_poseRowsSinceFlush >= 256) {
      m_pose.flush();
      m_poseRowsSinceFlush = 0;
    }
  }

  void WriteVsync(double vsyncTimeOffsetSeconds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vsync.is_open()) {
      return;
    }

    m_vsync << NowNs() << ',' << vsyncTimeOffsetSeconds << '\n';
    if (++m_vsyncRowsSinceFlush >= 120) {
      m_vsync.flush();
      m_vsyncRowsSinceFlush = 0;
    }
  }

  void WriteHmdProperties(bool reportsTimeSinceVsync,
                          vr::ETrackedPropertyError reportsError,
                          float secondsFromVsyncToPhotons,
                          vr::ETrackedPropertyError photonsError,
                          float displayFrequency,
                          vr::ETrackedPropertyError frequencyError) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_properties.is_open()) {
      return;
    }

    m_properties << NowNs() << ',' << (reportsTimeSinceVsync ? 1 : 0) << ',' << static_cast<int>(reportsError) << ','
                 << secondsFromVsyncToPhotons << ',' << static_cast<int>(photonsError) << ',' << displayFrequency << ','
                 << static_cast<int>(frequencyError) << '\n';
    m_properties.flush();
  }

  void WriteTrackedDeviceAdded(const char *serial, vr::ETrackedDeviceClass deviceClass) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_events.is_open()) {
      return;
    }

    m_events << NowNs() << ",tracked_device_added," << static_cast<int>(deviceClass) << ',';
    if (serial != nullptr) {
      for (const char *p = serial; *p != '\0'; ++p) {
        // Keep this very small logger CSV-safe without pulling in an escaping library.
        m_events << (*p == ',' ? ';' : *p);
      }
    }
    m_events << '\n';
    m_events.flush();
  }

private:
  DiagnosticTrace() {
    QueryPerformanceFrequency(&m_frequency);

    std::string directory;
    if (const char *configured = std::getenv("PSVR2TOOLKIT_TRACE_DIR"); configured != nullptr && configured[0] != '\0') {
      directory = configured;
    } else {
      char tempPath[MAX_PATH]{};
      DWORD length = GetTempPathA(MAX_PATH, tempPath);
      if (length > 0 && length < MAX_PATH) {
        directory.assign(tempPath, length);
      } else {
        directory = ".";
      }
    }

    if (!directory.empty() && directory.back() != '\\' && directory.back() != '/') {
      directory.push_back('\\');
    }

    std::ostringstream prefix;
    prefix << directory << "psvr2toolkit_" << GetCurrentProcessId();
    m_prefix = prefix.str();

    m_pose.open(m_prefix + "_pose.csv", std::ios::out | std::ios::trunc);
    m_vsync.open(m_prefix + "_vsync.csv", std::ios::out | std::ios::trunc);
    m_properties.open(m_prefix + "_properties.csv", std::ios::out | std::ios::trunc);
    m_events.open(m_prefix + "_events.csv", std::ios::out | std::ios::trunc);

    m_pose << std::setprecision(17);
    m_vsync << std::setprecision(17);
    m_properties << std::setprecision(17);

    if (m_pose.is_open()) {
      m_pose << "host_qpc_ns,device_index,pose_struct_size,pose_time_offset_s,world_qw,world_qx,world_qy,world_qz,"
                "world_tx,world_ty,world_tz,driver_head_qw,driver_head_qx,driver_head_qy,driver_head_qz,driver_head_tx,"
                "driver_head_ty,driver_head_tz,pos_x,pos_y,pos_z,rot_w,rot_x,rot_y,rot_z,vel_x,vel_y,vel_z,angvel_x,"
                "angvel_y,angvel_z,tracking_result,pose_valid,will_drift_yaw,apply_head_model,connected\n";
      m_pose.flush();
    }
    if (m_vsync.is_open()) {
      m_vsync << "host_qpc_ns,vsync_time_offset_s\n";
      m_vsync.flush();
    }
    if (m_properties.is_open()) {
      m_properties << "host_qpc_ns,reports_time_since_vsync,reports_error,seconds_from_vsync_to_photons_s,photons_error,"
                      "display_frequency_hz,frequency_error\n";
      m_properties.flush();
    }
    if (m_events.is_open()) {
      m_events << "host_qpc_ns,event,value,text\n";
      m_events.flush();
    }

    std::string message = "PSVR2Toolkit diagnostic trace prefix: " + m_prefix + "\n";
    OutputDebugStringA(message.c_str());
  }

  uint64_t NowNs() const {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);

    const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
    const uint64_t frequency = static_cast<uint64_t>(m_frequency.QuadPart);
    return (ticks / frequency) * 1000000000ULL + ((ticks % frequency) * 1000000000ULL) / frequency;
  }

  LARGE_INTEGER m_frequency{};
  std::mutex m_mutex;
  std::string m_prefix;
  std::ofstream m_pose;
  std::ofstream m_vsync;
  std::ofstream m_properties;
  std::ofstream m_events;
  uint32_t m_poseRowsSinceFlush = 0;
  uint32_t m_vsyncRowsSinceFlush = 0;
};

} // namespace

DriverHostProxy *DriverHostProxy::m_pInstance = nullptr;

DriverHostProxy::DriverHostProxy() : m_pDriverHost(nullptr), m_pfnEventHandlers() {}

DriverHostProxy *DriverHostProxy::Instance() {
  if (!m_pInstance) {
    m_pInstance = new DriverHostProxy;
  }

  return m_pInstance;
}

void DriverHostProxy::SetDriverHost(vr::IVRServerDriverHost *pDriverHost) {
  m_pDriverHost = pDriverHost;
  DiagnosticTrace::Instance().WriteProxyInitialised();
}

void DriverHostProxy::AddEventHandler(void (*pfnEventHandler)(vr::VREvent_t *)) { m_pfnEventHandlers.push_back(pfnEventHandler); }

void DriverHostProxy::LogHmdTimingProperties() {
  if (hmdContainer == vr::k_ulInvalidPropertyContainer || vr::VRProperties() == nullptr) {
    return;
  }

  vr::ETrackedPropertyError reportsError = vr::TrackedProp_Success;
  vr::ETrackedPropertyError photonsError = vr::TrackedProp_Success;
  vr::ETrackedPropertyError frequencyError = vr::TrackedProp_Success;

  bool reportsTimeSinceVsync = vr::VRProperties()->GetBoolProperty(hmdContainer, vr::Prop_ReportsTimeSinceVSync_Bool, &reportsError);
  float secondsFromVsyncToPhotons =
      vr::VRProperties()->GetFloatProperty(hmdContainer, vr::Prop_SecondsFromVsyncToPhotons_Float, &photonsError);
  float displayFrequency = vr::VRProperties()->GetFloatProperty(hmdContainer, vr::Prop_DisplayFrequency_Float, &frequencyError);

  DiagnosticTrace::Instance().WriteHmdProperties(reportsTimeSinceVsync, reportsError, secondsFromVsyncToPhotons, photonsError,
                                                 displayFrequency, frequencyError);
}

bool DriverHostProxy::TrackedDeviceAdded(const char *pchDeviceSerialNumber, vr::ETrackedDeviceClass eDeviceClass, vr::ITrackedDeviceServerDriver *pDriver) {
  DiagnosticTrace::Instance().WriteTrackedDeviceAdded(pchDeviceSerialNumber, eDeviceClass);

  if (Util::StartsWith(pchDeviceSerialNumber, "playstation_vr2_sense_controller_") &&
      VRSettings::GetBool(STEAMVR_SETTINGS_DISABLE_SENSE, SETTING_DISABLE_SENSE_DEFAULT_VALUE)) {
    return false;
  }

  bool success = m_pDriverHost->TrackedDeviceAdded(pchDeviceSerialNumber, eDeviceClass, pDriver);

  return success;
}

void DriverHostProxy::TrackedDevicePoseUpdated(uint32_t unWhichDevice, const vr::DriverPose_t &newPose, uint32_t unPoseStructSize) {
  DeviceType deviceType = GetDeviceType(unWhichDevice);

  if (deviceType == DeviceType::HMD || unWhichDevice == vr::k_unTrackedDeviceIndex_Hmd) {
    DiagnosticTrace::Instance().WritePose(unWhichDevice, unPoseStructSize, newPose);
  }

  if (deviceType != DeviceType::SenseControllerLeft && deviceType != DeviceType::SenseControllerRight) {
    return m_pDriverHost->TrackedDevicePoseUpdated(unWhichDevice, newPose, unPoseStructSize);
  }

  return m_pDriverHost->TrackedDevicePoseUpdated(unWhichDevice, GetPose(deviceType, newPose), unPoseStructSize);
}

void DriverHostProxy::VsyncEvent(double vsyncTimeOffsetSeconds) {
  DiagnosticTrace::Instance().WriteVsync(vsyncTimeOffsetSeconds);
  m_pDriverHost->VsyncEvent(vsyncTimeOffsetSeconds);
}

void DriverHostProxy::VendorSpecificEvent(uint32_t unWhichDevice, vr::EVREventType eventType, const vr::VREvent_Data_t &eventData, double eventTimeOffset) {
  m_pDriverHost->VendorSpecificEvent(unWhichDevice, eventType, eventData, eventTimeOffset);
}

bool DriverHostProxy::IsExiting() { return m_pDriverHost->IsExiting(); }

bool DriverHostProxy::PollNextEvent(vr::VREvent_t *pEvent, uint32_t uncbVREvent) {
  if (m_pDriverHost->PollNextEvent(pEvent, uncbVREvent)) {
    for (auto &m_pfnEventHandler : m_pfnEventHandlers) {
      m_pfnEventHandler(pEvent);
    }
    return true;
  }
  return false;
}

void DriverHostProxy::GetRawTrackedDevicePoses(float fPredictedSecondsFromNow, vr::TrackedDevicePose_t *pTrackedDevicePoseArray,
                                               uint32_t unTrackedDevicePoseArrayCount) {
  m_pDriverHost->GetRawTrackedDevicePoses(fPredictedSecondsFromNow, pTrackedDevicePoseArray, unTrackedDevicePoseArrayCount);
}

void DriverHostProxy::RequestRestart(const char *pchLocalizedReason, const char *pchExecutableToStart, const char *pchArguments,
                                     const char *pchWorkingDirectory) {
  m_pDriverHost->RequestRestart(pchLocalizedReason, pchExecutableToStart, pchArguments, pchWorkingDirectory);
}

uint32_t DriverHostProxy::GetFrameTimings(vr::Compositor_FrameTiming *pTiming, uint32_t nFrames) { return m_pDriverHost->GetFrameTimings(pTiming, nFrames); }

void DriverHostProxy::SetDisplayEyeToHead(uint32_t unWhichDevice, const vr::HmdMatrix34_t &eyeToHeadLeft, const vr::HmdMatrix34_t &eyeToHeadRight) {
  m_pDriverHost->SetDisplayEyeToHead(unWhichDevice, eyeToHeadLeft, eyeToHeadRight);
}

void DriverHostProxy::SetDisplayProjectionRaw(uint32_t unWhichDevice, const vr::HmdRect2_t &eyeLeft, const vr::HmdRect2_t &eyeRight) {
  m_pDriverHost->SetDisplayProjectionRaw(unWhichDevice, eyeLeft, eyeRight);
}

void DriverHostProxy::SetRecommendedRenderTargetSize(uint32_t unWhichDevice, uint32_t nWidth, uint32_t nHeight) {
  m_pDriverHost->SetRecommendedRenderTargetSize(unWhichDevice, nWidth, nHeight);
}

vr::DriverPose_t DriverHostProxy::GetPose(DeviceType type, const vr::DriverPose_t &originalPose) {
  static vr::HmdQuaternion_t imuRotationOffset = HmdMath::EulerToQuaternion(0, 0, 0.680678427219391);
  static vr::HmdQuaternion_t imuRotationOffsetInverse = HmdMath::QuaternionInverse(imuRotationOffset);

  // Whether this is the left controller or not.
  bool isLeft = (type == DeviceType::SenseControllerLeft);

  // Our new pose is a copy of the original pose.
  vr::DriverPose_t newPose = originalPose;

  // Apply inverse of imuRotationOffset to qRotation.
  newPose.qRotation = HmdMath::QuaternionMultiply(newPose.qRotation, imuRotationOffsetInverse);

  // PS VR2 driver pose offset.
  vr::HmdVector3d_t poseOffset = {isLeft ? 0.03439270332455635 : -0.03439270332455635, 0.05370872840285301, -0.09804324805736542};

  // Rotate the offset by the new rotation.
  vr::HmdVector3d_t rotationOffset = HmdMath::RotateVectorByQuaternion(poseOffset, newPose.qRotation);

  // Adjust position (negate the offset from the driver).
  newPose.vecPosition[0] -= rotationOffset.v[0];
  newPose.vecPosition[1] -= rotationOffset.v[1];
  newPose.vecPosition[2] -= rotationOffset.v[2];

  // Offset from the driver's root to the IMU. Given by the PS VR2 driver.
  // We'll also have to factor it to make the result pose identical to the one from the driver.
  vr::HmdVector3d_t imuOffset = {isLeft ? -0.00937270000576973 : 0.020072702318429947, 0.012248100712895393, 0.006003900431096554};

  // Rotate IMU offset to counteract the rotation we did on qRotation. See next comment.
  imuOffset = HmdMath::RotateVectorByQuaternion(imuOffset, imuRotationOffset);

  poseOffset.v[0] += imuOffset.v[0];
  poseOffset.v[1] += imuOffset.v[1];
  poseOffset.v[2] += imuOffset.v[2];

  newPose.vecDriverFromHeadTranslation[0] = poseOffset.v[0];
  newPose.vecDriverFromHeadTranslation[1] = poseOffset.v[1];
  newPose.vecDriverFromHeadTranslation[2] = poseOffset.v[2];

  // Since qRotation was rotated by the inverse of imuRotationOffset, we'll have to counteract it.
  newPose.qDriverFromHeadRotation = imuRotationOffset;

  return newPose;
}

} // namespace psvr2_toolkit
