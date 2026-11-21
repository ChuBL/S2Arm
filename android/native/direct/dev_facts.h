// SPDX-License-Identifier: Apache-2.0
// S2Arm 设备事实（dev_facts，#1749 §2.1 F0）——推导层的唯一输入。
//   三段：identity（只做指纹 / 档案标签，推导代码不读 vendorId / deviceName）、
//        vk（s2a_vk_device_create 现查处填充）、sys（启动时一次读，M4 消费方共用）。
//   三态：1=有 0=无 -1=未知（why 记原因；未知按不支持处理并报红，坑记录 :5732）。
//   纯数据结构：不调 Vulkan、不读属性、不读时钟；宿主测试可整份填 JSON 档案喂推导。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2A_FACT_WHY   48
#define S2A_FACT_NAME  64
#define S2A_FACT_EXT_NAME 256   // 同 VK_MAX_EXTENSION_NAME_SIZE（本头不引 vulkan.h，宿主纯喂档案）
#define S2A_FACT_MAX_CPU   16
#define S2A_FACT_MAX_EXT  192
#define S2A_FACT_MAX_FMT  157   // NVN_FMT_MAX(0x9C)+1；下标 = NVN_FORMAT_* 枚举值（稀疏，空洞项为 0）

typedef struct { int8_t v; char why[S2A_FACT_WHY]; } s2a_fact3;   // 三态事实：1 有 / 0 无 / -1 未知
typedef struct s2a_dev_cpu { int id; uint32_t capacity, max_khz; int8_t cluster; } s2a_dev_cpu;

// ---- 身份：只做 facts_digest 与档案标签。推导规则不许读这段（L0 清空测试 + 负控）。 ----
typedef struct {
    uint32_t vendor_id, device_id, driver_version;
    char device_name[S2A_FACT_NAME];   // VkPhysicalDeviceProperties::deviceName
    char driver_name[S2A_FACT_NAME];   // VK_KHR_driver_properties
    char model[S2A_FACT_NAME];         // ro.product.model（档案标签用）
    uint8_t pipeline_cache_uuid[16];
    int api_level;                     // SDK_INT
} s2a_dev_identity;

// ---- Vulkan 段：s2a_vk_device_create 现查处填，与 device 字段同源（M0 不移除原字段）。 ----
typedef struct {
    uint32_t api_version;                       // physical device apiVersion
    // 扩展清单原样记下（三态在扩展名表里查）；策略 requires 引用此处下标。
    char ext[S2A_FACT_MAX_EXT][S2A_FACT_EXT_NAME];
    uint8_t ext_n;                              // 超出 MAX_EXT 截断
    uint8_t ext_truncated;
    uint8_t ext_enum_failed;                    // 枚举失败 → 全部按未知处理
    // features / features2 链：策略表按名索引（X-macro 生成），M0 先放常用子集
    struct {
        s2a_fact3 texture_compression_bc;
        s2a_fact3 geometry_shader;
        s2a_fact3 multi_draw_indirect;
        s2a_fact3 draw_indirect_first_instance;
        s2a_fact3 index_type_uint8;
        s2a_fact3 sampler_filter_minmax;
        s2a_fact3 shader_int16;
    } feat;
    // 限值（消费方按字段读，同 fill_caps 口径）
    struct {
        uint32_t max_desc_set_ubo_dynamic;      // maxDescriptorSetUniformBuffersDynamic
        uint64_t max_allocation_size;           // maintenance3 maxMemoryAllocationSize
        uint32_t timestamp_valid_bits;          // 渲染队列 queueFamilyProperties
        float timestamp_period;                 // ns
        int time_domain_device;                 // VK_TIME_DOMAIN_DEVICE_KHR 在列
        int time_domain_monotonic;              // VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR
    } limits;
    // 内存类型：位掩码按 memoryType 下标
    uint32_t mem_host_cached;                   // HOST_VISIBLE ∧ HOST_CACHED
    uint32_t mem_host_coherent;                 // HOST_VISIBLE ∧ HOST_COHERENT
    uint32_t mem_device_local;                  // DEVICE_LOCAL
    uint64_t heap_device_local_bytes;
    // 逐格式能力：NVN 格式序号 → VkFormatProperties 原值（格式替换推导在 M1 读；档案原样导出）
    uint32_t fmt_optimal[S2A_FACT_MAX_FMT];
    uint32_t fmt_buffer[S2A_FACT_MAX_FMT];
} s2a_dev_vk;

