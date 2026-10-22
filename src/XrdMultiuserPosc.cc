/*
 * XrdMultiuserPosc: persist-on-successful-close for writes that cannot use XRootD's
 * own POSC (HTTP-TPC, WebDAV PUT, and any server that runs the multiuser
 * plugin, which disables XrdOfs POSC).
 *
 * New files are created under a hidden temporary name
 *
 *     <dir>/.<name>.mu-part.<16 hex>
 *
 * and moved to their final name in Close() when the file is complete; an
 * incomplete or failed upload is removed instead.  If the server stops while
 * an upload is in flight, the final name never shows a partial file, and the
 * leftover temporary file is removed at the next startup from a journal.
 *
 * Stacking: load this plugin BELOW xrootd-multiuser so that every call made
 * here already runs with the mapped user's fsuid/fsgid:
 *
 *     ofs.osslib ++ libXrdMultiuserPosc.so  # inner, wraps the default OSS
 *     ofs.osslib ++ libXrdMultiuser.so      # outer, switches the user
 *
 * Configuration (default off):
 *
 *     multiuser.posc.enable on|off          # explicit setting wins
 *     ofs.persist auto                      # if multiuser.posc.enable is not set: on; else off
 *     multiuser.posc.journal <dir>          # required when enabled; see below
 *     multiuser.posc.trace all|debug|info|none
 *
 * Journal: one file per in-flight upload, "<uid> <pid>\n<pfn of temp>\n",
 * written with the caller's IDs into <dir>, which must be sticky and
 * writable by all (mode 1733, created by the daemon if missing).  At startup
 * an entry is acted on only if the entry file, the journal line and the
 * temporary file agree on the owner, so a user can never get another user's
 * file removed.
 */

#include "XrdOss/XrdOss.hh"
#include "XrdOss/XrdOssWrapper.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucStream.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdVersion.hh"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <sys/capability.h>
#include <sys/fsuid.h>
#include <sys/param.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

const char kMarker[] = ".mu-part.";  // part of the on-disk format: external tools match on it

namespace Trace {
const int Debug = 0x01;
const int Info  = 0x02;
const int All   = 0xff;
int mask = Info;
inline bool On(int cls) {return (mask & cls) != 0;}
}

// "<dir>/<name>" -> "<dir>/.<name>.mu-part.<token>", kept within NAME_MAX.
std::string TempName(const std::string &path, const std::string &token)
{
    auto slash = path.rfind('/');
    std::string dir = (slash == std::string::npos) ? "" : path.substr(0, slash + 1);
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t extra = 1 + sizeof(kMarker) - 1 + token.size();
    if (name.size() + extra > NAME_MAX) {name.resize(NAME_MAX - extra);}
    return dir + "." + name + kMarker + token;
}

std::string TokenOf(const std::string &tmp)
{
    auto pos = tmp.rfind(kMarker);
    return (pos == std::string::npos) ? "" : tmp.substr(pos + sizeof(kMarker) - 1);
}

// 16 hex digits.  getrandom(2) needs no descriptor and cannot throw;
// std::random_device opens /dev/urandom and throws under fd exhaustion.
std::string NewToken()
{
    unsigned char bytes[8];
    if (getrandom(bytes, sizeof(bytes), 0) != static_cast<ssize_t>(sizeof(bytes))) {
        static thread_local std::mt19937_64 gen(static_cast<unsigned long long>(time(nullptr)) ^
                                                (static_cast<unsigned long long>(getpid()) << 32) ^
                                                reinterpret_cast<uintptr_t>(&gen));
        unsigned long long v = gen();
        memcpy(bytes, &v, sizeof(bytes));
    }
    char hex[2 * sizeof(bytes) + 1];
    for (size_t i = 0; i < sizeof(bytes); i++) {snprintf(hex + 2 * i, 3, "%02x", bytes[i]);}
    return hex;
}

// oss.asize as a size, or -1 if absent or not a size.  XrdHttpTpc sends
// 18446744073709551615 (-1) when the source gave no Content-Length.
long long ExpectedSize(XrdOucEnv &env)
{
    const char *asize = env.Get("oss.asize");
    if (!asize) {return -1;}
    errno = 0;
    char *end = nullptr;
    long long val = strtoll(asize, &end, 10);
    return (errno == 0 && end != asize && *end == '\0' && val >= 0) ? val : -1;
}

