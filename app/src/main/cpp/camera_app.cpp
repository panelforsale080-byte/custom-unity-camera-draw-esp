#include "il2_layout.hpp"
#include "matrix4x4.hpp"
#include "mem.hpp"
#include "proc.hpp"
#include "remote_call.hpp"
#include "remote_il2cpp.hpp"

#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Hero {
    uintptr_t ent = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    bool self = false;
};

std::mutex g_mu;
std::atomic<bool> g_run{false};
std::thread g_worker;
pid_t g_pid = 0;
float g_screenW = 1.f;
float g_screenH = 1.f;
char g_status[240] = "Pick a process";
std::vector<float> g_lines;

RemoteIl2CppInfo g_il2{};
bool g_resolved = false;
uintptr_t g_rtInvoke = 0;
uintptr_t g_getMain = 0;
uintptr_t g_w2c = 0;
uintptr_t g_proj = 0;
uintptr_t g_w2s = 0;
uintptr_t g_bm = 0;
uintptr_t g_playerOff = 0;
uintptr_t g_localOff = 0;
uintptr_t g_posOff = 0;
uintptr_t g_campOff = 0;

void set_status(const char *text) {
    std::lock_guard<std::mutex> lock(g_mu);
    std::snprintf(g_status, sizeof(g_status), "%s", text ? text : "");
}

uintptr_t scratch_cstr(RemoteSession &s, pid_t pid, int slot, const char *lit) {
    if (!lit) lit = "";
    const size_t n = std::strlen(lit) + 1;
    const uintptr_t p = remote_session_scratch(s, slot, n);
    if (!p || !mem_write(pid, p, lit, n)) return 0;
    return p;
}

uintptr_t class_from_image(RemoteSession &s, pid_t pid, uintptr_t image, const char *ns,
                           const char *cls) {
    if (!image || !g_il2.sym_class_from_name) return 0;
    const uintptr_t nsPtr = scratch_cstr(s, pid, 5, ns ? ns : "");
    const uintptr_t namePtr = scratch_cstr(s, pid, 6, cls ? cls : "");
    if (!nsPtr || !namePtr) return 0;
    uintptr_t klass = 0;
    if (!remote_session_call(s, g_il2.sym_class_from_name, {image, nsPtr, namePtr}, klass))
        return 0;
    return mem_ptr_ok(klass) ? klass : 0;
}

uintptr_t find_class(RemoteSession &s, pid_t pid, const std::vector<uintptr_t> &images,
                     uintptr_t prefer, const char *full) {
    std::string ns, cls = full ? full : "";
    const size_t dot = cls.find_last_of('.');
    if (dot != std::string::npos) {
        ns = cls.substr(0, dot);
        cls = cls.substr(dot + 1);
    }
    if (prefer) {
        uintptr_t k = class_from_image(s, pid, prefer, ns.c_str(), cls.c_str());
        if (k) return k;
    }
    for (uintptr_t image : images) {
        if (image == prefer) continue;
        uintptr_t k = class_from_image(s, pid, image, ns.c_str(), cls.c_str());
        if (k) return k;
    }
    return 0;
}

uintptr_t method_from_name(RemoteSession &s, pid_t pid, uintptr_t klass, const char *name,
                           int args) {
    if (!g_il2.sym_class_get_method_from_name || !klass) return 0;
    const uintptr_t namePtr = scratch_cstr(s, pid, 7, name);
    if (!namePtr) return 0;
    uintptr_t method = 0;
    if (!remote_session_call(s, g_il2.sym_class_get_method_from_name,
                             {klass, namePtr, (uintptr_t)args}, method))
        return 0;
    return mem_ptr_ok(method) ? method : 0;
}

