// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Emu/VulkanHost.h"

#include "Platform/Log.h"

namespace onyx::app {

// ---------------------------------------------------------------------------
// DozenDriver

typedef VkResult(VKAPI_PTR* PFN_IcdNegotiate)(uint32_t* version);

DozenDriver::~DozenDriver() {
    if (module_) FreeLibrary(module_);
}

bool DozenDriver::Load(std::string& error) {
    if (gipa_) return true;
    module_ = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
    if (!module_) {
        error = "vulkan_dzn.dll is missing from the app package (error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
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
    if (!has_external_memory_) {
        error = "Dozen lacks VK_KHR_external_memory_win32, frames cannot reach the screen";
        probe.notes.push_back(error);
        return false;
    }
    if (!has_external_semaphore_ || !has_timeline_)
        probe.notes.push_back("No shared fence support: falling back to CPU frame sync (slower)");
    ONYX_INFO("Vulkan GPU: %s (%s)", probe.device_name.c_str(), probe.driver_info.c_str());
    return true;
}

void VulkanHost::SetNegotiationInterface(
    const retro_hw_render_context_negotiation_interface_vulkan* iface) {
    negotiation_ = iface;
}

bool VulkanHost::CreateDevice(std::string& error) {
    if (device_) return true;
    auto& f = *fn_;
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

    gpu_sync_ = ImportSharedFence();
    ONYX_INFO("Vulkan device ready; frame sync on %s", gpu_sync_ ? "GPU (shared fence)" : "CPU");

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

} // namespace onyx::app
