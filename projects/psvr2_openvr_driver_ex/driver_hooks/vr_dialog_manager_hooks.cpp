#include "vr_dialog_manager_hooks.h"

#include "hmd_driver_loader.h"
#include "hook_lib.h"
#include "util.h"

#include <openvr_driver.h>

namespace psvr2_toolkit {

struct CachedChaperoneSettings {
  float fadeDistance = 0.7f;
  bool playSpaceOn = false;
  bool groundPerimeterOn = false;
  bool centerMarkerOn = false;
};

static CachedChaperoneSettings s_cachedSettings;
static bool s_bChaperoneHidden = false;

static void sie__psvr2__VrDialogManager__hideChaperone_Hook(void *thisptr) {
  if (!s_bChaperoneHidden) {
    vr::IVRSettings *pSettings = vr::VRSettings();
    if (pSettings) {
      s_cachedSettings.fadeDistance = pSettings->GetFloat(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_FadeDistance_Float);
      s_cachedSettings.playSpaceOn = pSettings->GetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_PlaySpaceOn_Bool);
      s_cachedSettings.groundPerimeterOn = pSettings->GetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_GroundPerimeterOn_Bool);
      s_cachedSettings.centerMarkerOn = pSettings->GetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_CenterMarkerOn_Bool);

      pSettings->SetFloat(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_FadeDistance_Float, 0.0f);
      pSettings->SetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_PlaySpaceOn_Bool, false);
      pSettings->SetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_GroundPerimeterOn_Bool, false);
      pSettings->SetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_CenterMarkerOn_Bool, false);

      Util::DriverLog("[VrTracker2] hide Chaperone (cached FadeDistance: {})\n", s_cachedSettings.fadeDistance);
    }
    s_bChaperoneHidden = true;
  }
}

static void sie__psvr2__VrDialogManager__showChaperone_Hook(void *thisptr) {
  // If the chaperone was never hidden, do not restore/write anything.
  // This prevents clobbering user settings with driver defaults when no HMD is connected or on driver cleanup.
  if (!s_bChaperoneHidden) {
    return;
  }
  s_bChaperoneHidden = false;

  vr::IVRSettings *pSettings = vr::VRSettings();
  if (pSettings) {
    pSettings->SetFloat(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_FadeDistance_Float, s_cachedSettings.fadeDistance);
    pSettings->SetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_PlaySpaceOn_Bool, s_cachedSettings.playSpaceOn);
    pSettings->SetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_GroundPerimeterOn_Bool, s_cachedSettings.groundPerimeterOn);
    pSettings->SetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_CenterMarkerOn_Bool, s_cachedSettings.centerMarkerOn);
  }

  Util::DriverLog("[VrTracker2] show Chaperone({})\n", s_cachedSettings.fadeDistance);
}

void VrDialogManagerHooks::InstallHooks() {
  static HmdDriverLoader *pHmdDriverLoader = HmdDriverLoader::Instance();

  HookLib::InstallHook(reinterpret_cast<void *>(pHmdDriverLoader->GetBaseAddress() + 0x131390),
                       reinterpret_cast<void *>(sie__psvr2__VrDialogManager__hideChaperone_Hook));

  HookLib::InstallHook(reinterpret_cast<void *>(pHmdDriverLoader->GetBaseAddress() + 0x133CA0),
                       reinterpret_cast<void *>(sie__psvr2__VrDialogManager__showChaperone_Hook));

  // Remove signature checks.
  INSTALL_STUB_RET0(reinterpret_cast<void *>(pHmdDriverLoader->GetBaseAddress() + 0x134FF0)); // VrDialogManager::VerifyLibrary

  // Remove dashboard, dialog, and desktop app process launch.
  INSTALL_STUB(reinterpret_cast<void *>(pHmdDriverLoader->GetBaseAddress() + 0x12F830)); // VrDialogManager::CreateDashboardProcess
  INSTALL_STUB(reinterpret_cast<void *>(pHmdDriverLoader->GetBaseAddress() + 0x130020)); // VrDialogManager::CreateDialogProcess
  INSTALL_STUB(reinterpret_cast<void *>(pHmdDriverLoader->GetBaseAddress() + 0x131D90)); // VrDialogManager::CreateDesktopAppProcess
}

} // namespace psvr2_toolkit
