#pragma once
// Rewind (kernel/rewind.h): what Direct3D and DirectSound keep on the host that guest memory does not hold. The guest is held
// still by the caller for each.
#include <memory>

namespace xdk {

struct DeviceSnapshot {
    DeviceSnapshot();
    ~DeviceSnapshot();
    DeviceSnapshot(const DeviceSnapshot&) = delete;
    DeviceSnapshot& operator=(const DeviceSnapshot&) = delete;
    struct D;
    std::unique_ptr<D> d;
};
// Waits for the renderer to finish what it was given, then copies the host's Direct3D state (shaders, render state,
// surfaces, the immediate mode vertices, the ring of user pointer vertices).
void CaptureDevice(DeviceSnapshot& out);
// Puts that state back and has the renderer forget every texture and target it made: they are made again from guest memory.
void RestoreDevice(const DeviceSnapshot& in);

struct SoundSnapshot {
    SoundSnapshot();
    ~SoundSnapshot();
    SoundSnapshot(const SoundSnapshot&) = delete;
    SoundSnapshot& operator=(const SoundSnapshot&) = delete;
    struct D;
    std::unique_ptr<D> d;
};
void CaptureSound(SoundSnapshot& out);
void RestoreSound(const SoundSnapshot& in);

}  // namespace xdk
