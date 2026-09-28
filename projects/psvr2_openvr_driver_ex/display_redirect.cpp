#include "display_redirect.h"

#include "util.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace psvr2_toolkit {

DisplayRedirect *DisplayRedirect::m_pInstance = nullptr;

DisplayRedirect *DisplayRedirect::Instance() {
  if (!m_pInstance) {
    m_pInstance = new DisplayRedirect();
  }
  return m_pInstance;
}

DisplayRedirect::DisplayRedirect() : m_adapterLuid(ResolvePrimaryAdapterLuid()) {}

uint64_t DisplayRedirect::ResolvePrimaryAdapterLuid() {
  IDXGIFactory1 *factory = nullptr;
  HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
  if (FAILED(hr) || !factory) {
    Util::DriverLog("[DisplayRedirect] CreateDXGIFactory1 failed: hr=0x{:08x}", static_cast<uint32_t>(hr));
    return 0;
  }

  IDXGIAdapter1 *adapter = nullptr;
  hr = factory->EnumAdapters1(0, &adapter);
  if (FAILED(hr) || !adapter) {
    Util::DriverLog("[DisplayRedirect] EnumAdapters1(0) failed: hr=0x{:08x}", static_cast<uint32_t>(hr));
    factory->Release();
    return 0;
  }

  DXGI_ADAPTER_DESC1 desc{};
  hr = adapter->GetDesc1(&desc);

  uint64_t luid = 0;
  if (SUCCEEDED(hr)) {
    luid = static_cast<uint64_t>(desc.AdapterLuid.LowPart) |
           (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32);
    Util::DriverLog("[DisplayRedirect] Using DXGI adapter 0 LUID=0x{:016x}", luid);
  } else {
    Util::DriverLog("[DisplayRedirect] IDXGIAdapter1::GetDesc1 failed: hr=0x{:08x}", static_cast<uint32_t>(hr));
  }

  adapter->Release();
  factory->Release();
  return luid;
}

bool DisplayRedirect::Register() {
  if (m_registered) {
    return true;
  }

  if (m_adapterLuid == 0) {
    m_adapterLuid = ResolvePrimaryAdapterLuid();
  }

  Util::DriverLog("[DisplayRedirect] Registering {} at {:.1f} Hz, adapter LUID=0x{:016x}", kSerialNumber, kRefreshHz, m_adapterLuid);

  const bool success =
      vr::VRServerDriverHost()->TrackedDeviceAdded(kSerialNumber, vr::TrackedDeviceClass_DisplayRedirect, this);
  if (!success) {
    Util::DriverLog("[DisplayRedirect] TrackedDeviceAdded failed.");
    return false;
  }

  m_registered = true;
  return true;
}

void DisplayRedirect::Shutdown() {
  std::lock_guard<std::mutex> lock(m_timingMutex);
  m_active = false;
  m_registered = false;
  m_objectId = vr::k_unTrackedDeviceIndexInvalid;
  m_vsyncCounter = 0;
  m_presentCount = 0;
  m_waitForPresentCount = 0;
  m_vsyncQueryCount = 0;
  m_lastLoggedPresent = 0;
}

void DisplayRedirect::ResetTimingLocked(std::chrono::steady_clock::time_point now) {
  using duration = std::chrono::steady_clock::duration;
  const auto framePeriod = std::chrono::duration_cast<duration>(std::chrono::duration<double>(1.0 / kRefreshHz));
  m_lastVsync = now;
  m_nextVsync = now + framePeriod;
  m_vsyncCounter = 0;
}

vr::EVRInitError DisplayRedirect::Activate(uint32_t unObjectId) {
  m_objectId = unObjectId;

  const vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(unObjectId);
  vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String, kModelNumber);
  vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, "PSVR2 Toolkit");
  vr::VRProperties()->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.0f);
  vr::VRProperties()->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, static_cast<float>(kRefreshHz));
  vr::VRProperties()->SetUint64Property(container, vr::Prop_GraphicsAdapterLuid_Uint64, m_adapterLuid);

  {
    std::lock_guard<std::mutex> lock(m_timingMutex);
    m_active = true;
    m_presentCount = 0;
    m_waitForPresentCount = 0;
    m_vsyncQueryCount = 0;
    m_lastLoggedPresent = 0;
    ResetTimingLocked(std::chrono::steady_clock::now());
  }

  Util::DriverLog("[DisplayRedirect] Activated object={} at {:.1f} Hz.", unObjectId, kRefreshHz);
  return vr::VRInitError_None;
}

void DisplayRedirect::Deactivate() {
  std::lock_guard<std::mutex> lock(m_timingMutex);
  Util::DriverLog("[DisplayRedirect] Deactivated after {} Present calls, {} WaitForPresent calls, {} vsync queries.",
                  m_presentCount, m_waitForPresentCount, m_vsyncQueryCount);
  m_active = false;
  m_objectId = vr::k_unTrackedDeviceIndexInvalid;
}

void DisplayRedirect::EnterStandby() {}

