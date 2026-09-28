#include "hmd_presence_hooks.h"

#include "hmd_driver_loader.h"
#include "hook_lib.h"
#include "util.h"
#include "vr_settings.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace psvr2_toolkit {
namespace {

using IsHmdPresentFn = bool (*)(void *);

IsHmdPresentFn g_isHmdPresent = nullptr;

// Verified against the Sony driver with SHA-256:
// aa0e1202420fe0014d8cc0634efd02ba23f7c292ea9099d7befb09b4ccfd77e6
constexpr uintptr_t kIsHmdPresentRva = 0x1253C0;

// Prologue/signature at kIsHmdPresentRva. Refuse to hook if Sony updates the
// implementation instead of blindly applying a stale fixed offset.
constexpr std::array<uint8_t, 26> kExpectedPrologue = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x68, 0x18, 0x57, 0x48,
    0x83, 0xEC, 0x70, 0x48, 0x8B, 0xB9, 0xB8, 0x00, 0x00,
    0x00, 0x48, 0x8B, 0xE9, 0x83, 0x7F, 0x28, 0x02};

bool IsHmdPresentHook(void *thisptr) {
  const bool present = g_isHmdPresent(thisptr);
  if (present) {
    return true;
  }

  static bool loggedOverride = false;
  if (!loggedOverride) {
    Util::DriverLog("Sony HMD presence check returned false; forcing present because forceHmdPresentWithoutDisplay is enabled.");
    loggedOverride = true;
  }

  return true;
}

} // namespace

void HmdPresenceHooks::InstallHooks() {
  if (!VRSettings::GetBool(STEAMVR_SETTINGS_FORCE_HMD_PRESENT_WITHOUT_DISPLAY,
                           SETTING_FORCE_HMD_PRESENT_WITHOUT_DISPLAY_DEFAULT_VALUE)) {
    return;
  }

  static HmdDriverLoader *pHmdDriverLoader = HmdDriverLoader::Instance();
  auto *pTarget = reinterpret_cast<void *>(pHmdDriverLoader->GetBaseAddress() + kIsHmdPresentRva);

  if (std::memcmp(pTarget, kExpectedPrologue.data(), kExpectedPrologue.size()) != 0) {
    Util::DriverLog(
        "forceHmdPresentWithoutDisplay requested, but the Sony HMD-presence function signature does not match this Toolkit build; hook not installed.");
    return;
  }

  HookLib::InstallHook(pTarget, reinterpret_cast<void *>(IsHmdPresentHook),
                       reinterpret_cast<void **>(&g_isHmdPresent));

  Util::DriverLog("Installed experimental forceHmdPresentWithoutDisplay compatibility hook.");
}

} // namespace psvr2_toolkit
