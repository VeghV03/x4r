#pragma once

// Serves the x4 USB protocol (docs/PROTOCOL.md) on the USB-Serial/JTAG port.
// poll() runs once per main-loop pass and never blocks for long: file and
// upload transfers advance a bounded amount per call.
class UsbLink {
 public:
  // Call before Serial.begin(): sizes the receive queue to hold a whole upload chunk.
  void begin();
  void poll();
  bool sessionOpen() const;
  // True while a file streams in or out, also between the ranges of a get;
  // the main loop then skips its idle delay.
  bool transferring() const;
};

extern UsbLink usbLink;
