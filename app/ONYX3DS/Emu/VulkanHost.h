// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Vulkan half of the libretro hardware-render contract, backed by Dozen.
//
//   * DozenDriver loads vulkan_dzn.dll from the app package and talks to it
//     through the ICD interface directly (there is no Vulkan loader on Xbox).
//   * VulkanHost creates the VkInstance, lets the Azahar core create the
//     device through the context-negotiation interface, implements
//     retro_hw_render_interface_vulkan, and on every frame copies the core's
//     output image into a D3D12 texture shared with D3D12Presenter.
#pragma once

#include "Emu/D3D12Presenter.h"

namespace onyx::app {

class DozenDriver {
public:
    ~DozenDriver();
    bool Load(std::string& error);
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr() const { return gipa_; }
    uint32_t InterfaceVersion() const { return icd_version_; }

private:
    HMODULE module_ = nullptr;
    PFN_vkGetInstanceProcAddr gipa_ = nullptr;
    uint32_t icd_version_ = 0;
};

// Result of the startup self-test shown in Settings > System check.
struct VulkanProbe {
    bool driver_loaded = false;
    bool instance_created = false;
    bool device_found = false;
    bool luid_matched = false;
    bool external_memory = false;
    bool external_fence = false;
    bool timeline = false;
    std::string device_name;
    std::string driver_info;
    uint32_t api_version = 0;
    std::vector<std::string> notes;
};

class VulkanHost {
public:
    VulkanHost(DozenDriver& driver, D3D12Presenter& presenter);
    ~VulkanHost();
    VulkanHost(const VulkanHost&) = delete;
    VulkanHost& operator=(const VulkanHost&) = delete;

    // Instance + physical device. Safe to call before a game is chosen; also
    // used by the System Check page as a capability probe.
    bool CreateInstance(VulkanProbe& probe, std::string& error);

    // RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE
    void SetNegotiationInterface(const retro_hw_render_context_negotiation_interface_vulkan* iface);
    // Called right before the core's context_reset: builds the device (via the
    // core when it offered a negotiation interface).
    bool CreateDevice(std::string& error);
    // RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE
    const retro_hw_render_interface_vulkan* Interface() const { return &iface_; }

    // video_refresh with RETRO_HW_FRAME_BUFFER_VALID.
    void OnFrame(unsigned width, unsigned height);

    void DestroyDevice();
    bool HasDevice() const { return device_ != VK_NULL_HANDLE; }
    bool UsingGpuSync() const { return gpu_sync_; }

private:
    struct Fn; // loaded entry points
    struct SlotImport {
        uint32_t generation = 0;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    bool LoadDeviceFunctions();
    bool ImportSlot(int slot, const D3D12Presenter::SharedSlot& shared);
    void ReleaseSlot(SlotImport& s);
    bool ImportSharedFence();
    uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const;

    // retro_hw_render_interface_vulkan callbacks (handle = this)
    static void SetImage(void* h, const retro_vulkan_image* image, uint32_t num_semaphores,
                         const VkSemaphore* semaphores, uint32_t src_queue_family);
    static uint32_t GetSyncIndex(void* h);
    static uint32_t GetSyncIndexMask(void* h);
    static void SetCommandBuffers(void* h, uint32_t num, const VkCommandBuffer* cmd);
    static void WaitSyncIndex(void* h);
    static void LockQueue(void* h);
    static void UnlockQueue(void* h);
    static void SetSignalSemaphore(void* h, VkSemaphore semaphore);

    DozenDriver& driver_;
    D3D12Presenter& presenter_;
    std::unique_ptr<Fn> fn_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkPhysicalDeviceMemoryProperties memory_props_{};
    std::vector<std::string> device_extensions_;
    bool has_external_memory_ = false;
    bool has_external_semaphore_ = false;
    bool has_timeline_ = false;

    const retro_hw_render_context_negotiation_interface_vulkan* negotiation_ = nullptr;
    retro_hw_render_interface_vulkan iface_{};

    static constexpr uint32_t kSyncFrames = 2;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kSyncFrames> cmd_{};
    std::array<VkFence, kSyncFrames> fences_{};
    std::array<bool, kSyncFrames> fence_pending_{};
    uint32_t sync_index_ = 0;

    std::mutex queue_mutex_;
    retro_vulkan_image image_{};
    bool have_image_ = false;
    std::vector<VkSemaphore> wait_semaphores_;
    std::vector<VkPipelineStageFlags> wait_stages_;
    std::vector<VkCommandBuffer> core_cmds_;
    VkSemaphore signal_semaphore_ = VK_NULL_HANDLE;

    std::array<SlotImport, D3D12Presenter::kSlots> imports_{};
    VkSemaphore shared_timeline_ = VK_NULL_HANDLE;
    uint64_t timeline_value_ = 0;
    bool gpu_sync_ = false;
};

} // namespace onyx::app