// Switch this thread's FS IDs to a file owner for startup cleanup.
class OwnerIDs {
public:
    explicit OwnerIDs(uid_t uid) {
        if (static_cast<uid_t>(setfsuid(-1)) == uid) {m_ok = true; return;}  // already there
        struct passwd pwd, *res = nullptr;
        std::vector<char> buf(16384);
        if (getpwuid_r(uid, &pwd, buf.data(), buf.size(), &res) || !res) {return;}
        int ngroups = 64;
        std::vector<gid_t> groups(ngroups);
        if (getgrouplist(res->pw_name, res->pw_gid, groups.data(), &ngroups) < 0) {
            groups.resize(ngroups);
            if (getgrouplist(res->pw_name, res->pw_gid, groups.data(), &ngroups) < 0) {return;}
        }
        groups.resize(ngroups);
        m_uid = setfsuid(uid);
        m_gid = setfsgid(res->pw_gid);
        syscall(SYS_setgroups, groups.size(), groups.data());
        m_ok = (static_cast<uid_t>(setfsuid(-1)) == uid);
        m_switched = true;
    }
    ~OwnerIDs() {
        if (!m_switched) {return;}
        setfsuid(m_uid);
        setfsgid(m_gid);
        syscall(SYS_setgroups, 0, nullptr);
    }
    bool ok() const {return m_ok;}
private:
    uid_t m_uid{0};
    gid_t m_gid{0};
    bool m_switched{false}, m_ok{false};
};

// Raise CAP_SETUID/CAP_SETGID in this thread's effective set (permitted by
// the xrootd-privileged units) for the startup cleanup.
bool RaiseIDCaps()
{
    cap_t caps = cap_get_proc();
    if (!caps) {return false;}
    cap_value_t list[2] = {CAP_SETUID, CAP_SETGID};
    bool ok = cap_set_flag(caps, CAP_EFFECTIVE, 2, list, CAP_SET) == 0 && cap_set_proc(caps) == 0;
    cap_free(caps);
    return ok;
}

struct Pending {
    const XrdOucEnv *env{nullptr};  // Create() and the following Open() see the same object
    std::string path;               // final name
    std::string tmp;                // temporary name
    bool exclusive{false};          // O_EXCL create: must not replace an existing final name
};

class PoscOss;

class PoscFile : public XrdOssWrapDF {
public:
    PoscFile(XrdOssDF *df, PoscOss &oss) : XrdOssWrapDF(*df), m_df(df), m_oss(oss) {}
    virtual ~PoscFile();
    int Open(const char *path, int Oflag, mode_t Mode, XrdOucEnv &env) override;
    int Close(long long *retsz = 0) override;
private:
    int Publish();
    void Abandon();
    bool AsOpener(std::unique_ptr<OwnerIDs> &ids);
    std::unique_ptr<XrdOssDF> m_df;
    PoscOss &m_oss;
    std::string m_path, m_tmp;
    bool m_excl{false};
    long long m_asize{-1};
    uid_t m_open_uid{0};  // fsuid the file was opened with
};

class PoscOss : public XrdOssWrapper {
public:
    PoscOss(XrdOss &wrapped, XrdSysLogger *lp) : XrdOssWrapper(wrapped), m_wrapped(wrapped), m_log(lp, "mposc_") {}

    bool Config(const char *configfn);

    XrdOssDF *newFile(const char *tident) override {
        XrdOssDF *df = m_wrapped.newFile(tident);
        return (df && m_on) ? new PoscFile(df, *this) : df;
    }

    int Create(const char *tid, const char *path, mode_t mode, XrdOucEnv &env, int opts = 0) override;

    int Unlink(const char *path, int Opts = 0, XrdOucEnv *envP = 0) override {
        int rc = m_wrapped.Unlink(path, Opts, envP);
        if (rc == 0 && envP && m_on) {Cancel(path);}
        return rc;
    }

    // Used by PoscFile.
    bool Take(const XrdOucEnv *env, const char *path, Pending &pending);
    bool Finish(const std::string &path, const std::string &tmp);
    void JournalDel(const std::string &tmp);
    XrdOss &Base() {return m_wrapped;}
    XrdSysError &Log() {return m_log;}

private:
    bool FinishLocked(const std::string &path, const std::string &tmp);
    void Cancel(const char *path);
    void Drop(const Pending &pending);
    bool JournalAdd(const std::string &tmp);
    void Recover();

