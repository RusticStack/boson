#include <boson/gpu/vulkan_interop.hpp>

#include <boson/core/result.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace boson::gpu {

using Microsoft::WRL::ComPtr;

namespace {

constexpr VkFormat kVkFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kDxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

struct Loader {
    HMODULE dll = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
    VulkanSupport support;
};

const Loader& loader() {
    static const Loader instance = []() {
        Loader l;
        // System32 only: vulkan-1.dll is installed there by every Vulkan driver (no DLL search-path planting).
        l.dll = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (l.dll == nullptr) {
            return l;
        }
        l.get_instance_proc_addr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(l.dll, "vkGetInstanceProcAddr")));
        if (l.get_instance_proc_addr == nullptr) {
            return l;
        }
        l.support.loader = true;
        l.support.api_version = VK_API_VERSION_1_0;
        const auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            l.get_instance_proc_addr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        if (enumerate_version != nullptr) {
            std::uint32_t version = 0;
            if (enumerate_version(&version) == VK_SUCCESS) {
                l.support.api_version = version;
            }
        }
        return l;
    }();
    return instance;
}

template <typename Fn>
void load(Fn& fn, PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* name) {
    fn = reinterpret_cast<Fn>(gipa(instance, name));
}

template <typename Fn>
void load_device(Fn& fn, PFN_vkGetDeviceProcAddr gdpa, VkDevice device, const char* name) {
    fn = reinterpret_cast<Fn>(gdpa(device, name));
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::ranges::any_of(extensions, [name](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

} // namespace

std::string VulkanSupport::version_string() const {
    if (!loader) {
        return {};
    }
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u", VK_API_VERSION_MAJOR(api_version), VK_API_VERSION_MINOR(api_version),
                  VK_API_VERSION_PATCH(api_version));
    return buffer;
}

const VulkanSupport& vulkan_support() {
    return loader().support;
}

// ---- VulkanInterop ---------------------------------------------------------------------------------------

std::unique_ptr<VulkanInterop> VulkanInterop::create(std::shared_ptr<Device> device, std::string* why_not) {
    std::string reason;
    auto interop = std::make_unique<VulkanInterop>(PassKey{});
    if (!device || !interop->initialize(std::move(device), reason)) {
        if (why_not != nullptr) {
            *why_not = reason.empty() ? "no device" : reason;
        }
        return nullptr;
    }
    return interop;
}

bool VulkanInterop::initialize(std::shared_ptr<Device> device, std::string& why_not) {
    m_d3d = std::move(device);
    const Loader& l = loader();
    if (!l.support.loader) {
        why_not = "vulkan-1.dll not found";
        return false;
    }
    if (l.support.api_version < VK_API_VERSION_1_2) {
        why_not = "Vulkan 1.2 loader required";
        return false;
    }
    m_fn.vkGetInstanceProcAddr = l.get_instance_proc_addr;
    const auto gipa = l.get_instance_proc_addr;
    PFN_vkCreateInstance create_instance = nullptr;
    load(create_instance, gipa, VK_NULL_HANDLE, "vkCreateInstance");
    if (create_instance == nullptr) {
        why_not = "vkCreateInstance missing";
        return false;
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Boson";
    app.pEngineName = "Boson gpu";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    if (create_instance(&instance_info, nullptr, &m_instance) != VK_SUCCESS) {
        why_not = "vkCreateInstance failed";
        return false;
    }
    load(m_fn.vkDestroyInstance, gipa, m_instance, "vkDestroyInstance");
    load(m_fn.vkEnumeratePhysicalDevices, gipa, m_instance, "vkEnumeratePhysicalDevices");
    load(m_fn.vkGetPhysicalDeviceProperties2, gipa, m_instance, "vkGetPhysicalDeviceProperties2");
    load(m_fn.vkGetPhysicalDeviceQueueFamilyProperties, gipa, m_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    load(m_fn.vkGetPhysicalDeviceMemoryProperties, gipa, m_instance, "vkGetPhysicalDeviceMemoryProperties");
    load(m_fn.vkGetPhysicalDeviceImageFormatProperties2, gipa, m_instance, "vkGetPhysicalDeviceImageFormatProperties2");
    load(m_fn.vkGetPhysicalDeviceExternalSemaphoreProperties, gipa, m_instance, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    load(m_fn.vkEnumerateDeviceExtensionProperties, gipa, m_instance, "vkEnumerateDeviceExtensionProperties");
    load(m_fn.vkGetPhysicalDeviceFeatures2, gipa, m_instance, "vkGetPhysicalDeviceFeatures2");
    load(m_fn.vkCreateDevice, gipa, m_instance, "vkCreateDevice");
    load(m_fn.vkGetDeviceProcAddr, gipa, m_instance, "vkGetDeviceProcAddr");
    if (m_fn.vkEnumeratePhysicalDevices == nullptr || m_fn.vkGetPhysicalDeviceProperties2 == nullptr ||
        m_fn.vkCreateDevice == nullptr || m_fn.vkGetDeviceProcAddr == nullptr) {
        why_not = "Vulkan 1.1 instance functions missing";
        return false;
    }

    // The physical device of the D3D adapter, matched by LUID.
    const std::uint64_t luid = m_d3d->info().adapter.luid;
    std::uint32_t count = 0;
    m_fn.vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physical(count);
    m_fn.vkEnumeratePhysicalDevices(m_instance, &count, physical.data());
    for (VkPhysicalDevice candidate : physical) {
        VkPhysicalDeviceIDProperties id{};
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &id;
        m_fn.vkGetPhysicalDeviceProperties2(candidate, &props);
        LUID vk_luid{};
        std::memcpy(&vk_luid, id.deviceLUID, sizeof(vk_luid));
        const std::uint64_t value = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(vk_luid.HighPart)) << 32) | vk_luid.LowPart;
        if (id.deviceLUIDValid == VK_TRUE && value == luid && props.properties.apiVersion >= VK_API_VERSION_1_2) {
            m_physical = candidate;
            m_device_name = props.properties.deviceName;
            break;
        }
    }
    if (m_physical == VK_NULL_HANDLE) {
        why_not = "no Vulkan 1.2 device matches the D3D adapter LUID";
        return false;
    }

    m_fn.vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    m_fn.vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, extensions.data());
    if (!has_extension(extensions, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)) {
        why_not = "VK_KHR_external_memory_win32 unsupported";
        return false;
    }
    std::vector<const char*> enabled{VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME};
    const bool semaphore_win32 = has_extension(extensions, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
    if (semaphore_win32) {
        enabled.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
    }

    m_fn.vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    m_fn.vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &count, families.data());
    bool found_queue = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            m_queue_family = i;
            found_queue = true;
            break;
        }
    }
    if (!found_queue) {
        why_not = "no graphics queue";
        return false;
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &timeline;
    m_fn.vkGetPhysicalDeviceFeatures2(m_physical, &features);

    if (semaphore_win32 && timeline.timelineSemaphore == VK_TRUE && m_fn.vkGetPhysicalDeviceExternalSemaphoreProperties != nullptr) {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkPhysicalDeviceExternalSemaphoreInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
        info.pNext = &type;
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        VkExternalSemaphoreProperties props{};
        props.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
        m_fn.vkGetPhysicalDeviceExternalSemaphoreProperties(m_physical, &info, &props);
        m_timeline_import = (props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = m_queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceTimelineSemaphoreFeatures enable_timeline{};
    enable_timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    enable_timeline.timelineSemaphore = timeline.timelineSemaphore;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &enable_timeline;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    device_info.ppEnabledExtensionNames = enabled.data();
    if (m_fn.vkCreateDevice(m_physical, &device_info, nullptr, &m_device) != VK_SUCCESS) {
        why_not = "vkCreateDevice failed";
        return false;
    }

    const auto gdpa = m_fn.vkGetDeviceProcAddr;
#define BOSON_VK_LOAD(name) load_device(m_fn.name, gdpa, m_device, #name)
    BOSON_VK_LOAD(vkDestroyDevice);
    BOSON_VK_LOAD(vkGetDeviceQueue);
    BOSON_VK_LOAD(vkDeviceWaitIdle);
    BOSON_VK_LOAD(vkCreateImage);
    BOSON_VK_LOAD(vkDestroyImage);
    BOSON_VK_LOAD(vkGetImageMemoryRequirements);
    BOSON_VK_LOAD(vkAllocateMemory);
    BOSON_VK_LOAD(vkFreeMemory);
    BOSON_VK_LOAD(vkBindImageMemory);
    BOSON_VK_LOAD(vkGetMemoryWin32HandlePropertiesKHR);
    BOSON_VK_LOAD(vkCreateImageView);
    BOSON_VK_LOAD(vkDestroyImageView);
    BOSON_VK_LOAD(vkCreateRenderPass);
    BOSON_VK_LOAD(vkDestroyRenderPass);
    BOSON_VK_LOAD(vkCreateFramebuffer);
    BOSON_VK_LOAD(vkDestroyFramebuffer);
    BOSON_VK_LOAD(vkCreateCommandPool);
    BOSON_VK_LOAD(vkDestroyCommandPool);
    BOSON_VK_LOAD(vkAllocateCommandBuffers);
    BOSON_VK_LOAD(vkResetCommandBuffer);
    BOSON_VK_LOAD(vkBeginCommandBuffer);
    BOSON_VK_LOAD(vkEndCommandBuffer);
    BOSON_VK_LOAD(vkCmdPipelineBarrier);
    BOSON_VK_LOAD(vkCmdBeginRenderPass);
    BOSON_VK_LOAD(vkCmdEndRenderPass);
    BOSON_VK_LOAD(vkCmdClearAttachments);
    BOSON_VK_LOAD(vkCreateSemaphore);
    BOSON_VK_LOAD(vkDestroySemaphore);
    BOSON_VK_LOAD(vkImportSemaphoreWin32HandleKHR);
    BOSON_VK_LOAD(vkCreateFence);
    BOSON_VK_LOAD(vkDestroyFence);
    BOSON_VK_LOAD(vkWaitForFences);
    BOSON_VK_LOAD(vkResetFences);
    BOSON_VK_LOAD(vkQueueSubmit);
#undef BOSON_VK_LOAD
    if (m_fn.vkGetMemoryWin32HandlePropertiesKHR == nullptr || m_fn.vkQueueSubmit == nullptr || m_fn.vkCreateImage == nullptr) {
        why_not = "Vulkan device functions missing";
        return false;
    }
    if (m_fn.vkImportSemaphoreWin32HandleKHR == nullptr) {
        m_timeline_import = false;
    }
    m_fn.vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);
    if (m_fn.vkGetPhysicalDeviceMemoryProperties != nullptr) {
        m_fn.vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memory_props);
    }
    return true;
}