uintptr_t field_offset(RemoteSession &s, pid_t pid, uintptr_t klass, const char *name) {
    if (!g_il2.sym_class_get_field_from_name || !klass) return 0;
    const uintptr_t namePtr = scratch_cstr(s, pid, 10, name);
    if (!namePtr) return 0;
    uintptr_t field = 0;
    if (!remote_session_call(s, g_il2.sym_class_get_field_from_name, {klass, namePtr}, field) ||
        !field)
        return 0;
    if (g_il2.sym_field_get_offset) {
        uintptr_t off = 0;
        if (remote_session_call(s, g_il2.sym_field_get_offset, {field}, off) && off > 0 &&
            off < 0x4000)
            return off;
    }
    return 0;
}

uintptr_t static_ptr(RemoteSession &s, pid_t pid, uintptr_t klass, const char *name) {
    if (!g_il2.sym_class_get_field_from_name || !g_il2.sym_field_static_get_value || !klass)
        return 0;
    const uintptr_t namePtr = scratch_cstr(s, pid, 8, name);
    if (!namePtr) return 0;
    uintptr_t field = 0;
    if (!remote_session_call(s, g_il2.sym_class_get_field_from_name, {klass, namePtr}, field) ||
        !field)
        return 0;
    const uintptr_t valSlot = remote_session_scratch(s, 2, sizeof(uintptr_t));
    if (!valSlot) return 0;
    uintptr_t z = 0;
    mem_write(pid, valSlot, &z, sizeof(z));
    uintptr_t dummy = 0;
    if (!remote_session_call(s, g_il2.sym_field_static_get_value, {field, valSlot}, dummy))
        return 0;
    uintptr_t inst = 0;
    mem_read(pid, valSlot, inst);
    return mem_ptr_ok(inst) ? inst : 0;
}

bool read_boxed_matrix(pid_t pid, uintptr_t obj, Matrix4x4 &out) {
    if (!mem_ptr_ok(obj)) return false;
    for (uintptr_t off : {0x10u, 0x18u, 0x20u}) {
        Matrix4x4 m{};
        if (!mem_read(pid, obj + off, m)) continue;
        if (!std::isfinite(m.m00) || !std::isfinite(m.m33)) continue;
        const float mag =
            std::fabs(m.m00) + std::fabs(m.m11) + std::fabs(m.m22) + std::fabs(m.m33);
        if (mag > 1e-4f && mag < 1e8f) {
            out = m;
            return true;
        }
    }
    return false;
}

bool invoke_matrix(RemoteSession &s, pid_t pid, uintptr_t method, uintptr_t cam, Matrix4x4 &out) {
    uintptr_t exc = 0;
    const uintptr_t excSlot = remote_session_scratch(s, 5, 8);
    if (!excSlot) return false;
    mem_write(pid, excSlot, &exc, sizeof(exc));
    uintptr_t ret = 0;
    if (!remote_session_call(s, g_rtInvoke, {method, cam, 0, excSlot}, ret)) return false;
    return read_boxed_matrix(pid, ret, out);
}

bool project_matrix(const Matrix4x4 &vp, float wx, float wy, float wz, float &outX, float &outY) {
    const Vector3 from(wx, wy, wz);
    const float w = vp.m30 * from.X + vp.m31 * from.Y + vp.m32 * from.Z + vp.m33;
    if (w < 0.01f) return false;
    const Vector3 clip = vp.MultiplyPoint(from);
    outX = (clip.X + 1.f) * 0.5f * g_screenW;
    outY = g_screenH - (clip.Y + 1.f) * 0.5f * g_screenH;
    return std::isfinite(outX) && std::isfinite(outY);
}

bool read_vec3_obj(pid_t pid, uintptr_t obj, float &x, float &y, float &z) {
    if (!mem_ptr_ok(obj)) return false;
    for (uintptr_t off : {0x10u, 0x14u, 0x18u, 0x20u}) {
        Vec3 v{};
        if (!mem_read(pid, obj + off, v)) continue;
        if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) {
            x = v.x;
            y = v.y;
            z = v.z;
            return true;
        }
    }
    return false;
}