// ---- 系统段：启动时一次读（M4 共用面：capacity / cpu_floor / ADPF / vsync / 页大小都改读这里）。 ----
typedef struct {
    // CPU 拓扑：覆盖 _SC_NPROCESSORS_CONF 的全部已配置核（falo M4 契约：cpu_floor 按所有核算
    // 「核数×容量」、不允许集裁剪改变掩码语义）；读不到 capacity 的核 capacity=0；cluster 由 related_cpus 聚成。
    // 允许集不在快照里——capacity 消费方仍按线程当前 sched_getaffinity 过滤（行为不变）。
    // 超出 MAX_CPU 时前 16 项进表、complete=0；内部保留未裁剪的 capacity 访问（实现侧全量表）。
    s2a_dev_cpu cpu[S2A_FACT_MAX_CPU];
    int configured_ncpu;                        // _SC_NPROCESSORS_CONF 已配置核总数
    int affinity_ncpu;                          // 采集当时 sched_getaffinity 允许核数（仅信息，不作推导输入）
    s2a_fact3 complete;                         // 全配置核 cpu_capacity 都读到且未截断
    s2a_fact3 topology_ok;                      // 全核 cpu_capacity 可读且非全同构
    int page_size;                              // sysconf(_SC_PAGESIZE)，≤0 = 未知
    s2a_fact3 kgsl_node;                        // /dev/kgsl-3d0 存在（只 stat，不 ioctl）
    s2a_fact3 adpf_syms;                        // APerformanceHint_* 五个符号 dlsym 全齐
    // vsync 三档：每档 = 该档回调 + 公共前提（AChoreographer_getInstance / ALooper_*）全齐才算 1。
    s2a_fact3 vsync_api33;                      // postVsyncCallback + FrameCallbackData 系列 + 公共前提
    s2a_fact3 vsync_api29;                      // postFrameCallback64 + 公共前提
    s2a_fact3 vsync_api24;                      // postFrameCallback + 公共前提
    s2a_fact3 power_monitor;                    // 功耗轨节点（SystemPowerSampler 同源）
} s2a_dev_sys;

typedef struct {
    s2a_dev_identity id;
    s2a_dev_vk vk;
    s2a_dev_sys sys;
    uint64_t facts_ns;                          // 采集耗时（M0 验证 < 10 ms 用）
} s2a_dev_facts;

// 填充入口（M0 实现）：
//   vk 段在 s2a_vk_device_create 现查处顺手填（同一份 has_device_extension / feature 链结果复用）。
//   sys 段一次读（替代 cpu_floor / capacity_affinity / adpf / vsync 各读一遍 sysfs / dlsym）。
//   档案落盘 / 导出在 tools/devprofile（M0 同批）。
int s2a_dev_facts_fill_sys(s2a_dev_sys *sys);
// vk 段填充：s2a_vk_device_create 收尾处调用（dev_facts_vk.c），组好 s2a_dev_vk 交进共享快照
// （identity 段同时就位）。0 = 交付成功；-1 = device 未建成（why 有原因）。线程：[建设备处]。
struct s2a_vk_device;   // 前置声明：本头不引 vulkan.h / vk_device.h
int s2a_dev_facts_fill_vk(const struct s2a_vk_device *d, char *why, size_t why_size);
// 每进程共享一份的 sys 快照（falo M4 契约）。时序：capacity 注册 / cpu_floor 线程可能早于
// 首次 Vulkan 建设备——首读即触发进程级一次性 sys 初始化（pthread_once，不等 VK 段）；
// 此刻 vk 段尚为 0 属正常，vk 段字段只在 s2a_vk_device_create 之后才有效。
// sys 初始化失败的消费方按「未知」处理（s2a_fact3 -1 已在字段里表达）。
const s2a_dev_facts *s2a_dev_facts_get(void);            // sys 未初始化时先初始化再返回；NULL = 采集失败
// 未裁剪的已配置核容量表（falo M4 契约）：内部动态表，上限 _SC_NPROCESSORS_CONF ≤ 65536。
// 约定：下标连续 cpu0..ncpu-1 直接索引（缺失 / 读不到的项 capacity=0）；
// 各消费方的数量上限自理（capacity 全量、cpu_floor 照旧截 64），M0 侧不裁剪。
// 采集失败的字段按坏数据回退（该核 capacity=0），不整表失败。
// 与 s2a_dev_facts_get 同一初始化时序：首读即填充，不等 VK 段。
const s2a_dev_cpu *s2a_dev_cpus(int *ncpu);              // NULL = 采集失败；*ncpu = _SC_NPROCESSORS_CONF
uint32_t s2a_dev_facts_digest(const s2a_dev_facts *f);   // 规范化字节摘要（判「设备 / 驱动变了没」）
// vk 段交付点：s2a_vk_device_create 现查处组好 s2a_dev_vk 后调进来（dev_facts_vk.c），
// 写进 s2a_dev_facts_get() 共享快照的 vk 段并把采集耗时并入 facts_ns。sys 未初始化时先初始化。
void s2a_dev_facts_set_vk(const s2a_dev_vk *vk, uint64_t collect_ns);
// 档案落盘：files_dir/devprofile/<facts_digest 前 16 hex>.json，s2a-device-profile-v1（M0 段）；
// 临时文件 + rename 一次落盘。0 = 写出，-1 = 失败（why 有原因，不落半成品）。线程：[建设备处]。
int s2a_dev_profile_write(const char *files_dir, char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
