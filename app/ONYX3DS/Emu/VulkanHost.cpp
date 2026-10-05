// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include <stdlib.h>
#include "Emu/VulkanHost.h"

#include "Platform/Log.h"

namespace onyx::app {

// ---------------------------------------------------------------------------
// DozenDriver

typedef VkResult(VKAPI_PTR* PFN_IcdNegotiate)(uint32_t* version);

DozenDriver::~DozenDriver() {
    if (!module_) return;
    // Unhook first: the driver must not call into our logger while it unloads.
    using SetHookFn = void(__cdecl*)(void (*)(int, const char*));
    if (const auto set_hook =
            reinterpret_cast<SetHookFn>(GetProcAddress(module_, "onyx_set_log_hook")))
        set_hook(nullptr);
    FreeLibrary(module_);
}

bool DozenDriver::Load(std::string& error) {
    if (gipa_) return true;
    module_ = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
    if (!module_) {
        error = "vulkan_dzn.dll is missing from the app package (error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    // Before anything else talks to the driver, so instance creation is logged too.
    InstallDriverLogHook(module_);
    // Loader/ICD interface: negotiate first, then ask for the entry point.
    // Version 5 is what Mesa implements; it may lower it.
    if (auto negotiate = reinterpret_cast<PFN_IcdNegotiate>(
            GetProcAddress(module_, "vk_icdNegotiateLoaderICDInterfaceVersion"))) {
        uint32_t version = 5;
        if (negotiate(&version) != VK_SUCCESS) {
            error = "Dozen rejected the ICD interface negotiation";
            return false;
        }
        icd_version_ = version;
    }
    gipa_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(module_, "vk_icdGetInstanceProcAddr"));
    if (!gipa_) {
        error = "vulkan_dzn.dll has no vk_icdGetInstanceProcAddr export";
        return false;
    }
    ONYX_INFO("Dozen loaded (ICD interface v%u)", icd_version_);
    return true;
}

// ---------------------------------------------------------------------------
// Entry points

struct VulkanHost::Fn {
    // global / instance
    PFN_vkCreateInstance CreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    // device
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkCmdBlitImage CmdBlitImage = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkImportSemaphoreWin32HandleKHR ImportSemaphoreWin32HandleKHR = nullptr;
    PFN_vkGetMemoryWin32HandlePropertiesKHR GetMemoryWin32HandlePropertiesKHR = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges = nullptr;
};

namespace {

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

const char* VkResultName(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    default: return "VkResult error";
    }
}

} // namespace

VulkanHost::VulkanHost(DozenDriver& driver, D3D12Presenter& presenter)
    : driver_(driver), presenter_(presenter), fn_(std::make_unique<Fn>()) {
    iface_.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    iface_.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    iface_.handle = this;
    iface_.set_image = &SetImage;
    iface_.get_sync_index = &GetSyncIndex;
    iface_.get_sync_index_mask = &GetSyncIndexMask;
    iface_.set_command_buffers = &SetCommandBuffers;
    iface_.wait_sync_index = &WaitSyncIndex;
    iface_.lock_queue = &LockQueue;
    iface_.unlock_queue = &UnlockQueue;
    iface_.set_signal_semaphore = &SetSignalSemaphore;
}

VulkanHost::~VulkanHost() {
    DestroyDevice();
    if (instance_ && fn_->DestroyInstance) fn_->DestroyInstance(instance_, nullptr);
}

