#pragma once
/* Runtime for np_3ds: azahar-room process (host), UDP forwarders, hubnet::Session transport. See netplay_3ds.h.
 *
 * Safety rule: Azahar shows a MODAL error dialog when a join fails, so RetroHub starts Azahar only after
 * ready() is true (room process alive and listening, Session connected to every PC). The room runs without
 * a password (a mismatch is what raises the dialog); it is only reachable by guests through their forwarder. */
#include <memory>
#include <string>

#include "netplay_3ds.h"

namespace hubnp3ds {

class Runner {
  public:
    Runner();
    ~Runner();
    /* Host: spawns azahar-room (hidden, kill-on-close) and the host forwarder. room_exe = azahar-room.exe path. */
    bool start_host(const std::string& server, const std::string& lobby_code, int launch_no, const Plan& plan,
                    const std::string& room_exe, const std::string& work_dir, std::string* err);
    /* Host, no child process: the emulator itself listens on 127.0.0.1:plan.port (Dolphin NetPlay). tag names the Session room ("3ds" / "gc"). */
    bool start_host_forward(const std::string& server, const std::string& lobby_code, int launch_no, const Plan& plan, const std::string& tag, std::string* err);
    /* Guest: local loopback UDP port that Azahar's -m must point at is local_port(). */
    bool start_guest(const std::string& server, const std::string& lobby_code, int launch_no, const Plan& plan, int my_slot,
                     std::string* err, const std::string& tag = "3ds");
    void update();   // call every UI tick
    void stop();
    bool active() const;
    bool ready() const;    // safe to start Azahar
    bool failed() const;
    std::string status() const;
    int connect_port() const;   // host: plan.port; guest: the local forwarder port
    unsigned long long datagrams_out() const;
    unsigned long long datagrams_in() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* azahar-room.exe beside the given azahar.exe ("" when missing). */
std::string room_exe_for(const std::string& azahar_exe);

}  // namespace hubnp3ds
