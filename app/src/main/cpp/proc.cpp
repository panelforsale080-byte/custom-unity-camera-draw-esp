#include "proc.hpp"
#include "mem.hpp"
#include <dirent.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

static std::string read_cmdline(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!f) return {};
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (char &c : all)
        if (c == '\0') c = ' ';
    while (!all.empty() && all.back() == ' ') all.pop_back();
    return all;
}

static bool cmdline_has(pid_t pid, const char *needle) {
    return read_cmdline(pid).find(needle) != std::string::npos;
}

void proc_format_cmdline(pid_t pid, char *out, size_t outCap) {
    if (!out || !outCap) return;
    const std::string s = read_cmdline(pid);
    std::snprintf(out, outCap, "%s", s.c_str());
}

size_t proc_list_mlbb(ProcCandidate *out, size_t maxOut) {
    if (!out || !maxOut) return 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        const pid_t pid = (pid_t)std::atoi(e->d_name);
        if (pid <= 0) continue;
        if (!cmdline_has(pid, "com.mobile.legends")) continue;
        out[n].pid = pid;
        proc_format_cmdline(pid, out[n].cmdline, sizeof(out[n].cmdline));
        if (++n >= maxOut) break;
    }
    closedir(d);
    return n;
}

pid_t proc_find_unity_kills() {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    pid_t best = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        const pid_t pid = (pid_t)std::atoi(e->d_name);
        if (pid <= 0) continue;
        if (cmdline_has(pid, "com.mobile.legends") && cmdline_has(pid, "UnityKillsMe"))
            best = pid;
    }
    closedir(d);
    return best;
}

pid_t proc_find_mlbb_fallback() {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    pid_t best = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        const pid_t pid = (pid_t)std::atoi(e->d_name);
        if (pid <= 0) continue;
        if (!cmdline_has(pid, "com.mobile.legends")) continue;
        if (cmdline_has(pid, "UnityKillsMe")) {
            best = pid;
            break;
        }
        if (!best) best = pid;
    }
    closedir(d);
    return best;
}

bool proc_alive(pid_t pid) {
    return pid > 0 && kill(pid, 0) == 0;
}

bool proc_probe_read(pid_t pid) {
    std::vector<MapsReg> maps;
    if (!mem_maps(pid, maps)) return false;
    int tried = 0;
    for (const auto &r : maps) {
        if (r.perms[0] != 'r' || r.end <= r.start) continue;
        uint8_t scratch[8] = {};
        if (mem_read(pid, r.start, scratch, sizeof(scratch)))
            return true;
        if (++tried >= 64) break;
    }
    return false;
}
