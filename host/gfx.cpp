#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include "gfx.h"
#include <algorithm>
#include <string.h>

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT *d, void *) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) Log("[vk] %s", d->pMessage);
    return VK_FALSE;
}
static VkDebugUtilsMessengerEXT g_messenger;

void Gfx::init() {
    VKC(volkInitialize());
    bool validate = GetEnvironmentVariableA("LITHRAY_VALIDATE", nullptr, 0) != 0;
    std::vector<const char *> layers, iext = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    {   // HDR10 colour spaces are only listed with this extension
        uint32_t en = 0; vkEnumerateInstanceExtensionProperties(nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ex(en); vkEnumerateInstanceExtensionProperties(nullptr, &en, ex.data());
        for (auto &e : ex) if (!strcmp(e.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) iext.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }
    if (validate) {
        uint32_t n = 0; vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> lp(n); vkEnumerateInstanceLayerProperties(&n, lp.data());
        bool have = false;
        for (auto &l : lp) if (!strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) have = true;
        if (have) { layers.push_back("VK_LAYER_KHRONOS_validation"); iext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }
        else { Log("validation requested but VK_LAYER_KHRONOS_validation is not installed"); validate = false; }
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "LithRay"; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = (uint32_t)layers.size(); ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = (uint32_t)iext.size(); ici.ppEnabledExtensionNames = iext.data();
    VKC(vkCreateInstance(&ici, nullptr, &instance));
    volkLoadInstance(instance);
    if (validate) {
        VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        mi.pfnUserCallback = DebugCb;
        vkCreateDebugUtilsMessengerEXT(instance, &mi, nullptr, &g_messenger);
    }

    // Pick the device: ray tracing first, then discrete.
    uint32_t n = 0; vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(instance, &n, devs.data());
    int best = -1, bestScore = -1; bool bestRt = false;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t en = 0; vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ex(en); vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &en, ex.data());
        auto has = [&](const char *name) { for (auto &e : ex) if (!strcmp(e.extensionName, name)) return true; return false; };
        if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        bool r = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && has(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                 has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        int score = (r ? 100 : 0) + (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 10 : 0);
        Log("device %u: %s%s", i, p.deviceName, r ? " (ray tracing)" : "");
        if (score > bestScore) { bestScore = score; best = (int)i; bestRt = r; }
    }
    if (best < 0) Fatal("no Vulkan 1.3 device found");
    phys = devs[best]; rt = bestRt;
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys, &pp);
    strncpy(deviceName, pp.deviceName, sizeof(deviceName) - 1);
    timestampPeriod = pp.limits.timestampPeriod;

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
    qfam = ~0u;
    for (uint32_t i = 0; i < qn; i++)
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && vkGetPhysicalDeviceWin32PresentationSupportKHR(phys, i)) { qfam = i; break; }
    if (qfam == ~0u) Fatal("no graphics+present queue");

    std::vector<const char *> dext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (rt) {
        dext.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        dext.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        dext.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    }
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.fragmentStoresAndAtomics = VK_TRUE;       // per-pixel transparency lists are written from fragment shaders
    f2.features.samplerAnisotropy = VK_TRUE; f2.features.shaderInt16 = VK_FALSE; f2.features.independentBlend = VK_TRUE;
    f2.features.depthClamp = VK_TRUE; f2.features.depthBiasClamp = VK_TRUE; f2.features.wideLines = VK_FALSE;
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE; f12.descriptorIndexing = VK_TRUE; f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE; f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE; f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fas = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    fas.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR frq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    frq.rayQuery = VK_TRUE;
    f2.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13;
    if (rt) { f13.pNext = &fas; fas.pNext = &frq; }

    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dext.size(); dci.ppEnabledExtensionNames = dext.data();
    VKC(vkCreateDevice(phys, &dci, nullptr, &dev));
    volkLoadDevice(dev);
    vkGetDeviceQueue(dev, qfam, 0, &queue);

    VmaVulkanFunctions vf = {};
    VmaAllocatorCreateInfo aci = {};
    aci.physicalDevice = phys; aci.device = dev; aci.instance = instance; aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    VKC(vmaImportVulkanFunctionsFromVolk(&aci, &vf));
    aci.pVulkanFunctions = &vf;
    VKC(vmaCreateAllocator(&aci, &vma));

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = qfam;
    VKC(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    Log("using %s, ray tracing %s", deviceName, rt ? "on" : "unavailable");
}

void Gfx::shutdown() {
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    destroySwapchain(true);
    vkDestroyCommandPool(dev, pool, nullptr);
    vmaDestroyAllocator(vma);
    vkDestroyDevice(dev, nullptr);
    if (g_messenger) vkDestroyDebugUtilsMessengerEXT(instance, g_messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
    dev = VK_NULL_HANDLE;
}

void Gfx::destroySwapchain(bool surfaceToo) {
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    for (auto v : swapViews) vkDestroyImageView(dev, v, nullptr);
    swapViews.clear(); swapImages.clear();
    if (swapchain) { vkDestroySwapchainKHR(dev, swapchain, nullptr); swapchain = VK_NULL_HANDLE; }
    if (surfaceToo && surface) { vkDestroySurfaceKHR(instance, surface, nullptr); surface = VK_NULL_HANDLE; }
    extent = {};
}

bool Gfx::createSwapchain(HWND hwnd, bool vsyncOn, bool wantHdr) {
    vkDeviceWaitIdle(dev);
    if (!surface) {
        VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleA(nullptr); sci.hwnd = hwnd;
        VKC(vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface));
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, qfam, surface, &ok);
        if (!ok) Fatal("the device cannot present to the window");
    }
    VkSurfaceCapabilitiesKHR caps;
    VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;

    uint32_t fn = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn); vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, fmts.data());
    VkSurfaceFormatKHR fmt = fmts[0];
    // preference: 10 bits per channel (no banding in smooth gradients), then 8; HDR10 only on request
    for (auto &f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt = f;
    for (auto &f : fmts) if (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt = f;
    hdrAvailable = false;
    for (auto &f : fmts) if (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) { hdrAvailable = true; if (wantHdr) fmt = f; }
    hdrWanted = wantHdr;
    hdrOutput = fmt.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
    uint32_t pn = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, nullptr);
    std::vector<VkPresentModeKHR> pms(pn); vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, pms.data());
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsyncOn) {
        for (auto m : pms) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) pm = m;
        for (auto m : pms) if (m == VK_PRESENT_MODE_MAILBOX_KHR) pm = m;
    }
    VkSwapchainKHR old = swapchain;
    for (auto v : swapViews) vkDestroyImageView(dev, v, nullptr);
    swapViews.clear();

    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = fmt.format; ci.imageColorSpace = fmt.colorSpace; ci.imageExtent = caps.currentExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; ci.presentMode = pm; ci.clipped = VK_TRUE; ci.oldSwapchain = old;
    VKC(vkCreateSwapchainKHR(dev, &ci, nullptr, &swapchain));
    if (old) vkDestroySwapchainKHR(dev, old, nullptr);
    swapFormat = fmt.format; extent = caps.currentExtent; vsync = vsyncOn;
    uint32_t in = 0; vkGetSwapchainImagesKHR(dev, swapchain, &in, nullptr);
    swapImages.resize(in); vkGetSwapchainImagesKHR(dev, swapchain, &in, swapImages.data());
    for (auto img : swapImages) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = swapFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v; VKC(vkCreateImageView(dev, &vi, nullptr, &v));
        swapViews.push_back(v);
    }
    Log("swapchain %ux%u, %u images, present mode %d, format %d%s%s", extent.width, extent.height, in, (int)pm, (int)swapFormat,
        hdrOutput ? " HDR10" : "", (!hdrOutput && hdrAvailable) ? " (HDR10 available: lr_HDR 1)" : "");
    return true;
}

