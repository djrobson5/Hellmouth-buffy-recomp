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
    void Clear();  // lets go of the copy
};
// Waits for the renderer to finish what it was given, then copies the host's Direct3D state (shaders, render state,
// surfaces, the immediate mode vertices, the ring of user pointer vertices).
void CaptureDevice(DeviceSnapshot& out);
// Waits, with the guest held and before a restore replaces its memory, for the renderer to finish everything it was given (a render
// thread may still be reading guest pages).
void FlushDevice();
// Whether the renderer can drop what it made from guest memory (gpu::Backend::SupportsReset); false before there is one
bool DeviceCanReset();
// Puts that state back and has the renderer forget every texture and target it made: they are made again from guest memory.
void RestoreDevice(const DeviceSnapshot& in);

struct SoundSnapshot {
    SoundSnapshot();
    ~SoundSnapshot();
    SoundSnapshot(const SoundSnapshot&) = delete;
    SoundSnapshot& operator=(const SoundSnapshot&) = delete;
    struct D;
    std::unique_ptr<D> d;
    void Clear();  // lets go of the copy
};
void CaptureSound(SoundSnapshot& out);
void RestoreSound(const SoundSnapshot& in);

// The XInput handles the title holds (a handle is guest memory, what it names is on the host): one closed since the snapshot
// would leave the title polling a handle that is gone.
struct InputSnapshot {
    InputSnapshot();
    ~InputSnapshot();
    InputSnapshot(const InputSnapshot&) = delete;
    InputSnapshot& operator=(const InputSnapshot&) = delete;
    struct D;
    std::unique_ptr<D> d;
    void Clear();  // lets go of the copy
};
void CaptureInput(InputSnapshot& out);
void RestoreInput(const InputSnapshot& in);

// Movies whose frame the host held back (bink.cpp) are named by guest address; after a restore none is held.
void ForgetMovies();

}  // namespace xdk
