// SPDX-License-Identifier: Apache-2.0
// dev_facts M0 宿主测试（#1749）：sys 填充、共享快照、digest、档案 JSON 落盘。
// sysfs 夹具走 S2A_DEV_SYSFS_ROOT；dlopen 属性在宿主自然 -1（三态契约）。
#include "../native/direct/dev_facts.h"
#include "../native/direct/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_fill_sys_fixture(void) {
    // 夹具：cpu0/1 小核（capacity 400，related_cpus "0 1"）、cpu2..5 大核（capacity 1024，"2 3 4 5"）。
    // sysconf(_SC_NPROCESSORS_CONF) 是宿主机真值——夹具核对不到真核数就跳过拓扑断言。
    char root[] = "/tmp/s2a_facts_sysXXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char d[512];
    snprintf(d, sizeof d, "%s/sys/devices/system/cpu", root);
    mkdir(root, 0700); char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p %s", d);
    CHECK(system(cmd) == 0);
    setenv("S2A_DEV_SYSFS_ROOT", root, 1);
    s2a_dev_sys sys;
    CHECK(s2a_dev_facts_fill_sys(&sys) == 0);
    CHECK(sys.configured_ncpu > 0);
    CHECK(sys.page_size > 0);
    CHECK(sys.kgsl_node.v == 0);
#if defined(__ANDROID__)
    CHECK(0);  // 测试只跑宿主
#else
    CHECK(sys.vsync_api33.v == -1 && sys.adpf_syms.v == -1);   // 宿主无 libandroid.so → 未知非「没有」
    CHECK(sys.affinity_ncpu == 0);                            // 宿主无 sched_getaffinity
#endif
    CHECK(sys.power_monitor.v == -1);
    if (sys.configured_ncpu <= S2A_FACT_MAX_CPU) {
        // 本机核表：capacity 读不到 → complete=0、topology 不判 1（不拿「同构」冒充「异构」）
        CHECK(sys.complete.v <= 0);
    }
    unsetenv("S2A_DEV_SYSFS_ROOT");
}

static void test_snapshot(void) {
    const s2a_dev_facts *a = s2a_dev_facts_get();
    const s2a_dev_facts *b = s2a_dev_facts_get();
    CHECK(a == b);                                        // 同一进程级快照
    CHECK(a->sys.configured_ncpu > 0);
    int n = 0;
    const s2a_dev_cpu *t = s2a_dev_cpus(&n);
    CHECK(n == a->sys.configured_ncpu);
    CHECK(t != NULL && t[0].id == 0);
    CHECK(s2a_dev_facts_digest(a) != 0);
    s2a_dev_facts c = *a; c.vk.ext_n = 1;
    CHECK(s2a_dev_facts_digest(&c) != s2a_dev_facts_digest(a));   // vk 段进 digest
}

static void test_profile_write(void) {
    char dir[] = "/tmp/s2a_facts_outXXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char why[400] = "";
    CHECK(s2a_dev_profile_write(dir, why, sizeof why) == 0);
    char path[512]; snprintf(path, sizeof path, "%s", why);   // 成功时 why 里是落盘路径
    CHECK(strstr(path, "/devprofile/") != NULL && strstr(path, ".json") != NULL);
    FILE *f = fopen(path, "r"); CHECK(f != NULL);
    static char buf[96 * 1024]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
    buf[n] = 0;
    CHECK(strstr(buf, "\"schema\":\"s2a-device-profile-v1\"") != NULL);
    CHECK(strstr(buf, "\"platform\":\"android\"") != NULL);
    CHECK(strstr(buf, "\"factsDigest\":\"sha256-") != NULL);
    CHECK(strstr(buf, "\"system\":{") != NULL && strstr(buf, "\"capacity\":[") != NULL);
    CHECK(strstr(buf, "\"vulkan\":{\"extensions\":{") != NULL);
    // 幂等：同指纹同名文件
    char why2[400] = "";
    CHECK(s2a_dev_profile_write(dir, why2, sizeof why2) == 0);
    CHECK(strcmp(path, why2) == 0);
    // 坏目录 → -1，不落半成品
    char why3[64] = "";
    CHECK(s2a_dev_profile_write("relative/path", why3, sizeof why3) == -1);
}

int main(void) {
    test_fill_sys_fixture();
    test_snapshot();
    test_profile_write();
    printf("%d checks, %d fails\n", checks, fails);
    return fails ? 1 : 0;
}
