#include "driver_input_proxy.h"

#include "driver_host_proxy.h"
#include "hmd_types.h"
#include "util.h"
#include "vr_settings.h"

#include <cstring>

namespace psvr2_toolkit {

DriverInputProxy *DriverInputProxy::m_pInstance = nullptr;

DriverInputProxy::DriverInputProxy()
    : m_pDriverInput(nullptr), m_hmdProximityHandle(vr::k_ulInvalidInputComponentHandle), m_loggedForcedUpdate(false) {}

DriverInputProxy *DriverInputProxy::Instance() {
  if (!m_pInstance) {
    m_pInstance = new DriverInputProxy();
  }
  return m_pInstance;
}

void DriverInputProxy::SetDriverInput(vr::IVRDriverInput *pDriverInput) { m_pDriverInput = pDriverInput; }

vr::EVRInputError DriverInputProxy::CreateBooleanComponent(vr::PropertyContainerHandle_t ulContainer, const char *pchName,
                                                           vr::VRInputComponentHandle_t *pHandle) {
  const auto result = m_pDriverInput->CreateBooleanComponent(ulContainer, pchName, pHandle);

  const bool looksLikeProximity = pchName && std::strstr(pchName, "proximity") != nullptr;
  const bool forceProximity = VRSettings::GetBool(STEAMVR_SETTINGS_FORCE_HMD_PROXIMITY, SETTING_FORCE_HMD_PROXIMITY_DEFAULT_VALUE);

  if (result == vr::VRInputError_None && pHandle && looksLikeProximity) {
    m_hmdProximityHandle = *pHandle;
    Util::DriverLog("[HMD Proximity] Detected Sony HMD boolean component '{}' handle={} container={}.", pchName,
                    static_cast<uint64_t>(m_hmdProximityHandle), static_cast<uint64_t>(ulContainer));

    if (forceProximity) {
      const auto forceResult = m_pDriverInput->UpdateBooleanComponent(m_hmdProximityHandle, true, 0.0);
      Util::DriverLog("[HMD Proximity] Forced headset worn immediately; UpdateBooleanComponent result={}.",
                      static_cast<int>(forceResult));
    }
  }

  return result;
}

vr::EVRInputError DriverInputProxy::UpdateBooleanComponent(vr::VRInputComponentHandle_t ulComponent, bool bNewValue,
                                                           double fTimeOffset) {
  if (ulComponent == m_hmdProximityHandle &&
      VRSettings::GetBool(STEAMVR_SETTINGS_FORCE_HMD_PROXIMITY, SETTING_FORCE_HMD_PROXIMITY_DEFAULT_VALUE)) {
    if (!m_loggedForcedUpdate || !bNewValue) {
      Util::DriverLog("[HMD Proximity] Sony update requested value={}; forwarding value=true (timeOffset={:.6f}).",
                      bNewValue ? 1 : 0, fTimeOffset);
      m_loggedForcedUpdate = true;
    }
    return m_pDriverInput->UpdateBooleanComponent(ulComponent, true, fTimeOffset);
  }

  return m_pDriverInput->UpdateBooleanComponent(ulComponent, bNewValue, fTimeOffset);
}

vr::EVRInputError DriverInputProxy::CreateScalarComponent(vr::PropertyContainerHandle_t ulContainer, const char *pchName,
                                                          vr::VRInputComponentHandle_t *pHandle, vr::EVRScalarType eType,
                                                          vr::EVRScalarUnits eUnits) {
  return m_pDriverInput->CreateScalarComponent(ulContainer, pchName, pHandle, eType, eUnits);
}

vr::EVRInputError DriverInputProxy::UpdateScalarComponent(vr::VRInputComponentHandle_t ulComponent, float fNewValue,
                                                          double fTimeOffset) {
  return m_pDriverInput->UpdateScalarComponent(ulComponent, fNewValue, fTimeOffset);
}

vr::EVRInputError DriverInputProxy::CreateHapticComponent(vr::PropertyContainerHandle_t ulContainer, const char *pchName,
                                                          vr::VRInputComponentHandle_t *pHandle) {
  return m_pDriverInput->CreateHapticComponent(ulContainer, pchName, pHandle);
}

vr::EVRInputError DriverInputProxy::CreateSkeletonComponent(vr::PropertyContainerHandle_t ulContainer, const char *pchName,
                                                            const char *pchSkeletonPath, const char *pchBasePosePath,
                                                            vr::EVRSkeletalTrackingLevel eSkeletalTrackingLevel,
                                                            const vr::VRBoneTransform_t *pGripLimitTransforms,
                                                            uint32_t unGripLimitTransformCount,
                                                            vr::VRInputComponentHandle_t *pHandle) {
  return m_pDriverInput->CreateSkeletonComponent(ulContainer, pchName, pchSkeletonPath, pchBasePosePath,
                                                 eSkeletalTrackingLevel, pGripLimitTransforms,
                                                 unGripLimitTransformCount, pHandle);
}

vr::EVRInputError DriverInputProxy::UpdateSkeletonComponent(vr::VRInputComponentHandle_t ulComponent,
                                                            vr::EVRSkeletalMotionRange eMotionRange,
                                                            const vr::VRBoneTransform_t *pTransforms,
                                                            uint32_t unTransformCount) {
  return m_pDriverInput->UpdateSkeletonComponent(ulComponent, eMotionRange, pTransforms, unTransformCount);
}

vr::EVRInputError DriverInputProxy::CreatePoseComponent(vr::PropertyContainerHandle_t ulContainer, const char *pchName,
                                                        vr::VRInputComponentHandle_t *pHandle) {
  return m_pDriverInput->CreatePoseComponent(ulContainer, pchName, pHandle);
}

vr::EVRInputError DriverInputProxy::UpdatePoseComponent(vr::VRInputComponentHandle_t ulComponent,
                                                        const vr::HmdMatrix34_t *pMatPoseOffset, double fTimeOffset) {
  return m_pDriverInput->UpdatePoseComponent(ulComponent, pMatPoseOffset, fTimeOffset);
}

vr::EVRInputError DriverInputProxy::CreateEyeTrackingComponent(vr::PropertyContainerHandle_t ulContainer,
                                                               const char *pchName,
                                                               vr::VRInputComponentHandle_t *pHandle) {
  return m_pDriverInput->CreateEyeTrackingComponent(ulContainer, pchName, pHandle);
}

vr::EVRInputError DriverInputProxy::UpdateEyeTrackingComponent(vr::VRInputComponentHandle_t ulComponent,
                                                               const vr::VREyeTrackingData_t *pEyeTrackingData,
                                                               double fTimeOffset) {
  return m_pDriverInput->UpdateEyeTrackingComponent(ulComponent, pEyeTrackingData, fTimeOffset);
}

} // namespace psvr2_toolkit