    XrdOss &m_wrapped;
    XrdSysError m_log;
    bool m_on{false};
    std::string m_journal;

    std::mutex m_mutex;  // guards the three containers below
    // Creates whose Open() has not happened yet, by thread: XrdOfs calls
    // Create() and Open() back to back on one thread, so an entry still
    // present when the thread creates again is stale (the open was refused
    // or attached to an existing handle) and its temporary file is dropped.
    std::map<pthread_t, Pending> m_pending;
    // Final name -> temporary files being written to it.
    std::map<std::string, std::set<std::string>> m_inflight;
    // Temporary files whose final name a client removed while in flight.
    std::set<std::string> m_cancelled;
};

/******************************************************************************/
/*                                C o n f i g                                 */
/******************************************************************************/

bool PoscOss::Config(const char *configfn)
{
    XrdOucEnv myEnv;
    XrdOucStream cfg(&m_log, getenv("XRDINSTANCE"), &myEnv, "=====> ");
    int fd = open(configfn, O_RDONLY, 0);
    if (fd < 0) {
        m_log.Emsg("Config", errno, "open config file", configfn);
        return false;
    }
    cfg.Attach(fd);
    int enable = -1;
    bool persist_auto = false;
    // ofs.osslib entries in order, to check the stacking (see below).
    std::vector<std::string> osslibs;
    const char *val;
    while ((val = cfg.GetMyFirstWord())) {
        if (!strcmp(val, "ofs.osslib")) {
            std::string line;
            while ((val = cfg.GetWord())) {
                if (!strcmp(val, "++") || val[0] == '-') {continue;}  // stacking marker, options
                line = val;
                break;
            }
            if (!line.empty()) {osslibs.push_back(line);}
            continue;
        }
        if (!strcmp(val, "multiuser.posc.enable")) {
            val = cfg.GetWord();
            if (val && !strcmp(val, "on")) {enable = 1;}
            else if (val && !strcmp(val, "off")) {enable = 0;}
            else {m_log.Emsg("Config", "multiuser.posc.enable must be on or off"); cfg.Close(); return false;}
        } else if (!strcmp(val, "multiuser.posc.journal")) {
            val = cfg.GetWord();
            if (!val || val[0] != '/') {m_log.Emsg("Config", "multiuser.posc.journal requires an absolute directory"); cfg.Close(); return false;}
            m_journal = val;
            while (m_journal.size() > 1 && m_journal.back() == '/') {m_journal.pop_back();}
        } else if (!strcmp(val, "multiuser.posc.trace")) {
            int mask = 0;
            if (!(val = cfg.GetWord())) {m_log.Emsg("Config", "multiuser.posc.trace: trace option not specified"); cfg.Close(); return false;}
            do {
                if (!strcmp(val, "all")) {mask |= Trace::All;}
                else if (!strcmp(val, "debug")) {mask |= Trace::Debug | Trace::Info;}
                else if (!strcmp(val, "info")) {mask |= Trace::Info;}
                else if (!strcmp(val, "none")) {mask = 0;}
                else {m_log.Emsg("Config", "multiuser.posc.trace: unknown level", val); cfg.Close(); return false;}
            } while ((val = cfg.GetWord()));
            Trace::mask = mask;
        } else if (!strcmp(val, "ofs.persist")) {
            val = cfg.GetWord();
            persist_auto = val && !strcmp(val, "auto");
        }
    }
    int retc = cfg.LastError();
    cfg.Close();
    if (retc) {m_log.Emsg("Config", -retc, "read config file", configfn); return false;}

    // This plugin creates, renames and removes files with whatever IDs the
    // calling thread has; only xrootd-multiuser stacked ABOVE it makes those
    // the mapped user's.  Each "ofs.osslib ++" wraps the libraries listed
    // before it, so a libXrdMultiuser line must follow ours.
    auto base = [](const std::string &lib) {
        std::string b = lib.substr(lib.rfind('/') + 1);
        auto dot = b.find('.');
        if (dot != std::string::npos) {b.resize(dot);}
        auto dash = b.rfind('-');                // drop the "-5" version suffix
        if (dash != std::string::npos && b.find_first_not_of("0123456789", dash + 1) == std::string::npos) {b.resize(dash);}
        return b;
    };
    int self = -1, multiuser = -1;
    for (size_t i = 0; i < osslibs.size(); i++) {
        std::string b = base(osslibs[i]);
        if (b == "libXrdMultiuserPosc") {self = static_cast<int>(i);}
        else if (b == "libXrdMultiuser") {multiuser = static_cast<int>(i);}
    }
    if (multiuser < 0) {
        m_log.Emsg("Config", "libXrdMultiuserPosc requires libXrdMultiuser; add \"ofs.osslib ++ libXrdMultiuser.so\" after it");
        return false;
    }
    if (self >= 0 && multiuser < self) {
        m_log.Emsg("Config", "libXrdMultiuser must be listed after libXrdMultiuserPosc (\"ofs.osslib ++\" wraps the libraries listed before it)");
        return false;
    }

    m_on = (enable >= 0) ? (enable == 1) : persist_auto;
    if (!m_on) {
        m_log.Say("------ POSC emulation is off (multiuser.posc.enable / ofs.persist auto not set).");
        return true;
    }
    if (m_journal.empty()) {
        m_log.Emsg("Config", "POSC emulation needs multiuser.posc.journal <dir>");
        return false;
    }
    // The journal must be writable by every mapped user and protected
    // against deletion by others: sticky, world-writable, owned by us.
    struct stat st;
    if (stat(m_journal.c_str(), &st)) {
        mode_t old = umask(0);
        int rc = mkdir(m_journal.c_str(), 01733);
        umask(old);
        if (rc || stat(m_journal.c_str(), &st)) {
            m_log.Emsg("Config", errno, "create POSC journal directory", m_journal.c_str());
            return false;
        }
    }
    if (!S_ISDIR(st.st_mode) || (st.st_mode & 01333) != 01333 || st.st_uid != geteuid()) {
        m_log.Emsg("Config", "POSC journal must be a directory owned by the daemon with mode 1733:", m_journal.c_str());
        return false;
    }
    m_log.Emsg("Config", "POSC emulation (rename-on-close) enabled; journal", m_journal.c_str());

    // The cmsd loads the OSS too but never writes files.
    char comm[32] = {0};
    if (FILE *f = fopen("/proc/self/comm", "r")) {
        if (!fgets(comm, sizeof(comm), f)) {comm[0] = '\0';}
        fclose(f);
    }
    if (strncmp(comm, "cmsd", 4)) {Recover();}
    return true;
}