VulkanInterop::~VulkanInterop() {
    if (m_device != VK_NULL_HANDLE) {
        if (m_fn.vkDeviceWaitIdle != nullptr) {
            m_fn.vkDeviceWaitIdle(m_device);
        }
        if (m_fn.vkDestroyDevice != nullptr) {
            m_fn.vkDestroyDevice(m_device, nullptr);
        }
    }
    if (m_instance != VK_NULL_HANDLE && m_fn.vkDestroyInstance != nullptr) {
        m_fn.vkDestroyInstance(m_instance, nullptr);
    }
}

std::unique_ptr<SharedImage> VulkanInterop::create_image(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return nullptr;
    }
    auto image = std::make_unique<SharedImage>(SharedImageKey{}, *this, width, height);
    if (!image->create()) {
        return nullptr;
    }
    return image;
}

// ---- SharedImage -----------------------------------------------------------------------------------------

SharedImage::SharedImage(SharedImageKey, VulkanInterop& owner, std::uint32_t width, std::uint32_t height)
    : m_owner(owner)
    , m_width(width)
    , m_height(height) {
    m_owner.d3d_device()->add_resource_owner(this);
}

SharedImage::~SharedImage() {
    m_owner.d3d_device()->remove_resource_owner(this);
    const auto guard = m_owner.d3d_device()->lock();
    destroy_vulkan();
    m_srv.Reset();
    m_d3d11_texture.Reset();
    m_d3d11_fence.Reset();
    m_d3d12_resource.Reset();
    m_d3d12_fence.Reset();
}

