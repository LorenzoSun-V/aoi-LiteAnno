#pragma once

#include <QLockFile>
#include <QString>

#include <memory>

namespace liteanno::application {

// The lock of one project location, shared by the window and the command line (format spec
// 8.2), so both cannot change the same project at once. It only binds this program: changes
// by other programs are caught by the check before saving.
class ProjectLock
{
public:
    enum class Status {
        Locked,
        HeldElsewhere,  // another running LiteAnno process holds it
        Error,          // the lock file cannot be created
    };

    struct Holder {
        qint64 pid = 0;
        QString hostName;
        QString appName;
    };

    explicit ProjectLock(const QString& lockPath);
    ~ProjectLock();
    ProjectLock(const ProjectLock&) = delete;
    ProjectLock& operator=(const ProjectLock&) = delete;

    // Does not wait. A lock left by a process that is gone (a crash, a forced kill, a
    // restart) is taken over.
    Status tryLock();
    void unlock();
    bool isLocked() const;

    // Who holds the lock after tryLock() returned HeldElsewhere.
    const Holder& holder() const { return m_holder; }
    const QString& path() const { return m_path; }

private:
    QString m_path;
    std::unique_ptr<QLockFile> m_lock;
    Holder m_holder;
};

} // namespace liteanno::application