Buffer Gfx::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
    Buffer b; b.size = size;
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size ? size : 16; ci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;
    VKC(vmaCreateBuffer(vma, &ci, &ai, &b.buf, &b.alloc, &info));
    b.mapped = hostVisible ? info.pMappedData : nullptr;
    VkBufferDeviceAddressInfo da = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b.buf;
    b.addr = vkGetBufferDeviceAddress(dev, &da);
    return b;
}
void Gfx::destroy(Buffer &b) { if (b.buf) vmaDestroyBuffer(vma, b.buf, b.alloc); b = Buffer(); }

Image Gfx::createImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, uint32_t mips, VkImageAspectFlags aspect) {
    Image i; i.width = w; i.height = h; i.format = format; i.mips = mips;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = {w, h, 1}; ci.mipLevels = mips; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    VmaAllocationCreateInfo ai = {}; ai.usage = VMA_MEMORY_USAGE_AUTO;
    VKC(vmaCreateImage(vma, &ci, &ai, &i.img, &i.alloc, nullptr));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format; vi.subresourceRange = {aspect, 0, mips, 0, 1};
    VKC(vkCreateImageView(dev, &vi, nullptr, &i.view));
    return i;
}
void Gfx::destroy(Image &i) {
    if (i.view) vkDestroyImageView(dev, i.view, nullptr);
    if (i.img) vmaDestroyImage(vma, i.img, i.alloc);
    i = Image();
}
void Gfx::destroy(Accel &a) {
    if (a.as) vkDestroyAccelerationStructureKHR(dev, a.as, nullptr);
    destroy(a.buffer);
    a = Accel();
}