void SharedImage::destroy_vulkan() noexcept {
    const VulkanFunctions& f = m_owner.fn();
    const VkDevice device = m_owner.device();
    if (device == VK_NULL_HANDLE) {
        return;
    }
    if (m_cpu_fence != VK_NULL_HANDLE && m_cpu_fence_pending) {
        f.vkWaitForFences(device, 1, &m_cpu_fence, VK_TRUE, 2'000'000'000ull);
    }
    m_cpu_fence_pending = false;
    if (m_framebuffer != VK_NULL_HANDLE) f.vkDestroyFramebuffer(device, m_framebuffer, nullptr);
    if (m_render_pass != VK_NULL_HANDLE) f.vkDestroyRenderPass(device, m_render_pass, nullptr);
    if (m_view != VK_NULL_HANDLE) f.vkDestroyImageView(device, m_view, nullptr);
    if (m_image != VK_NULL_HANDLE) f.vkDestroyImage(device, m_image, nullptr);
    if (m_memory != VK_NULL_HANDLE) f.vkFreeMemory(device, m_memory, nullptr);
    if (m_pool != VK_NULL_HANDLE) f.vkDestroyCommandPool(device, m_pool, nullptr);
    if (m_timeline != VK_NULL_HANDLE) f.vkDestroySemaphore(device, m_timeline, nullptr);
    if (m_cpu_fence != VK_NULL_HANDLE) f.vkDestroyFence(device, m_cpu_fence, nullptr);
    m_framebuffer = VK_NULL_HANDLE;
    m_render_pass = VK_NULL_HANDLE;
    m_view = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_memory = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_cmd = VK_NULL_HANDLE;
    m_timeline = VK_NULL_HANDLE;
    m_cpu_fence = VK_NULL_HANDLE;
}

void SharedImage::release_device_resources() noexcept {
    destroy_vulkan();
    m_srv.Reset();
    m_d3d11_texture.Reset();
    m_d3d11_fence.Reset();
    m_d3d12_resource.Reset();
    m_d3d12_fence.Reset();
    m_generation = 0;
}

bool SharedImage::create_d3d_side(HANDLE& memory_handle, VkExternalMemoryHandleTypeFlagBits& handle_type, HANDLE& fence_handle) {
    Device& device = *m_owner.d3d_device();
    if (device.backend() == Backend::D3D12) {
        ID3D12Device* d3d12 = device.d3d12();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = m_width;
        desc.Height = m_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kDxgiFormat;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (!BOSON_HR_CHECK(d3d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                           nullptr, IID_PPV_ARGS(&m_d3d12_resource))) ||
            !BOSON_HR_CHECK(d3d12->CreateSharedHandle(m_d3d12_resource.Get(), nullptr, GENERIC_ALL, nullptr, &memory_handle))) {
            return false;
        }
        handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        D3D11_RESOURCE_FLAGS flags{};
        flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (!BOSON_HR_CHECK(device.d3d11on12()->CreateWrappedResource(m_d3d12_resource.Get(), &flags, D3D12_RESOURCE_STATE_COMMON,
                                                                       D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&m_d3d11_texture)))) {
            return false;
        }
        if (SUCCEEDED(d3d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_d3d12_fence)))) {
            if (FAILED(d3d12->CreateSharedHandle(m_d3d12_fence.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle))) {
                fence_handle = nullptr;
            }
        }
    } else {
        ID3D11Device* d3d11 = device.d3d11();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = kDxgiFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        ComPtr<IDXGIResource1> resource;
        if (!BOSON_HR_CHECK(d3d11->CreateTexture2D(&desc, nullptr, &m_d3d11_texture)) ||
            !BOSON_HR_CHECK(m_d3d11_texture.As(&resource)) ||
            !BOSON_HR_CHECK(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                                         &memory_handle))) {
            return false;
        }
        handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        ComPtr<ID3D11Device5> device5;
        if (SUCCEEDED(device.d3d11()->QueryInterface(IID_PPV_ARGS(&device5))) && device.d3d11_context4() != nullptr &&
            SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_d3d11_fence)))) {
            if (FAILED(m_d3d11_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle))) {
                fence_handle = nullptr;
            }
        }
    }
    return BOSON_HR_CHECK(device.d3d11()->CreateShaderResourceView(m_d3d11_texture.Get(), nullptr, &m_srv));
}