void *DisplayRedirect::GetComponent(const char *pchComponentNameAndVersion) {
  if (pchComponentNameAndVersion && _stricmp(pchComponentNameAndVersion, vr::IVRVirtualDisplay_Version) == 0) {
    Util::DriverLog("[DisplayRedirect] IVRVirtualDisplay requested ({})", vr::IVRVirtualDisplay_Version);
    return static_cast<vr::IVRVirtualDisplay *>(this);
  }
  return nullptr;
}

void DisplayRedirect::DebugRequest(const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize) {
  (void)pchRequest;
  if (pchResponseBuffer && unResponseBufferSize > 0) {
    pchResponseBuffer[0] = '\0';
  }
}

vr::DriverPose_t DisplayRedirect::GetPose() {
  vr::DriverPose_t pose{};
  pose.poseIsValid = true;
  pose.deviceIsConnected = true;
  pose.result = vr::TrackingResult_Running_OK;
  pose.qWorldFromDriverRotation.w = 1.0;
  pose.qDriverFromHeadRotation.w = 1.0;
  pose.qRotation.w = 1.0;
  return pose;
}

void DisplayRedirect::Present(const vr::PresentInfo_t *pPresentInfo, uint32_t unPresentInfoSize) {
  if (!pPresentInfo || unPresentInfoSize < sizeof(vr::PresentInfo_t)) {
    Util::DriverLog("[DisplayRedirect] Present called with invalid PresentInfo size {} (expected >= {}).", unPresentInfoSize,
                    sizeof(vr::PresentInfo_t));
    return;
  }

  uint64_t presentCount = 0;
  bool shouldLog = false;
  {
    std::lock_guard<std::mutex> lock(m_timingMutex);
    ++m_presentCount;
    presentCount = m_presentCount;
    if (m_presentCount == 1 || m_presentCount - m_lastLoggedPresent >= 90) {
      m_lastLoggedPresent = m_presentCount;
      shouldLog = true;
    }
  }

  // Deliberately do not open or touch the shared texture in the first experiment.
  // We only need to prove that SteamVR can keep the compositor alive with a
  // virtual target. If we later consume the pixels, access must obey the keyed
  // mutex synchronization rules documented by Valve.
  if (shouldLog) {
    const uint64_t handleValue = static_cast<uint64_t>(pPresentInfo->backbufferTextureHandle);
    Util::DriverLog(
        "[DisplayRedirect] Present #{} frame={} handle=0x{:x} vsync={} compositorVsyncTime={:.6f}", presentCount,
        pPresentInfo->nFrameId, handleValue, static_cast<int>(pPresentInfo->vsync), pPresentInfo->flVSyncTimeInSeconds);
  }
}

void DisplayRedirect::WaitForPresent() {
  using clock = std::chrono::steady_clock;
  using duration = clock::duration;

  const auto framePeriod = std::chrono::duration_cast<duration>(std::chrono::duration<double>(1.0 / kRefreshHz));

  clock::time_point target;
  {
    std::lock_guard<std::mutex> lock(m_timingMutex);
    if (!m_active) {
      return;
    }
    ++m_waitForPresentCount;
    if (m_waitForPresentCount <= 5 || (m_waitForPresentCount % 90) == 0) {
      Util::DriverLog("[DisplayRedirect] WaitForPresent #{}", m_waitForPresentCount);
    }
    target = m_nextVsync;
  }

  const auto nowBeforeSleep = clock::now();
  if (nowBeforeSleep < target) {
    std::this_thread::sleep_until(target);
  }

  const auto now = clock::now();
  std::lock_guard<std::mutex> lock(m_timingMutex);
  if (!m_active) {
    return;
  }

  // Advance to the most recent synthetic-vsync boundary. If the compositor was
  // late, advance the frame counter by the skipped intervals as well.
  uint64_t intervals = 1;
  if (now > m_nextVsync + framePeriod) {
    const auto late = now - m_nextVsync;
    intervals += static_cast<uint64_t>(late / framePeriod);
  }

  m_lastVsync = m_nextVsync + framePeriod * static_cast<duration::rep>(intervals - 1);
  m_nextVsync = m_lastVsync + framePeriod;
  m_vsyncCounter += intervals;
}

bool DisplayRedirect::GetTimeSinceLastVsync(float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter) {
  if (!pfSecondsSinceLastVsync || !pulFrameCounter) {
    return false;
  }

  std::lock_guard<std::mutex> lock(m_timingMutex);
  if (!m_active) {
    return false;
  }

  ++m_vsyncQueryCount;
  if (m_vsyncQueryCount <= 5 || (m_vsyncQueryCount % 90) == 0) {
    Util::DriverLog("[DisplayRedirect] GetTimeSinceLastVsync #{}", m_vsyncQueryCount);
  }

  const auto now = std::chrono::steady_clock::now();
  const double seconds = std::max(0.0, std::chrono::duration<double>(now - m_lastVsync).count());
  *pfSecondsSinceLastVsync = static_cast<float>(seconds);
  *pulFrameCounter = m_vsyncCounter;
  return true;
}

} // namespace psvr2_toolkit