VkCommandBuffer Gfx::beginOnce() {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cmd; VKC(vkAllocateCommandBuffers(dev, &ai, &cmd));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}
void Gfx::endOnce(VkCommandBuffer cmd) {
    VKC(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VKC(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

void Gfx::uploadBuffer(Buffer &dst, const void *data, VkDeviceSize size, VkDeviceSize dstOffset) {
    if (!size) return;
    if (dst.mapped) { memcpy((uint8_t *)dst.mapped + dstOffset, data, size); return; }
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(staging.mapped, data, size);
    VkCommandBuffer cmd = beginOnce();
    VkBufferCopy c = {0, dstOffset, size};
    vkCmdCopyBuffer(cmd, staging.buf, dst.buf, 1, &c);
    endOnce(cmd);
    destroy(staging);
}

void transition(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect, uint32_t mips) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to; b.image = img;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {aspect, 0, mips, 0, 1};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

void Gfx::uploadImage(Image &img, const uint8_t *pixels, uint32_t mipsProvided) {
    VkDeviceSize total = 0;
    for (uint32_t m = 0; m < mipsProvided; m++) total += (VkDeviceSize)std::max(1u, img.width >> m) * std::max(1u, img.height >> m) * 4;
    Buffer staging = createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(staging.mapped, pixels, total);
    VkCommandBuffer cmd = beginOnce();
    transition(cmd, img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkDeviceSize off = 0;
    for (uint32_t m = 0; m < mipsProvided; m++) {
        uint32_t w = std::max(1u, img.width >> m), h = std::max(1u, img.height >> m);
        VkBufferImageCopy r = {};
        r.bufferOffset = off; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1}; r.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, staging.buf, img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        off += (VkDeviceSize)w * h * 4;
    }
    transition(cmd, img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    endOnce(cmd);
    destroy(staging);
}

Accel Gfx::buildBlas(VkCommandBuffer cmd, VkDeviceAddress vertexAddr, uint32_t vertexStride, uint32_t maxVertex,
                     VkDeviceAddress indexAddr, uint32_t triangleCount, bool fastBuild, Buffer &scratch, bool opaque) {
    VkAccelerationStructureGeometryKHR geo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
    auto &t = geo.geometry.triangles;
    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; t.vertexData.deviceAddress = vertexAddr; t.vertexStride = vertexStride;
    t.maxVertex = maxVertex; t.indexType = VK_INDEX_TYPE_UINT32; t.indexData.deviceAddress = indexAddr;
    VkAccelerationStructureBuildGeometryInfoKHR bi = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = fastBuild ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; bi.geometryCount = 1; bi.pGeometries = &geo;
    VkAccelerationStructureBuildSizesInfoKHR sz = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &triangleCount, &sz);

    Accel a;
    a.buffer = createBuffer(sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
    VkAccelerationStructureCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = a.buffer.buf; ci.size = sz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VKC(vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &a.as));
    if (scratch.size < sz.buildScratchSize + 256) {
        destroy(scratch);
        scratch = createBuffer(sz.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    }
    bi.dstAccelerationStructure = a.as;
    bi.scratchData.deviceAddress = (scratch.addr + 255) & ~(VkDeviceAddress)255;
    VkAccelerationStructureBuildRangeInfoKHR range = {triangleCount, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR *pr = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bi, &pr);
    VkAccelerationStructureDeviceAddressInfoKHR ai = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = a.as;
    a.addr = vkGetAccelerationStructureDeviceAddressKHR(dev, &ai);
    return a;
}

VkShaderModule Gfx::shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes; ci.pCode = code;
    VkShaderModule m; VKC(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}