/******************************************************************************/
/*                         C r e a t e  /  P e n d i n g                      */
/******************************************************************************/

int PoscOss::Create(const char *tid, const char *path, mode_t mode, XrdOucEnv &env, int opts)
{
    // An xroot third-party copy ("tpc.key") is written by the copy program
    // straight to the final name, not through this handle.
    if (!m_on || env.Get("tpc.key")) {return m_wrapped.Create(tid, path, mode, env, opts);}

    Pending pending;
    pending.env = &env;
    pending.path = path;
    pending.tmp = TempName(path, NewToken());
    pending.exclusive = ((opts >> 8) & O_EXCL) != 0;

    Pending stale;
    bool have_stale = false, busy = false;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        auto iter = m_pending.find(pthread_self());
        if (iter != m_pending.end()) {
            stale = std::move(iter->second);
            have_stale = true;
            m_pending.erase(iter);
            FinishLocked(stale.path, stale.tmp);
        }
        // Exclusive create while another upload to the name is in flight:
        // refuse, as the in-place create did once the file existed.
        auto in = m_inflight.find(pending.path);
        busy = pending.exclusive && in != m_inflight.end() && !in->second.empty();
        if (!busy) {m_inflight[pending.path].insert(pending.tmp);}
    }
    if (have_stale) {Drop(stale);}
    if (busy) {return -EEXIST;}

    // Without a journal entry a crash would leave the temporary file behind
    // forever; fall back to the in-place create for this file instead.
    if (!JournalAdd(pending.tmp)) {
        Finish(pending.path, pending.tmp);
        return m_wrapped.Create(tid, path, mode, env, opts);
    }

    struct stat st;
    int rc;
    if (pending.exclusive && m_wrapped.Stat(path, &st) == 0) {
        rc = -EEXIST;  // fail early; Close() enforces it atomically
    } else {
        rc = m_wrapped.Create(tid, pending.tmp.c_str(), mode, env, opts);
    }
    if (rc) {
        JournalDel(pending.tmp);
        Finish(pending.path, pending.tmp);
        return rc;
    }
    if (Trace::On(Trace::Debug)) {m_log.Emsg("Create", "writing", path, "via a temporary file");}
    std::lock_guard<std::mutex> guard(m_mutex);
    m_pending[pthread_self()] = std::move(pending);
    return 0;
}