bool project_w2s(RemoteSession &s, pid_t pid, uintptr_t cam, float wx, float wy, float wz,
                 float &outX, float &outY) {
    const uintptr_t vecSlot = remote_session_scratch(s, 0, 16);
    const uintptr_t paramSlot = remote_session_scratch(s, 1, 8);
    const uintptr_t excSlot = remote_session_scratch(s, 2, 8);
    if (!vecSlot || !paramSlot || !excSlot || !g_w2s) return false;
    const float xyz[3] = {wx, wy, wz};
    mem_write(pid, vecSlot, xyz, sizeof(xyz));
    mem_write(pid, paramSlot, &vecSlot, sizeof(vecSlot));
    uintptr_t zero = 0;
    mem_write(pid, excSlot, &zero, sizeof(zero));
    uintptr_t ret = 0;
    if (!remote_session_call(s, g_rtInvoke, {g_w2s, cam, paramSlot, excSlot}, ret)) return false;
    float sx = 0.f, sy = 0.f, sz = 0.f;
    if (!read_vec3_obj(pid, ret, sx, sy, sz)) {
        if (!mem_read(pid, vecSlot, sx) || !mem_read(pid, vecSlot + 4, sy) ||
            !mem_read(pid, vecSlot + 8, sz))
            return false;
    }
    if (sz <= 0.f) return false;
    outX = sx;
    outY = g_screenH - sy;
    return std::isfinite(outX) && std::isfinite(outY);
}

bool pos_ok(const Vec3 &p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
           !(std::fabs(p.x) < 0.01f && std::fabs(p.z) < 0.01f) && std::fabs(p.x) <= 10000.f &&
           std::fabs(p.z) <= 10000.f;
}

bool read_heroes(std::vector<Hero> &out, uintptr_t &local) {
    out.clear();
    local = 0;
    if (!g_bm || !g_playerOff || !g_posOff) return false;
    if (g_localOff) mem_read(g_pid, g_bm + g_localOff, local);
    uintptr_t dict = 0;
    if (!mem_read(g_pid, g_bm + g_playerOff, dict) || !mem_ptr_ok(dict)) return false;
    uintptr_t entries = 0;
    int count = 0;
    if (!mem_read(g_pid, dict + Il2::DictEntries, entries) || !entries) return false;
    if (!mem_read(g_pid, dict + Il2::DictCount, count) || count <= 0 || count > 64) return false;
    int maxLength = 0;
    if (!mem_read(g_pid, entries + Il2::ArrMaxLen, maxLength) || maxLength <= 0 || maxLength > 128)
        return false;
    const uintptr_t data = entries + Il2::ArrData;
    std::vector<uint8_t> raw((size_t)maxLength * Il2::EntryStride);
    if (!mem_read(g_pid, data, raw.data(), raw.size())) return false;
    for (int i = 0; i < maxLength && (int)out.size() < count; ++i) {
        const uint8_t *base = raw.data() + (size_t)i * Il2::EntryStride;
        int hash = 0;
        uintptr_t value = 0;
        std::memcpy(&hash, base + Il2::EntryHash, sizeof(hash));
        if (hash < 0) continue;
        std::memcpy(&value, base + Il2::EntryValue, sizeof(value));
        if (!mem_ptr_ok(value)) continue;
        if (g_campOff) {
            uint8_t camp = 0;
            if (mem_read(g_pid, value + g_campOff, camp) && camp) continue;
        }
        Vec3 p{};
        if (!mem_read(g_pid, value + g_posOff, p) || !pos_ok(p)) continue;
        Hero h{};
        h.ent = value;
        h.x = p.x;
        h.y = p.y;
        h.z = p.z;
        h.self = value == local;
        out.push_back(h);
    }
    return !out.empty();
}

