#pragma once

#include <cstddef>
#include <cstdint>
#include <openvr_driver.h>

namespace psvr2_toolkit {

class SonyOpticalCapture {
public:
  static bool Enabled();

  static void NoteLedCommand(bool isLeft, const void *command, size_t commandSize);

  static void CaptureTrackingImage(const void *imageData, size_t imageSize, uint32_t imageTimestamp,
                                   uint16_t imageType);
  static void CapturePublishedPose(const char *deviceLabel, uint32_t deviceIndex,
                                   const vr::DriverPose_t &pose);
  static void CaptureObservableUsbRead(uint8_t interfaceNumber, uint8_t pipeId,
                                       const void *data, size_t size);
};

} // namespace psvr2_toolkit
