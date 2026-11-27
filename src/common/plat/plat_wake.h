#pragma once
/* Wake-up for threads that sleep until data arrives (docs/MACOS.md). Windows code keeps its auto-reset Win32 events and
 * WSAEventSelect. On macOS the equivalent is a Waker (a non-blocking self-pipe): producers call signal(), and the sleeping
 * owner poll()s waker.fd() together with its sockets (plat::wait_readable), then drain()s. Header-only. */

#include "plat.h"

#if !RH_WIN
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <vector>

namespace plat {

class Waker {
  public:
    Waker() {
        if (::pipe(fds_) != 0) { fds_[0] = fds_[1] = -1; return; }
        for (int fd : fds_) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
    ~Waker() { for (int fd : fds_) if (fd >= 0) ::close(fd); }
    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;
    bool ok() const { return fds_[0] >= 0; }
    /* Safe from any thread, any number of times; extra signals before the drain coalesce (like an auto-reset event). */
    void signal() { if (fds_[1] >= 0) { const char c = 1; (void)!::write(fds_[1], &c, 1); } }
    void drain() { char b[64]; if (fds_[0] >= 0) while (::read(fds_[0], b, sizeof b) > 0) {} }
    int fd() const { return fds_[0]; }
  private:
    int fds_[2];
};

/* Sleep until the waker is signalled, one of `fds` is readable, or timeout_ms passes (-1 = forever). Drains the waker.
 * Returns true when woken by the waker or data, false on timeout. */
inline bool wait_readable(Waker& w, const std::vector<int>& fds, int timeout_ms) {
    std::vector<pollfd> p;
    p.reserve(fds.size() + 1);
    p.push_back(pollfd{w.fd(), POLLIN, 0});
    for (int fd : fds) if (fd >= 0) p.push_back(pollfd{fd, POLLIN, 0});
    int r;
    do { r = ::poll(p.data(), (nfds_t)p.size(), timeout_ms); } while (r < 0 && errno == EINTR);
    if (p[0].revents) w.drain();
    return r > 0;
}

}  // namespace plat
#endif
