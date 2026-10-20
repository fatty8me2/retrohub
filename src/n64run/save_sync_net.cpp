/* Drives the save-sync state machine over a started, ready hubnet::Session. */
#include <windows.h>

#include <vector>

#include "session.h"
#include "save_sync.h"

namespace n64run {

bool run_save_sync(hubnet::Session& s, int my_slot, int num_slots, const std::vector<SaveFile>* host_files,
                   const std::string& client_dir, uint32_t timeout_ms, std::string* err) {
    if (num_slots < 2) return true;   // nobody to share with
    const uint32_t t0 = hubnet::now_ms();
    std::vector<uint8_t> msg;
    int from = 0;

    if (my_slot == 0) {
        static const std::vector<SaveFile> none;
        SaveSender snd(host_files ? *host_files : none, num_slots, t0, timeout_ms);
        SendFn send = [&s](const std::vector<uint8_t>& d) { s.send(-1, d.data(), (int)d.size()); };
        for (;;) {
            s.update();
            while (s.recv(&from, &msg)) snd.on_packet(from, msg.data(), msg.size());
            snd.tick(hubnet::now_ms(), send);
            if (snd.failed()) { if (err) *err = snd.error(); return false; }
            if (snd.finished()) return true;
            if (s.failed()) { if (err) *err = "netplay failed during save sync"; return false; }
            Sleep(2);
        }
    }

    SaveReceiver rcv(my_slot, num_slots, client_dir);
    SendFn send = [&s](const std::vector<uint8_t>& d) { s.send(0, d.data(), (int)d.size()); };
    for (;;) {
        s.update();
        while (s.recv(&from, &msg)) rcv.on_packet(from, msg.data(), msg.size());
        rcv.tick(hubnet::now_ms(), send);
        if (rcv.failed()) { if (err) *err = rcv.error(); return false; }
        if (rcv.finished()) return true;
        if (s.failed()) { if (err) *err = "netplay failed during save sync"; return false; }
        if (hubnet::now_ms() - t0 > timeout_ms) { if (err) *err = "timed out waiting for the host's saves"; return false; }
        Sleep(2);
    }
}

}  // namespace n64run
