#pragma once

#include <windows.h>
#include <wrl.h>
#include <d3d11_4.h>
#include <d3d12.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <boson/gpu/device.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace boson::gpu {

/// @brief What the Vulkan loader on this machine offers. Loading vulkan-1.dll is lazy (first call) and
/// optional: Boson has no link-time Vulkan dependency.
struct VulkanSupport {
    bool loader = false;            ///< vulkan-1.dll could be loaded
    std::uint32_t api_version = 0;  ///< vkEnumerateInstanceVersion
    [[nodiscard]] std::string version_string() const;
};

[[nodiscard]] const VulkanSupport& vulkan_support();

/// The Vulkan entry points VulkanInterop uses, loaded through vkGetInstanceProcAddr / vkGetDeviceProcAddr.
/// Applications use them (or load their own from get_instance_proc_addr) to record into a SharedImage.
struct VulkanFunctions {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 vkGetPhysicalDeviceImageFormatProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties vkGetPhysicalDeviceExternalSemaphoreProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkCreateDevice vkCreateDevice = nullptr;
    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
    PFN_vkCreateImage vkCreateImage = nullptr;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory = nullptr;
    PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR = nullptr;
    PFN_vkCreateImageView vkCreateImageView = nullptr;
    PFN_vkDestroyImageView vkDestroyImageView = nullptr;
    PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
    PFN_vkCmdClearAttachments vkCmdClearAttachments = nullptr;
    PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
    PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
    PFN_vkImportSemaphoreWin32HandleKHR vkImportSemaphoreWin32HandleKHR = nullptr;
    PFN_vkCreateFence vkCreateFence = nullptr;
    PFN_vkDestroyFence vkDestroyFence = nullptr;
    PFN_vkWaitForFences vkWaitForFences = nullptr;
    PFN_vkResetFences vkResetFences = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
};

class VulkanInterop;

/// Only VulkanInterop creates SharedImages (make_unique needs a public constructor).
class SharedImageKey {
    friend class VulkanInterop;
    explicit SharedImageKey() = default;
};

/// A rectangle of one color for SharedImage::clear_rects (the self-contained demo / test "renderer").
struct VulkanClearRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float rgba[4]{0, 0, 0, 1};
};

/// @brief A B8G8R8A8 image shared between Vulkan and Direct3D. The D3D side owns the memory - a D3D11 texture
/// (D3D11_RESOURCE_MISC_SHARED_NTHANDLE) on the D3D11 backend, a shared committed ID3D12Resource on D3D12 -
/// and exports it as an NT handle that Vulkan imports (VK_KHR_external_memory_win32, dedicated allocation).
/// Synchronization is GPU-side through one shared fence (ID3D11Fence / ID3D12Fence) imported as a Vulkan
/// timeline semaphore (VK_KHR_external_semaphore_win32): Vulkan signals 2n+1 when it finished writing,
/// Direct3D waits for it, reads, and signals 2n+2, which the next Vulkan submit waits on. When the driver
/// cannot import the fence, a CPU fallback (vkWaitForFences / Device::wait_idle) is used (see fence_shared()).
///
/// Frame protocol: begin_vulkan() -> record into command_buffer() -> end_vulkan() (any thread, Vulkan side);
/// then on the D3D side with Device::lock() held: acquire_d3d() -> sample / copy texture() -> release_d3d().
class SharedImage final : public DeviceResourceOwner {
public:
    SharedImage(SharedImageKey key, VulkanInterop& owner, std::uint32_t width, std::uint32_t height);
    ~SharedImage();