bool resolve_locked(pid_t pid) {
    g_resolved = false;
    g_il2 = {};
    g_rtInvoke = g_getMain = g_w2c = g_proj = g_w2s = 0;
    g_bm = g_playerOff = g_localOff = g_posOff = g_campOff = 0;
    if (!remote_detect_il2cpp(pid, g_il2)) {
        if (g_il2.note[0])
            set_status(g_il2.note);
        else
            set_status("IL2CPP was not found in this process");
        return false;
    }
    if (!g_il2.domain_ptr) remote_il2cpp_prime_domain(pid, g_il2);
    RemoteSession s{};
    if (!remote_session_begin_try(pid, s)) {
        set_status("Could not attach to read IL2CPP");
        return false;
    }
    uintptr_t domain = mem_ptr_ok(g_il2.domain_ptr) ? g_il2.domain_ptr : 0;
    if (!domain && g_il2.sym_domain_get)
        remote_session_call(s, g_il2.sym_domain_get, {}, domain);
    if (!mem_ptr_ok(domain)) {
        remote_session_end(s);
        set_status("il2cpp_domain_get failed");
        return false;
    }
    g_il2.domain_ptr = domain;
    if (g_il2.sym_thread_attach) {
        uintptr_t thr = 0;
        remote_session_call(s, g_il2.sym_thread_attach, {domain}, thr);
    }
    const uintptr_t sizeSlot = remote_session_scratch(s, 0, sizeof(size_t));
    size_t asmCount = 0;
    uintptr_t assemblies = 0;
    size_t zero = 0;
    if (sizeSlot) mem_write(pid, sizeSlot, &zero, sizeof(zero));
    if (!sizeSlot || !g_il2.sym_domain_get_assemblies ||
        !remote_session_call(s, g_il2.sym_domain_get_assemblies, {domain, sizeSlot}, assemblies) ||
        !mem_read(pid, sizeSlot, asmCount) || asmCount == 0 || asmCount > 512) {
        remote_session_end(s);
        set_status("IL2CPP assemblies were not found");
        return false;
    }
    std::vector<uintptr_t> images;
    uintptr_t unityImg = 0;
    uintptr_t csharpImg = 0;
    if (g_il2.sym_domain_assembly_open && g_il2.sym_assembly_get_image) {
        const char *unityNames[] = {"UnityEngine.CoreModule", "UnityEngine.CoreModule.dll",
                                    "UnityEngine.dll"};
        for (const char *name : unityNames) {
            if (unityImg) break;
            const uintptr_t np = scratch_cstr(s, pid, 3, name);
            uintptr_t as = 0, img = 0;
            if (np && remote_session_call(s, g_il2.sym_domain_assembly_open, {domain, np}, as) &&
                as && remote_session_call(s, g_il2.sym_assembly_get_image, {as}, img) && img)
                unityImg = img;
        }
        const char *csNames[] = {"Assembly-CSharp", "Assembly-CSharp.dll"};
        for (const char *name : csNames) {
            if (csharpImg) break;
            const uintptr_t np = scratch_cstr(s, pid, 4, name);
            uintptr_t as = 0, img = 0;
            if (np && remote_session_call(s, g_il2.sym_domain_assembly_open, {domain, np}, as) &&
                as && remote_session_call(s, g_il2.sym_assembly_get_image, {as}, img) && img)
                csharpImg = img;
        }
    }
    for (size_t i = 0; i < asmCount && images.size() < 80; ++i) {
        uintptr_t asmPtr = 0, image = 0;
        if (!mem_read(pid, assemblies + i * sizeof(uintptr_t), asmPtr) || !asmPtr) continue;
        if (!remote_session_call(s, g_il2.sym_assembly_get_image, {asmPtr}, image) || !image)
            continue;
        images.push_back(image);
    }
    const uintptr_t camKlass = find_class(s, pid, images, unityImg, "UnityEngine.Camera");
    if (!camKlass) {
        remote_session_end(s);
        set_status("UnityEngine.Camera class was not found");
        return false;
    }
    g_getMain = method_from_name(s, pid, camKlass, "get_main", 0);
    g_w2c = method_from_name(s, pid, camKlass, "get_worldToCameraMatrix", 0);
    g_proj = method_from_name(s, pid, camKlass, "get_projectionMatrix", 0);
    g_w2s = method_from_name(s, pid, camKlass, "WorldToScreenPoint", 1);
    g_rtInvoke = g_il2.sym_runtime_invoke;
    const uintptr_t bmKlass = find_class(s, pid, images, csharpImg, "BattleManager");
    const uintptr_t seKlass = find_class(s, pid, images, csharpImg, "ShowEntity");
    if (bmKlass) {
        g_bm = static_ptr(s, pid, bmKlass, "Instance");
        g_playerOff = field_offset(s, pid, bmKlass, "m_dicPlayerShow");
        g_localOff = field_offset(s, pid, bmKlass, "m_LocalPlayerShow");
    }
    if (seKlass) {
        g_posOff = field_offset(s, pid, seKlass, "m_vCachePosition");
        if (!g_posOff) g_posOff = field_offset(s, pid, seKlass, "<LastPosition>k__BackingField");
        if (!g_posOff) g_posOff = field_offset(s, pid, seKlass, "LastPosition");
        g_campOff = field_offset(s, pid, seKlass, "m_bSameCampType");
    }
    remote_session_end(s);
    if (!g_rtInvoke || !g_getMain) {
        set_status("Camera class found, get_main was not");
        return false;
    }
    g_resolved = true;
    char buf[240];
    std::snprintf(buf, sizeof(buf), "Camera class ready  get_main=0x%llx",
                  (unsigned long long)g_getMain);
    set_status(buf);
    return true;
}

