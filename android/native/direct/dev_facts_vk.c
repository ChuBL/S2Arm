// SPDX-License-Identifier: Apache-2.0
// 设备事实 vk 段：s2a_vk_device_create 现查处收尾调用（#1749 M0）。
//   与设备创建同一份查询结果（properties / memory / features / 已枚举扩展）进快照，
//   不重复决策：这里只抄事实，不做「要不要启用」的判断。
//   三态口径：枚举失败 = -1（未知，不是「没有」）；明确查到缺席 = 0。
#include "dev_facts.h"
#include "vk_device.h"
#include "../nvn/nvn_vk_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static s2a_fact3 fv(int8_t v, const char *why) {
    s2a_fact3 t; t.v = v;
    snprintf(t.why, sizeof t.why, "%s", why ? why : "");
    return t;
}

// 设备扩展枚举进 ext[]（截断只记 ext_truncated；枚举失败整段 -1）。
static void fill_extensions(VkPhysicalDevice physical, s2a_dev_vk *vk) {
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &n, NULL) != VK_SUCCESS || !n) {
        vk->ext_enum_failed = 1;
        return;
    }
    VkExtensionProperties props[384];
    uint32_t want = n < 384 ? n : 384;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &want, props) != VK_SUCCESS) {
        vk->ext_enum_failed = 1;
        return;
    }
    if (want < n) vk->ext_truncated = 1;
    const uint32_t copy = want < S2A_FACT_MAX_EXT ? want : S2A_FACT_MAX_EXT;
    if (copy < want) vk->ext_truncated = 1;
    for (uint32_t i = 0; i < copy; i++) {
        snprintf(vk->ext[i], S2A_FACT_EXT_NAME, "%s", props[i].extensionName);
    }
    vk->ext_n = (uint8_t)copy;
}

// 队列族时间戳位：选中的渲染队列族 timestampValidBits。
static void fill_queue_bits(VkPhysicalDevice physical, uint32_t family, s2a_dev_vk *vk) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, NULL);
    VkQueueFamilyProperties q[16];
    uint32_t want = n < 16 ? n : 16;
    if (!want) return;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &want, q);
    if (family < want) vk->limits.timestamp_valid_bits = q[family].timestampValidBits;
}

// GPU 时间域：校准扩展启用时才有真值（DEVICE / MONOTONIC 在不在列）。
static void fill_time_domains(const struct s2a_vk_device *d, s2a_dev_vk *vk) {
    const char *fn_name = d->calibrated_timestamps == 1 ? "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"
                        : d->calibrated_timestamps == 2 ? "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT" : NULL;
    if (!fn_name) return;
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR fn =
        (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)
        vkGetInstanceProcAddr(d->instance, fn_name);
    if (!fn) return;
    VkTimeDomainEXT doms[8]; uint32_t n = 8;
    if (fn(d->physical, &n, doms) != VK_SUCCESS) return;
    for (uint32_t i = 0; i < n; i++) {
        if (doms[i] == VK_TIME_DOMAIN_DEVICE_EXT) vk->limits.time_domain_device = 1;
        if (doms[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT) vk->limits.time_domain_monotonic = 1;
    }
}

// 逐 NVN 格式：optimalTiling / bufferFeatures 原值（同 vk_device.c 的 s2a_vk_format_properties 口径，
// 含 A4B4G4R4 不发查询的既有保护）。NVN 格式 → VkFormat 走 nvn_vk_format 既有映射。
static void fill_formats(const struct s2a_vk_device *d, s2a_dev_vk *vk) {
    for (int i = 0; i < S2A_FACT_MAX_FMT; i++) {
        const VkFormat f = nvn_vk_format((uint32_t)i);
        if (f == VK_FORMAT_UNDEFINED) continue;
        VkFormatProperties p; memset(&p, 0, sizeof p);
        s2a_vk_format_properties(d->physical, f, &p);
        vk->fmt_optimal[i] = p.optimalTilingFeatures;
        vk->fmt_buffer[i] = p.bufferFeatures;
    }
}

int s2a_dev_facts_fill_vk(const struct s2a_vk_device *d, char *why, size_t why_size) {
    if (!d || !d->physical) { snprintf(why, why_size, "device 未建成"); return -1; }
    const uint64_t t0 = now_ns();
    s2a_dev_facts f; memset(&f, 0, sizeof f);   // 只组 vk + id 段；sys 已在共享快照里
    s2a_dev_vk *vk = &f.vk;
    const VkPhysicalDeviceProperties *p = &d->properties;
    vk->api_version = p->apiVersion;
    fill_extensions(d->physical, vk);
    vk->feat.texture_compression_bc = fv(d->core_features.textureCompressionBC != 0, "");
    vk->feat.geometry_shader = fv(d->core_features.geometryShader != 0, "");
    vk->feat.multi_draw_indirect = fv(d->core_features.multiDrawIndirect != 0, "");
    vk->feat.draw_indirect_first_instance = fv(d->core_features.drawIndirectFirstInstance != 0, "");
    vk->feat.index_type_uint8 = fv(d->index_type_uint8_enabled != 0, "");
    vk->feat.sampler_filter_minmax = fv(d->sampler_minmax_enabled != 0, "");
    vk->feat.shader_int16 = fv(d->core_features.shaderInt16 != 0, "");
    const VkPhysicalDeviceLimits *l = &p->limits;
    vk->limits.max_desc_set_ubo_dynamic = l->maxDescriptorSetUniformBuffersDynamic;
    vk->limits.max_allocation_size = d->max_alloc;
    vk->limits.timestamp_period = l->timestampPeriod;
    fill_queue_bits(d->physical, d->queue_family, vk);
    fill_time_domains(d, vk);
    for (uint32_t i = 0; i < d->memory.memoryTypeCount && i < 32; i++) {
        const VkMemoryPropertyFlags fl = d->memory.memoryTypes[i].propertyFlags;
        if ((fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (fl & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
            vk->mem_host_cached |= 1u << i;
        if ((fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            vk->mem_host_coherent |= 1u << i;
        if (fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) vk->mem_device_local |= 1u << i;
    }
    for (uint32_t i = 0; i < d->memory.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
        if (d->memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            vk->heap_device_local_bytes += (uint64_t)d->memory.memoryHeaps[i].size;
    fill_formats(d, vk);

    // 身份段：只做指纹 / 档案标签（推导不读）。model / api_level 走系统属性（Android 有值，宿主空）。
    s2a_dev_facts *m = (s2a_dev_facts *)s2a_dev_facts_get();   // 借快照写 id（set_vk 只覆盖 vk 段）
    m->id.vendor_id = p->vendorID;
    m->id.device_id = p->deviceID;
    m->id.driver_version = p->driverVersion;
    snprintf(m->id.device_name, sizeof m->id.device_name, "%s", p->deviceName);
    snprintf(m->id.driver_name, sizeof m->id.driver_name, "%s", d->driver_name);
#if defined(__ANDROID__)
    {   char b[S2A_FACT_NAME];
        if (__system_property_get("ro.product.model", b) > 0) snprintf(m->id.model, sizeof m->id.model, "%s", b);
        b[0] = 0;
        if (__system_property_get("ro.build.version.sdk", b) > 0) m->id.api_level = atoi(b);
        memcpy(m->id.pipeline_cache_uuid, p->pipelineCacheUUID, 16);
    }
#endif
    s2a_dev_facts_set_vk(vk, now_ns() - t0);
    return 0;
}