    SharedImage(const SharedImage&) = delete;
    SharedImage& operator=(const SharedImage&) = delete;
    SharedImage(SharedImage&&) = delete;
    SharedImage& operator=(SharedImage&&) = delete;

    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] VkImage image() const noexcept { return m_image; }
    [[nodiscard]] VkImageView view() const noexcept { return m_view; }
    [[nodiscard]] VkRenderPass render_pass() const noexcept { return m_render_pass; } ///< loadOp CLEAR, 1 color attachment
    [[nodiscard]] VkFramebuffer framebuffer() const noexcept { return m_framebuffer; }
    [[nodiscard]] VkCommandBuffer command_buffer() const noexcept { return m_cmd; }
    [[nodiscard]] bool fence_shared() const noexcept { return m_timeline != VK_NULL_HANDLE; }
    [[nodiscard]] bool valid() const noexcept { return m_image != VK_NULL_HANDLE && m_d3d11_texture != nullptr; }

    /// @brief Waits until the previous submission of this image finished on the CPU timeline, then begins
    /// command_buffer() (one-time submit). false when the image is not usable (device lost).
    bool begin_vulkan();
    /// @brief Records the layout transition for Direct3D, submits waiting for D3D's last release and signals
    /// the shared fence.
    bool end_vulkan();
    /// @brief Convenience renderer: render pass clearing to background, then each rect with its color.
    void clear_rects(const float background[4], std::span<const VulkanClearRect> rects);

    /// @brief D3D side: makes the D3D queue wait for Vulkan's last end_vulkan() (GPU wait). Device lock held.
    bool acquire_d3d();
    /// @brief D3D side: signals that D3D finished reading (the next Vulkan submit waits for it).
    bool release_d3d();
    /// @brief The image as a D3D11 texture (D3D11On12 wrapped on the D3D12 backend; valid between
    /// acquire_d3d() and release_d3d() there) and a shader resource view of it for Renderer2D::draw_texture.
    [[nodiscard]] ID3D11Texture2D* texture() const noexcept { return m_d3d11_texture.Get(); }
    [[nodiscard]] ID3D11ShaderResourceView* srv() const noexcept { return m_srv.Get(); }
    [[nodiscard]] ID3D12Resource* d3d12_resource() const noexcept { return m_d3d12_resource.Get(); }

    void release_device_resources() noexcept override;

private:
    friend class VulkanInterop;
    bool create();
    bool create_d3d_side(HANDLE& memory_handle, VkExternalMemoryHandleTypeFlagBits& handle_type, HANDLE& fence_handle);
    void destroy_vulkan() noexcept;

    VulkanInterop& m_owner;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint64_t m_generation = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_d3d11_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_d3d11_fence;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_d3d12_resource;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_d3d12_fence;

    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkRenderPass m_render_pass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    VkFence m_cpu_fence = VK_NULL_HANDLE;
    bool m_cpu_fence_pending = false;
    bool m_first_use = true;
    std::uint64_t m_value = 0; ///< last value signaled on the shared fence
};

/// @brief A Vulkan instance + device on the same physical GPU as a gpu::Device (matched by adapter LUID), with
/// the Win32 external memory / semaphore extensions enabled. Its images present through the same Surface
/// path as native D3D content: draw SharedImage::srv() with Renderer2D (or copy texture()) in a frame callback.
class VulkanInterop final {
    struct PassKey {
        explicit PassKey() = default;
    };

public:
    explicit VulkanInterop(PassKey) {}
    /// @brief nullptr when vulkan-1.dll is missing, no Vulkan 1.2 device matches the adapter LUID, or the
    /// external memory extensions are unavailable. Does not throw.
    [[nodiscard]] static std::unique_ptr<VulkanInterop> create(std::shared_ptr<Device> device, std::string* why_not = nullptr);
    ~VulkanInterop();

    VulkanInterop(const VulkanInterop&) = delete;
    VulkanInterop& operator=(const VulkanInterop&) = delete;
    VulkanInterop(VulkanInterop&&) = delete;
    VulkanInterop& operator=(VulkanInterop&&) = delete;

    [[nodiscard]] std::unique_ptr<SharedImage> create_image(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] VkInstance instance() const noexcept { return m_instance; }
    [[nodiscard]] VkPhysicalDevice physical_device() const noexcept { return m_physical; }
    [[nodiscard]] VkDevice device() const noexcept { return m_device; }
    [[nodiscard]] VkQueue queue() const noexcept { return m_queue; }
    [[nodiscard]] std::uint32_t queue_family() const noexcept { return m_queue_family; }
    [[nodiscard]] const VulkanFunctions& fn() const noexcept { return m_fn; }
    [[nodiscard]] const std::shared_ptr<Device>& d3d_device() const noexcept { return m_d3d; }
    [[nodiscard]] const std::string& device_name() const noexcept { return m_device_name; }
    [[nodiscard]] bool timeline_import_supported() const noexcept { return m_timeline_import; }
    /// @brief Serializes vkQueueSubmit (VkQueue is externally synchronized).
    [[nodiscard]] std::unique_lock<std::mutex> lock_queue() { return std::unique_lock{m_queue_mutex}; }

private:
    bool initialize(std::shared_ptr<Device> device, std::string& why_not);

    std::shared_ptr<Device> m_d3d;
    VulkanFunctions m_fn;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    std::uint32_t m_queue_family = 0;
    VkPhysicalDeviceMemoryProperties m_memory_props{};
    std::string m_device_name;
    bool m_timeline_import = false;
    std::mutex m_queue_mutex;

    friend class SharedImage;
};

} // namespace boson::gpu
