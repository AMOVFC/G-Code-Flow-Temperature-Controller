// Process execution.
//
// The ONLY file in core permitted to touch OS APIs directly.
//
// Notably this does NOT go through a shell. The legacy built a single command string and
// handed it to `cmd.exe` with `>` redirection, which is where its documented "paths must
// not contain spaces" limitation came from and why the estimator's stderr was invisible
// (known-bugs.md #7). Passing an argument vector and capturing the pipes directly makes
// both problems disappear.

#include "sb53/SubprocessRunner.hpp"

#include <array>
#include <thread>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sb53 {
namespace {

// RAII for a Win32 handle. Process launch has several failure points and leaking a
// handle on any of them would be a slow resource leak in a long-running GUI session.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : m_h(h) {}
    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : m_h(o.release()) {}
    Handle& operator=(Handle&& o) noexcept
    {
        if (this != &o) { reset(o.release()); }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return m_h; }
    [[nodiscard]] HANDLE* put() noexcept { return &m_h; }
    [[nodiscard]] bool valid() const noexcept
    {
        return m_h != nullptr && m_h != INVALID_HANDLE_VALUE;
    }

    HANDLE release() noexcept { HANDLE h = m_h; m_h = nullptr; return h; }

    void reset(HANDLE h = nullptr) noexcept
    {
        if (valid()) { ::CloseHandle(m_h); }
        m_h = h;
    }

private:
    HANDLE m_h = nullptr;
};

// Quotes one argument per the Windows command-line parsing rules that CommandLineToArgvW
// implements. Without this, any path containing a space is silently split into two
// arguments -- exactly the legacy's bug.
void appendQuoted(std::wstring& out, const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        out += arg;
        return;
    }

    out += L'"';
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }

        if (it == arg.end()) {
            // Escape trailing backslashes so they do not escape the closing quote.
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += *it;
        }
    }
    out += L'"';
}

std::wstring widen(const std::string& s)
{
    if (s.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                           static_cast<int>(s.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          result.data(), size);
    return result;
}

// Drains a pipe to completion. Run on its own thread per stream: a single-threaded
// read of stdout can deadlock when the child fills the stderr buffer and blocks.
void drain(HANDLE pipe, std::string& out)
{
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                        &read, nullptr) || read == 0) {
            break;   // EOF or the write end closed
        }
        out.append(buffer.data(), read);
    }
}

} // namespace

ProcessResult SubprocessRunner::run(const std::filesystem::path& executable,
                                    const std::vector<std::string>& arguments,
                                    std::chrono::milliseconds timeout)
{
    ProcessResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    Handle outRead, outWrite, errRead, errWrite;
    if (!::CreatePipe(outRead.put(), outWrite.put(), &sa, 0) ||
        !::CreatePipe(errRead.put(), errWrite.put(), &sa, 0)) {
        result.launchFailed = true;
        result.stdErr = "Failed to create pipes for the child process.";
        return result;
    }

    // The child must not inherit our read ends, or the pipes never report EOF.
    ::SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(errRead.get(), HANDLE_FLAG_INHERIT, 0);

    std::wstring commandLine;
    appendQuoted(commandLine, executable.wstring());
    for (const auto& arg : arguments) {
        commandLine += L' ';
        appendQuoted(commandLine, widen(arg));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = outWrite.get();
    si.hStdError = errWrite.get();
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    const BOOL ok = ::CreateProcessW(
        executable.wstring().c_str(),
        commandLine.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    if (!ok) {
        result.launchFailed = true;
        result.stdErr = "Could not start '" + executable.string() +
                        "' (Windows error " + std::to_string(::GetLastError()) + ").";
        return result;
    }

    Handle process{pi.hProcess};
    Handle thread{pi.hThread};

    // Close our copies of the write ends, otherwise the reads never see EOF.
    outWrite.reset();
    errWrite.reset();

    std::thread errThread{[&] { drain(errRead.get(), result.stdErr); }};
    drain(outRead.get(), result.stdOut);
    errThread.join();

    const DWORD waitMs = timeout.count() > 0
                             ? static_cast<DWORD>(timeout.count())
                             : INFINITE;
    if (::WaitForSingleObject(process.get(), waitMs) == WAIT_TIMEOUT) {
        ::TerminateProcess(process.get(), 1);
        ::WaitForSingleObject(process.get(), 5000);
        result.timedOut = true;
        return result;
    }

    DWORD code = 0;
    ::GetExitCodeProcess(process.get(), &code);
    result.exitCode = static_cast<int>(code);
    return result;
}

} // namespace sb53

#else   // ---------------------------------------------------------------- POSIX

#include <cstdio>

namespace sb53 {

ProcessResult SubprocessRunner::run(const std::filesystem::path& executable,
                                    const std::vector<std::string>&,
                                    std::chrono::milliseconds)
{
    // Windows is the current target (ADR-0001). The Linux CI build exists to prove core
    // stayed portable and UI-agnostic, not to ship -- so this fails honestly rather than
    // pretending to work.
    ProcessResult result;
    result.launchFailed = true;
    result.stdErr = "Running external processes is not implemented on this platform yet "
                    "(tried to run '" + executable.string() + "').";
    return result;
}

} // namespace sb53

#endif