bool VulkanHost::CreateInstance(VulkanProbe& probe, std::string& error) {
    if (instance_) return true;
    const PFN_vkGetInstanceProcAddr gipa = driver_.GetInstanceProcAddr();
    probe.driver_loaded = gipa != nullptr;
    if (!gipa) {
        error = "Vulkan driver not loaded";
        return false;
    }
    auto& f = *fn_;
    f.CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    f.EnumerateInstanceExtensionProperties =
        reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (!f.CreateInstance || !f.EnumerateInstanceExtensionProperties) {
        error = "Dozen did not expose vkCreateInstance";
        return false;
    }

    uint32_t n = 0;
    f.EnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> inst_exts(n);
    f.EnumerateInstanceExtensionProperties(nullptr, &n, inst_exts.data());
    std::vector<const char*> enable;
    for (const char* want : {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                             VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
                             VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME}) {
        if (HasExtension(inst_exts, want)) enable.push_back(want);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ONYX 3DS";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "Azahar";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    ci.ppEnabledExtensionNames = enable.data();
    const VkResult r = f.CreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        error = std::string("vkCreateInstance failed: ") + VkResultName(r);
        return false;
    }
    probe.instance_created = true;

#define LOAD_I(name) f.name = reinterpret_cast<PFN_vk##name>(gipa(instance_, "vk" #name))
    LOAD_I(DestroyInstance);
    LOAD_I(EnumeratePhysicalDevices);
    LOAD_I(GetPhysicalDeviceProperties);
    LOAD_I(GetPhysicalDeviceProperties2);
    LOAD_I(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(GetPhysicalDeviceMemoryProperties);
    LOAD_I(EnumerateDeviceExtensionProperties);
    LOAD_I(CreateDevice);
    LOAD_I(GetDeviceProcAddr);
#undef LOAD_I
    if (!f.GetPhysicalDeviceProperties2)
        f.GetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            gipa(instance_, "vkGetPhysicalDeviceProperties2KHR"));

    uint32_t count = 0;
    f.EnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    f.EnumeratePhysicalDevices(instance_, &count, gpus.data());
    if (gpus.empty()) {
        error = "Dozen found no D3D12 GPU";
        return false;
    }
    probe.device_found = true;

    // Prefer the adapter the presenter uses (same LUID), so shared handles
    // open on both devices. On a console there is only one anyway.
    const LUID want = presenter_.AdapterLuid();
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id;
        f.GetPhysicalDeviceProperties2(gpu, &props);
        if (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        if (id.deviceLUIDValid && std::memcmp(id.deviceLUID, &want, sizeof(LUID)) == 0) {
            gpu_ = gpu;
            probe.luid_matched = true;
            break;
        }
        if (!gpu_) gpu_ = gpu;
    }
    if (!gpu_) gpu_ = gpus.front();

    VkPhysicalDeviceProperties props{};
    f.GetPhysicalDeviceProperties(gpu_, &props);
    probe.device_name = props.deviceName;
    probe.api_version = props.apiVersion;
    char info[96];
    std::snprintf(info, sizeof(info), "Vulkan %u.%u.%u, driver 0x%08X",
                  VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                  VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
    probe.driver_info = info;
    f.GetPhysicalDeviceMemoryProperties(gpu_, &memory_props_);

    uint32_t qn = 0;
    f.GetPhysicalDeviceQueueFamilyProperties(gpu_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    f.GetPhysicalDeviceQueueFamilyProperties(gpu_, &qn, qf.data());
    for (uint32_t i = 0; i < qn; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queue_family_ = i;
            break;
        }
    }

    f.EnumerateDeviceExtensionProperties(gpu_, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> dev_exts(n);
    f.EnumerateDeviceExtensionProperties(gpu_, nullptr, &n, dev_exts.data());
    has_external_memory_ = HasExtension(dev_exts, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) &&
                           HasExtension(dev_exts, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    has_external_semaphore_ =
        HasExtension(dev_exts, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) &&
        HasExtension(dev_exts, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
    has_timeline_ = HasExtension(dev_exts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    probe.external_memory = has_external_memory_;
    probe.external_fence = has_external_semaphore_;
    probe.timeline = has_timeline_;

    // Extensions the frontend needs on the core's device.
    device_extensions_.clear();
    for (const char* want_ext :
         {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
          VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
          VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
          VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME}) {
        if (HasExtension(dev_exts, want_ext)) device_extensions_.emplace_back(want_ext);
    }
    if (readback_) {
        // Nothing is shared with D3D12: no external memory/semaphores, and no timeline
        // semaphores (Azahar then synchronises with plain fences).
        device_extensions_.clear();
        ONYX_INFO("Vulkan frames: copied back to memory (no D3D12 sharing)");
    } else if (!has_external_memory_) {
        error = "Dozen lacks VK_KHR_external_memory_win32, frames cannot reach the screen";
        probe.notes.push_back(error);
        return false;
    }
    if (!readback_ && (!has_external_semaphore_ || !has_timeline_))
        probe.notes.push_back("No shared fence support: falling back to CPU frame sync (slower)");
    ONYX_INFO("Vulkan GPU: %s (%s)", probe.device_name.c_str(), probe.driver_info.c_str());
    // A failure here leaves the hardware renderer unavailable, so the session runs games
    // with the software renderer instead of starting the core on a broken driver.
    if (!RunSelfTest(error)) {
        probe.notes.push_back(error);
        return false;
    }
    return true;
}

void VulkanHost::SetNegotiationInterface(
    const retro_hw_render_context_negotiation_interface_vulkan* iface) {
    negotiation_ = iface;
}

bool VulkanHost::CreateDevice(std::string& error) {
    if (device_) return true;
    auto& f = *fn_;
    // On Series S the DXIL validator refuses Dozen's shaders (see the self-test lines);
    // patches/mesa/0008 then signs them itself so the driver gets to compile them.
    // Only for the game's device: the start-up self-test runs plain validation.
    _putenv_s("ONYX_DXIL_SELFSIGN", "1");
    ONYX_INFO("Vulkan: shaders the DXIL validator refuses are signed by ONYX");
    std::vector<const char*> exts;
    for (const auto& e : device_extensions_) exts.push_back(e.c_str());

    if (negotiation_ && negotiation_->interface_type ==
                            RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN &&
        negotiation_->create_device) {
        retro_vulkan_context ctx{};
        const bool ok = negotiation_->create_device(
            &ctx, instance_, gpu_, VK_NULL_HANDLE, driver_.GetInstanceProcAddr(), exts.data(),
            static_cast<unsigned>(exts.size()), nullptr, 0, nullptr);
        if (!ok || !ctx.device) {
            error = "The emulator could not create its Vulkan device on Dozen";
            return false;
        }
        device_ = ctx.device;
        queue_ = ctx.queue;
        queue_family_ = ctx.queue_family_index;
        gpu_ = ctx.gpu;
    } else {
        // Plain device for cores that do not negotiate (not Azahar today).
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = queue_family_;
        q.queueCount = 1;
        q.pQueuePriorities = &prio;
        VkPhysicalDeviceTimelineSemaphoreFeaturesKHR tl{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR};
        tl.timelineSemaphore = has_timeline_;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = has_timeline_ ? &tl : nullptr;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &q;
        dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        dci.ppEnabledExtensionNames = exts.data();
        const VkResult r = f.CreateDevice(gpu_, &dci, nullptr, &device_);
        if (r != VK_SUCCESS) {
            error = std::string("vkCreateDevice failed: ") + VkResultName(r);
            return false;
        }
    }
    if (!LoadDeviceFunctions()) {
        error = "Missing Vulkan device functions";
        return false;
    }
    if (!queue_) f.GetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    f.CreateCommandPool(device_, &pci, nullptr, &pool_);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kSyncFrames;
    f.AllocateCommandBuffers(device_, &cai, cmd_.data());
    for (auto& fence : fences_) {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        f.CreateFence(device_, &fci, nullptr, &fence);
    }
    fence_pending_.fill(false);

    gpu_sync_ = readback_ ? false : ImportSharedFence();
    ONYX_INFO("Vulkan device ready; frame sync on %s",
              readback_ ? "CPU (readback)" : gpu_sync_ ? "GPU (shared fence)" : "CPU");

    iface_.instance = instance_;
    iface_.gpu = gpu_;
    iface_.device = device_;
    iface_.get_device_proc_addr = f.GetDeviceProcAddr;
    iface_.get_instance_proc_addr = driver_.GetInstanceProcAddr();
    iface_.queue = queue_;
    iface_.queue_index = queue_family_;
    return true;
}

bool VulkanHost::LoadDeviceFunctions() {
    auto& f = *fn_;
#define LOAD_D(name) f.name = reinterpret_cast<PFN_vk##name>(f.GetDeviceProcAddr(device_, "vk" #name))
    LOAD_D(DestroyDevice);
    LOAD_D(GetDeviceQueue);
    LOAD_D(DeviceWaitIdle);
    LOAD_D(CreateCommandPool);
    LOAD_D(DestroyCommandPool);
    LOAD_D(AllocateCommandBuffers);
    LOAD_D(ResetCommandBuffer);
    LOAD_D(BeginCommandBuffer);
    LOAD_D(EndCommandBuffer);
    LOAD_D(CmdPipelineBarrier);
    LOAD_D(CmdCopyImage);
    LOAD_D(CmdBlitImage);
    LOAD_D(QueueSubmit);
    LOAD_D(CreateFence);
    LOAD_D(DestroyFence);
    LOAD_D(WaitForFences);
    LOAD_D(ResetFences);
    LOAD_D(CreateImage);
    LOAD_D(DestroyImage);
    LOAD_D(GetImageMemoryRequirements);
    LOAD_D(AllocateMemory);
    LOAD_D(FreeMemory);
    LOAD_D(BindImageMemory);
    LOAD_D(CreateSemaphore);
    LOAD_D(DestroySemaphore);
    LOAD_D(ImportSemaphoreWin32HandleKHR);
    LOAD_D(GetMemoryWin32HandlePropertiesKHR);
    LOAD_D(CreateBuffer);
    LOAD_D(DestroyBuffer);
    LOAD_D(GetBufferMemoryRequirements);
    LOAD_D(BindBufferMemory);
    LOAD_D(MapMemory);
    LOAD_D(UnmapMemory);
    LOAD_D(CmdCopyImageToBuffer);
    LOAD_D(InvalidateMappedMemoryRanges);
#undef LOAD_D
    return f.QueueSubmit && f.CmdCopyImage && f.CreateImage && f.AllocateMemory;
}

bool VulkanHost::ImportSharedFence() {
    auto& f = *fn_;
    const HANDLE handle = presenter_.SharedFenceHandle();
    if (!handle || !has_timeline_ || !has_external_semaphore_ || !f.ImportSemaphoreWin32HandleKHR)
        return false;
    VkSemaphoreTypeCreateInfoKHR type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
    type.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &type;
    if (f.CreateSemaphore(device_, &sci, nullptr, &shared_timeline_) != VK_SUCCESS) return false;
    VkImportSemaphoreWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    imp.semaphore = shared_timeline_;
    imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    imp.handle = handle;
    const VkResult r = f.ImportSemaphoreWin32HandleKHR(device_, &imp);
    if (r != VK_SUCCESS) {
        ONYX_WARN("Importing the presenter fence failed (%s)", VkResultName(r));
        f.DestroySemaphore(device_, shared_timeline_, nullptr);
        shared_timeline_ = VK_NULL_HANDLE;
        return false;
    }
    timeline_value_ = 0;
    return true;
}

uint32_t VulkanHost::FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memory_props_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory_props_.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    for (uint32_t i = 0; i < memory_props_.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    return 0;
}

void VulkanHost::ReleaseSlot(SlotImport& s) {
    auto& f = *fn_;
    if (s.image) f.DestroyImage(device_, s.image, nullptr);
    if (s.memory) f.FreeMemory(device_, s.memory, nullptr);
    s = SlotImport{};
}

bool VulkanHost::ImportSlot(int index, const D3D12Presenter::SharedSlot& shared) {
    auto& f = *fn_;
    SlotImport& s = imports_[index];
    ReleaseSlot(s);

    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {shared.width, shared.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (f.CreateImage(device_, &ici, nullptr, &s.image) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    f.GetImageMemoryRequirements(device_, s.image, &req);
    uint32_t type_bits = req.memoryTypeBits;
    if (f.GetMemoryWin32HandlePropertiesKHR) {
        VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        if (f.GetMemoryWin32HandlePropertiesKHR(device_,
                                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
                                                shared.shared_handle, &hp) == VK_SUCCESS &&
            hp.memoryTypeBits)
            type_bits &= hp.memoryTypeBits;
    }

    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = s.image;
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    imp.pNext = &dedicated;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    imp.handle = shared.shared_handle;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &imp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkResult r = f.AllocateMemory(device_, &mai, nullptr, &s.memory);
    if (r == VK_SUCCESS) r = f.BindImageMemory(device_, s.image, s.memory, 0);
    if (r != VK_SUCCESS) {
        ONYX_ERROR("Importing frame texture %d failed: %s", index, VkResultName(r));
        ReleaseSlot(s);
        return false;
    }
    return true;
}

void VulkanHost::OnFrame(unsigned width, unsigned height) {
    if (!device_ || !have_image_) return;
    if (readback_) {
        OnFrameReadback(width, height);
        return;
    }
    auto& f = *fn_;

    // The core's output image; video_refresh gives the visible size, which
    // starts at the image origin (libretro convention).
    const VkImage src = image_.create_info.image;
    const VkExtent3D src_extent{width, height, 1};
    if (!src || width == 0 || height == 0) return;

    // This sync index's command buffer and fence must be idle before reuse.
    const uint32_t idx = sync_index_;
    if (fence_pending_[idx]) {
        f.WaitForFences(device_, 1, &fences_[idx], VK_TRUE, UINT64_MAX);
        f.ResetFences(device_, 1, &fences_[idx]);
        fence_pending_[idx] = false;
    }

    int slot_index = -1;
    D3D12Presenter::SharedSlot* shared =
        presenter_.BeginWrite(src_extent.width, src_extent.height, slot_index);
    if (!shared) return;
    SlotImport& dst = imports_[slot_index];
    const uint32_t gen = presenter_.SlotGeneration(slot_index);
    if (dst.generation != gen || !dst.image) {
        if (!ImportSlot(slot_index, *shared)) {
            presenter_.AbortWrite(slot_index);
            return;
        }
        dst.generation = gen;
    }

    VkCommandBuffer cb = cmd_[idx];
    f.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f.BeginCommandBuffer(cb, &bi);

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier pre[2]{};
    pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    pre[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pre[0].oldLayout = image_.image_layout;
    pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[0].image = src;
    pre[0].subresourceRange = range;
    pre[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre[1].srcAccessMask = 0;
    pre[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pre[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    pre[1].dstQueueFamilyIndex = queue_family_;
    pre[1].image = dst.image;
    pre[1].subresourceRange = range;
    f.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 2, pre);

    if (image_.create_info.format == VK_FORMAT_R8G8B8A8_UNORM) {
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = src_extent;
        f.CmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    } else {
        // BGRA (or anything else): a blit converts the channel order.
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(src_extent.width),
                              static_cast<int32_t>(src_extent.height), 1};
        blit.dstOffsets[1] = blit.srcOffsets[1];
        f.CmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }

    VkImageMemoryBarrier post[2]{};
    post[0] = pre[0];
    post[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post[0].newLayout = image_.image_layout; // hand it back the way the core left it
    // Release the shared texture to D3D12.
    post[1] = pre[1];
    post[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    post[1].dstAccessMask = 0;
    post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    post[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    post[1].srcQueueFamilyIndex = queue_family_;
    post[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    f.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 2, post);
    f.EndCommandBuffer(cb);

    // Submit: any command buffers the core handed us first, then the copy.
    std::vector<VkCommandBuffer> cbs = core_cmds_;
    cbs.push_back(cb);
    core_cmds_.clear();

    std::vector<VkSemaphore> signals;
    std::vector<uint64_t> signal_values;
    uint64_t ready_value = 0;
    if (gpu_sync_) {
        ready_value = ++timeline_value_;
        signals.push_back(shared_timeline_);
        signal_values.push_back(ready_value);
    }
    if (signal_semaphore_) {
        signals.push_back(signal_semaphore_);
        signal_values.push_back(0); // binary: value ignored
        signal_semaphore_ = VK_NULL_HANDLE;
    }
    std::vector<uint64_t> wait_values(wait_semaphores_.size(), 0);
    VkTimelineSemaphoreSubmitInfoKHR tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR};
    tsi.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size());
    tsi.pWaitSemaphoreValues = wait_values.data();
    tsi.signalSemaphoreValueCount = static_cast<uint32_t>(signal_values.size());
    tsi.pSignalSemaphoreValues = signal_values.data();

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = gpu_sync_ ? &tsi : nullptr;
    si.waitSemaphoreCount = static_cast<uint32_t>(wait_semaphores_.size());
    si.pWaitSemaphores = wait_semaphores_.data();
    si.pWaitDstStageMask = wait_stages_.data();
    si.commandBufferCount = static_cast<uint32_t>(cbs.size());
    si.pCommandBuffers = cbs.data();
    si.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
    si.pSignalSemaphores = signals.data();

    VkResult r;
    {
        std::lock_guard lock(queue_mutex_);
        r = f.QueueSubmit(queue_, 1, &si, fences_[idx]);
    }
    wait_semaphores_.clear();
    wait_stages_.clear();
    if (r != VK_SUCCESS) {
        ONYX_ERROR("Frame copy submit failed: %s", VkResultName(r));
        presenter_.AbortWrite(slot_index);
        return;
    }
    fence_pending_[idx] = true;

    if (!gpu_sync_) {
        // CPU fallback: the presenter samples as soon as we publish.
        f.WaitForFences(device_, 1, &fences_[idx], VK_TRUE, UINT64_MAX);
    }
    presenter_.EndWrite(slot_index, ready_value);
    sync_index_ = (sync_index_ + 1) % kSyncFrames;
}

bool VulkanHost::EnsureReadback(Readback& rb, VkDeviceSize size) {
    auto& f = *fn_;
    if (rb.buffer && rb.size >= size) return true;
    if (rb.buffer) {
        if (rb.mapped) f.UnmapMemory(device_, rb.memory);
        f.DestroyBuffer(device_, rb.buffer, nullptr);
        f.FreeMemory(device_, rb.memory, nullptr);
        rb = {};
    }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (f.CreateBuffer(device_, &bci, nullptr, &rb.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    f.GetBufferMemoryRequirements(device_, rb.buffer, &req);
    // Host-visible memory, cached if there is such a type (CPU reads it every frame).
    uint32_t type = UINT32_MAX;
    for (const VkMemoryPropertyFlags want :
         {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
          VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)}) {
        for (uint32_t i = 0; i < memory_props_.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (memory_props_.memoryTypes[i].propertyFlags & want) == want)
                type = i;
        if (type != UINT32_MAX) break;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || f.AllocateMemory(device_, &mai, nullptr, &rb.memory) != VK_SUCCESS ||
        f.BindBufferMemory(device_, rb.buffer, rb.memory, 0) != VK_SUCCESS ||
        f.MapMemory(device_, rb.memory, 0, VK_WHOLE_SIZE, 0, &rb.mapped) != VK_SUCCESS) {
        ONYX_ERROR("Vulkan readback buffer (%llu bytes) could not be created",
                   static_cast<unsigned long long>(size));
        if (rb.buffer) f.DestroyBuffer(device_, rb.buffer, nullptr);
        if (rb.memory) f.FreeMemory(device_, rb.memory, nullptr);
        rb = {};
        return false;
    }
    rb.size = size;
    return true;
}

void VulkanHost::ShowReadback(uint32_t index) {
    auto& f = *fn_;
    Readback& rb = readbacks_[index];
    if (!rb.ready) return;
    rb.ready = false;
    if (fence_pending_[index]) {
        f.WaitForFences(device_, 1, &fences_[index], VK_TRUE, UINT64_MAX);
        f.ResetFences(device_, 1, &fences_[index]);
        fence_pending_[index] = false;
    }
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = rb.memory;
    range.size = VK_WHOLE_SIZE;
    f.InvalidateMappedMemoryRanges(device_, 1, &range);
    const size_t pitch = static_cast<size_t>(rb.width) * 4;
    if (rb.bgra) {
        presenter_.PushCpuFrame(rb.mapped, rb.width, rb.height, pitch);
        return;
    }
    // RGBA -> BGRA for the presenter.
    readback_pixels_.resize(pitch * rb.height);
    const auto* src = static_cast<const uint32_t*>(rb.mapped);
    auto* dst = reinterpret_cast<uint32_t*>(readback_pixels_.data());
    const size_t n = static_cast<size_t>(rb.width) * rb.height;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t p = src[i];
        dst[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
    }
    presenter_.PushCpuFrame(readback_pixels_.data(), rb.width, rb.height, pitch);
}

void VulkanHost::OnFrameReadback(unsigned width, unsigned height) {
    auto& f = *fn_;
    const VkImage src = image_.create_info.image;
    if (!src || width == 0 || height == 0) return;
    const uint32_t idx = sync_index_;
    // This index's previous frame must be finished (and shown) before reuse.
    ShowReadback(idx);
    if (fence_pending_[idx]) {
        f.WaitForFences(device_, 1, &fences_[idx], VK_TRUE, UINT64_MAX);
        f.ResetFences(device_, 1, &fences_[idx]);
        fence_pending_[idx] = false;
    }
    Readback& rb = readbacks_[idx];
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
    if (!EnsureReadback(rb, bytes)) return;

    VkCommandBuffer cb = cmd_[idx];
    f.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f.BeginCommandBuffer(cb, &bi);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier pre{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    pre.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    pre.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pre.oldLayout = image_.image_layout;
    pre.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.image = src;
    pre.subresourceRange = range;
    f.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &pre);
    VkBufferImageCopy copy{};
    copy.bufferRowLength = width;
    copy.bufferImageHeight = height;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    f.CmdCopyImageToBuffer(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &copy);
    VkImageMemoryBarrier post = pre;
    post.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post.newLayout = image_.image_layout;
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = rb.buffer;
    host.size = VK_WHOLE_SIZE;
    f.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &host, 1, &post);
    f.EndCommandBuffer(cb);

    std::vector<VkCommandBuffer> cbs = core_cmds_;
    cbs.push_back(cb);
    core_cmds_.clear();
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = static_cast<uint32_t>(wait_semaphores_.size());
    si.pWaitSemaphores = wait_semaphores_.data();
    si.pWaitDstStageMask = wait_stages_.data();
    si.commandBufferCount = static_cast<uint32_t>(cbs.size());
    si.pCommandBuffers = cbs.data();
    VkSemaphore signal = signal_semaphore_;
    signal_semaphore_ = VK_NULL_HANDLE;
    si.signalSemaphoreCount = signal ? 1 : 0;
    si.pSignalSemaphores = signal ? &signal : nullptr;
    VkResult r;
    {
        std::lock_guard lock(queue_mutex_);
        r = f.QueueSubmit(queue_, 1, &si, fences_[idx]);
    }
    wait_semaphores_.clear();
    wait_stages_.clear();
    if (r != VK_SUCCESS) {
        static int s_logged = 0;
        if (s_logged++ < 8) ONYX_ERROR("Frame readback submit failed: %s", VkResultName(r));
        return;
    }
    fence_pending_[idx] = true;
    rb.width = width;
    rb.height = height;
    rb.bgra = image_.create_info.format == VK_FORMAT_B8G8R8A8_UNORM ||
              image_.create_info.format == VK_FORMAT_B8G8R8A8_SRGB;
    rb.ready = true;
    sync_index_ = (sync_index_ + 1) % kSyncFrames;
    // Show the previous frame now (its copy has most likely finished while this
    // frame was emulated), keeping one frame of latency instead of a GPU stall.
    ShowReadback(sync_index_);
}

void VulkanHost::DestroyDevice() {
    if (!device_) return;
    auto& f = *fn_;
    f.DeviceWaitIdle(device_);
    for (auto& s : imports_) ReleaseSlot(s);
    for (auto& fence : fences_)
        if (fence) f.DestroyFence(device_, fence, nullptr);
    fences_.fill(VK_NULL_HANDLE);
    if (pool_) f.DestroyCommandPool(device_, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    for (auto& rb : readbacks_) {
        if (rb.mapped) f.UnmapMemory(device_, rb.memory);
        if (rb.buffer) f.DestroyBuffer(device_, rb.buffer, nullptr);
        if (rb.memory) f.FreeMemory(device_, rb.memory, nullptr);
        rb = {};
    }
    if (shared_timeline_) f.DestroySemaphore(device_, shared_timeline_, nullptr);
    shared_timeline_ = VK_NULL_HANDLE;
    // The device belongs to the frontend (Azahar's negotiation interface has
    // no destroy_device), so we tear it down after the core is unloaded.
    f.DestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    have_image_ = false;
    gpu_sync_ = false;
}

// --- libretro callbacks ------------------------------------------------------

void VulkanHost::SetImage(void* h, const retro_vulkan_image* image, uint32_t num_semaphores,
                          const VkSemaphore* semaphores, uint32_t) {
    auto* self = static_cast<VulkanHost*>(h);
    self->image_ = *image;
    self->have_image_ = image != nullptr;
    self->wait_semaphores_.assign(semaphores, semaphores + num_semaphores);
    self->wait_stages_.assign(num_semaphores, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}

uint32_t VulkanHost::GetSyncIndex(void* h) {
    return static_cast<VulkanHost*>(h)->sync_index_;
}

uint32_t VulkanHost::GetSyncIndexMask(void*) {
    return (1u << kSyncFrames) - 1;
}

void VulkanHost::SetCommandBuffers(void* h, uint32_t num, const VkCommandBuffer* cmd) {
    auto* self = static_cast<VulkanHost*>(h);
    self->core_cmds_.insert(self->core_cmds_.end(), cmd, cmd + num);
}

void VulkanHost::WaitSyncIndex(void* h) {
    auto* self = static_cast<VulkanHost*>(h);
    const uint32_t idx = self->sync_index_;
    if (!self->device_ || !self->fence_pending_[idx]) return;
    auto& f = *self->fn_;
    f.WaitForFences(self->device_, 1, &self->fences_[idx], VK_TRUE, UINT64_MAX);
    f.ResetFences(self->device_, 1, &self->fences_[idx]);
    self->fence_pending_[idx] = false;
}

void VulkanHost::LockQueue(void* h) {
    static_cast<VulkanHost*>(h)->queue_mutex_.lock();
}

void VulkanHost::UnlockQueue(void* h) {
    static_cast<VulkanHost*>(h)->queue_mutex_.unlock();
}

void VulkanHost::SetSignalSemaphore(void* h, VkSemaphore semaphore) {
    static_cast<VulkanHost*>(h)->signal_semaphore_ = semaphore;
}

// ---------------------------------------------------------------------------
// Startup self-test
//
// Before the core gets a device, run the Dozen paths it depends on on a
// throwaway VkDevice and log each step. Dozen hands every VkDevice the same
// ID3D12Device the presenter uses, so anything invalid it submits removes the
// presenter's device too; finding that here (rather than inside the core)
// tells us which basic operation is broken, and lets the session stay on the
// software renderer instead of starting the core on a dying GPU.

namespace {

// Compile-time switch until there is a setting for it. When it fails, the
// hardware renderer is reported unavailable (games run in software).
constexpr bool kRunVulkanSelfTest = true;
constexpr uint64_t kSelfTestTimeoutNs = 2'000'000'000ull; // per submission

// SPIR-V 1.0, glslangValidator -V --target-env vulkan1.0, debug info stripped:
//   #version 450
//   layout(local_size_x = 64) in;
//   layout(set = 0, binding = 0, std430) buffer Buf { uint data[]; };
//   void main() { uint i = gl_GlobalInvocationID.x; data[i] = i * 3u + 7u; }
constexpr uint32_t kSelfTestFillComp[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000021, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000b, 0x00060010, 0x00000004,
    0x00000011, 0x00000040, 0x00000001, 0x00000001, 0x00040047, 0x0000000b,
    0x0000000b, 0x0000001c, 0x00040047, 0x00000010, 0x00000006, 0x00000004,
    0x00030047, 0x00000011, 0x00000003, 0x00050048, 0x00000011, 0x00000000,
    0x00000023, 0x00000000, 0x00040047, 0x00000013, 0x00000021, 0x00000000,
    0x00040047, 0x00000013, 0x00000022, 0x00000000, 0x00040047, 0x00000020,
    0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040020,
    0x00000007, 0x00000007, 0x00000006, 0x00040017, 0x00000009, 0x00000006,
    0x00000003, 0x00040020, 0x0000000a, 0x00000001, 0x00000009, 0x0004003b,
    0x0000000a, 0x0000000b, 0x00000001, 0x0004002b, 0x00000006, 0x0000000c,
    0x00000000, 0x00040020, 0x0000000d, 0x00000001, 0x00000006, 0x0003001d,
    0x00000010, 0x00000006, 0x0003001e, 0x00000011, 0x00000010, 0x00040020,
    0x00000012, 0x00000002, 0x00000011, 0x0004003b, 0x00000012, 0x00000013,
    0x00000002, 0x00040015, 0x00000014, 0x00000020, 0x00000001, 0x0004002b,
    0x00000014, 0x00000015, 0x00000000, 0x0004002b, 0x00000006, 0x00000018,
    0x00000003, 0x0004002b, 0x00000006, 0x0000001a, 0x00000007, 0x00040020,
    0x0000001c, 0x00000002, 0x00000006, 0x0004002b, 0x00000006, 0x0000001e,
    0x00000040, 0x0004002b, 0x00000006, 0x0000001f, 0x00000001, 0x0006002c,
    0x00000009, 0x00000020, 0x0000001e, 0x0000001f, 0x0000001f, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003b, 0x00000007, 0x00000008, 0x00000007, 0x00050041, 0x0000000d,
    0x0000000e, 0x0000000b, 0x0000000c, 0x0004003d, 0x00000006, 0x0000000f,
    0x0000000e, 0x0003003e, 0x00000008, 0x0000000f, 0x0004003d, 0x00000006,
    0x00000016, 0x00000008, 0x0004003d, 0x00000006, 0x00000017, 0x00000008,
    0x00050084, 0x00000006, 0x00000019, 0x00000017, 0x00000018, 0x00050080,
    0x00000006, 0x0000001b, 0x00000019, 0x0000001a, 0x00060041, 0x0000001c,
    0x0000001d, 0x00000013, 0x00000015, 0x00000016, 0x0003003e, 0x0000001d,
    0x0000001b, 0x000100fd, 0x00010038,
};

// Azahar's BlitHelper compute shaders (src/video_core/host_shaders/
// vulkan_depth_to_buffer.comp and format_reinterpreter/vulkan_d24s8_to_rgba8.comp),
// compiled the way Azahar does (SPIR-V 1.3, glslangValidator --target-env
// vulkan1.1), debug info stripped. Only pipeline creation is tested.
constexpr uint32_t kAzaharDepthToBufferComp[] = {
    0x07230203, 0x00010300, 0x0008000b, 0x00000061, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000005,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000016, 0x00060010,
    0x00000004, 0x00000011, 0x00000008, 0x00000008, 0x00000001, 0x00040047,
    0x0000000d, 0x0000000b, 0x00000018, 0x00040047, 0x00000016, 0x0000000b,
    0x0000001c, 0x00030047, 0x0000001b, 0x00000002, 0x00040048, 0x0000001b,
    0x00000000, 0x00000000, 0x00050048, 0x0000001b, 0x00000000, 0x00000023,
    0x00000000, 0x00040048, 0x0000001b, 0x00000001, 0x00000000, 0x00050048,
    0x0000001b, 0x00000001, 0x00000023, 0x00000008, 0x00040048, 0x0000001b,
    0x00000002, 0x00000000, 0x00050048, 0x0000001b, 0x00000002, 0x00000023,
    0x00000010, 0x00030047, 0x00000021, 0x00000000, 0x00030047, 0x0000002c,
    0x00000000, 0x00030047, 0x0000002d, 0x00000000, 0x00040047, 0x00000034,
    0x00000021, 0x00000000, 0x00040047, 0x00000034, 0x00000022, 0x00000000,
    0x00030047, 0x0000003f, 0x00000000, 0x00030047, 0x00000043, 0x00000000,
    0x00040047, 0x00000043, 0x00000021, 0x00000001, 0x00040047, 0x00000043,
    0x00000022, 0x00000000, 0x00030047, 0x00000044, 0x00000000, 0x00030047,
    0x00000047, 0x00000000, 0x00030047, 0x00000048, 0x00000000, 0x00030047,
    0x0000004a, 0x00000000, 0x00040047, 0x0000004e, 0x00000006, 0x00000004,
    0x00030047, 0x0000004f, 0x00000002, 0x00040048, 0x0000004f, 0x00000000,
    0x00000019, 0x00050048, 0x0000004f, 0x00000000, 0x00000023, 0x00000000,
    0x00030047, 0x00000051, 0x00000019, 0x00040047, 0x00000051, 0x00000021,
    0x00000002, 0x00040047, 0x00000051, 0x00000022, 0x00000000, 0x00040047,
    0x00000060, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000001,
    0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008,
    0x00000007, 0x00000007, 0x00040015, 0x0000000a, 0x00000020, 0x00000000,
    0x00040017, 0x0000000b, 0x0000000a, 0x00000003, 0x00040020, 0x0000000c,
    0x00000001, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000001,
    0x00040017, 0x0000000e, 0x0000000a, 0x00000002, 0x0004002b, 0x00000006,
    0x00000012, 0x00000008, 0x0005002c, 0x00000007, 0x00000013, 0x00000012,
    0x00000012, 0x0004003b, 0x0000000c, 0x00000016, 0x00000001, 0x0005001e,
    0x0000001b, 0x00000007, 0x00000007, 0x00000007, 0x00040020, 0x0000001c,
    0x00000009, 0x0000001b, 0x0004003b, 0x0000001c, 0x0000001d, 0x00000009,
    0x0004002b, 0x00000006, 0x0000001e, 0x00000000, 0x00040020, 0x0000001f,
    0x00000009, 0x00000007, 0x00030016, 0x00000024, 0x00000020, 0x00040017,
    0x00000025, 0x00000024, 0x00000002, 0x00040020, 0x00000026, 0x00000007,
    0x00000025, 0x0004002b, 0x00000006, 0x0000002a, 0x00000002, 0x00040020,
    0x0000002f, 0x00000007, 0x0000000a, 0x00090019, 0x00000031, 0x00000024,
    0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
    0x0003001b, 0x00000032, 0x00000031, 0x00040020, 0x00000033, 0x00000000,
    0x00000032, 0x0004003b, 0x00000033, 0x00000034, 0x00000000, 0x00040017,
    0x00000037, 0x00000024, 0x00000004, 0x0004002b, 0x00000024, 0x00000038,
    0x00000000, 0x0004002b, 0x0000000a, 0x0000003a, 0x00000000, 0x0004002b,
    0x00000024, 0x0000003c, 0x4b7fffff, 0x00090019, 0x00000040, 0x0000000a,
    0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
    0x0003001b, 0x00000041, 0x00000040, 0x00040020, 0x00000042, 0x00000000,
    0x00000041, 0x0004003b, 0x00000042, 0x00000043, 0x00000000, 0x00040017,
    0x00000046, 0x0000000a, 0x00000004, 0x0003001d, 0x0000004e, 0x0000000a,
    0x0003001e, 0x0000004f, 0x0000004e, 0x00040020, 0x00000050, 0x0000000c,
    0x0000004f, 0x0004003b, 0x00000050, 0x00000051, 0x0000000c, 0x0004002b,
    0x0000000a, 0x00000052, 0x00000001, 0x00040020, 0x00000053, 0x00000007,
    0x00000006, 0x00040020, 0x0000005d, 0x0000000c, 0x0000000a, 0x0004002b,
    0x0000000a, 0x0000005f, 0x00000008, 0x0006002c, 0x0000000b, 0x00000060,
    0x0000005f, 0x0000005f, 0x00000052, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000008,
    0x00000009, 0x00000007, 0x0004003b, 0x00000008, 0x00000015, 0x00000007,
    0x0004003b, 0x00000008, 0x0000001a, 0x00000007, 0x0004003b, 0x00000026,
    0x00000027, 0x00000007, 0x0004003b, 0x0000002f, 0x00000030, 0x00000007,
    0x0004003b, 0x0000002f, 0x0000003f, 0x00000007, 0x0004003b, 0x0000002f,
    0x00000049, 0x00000007, 0x0004003d, 0x0000000b, 0x0000000f, 0x0000000d,
    0x0007004f, 0x0000000e, 0x00000010, 0x0000000f, 0x0000000f, 0x00000000,
    0x00000001, 0x0004007c, 0x00000007, 0x00000011, 0x00000010, 0x00050084,
    0x00000007, 0x00000014, 0x00000011, 0x00000013, 0x0003003e, 0x00000009,
    0x00000014, 0x0004003d, 0x0000000b, 0x00000017, 0x00000016, 0x0007004f,
    0x0000000e, 0x00000018, 0x00000017, 0x00000017, 0x00000000, 0x00000001,
    0x0004007c, 0x00000007, 0x00000019, 0x00000018, 0x0003003e, 0x00000015,
    0x00000019, 0x00050041, 0x0000001f, 0x00000020, 0x0000001d, 0x0000001e,
    0x0004003d, 0x00000007, 0x00000021, 0x00000020, 0x0004003d, 0x00000007,
    0x00000022, 0x00000015, 0x00050080, 0x00000007, 0x00000023, 0x00000021,
    0x00000022, 0x0003003e, 0x0000001a, 0x00000023, 0x0004003d, 0x00000007,
    0x00000028, 0x0000001a, 0x0004006f, 0x00000025, 0x00000029, 0x00000028,
    0x00050041, 0x0000001f, 0x0000002b, 0x0000001d, 0x0000002a, 0x0004003d,
    0x00000007, 0x0000002c, 0x0000002b, 0x0004006f, 0x00000025, 0x0000002d,
    0x0000002c, 0x00050088, 0x00000025, 0x0000002e, 0x00000029, 0x0000002d,
    0x0003003e, 0x00000027, 0x0000002e, 0x0004003d, 0x00000032, 0x00000035,
    0x00000034, 0x0004003d, 0x00000025, 0x00000036, 0x00000027, 0x00070058,
    0x00000037, 0x00000039, 0x00000035, 0x00000036, 0x00000002, 0x00000038,
    0x00050051, 0x00000024, 0x0000003b, 0x00000039, 0x00000000, 0x00050085,
    0x00000024, 0x0000003d, 0x0000003b, 0x0000003c, 0x0004006d, 0x0000000a,
    0x0000003e, 0x0000003d, 0x0003003e, 0x00000030, 0x0000003e, 0x0004003d,
    0x00000041, 0x00000044, 0x00000043, 0x0004003d, 0x00000025, 0x00000045,
    0x00000027, 0x00070058, 0x00000046, 0x00000047, 0x00000044, 0x00000045,
    0x00000002, 0x00000038, 0x00050051, 0x0000000a, 0x00000048, 0x00000047,
    0x00000000, 0x0003003e, 0x0000003f, 0x00000048, 0x0004003d, 0x0000000a,
    0x0000004a, 0x0000003f, 0x0004003d, 0x0000000a, 0x0000004b, 0x00000030,
    0x000500c4, 0x0000000a, 0x0000004c, 0x0000004b, 0x00000012, 0x000500c5,
    0x0000000a, 0x0000004d, 0x0000004a, 0x0000004c, 0x0003003e, 0x00000049,
    0x0000004d, 0x00050041, 0x00000053, 0x00000054, 0x00000015, 0x00000052,
    0x0004003d, 0x00000006, 0x00000055, 0x00000054, 0x00050041, 0x00000053,
    0x00000056, 0x00000009, 0x0000003a, 0x0004003d, 0x00000006, 0x00000057,
    0x00000056, 0x00050084, 0x00000006, 0x00000058, 0x00000055, 0x00000057,
    0x00050041, 0x00000053, 0x00000059, 0x00000015, 0x0000003a, 0x0004003d,
    0x00000006, 0x0000005a, 0x00000059, 0x00050080, 0x00000006, 0x0000005b,
    0x00000058, 0x0000005a, 0x0004003d, 0x0000000a, 0x0000005c, 0x00000049,
    0x00060041, 0x0000005d, 0x0000005e, 0x00000051, 0x0000001e, 0x0000005b,
    0x0003003e, 0x0000005e, 0x0000005c, 0x000100fd, 0x00010038,
};

constexpr uint32_t kAzaharD24S8ToRgba8Comp[] = {
    0x07230203, 0x00010300, 0x0008000b, 0x00000057, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000014, 0x00060010, 0x00000004,
    0x00000011, 0x00000008, 0x00000008, 0x00000001, 0x00030047, 0x0000000a,
    0x00000002, 0x00040048, 0x0000000a, 0x00000000, 0x00000000, 0x00050048,
    0x0000000a, 0x00000000, 0x00000023, 0x00000000, 0x00040048, 0x0000000a,
    0x00000001, 0x00000000, 0x00050048, 0x0000000a, 0x00000001, 0x00000023,
    0x00000008, 0x00040048, 0x0000000a, 0x00000002, 0x00000000, 0x00050048,
    0x0000000a, 0x00000002, 0x00000023, 0x00000010, 0x00030047, 0x00000010,
    0x00000000, 0x00040047, 0x00000014, 0x0000000b, 0x0000001c, 0x00030047,
    0x0000001d, 0x00000000, 0x00040047, 0x00000027, 0x00000021, 0x00000000,
    0x00040047, 0x00000027, 0x00000022, 0x00000000, 0x00030047, 0x00000031,
    0x00000000, 0x00030047, 0x00000034, 0x00000000, 0x00040047, 0x00000034,
    0x00000021, 0x00000001, 0x00040047, 0x00000034, 0x00000022, 0x00000000,
    0x00030047, 0x00000035, 0x00000000, 0x00030047, 0x00000038, 0x00000000,
    0x00030047, 0x00000039, 0x00000000, 0x00030047, 0x0000003c, 0x00000000,
    0x00030047, 0x0000003e, 0x00000000, 0x00030047, 0x00000043, 0x00000000,
    0x00030047, 0x00000045, 0x00000000, 0x00030047, 0x00000046, 0x00000000,
    0x00030047, 0x00000047, 0x00000000, 0x00030047, 0x00000048, 0x00000000,
    0x00030047, 0x00000049, 0x00000000, 0x00030047, 0x0000004a, 0x00000000,
    0x00030047, 0x0000004d, 0x00000019, 0x00040047, 0x0000004d, 0x00000021,
    0x00000002, 0x00040047, 0x0000004d, 0x00000022, 0x00000000, 0x00040047,
    0x00000056, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000001,
    0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008,
    0x00000007, 0x00000007, 0x0005001e, 0x0000000a, 0x00000007, 0x00000007,
    0x00000007, 0x00040020, 0x0000000b, 0x00000009, 0x0000000a, 0x0004003b,
    0x0000000b, 0x0000000c, 0x00000009, 0x0004002b, 0x00000006, 0x0000000d,
    0x00000000, 0x00040020, 0x0000000e, 0x00000009, 0x00000007, 0x00040015,
    0x00000011, 0x00000020, 0x00000000, 0x00040017, 0x00000012, 0x00000011,
    0x00000003, 0x00040020, 0x00000013, 0x00000001, 0x00000012, 0x0004003b,
    0x00000013, 0x00000014, 0x00000001, 0x00040017, 0x00000015, 0x00000011,
    0x00000002, 0x0004002b, 0x00000006, 0x0000001b, 0x00000001, 0x00040020,
    0x00000022, 0x00000007, 0x00000011, 0x00030016, 0x00000024, 0x00000020,
    0x00090019, 0x00000025, 0x00000024, 0x00000001, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000000, 0x00040020, 0x00000026, 0x00000000,
    0x00000025, 0x0004003b, 0x00000026, 0x00000027, 0x00000000, 0x00040017,
    0x0000002a, 0x00000024, 0x00000004, 0x0004002b, 0x00000011, 0x0000002c,
    0x00000000, 0x0004002b, 0x00000024, 0x0000002e, 0x4f800000, 0x00090019,
    0x00000032, 0x00000011, 0x00000001, 0x00000000, 0x00000000, 0x00000000,
    0x00000001, 0x00000000, 0x00040020, 0x00000033, 0x00000000, 0x00000032,
    0x0004003b, 0x00000033, 0x00000034, 0x00000000, 0x00040017, 0x00000037,
    0x00000011, 0x00000004, 0x00040020, 0x0000003a, 0x00000007, 0x00000037,
    0x0004002b, 0x00000011, 0x0000003f, 0x00000018, 0x0004002b, 0x00000011,
    0x00000040, 0x00000010, 0x0004002b, 0x00000011, 0x00000041, 0x00000008,
    0x0006002c, 0x00000012, 0x00000042, 0x0000003f, 0x00000040, 0x00000041,
    0x0004002b, 0x00000011, 0x00000044, 0x000000ff, 0x00090019, 0x0000004b,
    0x00000024, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000002,
    0x00000004, 0x00040020, 0x0000004c, 0x00000000, 0x0000004b, 0x0004003b,
    0x0000004c, 0x0000004d, 0x00000000, 0x0004002b, 0x00000024, 0x00000052,
    0x437f0000, 0x0004002b, 0x00000011, 0x00000055, 0x00000001, 0x0006002c,
    0x00000012, 0x00000056, 0x00000041, 0x00000041, 0x00000055, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003b, 0x00000008,
    0x0000001a, 0x00000007, 0x0004003b, 0x00000022, 0x00000023, 0x00000007,
    0x0004003b, 0x00000022, 0x00000031, 0x00000007, 0x0004003b, 0x0000003a,
    0x0000003b, 0x00000007, 0x00050041, 0x0000000e, 0x0000000f, 0x0000000c,
    0x0000000d, 0x0004003d, 0x00000007, 0x00000010, 0x0000000f, 0x0004003d,
    0x00000012, 0x00000016, 0x00000014, 0x0007004f, 0x00000015, 0x00000017,
    0x00000016, 0x00000016, 0x00000000, 0x00000001, 0x0004007c, 0x00000007,
    0x00000018, 0x00000017, 0x00050080, 0x00000007, 0x00000019, 0x00000010,
    0x00000018, 0x0003003e, 0x00000009, 0x00000019, 0x00050041, 0x0000000e,
    0x0000001c, 0x0000000c, 0x0000001b, 0x0004003d, 0x00000007, 0x0000001d,
    0x0000001c, 0x0004003d, 0x00000012, 0x0000001e, 0x00000014, 0x0007004f,
    0x00000015, 0x0000001f, 0x0000001e, 0x0000001e, 0x00000000, 0x00000001,
    0x0004007c, 0x00000007, 0x00000020, 0x0000001f, 0x00050080, 0x00000007,
    0x00000021, 0x0000001d, 0x00000020, 0x0003003e, 0x0000001a, 0x00000021,
    0x0004003d, 0x00000025, 0x00000028, 0x00000027, 0x0004003d, 0x00000007,
    0x00000029, 0x00000009, 0x0007005f, 0x0000002a, 0x0000002b, 0x00000028,
    0x00000029, 0x00000002, 0x0000000d, 0x00050051, 0x00000024, 0x0000002d,
    0x0000002b, 0x00000000, 0x00050085, 0x00000024, 0x0000002f, 0x0000002d,
    0x0000002e, 0x0004006d, 0x00000011, 0x00000030, 0x0000002f, 0x0003003e,
    0x00000023, 0x00000030, 0x0004003d, 0x00000032, 0x00000035, 0x00000034,
    0x0004003d, 0x00000007, 0x00000036, 0x00000009, 0x0007005f, 0x00000037,
    0x00000038, 0x00000035, 0x00000036, 0x00000002, 0x0000000d, 0x00050051,
    0x00000011, 0x00000039, 0x00000038, 0x00000000, 0x0003003e, 0x00000031,
    0x00000039, 0x0004003d, 0x00000011, 0x0000003c, 0x00000031, 0x0004003d,
    0x00000011, 0x0000003d, 0x00000023, 0x00060050, 0x00000012, 0x0000003e,
    0x0000003d, 0x0000003d, 0x0000003d, 0x000500c2, 0x00000012, 0x00000043,
    0x0000003e, 0x00000042, 0x00060050, 0x00000012, 0x00000045, 0x00000044,
    0x00000044, 0x00000044, 0x000500c7, 0x00000012, 0x00000046, 0x00000043,
    0x00000045, 0x00050051, 0x00000011, 0x00000047, 0x00000046, 0x00000000,
    0x00050051, 0x00000011, 0x00000048, 0x00000046, 0x00000001, 0x00050051,
    0x00000011, 0x00000049, 0x00000046, 0x00000002, 0x00070050, 0x00000037,
    0x0000004a, 0x0000003c, 0x00000047, 0x00000048, 0x00000049, 0x0003003e,
    0x0000003b, 0x0000004a, 0x0004003d, 0x0000004b, 0x0000004e, 0x0000004d,
    0x0004003d, 0x00000007, 0x0000004f, 0x0000001a, 0x0004003d, 0x00000037,
    0x00000050, 0x0000003b, 0x00040070, 0x0000002a, 0x00000051, 0x00000050,
    0x00070050, 0x0000002a, 0x00000053, 0x00000052, 0x00000052, 0x00000052,
    0x00000052, 0x00050088, 0x0000002a, 0x00000054, 0x00000051, 0x00000053,
    0x00040063, 0x0000004e, 0x0000004f, 0x00000054, 0x000100fd, 0x00010038,
};

class VulkanSelfTest {
public:
    VulkanSelfTest(VkInstance instance, VkPhysicalDevice gpu, PFN_vkGetInstanceProcAddr gipa,
                   PFN_vkGetDeviceProcAddr gdpa, uint32_t queue_family,
                   const VkPhysicalDeviceMemoryProperties& memory, ID3D12Device* presenter_device)
        : instance_(instance), gpu_(gpu), gipa_(gipa), gdpa_(gdpa), queue_family_(queue_family),
          memory_(memory), d3d_(presenter_device) {}

    ~VulkanSelfTest() { Teardown(); }
    VulkanSelfTest(const VulkanSelfTest&) = delete;
    VulkanSelfTest& operator=(const VulkanSelfTest&) = delete;

    // Runs every step; stops at the first one that fails or loses the device.
    bool Run(std::string& error);

private:
    VkResult SetUpDevice();
    VkResult StepEmptySubmit();
    VkResult StepFillBuffer();
    VkResult StepClearColorImage();
    VkResult StepComputeDispatch();
    VkResult StepClearDepthStencil();
    VkResult StepAzaharPipeline(const uint32_t* code, size_t code_size, bool storage_image);

    VkResult Begin();
    VkResult SubmitAndWait();
    VkResult MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible,
                          VkBuffer& buffer, VkDeviceMemory& memory);
    VkResult MakeImage(VkFormat format, VkImageUsageFlags usage, VkImage& image);
    VkResult BindNewMemory(const VkMemoryRequirements& req, VkMemoryPropertyFlags want,
                           VkDeviceMemory& memory);
    bool FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want, uint32_t& index) const;
    // Device lost (Vulkan or D3D12) after a step? Fills reason.
    bool DeviceLost(std::string& reason);
    void Teardown();

    VkInstance instance_;
    VkPhysicalDevice gpu_;
    PFN_vkGetInstanceProcAddr gipa_;
    PFN_vkGetDeviceProcAddr gdpa_;
    uint32_t queue_family_;
    const VkPhysicalDeviceMemoryProperties& memory_;
    ID3D12Device* d3d_;
    std::string detail_; // extra text for the current step's FAILED line

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::vector<VkBuffer> buffers_;
    std::vector<VkImage> images_;
    std::vector<VkDeviceMemory> memories_;
    std::vector<VkShaderModule> shaders_;
    std::vector<VkDescriptorSetLayout> set_layouts_;
    std::vector<VkPipelineLayout> pipeline_layouts_;
    std::vector<VkDescriptorPool> descriptor_pools_;
    std::vector<VkPipeline> pipelines_;

    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkCreateDevice CreateDeviceFn = nullptr;
#define ST_FN(name) PFN_vk##name name = nullptr;
    ST_FN(DestroyDevice)
    ST_FN(GetDeviceQueue)
    ST_FN(DeviceWaitIdle)
    ST_FN(QueueWaitIdle)
    ST_FN(QueueSubmit)
    ST_FN(CreateCommandPool)
    ST_FN(DestroyCommandPool)
    ST_FN(AllocateCommandBuffers)
    ST_FN(BeginCommandBuffer)
    ST_FN(EndCommandBuffer)
    ST_FN(CreateFence)
    ST_FN(DestroyFence)
    ST_FN(WaitForFences)
    ST_FN(ResetFences)
    ST_FN(CreateBuffer)
    ST_FN(DestroyBuffer)
    ST_FN(GetBufferMemoryRequirements)
    ST_FN(BindBufferMemory)
    ST_FN(CreateImage)
    ST_FN(DestroyImage)
    ST_FN(GetImageMemoryRequirements)
    ST_FN(BindImageMemory)
    ST_FN(AllocateMemory)
    ST_FN(FreeMemory)
    ST_FN(MapMemory)
    ST_FN(UnmapMemory)
    ST_FN(InvalidateMappedMemoryRanges)
    ST_FN(CmdPipelineBarrier)
    ST_FN(CmdFillBuffer)
    ST_FN(CmdClearColorImage)
    ST_FN(CmdClearDepthStencilImage)
    ST_FN(CmdBindPipeline)
    ST_FN(CmdBindDescriptorSets)
    ST_FN(CmdDispatch)
    ST_FN(CreateShaderModule)
    ST_FN(DestroyShaderModule)
    ST_FN(CreateDescriptorSetLayout)
    ST_FN(DestroyDescriptorSetLayout)
    ST_FN(CreatePipelineLayout)
    ST_FN(DestroyPipelineLayout)
    ST_FN(CreateDescriptorPool)
    ST_FN(DestroyDescriptorPool)
    ST_FN(AllocateDescriptorSets)
    ST_FN(UpdateDescriptorSets)
    ST_FN(CreateComputePipelines)
    ST_FN(DestroyPipeline)
#undef ST_FN
};

const char* const kSelfTestStepNames[] = {
    "create device",
    "empty submit",
    "vkCmdFillBuffer",
    "vkCmdClearColorImage (64x64 RGBA8)",
    "compute dispatch",
    "vkCmdClearDepthStencilImage",
    "Azahar depth_to_buffer pipeline",
    "Azahar d24s8_to_rgba8 pipeline",
};

bool VulkanSelfTest::Run(std::string& error) {
    const int step_count = static_cast<int>(std::size(kSelfTestStepNames));
    int failed_steps = 0;
    for (int step = 0; step < step_count; ++step) {
        const char* name = kSelfTestStepNames[step];
        detail_.clear();
        VkResult r = VK_SUCCESS;
        switch (step) {
        case 0: r = SetUpDevice(); break;
        case 1: r = StepEmptySubmit(); break;
        case 2: r = StepFillBuffer(); break;
        case 3: r = StepClearColorImage(); break;
        case 4: r = StepComputeDispatch(); break;
        case 5: r = StepClearDepthStencil(); break;
        case 6:
            r = StepAzaharPipeline(kAzaharDepthToBufferComp, sizeof(kAzaharDepthToBufferComp),
                                   false);
            break;
        case 7:
            r = StepAzaharPipeline(kAzaharD24S8ToRgba8Comp, sizeof(kAzaharD24S8ToRgba8Comp),
                                   true);
            break;
        default: break;
        }
        std::string lost_reason;
        const bool lost = DeviceLost(lost_reason);
        if (r == VK_SUCCESS && !lost) {
            ONYX_INFO("Vulkan self-test: %d/%d %s ok%s%s%s", step + 1, step_count, name,
                      detail_.empty() ? "" : " (", detail_.c_str(), detail_.empty() ? "" : ")");
            continue;
        }
        if (r != VK_SUCCESS)
            ONYX_ERROR("Vulkan self-test: %d/%d %s FAILED (%s, %d)%s%s", step + 1, step_count,
                       name, VkResultName(r), static_cast<int>(r), detail_.empty() ? "" : ": ",
                       detail_.c_str());
        if (lost) {
            ONYX_ERROR("Vulkan self-test lost the GPU device at step %d (%s): %s", step + 1, name,
                       lost_reason.c_str());
            error = "Vulkan self-test lost the GPU device at step " + std::to_string(step + 1) +
                    " (" + name + "): " + lost_reason;
        } else if (step != 0) {
            // A failed step that leaves the device alive is only reported: the core
            // copes with some missing pipelines, so let it try and log what it hits.
            ++failed_steps;
            continue;
        } else {
            error = "Vulkan self-test failed at step " + std::to_string(step + 1) + " (" + name +
                    "): " + VkResultName(r) + (detail_.empty() ? "" : ": " + detail_);
        }
        return false;
    }
    if (failed_steps > 0) {
        ONYX_WARN("Vulkan self-test: %d of %d steps failed without losing the device; "
                  "starting the hardware renderer anyway", failed_steps, step_count);
    } else {
        ONYX_INFO("Vulkan self-test passed (%d steps)", step_count);
    }
    return true;
}

bool VulkanSelfTest::DeviceLost(std::string& reason) {
    bool lost = false;
    if (device_ && DeviceWaitIdle) {
        const VkResult r = DeviceWaitIdle(device_);
        if (r == VK_ERROR_DEVICE_LOST) {
            reason = "vkDeviceWaitIdle returned VK_ERROR_DEVICE_LOST";
            lost = true;
        }
    }
    if (d3d_) {
        const HRESULT hr = d3d_->GetDeviceRemovedReason();
        if (hr != S_OK) {
            char text[96];
            std::snprintf(text, sizeof(text), "%sD3D12 device removed, reason 0x%08X",
                          lost ? "; " : "", static_cast<unsigned>(hr));
            reason += text;
            lost = true;
        }
    }
    return lost;
}

VkResult VulkanSelfTest::SetUpDevice() {
    GetPhysicalDeviceFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        gipa_(instance_, "vkGetPhysicalDeviceFormatProperties"));
    CreateDeviceFn = reinterpret_cast<PFN_vkCreateDevice>(gipa_(instance_, "vkCreateDevice"));
    if (!GetPhysicalDeviceFormatProperties || !CreateDeviceFn || !gdpa_) {
        detail_ = "missing instance entry points";
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = queue_family_;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    VkResult r = CreateDeviceFn(gpu_, &dci, nullptr, &device_);
    if (r != VK_SUCCESS) {
        device_ = VK_NULL_HANDLE;
        return r;
    }

    std::string missing;
#define ST_LOAD(name)                                                                     \
    name = reinterpret_cast<PFN_vk##name>(gdpa_(device_, "vk" #name));                   \
    if (!name) missing += missing.empty() ? "vk" #name : ", vk" #name;
    ST_LOAD(DestroyDevice)
    ST_LOAD(GetDeviceQueue)
    ST_LOAD(DeviceWaitIdle)
    ST_LOAD(QueueWaitIdle)
    ST_LOAD(QueueSubmit)
    ST_LOAD(CreateCommandPool)
    ST_LOAD(DestroyCommandPool)
    ST_LOAD(AllocateCommandBuffers)
    ST_LOAD(BeginCommandBuffer)
    ST_LOAD(EndCommandBuffer)
    ST_LOAD(CreateFence)
    ST_LOAD(DestroyFence)
    ST_LOAD(WaitForFences)
    ST_LOAD(ResetFences)
    ST_LOAD(CreateBuffer)
    ST_LOAD(DestroyBuffer)
    ST_LOAD(GetBufferMemoryRequirements)
    ST_LOAD(BindBufferMemory)
    ST_LOAD(CreateImage)
    ST_LOAD(DestroyImage)
    ST_LOAD(GetImageMemoryRequirements)
    ST_LOAD(BindImageMemory)
    ST_LOAD(AllocateMemory)
    ST_LOAD(FreeMemory)
    ST_LOAD(MapMemory)
    ST_LOAD(UnmapMemory)
    ST_LOAD(InvalidateMappedMemoryRanges)
    ST_LOAD(CmdPipelineBarrier)
    ST_LOAD(CmdFillBuffer)
    ST_LOAD(CmdClearColorImage)
    ST_LOAD(CmdClearDepthStencilImage)
    ST_LOAD(CmdBindPipeline)
    ST_LOAD(CmdBindDescriptorSets)
    ST_LOAD(CmdDispatch)
    ST_LOAD(CreateShaderModule)
    ST_LOAD(DestroyShaderModule)
    ST_LOAD(CreateDescriptorSetLayout)
    ST_LOAD(DestroyDescriptorSetLayout)
    ST_LOAD(CreatePipelineLayout)
    ST_LOAD(DestroyPipelineLayout)
    ST_LOAD(CreateDescriptorPool)
    ST_LOAD(DestroyDescriptorPool)
    ST_LOAD(AllocateDescriptorSets)
    ST_LOAD(UpdateDescriptorSets)
    ST_LOAD(CreateComputePipelines)
    ST_LOAD(DestroyPipeline)
#undef ST_LOAD
    if (!missing.empty()) {
        detail_ = "missing " + missing;
        // DestroyDevice may be among them; Teardown copes with that.
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    GetDeviceQueue(device_, queue_family_, 0, &queue_);
    if (!queue_) {
        detail_ = "no queue";
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    r = CreateCommandPool(device_, &pci, nullptr, &pool_);
    if (r != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        detail_ = "vkCreateCommandPool";
        return r;
    }
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    r = AllocateCommandBuffers(device_, &cai, &cmd_);
    if (r != VK_SUCCESS) {
        cmd_ = VK_NULL_HANDLE;
        detail_ = "vkAllocateCommandBuffers";
        return r;
    }
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    r = CreateFence(device_, &fci, nullptr, &fence_);
    if (r != VK_SUCCESS) {
        fence_ = VK_NULL_HANDLE;
        detail_ = "vkCreateFence";
        return r;
    }
    return VK_SUCCESS;
}

VkResult VulkanSelfTest::Begin() {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkResult r = BeginCommandBuffer(cmd_, &bi); // implicitly resets (pool flag)
    if (r != VK_SUCCESS) detail_ = "vkBeginCommandBuffer";
    return r;
}

VkResult VulkanSelfTest::SubmitAndWait() {
    VkResult r = EndCommandBuffer(cmd_);
    if (r != VK_SUCCESS) {
        detail_ = "vkEndCommandBuffer";
        return r;
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    r = QueueSubmit(queue_, 1, &si, fence_);
    if (r != VK_SUCCESS) {
        detail_ = "vkQueueSubmit";
        return r;
    }
    r = WaitForFences(device_, 1, &fence_, VK_TRUE, kSelfTestTimeoutNs);
    if (r != VK_SUCCESS) {
        detail_ = r == VK_TIMEOUT ? "the GPU did not finish within 2 s" : "vkWaitForFences";
        return r == VK_TIMEOUT ? VK_ERROR_DEVICE_LOST : r;
    }
    r = ResetFences(device_, 1, &fence_);
    if (r != VK_SUCCESS) detail_ = "vkResetFences";
    return r;
}

bool VulkanSelfTest::FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want,
                                    uint32_t& index) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & want) == want) {
            index = i;
            return true;
        }
    }
    return false;
}

VkResult VulkanSelfTest::BindNewMemory(const VkMemoryRequirements& req, VkMemoryPropertyFlags want,
                                       VkDeviceMemory& memory) {
    uint32_t type = 0;
    if (!FindMemoryType(req.memoryTypeBits, want, type) &&
        !FindMemoryType(req.memoryTypeBits, 0, type)) {
        detail_ = "no usable memory type";
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    const VkResult r = AllocateMemory(device_, &mai, nullptr, &memory);
    if (r != VK_SUCCESS) {
        memory = VK_NULL_HANDLE;
        detail_ = "vkAllocateMemory";
        return r;
    }
    memories_.push_back(memory);
    return VK_SUCCESS;
}

VkResult VulkanSelfTest::MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                      bool host_visible, VkBuffer& buffer,
                                      VkDeviceMemory& memory) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = CreateBuffer(device_, &bci, nullptr, &buffer);
    if (r != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        detail_ = "vkCreateBuffer";
        return r;
    }
    buffers_.push_back(buffer);
    VkMemoryRequirements req{};
    GetBufferMemoryRequirements(device_, buffer, &req);
    r = BindNewMemory(req,
                      host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                   : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      memory);
    if (r != VK_SUCCESS) return r;
    if (host_visible) {
        uint32_t type = 0;
        if (!FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, type)) {
            detail_ = "no host-visible memory type for the buffer";
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
    }
    r = BindBufferMemory(device_, buffer, memory, 0);
    if (r != VK_SUCCESS) detail_ = "vkBindBufferMemory";
    return r;
}

VkResult VulkanSelfTest::MakeImage(VkFormat format, VkImageUsageFlags usage, VkImage& image) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {64, 64, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = CreateImage(device_, &ici, nullptr, &image);
    if (r != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        detail_ = "vkCreateImage";
        return r;
    }
    images_.push_back(image);
    VkMemoryRequirements req{};
    GetImageMemoryRequirements(device_, image, &req);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    r = BindNewMemory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory);
    if (r != VK_SUCCESS) return r;
    r = BindImageMemory(device_, image, memory, 0);
    if (r != VK_SUCCESS) detail_ = "vkBindImageMemory";
    return r;
}

VkResult VulkanSelfTest::StepEmptySubmit() {
    const VkResult r = Begin();
    if (r != VK_SUCCESS) return r;
    return SubmitAndWait();
}

VkResult VulkanSelfTest::StepFillBuffer() {
    constexpr VkDeviceSize kSize = 4096;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult r = MakeBuffer(kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, buffer, memory);
    if (r != VK_SUCCESS) return r;
    if ((r = Begin()) != VK_SUCCESS) return r;
    CmdFillBuffer(cmd_, buffer, 0, VK_WHOLE_SIZE, 0x5A5A5A5Au);
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.size = VK_WHOLE_SIZE;
    CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                       nullptr, 1, &b, 0, nullptr);
    if ((r = SubmitAndWait()) != VK_SUCCESS) return r;

    void* mapped = nullptr;
    r = MapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS || !mapped) {
        detail_ = "vkMapMemory";
        return r != VK_SUCCESS ? r : VK_ERROR_MEMORY_MAP_FAILED;
    }
    const auto* words = static_cast<const uint32_t*>(mapped);
    uint32_t bad = 0, first_bad = 0;
    for (uint32_t i = 0; i < kSize / 4; ++i) {
        if (words[i] != 0x5A5A5A5Au && bad++ == 0) first_bad = i;
    }
    const uint32_t seen = bad ? words[first_bad] : 0;
    UnmapMemory(device_, memory);
    if (bad) {
        char text[96];
        std::snprintf(text, sizeof(text), "%u of %u words wrong (word %u = 0x%08X)", bad,
                      static_cast<unsigned>(kSize / 4), first_bad, seen);
        detail_ = text;
        return VK_ERROR_UNKNOWN;
    }
    return VK_SUCCESS;
}

VkResult VulkanSelfTest::StepClearColorImage() {
    VkImage image = VK_NULL_HANDLE;
    VkResult r = MakeImage(VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             image);
    if (r != VK_SUCCESS) return r;
    if ((r = Begin()) != VK_SUCCESS) return r;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &b);
    VkClearColorValue color{};
    color.float32[0] = 0.25f;
    color.float32[1] = 0.5f;
    color.float32[2] = 0.75f;
    color.float32[3] = 1.0f;
    CmdClearColorImage(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &b);
    return SubmitAndWait();
}

VkResult VulkanSelfTest::StepComputeDispatch() {
    constexpr uint32_t kGroups = 4, kCount = kGroups * 64;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult r = MakeBuffer(kCount * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                              buffer, memory);
    if (r != VK_SUCCESS) return r;

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(kSelfTestFillComp);
    smci.pCode = kSelfTestFillComp;
    VkShaderModule shader = VK_NULL_HANDLE;
    if ((r = CreateShaderModule(device_, &smci, nullptr, &shader)) != VK_SUCCESS) {
        detail_ = "vkCreateShaderModule";
        return r;
    }
    shaders_.push_back(shader);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if ((r = CreateDescriptorSetLayout(device_, &dslci, nullptr, &set_layout)) != VK_SUCCESS) {
        detail_ = "vkCreateDescriptorSetLayout";
        return r;
    }
    set_layouts_.push_back(set_layout);
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if ((r = CreatePipelineLayout(device_, &plci, nullptr, &layout)) != VK_SUCCESS) {
        detail_ = "vkCreatePipelineLayout";
        return r;
    }
    pipeline_layouts_.push_back(layout);

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shader;
    cpci.stage.pName = "main";
    cpci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if ((r = CreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline)) !=
        VK_SUCCESS) {
        detail_ = "vkCreateComputePipelines";
        return r;
    }
    if (!pipeline) {
        detail_ = "vkCreateComputePipelines returned no pipeline";
        return VK_ERROR_UNKNOWN;
    }
    pipelines_.push_back(pipeline);

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if ((r = CreateDescriptorPool(device_, &dpci, nullptr, &pool)) != VK_SUCCESS) {
        detail_ = "vkCreateDescriptorPool";
        return r;
    }
    descriptor_pools_.push_back(pool);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if ((r = AllocateDescriptorSets(device_, &dsai, &set)) != VK_SUCCESS) {
        detail_ = "vkAllocateDescriptorSets";
        return r;
    }
    VkDescriptorBufferInfo dbi{buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &dbi;
    UpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    if ((r = Begin()) != VK_SUCCESS) return r;
    CmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    CmdDispatch(cmd_, kGroups, 1, 1);
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.size = VK_WHOLE_SIZE;
    CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                       0, nullptr, 1, &b, 0, nullptr);
    if ((r = SubmitAndWait()) != VK_SUCCESS) return r;

    void* mapped = nullptr;
    r = MapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS || !mapped) {
        detail_ = "vkMapMemory";
        return r != VK_SUCCESS ? r : VK_ERROR_MEMORY_MAP_FAILED;
    }
    const auto* words = static_cast<const uint32_t*>(mapped);
    uint32_t bad = 0, first_bad = 0;
    for (uint32_t i = 0; i < kCount; ++i) {
        if (words[i] != i * 3u + 7u && bad++ == 0) first_bad = i;
    }
    const uint32_t seen = bad ? words[first_bad] : 0;
    UnmapMemory(device_, memory);
    if (bad) {
        char text[112];
        std::snprintf(text, sizeof(text), "%u of %u results wrong (data[%u] = %u, expected %u)",
                      bad, kCount, first_bad, seen, first_bad * 3u + 7u);
        detail_ = text;
        return VK_ERROR_UNKNOWN;
    }
    return VK_SUCCESS;
}

VkResult VulkanSelfTest::StepClearDepthStencil() {
    // Azahar prefers D24S8 and falls back to D32S8, like the 3DS formats it emulates.
    VkFormat format = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        VkFormatProperties fp{};
        GetPhysicalDeviceFormatProperties(gpu_, candidate, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            format = candidate;
            break;
        }
    }
    if (format == VK_FORMAT_UNDEFINED) {
        detail_ = "neither D24S8 nor D32S8 is a depth/stencil attachment format";
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    detail_ = format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8";
    const std::string format_name = detail_;
    VkImage image = VK_NULL_HANDLE;
    VkResult r = MakeImage(format,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             image);
    if (r != VK_SUCCESS) {
        detail_ = format_name + ", " + detail_;
        return r;
    }
    if ((r = Begin()) != VK_SUCCESS) return r;
    const VkImageSubresourceRange range{
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &b);
    const VkClearDepthStencilValue value{1.0f, 0};
    CmdClearDepthStencilImage(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                              &range);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &b);
    r = SubmitAndWait();
    if (r != VK_SUCCESS) detail_ = format_name + ", " + detail_;
    return r;
}

VkResult VulkanSelfTest::StepAzaharPipeline(const uint32_t* code, size_t code_size,
                                            bool storage_image) {
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = code_size;
    smci.pCode = code;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkResult r = CreateShaderModule(device_, &smci, nullptr, &shader);
    if (r != VK_SUCCESS) {
        detail_ = "vkCreateShaderModule";
        return r;
    }
    shaders_.push_back(shader);

    // Same bindings and push constants as Azahar's BlitHelper (COMPUTE_BINDINGS /
    // COMPUTE_BUFFER_BINDINGS, ComputeInfo = 3 x ivec2).
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[0].descriptorType = storage_image ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorType = bindings[0].descriptorType;
    bindings[2].descriptorType =
        storage_image ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 3;
    dslci.pBindings = bindings;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if ((r = CreateDescriptorSetLayout(device_, &dslci, nullptr, &set_layout)) != VK_SUCCESS) {
        detail_ = "vkCreateDescriptorSetLayout";
        return r;
    }
    set_layouts_.push_back(set_layout);
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 6 * sizeof(int32_t)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if ((r = CreatePipelineLayout(device_, &plci, nullptr, &layout)) != VK_SUCCESS) {
        detail_ = "vkCreatePipelineLayout";
        return r;
    }
    pipeline_layouts_.push_back(layout);

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shader;
    cpci.stage.pName = "main";
    cpci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    r = CreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        detail_ = "vkCreateComputePipelines (see the [driver] lines above for why)";
        return r;
    }
    if (pipeline) pipelines_.push_back(pipeline);
    return VK_SUCCESS;
}

