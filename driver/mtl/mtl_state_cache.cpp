#include "mtl_state_cache.h"
#include "mtl_context.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "../util/log/log.h"
#include "../util/util_likely.h"
#include "../util/util_string.h"

#ifdef _WIN32
#include "../util/util_env.h"
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctime>
#include <thread>
#endif

namespace dxvk {

  using namespace statecache;

  namespace {

    constexpr auto     WriteDebounce     = std::chrono::seconds(20);
    constexpr auto     WriteMaxInterval  = std::chrono::seconds(300);
    constexpr auto     LogInterval       = std::chrono::seconds(60);
    constexpr size_t   MaxFileBytes      = 64u << 20;    // anything bigger is not ours
    constexpr uint32_t SecondThreadMinReady = 32u;

    // Temp file names: unique per write even with two devices in one process.
    std::atomic<uint32_t> g_tmpSeq = { 0u };

    std::string getEnv(const char* name) {
#ifdef _WIN32
      return env::getEnvVar(name);
#else
      const char* v = std::getenv(name);
      return v ? std::string(v) : std::string();
#endif
    }

    void sleepMs(uint32_t ms) {
#ifdef _WIN32
      ::Sleep(ms);
#else
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
    }

    /// A decimal number, all of it; false for empty or anything else.
    bool parseNumber(const std::string& s, unsigned long& out) {
      if (s.empty() || s.size() > 9u)
        return false;
      for (char c : s) {
        if (c < '0' || c > '9')
          return false;
      }
      out = std::strtoul(s.c_str(), nullptr, 10);
      return true;
    }

    std::string sanitizeName(const std::string& s) {
      std::string out = s;
      for (char& c : out) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                     || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_';
        if (!ok)
          c = '_';
      }
      return out.empty() ? std::string("game") : out;
    }

    /// Collapses repeated slashes and drops a trailing one.
    std::string normalizeUnixPath(std::string p) {
      std::string out;
      for (char c : p) {
        if (c == '/' && !out.empty() && out.back() == '/')
          continue;
        out.push_back(c);
      }
      while (out.size() > 1u && out.back() == '/')
        out.pop_back();
      return out;
    }

    bool endsWith(const std::string& s, const char* suffix) {
      const size_t n = std::strlen(suffix);
      return s.size() >= n && !s.compare(s.size() - n, n, suffix);
    }

    std::string parentOf(const std::string& path) {
      const size_t n = path.find_last_of("/\\");
      return n == std::string::npos ? std::string() : path.substr(0u, n);
    }

    std::string leafOf(const std::string& path) {
      const size_t n = path.find_last_of("/\\");
      return n == std::string::npos ? path : path.substr(n + 1u);
    }

#ifdef _WIN32

