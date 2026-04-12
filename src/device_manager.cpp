#include "device_manager.hpp"
#include "drivers/a50_gen5.hpp"
#include <libudev.h>
#include <iostream>

namespace ratatoskr {

DeviceManager::DeviceManager() {
    udev_ = udev_new();
    if (!udev_) {
        std::cerr << "Failed to create udev context\n";
        return;
    }

    monitor_ = udev_monitor_new_from_netlink(udev_, "udev");
    if (monitor_) {
        udev_monitor_filter_add_match_subsystem_devtype(monitor_, "hidraw", nullptr);
        udev_monitor_enable_receiving(monitor_);
    }

    registerDrivers();
}

DeviceManager::~DeviceManager() {
    if (monitor_) udev_monitor_unref(monitor_);
    if (udev_) udev_unref(udev_);
}

void DeviceManager::registerDrivers() {
    auto dev = ManagedDevice{};
    dev.driver = std::make_unique<A50Gen5Driver>();
    devices_.push_back(std::move(dev));
}

void DeviceManager::scan() {
    for (auto& dev : devices_) {
        tryOpen(dev);
    }
}

void DeviceManager::tryOpen(ManagedDevice& dev) {
    bool was_connected = dev.connected;

    if (dev.hid.isOpen()) {
        return;
    }

    if (dev.driver->open(dev.hid)) {
        dev.connected = true;
        std::cout << "Device connected: " << dev.driver->name() << "\n";
        if (callback_ && !was_connected) {
            callback_(dev, true);
        }
    }
}

int DeviceManager::udevFd() const {
    if (!monitor_) return -1;
    return udev_monitor_get_fd(monitor_);
}

void DeviceManager::processUdevEvents() {
    if (!monitor_) return;

    auto* dev = udev_monitor_receive_device(monitor_);
    if (!dev) return;

    const char* action = udev_device_get_action(dev);
    if (!action) {
        udev_device_unref(dev);
        return;
    }

    std::string act(action);

    if (act == "remove") {
        for (auto& managed : devices_) {
            if (managed.connected && !managed.hid.isOpen()) {
                managed.connected = false;
                std::cout << "Device disconnected: " << managed.driver->name() << "\n";
                if (callback_) {
                    callback_(managed, false);
                }
            }
        }
    } else if (act == "add") {
        for (auto& managed : devices_) {
            if (!managed.connected || !managed.hid.isOpen()) {
                // Close stale handle if still marked connected
                if (managed.connected) {
                    managed.hid.close();
                    managed.connected = false;
                }
                tryOpen(managed);
            }
        }
    }

    udev_device_unref(dev);
}

} // namespace ratatoskr
