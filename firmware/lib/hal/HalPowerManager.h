#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds
  mutable uint16_t _batteryCachedMv = 0;         // x4r: the averaged voltage behind the cached percentage

  int lockCount = 0;                      // Live Lock instances; any one holds full speed
  SemaphoreHandle_t modeMutex = nullptr;  // Protects lockCount

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);
  bool isPowerSaving() const { return isLowPower; }

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop()
  // wakeOnHeldButton: arm the wake without waiting for the power button's
  // release, so a press that is still down wakes the device straight back up
  // through the normal power-button wake path.
  void startDeepSleep(HalGPIO& gpio, bool wakeOnHeldButton = false) const;

#if FREEINK_DEVICE_PICCO
  // While USB powers the device, light-sleep instead of deep sleep (deep sleep
  // cannot observe the charger), calling onStatus whenever the charging state
  // or percentage changes. Returns true when the power button ended the hold,
  // false when USB went away or was never present.
  bool holdWhileUsbPowered(HalGPIO& gpio, void (*onStatus)(bool charging, uint16_t percent)) const;
#endif

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;
#if X4R
  // The averaged battery voltage the percentage was read from, in mV (0 before the first read).
  uint16_t getBatteryMillivolts() const;
#endif

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. Power saving is allowed again once every Lock is destroyed.
  class Lock {
   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
