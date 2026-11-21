// SPDX-License-Identifier: Apache-2.0
// 设备事实 M0 实现（#1749）：sys 段采集 + 进程级共享快照 + 档案落盘。vk 段由 dev_facts_vk.c
// 在 s2a_vk_device_create 现查处组好经 s2a_dev_facts_set_vk 交进来。
// 宿主测试：S2A_DEV_SYSFS_ROOT 把 sysfs 根指到夹具目录（生产路径为空 = 绝对路径），
//   dlsym 探测在宿主拿不到 libandroid.so → vsync/adpf 自然落 -1，与三态契约一致。
#define _GNU_SOURCE   // sched_getaffinity / cpu_set_t（同 cpu_floor.c）
#include "dev_facts.h"
#include "sha256.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static s2a_fact3 f3(int8_t v, const char *why) {
    s2a_fact3 t; t.v = v;
    snprintf(t.why, sizeof t.why, "%s", why ? why : "");
    return t;
}

// ---------- sysfs 读取（S2A_DEV_SYSFS_ROOT 只供宿主夹具；生产为空） ----------
static const char *sysfs_root(void) {
    const char *r = getenv("S2A_DEV_SYSFS_ROOT");
    return r && r[0] ? r : "";
}
static int read_uint_file(const char *path, uint32_t *out) {
    FILE *f = fopen(path, "r");
    unsigned v = 0; int ok = f && fscanf(f, "%u", &v) == 1;
    if (f) fclose(f);
    if (ok) *out = v;
    return ok;
}

// 全配置核表：cpu0..ncpu-1 连续下标；单核读不到字段记 0（坏数据回退，不整表失败）。
// cluster = related_cpus 集按首见序聚成的簇序号（0..126，>126 截 -1 表示超出表宽）。
static s2a_dev_cpu *read_cpu_table(int *out_n) {
    const long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n <= 0 || n > 65536) return NULL;
    s2a_dev_cpu *t = calloc((size_t)n, sizeof *t);
    if (!t) return NULL;
    char roots[128]; snprintf(roots, sizeof roots, "%s/sys/devices/system/cpu", sysfs_root());
    // 第一遍：各核 related_cpus 的最小核号做簇键；第二遍：簇键按首见序编簇号。
    int *key = calloc((size_t)n, sizeof *key);
    if (!key) { free(t); return NULL; }
    for (long i = 0; i < n; i++) {
        char p[160];
        t[i].id = (int)i;
        t[i].cluster = -1;
        snprintf(p, sizeof p, "%s/cpu%ld/cpu_capacity", roots, i);
        read_uint_file(p, &t[i].capacity);
        snprintf(p, sizeof p, "%s/cpu%ld/cpufreq/cpuinfo_max_freq", roots, i);
        read_uint_file(p, &t[i].max_khz);
        snprintf(p, sizeof p, "%s/cpu%ld/cpufreq/related_cpus", roots, i);
        FILE *f = fopen(p, "r");
        key[i] = (int)i;
        if (f) {
            int v; while (fscanf(f, "%d", &v) == 1) if (v >= 0 && v < key[i]) key[i] = v;
            fclose(f);
        }
    }
    int ncluster = 0;
    for (long i = 0; i < n; i++) {
        int cid = -1;
        for (long k = 0; k < ncluster; k++) if (key[k] == key[i]) { cid = (int)k; break; }
        if (cid < 0) { cid = ncluster; key[ncluster] = key[i]; ncluster++; }
        t[i].cluster = (int8_t)(cid <= 126 ? cid : -1);
    }
    free(key);
    *out_n = (int)n;
    return t;
}

static void fill_cpu(s2a_dev_sys *sys, const s2a_dev_cpu *t, int n) {
    sys->configured_ncpu = n;
    int all_cap = n > 0, homo = 1; unsigned first = n > 0 ? t[0].capacity : 0;
    for (int i = 0; i < n; i++) {
        if (!t[i].capacity) all_cap = 0;
        if (t[i].capacity != first) homo = 0;
    }
    const int copy = n < S2A_FACT_MAX_CPU ? n : S2A_FACT_MAX_CPU;
    if (t && copy > 0) memcpy(sys->cpu, t, (size_t)copy * sizeof *t);
    sys->complete = f3(n <= 0 ? -1 : (all_cap && n <= S2A_FACT_MAX_CPU),
                       n <= 0 ? "_SC_NPROCESSORS_CONF 读不到" :
                       !all_cap ? "有核 cpu_capacity 读不到" :
                       n > S2A_FACT_MAX_CPU ? "核数超 cpu[] 表宽（全量见 s2a_dev_cpus）" : "");
    sys->topology_ok = f3(n <= 0 ? -1 : (all_cap && !homo),
                          n <= 0 ? "_SC_NPROCESSORS_CONF 读不到" :
                          !all_cap ? "有核 cpu_capacity 读不到" : "");
}