void tick() {
    if (g_pid <= 0 || !proc_alive(g_pid)) {
        set_status("Process ended");
        g_run.store(false);
        return;
    }
    if (!g_resolved) {
        static auto nextTry = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now < nextTry) return;
        if (!resolve_locked(g_pid)) {
            nextTry = now + std::chrono::seconds(1);
            return;
        }
    }
    std::vector<Hero> heroes;
    uintptr_t local = 0;
    read_heroes(heroes, local);
    RemoteSession s{};
    if (!remote_session_begin_try(g_pid, s)) {
        set_status("Could not attach to UnityEngine.Camera");
        return;
    }
    uintptr_t exc = 0;
    const uintptr_t excSlot = remote_session_scratch(s, 4, 8);
    if (excSlot) mem_write(g_pid, excSlot, &exc, sizeof(exc));
    uintptr_t cam = 0;
    const bool gotCam =
        remote_session_call(s, g_rtInvoke, {g_getMain, 0, excSlot, 0}, cam) && mem_ptr_ok(cam);
    Matrix4x4 view{}, proj{}, vp{};
    bool haveVp = false;
    if (gotCam && g_w2c && g_proj) {
        haveVp = invoke_matrix(s, g_pid, g_w2c, cam, view) &&
                 invoke_matrix(s, g_pid, g_proj, cam, proj);
        if (haveVp) {
            vp = proj * view;
            const float mag =
                std::fabs(vp.m00) + std::fabs(vp.m11) + std::fabs(vp.m22) + std::fabs(vp.m33);
            haveVp = std::isfinite(vp.m00) && mag > 1e-4f && mag < 1e8f;
        }
    }
    Hero self{};
    bool haveSelf = false;
    for (const Hero &h : heroes) {
        if (h.ent == local || h.self) {
            self = h;
            haveSelf = true;
            break;
        }
    }
    float sx = 0.f, sy = 0.f, tx = 0.f, ty = 0.f;
    bool selfOn = false;
    if (haveSelf) {
        selfOn = haveVp ? project_matrix(vp, self.x, self.y, self.z, sx, sy)
                        : (gotCam && project_w2s(s, g_pid, cam, self.x, self.y, self.z, sx, sy));
    }
    if (!selfOn) {
        sx = g_screenW * 0.5f;
        sy = g_screenH * 0.92f;
    }
    std::vector<float> lines;
    int hit = 0;
    int shown = 0;
    for (const Hero &h : heroes) {
        if (h.ent == local) continue;
        ++shown;
        const bool ok = haveVp ? project_matrix(vp, h.x, h.y, h.z, tx, ty)
                               : (gotCam && project_w2s(s, g_pid, cam, h.x, h.y, h.z, tx, ty));
        if (!ok) continue;
        lines.push_back(sx);
        lines.push_back(sy);
        lines.push_back(tx);
        lines.push_back(ty);
        ++hit;
    }
    remote_session_end(s);
    char buf[240];
    if (!gotCam) {
        std::snprintf(buf, sizeof(buf), "Camera class ready. No camera object yet.");
    } else {
        std::snprintf(buf, sizeof(buf), "OK %s  camera=0x%llx  lines %d/%d",
                      haveVp ? "matrix" : "w2s", (unsigned long long)cam, hit, shown);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    std::snprintf(g_status, sizeof(g_status), "%s", buf);
    g_lines.swap(lines);
}

