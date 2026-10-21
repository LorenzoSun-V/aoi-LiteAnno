#include "application/ProjectLock.h"

#include <QDir>
#include <QFileInfo>

namespace liteanno::application {

ProjectLock::ProjectLock(const QString& lockPath)
    : m_path(lockPath)
    , m_lock(std::make_unique<QLockFile>(lockPath))
{
    // Never stale because of age: a project can stay open for days. A lock whose process has
    // exited is still detected as stale (qtbase qlockfile.cpp, isApparentlyStale).
    m_lock->setStaleLockTime(0);
}

ProjectLock::~ProjectLock() = default;

ProjectLock::Status ProjectLock::tryLock()
{
    if (m_lock->isLocked())
        return Status::Locked;
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return Status::Error;
    if (m_lock->tryLock(0))
        return Status::Locked;
    if (m_lock->error() != QLockFile::LockFailedError)
        return Status::Error;
    m_holder = Holder();
    m_lock->getLockInfo(&m_holder.pid, &m_holder.hostName, &m_holder.appName);
    return Status::HeldElsewhere;
}

void ProjectLock::unlock()
{
    m_lock->unlock();
}

bool ProjectLock::isLocked() const
{
    return m_lock->isLocked();
}

} // namespace liteanno::application