void VulkanSelfTest::Teardown() {
    if (!device_) return;
    if (!DestroyDevice)
        DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(gipa_(instance_, "vkDestroyDevice"));
    if (DeviceWaitIdle) DeviceWaitIdle(device_); // result ignored: lost devices still tear down
    if (DestroyPipeline)
        for (VkPipeline p : pipelines_) DestroyPipeline(device_, p, nullptr);
    if (DestroyPipelineLayout)
        for (VkPipelineLayout l : pipeline_layouts_) DestroyPipelineLayout(device_, l, nullptr);
    if (DestroyDescriptorPool)
        for (VkDescriptorPool p : descriptor_pools_) DestroyDescriptorPool(device_, p, nullptr);
    if (DestroyDescriptorSetLayout)
        for (VkDescriptorSetLayout l : set_layouts_) DestroyDescriptorSetLayout(device_, l, nullptr);
    if (DestroyShaderModule)
        for (VkShaderModule s : shaders_) DestroyShaderModule(device_, s, nullptr);
    if (DestroyImage)
        for (VkImage i : images_) DestroyImage(device_, i, nullptr);
    if (DestroyBuffer)
        for (VkBuffer b : buffers_) DestroyBuffer(device_, b, nullptr);
    if (FreeMemory)
        for (VkDeviceMemory m : memories_) FreeMemory(device_, m, nullptr);
    if (fence_ && DestroyFence) DestroyFence(device_, fence_, nullptr);
    if (pool_ && DestroyCommandPool) DestroyCommandPool(device_, pool_, nullptr); // frees cmd_
    pipelines_.clear();
    pipeline_layouts_.clear();
    descriptor_pools_.clear();
    set_layouts_.clear();
    shaders_.clear();
    images_.clear();
    buffers_.clear();
    memories_.clear();
    fence_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    cmd_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    if (DestroyDevice) DestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
}

} // namespace

bool VulkanHost::RunSelfTest(std::string& error) {
    if (!kRunVulkanSelfTest) return true;
    ONYX_INFO("Vulkan self-test: starting on a temporary device");
    VulkanSelfTest test(instance_, gpu_, driver_.GetInstanceProcAddr(), fn_->GetDeviceProcAddr,
                        queue_family_, memory_props_, presenter_.Device());
    const bool ok = test.Run(error);
    DrainStderrToLog(); // anything the driver printed instead of logging
    return ok;
}

} // namespace onyx::app