// Hand over the temporary file Create() made on this thread for this open.
// Only the same env object and path match, so a client cannot inject a name.
bool PoscOss::Take(const XrdOucEnv *env, const char *path, Pending &pending)
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        auto iter = m_pending.find(pthread_self());
        if (iter == m_pending.end()) {return false;}
        pending = std::move(iter->second);
        m_pending.erase(iter);
        if (pending.env == env && pending.path == path) {return true;}
        FinishLocked(pending.path, pending.tmp);
    }
    Drop(pending);
    pending = Pending();
    return false;
}

// Remove the temporary file of a create whose open never happened.
void PoscOss::Drop(const Pending &pending)
{
    int rc = m_wrapped.Unlink(pending.tmp.c_str());
    if (rc && rc != -ENOENT) {
        // Keep the journal entry; the next startup retries as the owner.
        m_log.Emsg("Posc", rc, "remove unused temporary file", pending.tmp.c_str());
        return;
    }
    JournalDel(pending.tmp);
}

bool PoscOss::Finish(const std::string &path, const std::string &tmp)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return FinishLocked(path, tmp);
}

// tmp is no longer being written.  True if a client removed the final name
// meanwhile, in which case tmp must not be published.
bool PoscOss::FinishLocked(const std::string &path, const std::string &tmp)
{
    bool cancelled = m_cancelled.erase(tmp) > 0;
    auto iter = m_inflight.find(path);
    if (iter != m_inflight.end()) {
        iter->second.erase(tmp);
        if (iter->second.empty()) {m_inflight.erase(iter);}
    }
    return cancelled;
}

// With in-place writes a removed name takes the data with it; uploads in
// flight to it must not reappear when they close.
void PoscOss::Cancel(const char *path)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    auto iter = m_inflight.find(path);
    if (iter == m_inflight.end()) {return;}
    for (const auto &tmp : iter->second) {m_cancelled.insert(tmp);}
    m_inflight.erase(iter);
}

/******************************************************************************/
/*                               J o u r n a l                                */
/******************************************************************************/

