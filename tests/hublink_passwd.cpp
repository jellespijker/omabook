// A user for the test's own sshd (#180), preloaded into it and into ssh: in
// a container whose user has no line in /etc/passwd, sshd can log no one in
// and ssh won't start, and the test can't add one there. With
// HUBLINK_PASSWD_NAME and HUBLINK_PASSWD_HOME set, this answers the passwd
// lookups for the running user (by that name, or by its uid) with a home in
// the test's temporary folder, and hands every other lookup on to the C
// library. As nss_wrapper does, for this one user.

#include <dlfcn.h>
#include <pwd.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

[[nodiscard]] const char *named() {
    return std::getenv("HUBLINK_PASSWD_NAME");
} // NOLINT(concurrency-mt-unsafe): read only, before any thread asks

[[nodiscard]] const char *home() { return std::getenv("HUBLINK_PASSWD_HOME"); } // NOLINT(concurrency-mt-unsafe): as above

[[nodiscard]] bool isTheUser(const char *name) {
    return named() != nullptr && home() != nullptr && name != nullptr && std::string_view(name) == named();
}

[[nodiscard]] bool isTheUid(uid_t uid) { return named() != nullptr && home() != nullptr && uid == ::getuid(); }

// Copies `text` into `buffer` at `used`; where it went, or nothing when it doesn't fit.
[[nodiscard]] char *place(const char *text, char *buffer, std::size_t size, std::size_t &used) {
    const std::size_t length = std::strlen(text) + 1;
    if (used + length > size)
        return nullptr;
    char *at = buffer + used; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): the caller's buffer, as getpwnam_r hands it
    std::memcpy(at, text, length);
    used += length;
    return at;
}

// The user's entry, written in `buffer` as the _r functions do.
[[nodiscard]] int fill(passwd *entry, char *buffer, std::size_t size, passwd **result) {
    std::size_t used = 0;
    char *name = place(named(), buffer, size, used);
    char *password = place("x", buffer, size, used);
    char *gecos = place("", buffer, size, used);
    char *folder = place(home(), buffer, size, used);
    char *shell = place("/bin/sh", buffer, size, used);
    if (name == nullptr || password == nullptr || gecos == nullptr || folder == nullptr || shell == nullptr) {
        *result = nullptr;
        return ERANGE;
    }
    *entry = passwd{.pw_name = name,
                    .pw_passwd = password,
                    .pw_uid = ::getuid(),
                    .pw_gid = ::getgid(),
                    .pw_gecos = gecos,
                    .pw_dir = folder,
                    .pw_shell = shell};
    *result = entry;
    return 0;
}

[[nodiscard]] passwd *theEntry() {
    static passwd entry{};
    static char buffer[8192]; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): getpwnam's own storage, as the C library
                              // keeps it
    passwd *result = nullptr;
    return fill(&entry, buffer, sizeof buffer, &result) == 0 ? result : nullptr;
}

// The C library's own function `symbol`.
template <typename Function> [[nodiscard]] Function next(const char *symbol) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): dlsym answers a function as a void pointer
    return reinterpret_cast<Function>(::dlsym(RTLD_NEXT, symbol)); // ast-grep-ignore: no-reinterpret-cast
}

} // namespace

extern "C" {

passwd *getpwnam(const char *name) {
    if (isTheUser(name))
        return theEntry();
    return next<passwd *(*)(const char *)>("getpwnam")(name);
}

passwd *getpwuid(uid_t uid) {
    if (isTheUid(uid))
        return theEntry();
    return next<passwd *(*)(uid_t)>("getpwuid")(uid);
}

int getpwnam_r(const char *name, passwd *entry, char *buffer, std::size_t size, passwd **result) {
    if (isTheUser(name))
        return fill(entry, buffer, size, result);
    return next<int (*)(const char *, passwd *, char *, std::size_t, passwd **)>("getpwnam_r")(name, entry, buffer, size, result);
}

int getpwuid_r(uid_t uid, passwd *entry, char *buffer, std::size_t size, passwd **result) {
    if (isTheUid(uid))
        return fill(entry, buffer, size, result);
    return next<int (*)(uid_t, passwd *, char *, std::size_t, passwd **)>("getpwuid_r")(uid, entry, buffer, size, result);
}
}
