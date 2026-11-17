/* ps2_vk13_android.c -- the Vulkan 1.3 core commands the renderer calls directly, for the Quest build.
   The NDK's libvulkan.so stub for API 32 (the APK's minSdk) exports Vulkan 1.1 only; Android 13+ loaders (Horizon OS
   v76+ is Android 14) export 1.3, but linking against them would make libmain.so refuse to load on an older system.
   So these four are defined here and forward to the device's own entry points, fetched once with vkGetDeviceProcAddr
   right after vkCreateDevice (ps2_video.c calls ps2_vk13_bind); the KHR names are the fallback. Device-level pointers
   also skip the loader's trampoline. */
#include <vulkan/vulkan.h>
#include <stdio.h>

void ps2_log(const char *fmt, ...);

static PFN_vkCmdBeginRendering p_begin;
static PFN_vkCmdEndRendering p_end;
static PFN_vkCmdPipelineBarrier2 p_barrier2;
static PFN_vkCmdWriteTimestamp2 p_ts2;

static PFN_vkVoidFunction get2(VkDevice dev, const char *core, const char *khr) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, core);
    return f ? f : vkGetDeviceProcAddr(dev, khr);
}
int ps2_vk13_bind(VkDevice dev) {
    p_begin = (PFN_vkCmdBeginRendering)get2(dev, "vkCmdBeginRendering", "vkCmdBeginRenderingKHR");
    p_end = (PFN_vkCmdEndRendering)get2(dev, "vkCmdEndRendering", "vkCmdEndRenderingKHR");
    p_barrier2 = (PFN_vkCmdPipelineBarrier2)get2(dev, "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR");
    p_ts2 = (PFN_vkCmdWriteTimestamp2)get2(dev, "vkCmdWriteTimestamp2", "vkCmdWriteTimestamp2KHR");
    int ok = p_begin && p_end && p_barrier2 && p_ts2;
    ps2_log("vk: Vulkan 1.3 commands from the device: %s", ok ? "all four" : "MISSING (the renderer will crash)");
    return ok ? 0 : -1;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) { p_begin(cb, ri); }
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cb) { p_end(cb); }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) { p_barrier2(cb, di); }
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 st, VkQueryPool qp, uint32_t q) {
    p_ts2(cb, st, qp, q);
}
