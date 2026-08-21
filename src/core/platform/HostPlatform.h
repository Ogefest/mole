#pragma once

namespace mole {

/// Which operating system a rule is being asked about.
///
/// Several things in Mole are decided by the platform and are otherwise pure
/// string handling: how a local path is spelled inside a uri, whether two names
/// that differ in case are the same file, what counts as a drive, what a name is
/// allowed to contain. Every one of those was written as an `#ifdef`, and every
/// one of them was wrong somewhere -- because an `#ifdef` cannot be tested. The
/// suite runs on Linux, so the Windows arm of a compile-time switch is a branch
/// no test has ever entered.
///
/// So the platform is an argument with a default, not a compile switch. The
/// application asks nothing extra and gets the answer for the system it is
/// running on; a test asks for Windows on a Linux machine and gets the Windows
/// answer. That is the difference between a fault that can be fixed today and
/// one that waits for somebody to have the right computer.
enum class HostPlatform {
    Posix, ///< Linux, the BSDs, and anything else with one root and '/'
    Windows,
    MacOS,
};

/// The platform this build targets. The default for every rule below.
constexpr HostPlatform hostPlatform()
{
#if defined(Q_OS_WIN) || defined(_WIN32)
    return HostPlatform::Windows;
#elif defined(Q_OS_MACOS) || defined(__APPLE__)
    return HostPlatform::MacOS;
#else
    return HostPlatform::Posix;
#endif
}

/// Whether paths on this platform are spelled with drive letters, UNC shares and
/// backslashes. Only Windows is, and asking it this way keeps the reason legible
/// at the call site.
constexpr bool usesWindowsPathSyntax(HostPlatform platform)
{
    return platform == HostPlatform::Windows;
}

} // namespace mole