// Written with the caller's (mapped user's) IDs; the entry is owned by it.
bool PoscOss::JournalAdd(const std::string &tmp)
{
    char pfn[MAXPATHLEN + 1];
    if (m_wrapped.Lfn2Pfn(tmp.c_str(), pfn, sizeof(pfn))) {
        m_log.Emsg("Posc", "Unable to translate temporary file name", tmp.c_str());
        return false;
    }
    std::string entry = m_journal + "/" + TokenOf(tmp);
    int fd = open(entry.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        m_log.Emsg("Posc", errno, "create journal entry; writing in place", entry.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << static_cast<uid_t>(setfsuid(-1)) << " " << getpid() << "\n" << pfn << "\n";
    std::string data = ss.str();
    bool ok = write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size());
    if (close(fd)) {ok = false;}
    if (!ok) {
        m_log.Emsg("Posc", errno, "write journal entry; writing in place", entry.c_str());
        unlink(entry.c_str());
    }
    return ok;
}

void PoscOss::JournalDel(const std::string &tmp)
{
    std::string entry = m_journal + "/" + TokenOf(tmp);
    if (unlink(entry.c_str()) && errno != ENOENT) {
        m_log.Emsg("Posc", errno, "remove journal entry", entry.c_str());
    }
}

// At startup, remove temporary files of uploads that were in flight when the
// previous process stopped.  Runs with the daemon's IDs; each removal is done
// with the owner's IDs and only if entry, journal line and file agree on it.
void PoscOss::Recover()
{
    DIR *dir = opendir(m_journal.c_str());
    if (!dir) {return;}
    bool caps = RaiseIDCaps();
    int removed = 0, failed = 0, skipped = 0;
    while (struct dirent *ent = readdir(dir)) {
        if (ent->d_name[0] == '.') {continue;}
        std::string entry = m_journal + "/" + ent->d_name;
        // Entries are 0600 and owned by the user that wrote them (others
        // cannot read the directory either), so read them as that owner.
        struct stat est;
        if (lstat(entry.c_str(), &est) || !S_ISREG(est.st_mode) || est.st_size > 8192) {
            m_log.Emsg("Posc", "Ignoring unusable journal entry", entry.c_str());
            continue;
        }
        const uid_t owner = est.st_uid;
        int rc = 0;
        bool bad = false, busy = false;
        std::string pfn;
        {
            OwnerIDs ids(owner);
            std::string data;
            int fd = (caps && ids.ok()) ? open(entry.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
            if (fd >= 0) {
                data.resize(est.st_size);
                ssize_t n = read(fd, &data[0], data.size());
                data.resize(n < 0 ? 0 : n);
                close(fd);
            }
            std::istringstream in(data);
            std::string header;
            unsigned long uid = 0, pid = 0;
            if (fd < 0) {
                rc = (caps && ids.ok()) ? errno : EPERM;
            } else if (!std::getline(in, header) || !(std::istringstream(header) >> uid >> pid) ||
                       !std::getline(in, pfn) || pfn.find(kMarker) == std::string::npos ||
                       uid != owner || TokenOf(pfn) != ent->d_name) {
                bad = true;
            } else if (pid != static_cast<unsigned long>(getpid()) &&
                       (kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM)) {
                busy = true;  // another live instance sharing this journal
            } else {
                struct stat fst;
                if (lstat(pfn.c_str(), &fst)) {
                    rc = (errno == ENOENT) ? 0 : errno;  // already gone
                } else if (!S_ISREG(fst.st_mode) || fst.st_uid != owner) {
                    rc = EPERM;  // not the file the entry describes
                } else if (unlink(pfn.c_str()) && errno != ENOENT) {
                    rc = errno;
                }
            }
        }
        if (bad) {
            m_log.Emsg("Posc", "Ignoring malformed journal entry", entry.c_str());
        } else if (busy) {
            skipped++;
        } else if (rc) {
            m_log.Emsg("Posc", rc, "remove abandoned file", pfn.empty() ? entry.c_str() : pfn.c_str());
            failed++;
        } else {
            m_log.Emsg("Posc", "Removed incomplete file left by a previous run:", pfn.c_str());
            unlink(entry.c_str());  // the daemon owns the sticky directory
            removed++;
        }
    }
    closedir(dir);
    if (removed || failed || skipped) {
        std::ostringstream ss;
        ss << "Startup cleanup: removed " << removed << " incomplete file(s), " << failed
           << " failure(s), " << skipped << " still in flight elsewhere";
        m_log.Emsg("Posc", ss.str().c_str());
    }
}

/******************************************************************************/
/*                                P o s c F i l e                             */
/******************************************************************************/

PoscFile::~PoscFile()
{
    // Destroyed without a successful Close(): the wrapped file closes when
    // m_df goes; then drop the temporary file.
    if (!m_tmp.empty()) {
        std::unique_ptr<OwnerIDs> ids;
        AsOpener(ids);
        m_df.reset();
        Abandon();
    }
}

// Close, rename and remove must run with the IDs the file was opened with.
// xrootd-multiuser >= 2.2.1-1.1.2 (KISTI site build) does that; older builds
// and the stock OSG plugin run Close() with the daemon's IDs, which would make
// every publish fail with EACCES in user-owned directories.  Switch here in
// that case (the thread's CAP_SETUID was raised by multiuser at Open()).
bool PoscFile::AsOpener(std::unique_ptr<OwnerIDs> &ids)
{
    if (static_cast<uid_t>(setfsuid(-1)) == m_open_uid) {return true;}
    static bool warned = false;
    if (!warned) {
        warned = true;
        m_oss.Log().Emsg("Posc", "Close() is not running as the opening user (xrootd-multiuser older than "
                         "2.2.1-1.1.2?); switching IDs here");
    }
    ids.reset(new OwnerIDs(m_open_uid));
    if (!ids->ok()) {
        m_oss.Log().Emsg("Posc", "Unable to switch to the opening user for", m_path.c_str());
        return false;
    }
    return true;
}

int PoscFile::Open(const char *path, int Oflag, mode_t Mode, XrdOucEnv &env)
{
    // XrdOfs opens the file it has just created with O_RDWR|O_TRUNC; an
    // update-mode open (no create) never carries O_TRUNC.
    Pending pending;
    if ((Oflag & O_TRUNC) && m_oss.Take(&env, path, pending)) {
        m_path = path;
        m_tmp = pending.tmp;
        m_excl = pending.exclusive;
        m_asize = ExpectedSize(env);
        m_open_uid = static_cast<uid_t>(setfsuid(-1));
    }
    int rc = wrapDF.Open(m_tmp.empty() ? path : m_tmp.c_str(), Oflag, Mode, env);
    if (rc && !m_tmp.empty()) {Abandon();}
    return rc;
}

// Move the temporary file to its final name.  An exclusive create must not
// replace a file that appeared meanwhile: link(2) fails atomically with
// EEXIST where rename(2) would silently replace.
int PoscFile::Publish()
{
    XrdOss &base = m_oss.Base();
    if (!m_excl) {return base.Rename(m_tmp.c_str(), m_path.c_str());}
    char tmp_pfn[MAXPATHLEN + 1], final_pfn[MAXPATHLEN + 1];
    int rc;
    if ((rc = base.Lfn2Pfn(m_tmp.c_str(), tmp_pfn, sizeof(tmp_pfn))) ||
        (rc = base.Lfn2Pfn(m_path.c_str(), final_pfn, sizeof(final_pfn)))) {
        return rc;
    }
    if (link(tmp_pfn, final_pfn)) {return -errno;}
    rc = base.Unlink(m_tmp.c_str());
    if (rc && rc != -ENOENT) {
        m_oss.Log().Emsg("Posc", rc, "remove temporary name after publishing", m_tmp.c_str());
    }
    return 0;
}

void PoscFile::Abandon()
{
    m_oss.Finish(m_path, m_tmp);
    int rc = m_oss.Base().Unlink(m_tmp.c_str());
    if (rc && rc != -ENOENT) {
        // Keep the journal entry; the next startup retries as the owner.
        m_oss.Log().Emsg("Posc", rc, "remove temporary file", m_tmp.c_str());
    } else {
        if (Trace::On(Trace::Info)) {m_oss.Log().Emsg("Posc", "Removed incomplete file for", m_path.c_str());}
        m_oss.JournalDel(m_tmp);
    }
    m_tmp.clear();
}

int PoscFile::Close(long long *retsz)
{
    if (m_tmp.empty()) {return wrapDF.Close(retsz);}
    std::unique_ptr<OwnerIDs> ids;
    AsOpener(ids);

    // A transfer that stopped short of the announced size is incomplete even
    // if the close succeeds.  Without oss.asize (chunked PUT, xrdcp from a
    // pipe) the close is the only completion signal this layer gets, so such
    // a file is published as is; only a server stop is covered then.
    bool short_file = false;
    if (m_asize >= 0) {
        struct stat st;
        if (wrapDF.Fstat(&st) == 0 && st.st_size != m_asize) {
            short_file = true;
            std::ostringstream ss;
            ss << "Incomplete file (" << st.st_size << " of " << m_asize << " bytes); not persisting";
            m_oss.Log().Emsg("Posc", ss.str().c_str(), m_path.c_str());
        }
    }
    int rc = wrapDF.Close(retsz);
    if (rc == 0 && short_file) {
        rc = -EIO;
    } else if (rc == 0) {
        if (m_oss.Finish(m_path, m_tmp)) {
            m_oss.Log().Emsg("Posc", "Final name was removed during the upload; not persisting", m_path.c_str());
            rc = -ENOENT;
        } else if ((rc = Publish())) {
            m_oss.Log().Emsg("Posc", rc, "publish temporary file as", m_path.c_str());
        }
    }
    if (rc == 0) {
        m_oss.JournalDel(m_tmp);
        m_tmp.clear();
    } else {
        Abandon();
    }
    return rc;
}

}  // namespace

/******************************************************************************/
/*                          P l u g i n   E n t r y                           */
/******************************************************************************/

extern "C" {

XrdOss *XrdOssAddStorageSystem2(XrdOss *curr_oss, XrdSysLogger *logger,
                                const char *config_fn, const char *parms, XrdOucEnv *envP)
{
    XrdSysError log(logger, "mposc_");
    if (!curr_oss) {
        log.Emsg("Initialize", "libXrdMultiuserPosc must be stacked on another OSS (ofs.osslib ++)");
        return nullptr;
    }
    std::unique_ptr<PoscOss> oss(new PoscOss(*curr_oss, logger));
    if (!oss->Config(config_fn)) {
        log.Emsg("Initialize", "configuration failed");
        return nullptr;
    }
    return oss.release();
}

}

XrdVERSIONINFO(XrdOssAddStorageSystem2, XrdMultiuserPosc);
