#include "psvr2tk_capi.h"
#include "common.h"
#include "psvr2tk_capi_private.h"
#include "custom_share_manager.h"

extern "C" {
static int g_slot = -1;
static int g_lastGazeStatusCounter = -1;
static int g_lastGazeImageCounter = -1;

int psvr2_toolkit_init() {
  CustomShareManager::createSingleton();

  if (g_slot < 0) {
    g_slot = CustomShareManager::getSingleton()->claimSlot();
  }
  return g_slot >= 0 ? PSVR2TK_RESULT_OK : PSVR2TK_RESULT_NO_SLOT;
}

void psvr2_toolkit_deinit() {
  if (g_slot >= 0) {
    CustomShareManager::getSingleton()->releaseSlot(g_slot);
    g_slot = -1;
  }
}

bool psvr2_toolkit_get_driver_active() { return CustomShareManager::getSingleton()->getDriverActive(); }

bool psvr2_toolkit_gaze_status(hmd2_gaze_status_t *pGazeStatus, uint32_t timeoutMs) {
  return CustomShareManager::getSingleton()->getGazeStatus(pGazeStatus, &g_lastGazeStatusCounter, timeoutMs);
}

bool psvr2_toolkit_gaze_image(unsigned char **pGazeImage, uint32_t timeoutMs) {
  return CustomShareManager::getSingleton()->getGazeImageBuffer(pGazeImage, &g_lastGazeImageCounter, timeoutMs);
}

int psvr2_toolkit_write_pcm(VRControllerType controllerType, const unsigned char *pcm) {
  if (!CustomShareManager::getSingleton()->getDriverActive()) {
    return PSVR2TK_RESULT_DRIVER_INACTIVE;
  }

  if (controllerType > VRControllerType::Both) {
    return PSVR2TK_RESULT_INVALID_PARAMETER;
  }

  if (g_slot >= 0) {
    CustomShareManager::getSingleton()->writePcm(g_slot, controllerType, pcm);
  } else {
    return PSVR2TK_RESULT_NO_SLOT;
  }

  return PSVR2TK_RESULT_OK;
}

int psvr2_toolkit_wait_for_pcm() {
  if (!CustomShareManager::getSingleton()->getDriverActive()) {
    return PSVR2TK_RESULT_DRIVER_INACTIVE;
  }

  bool success = CustomShareManager::getSingleton()->waitForPcmUpdate();
  return success ? PSVR2TK_RESULT_OK : PSVR2TK_RESULT_TIMEOUT;
}

int psvr2_toolkit_set_trigger_effect(VRControllerType controllerType, const ScePadTriggerEffectCommand &command) {
  if (!CustomShareManager::getSingleton()->getDriverActive()) {
    return PSVR2TK_RESULT_DRIVER_INACTIVE;
  }

  if (controllerType > VRControllerType::Both) {
    return PSVR2TK_RESULT_INVALID_PARAMETER;
  }

  if (g_slot >= 0) {
    DriverCommand drvCmd = {};
    drvCmd.type = DriverCommandType::TriggerEffectSet;
    drvCmd.triggerEffect.slot = g_slot;
    drvCmd.triggerEffect.payload.controllerType = controllerType;
    drvCmd.triggerEffect.payload.command = command;
    bool success = CustomShareManager::getSingleton()->submitCommand(drvCmd);

    return success ? PSVR2TK_RESULT_OK : PSVR2TK_RESULT_TIMEOUT;
  } else {
    return PSVR2TK_RESULT_NO_SLOT;
  }
}

int psvr2_toolkit_set_hmd_rumble(uint8_t rumbleHz) {
  if (!CustomShareManager::getSingleton()->getDriverActive()) {
    return PSVR2TK_RESULT_DRIVER_INACTIVE;
  }

  // Headset does not do more than 25 hz.
  // The range is technically 10-25 hz, but the headset does accept numbers in the 1-9 range and makes it 10.
  // And we also want to support 0 hz to allow stopping the motor.
  if (rumbleHz > 25) {
    return PSVR2TK_RESULT_INVALID_PARAMETER;
  }

  DriverCommand drvCmd = {};
  drvCmd.type = DriverCommandType::HeadsetRumbleSet;
  drvCmd.headsetRumble.rumbleHz = rumbleHz;
  bool success = CustomShareManager::getSingleton()->submitCommand(drvCmd);

  return success ? PSVR2TK_RESULT_OK : PSVR2TK_RESULT_TIMEOUT;
}

GazeCalibrationCommand psvr2_toolkit_private_send_gaze_set_command(GazeCalibrationCommand command) {
  DriverCommand drvCmd = {};
  drvCmd.type = DriverCommandType::GazeCalibrationSet;
  drvCmd.gazeCalibration = command;
  CustomShareManager::getSingleton()->submitCommand(drvCmd);
  return drvCmd.gazeCalibration;
}

GazeCalibrationCommand psvr2_toolkit_private_send_gaze_get_command(GazeCalibrationCommand command) {
  DriverCommand drvCmd = {};
  drvCmd.type = DriverCommandType::GazeCalibrationGet;
  drvCmd.gazeCalibration = command;
  CustomShareManager::getSingleton()->submitCommand(drvCmd);
  return drvCmd.gazeCalibration;
}

int psvr2_toolkit_private_set_usb_connection_state(bool connected) {
  if (!CustomShareManager::getSingleton()->getDriverActive()) {
    return PSVR2TK_RESULT_DRIVER_INACTIVE;
  }

  DriverCommand drvCmd = {};
  drvCmd.type = DriverCommandType::UsbConnectionStateSet;
  drvCmd.usbConnection.isConnected = connected;
  bool success = CustomShareManager::getSingleton()->submitCommand(drvCmd);

  return success ? PSVR2TK_RESULT_OK : PSVR2TK_RESULT_TIMEOUT;
}
}