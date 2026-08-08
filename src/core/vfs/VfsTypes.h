#pragma once

#include <QFlags>
#include <QMetaType>
#include <QString>

#include <atomic>
#include <memory>
#include <utility>

namespace mole {

/// Everything a backend can fail with. Backends must map their native errors
/// onto this list so the UI never has to know which backend it is talking to.
struct VfsError
{
    enum Code {
        None,
        NotFound,
        AccessDenied,
        NotSupported,
        NotADirectory,
        IsADirectory,
        AlreadyExists,
        NotEmpty,
        IoError,
        NetworkError,
        Cancelled,
        Unknown
    };

    Code code = None;
    QString message;

    bool isError() const { return code != None; }

    static VfsError ok() { return {}; }
    static VfsError make(Code c, QString msg) { return { c, std::move(msg) }; }
};

/// A backend advertises what it can actually do. The UI greys out actions
/// instead of letting the user hit "not supported" at runtime.
enum class VfsCapability : quint32 {
    None = 0,
    Read = 1u << 0,
    Write = 1u << 1,
    Create = 1u << 2,
    Delete = 1u << 3,
    Rename = 1u << 4,
    MakeDirectory = 1u << 5,
    RandomAccessRead = 1u << 6,
    NativeSearch = 1u << 7, ///< backend can search server-side, no client walk
    Watch = 1u << 8, ///< backend can push change notifications
    ReportsSpace = 1u << 9, ///< backend knows its own capacity
    ReportsAccess = 1u << 10, ///< backend can say who may do what here
};
Q_DECLARE_FLAGS(VfsCapabilities, VfsCapability)
Q_DECLARE_OPERATORS_FOR_FLAGS(VfsCapabilities)

/// How much room a drive has.
///
/// Not every backend can answer. A cloud bucket has no capacity in any useful
/// sense, and an archive's "size" is the file it came from -- so the interface
/// returns NotSupported and callers leave the figure out rather than inventing
/// one. A wrong number here would be read as fact.
struct SpaceInfo
{
    qint64 totalBytes = 0;
    qint64 freeBytes = 0;

    qint64 usedBytes() const { return totalBytes > freeBytes ? totalBytes - freeBytes : 0; }
    /// 0.0 .. 1.0, or 0.0 when the total is unknown.
    double usedFraction() const
    {
        return totalBytes > 0 ? static_cast<double>(usedBytes()) / static_cast<double>(totalBytes) : 0.0;
    }
    bool isValid() const { return totalBytes > 0; }
};

/// What the current user may do at a location.
///
/// Deliberately a set of questions rather than a mode. POSIX mode bits do not
/// describe a Windows ACL, and neither describes an S3 bucket policy or a zip
/// file -- but "may I write here?" is a question every one of them can answer,
/// or admit it cannot.
///
/// `Unknown` is a first-class answer. A drive that has no idea says so, and the
/// interface shows nothing rather than a guess presented as fact.
struct AccessInfo
{
    enum class Answer { Unknown, Yes, No };

    Answer read = Answer::Unknown;
    Answer write = Answer::Unknown;
    /// Whether new entries can be made inside. Only meaningful for a directory.
    Answer createInside = Answer::Unknown;
    /// Whether this entry itself can be removed.
    Answer remove = Answer::Unknown;
    Answer changePermissions = Answer::Unknown;

    /// The platform's own form, shown as-is when the platform has one:
    /// "rwxr-xr--" on POSIX, an ACL summary on Windows, "public-read" for a
    /// bucket. Empty when there is nothing native to show.
    QString nativeText;
    /// Who it belongs to, when the drive knows.
    QString owner;
    QString group;

    bool isKnown() const
    {
        return read != Answer::Unknown || write != Answer::Unknown || !nativeText.isEmpty();
    }
};

/// Cooperative cancellation. Cheap to copy, safe to share across threads.
/// Long-running backend calls must poll it.
class CancelToken
{
public:
    CancelToken()
        : m_flag(std::make_shared<std::atomic_bool>(false))
    {
    }

    void cancel() const { m_flag->store(true, std::memory_order_relaxed); }
    bool isCancelled() const { return m_flag->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic_bool> m_flag;
};

/// Result<T> keeps the error next to the value so backends never throw.
/// Use Result<void> for operations that only report success.
template<typename T>
class Result
{
public:
    Result(T value)
        : m_value(std::move(value))
    {
    }
    Result(VfsError error)
        : m_error(std::move(error))
    {
    }

    static Result failure(VfsError::Code code, QString message)
    {
        return Result(VfsError::make(code, std::move(message)));
    }

    bool ok() const { return !m_error.isError(); }
    explicit operator bool() const { return ok(); }

    const VfsError& error() const { return m_error; }

    /// Only valid when ok() is true.
    const T& value() const { return m_value; }
    T& value() { return m_value; }
    T valueOr(T fallback) const { return ok() ? m_value : std::move(fallback); }

private:
    T m_value {};
    VfsError m_error;
};

template<>
class Result<void>
{
public:
    Result() = default;
    Result(VfsError error)
        : m_error(std::move(error))
    {
    }

    static Result failure(VfsError::Code code, QString message)
    {
        return Result(VfsError::make(code, std::move(message)));
    }

    bool ok() const { return !m_error.isError(); }
    explicit operator bool() const { return ok(); }
    const VfsError& error() const { return m_error; }

private:
    VfsError m_error;
};

} // namespace mole

Q_DECLARE_METATYPE(mole::SpaceInfo)
Q_DECLARE_METATYPE(mole::AccessInfo)
