#pragma once

#include <hidapi/hidapi.h>
#include <cstdint>
#include <string>

namespace ratatoskr {

/// Result of a HID write/read operation
struct HidError {
    std::string message;
};

/// RAII wrapper around hidapi device handle
class HidDevice {
public:
    HidDevice() = default;
    ~HidDevice();

    HidDevice(const HidDevice&) = delete;
    HidDevice& operator=(const HidDevice&) = delete;
    HidDevice(HidDevice&& other) noexcept;
    HidDevice& operator=(HidDevice&& other) noexcept;

    /// Open a HID device by vendor/product ID and usage page
    bool open(uint16_t vendor_id, uint16_t product_id, uint16_t usage_page);

    /// Close the device
    void close();

    /// Check if device is open
    bool isOpen() const { return handle_ != nullptr; }

    /// Write data to device (prepends report ID if needed)
    bool write(const uint8_t* data, size_t length);

    /// Read data from device with timeout (ms). Returns bytes read, 0 on timeout, -1 on error.
    int read(uint8_t* buffer, size_t length, int timeout_ms = 1000);

    /// Get last error message
    std::string lastError() const;

private:
    hid_device* handle_ = nullptr;
};

} // namespace ratatoskr
