// plat_proc on macOS: spawn / wait / kill_tree / process lookups. No GUI side effects (no open / activate / hide / send_key).
#include "plat_proc.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

using namespace plat::proc;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static long ms_since(std::chrono::steady_clock::time_point t) {
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}

int main() {
    char tmpl[] = "/tmp/plat_proc_testXXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string out = dir + "/out.txt";
    auto run = [&](SpawnOptions o, int* code) {
        o.stdout_path = out;
        std::remove(out.c_str());
        Child c; std::string err;
        bool ok = spawn(o, &c, &err);
        if (!ok) std::printf("spawn failed: %s\n", err.c_str());
        CHECK(ok);
        int rc = -1;
        CHECK(wait(c, 10000, &rc));
        if (code) *code = rc;
        return slurp(out);
    };

    { // args verbatim
        SpawnOptions o; o.exe = "/bin/echo";
        o.args = {"a b", "\"q\"", "$HOME", "x'y", "*"};
        int rc; std::string s = run(o, &rc);
        CHECK(rc == 0);
        CHECK(s == "a b \"q\" $HOME x'y *\n");
    }
    { // cwd
        SpawnOptions o; o.exe = "/bin/pwd"; o.cwd = "/usr/bin";
        int rc; std::string s = run(o, &rc);
        CHECK(s == "/usr/bin\n");
    }
    { // env set / unset
        setenv("PLAT_PROC_KEEP", "1", 1);
        setenv("PLAT_PROC_DROP", "1", 1);
        SpawnOptions o; o.exe = "/usr/bin/env";
        o.env_set = {{"PLAT_PROC_NEW", "v a=l"}, {"PLAT_PROC_KEEP", "2"}};
        o.env_unset = {"PLAT_PROC_DROP"};
        int rc; std::string s = run(o, &rc);
        CHECK(s.find("PLAT_PROC_NEW=v a=l\n") != std::string::npos);
        CHECK(s.find("PLAT_PROC_KEEP=2\n") != std::string::npos);
        CHECK(s.find("PLAT_PROC_KEEP=1") == std::string::npos);
        CHECK(s.find("PLAT_PROC_DROP") == std::string::npos);
    }
    { // exit code
        SpawnOptions o; o.exe = "/bin/sh"; o.args = {"-c", "exit 7"};
        int rc = -1; run(o, &rc);
        CHECK(rc == 7);
    }
    { // signal exit
        SpawnOptions o; o.exe = "/bin/sh"; o.args = {"-c", "kill -TERM $$"};
        int rc = -1; run(o, &rc);
        CHECK(rc == 128 + 15);
    }
    { // spawn failure
        SpawnOptions o; o.exe = "/nonexistent/nope";
        Child c; std::string err;
        CHECK(!spawn(o, &c, &err));
        CHECK(!err.empty());
    }
    { // wait timeout, then running / kill_tree
        SpawnOptions o; o.exe = "/bin/sleep"; o.args = {"5"};
        Child c; CHECK(spawn(o, &c, nullptr));
        CHECK(c.valid() && c.pgid == c.pid);
        auto t = std::chrono::steady_clock::now();
        int rc = 0;
        CHECK(!wait(c, 150, &rc));
        long el = ms_since(t);
        CHECK(el >= 100 && el < 600);
        CHECK(running(c));
        CHECK(pid_alive(c.pid));
        // identity
        CHECK(c.start_time_us != 0);
        CHECK(pid_start_time_us(c.pid) == c.start_time_us);
        CHECK(pid_exe_path(c.pid).find("/sleep") != std::string::npos);
        bool found = false;
        for (int p : pids_by_exe_name("SLEEP")) if (p == c.pid) found = true;
        CHECK(found);
        kill_tree(c, 1500);
        CHECK(!running(c));
        CHECK(!pid_alive(c.pid));
        CHECK(wait(c, 1000, &rc));
        CHECK(rc == 128 + 15);
    }
    { // kill_tree reaches a grandchild
        std::string gf = dir + "/gc.txt";
        SpawnOptions o; o.exe = "/bin/sh";
        o.args = {"-c", "sleep 30 & echo $! > " + gf + "; wait"};
        Child c; CHECK(spawn(o, &c, nullptr));
        int gc = 0;
        for (int i = 0; i < 200 && gc == 0; ++i) {
            std::string s = slurp(gf);
            if (!s.empty() && s.back() == '\n') gc = atoi(s.c_str());
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(gc > 0);
        CHECK(pid_alive(gc));
        kill_tree(c, 2000);
        bool dead = false;
        for (int i = 0; i < 200 && !dead; ++i) {
            dead = !pid_alive(gc);
            if (!dead) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(dead);
        CHECK(!running(c));
    }
    { // our own process
        int me = (int)getpid();
        CHECK(pid_alive(me));
        CHECK(!pid_alive(0));
        std::string exe = pid_exe_path(me);
        CHECK(exe.find("plat_proc_test") != std::string::npos);
        bool found = false;
        for (int p : pids_by_exe_name("plat_proc_test")) if (p == me) found = true;
        CHECK(found);
        uint64_t a = pid_start_time_us(me);
        CHECK(a != 0);
        CHECK(pid_start_time_us(me) == a);
        CHECK(!pid_has_window(me));
        (void)accessibility_trusted(false);
    }
    std::system(("rm -rf " + dir).c_str());
    std::printf(g_fail ? "plat_proc_test: %d FAILED\n" : "plat_proc_test: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
