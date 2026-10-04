#pragma once

#include <cstddef>
#include <cstdint>
#include <openvr_driver.h>

namespace psvr2_toolkit {

class SonyOpticalCapture {
public:
  static bool Enabled();

  static void NoteLedCommand(bool isLeft, const void *command, size_t commandSize, uint8_t currentPhase,
                             uint8_t currentSequence, uint8_t currentPeriod, int32_t baseTime,
                             uint32_t frameCycle);
  static void NoteTracking(bool isLeft, int oldFlag, int newFlag);

  static void CaptureTrackingImage(const void *imageData, size_t imageSize, uint32_t imageTimestamp,
                                   uint16_t imageType);
  static void CapturePublishedPose(const char *deviceLabel, uint32_t deviceIndex,
                                   const vr::DriverPose_t &pose);
};

} // namespace psvr2_toolkit
