#pragma once

#include <openvr_driver.h>

#include <chrono>
#include <cstdint>
#include <mutex>

namespace psvr2_toolkit {

class DisplayRedirect final : public vr::ITrackedDeviceServerDriver, public vr::IVRVirtualDisplay {
public:
  static DisplayRedirect *Instance();

  bool Register();
  void Shutdown();

  // ITrackedDeviceServerDriver
  vr::EVRInitError Activate(uint32_t unObjectId) override;
  void Deactivate() override;
  void EnterStandby() override;
  void *GetComponent(const char *pchComponentNameAndVersion) override;
  void DebugRequest(const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize) override;
  vr::DriverPose_t GetPose() override;

  // IVRVirtualDisplay_002
  void Present(const vr::PresentInfo_t *pPresentInfo, uint32_t unPresentInfoSize) override;
  void WaitForPresent() override;
  bool GetTimeSinceLastVsync(float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter) override;

private:
  DisplayRedirect();

  uint64_t ResolvePrimaryAdapterLuid();
  void ResetTimingLocked(std::chrono::steady_clock::time_point now);

  static DisplayRedirect *m_pInstance;

  static constexpr double kRefreshHz = 90.0;
  static constexpr const char *kSerialNumber = "psvr2_toolkit_display_redirect";
  static constexpr const char *kModelNumber = "PSVR2 Toolkit Display Redirect";

  std::mutex m_timingMutex;
  std::chrono::steady_clock::time_point m_lastVsync;
  std::chrono::steady_clock::time_point m_nextVsync;

  vr::TrackedDeviceIndex_t m_objectId = vr::k_unTrackedDeviceIndexInvalid;
  uint64_t m_adapterLuid = 0;
  uint64_t m_vsyncCounter = 0;
  uint64_t m_presentCount = 0;
  uint64_t m_lastLoggedPresent = 0;
  bool m_registered = false;
  bool m_active = false;
};

} // namespace psvr2_toolkit