static int have_sym(void *h, const char *name) { return h && dlsym(h, name); }

// 允许调度集大小（sched_getaffinity）：Linux/Android 走 cpu_set_t；宿主无此 API → 0。
static int read_affinity_ncpu(void) {
#if defined(__linux__) || defined(__ANDROID__)
    cpu_set_t set; CPU_ZERO(&set);
    return sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : 0;
#else
    return 0;
#endif
}

// AChoreographer 三档与 ADPF：每档 = 该档回调 + 公共前提（ALooper 三件套 + getInstance）全齐才算 1；
// libandroid.so 打不开 → 全部 -1（无法判定，不是「没有」）。
static void fill_dlopen_facts(s2a_dev_sys *sys) {
    void *h = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *w = "dlopen libandroid.so 失败";
        sys->adpf_syms = f3(-1, w);
        sys->vsync_api33 = sys->vsync_api29 = sys->vsync_api24 = f3(-1, w);
        return;
    }
    const int common = have_sym(h, "ALooper_prepare") && have_sym(h, "ALooper_pollOnce") &&
                       have_sym(h, "ALooper_wake") && have_sym(h, "AChoreographer_getInstance");
    sys->vsync_api33 = f3(!common ? 0 :
        (have_sym(h, "AChoreographer_postVsyncCallback") &&
         have_sym(h, "AChoreographerFrameCallbackData_getFrameTimeNanos") &&
         have_sym(h, "AChoreographerFrameCallbackData_getFrameTimelinesLength") &&
         have_sym(h, "AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex") &&
         have_sym(h, "AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos")),
        common ? "" : "ALooper/AChoreographer 公共前提不齐");
    sys->vsync_api29 = f3(common && have_sym(h, "AChoreographer_postFrameCallback64"),
                          common ? "" : "ALooper/AChoreographer 公共前提不齐");
    sys->vsync_api24 = f3(common && have_sym(h, "AChoreographer_postFrameCallback"),
                          common ? "" : "ALooper/AChoreographer 公共前提不齐");
    sys->adpf_syms = f3(have_sym(h, "APerformanceHint_getManager") &&
                        have_sym(h, "APerformanceHint_createSession") &&
                        have_sym(h, "APerformanceHint_updateTargetWorkDuration") &&
                        have_sym(h, "APerformanceHint_reportActualWorkDuration") &&
                        have_sym(h, "APerformanceHint_closeSession"), "");
    dlclose(h);
}

int s2a_dev_facts_fill_sys(s2a_dev_sys *sys) {
    if (!sys) return -1;
    memset(sys, 0, sizeof *sys);
    int n = 0; s2a_dev_cpu *t = read_cpu_table(&n);
    fill_cpu(sys, t, n);
    free(t);
    sys->affinity_ncpu = read_affinity_ncpu();
    sys->page_size = (int)sysconf(_SC_PAGESIZE);
    struct stat st;
    sys->kgsl_node = f3(stat("/dev/kgsl-3d0", &st) == 0, "");
    fill_dlopen_facts(sys);
    // PowerMonitor 轨有无是 Java 侧 SystemHealthManager.getSupportedPowerMonitors 的事实，
    // 原生层无可查符号；M0 未桥接 → 未知（不拿 SDK 版本猜）。
    sys->power_monitor = f3(-1, "PowerMonitor 轨属 Java 侧事实，M0 原生未桥接");
    return 0;
}

// ---------- 进程级共享快照 ----------
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static s2a_dev_facts g_facts;
static s2a_dev_cpu *g_cpus;
static int g_ncpu;

