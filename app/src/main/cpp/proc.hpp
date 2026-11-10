#pragma once
#include <sys/types.h>
#include <cstddef>

struct ProcCandidate {
    pid_t pid = 0;
    char cmdline[280] = {};
};

pid_t proc_find_unity_kills();
pid_t proc_find_mlbb_fallback();
/** True when cmdline contains UnityKillsMe (may be empty briefly during load). */
bool proc_is_unity_kills_me(pid_t pid);
/** MLBB game process: UnityKillsMe cmdline or liblogic.so mapped (stable during transitions). */
bool proc_is_mlbb_unity(pid_t pid);
bool proc_maps_has_liblogic(pid_t pid);
bool proc_alive(pid_t pid);
bool proc_probe_read(pid_t pid);
size_t proc_list_mlbb(ProcCandidate *out, size_t maxOut);
void proc_format_cmdline(pid_t pid, char *out, size_t outCap);
