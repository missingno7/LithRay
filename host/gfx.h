// Vulkan device layer for the host: instance/device selection, allocation, upload helpers, swapchain.
#pragma once
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "volk.h"
#include "vk_mem_alloc.h"
#include <vector>
#include <string>
#include <stdint.h>

void Log(const char *fmt, ...);
[[noreturn]] void Fatal(const char *fmt, ...);
#define VKC(expr) do { VkResult r_ = (expr); if (r_ != VK_SUCCESS) Fatal("%s failed: %d (%s:%d)", #expr, (int)r_, __FILE__, __LINE__); } while (0)

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE; VmaAllocation alloc = nullptr; void *mapped = nullptr;
    VkDeviceSize size = 0; VkDeviceAddress addr = 0;
};
struct Image {
    VkImage img = VK_NULL_HANDLE; VmaAllocation alloc = nullptr; VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED; uint32_t width = 0, height = 0, mips = 1;
};
struct Accel { VkAccelerationStructureKHR as = VK_NULL_HANDLE; Buffer buffer; VkDeviceAddress addr = 0; };

struct Gfx {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VmaAllocator vma = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    bool rt = false;
    char deviceName[128] = {};
    float timestampPeriod = 1;

    // swapchain
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent = {};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    bool vsync = true;

    void init();
    void shutdown();
    bool createSwapchain(HWND hwnd, bool vsyncOn);     // (re)creates; false while the window has no area
    void destroySwapchain(bool surfaceToo);

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void destroy(Buffer &b);
    Image createImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, uint32_t mips = 1, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
    void destroy(Image &i);
    void destroy(Accel &a);

    VkCommandBuffer beginOnce();
    void endOnce(VkCommandBuffer cmd);                  // submits and waits
    void uploadBuffer(Buffer &dst, const void *data, VkDeviceSize size, VkDeviceSize dstOffset = 0);
    // RGBA8 mip chain, tightly packed top mip first.
    void uploadImage(Image &img, const uint8_t *pixels, uint32_t mipsProvided);

    // One-geometry triangle BLAS from device-local buffers (positions: float3 at vertexStride).
    Accel buildBlas(VkCommandBuffer cmd, VkDeviceAddress vertexAddr, uint32_t vertexStride, uint32_t maxVertex,
                    VkDeviceAddress indexAddr, uint32_t triangleCount, bool fastBuild, Buffer &scratch, bool opaque = true);
    VkShaderModule shader(const uint32_t *code, size_t bytes);
};

void transition(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                uint32_t mips = VK_REMAINING_MIP_LEVELS);