static void facts_init(void) {
    const uint64_t t0 = now_ns();
    g_cpus = read_cpu_table(&g_ncpu);
    memset(&g_facts.sys, 0, sizeof g_facts.sys);
    fill_cpu(&g_facts.sys, g_cpus, g_ncpu);
    g_facts.sys.affinity_ncpu = read_affinity_ncpu();
    g_facts.sys.page_size = (int)sysconf(_SC_PAGESIZE);
    struct stat st;
    g_facts.sys.kgsl_node = f3(stat("/dev/kgsl-3d0", &st) == 0, "");
    fill_dlopen_facts(&g_facts.sys);
    g_facts.sys.power_monitor = f3(-1, "PowerMonitor 轨属 Java 侧事实，M0 原生未桥接");
    g_facts.facts_ns = now_ns() - t0;
}

const s2a_dev_facts *s2a_dev_facts_get(void) {
    pthread_once(&g_once, facts_init);
    return &g_facts;
}
const s2a_dev_cpu *s2a_dev_cpus(int *ncpu) {
    pthread_once(&g_once, facts_init);
    if (ncpu) *ncpu = g_ncpu;
    return g_cpus;
}
void s2a_dev_facts_set_vk(const s2a_dev_vk *vk, uint64_t collect_ns) {
    pthread_once(&g_once, facts_init);
    if (vk) g_facts.vk = *vk;
    g_facts.facts_ns += collect_ns;
}

// ---------- 摘要 ----------
// 规范化字节：volatile 字段（采集耗时）清零后对整结构取 sha256。digest 取前 4 字节；
// 档案里 factsDigest 与文件名用同一哈希的 hex 文本。
static void facts_sha256_hex(const s2a_dev_facts *f, char out[65]) {
    s2a_dev_facts c = *f; c.facts_ns = 0;
    s2a_sha256 s; s2a_sha256_init(&s);
    s2a_sha256_update(&s, &c, sizeof c);
    uint8_t d[32]; s2a_sha256_final(&s, d);
    s2a_sha256_hex(d, out);
}
uint32_t s2a_dev_facts_digest(const s2a_dev_facts *f) {
    if (!f) return 0;
    char hex[65]; facts_sha256_hex(f, hex);
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = hex[i];
        v = (v << 4) | (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return v;
}

// ---------- 档案落盘（s2a-device-profile-v1，M0 段） ----------
// system / model 需要 Android 属性：生产读 __system_property_get；宿主为空（字段照写、值为空）。
#if defined(__ANDROID__)
#include <sys/system_properties.h>
static int prop_str(const char *name, char *out, size_t cap) {
    const int n = __system_property_get(name, out);
    if (n < 0) out[0] = 0; else if ((size_t)n >= cap) out[cap - 1] = 0;
    return n > 0;
}
#else
static int prop_str(const char *name, char *out, size_t cap) { (void)name; if (cap) out[0] = 0; return 0; }
#endif

// 追加写：缓冲足够不会失败；失败（截断）返回 -1 由调用方整体判失败。
static int put(size_t *used, char *out, size_t cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const int n = vsnprintf(out + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *used) return -1;
    *used += (size_t)n;
    return 0;
}
static int put_text(size_t *used, char *out, size_t cap, const char *s) {   // JSON 字符串（带引号 + 转义）
    if (put(used, out, cap, "\"")) return -1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { if (put(used, out, cap, "\\%c", *p)) return -1; }
        else if (*p < 0x20) { if (put(used, out, cap, "\\u%04x", *p)) return -1; }
        else { if (put(used, out, cap, "%c", *p)) return -1; }
    }
    return put(used, out, cap, "\"");
}
static int put_fact3(size_t *used, char *out, size_t cap, const char *key, s2a_fact3 v) {
    return put(used, out, cap, "\"%s\":%d,", key, v.v);
}

