#pragma once
// The keeper: what a start by RecompLauncher runs as on a Mac (the counterpart of app/supervisor.cpp's job object on Windows).
//
// Why: the game starts a new process at each level load (kernel::Relaunch) and the old one exits, while RecompLauncher keeps the process
// it started and stops only that one (SIGKILL to its pid, no process group, and it cannot follow a game's other processes on a Mac). So the
// first process is a keeper that lives for the whole session: it runs the game as its own child and, when the game wants to start the
// next instance, starts it for it, so every instance is the keeper's child as the first one is (an instance started by the one before it
// is what macOS listed as a second app, and kept in the Dock, when Relaunch tried it). When the keeper is killed, every game process ends
// within a moment. When the last one has gone the keeper ends with its exit code.
//
// Three pipes join them, made by the keeper; the game's ends reach each game process under the same numbers (the keeper passes them to each
// child alone; the game then makes them close-on-exec, so that what it starts has none), and the numbers are in the environment (kEnv,
// "<lifeline>,<requests>,<replies>"):
//   lifeline  the keeper holds the write end and never writes; every game process reads its end in a thread of its own, and when that reads
//             end of file the keeper is gone (killed): the process ends at once (kExitKeeperGone)
//   requests  every game process holds the write end: "start this program with this argv and this environment"
//   replies   the keeper answers each request with the child's pid, or an error
// The keeper keeps every end too, as it starts every instance. It reaps its children as they end, and when none is left it exits with
// the last one started: its exit code, or 128 + the signal that ended it.
//
// The keeper loads nothing (no SDL, no log of the game's, no window or Dock icon: it never touches AppKit). macOS only.
#ifdef __APPLE__
#include <cstddef>
#include <cstdint>
#include <optional>

namespace keeper {

// in the game process's environment, with the three descriptors; the game takes it out again (Attach)
inline constexpr const char* kEnv = "HELLMOUTH_SUPERVISED";
// the game process's exit code when the keeper is gone
inline constexpr int kExitKeeperGone = 86;
// A request is a frame on the requests' pipe: three 32-bit words (kFrameMagic, the payload's length, the payload's FNV-1a checksum), then the
// payload. A requester killed half way leaves a part of a frame, and what does not check out is skipped to the next whole one.
inline constexpr uint32_t kFrameMagic = 0x5245504B;
inline constexpr size_t kFrameHeader = 12;

// SIGTERM, SIGINT and SIGHUP sent to the keeper go on to the game processes (the keeper itself waits for them to end), and a SIGCHLD that the
// launcher ignores is not the keeper's.
// The keeper's side. Starts exe with argv and envp (what the game process would get, the marker added) and serves it until none is left.
// Returns the exit code to end with. nullopt: nothing could be started (no pipes, no child), and nothing has: the caller runs the game in
// this process, as it would without a keeper. A few lines of what happens go to log_path (nothing when it is null or cannot be written).
std::optional<int> Run(const char* exe, char* const* argv, char* const* envp, const char* log_path);

// The game's side. Takes kEnv out of the environment and, when it names three pipes, starts watching the lifeline and keeps the
// descriptors from every program this process starts. True: there is a keeper; false: there is none (and the game runs as it always did).
bool Attach();
bool Active();

// Asks the keeper to start path with argv and envp (null-terminated, as posix_spawn takes them; the marker is the keeper's to add). The
// pid, or -1 with errno set. Active() must be true.
int Spawn(const char* path, char* const* argv, char* const* envp);

// Before this process replaces itself with exec (Relaunch's last resort when no next instance could be started): the descriptors and the
// marker stay, so that the new program is the game process of the same keeper.
void KeepAcrossExec();

}  // namespace keeper
#endif