bool SharedImage::create() {
    Device& device = *m_owner.d3d_device();
    const auto guard = device.lock();
    const VulkanFunctions& f = m_owner.fn();
    const VkDevice vk = m_owner.device();

    HANDLE memory_handle = nullptr;
    HANDLE fence_handle = nullptr;
    VkExternalMemoryHandleTypeFlagBits handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    const auto close_handles = [&]() {
        if (memory_handle != nullptr) {
            CloseHandle(memory_handle);
            memory_handle = nullptr;
        }
        if (fence_handle != nullptr) {
            CloseHandle(fence_handle);
            fence_handle = nullptr;
        }
    };
    if (!create_d3d_side(memory_handle, handle_type, fence_handle)) {
        close_handles();
        return false;
    }

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkExternalMemoryImageCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = handle_type;
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kVkFormat;
    image_info.extent = VkExtent3D{m_width, m_height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (f.vkCreateImage(vk, &image_info, nullptr, &m_image) != VK_SUCCESS) {
        close_handles();
        return false;
    }

    VkMemoryWin32HandlePropertiesKHR handle_props{};
    handle_props.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
    f.vkGetMemoryWin32HandlePropertiesKHR(vk, handle_type, memory_handle, &handle_props);
    VkMemoryRequirements requirements{};
    f.vkGetImageMemoryRequirements(vk, m_image, &requirements);
    std::uint32_t bits = requirements.memoryTypeBits & (handle_props.memoryTypeBits != 0 ? handle_props.memoryTypeBits : ~0u);
    std::uint32_t type_index = UINT32_MAX;
    const VkPhysicalDeviceMemoryProperties& mem = m_owner.m_memory_props;
    for (std::uint32_t i = 0; i < mem.memoryTypeCount && i < 32; ++i) {
        if ((bits & (1u << i)) != 0 && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            type_index = i;
            break;
        }
    }
    for (std::uint32_t i = 0; type_index == UINT32_MAX && i < 32; ++i) {
        if ((bits & (1u << i)) != 0) {
            type_index = i;
        }
    }
    if (type_index == UINT32_MAX) {
        close_handles();
        destroy_vulkan();
        return false;
    }
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = m_image;
    VkImportMemoryWin32HandleInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    import_info.pNext = &dedicated;
    import_info.handleType = handle_type;
    import_info.handle = memory_handle;
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.pNext = &import_info;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = type_index;
    const VkResult imported = f.vkAllocateMemory(vk, &alloc, nullptr, &m_memory);
    if (memory_handle != nullptr) {
        CloseHandle(memory_handle); // importing an NT handle does not transfer its ownership
        memory_handle = nullptr;
    }
    if (imported != VK_SUCCESS || f.vkBindImageMemory(vk, m_image, m_memory, 0) != VK_SUCCESS) {
        close_handles();
        destroy_vulkan();
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = m_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kVkFormat;
    view_info.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkAttachmentDescription attachment{};
    attachment.format = kVkFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    VkRenderPassCreateInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = m_owner.queue_family();
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (f.vkCreateImageView(vk, &view_info, nullptr, &m_view) != VK_SUCCESS ||
        f.vkCreateRenderPass(vk, &pass_info, nullptr, &m_render_pass) != VK_SUCCESS ||
        f.vkCreateCommandPool(vk, &pool_info, nullptr, &m_pool) != VK_SUCCESS ||
        f.vkCreateFence(vk, &fence_info, nullptr, &m_cpu_fence) != VK_SUCCESS) {
        close_handles();
        destroy_vulkan();
        return false;
    }
    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = m_render_pass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &m_view;
    fb_info.width = m_width;
    fb_info.height = m_height;
    fb_info.layers = 1;
    VkCommandBufferAllocateInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = m_pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    if (f.vkCreateFramebuffer(vk, &fb_info, nullptr, &m_framebuffer) != VK_SUCCESS ||
        f.vkAllocateCommandBuffers(vk, &cmd_info, &m_cmd) != VK_SUCCESS) {
        close_handles();
        destroy_vulkan();
        return false;
    }

    // Shared fence -> Vulkan timeline semaphore (GPU-side synchronization in both directions).
    if (fence_handle != nullptr && m_owner.timeline_import_supported()) {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sem_info.pNext = &type;
        if (f.vkCreateSemaphore(vk, &sem_info, nullptr, &m_timeline) == VK_SUCCESS) {
            VkImportSemaphoreWin32HandleInfoKHR import_sem{};
            import_sem.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
            import_sem.semaphore = m_timeline;
            import_sem.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
            import_sem.handle = fence_handle;
            if (f.vkImportSemaphoreWin32HandleKHR(vk, &import_sem) != VK_SUCCESS) {
                f.vkDestroySemaphore(vk, m_timeline, nullptr);
                m_timeline = VK_NULL_HANDLE;
            }
        }
    }
    close_handles();
    m_value = 0;
    m_first_use = true;
    m_generation = device.generation() + 1;
    return true;
}

bool SharedImage::begin_vulkan() {
    if (m_generation != m_owner.d3d_device()->generation() + 1 || m_cmd == VK_NULL_HANDLE) {
        return false;
    }
    const VulkanFunctions& f = m_owner.fn();
    const VkDevice vk = m_owner.device();
    if (m_cpu_fence_pending) {
        f.vkWaitForFences(vk, 1, &m_cpu_fence, VK_TRUE, 2'000'000'000ull);
        f.vkResetFences(vk, 1, &m_cpu_fence);
        m_cpu_fence_pending = false;
    }
    f.vkResetCommandBuffer(m_cmd, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return f.vkBeginCommandBuffer(m_cmd, &begin) == VK_SUCCESS;
}

void SharedImage::clear_rects(const float background[4], std::span<const VulkanClearRect> rects) {
    const VulkanFunctions& f = m_owner.fn();
    VkClearValue clear{};
    std::memcpy(clear.color.float32, background, sizeof(float) * 4);
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = m_render_pass;
    pass.framebuffer = m_framebuffer;
    pass.renderArea = VkRect2D{{0, 0}, {m_width, m_height}};
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    f.vkCmdBeginRenderPass(m_cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    for (const VulkanClearRect& r : rects) {
        const std::int32_t x0 = std::clamp<std::int32_t>(r.x, 0, static_cast<std::int32_t>(m_width));
        const std::int32_t y0 = std::clamp<std::int32_t>(r.y, 0, static_cast<std::int32_t>(m_height));
        const std::int32_t x1 = std::clamp<std::int32_t>(r.x + static_cast<std::int32_t>(r.width), 0, static_cast<std::int32_t>(m_width));
        const std::int32_t y1 = std::clamp<std::int32_t>(r.y + static_cast<std::int32_t>(r.height), 0, static_cast<std::int32_t>(m_height));
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        VkClearAttachment attachment{};
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachment.colorAttachment = 0;
        std::memcpy(attachment.clearValue.color.float32, r.rgba, sizeof(float) * 4);
        VkClearRect rect{};
        rect.rect = VkRect2D{{x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
        rect.layerCount = 1;
        f.vkCmdClearAttachments(m_cmd, 1, &attachment, 1, &rect);
    }
    f.vkCmdEndRenderPass(m_cmd);
}

bool SharedImage::end_vulkan() {
    if (m_cmd == VK_NULL_HANDLE) {
        return false;
    }
    const VulkanFunctions& f = m_owner.fn();
    // Hand the image to Direct3D: GENERAL layout, released to the external queue family.
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = m_owner.queue_family();
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.image = m_image;
    barrier.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    if (f.vkEndCommandBuffer(m_cmd) != VK_SUCCESS) {
        return false;
    }
    m_first_use = false;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_cmd;
    VkTimelineSemaphoreSubmitInfo timeline{};
    const std::uint64_t wait_value = m_value;      // Direct3D's last release (0 = nothing yet)
    const std::uint64_t signal_value = m_value + 1;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (m_timeline != VK_NULL_HANDLE) {
        timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline.waitSemaphoreValueCount = 1;
        timeline.pWaitSemaphoreValues = &wait_value;
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &signal_value;
        submit.pNext = &timeline;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &m_timeline;
        submit.pWaitDstStageMask = &wait_stage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &m_timeline;
    }
    VkResult result = VK_SUCCESS;
    {
        const auto queue_guard = m_owner.lock_queue();
        result = f.vkQueueSubmit(m_owner.queue(), 1, &submit, m_cpu_fence);
    }
    if (result != VK_SUCCESS) {
        return false;
    }
    m_cpu_fence_pending = true;
    m_value = signal_value;
    if (m_timeline == VK_NULL_HANDLE) {
        // CPU fallback: Direct3D may only read once Vulkan is done.
        f.vkWaitForFences(m_owner.device(), 1, &m_cpu_fence, VK_TRUE, 2'000'000'000ull);
    }
    return true;
}

bool SharedImage::acquire_d3d() {
    Device& device = *m_owner.d3d_device();
    if (m_generation != device.generation() + 1) {
        return false;
    }
    if (device.backend() == Backend::D3D12) {
        if (m_timeline != VK_NULL_HANDLE && m_d3d12_fence) {
            BOSON_HR_CHECK(device.d3d12_queue()->Wait(m_d3d12_fence.Get(), m_value));
        }
        ID3D11Resource* wrapped[] = {m_d3d11_texture.Get()};
        device.d3d11on12()->AcquireWrappedResources(wrapped, 1);
        return true;
    }
    if (m_timeline != VK_NULL_HANDLE && m_d3d11_fence) {
        return BOSON_HR_CHECK(device.d3d11_context4()->Wait(m_d3d11_fence.Get(), m_value));
    }
    return true;
}

bool SharedImage::release_d3d() {
    Device& device = *m_owner.d3d_device();
    if (m_generation != device.generation() + 1) {
        return false;
    }
    const std::uint64_t value = m_value + 1;
    if (device.backend() == Backend::D3D12) {
        ID3D11Resource* wrapped[] = {m_d3d11_texture.Get()};
        device.d3d11on12()->ReleaseWrappedResources(wrapped, 1);
        device.d3d11_context()->Flush();
        if (m_timeline != VK_NULL_HANDLE && m_d3d12_fence) {
            if (!BOSON_HR_CHECK(device.d3d12_queue()->Signal(m_d3d12_fence.Get(), value))) {
                return false;
            }
            m_value = value;
            return true;
        }
    } else if (m_timeline != VK_NULL_HANDLE && m_d3d11_fence) {
        if (!BOSON_HR_CHECK(device.d3d11_context4()->Signal(m_d3d11_fence.Get(), value))) {
            return false;
        }
        device.d3d11_context()->Flush();
        m_value = value;
        return true;
    }
    device.wait_idle(); // CPU fallback: Vulkan may overwrite the image only after Direct3D finished reading
    return true;
}

} // namespace boson::gpu