void worker_main() {
    while (g_run.load()) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

} // namespace

extern "C" {

JNIEXPORT jobjectArray JNICALL Java_com_custom_unitycamera_NativeBridge_nativeListProcesses(
    JNIEnv *env, jclass) {
    std::vector<std::string> rows;
    DIR *d = opendir("/proc");
    if (d) {
        while (dirent *de = readdir(d)) {
            if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
            const pid_t pid = (pid_t)std::atoi(de->d_name);
            if (pid <= 0) continue;
            char cmd[240] = {};
            proc_format_cmdline(pid, cmd, sizeof(cmd));
            if (!cmd[0]) continue;
            char row[300];
            std::snprintf(row, sizeof(row), "%d  %s", (int)pid, cmd);
            rows.emplace_back(row);
            if (rows.size() >= 250) break;
        }
        closedir(d);
    }
    std::sort(rows.begin(), rows.end());
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)rows.size(), strCls, nullptr);
    for (jsize i = 0; i < (jsize)rows.size(); ++i)
        env->SetObjectArrayElement(arr, i, env->NewStringUTF(rows[(size_t)i].c_str()));
    return arr;
}

JNIEXPORT jstring JNICALL Java_com_custom_unitycamera_NativeBridge_nativeAttach(JNIEnv *env, jclass,
                                                                               jint pid) {
    if (g_run.exchange(false) && g_worker.joinable()) g_worker.join();
    g_pid = (pid_t)pid;
    g_resolved = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_lines.clear();
        std::snprintf(g_status, sizeof(g_status), "Attaching to pid %d", (int)pid);
    }
    if (!proc_alive(g_pid) || !proc_probe_read(g_pid)) {
        set_status("Cannot read that process");
        std::lock_guard<std::mutex> lock(g_mu);
        return env->NewStringUTF(g_status);
    }
    g_run.store(true);
    g_worker = std::thread(worker_main);
    std::lock_guard<std::mutex> lock(g_mu);
    return env->NewStringUTF(g_status);
}

JNIEXPORT void JNICALL Java_com_custom_unitycamera_NativeBridge_nativeStop(JNIEnv *, jclass) {
    if (g_run.exchange(false) && g_worker.joinable()) g_worker.join();
    std::lock_guard<std::mutex> lock(g_mu);
    g_lines.clear();
    std::snprintf(g_status, sizeof(g_status), "Stopped");
}

JNIEXPORT void JNICALL Java_com_custom_unitycamera_NativeBridge_nativeSetScreen(JNIEnv *, jclass,
                                                                               jint w, jint h) {
    if (w > 0 && h > 0) {
        g_screenW = (float)w;
        g_screenH = (float)h;
    }
}

JNIEXPORT jstring JNICALL Java_com_custom_unitycamera_NativeBridge_nativeStatus(JNIEnv *env,
                                                                               jclass) {
    std::lock_guard<std::mutex> lock(g_mu);
    return env->NewStringUTF(g_status);
}

JNIEXPORT jfloatArray JNICALL Java_com_custom_unitycamera_NativeBridge_nativeLines(JNIEnv *env,
                                                                                  jclass) {
    std::lock_guard<std::mutex> lock(g_mu);
    jfloatArray arr = env->NewFloatArray((jsize)g_lines.size());
    if (arr && !g_lines.empty())
        env->SetFloatArrayRegion(arr, 0, (jsize)g_lines.size(), g_lines.data());
    return arr;
}

} // extern "C"