// 档案正文（M0 段：schema/platform/api/identity/model/vulkan/system/factsDigest；
// probes / bench 段属 F1 / F2 里程碑，M0 不出键）。返回 0 = out 收完整 JSON。
static int profile_json(const s2a_dev_facts *f, const char hex65[65], char *out, size_t cap, size_t *len) {
    size_t u = 0;
    char manu[S2A_FACT_NAME], model[S2A_FACT_NAME], soc[S2A_FACT_NAME], rel[24], sdk[12];
    prop_str("ro.product.manufacturer", manu, sizeof manu);
    prop_str("ro.product.model", model, sizeof model);
    prop_str("ro.soc.model", soc, sizeof soc);
    prop_str("ro.build.version.release", rel, sizeof rel);
    prop_str("ro.build.version.sdk", sdk, sizeof sdk);
    const s2a_dev_vk *v = &f->vk;
    const s2a_dev_sys *s = &f->sys;
    if (put(&u, out, cap, "{\"schema\":\"s2a-device-profile-v1\",\"platform\":\"android\",\"api\":{\"kind\":\"vulkan\",\"version\":\"%u.%u.%u\"},",
            (v->api_version >> 22) & 0x3ffu, (v->api_version >> 12) & 0x3ffu, v->api_version & 0xfffu)) return -1;
    if (put(&u, out, cap, "\"identity\":{\"vendorId\":\"0x%04x\",\"deviceId\":\"0x%04x\",\"driverVersion\":%u,\"driverName\":",
            f->id.vendor_id, f->id.device_id, f->id.driver_version)) return -1;
    if (put_text(&u, out, cap, f->id.driver_name)) return -1;
    if (put(&u, out, cap, ",\"deviceName\":")) return -1;
    if (put_text(&u, out, cap, f->id.device_name)) return -1;
    if (put(&u, out, cap, "},\"model\":{\"manufacturer\":")) return -1;
    if (put_text(&u, out, cap, manu)) return -1;
    if (put(&u, out, cap, ",\"model\":")) return -1;
    if (put_text(&u, out, cap, model)) return -1;
    if (put(&u, out, cap, ",\"socModel\":")) return -1;
    if (put_text(&u, out, cap, soc)) return -1;
    if (put(&u, out, cap, ",\"androidRelease\":")) return -1;
    if (put_text(&u, out, cap, rel)) return -1;
    if (put(&u, out, cap, "},\"vulkan\":{\"extensions\":{")) return -1;
    for (int i = 0; i < v->ext_n && i < S2A_FACT_MAX_EXT; i++) {
        if (put(&u, out, cap, "%s", i ? "," : "")) return -1;
        if (put_text(&u, out, cap, v->ext[i])) return -1;
        if (put(&u, out, cap, ":1")) return -1;
    }
    if (put(&u, out, cap, "},\"enumFailed\":%s,\"features\":{\"textureCompressionBC\":%d,\"geometryShader\":%d,"
            "\"multiDrawIndirect\":%d,\"drawIndirectFirstInstance\":%d,\"indexTypeUint8\":%d,"
            "\"samplerFilterMinmax\":%d,\"shaderInt16\":%d},",
            v->ext_enum_failed ? "true" : "false",
            v->feat.texture_compression_bc.v, v->feat.geometry_shader.v, v->feat.multi_draw_indirect.v,
            v->feat.draw_indirect_first_instance.v, v->feat.index_type_uint8.v,
            v->feat.sampler_filter_minmax.v, v->feat.shader_int16.v)) return -1;
    if (put(&u, out, cap, "\"properties\":{\"maxDescriptorSetUniformBuffersDynamic\":%u,"
            "\"maxMemoryAllocationSize\":%llu,\"timestampValidBits\":%u,\"timestampPeriodNs\":%g},",
            v->limits.max_desc_set_ubo_dynamic, (unsigned long long)v->limits.max_allocation_size,
            v->limits.timestamp_valid_bits, (double)v->limits.timestamp_period)) return -1;
    if (put(&u, out, cap, "\"memory\":{\"hostCached\":\"0x%08x\",\"hostCoherent\":\"0x%08x\",\"deviceLocal\":\"0x%08x\",\"deviceLocalHeapBytes\":%llu},",
            v->mem_host_cached, v->mem_host_coherent, v->mem_device_local,
            (unsigned long long)v->heap_device_local_bytes)) return -1;
    if (put(&u, out, cap, "\"timeDomains\":[\"%s\",\"%s\"],\"formats\":{",
            v->limits.time_domain_device ? "DEVICE" : "", v->limits.time_domain_monotonic ? "MONOTONIC" : "")) return -1;
    int first_fmt = 1;
    for (int i = 0; i < S2A_FACT_MAX_FMT; i++) {
        if (!v->fmt_optimal[i] && !v->fmt_buffer[i]) continue;
        if (put(&u, out, cap, "%s\"%d\":{\"optimal\":\"0x%08x\",\"buffer\":\"0x%08x\"}",
                first_fmt ? "" : ",", i, v->fmt_optimal[i], v->fmt_buffer[i])) return -1;
        first_fmt = 0;
    }
    if (put(&u, out, cap, "}},\"system\":{\"sdk\":%s,\"pageSize\":%d,"
            "\"configuredNcpu\":%d,\"affinityNcpu\":%d,\"cpu\":{\"capacity\":[",
            sdk[0] ? sdk : "-1", s->page_size, s->configured_ncpu, s->affinity_ncpu)) return -1;
    for (int i = 0; i < S2A_FACT_MAX_CPU && i < s->configured_ncpu; i++) {
        if (put(&u, out, cap, "%s%u", i ? "," : "", s->cpu[i].capacity)) return -1;
    }
    if (put(&u, out, cap, "],\"maxKHz\":[")) return -1;
    for (int i = 0; i < S2A_FACT_MAX_CPU && i < s->configured_ncpu; i++) {
        if (put(&u, out, cap, "%s%u", i ? "," : "", s->cpu[i].max_khz)) return -1;
    }
    if (put(&u, out, cap, "],\"cluster\":[")) return -1;
    for (int i = 0; i < S2A_FACT_MAX_CPU && i < s->configured_ncpu; i++) {
        if (put(&u, out, cap, "%s%d", i ? "," : "", s->cpu[i].cluster)) return -1;
    }
    if (put(&u, out, cap, "]},")) return -1;
    if (put_fact3(&u, out, cap, "topologyComplete", s->complete)) return -1;
    if (put_fact3(&u, out, cap, "topologyOk", s->topology_ok)) return -1;
    if (put_fact3(&u, out, cap, "kgsl", s->kgsl_node)) return -1;
    if (put_fact3(&u, out, cap, "adpf", s->adpf_syms)) return -1;
    if (put_fact3(&u, out, cap, "vsyncApi33", s->vsync_api33)) return -1;
    if (put_fact3(&u, out, cap, "vsyncApi29", s->vsync_api29)) return -1;
    if (put_fact3(&u, out, cap, "vsyncApi24", s->vsync_api24)) return -1;
    if (put(&u, out, cap, "\"powerRails\":%d},\"factsDigest\":\"sha256-", s->power_monitor.v)) return -1;
    if (put(&u, out, cap, "%s\",\"collectMs\":%llu.%03llu}", hex65,
            (unsigned long long)(f->facts_ns / 1000000), (unsigned long long)(f->facts_ns / 1000 % 1000))) return -1;
    if (len) *len = u;
    return 0;
}

