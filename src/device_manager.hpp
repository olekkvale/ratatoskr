#pragma once

#include "hid_device.hpp"
#include "drivers/driver.hpp"
#include <memory>
#include <vector>
#include <functional>
#include <string>

struct udev;
struct udev_monitor;

namespace ratatoskr {

struct ManagedDevice {
    std::unique_ptr<Driver> driver;
    HidDevice hid;
    bool connected = false;
};

/// Manages device discovery, hotplug, and driver lifecycle
class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    /// Scan for known devices and open them
    void scan();

    /// Get file descriptor for udev monitor (for poll/select integration)
    int udevFd() const;

    /// Process pending udev events (call when udevFd() is readable)
    void processUdevEvents();

    /// Access managed devices
    const std::vector<ManagedDevice>& devices() const { return devices_; }
    std::vector<ManagedDevice>& devices() { return devices_; }

    /// Callback for device added/removed
    using DeviceCallback = std::function<void(const ManagedDevice&, bool connected)>;
    void setDeviceCallback(DeviceCallback cb) { callback_ = std::move(cb); }

private:
    std::vector<ManagedDevice> devices_;
    DeviceCallback callback_;

    udev* udev_ = nullptr;
    udev_monitor* monitor_ = nullptr;

    void registerDrivers();
    void tryOpen(ManagedDevice& dev);
};

} // namespace ratatoskr