    std::wstring widen(const std::string& s) {
      if (s.empty())
        return std::wstring();
      const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
      std::wstring w(size_t(n), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
      return w;
    }

    std::string narrow(const wchar_t* w) {
      const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
      if (n <= 1)
        return std::string();
      std::string s(size_t(n - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
      return s;
    }

    /// A Unix path as a Windows path: Wine's own conversion if available
    /// (a drive path such as Z:\..., or \\?\unix\...), else \\?\unix\....
    std::string unixToPlatform(const std::string& unixPath) {
      using PFN = WCHAR* (CDECL*)(LPCSTR);
      static PFN s_toDos = reinterpret_cast<PFN>(reinterpret_cast<void*>(
        ::GetProcAddress(::GetModuleHandleA("kernel32.dll"), "wine_get_dos_file_name")));

      if (s_toDos) {
        if (WCHAR* w = s_toDos(unixPath.c_str())) {
          std::string s = narrow(w);
          ::HeapFree(::GetProcessHeap(), 0, w);
          if (!s.empty())
            return s;
        }
      }

      std::string s = "\\\\?\\unix" + unixPath;
      std::replace(s.begin(), s.end(), '/', '\\');
      return s;
    }

    /// The Unix home directory: Wine sets WINEHOMEDIR to its NT path,
    /// "\??\Z:\Users\<name>" (a drive) or "\??\unix\Users\<name>".
    std::string unixHome() {
      std::string h = getEnv("WINEHOMEDIR");

      if (!h.compare(0u, 4u, "\\??\\"))
        h = h.substr(4u);

      if (h.empty())
        return std::string();

      if (!h.compare(0u, 5u, "unix\\")) {
        std::string u = h.substr(4u);
        std::replace(u.begin(), u.end(), '\\', '/');
        return u;
      }

      using PFN = char* (CDECL*)(LPCWSTR);
      static PFN s_toUnix = reinterpret_cast<PFN>(reinterpret_cast<void*>(
        ::GetProcAddress(::GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name")));

      if (!s_toUnix)
        return std::string();

      std::string u;
      if (char* p = s_toUnix(widen(h).c_str())) {
        u = p;
        ::HeapFree(::GetProcessHeap(), 0, p);
      }
      return u;
    }

    bool dirExists(const std::string& path) {
      const DWORD a = ::GetFileAttributesW(widen(path).c_str());
      return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool makeDirs(const std::string& path) {
      if (dirExists(path))
        return true;

      // Skip the root: "\\?\unix\", "X:\" or "\\server\share\".
      size_t start = 0u;
      if (!path.compare(0u, 9u, "\\\\?\\unix\\"))
        start = 9u;
      else if (path.size() >= 3u && path[1] == ':')
        start = 3u;

      for (size_t i = start; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '\\' || path[i] == '/') {
          const std::string part = path.substr(0u, i);
          if (part.size() > start && !dirExists(part))
            ::CreateDirectoryW(widen(part).c_str(), nullptr);
        }
      }

      return dirExists(path);
    }

    bool readAll(const std::string& path, std::vector<uint8_t>& out) {
      out.clear();
      HANDLE h = ::CreateFileW(widen(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE)
        return false;

      LARGE_INTEGER size = { };
      bool ok = ::GetFileSizeEx(h, &size) && uint64_t(size.QuadPart) <= MaxFileBytes;

      if (ok) {
        out.resize(size_t(size.QuadPart));
        size_t done = 0u;
        while (ok && done < out.size()) {
          DWORD n = 0u;
          ok = ::ReadFile(h, out.data() + done, DWORD(out.size() - done), &n, nullptr) && n;
          done += n;
        }
      }

      ::CloseHandle(h);
      if (!ok)
        out.clear();
      return ok;
    }

    bool writeAtomic(const std::string& path, const std::vector<uint8_t>& data) {
      const std::string tmp = path + ".tmp-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(++g_tmpSeq);
      const std::wstring wtmp = widen(tmp);

      HANDLE h = ::CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE)
        return false;

      bool ok = true;
      size_t done = 0u;
      while (ok && done < data.size()) {
        DWORD n = 0u;
        ok = ::WriteFile(h, data.data() + done, DWORD(data.size() - done), &n, nullptr) && n;
        done += n;
      }

      ok = ok && ::FlushFileBuffers(h);
      ::CloseHandle(h);

      ok = ok && ::MoveFileExW(wtmp.c_str(), widen(path).c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

      if (!ok)
        ::DeleteFileW(wtmp.c_str());
      return ok;
    }

    /// Deletes <file>.tmp-* left by interrupted writes, if older than 10 minutes.
    void removeStaleTemps(const std::string& file) {
      const std::string dir = parentOf(file);
      WIN32_FIND_DATAW fd;
      HANDLE f = ::FindFirstFileW(widen(file + ".tmp-*").c_str(), &fd);
      if (f == INVALID_HANDLE_VALUE)
        return;

      FILETIME nowFt;
      ::GetSystemTimeAsFileTime(&nowFt);
      const uint64_t now = (uint64_t(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;

      do {
        const uint64_t t = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (now > t && now - t > 600ull * 10000000ull)
          ::DeleteFileW(widen(dir + "\\" + narrow(fd.cFileName)).c_str());
      } while (::FindNextFileW(f, &fd));

      ::FindClose(f);
    }

    std::string exeName() {
      return env::getExeName();
    }

    bool fileExists(const std::string& path) {
      const DWORD a = ::GetFileAttributesW(widen(path).c_str());
      return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    void removeFile(const std::string& path) {
      ::DeleteFileW(widen(path).c_str());
    }

    /// The guard's small files (marker, trip count), replaced atomically:
    /// written to a temp file, then renamed over the old one, so a crash
    /// mid-write leaves the old file, never a torn one. No flush: they only
    /// have to survive the process, not the OS (the page cache keeps them).
    bool writeSmallFileAtomic(const std::string& path, const std::string& text) {
      const std::string tmp = path + ".tmp-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(++g_tmpSeq);
      const std::wstring wtmp = widen(tmp);
      HANDLE h = ::CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE)
        return false;
      DWORD n = 0u;
      bool ok = ::WriteFile(h, text.data(), DWORD(text.size()), &n, nullptr) && n == text.size();
      ::CloseHandle(h);
      ok = ok && ::MoveFileExW(wtmp.c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING);
      if (!ok)
        ::DeleteFileW(wtmp.c_str());
      return ok;
    }

    /// Opens the journal for appends (docs/state-cache-journal.md §3). Our header: Ready.
    /// Never truncates a file with our header, because another process (a second client) may
    /// be appending; a torn tail stays and the parser resyncs past it. Missing: created
    /// exclusively with the header. Shorter than the header: another process is creating it,
    /// so wait once; still short (its creator died) or foreign (bad header, older format,
    /// another key signature): removed and created anew. A newer d9mt's: Newer, untouched.
    statecache::JournalOpen openJournalFile(const std::string& path, const std::vector<uint8_t>& header) {
      using statecache::JournalOpen;
      for (int attempt = 0; attempt < 2; attempt++) {
        HANDLE h = ::CreateFileW(widen(path).c_str(), GENERIC_READ,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
          std::vector<uint8_t> head(header.size(), uint8_t(0u));
          DWORD n = 0u;
          const bool read = ::ReadFile(h, head.data(), DWORD(head.size()), &n, nullptr);
          ::CloseHandle(h);
          if (!read)
            return JournalOpen::Failed;
          if (n == head.size() && head == header)
            return JournalOpen::Ready;
          if (n >= 12u && !std::memcmp(head.data(), header.data(), 8u)
           && *reinterpret_cast<const uint32_t*>(head.data() + 8u) > statecache::JournalVersion)
            return JournalOpen::Newer;
          if (n < head.size() && !attempt) {
            ::Sleep(5);
            continue;
          }
          ::DeleteFileW(widen(path).c_str());
        } else if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
          return JournalOpen::Failed;
        }

        h = ::CreateFileW(widen(path).c_str(), GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
          if (::GetLastError() == ERROR_FILE_EXISTS)
            continue;   // another process created it first: read its header
          return JournalOpen::Failed;
        }
        DWORD n = 0u;
        const bool ok = ::WriteFile(h, header.data(), DWORD(header.size()), &n, nullptr) && n == header.size();
        ::CloseHandle(h);
        return ok ? JournalOpen::Ready : JournalOpen::Failed;
      }
      return JournalOpen::Busy;
    }

    /// The journal lock: an exclusive lock on one byte far past the end (JournalLockOffset),
    /// held around each append and around write()'s re-read and removal. Without it two
    /// clients' appends can land at the same end: Wine writes a FILE_APPEND_DATA handle at an
    /// end it read before, so one batch overwrote the other (n013-perf journal-wine: 0.8% of
    /// the records lost across two wineservers, 12% in one, 0 with the lock in both). Never
    /// waits long: up to JournalLockWaitMs of 1 ms retries, then Busy (the caller keeps the
    /// records for the next intake). Gone if the file is gone or was removed while we waited
    /// (another process's write): the caller opens it again, so records never land in a
    /// journal that is about to disappear or has no header.
    using JournalFd = HANDLE;
    const JournalFd NoJournalFd = INVALID_HANDLE_VALUE;

    OVERLAPPED journalLockRange() {
      OVERLAPPED ov = { };
      ov.Offset     = DWORD(statecache::JournalLockOffset);
      ov.OffsetHigh = DWORD(statecache::JournalLockOffset >> 32);
      return ov;
    }

    void unlockJournal(JournalFd h) {
      OVERLAPPED ov = journalLockRange();
      ::UnlockFileEx(h, 0u, 1u, 0u, &ov);
      ::CloseHandle(h);
    }

    statecache::JournalAppend lockJournal(const std::string& path, JournalFd& out) {
      using statecache::JournalAppend;
      out = NoJournalFd;
      HANDLE h = ::CreateFileW(widen(path).c_str(), GENERIC_READ | FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE)
        return ::GetLastError() == ERROR_FILE_NOT_FOUND ? JournalAppend::Gone : JournalAppend::Failed;

      OVERLAPPED ov = journalLockRange();
      const ULONGLONG deadline = ::GetTickCount64() + statecache::JournalLockWaitMs;
      bool locked = false;
      while (!(locked = ::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0u, 1u, 0u, &ov))) {
        const DWORD err = ::GetLastError();
        if ((err != ERROR_LOCK_VIOLATION && err != ERROR_IO_PENDING) || ::GetTickCount64() >= deadline)
          break;
        ::Sleep(1);
      }
      if (!locked) {
        ::CloseHandle(h);
        return JournalAppend::Busy;
      }

      // Removed while we waited for the lock (write()'s removal holds it): append to the new one.
      FILE_STANDARD_INFO si = { };
      if (::GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof(si)) && (si.DeletePending || !si.NumberOfLinks)) {
        unlockJournal(h);
        return JournalAppend::Gone;
      }
      out = h;
      return JournalAppend::Ok;
    }

    bool writeLocked(JournalFd h, const std::vector<uint8_t>& data) {
      DWORD n = 0u;
      return ::WriteFile(h, data.data(), DWORD(data.size()), &n, nullptr) && n == data.size();
    }

    void* openCurrentThread() {
      return ::OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, ::GetCurrentThreadId());
    }

    void setThreadPriorityNormal(void* handle) {
      if (handle)
        ::SetThreadPriority(HANDLE(handle), THREAD_PRIORITY_NORMAL);
    }

    void setCurrentThreadPriorityLowest() {
      ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    }

    void closeThreadHandle(void* handle) {
      if (handle)
        ::CloseHandle(HANDLE(handle));
    }

    void nameThread(const char* name) {
      env::setThreadName(name);
    }

#else

    std::string unixToPlatform(const std::string& unixPath) {
      return unixPath;
    }

    std::string unixHome() {
      return getEnv("HOME");
    }

    bool dirExists(const std::string& path) {
      struct stat st;
      return !::stat(path.c_str(), &st) && S_ISDIR(st.st_mode);
    }

    bool makeDirs(const std::string& path) {
      for (size_t i = 1u; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/')
          ::mkdir(path.substr(0u, i).c_str(), 0755);
      }
      return dirExists(path);
    }

    bool readAll(const std::string& path, std::vector<uint8_t>& out) {
      out.clear();
      FILE* f = std::fopen(path.c_str(), "rb");
      if (!f)
        return false;

      bool ok = !std::fseek(f, 0, SEEK_END);
      const long size = ok ? std::ftell(f) : -1;
      ok = ok && size >= 0 && size_t(size) <= MaxFileBytes && !std::fseek(f, 0, SEEK_SET);

      if (ok) {
        out.resize(size_t(size));
        ok = std::fread(out.data(), 1u, out.size(), f) == out.size();
      }

      std::fclose(f);
      if (!ok)
        out.clear();
      return ok;
    }

    bool writeAtomic(const std::string& path, const std::vector<uint8_t>& data) {
      const std::string tmp = path + ".tmp-" + std::to_string(::getpid()) + "-" + std::to_string(++g_tmpSeq);
      const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0)
        return false;

      bool ok = true;
      size_t done = 0u;
      while (ok && done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        ok = n > 0;
        done += ok ? size_t(n) : 0u;
      }

      ok = ok && !::fsync(fd);
      ok = !::close(fd) && ok;
      ok = ok && !::rename(tmp.c_str(), path.c_str());

      if (!ok)
        ::unlink(tmp.c_str());
      return ok;
    }

    void removeStaleTemps(const std::string& file) {
      const std::string dir = parentOf(file);
      const std::string prefix = leafOf(file) + ".tmp-";
      DIR* d = ::opendir(dir.c_str());
      if (!d)
        return;

      const time_t now = std::time(nullptr);
      while (dirent* e = ::readdir(d)) {
        if (std::strncmp(e->d_name, prefix.c_str(), prefix.size()))
          continue;
        const std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (!::stat(p.c_str(), &st) && now - st.st_mtime > 600)
          ::unlink(p.c_str());
      }

      ::closedir(d);
    }

    std::string exeName() {
      const std::string v = getEnv("D9MT_STATE_CACHE_EXE");
      return v.empty() ? std::string("native-test") : v;
    }

    bool fileExists(const std::string& path) {
      struct stat st;
      return !::stat(path.c_str(), &st) && S_ISREG(st.st_mode);
    }

    void removeFile(const std::string& path) {
      ::unlink(path.c_str());
    }

    bool writeSmallFileAtomic(const std::string& path, const std::string& text) {
      const std::string tmp = path + ".tmp-" + std::to_string(::getpid()) + "-" + std::to_string(++g_tmpSeq);
      FILE* f = std::fopen(tmp.c_str(), "wb");
      if (!f)
        return false;
      bool ok = std::fwrite(text.data(), 1u, text.size(), f) == text.size();
      ok = !std::fclose(f) && ok;
      ok = ok && !::rename(tmp.c_str(), path.c_str());
      if (!ok)
        ::unlink(tmp.c_str());
      return ok;
    }

    // As the Win32 openJournalFile.
    statecache::JournalOpen openJournalFile(const std::string& path, const std::vector<uint8_t>& header) {
      using statecache::JournalOpen;
      for (int attempt = 0; attempt < 2; attempt++) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd >= 0) {
          std::vector<uint8_t> head(header.size(), uint8_t(0u));
          const ssize_t n = ::pread(fd, head.data(), head.size(), 0);
          ::close(fd);
          if (n < 0)
            return JournalOpen::Failed;
          if (size_t(n) == head.size() && head == header)
            return JournalOpen::Ready;
          if (n >= 12 && !std::memcmp(head.data(), header.data(), 8u)
           && *reinterpret_cast<const uint32_t*>(head.data() + 8u) > statecache::JournalVersion)
            return JournalOpen::Newer;
          if (size_t(n) < head.size() && !attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
          }
          ::unlink(path.c_str());
        } else if (errno != ENOENT) {
          return JournalOpen::Failed;
        }

        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
          if (errno == EEXIST)
            continue;   // another process created it first: read its header
          return JournalOpen::Failed;
        }
        bool ok = ::write(fd, header.data(), header.size()) == ssize_t(header.size());
        ok = !::close(fd) && ok;
        return ok ? JournalOpen::Ready : JournalOpen::Failed;
      }
      return JournalOpen::Busy;
    }

    // As the Win32 journal lock, with flock (a native O_APPEND write is atomic anyway; the lock
    // keeps the two paths' rules the same, Busy included). flock locks don't meet Wine's
    // LockFileEx locks (fcntl ranges): only a native test next to a Wine process would see two
    // unrelated locks, never the driver (one build is either Win32 or native).
    using JournalFd = int;
    const JournalFd NoJournalFd = -1;

    void unlockJournal(JournalFd fd) {
      ::flock(fd, LOCK_UN);
      ::close(fd);
    }

    statecache::JournalAppend lockJournal(const std::string& path, JournalFd& out) {
      using statecache::JournalAppend;
      out = NoJournalFd;
      const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
      if (fd < 0)
        return errno == ENOENT ? JournalAppend::Gone : JournalAppend::Failed;

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(statecache::JournalLockWaitMs);
      bool locked = false;
      while (!(locked = !::flock(fd, LOCK_EX | LOCK_NB))) {
        if (errno != EWOULDBLOCK || std::chrono::steady_clock::now() >= deadline)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!locked) {
        ::close(fd);
        return JournalAppend::Busy;
      }

      struct stat st;
      if (!::fstat(fd, &st) && !st.st_nlink) {
        unlockJournal(fd);
        return JournalAppend::Gone;
      }
      out = fd;
      return JournalAppend::Ok;
    }

    bool writeLocked(JournalFd fd, const std::vector<uint8_t>& data) {
      return ::write(fd, data.data(), data.size()) == ssize_t(data.size());
    }

    // Thread priorities are Win32 (Wine) only; natively the boost is counted.
    void* openCurrentThread() { return nullptr; }
    void setThreadPriorityNormal(void*) { }
    void setCurrentThreadPriorityLowest() { }
    void closeThreadHandle(void*) { }

    void nameThread(const char*) { }

#endif

    /// One append under the journal lock (see lockJournal).
    statecache::JournalAppend appendFile(const std::string& path, const std::vector<uint8_t>& data) {
      JournalFd fd = NoJournalFd;
      const statecache::JournalAppend a = lockJournal(path, fd);
      if (a != statecache::JournalAppend::Ok)
        return a;
      const bool ok = writeLocked(fd, data);
      unlockJournal(fd);
      return ok ? statecache::JournalAppend::Ok : statecache::JournalAppend::Failed;
    }

    const char* statusName(ParseStatus s) {
      switch (s) {
        case ParseStatus::Ok:           return "ok";
        case ParseStatus::Empty:        return "new";
        case ParseStatus::BadHeader:    return "bad-header";
        case ParseStatus::OlderVersion: return "older-version";
        case ParseStatus::NewerVersion: return "newer-version";
        case ParseStatus::KeyMismatch:  return "key-changed";
      }
      return "?";
    }

    /// The guard marker: "d9mt-prewarm 1", "trips <n>", then one identity
    /// hash (16 hex digits) per job that was in flight. Anything unreadable
    /// counts as a marker with no suspects.
    void parseMarker(const std::vector<uint8_t>& data, uint32_t& trips, std::unordered_set<uint64_t>& suspects) {
      const std::string text(data.begin(), data.end());
      size_t pos = 0u;
      while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
          end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1u;

        if (!line.compare(0u, 6u, "trips ")) {
          trips = uint32_t(std::min<unsigned long>(std::strtoul(line.c_str() + 6, nullptr, 10), 1000u));
        } else if (line.size() == 16u && line.find_first_not_of("0123456789abcdef") == std::string::npos) {
          const uint64_t h = std::strtoull(line.c_str(), nullptr, 16);
          if (h)
            suspects.insert(h);
        }
      }
    }

  }


  bool MtlStateCache::resolveConfig(MtlStateCacheConfig& config, std::string& reason) {
    const std::string override = getEnv("D9MT_STATE_CACHE");

    if (override == "0") {
      reason = "off (D9MT_STATE_CACHE=0)";
      return false;
    }

    // Numbers: anything that isn't one leaves the default.
    unsigned long n = 0u;

    if (parseNumber(getEnv("D9MT_STATE_CACHE_THREADS"), n))
      config.threads = uint32_t(std::min<unsigned long>(n, MaxPrewarmThreads));

    if (parseNumber(getEnv("D9MT_STATE_CACHE_MAX"), n) && n)
      config.maxEntries = uint32_t(std::clamp<unsigned long>(n, MinMaxEntries, MaxMaxEntries));

#ifdef D9MT_STATE_CACHE_TESTING
    // Test builds only (STATE_CACHE_TESTING=1 scripts/build-driver.sh).
    config.noValidate = getEnv("D9MT_STATE_CACHE_TEST_NO_VALIDATE") == "1";
#endif

    const std::string fileName = sanitizeName(exeName()) + ".d9mtstate";
    std::string unixPath;

    if (!override.empty()) {
      const bool unixAbsolute = override[0] == '/';
      const bool winAbsolute = (override.size() >= 3u && std::isalpha(uint8_t(override[0]))
                                && override[1] == ':' && (override[2] == '\\' || override[2] == '/'))
                            || !override.compare(0u, 2u, "\\\\");

      if (winAbsolute) {
        // A Windows path, used as is.
        config.file = endsWith(override, ".d9mtstate")
          ? override : override + "\\" + fileName;
        config.displayPath = config.file;
        return true;
      }

      if (unixAbsolute)
        unixPath = normalizeUnixPath(override);
      else
        Logger::warn(str::format("STATECACHE ignoring D9MT_STATE_CACHE=", override,
          ": not an absolute path; using the default location"));
    }

    if (unixPath.empty()) {
      // 1. Next to the AIR cache and PSO archives, the macOS user cache dir
      //    (confstr _CS_DARWIN_USER_CACHE_DIR): launchd's TMPDIR is
      //    /var/folders/<a>/<b>/T/ and that dir is its sibling C/. Only if
      //    the Windows side sees TMPDIR (CrossOver's Wine doesn't pass it).
      // 2. ~/Library/Caches/d9mt/state, from Wine's WINEHOMEDIR.
      const std::string tmp = normalizeUnixPath(getEnv("TMPDIR"));

      if (tmp.find("/var/folders/") != std::string::npos && endsWith(tmp, "/T")) {
        const std::string cacheDir = tmp.substr(0u, tmp.size() - 1u) + "C";
        if (dirExists(unixToPlatform(cacheDir + "/d9mt")))
          unixPath = cacheDir + "/d9mt/state";
      }

      if (unixPath.empty()) {
        const std::string home = normalizeUnixPath(unixHome());
        if (home.size() > 1u && home[0] == '/' && dirExists(unixToPlatform(home + "/Library/Caches")))
          unixPath = home + "/Library/Caches/d9mt/state";
      }

      if (unixPath.empty()) {
        reason = "off (no cache dir: no usable TMPDIR or ~/Library/Caches; set D9MT_STATE_CACHE)";
        return false;
      }
    }

    if (!endsWith(unixPath, ".d9mtstate"))
      unixPath += "/" + fileName;

    config.displayPath = unixPath;
    config.file = unixToPlatform(unixPath);
    return true;
  }


  MtlStateCache::MtlStateCache(MtlStateCacheBackend* pBackend, const MtlStateCacheConfig& config)
  : m_backend(pBackend), m_config(config) {
    m_config.threads = std::min(m_config.threads, MaxPrewarmThreads);

    m_writer = dxvk::thread([this] { writerMain(); });
    m_writer.set_priority(ThreadPriority::Lowest);

    for (uint32_t i = 0u; i < m_config.threads; i++) {
      m_prewarmThreads.emplace_back([this, i] { prewarmMain(i); });
      m_prewarmThreads.back().set_priority(ThreadPriority::Lowest);
    }
  }


  MtlStateCache::~MtlStateCache() {
    shutdown();

    if (!this_thread::isInModuleDetachment()) {
      for (PrewarmSlot& slot : m_slots)
        closeThreadHandle(std::exchange(slot.handle, nullptr));
    }
  }


  void MtlStateCache::boostPrewarm(MtlCompileTask* pPso) {
    if (m_stopped.load(std::memory_order_relaxed))
      return;

    for (uint32_t i = 0u; i < m_config.threads; i++) {
      PrewarmSlot& slot = m_slots[i];

      if (slot.running.load(std::memory_order_acquire) == pPso && !slot.boosted.exchange(true)) {
        // The thread resets itself to the lowest priority after the build.
        // If it finished just now, it runs (at most) its next build at
        // normal priority before it resets.
        setThreadPriorityNormal(slot.handle);
        m_boosts.fetch_add(1u, std::memory_order_relaxed);
      }
    }
  }


  void MtlStateCache::announce(
          MtlShaderStage        stage,
    const MtlShaderStateId&     id,
          MtlStateCacheShader*  pShader) {
    if (!id.origin || !pShader || m_stopped.load(std::memory_order_relaxed))
      return;

    ShaderRef ref;
    ref.stage  = stage == MtlShaderStage::Vertex ? 0u : 1u;
    ref.origin = id.origin;
    std::memcpy(ref.sha, id.sha, sizeof(ref.sha));

    uint32_t ready = 0u;

    { std::lock_guard lock(m_mutex);
      if (m_stopped.load(std::memory_order_relaxed))
        return;

      ShaderSlot& slot = m_shaders[ref];
      if (slot.shader)
        return;

      slot.shader = pShader;

      if (m_indexBuilt)
        ready = m_index.announce(ref);
    }

    if (ready)
      m_prewarmCond.notify_all();
  }


  void MtlStateCache::record(
    const uint8_t*            pState,
    const MtlShaderFunction*  pVs,
    const MtlShaderFunction*  pFs,
          MtlCompileTask*     pPso,
          MtlPsoUse           use) {
    if (unlikely(m_stopped.load(std::memory_order_relaxed)))
      return;

    if (!pVs || !pVs->stateId.origin || (pFs && !pFs->stateId.origin) || !pPso) {
      m_unrecorded.fetch_add(1u, std::memory_order_relaxed);
      return;
    }

    switch (use) {
      case MtlPsoUse::Miss: m_misses.fetch_add(1u, std::memory_order_relaxed); break;
      case MtlPsoUse::Hit:  m_hits.fetch_add(1u, std::memory_order_relaxed);   break;
      case MtlPsoUse::Late: m_late.fetch_add(1u, std::memory_order_relaxed);   break;
    }

    Record r;
    r.entry.vsOrigin = pVs->stateId.origin;
    std::memcpy(r.entry.vsSha, pVs->stateId.sha, sizeof(r.entry.vsSha));

    if (pFs) {
      r.entry.psOrigin = pFs->stateId.origin;
      r.entry.psLinked = pFs->stateId.linked;
      std::memcpy(r.entry.psSha, pFs->stateId.sha, sizeof(r.entry.psSha));
    }

    std::memcpy(r.entry.key, pState, KeyBytes);

    const uint64_t ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - m_t0).count());
    r.entry.firstUseMs = uint32_t(std::min<uint64_t>(ms, ~0u));

    r.pso = pPso;
    r.use = uint8_t(use);

    if (!m_ring.push(r))
      m_ringDrops.fetch_add(1u, std::memory_order_relaxed);
  }


  void MtlStateCache::shutdown() {
    if (m_stopped.exchange(true))
      return;

    // Process exit (ExitProcess has already killed every other thread, which
    // may have died holding m_mutex or the writer's lock): no join, no lock,
    // no IO. The threads are detached so their objects can go; the marker
    // stays, which costs at most the in-flight entries next session.
    if (unlikely(this_thread::isInModuleDetachment())) {
      m_stop.store(true);
      for (auto& t : m_prewarmThreads)
        t.detach();
      if (m_writer.joinable())
        m_writer.detach();
      return;
    }

    // Prewarm first, so the writer sees every failure; then the writer,
    // which drains the ring and writes the file once more.
    { std::lock_guard lock(m_mutex);
      m_stop.store(true);
    }
    m_prewarmCond.notify_all();

    for (auto& t : m_prewarmThreads)
      t.join();
    m_prewarmThreads.clear();

    // A clean shutdown: whatever the prewarm did, it didn't crash us.
    { std::lock_guard lock(m_markerMutex);
      if (m_markerOnDisk) {
        removeFile(markerPath());
        m_markerOnDisk = false;
      }
    }

    { std::lock_guard lock(m_writerMutex);
      m_stopWriter = true;
    }
    m_writerCond.notify_all();

    if (m_writer.joinable())
      m_writer.join();

    // Drop every shader pointer and pipeline reference while the device
    // and its compiler are still alive.
    Record r;
    while (m_ring.pop(r))
      r.pso = nullptr;

    m_pendingRecords.clear();

    std::lock_guard lock(m_mutex);
    m_shaders.clear();
    m_index = PrewarmIndex();
  }


  bool MtlStateCache::waitPrewarmIdle(uint32_t timeoutMs) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock lock(m_mutex);

    while (!m_stop.load() && !(m_indexBuilt && !m_index.readyCount() && !m_activeJobs)) {
      const auto now = Clock::now();
      if (now >= deadline)
        return false;
      m_prewarmCond.wait_for(lock, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
        + std::chrono::milliseconds(1));
    }

    return true;
  }


  MtlStateCache::Stats MtlStateCache::stats() {
    Stats s;
    s.hits       = m_hits.load();
    s.late       = m_late.load();
    s.misses     = m_misses.load();
    s.unrecorded = m_unrecorded.load();
    s.ringDrops  = m_ringDrops.load();
    s.recorded   = m_recorded.load();
    s.built      = m_built.load();
    s.dup        = m_dup.load();
    s.failed     = m_failedCount.load();
    s.skipped    = m_skipped.load();
    s.writes     = m_writes.load();
    s.entries    = m_entries.load();
    s.invalid    = m_invalid.load();
    s.boosts     = m_boosts.load();
    s.guardTripped = m_guardTripped.load();
    s.suspects     = m_suspects.load();
    s.trips        = m_loadTrips.load();
    s.fileDropped  = m_fileDropped.load();
    s.prewarmOff   = m_prewarmOff.load();
    s.journaled       = m_journalRecords.load();
    s.journalReplayed = m_journalReplayed.load();
    return s;
  }


  void MtlStateCache::writerMain() {
    nameThread("d9mt-statecache");
    load();

    while (true) {
      { std::unique_lock lock(m_writerMutex);
        // 250 ms normally; 20 ms right after a big intake, so a burst of
        // misses doesn't overflow the record ring.
        const auto poll = std::chrono::milliseconds(m_lastIntake >= 256u ? 20 : 250);
        m_writerCond.wait_for(lock, poll, [this] { return m_stopWriter; });

        if (m_stopWriter)
          break;
      }

      intake(false);

      const auto now = Clock::now();

      if (m_dirty && !m_readOnly
       && (now - m_lastChange >= WriteDebounce || now - m_lastWrite >= WriteMaxInterval))
        write();

      if (now - m_lastLog >= LogInterval)
        logUse(false);
    }

    intake(true);

    if (m_dirty && !m_readOnly)
      write();

    logUse(true);
  }


  void MtlStateCache::load() {
    const auto t0 = Clock::now();
    const std::string& file = m_config.file;

    makeDirs(parentOf(file));
    removeStaleTemps(file);
    removeStaleTemps(markerPath());
    removeStaleTemps(tripsPath());

    // Crash-loop guard: the marker outlived the last session, which ended
    // while prewarm jobs were running: a crash, a kill, or simply an exit
    // without releasing the device (SWTOR quits that way). Only the entries
    // that were in flight are suspects: they are dropped (a draw records
    // them again if they are fine), everything else is kept.
    std::unordered_set<uint64_t> suspects;

    // The trip count survives sessions without prewarm (they can't trip):
    // only a drained prewarm queue resets it.
    { std::vector<uint8_t> trips;
      std::unordered_set<uint64_t> none;
      if (readAll(tripsPath(), trips))
        parseMarker(trips, m_trips, none);
    }

    if (fileExists(markerPath())) {
      std::vector<uint8_t> marker;
      uint32_t markerTrips = 0u;
      readAll(markerPath(), marker);
      parseMarker(marker, markerTrips, suspects);
      m_trips = std::max(m_trips, markerTrips) + 1u;
      m_guardTripped.store(true);
      writeSmallFileAtomic(tripsPath(), str::format("trips ", m_trips, "\n"));
      removeFile(markerPath());

      // Persistent trips the suspects don't explain (a crash on another
      // thread, a torn marker): give up on this file. Nothing left to
      // prewarm means nothing left to trip on; this session records afresh.
      if (m_trips >= GuardDropTrips) {
        m_fileDropped.store(true);
        suspects.clear();
        removeFile(file);
        removeFile(tripsPath());
      }
    }

    m_loadTrips.store(m_trips);

    std::vector<uint8_t> data;
    readAll(file, data);

    std::vector<Entry> entries;
    const ParseResult r = parse(data.data(), data.size(), m_config.keySignature, entries);
    uint32_t invalid = 0u;
    const char* invalidWhy = "";

    switch (r.status) {
      case ParseStatus::Ok:
        // Only what the draw path could have built is ever replayed.
        for (const Entry& e : entries) {
          if (!suspects.empty() && suspects.count(identityHash(e))) {
            m_suspects.fetch_add(1u, std::memory_order_relaxed);
            m_dropped.insert(identityHash(e));
            continue;
          }

          const char* why = nullptr;
          if (!m_config.noValidate && !m_backend->psoStateValid(e.key, &why)) {
            if (!invalid++)
              invalidWhy = why ? why : "?";
            m_dropped.insert(identityHash(e));   // and not merged back from disk
            continue;
          }
          m_db.merge(e);
        }
        m_invalid.store(invalid);
        m_dirty = r.corrupt || r.truncated || invalid || m_suspects.load();
        break;

      case ParseStatus::Empty:
        break;

      case ParseStatus::NewerVersion:
        m_readOnly = true;   // never overwrite a newer d9mt's cache
        break;

      case ParseStatus::BadHeader:
      case ParseStatus::OlderVersion:
      case ParseStatus::KeyMismatch:
        m_dirty = true;      // rewritten from this session's records
        break;
    }

    if (m_guardTripped.load()) {
      // A session without prewarm can't trip the guard: at GuardSkipTrips
      // in a row (a crash the suspects didn't explain, or a player who keeps
      // quitting during the prewarm), skip it once; the count carries on,
      // and at GuardDropTrips the file was dropped above.
      m_prewarmOff.store(m_trips == GuardSkipTrips);
      if (m_fileDropped.load()) {
        Logger::warn(str::format("STATECACHE ", m_trips, " sessions in a row ended while prewarm"
          " builds were running: dropped ", m_config.displayPath, "; recording afresh"));
      } else {
        Logger::warn(str::format("STATECACHE the last session ended while prewarm builds were running"
          " (crash, kill, or exit without releasing the device; ", m_trips, " in a row): dropped ",
          m_suspects.load(), " in-flight entries, kept the rest",
          m_prewarmOff.load() ? "; no prewarm this session" : ""));
      }
    }

    if (m_fileDropped.load())
      m_trips = 0u;

    // The journal: what a session that ended without writing recorded, through the same
    // filters as the file's entries (docs/state-cache-journal.md §4). No journal: one failed open.
    JournalParseResult jr;
    uint32_t journalInvalid = 0u;
    bool journalFound = false;
    if (m_readOnly) {
      m_journalOn = false;
    } else if (std::vector<uint8_t> jdata; readAll(journalPath(), jdata)) {
      journalFound = true;
      std::vector<Entry> jentries;
      jr = parseJournal(jdata.data(), jdata.size(), m_config.keySignature, jentries);
      if (jr.status == ParseStatus::Ok) {
        for (const Entry& e : jentries) {
          const uint64_t id = identityHash(e);
          if (m_fileDropped.load() || (!suspects.empty() && suspects.count(id)) || m_dropped.count(id)) {
            m_dropped.insert(id);
            continue;
          }
          if (!m_config.noValidate && !m_backend->psoStateValid(e.key, nullptr)) {
            journalInvalid++;
            m_dropped.insert(id);
            continue;
          }
          m_db.merge(e);
          m_journalReplayed.fetch_add(1u, std::memory_order_relaxed);
        }
        m_dirty |= !jentries.empty();
        m_invalid.fetch_add(journalInvalid);
      } else if (jr.status == ParseStatus::NewerVersion) {
        m_journalOn = false;          // a newer d9mt's: neither read nor touched
      } else {
        removeFile(journalPath());    // bad header, older format, another key layout: useless
      }
    }

    // A session that never wrote the file has the same number as this one would: count
    // past its journal records, so they don't count as used in this session.
    m_session = std::max(r.session, jr.maxSession) + 1u;
    const uint32_t capped = m_db.applyCap(m_config.maxEntries);
    m_dirty |= capped != 0u;
    m_entries.store(m_db.size());

    size_t indexed = 0u, ready = 0u;

    { std::lock_guard lock(m_mutex);
      m_index.build(m_prewarmOff.load() ? std::vector<Entry>() : m_db.entries(), m_session);

      for (const auto& s : m_shaders)
        m_index.announce(s.first);

      m_indexBuilt = true;
      indexed = m_index.indexedCount();
      ready   = m_index.readyCount();
    }

    m_loaded.store(true, std::memory_order_release);
    m_prewarmCond.notify_all();

    Logger::info(str::format("STATECACHE load path=", m_config.displayPath,
      " status=", statusName(r.status), " entries=", m_db.size(),
      " session=", m_session, " prewarmable=", indexed, " readyAtLoad=", ready,
      " corrupt=", r.corrupt, " truncated=", r.truncated, " invalid=", invalid,
      invalid ? str::format(" (first: ", invalidWhy, ")") : std::string(),
      " capped=", capped, " max=", m_config.maxEntries,
      m_guardTripped.load() ? " guard=tripped" : "",
      journalFound ? str::format(" journal=", statusName(jr.status), "/", jr.records, "/",
        m_journalReplayed.load(), " corrupt=", jr.corrupt, " torn=", jr.tornBytes,
        journalInvalid ? str::format(" invalid=", journalInvalid) : std::string()) : std::string(),
      " threads=", m_config.threads,
      " ms=", std::chrono::duration<double, std::milli>(Clock::now() - t0).count()));
  }


  bool MtlStateCache::ingest(Record& r) {
    if (!r.pso->isDone())
      return false;

    if (m_backend->psoSucceeded(r.pso.ptr())) {
      if (r.use == uint8_t(MtlPsoUse::Hit))
        m_hitBuildNs.fetch_add(m_backend->psoBuildNs(r.pso.ptr()), std::memory_order_relaxed);

      if (m_db.use(r.entry, m_session))
        m_recorded.fetch_add(1u, std::memory_order_relaxed);

      if (m_journalOn && m_journaled.insert(identityHash(r.entry)).second) {
        if (const Entry* stored = m_db.get(r.entry))
          m_journalPending.push_back(*stored);
      }

      m_dropped.erase(identityHash(r.entry));
      m_dirty = true;
      m_lastChange = Clock::now();
    }

    r.pso = nullptr;
    return true;
  }


  void MtlStateCache::intake(bool final) {
    for (size_t i = 0u; i < m_pendingRecords.size(); ) {
      if (ingest(m_pendingRecords[i])) {
        m_pendingRecords[i] = std::move(m_pendingRecords.back());
        m_pendingRecords.pop_back();
      } else {
        i++;
      }
    }

    Record r;
    m_lastIntake = 0u;

    while (m_ring.pop(r)) {
      m_lastIntake++;
      if (!ingest(r))
        m_pendingRecords.push_back(std::move(r));
    }

    // A PSO still building at exit isn't known to work: not recorded.
    if (final)
      m_pendingRecords.clear();

    std::vector<Entry> failed;
    { std::lock_guard lock(m_mutex);
      failed.swap(m_failed);
    }

    // A prewarm build that failed doesn't remove an entry a draw built (and
    // recorded) this session, whichever of the two arrived here first.
    for (const Entry& e : failed) {
      if (m_db.eraseUnlessUsedIn(e, m_session)) {
        m_dropped.insert(identityHash(e));
        m_dirty = true;
        m_lastChange = Clock::now();
      }
    }

    if (!m_journalPending.empty())
      appendJournal();

    m_entries.store(m_db.size());
  }


  statecache::JournalOpen MtlStateCache::openJournal(const std::string& path, uint64_t keySignature) {
    return openJournalFile(path, statecache::journalHeader(keySignature));
  }


  statecache::JournalAppend MtlStateCache::appendJournalRecords(const std::string& path, const std::vector<uint8_t>& bytes) {
    return appendFile(path, bytes);
  }


  void MtlStateCache::appendJournal() {
    // Writer thread, once per intake that used new entries: one append, never on the render thread.
    if (!m_journalOn) {
      m_journalPending.clear();
      return;
    }

    std::vector<uint8_t> bytes;
    statecache::appendRecords(bytes, m_journalPending);

    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
      if (!m_journalReady) {
        const statecache::JournalOpen open = openJournal(journalPath(), m_config.keySignature);
        if (open == statecache::JournalOpen::Busy)
          return;   // another process is creating it: the records stay pending for the next intake
        if (open != statecache::JournalOpen::Ready) {
          const bool newer = open == statecache::JournalOpen::Newer;
          m_journalOn = false;
          Logger::warn(newer
            ? std::string("STATECACHE journal written by a newer d9mt: not appending")
            : str::format("STATECACHE journal unusable: ", m_config.displayPath, ".journal"));
          m_journalPending.clear();
          return;
        }
        m_journalReady = true;
      }
      const statecache::JournalAppend a = appendJournalRecords(journalPath(), bytes);
      if (a == statecache::JournalAppend::Busy)
        return;   // another client holds the lock (or is frozen holding it): next intake
      ok = a == statecache::JournalAppend::Ok;
      // Gone (another process's write removed it): open it again, then retry once.
      m_journalReady = ok;
      if (a == statecache::JournalAppend::Failed)
        break;
    }

    if (ok)
      m_journalRecords.fetch_add(m_journalPending.size(), std::memory_order_relaxed);
    else {
      m_journalOn = false;
      Logger::warn(str::format("STATECACHE journal append failed: ", m_config.displayPath, ".journal"));
    }
    m_journalPending.clear();
  }


  bool MtlStateCache::write() {
    const auto t0 = Clock::now();
    uint32_t session = m_session;

    // Merge what other processes wrote meanwhile.
    std::vector<uint8_t> disk;
    if (readAll(m_config.file, disk)) {
      std::vector<Entry> entries;
      const ParseResult r = parse(disk.data(), disk.size(), m_config.keySignature, entries);

      if (r.status == ParseStatus::NewerVersion) {
        m_readOnly = true;
        m_journalOn = false;
        Logger::info("STATECACHE write skipped: the file was written by a newer d9mt");
        return false;
      }

      if (r.status == ParseStatus::Ok) {
        // Another process's entries are checked like ours at load.
        for (const Entry& e : entries) {
          if (!m_dropped.count(identityHash(e))
           && (m_config.noValidate || m_backend->psoStateValid(e.key, nullptr)))
            m_db.merge(e);
        }
        session = std::max(session, r.session);
      }
    }

    // ... and what they journaled since their last write (it is removed below). The journal
    // lock is held from this re-read to the removal, so no other client's append can land in
    // between, in a journal about to be removed. Busy: merged but kept (a later write removes
    // it; replaying it is idempotent).
    JournalFd journalFd = NoJournalFd;
    const bool journalLocked = m_journalOn
      && lockJournal(journalPath(), journalFd) == statecache::JournalAppend::Ok;
    if (m_journalOn) {
      std::vector<uint8_t> jdata;
      if (readAll(journalPath(), jdata)) {
        std::vector<Entry> jentries;
        const JournalParseResult jr = parseJournal(jdata.data(), jdata.size(), m_config.keySignature, jentries);
        for (const Entry& e : jentries) {
          if (!m_dropped.count(identityHash(e))
           && (m_config.noValidate || m_backend->psoStateValid(e.key, nullptr)))
            m_db.merge(e);
        }
        session = std::max(session, jr.maxSession);
      }
    }

    m_db.applyCap(m_config.maxEntries);

    const std::vector<uint8_t> bytes = serialize(m_db.entries(), m_config.keySignature, session);
    const auto now = Clock::now();
    m_lastWrite = now;

    if (!writeAtomic(m_config.file, bytes)) {
      if (journalLocked)
        unlockJournal(journalFd);
      if (++m_writeFailures <= 3u)
        Logger::warn(str::format("STATECACHE write failed: ", m_config.displayPath));
      return false;
    }

    m_dirty = false;
    m_writes.fetch_add(1u, std::memory_order_relaxed);

    // Everything journaled is in the file now. A crash between the rename and this only
    // replays records the file already has (merge is a union).
    if (journalLocked) {
      removeFile(journalPath());
      unlockJournal(journalFd);
      m_journalReady = false;
    }
    m_entries.store(m_db.size());

    Logger::info(str::format("STATECACHE write entries=", m_db.size(), " bytes=", bytes.size(),
      " ms=", std::chrono::duration<double, std::milli>(Clock::now() - t0).count()));
    return true;
  }


  void MtlStateCache::logUse(bool final) {
    m_lastLog = Clock::now();

    const Stats s = stats();
    const uint64_t sum = s.hits + s.late + s.misses + s.unrecorded + s.recorded
                       + s.built + s.dup + s.failed + s.skipped + s.ringDrops;

    if (!final && sum == m_loggedSum)
      return;

    m_loggedSum = sum;

    size_t ready = 0u;
    { std::lock_guard lock(m_mutex);
      ready = m_index.readyCount();
    }

    Logger::info(str::format("STATECACHE use t=", uint64_t(seconds()), "s",
      " hits=", s.hits, " late=", s.late, " misses=", s.misses,
      " recorded=", s.recorded, " unrecorded=", s.unrecorded, " ringDrops=", s.ringDrops,
      " hitBuildMs=", uint64_t(m_hitBuildNs.load() / 1000000u),
      " | prewarm built=", s.built, " dup=", s.dup, " failed=", s.failed, " skipped=", s.skipped,
      " buildMs=", uint64_t(m_buildNs.load() / 1000000u),
      " pausedMs=", uint64_t(m_pausedNs.load() / 1000000u), " boosts=", s.boosts, " ready=", ready,
      " | entries=", s.entries, " invalid=", s.invalid, final ? " (final)" : ""));
  }


  void MtlStateCache::prewarmMain(uint32_t index) {
    nameThread("d9mt-prewarm");
    m_slots[index].handle = openCurrentThread();

    while (true) {
      Entry e;
      ShaderSlot vs, ps;

      { std::unique_lock lock(m_mutex);
        m_prewarmCond.wait(lock, [this, index] {
          return m_stop.load() || (m_indexBuilt
            && m_index.readyCount() > (index ? SecondThreadMinReady : 0u));
        });

        if (m_stop.load())
          return;

        m_index.pop(e);

        auto v = m_shaders.find(vsRef(e));
        if (v != m_shaders.end())
          vs = v->second;

        if (e.psOrigin) {
          auto p = m_shaders.find(psRef(e));
          if (p != m_shaders.end())
            ps = p->second;
        }

        m_activeJobs++;
      }

      // The marker names this job before anything of it runs.
      m_slots[index].inflight.store(identityHash(e), std::memory_order_relaxed);
      writeMarker();

      bool ok = true;
      if (vs.shader && (!e.psOrigin || ps.shader))
        ok = prewarmOne(index, e, vs, ps);
      else
        m_skipped.fetch_add(1u, std::memory_order_relaxed);

      m_slots[index].inflight.store(0u, std::memory_order_relaxed);

      bool drained = false;

      { std::lock_guard lock(m_mutex);
        if (!ok)
          m_failed.push_back(e);
        m_activeJobs--;
        drained = !m_activeJobs && !m_index.readyCount();
      }

      m_prewarmCond.notify_all();

      if (drained)
        clearMarkerIfDrained();

      // One line each time the queue drains (start-up, then rarely).
      const uint64_t done = m_built.load() + m_dup.load() + m_failedCount.load() + m_skipped.load();
      if (drained && m_prewarmLogged.exchange(done) != done) {
        Logger::info(str::format("STATECACHE prewarm t=", seconds(), "s built=", m_built.load(),
          " dup=", m_dup.load(), " failed=", m_failedCount.load(), " skipped=", m_skipped.load(),
          " buildMs=", uint64_t(m_buildNs.load() / 1000000u),
          " pausedMs=", uint64_t(m_pausedNs.load() / 1000000u)));
      }
    }
  }


  bool MtlStateCache::prewarmOne(uint32_t index, const Entry& e, const ShaderSlot& vs, const ShaderSlot& ps) {
    if (!yieldToRenderThread())
      return true;

    Rc<MtlShaderFunction> vsFn = vs.shader->prewarmFunction(nullptr);
    Rc<MtlShaderFunction> psFn = e.psOrigin
      ? ps.shader->prewarmFunction(e.psLinked ? vs.shader : nullptr)
      : nullptr;

    if (vsFn == nullptr || (e.psOrigin && psFn == nullptr)) {
      m_skipped.fetch_add(1u, std::memory_order_relaxed);
      return true;
    }

    if (!finishFunction(vsFn.ptr()) || (psFn != nullptr && !finishFunction(psFn.ptr())))
      return true;

    // A shader that doesn't compile in this d9mt: nothing to build.
    if (!vsFn->library || (psFn != nullptr && !psFn->library)) {
      m_skipped.fetch_add(1u, std::memory_order_relaxed);
      return true;
    }

    if (!yieldToRenderThread())
      return true;

    Rc<MtlCompileTask> pso = m_backend->createPrewarmPso(e.key, vsFn.ptr(), psFn.ptr());

    if (pso == nullptr) {
      m_dup.fetch_add(1u, std::memory_order_relaxed);
      return true;
    }

    PrewarmSlot& slot = m_slots[index];
    const uint64_t t0 = compile_stats::nowNs();

    // False if a draw claimed it first; it then builds (or waits) there.
    slot.running.store(pso.ptr(), std::memory_order_release);
    const bool ran = pso->tryRun();
    slot.running.store(nullptr, std::memory_order_release);

    if (slot.boosted.exchange(false))
      setCurrentThreadPriorityLowest();

    if (!ran)
      return true;

    m_buildNs.fetch_add(compile_stats::nowNs() - t0, std::memory_order_relaxed);

    if (m_backend->psoSucceeded(pso.ptr())) {
      m_built.fetch_add(1u, std::memory_order_relaxed);
      return true;
    }

    m_failedCount.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }


  void MtlStateCache::writeMarker() {
    // Rewritten at every job start (a few hundred tiny writes at startup,
    // on prewarm threads): it always names the jobs in flight, and comes
    // back if another process on the same file removed it.
    std::lock_guard lock(m_markerMutex);
    std::string text = str::format("d9mt-prewarm 1\ntrips ", m_trips, "\n");

    for (uint32_t i = 0u; i < m_config.threads; i++) {
      if (const uint64_t h = m_slots[i].inflight.load(std::memory_order_relaxed)) {
        char hex[20];
        std::snprintf(hex, sizeof(hex), "%016llx\n", (unsigned long long)h);
        text += hex;
      }
    }

    m_markerOnDisk = writeSmallFileAtomic(markerPath(), text) || m_markerOnDisk;
  }


  void MtlStateCache::clearMarkerIfDrained() {
    std::lock_guard lock(m_markerMutex);
    if (!m_markerOnDisk)
      return;

    // Re-checked under the marker lock: a job that started after the drain
    // was seen either holds m_activeJobs here, or writes the marker again.
    { std::lock_guard indexLock(m_mutex);
      if (m_activeJobs || m_index.readyCount())
        return;
    }

    removeFile(markerPath());
    m_markerOnDisk = false;

    // A prewarm queue drained without the process dying: the trips are over.
    if (m_trips) {
      removeFile(tripsPath());
      m_trips = 0u;
    }
  }


  bool MtlStateCache::finishFunction(MtlShaderFunction* pFn) {
    MtlCompileTask* task = pFn->task.ptr();

    if (!task || task->isDone() || task->tryRun())
      return true;

    // Running elsewhere (a worker or a draw): wait without taking it over.
    while (!task->waitFor(5'000'000u)) {
      if (m_stop.load())
        return false;
    }

    return true;
  }


  bool MtlStateCache::yieldToRenderThread() {
    uint64_t t0 = 0u;

    while (!m_stop.load()) {
      // Only while an on-demand compile is pending. No grace period after
      // a wait: measured, that starves the prewarm exactly when the draws
      // catch up with it, and on-demand builds don't get faster from it.
      if (!m_backend->urgentCompilesQueued() && !compile_stats::renderWaiting())
        break;

      if (!t0)
        t0 = compile_stats::nowNs();
      sleepMs(2u);
    }

    if (t0)
      m_pausedNs.fetch_add(compile_stats::nowNs() - t0, std::memory_order_relaxed);

    return !m_stop.load();
  }

}