int s2a_dev_profile_write(const char *files_dir, char *why, size_t why_size) {
    if (!files_dir || files_dir[0] != '/') { snprintf(why, why_size, "files_dir 非法"); return -1; }
    const s2a_dev_facts *f = s2a_dev_facts_get();
    if (!f) { snprintf(why, why_size, "事实快照缺失"); return -1; }
    char hex[65]; facts_sha256_hex(f, hex);
    char dir[384]; snprintf(dir, sizeof dir, "%s/devprofile", files_dir);
    if (mkdir(dir, 0700) && errno != EEXIST) { snprintf(why, why_size, "mkdir devprofile: %s", strerror(errno)); return -1; }
    char path[400], tmp[408];
    snprintf(path, sizeof path, "%s/%.16s.json", dir, hex);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    static char buf[96 * 1024]; size_t len = 0;
    if (profile_json(f, hex, buf, sizeof buf, &len)) { snprintf(why, why_size, "档案 JSON 超缓冲"); return -1; }
    FILE *fp = fopen(tmp, "w");
    if (!fp) { snprintf(why, why_size, "open %s: %s", tmp, strerror(errno)); return -1; }
    const size_t w = fwrite(buf, 1, len, fp);
    const int bad = w != len || fclose(fp);
    if (bad) { unlink(tmp); snprintf(why, why_size, "写档案失败"); return -1; }
    if (rename(tmp, path)) { unlink(tmp); snprintf(why, why_size, "rename: %s", strerror(errno)); return -1; }
    if (why && why_size) snprintf(why, why_size, "%s", path);
    return 0;
}
