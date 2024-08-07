#pragma once

#include <optional>
#include <vector>
#include <filesystem>
#include <array>
#include <queue>

#include <vma/vk_mem_alloc.h>

#include "libs.hpp"
#include "globals.hpp"
#include "mesh/model.hpp"
#include "vk/cmd.hpp"
#include "vk/image.hpp"
#include "vk/pipeline.hpp"

class RenderTarget;
class InputManager;
class Model;
class Camera;
class Buffer;
class GraphicsPipeline;
class DescriptorSet;
class SwapChain;
class GuiRenderer;
class AccelerationStructure;

static constexpr std::array validationLayers{
    "VK_LAYER_KHRONOS_validation"
};

static constexpr std::array deviceExtensions{
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_MAINTENANCE2_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_KHR_MULTIVIEW_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
};

#ifdef NDEBUG
constexpr bool enableValidationLayers = false;
#else
constexpr bool enableValidationLayers = true;
#endif

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsComputeFamily;
    std::optional<uint32_t> presentFamily;

    [[nodiscard]] bool isComplete() const {
        return graphicsComputeFamily.has_value() && presentFamily.has_value();
    }
};

/**
 * Information held in the fragment shader's uniform buffer.
 * This (obviously) has to exactly match the corresponding definition in the fragment shader.
 */
struct GraphicsUBO {
    struct WindowRes {
        uint32_t windowWidth;
        uint32_t windowHeight;
    };

    struct Matrices {
        glm::mat4 model;
        glm::mat4 view;
        glm::mat4 proj;
        glm::mat4 inverseVp;
        glm::mat4 staticView;
        glm::mat4 cubemapCaptureViews[6];
        glm::mat4 cubemapCaptureProj;
    };

    struct MiscData {
        float debugNumber;
        float zNear;
        float zFar;
        uint32_t useSsao;
        float lightIntensity;
        glm::vec3 lightDir;
        glm::vec3 lightColor;
        glm::vec3 cameraPos;
    };

    alignas(16) WindowRes window{};
    alignas(16) Matrices matrices{};
    alignas(16) MiscData misc{};
};

struct ScenePushConstants {
    uint32_t materialID;
};

/**
 * Simple RAII-preserving wrapper class for the VMA allocator.
 */
class VmaAllocatorWrapper {
    VmaAllocator allocator{};

public:
    VmaAllocatorWrapper(vk::PhysicalDevice physicalDevice, vk::Device device, vk::Instance instance);

    ~VmaAllocatorWrapper();

    VmaAllocatorWrapper(const VmaAllocatorWrapper &other) = delete;

    VmaAllocatorWrapper(VmaAllocatorWrapper &&other) = delete;

    VmaAllocatorWrapper &operator=(const VmaAllocatorWrapper &other) = delete;

    VmaAllocatorWrapper &operator=(VmaAllocatorWrapper &&other) = delete;

    [[nodiscard]] VmaAllocator operator*() const { return allocator; }
};

/**
 * Helper structure used to pass handles to essential Vulkan objects which are used while interacting with the API.
 * Introduced so that we can preserve top-down data flow and no object needs to refer to a renderer object
 * to get access to these.
 */
struct RendererContext {
    unique_ptr<vk::raii::PhysicalDevice> physicalDevice;
    unique_ptr<vk::raii::Device> device;
    unique_ptr<vk::raii::CommandPool> commandPool;
    unique_ptr<vk::raii::Queue> graphicsQueue;
    unique_ptr<VmaAllocatorWrapper> allocator;
};

class RenderInfo {
    GraphicsPipelineBuilder cachedPipelineBuilder;
    shared_ptr<GraphicsPipeline> pipeline;

    std::vector<RenderTarget> colorTargets;
    std::optional<RenderTarget> depthTarget;

    std::vector<vk::RenderingAttachmentInfo> colorAttachments;
    std::optional<vk::RenderingAttachmentInfo> depthAttachment;

    std::vector<vk::Format> cachedColorAttachmentFormats;

public:
    RenderInfo(GraphicsPipelineBuilder builder, shared_ptr<GraphicsPipeline> pipeline, std::vector<RenderTarget> colors);

    RenderInfo(GraphicsPipelineBuilder builder, shared_ptr<GraphicsPipeline> pipeline, std::vector<RenderTarget> colors,
               RenderTarget depth);

    RenderInfo(std::vector<RenderTarget> colors);

    RenderInfo(std::vector<RenderTarget> colors, RenderTarget depth);

    [[nodiscard]] vk::RenderingInfo get(vk::Extent2D extent, uint32_t views = 1, vk::RenderingFlags flags = {}) const;

    [[nodiscard]] const GraphicsPipeline &getPipeline() const { return *pipeline; }

    [[nodiscard]] vk::CommandBufferInheritanceRenderingInfo getInheritanceRenderingInfo();

    void reloadShaders(const RendererContext& ctx) const;

private:
    void makeAttachmentInfos();
};

class VulkanRenderer {
    using TimelineSemValueType = std::uint64_t;

    struct GLFWwindow *window = nullptr;

    unique_ptr<Camera> camera;

    unique_ptr<InputManager> inputManager;

    vk::raii::Context vkCtx;
    unique_ptr<vk::raii::Instance> instance;
    unique_ptr<vk::raii::DebugUtilsMessengerEXT> debugMessenger;
    unique_ptr<vk::raii::SurfaceKHR> surface;

    RendererContext ctx;

    unique_ptr<vk::raii::Queue> presentQueue;

    unique_ptr<SwapChain> swapChain;

    unique_ptr<Model> model;
    Material separateMaterial;

    unique_ptr<Texture> ssaoTexture;
    unique_ptr<Texture> ssaoNoiseTexture;

    struct {
        unique_ptr<Texture> depth;
        unique_ptr<Texture> normal;
        unique_ptr<Texture> pos;
    } gBufferTextures;

    unique_ptr<Texture> skyboxTexture;
    unique_ptr<Texture> envmapTexture;

    unique_ptr<vk::raii::DescriptorPool> descriptorPool;

    unique_ptr<DescriptorSet> materialsDescriptorSet;
    unique_ptr<DescriptorSet> cubemapCaptureDescriptorSet;
    unique_ptr<DescriptorSet> debugQuadDescriptorSet;
    std::vector<DescriptorSet> renderTargetDescriptorSets;

    std::vector<RenderInfo> sceneRenderInfos;
    std::vector<RenderInfo> skyboxRenderInfos;
    std::vector<RenderInfo> guiRenderInfos;
    unique_ptr<RenderInfo> prepassRenderInfo;
    unique_ptr<RenderInfo> ssaoRenderInfo;
    unique_ptr<RenderInfo> cubemapCaptureRenderInfo;
    std::vector<RenderInfo> debugQuadRenderInfos;

    unique_ptr<Buffer> skyboxVertexBuffer;
    unique_ptr<Buffer> screenSpaceQuadVertexBuffer;

    unique_ptr<AccelerationStructure> tlas;

    struct FrameResources {
        struct {
            struct Timeline {
                unique_ptr<vk::raii::Semaphore> semaphore;
                TimelineSemValueType timeline = 0;
            };

            unique_ptr<vk::raii::Semaphore> imageAvailableSemaphore;
            unique_ptr<vk::raii::Semaphore> readyToPresentSemaphore;
            Timeline renderFinishedTimeline;
        } sync;

        // primary command buffer
        unique_ptr<vk::raii::CommandBuffer> graphicsCmdBuffer;

        SecondaryCommandBuffer sceneCmdBuffer;
        SecondaryCommandBuffer prepassCmdBuffer;
        SecondaryCommandBuffer ssaoCmdBuffer;
        SecondaryCommandBuffer guiCmdBuffer;
        SecondaryCommandBuffer debugCmdBuffer;

        unique_ptr<Buffer> graphicsUniformBuffer;
        void *graphicsUboMapped{};

        unique_ptr<DescriptorSet> sceneDescriptorSet;
        unique_ptr<DescriptorSet> skyboxDescriptorSet;
        unique_ptr<DescriptorSet> prepassDescriptorSet;
        unique_ptr<DescriptorSet> ssaoDescriptorSet;
        unique_ptr<DescriptorSet> rtDescriptorSet;
    };

    static constexpr size_t MAX_FRAMES_IN_FLIGHT = 3;
    std::array<FrameResources, MAX_FRAMES_IN_FLIGHT> frameResources;

    using FrameBeginCallback = std::function<void()>;
    std::queue<FrameBeginCallback> queuedFrameBeginActions;

    vk::SampleCountFlagBits msaaSampleCount = vk::SampleCountFlagBits::e1;

    unique_ptr<vk::raii::DescriptorPool> imguiDescriptorPool;
    unique_ptr<GuiRenderer> guiRenderer;

    // miscellaneous constants

    static constexpr auto prepassColorFormat = vk::Format::eR16G16B16A16Sfloat;
    static constexpr auto hdrEnvmapFormat = vk::Format::eR32G32B32A32Sfloat;

    static constexpr uint32_t MATERIAL_TEX_ARRAY_SIZE = 32;

    // miscellaneous state variables

    uint32_t currentFrameIdx = 0;

    bool framebufferResized = false;

    glm::vec3 backgroundColor = glm::vec3(26, 26, 26) / 255.0f;

    float modelScale = 1.0f;
    glm::vec3 modelTranslate{};
    glm::quat modelRotation{1, 0, 0, 0};

    glm::quat lightDirection = glm::normalize(glm::vec3(1, 1.5, -2));
    glm::vec3 lightColor = glm::normalize(glm::vec3(23.47, 21.31, 20.79));
    float lightIntensity = 20.0f;

    float debugNumber = 0;

    bool cullBackFaces = false;
    bool wireframeMode = false;
    bool useSsao = false;
    bool useMsaa = false;

public:
    explicit VulkanRenderer();

    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer &other) = delete;

    VulkanRenderer(VulkanRenderer &&other) = delete;

    VulkanRenderer &operator=(const VulkanRenderer &other) = delete;

    VulkanRenderer &operator=(VulkanRenderer &&other) = delete;

    [[nodiscard]] GLFWwindow *getWindow() const { return window; }

    [[nodiscard]] GuiRenderer &getGuiRenderer() const { return *guiRenderer; }

    [[nodiscard]] vk::SampleCountFlagBits getMsaaSampleCount() const {
        return useMsaa ? msaaSampleCount : vk::SampleCountFlagBits::e1;
    }

    void tick(float deltaTime);

    /**
     * Waits until the device has completed all previously submitted commands.
     */
    void waitIdle() const { ctx.device->waitIdle(); }

    void loadModelWithMaterials(const std::filesystem::path &path);

    void loadModel(const std::filesystem::path &path);

    void loadBaseColorTexture(const std::filesystem::path &path);

    void loadNormalMap(const std::filesystem::path &path);

    void loadOrmMap(const std::filesystem::path &path);

    void loadOrmMap(const std::filesystem::path &aoPath, const std::filesystem::path &roughnessPath,
                    const std::filesystem::path &metallicPath);

    void loadRmaMap(const std::filesystem::path &path);

    void loadEnvironmentMap(const std::filesystem::path &path);

    void reloadShaders() const;

private:
    static void framebufferResizeCallback(GLFWwindow *window, int width, int height);

    void bindMouseDragActions();

    // ==================== instance creation ====================

    void createInstance();

    static std::vector<const char *> getRequiredExtensions();

    // ==================== validation layers ====================

    static bool checkValidationLayerSupport();

    static vk::DebugUtilsMessengerCreateInfoEXT makeDebugMessengerCreateInfo();

    void setupDebugMessenger();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
        void *pUserData
    );

    // ==================== window surface ====================

    void createSurface();

    // ==================== physical device ====================

    void pickPhysicalDevice();

    [[nodiscard]] bool isDeviceSuitable(const vk::raii::PhysicalDevice &physicalDevice) const;

    [[nodiscard]] QueueFamilyIndices findQueueFamilies(const vk::raii::PhysicalDevice &physicalDevice) const;

    static bool checkDeviceExtensionSupport(const vk::raii::PhysicalDevice &physicalDevice);

    // ==================== logical device ====================

    void createLogicalDevice();

    // ==================== assets ====================

    void createSkyboxTexture();

    void createPrepassTextures();

    void createSsaoTextures();

    // ==================== swap chain ====================

    void recreateSwapChain();

    // ==================== descriptors ====================

    void createDescriptorPool();

    void createSceneDescriptorSets();

    void createMaterialsDescriptorSet();

    void createSkyboxDescriptorSets();

    void createPrepassDescriptorSets();

    void createRenderTargetDescriptorSets();

    void createSsaoDescriptorSets();

    void createCubemapCaptureDescriptorSet();

    void createDebugQuadDescriptorSet();

    void createRtDescriptorSets();

    // ==================== render infos ====================

    void createSceneRenderInfos();

    void createSkyboxRenderInfos();

    void createGuiRenderInfos();

    void createPrepassRenderInfo();

    void createSsaoRenderInfo();

    void createCubemapCaptureRenderInfo();

    void createDebugQuadRenderInfos();

    // ==================== multisampling ====================

    [[nodiscard]] vk::SampleCountFlagBits getMaxUsableSampleCount() const;

    // ==================== buffers ====================

    void createSkyboxVertexBuffer();

    void createScreenSpaceQuadVertexBuffer();

    template<typename ElemType>
    unique_ptr<Buffer> createLocalBuffer(const std::vector<ElemType> &contents, vk::BufferUsageFlags usage);

    void createUniformBuffers();

    // ==================== commands ====================

    void createCommandPool();

    void createCommandBuffers();

    void recordGraphicsCommandBuffer();

    // ==================== sync ====================

    void createSyncObjects();

    // ==================== ray tracing ====================

    void createTLAS();

    // ==================== gui ====================

    void initImgui();

public:
    void renderGuiSection();

    // ==================== render loop ====================

    bool startFrame();

    void endFrame();

    void renderGui(const std::function<void()> &renderCommands);

    void runPrepass();

    void runSsaoPass();

    void drawScene();

    void drawDebugQuad();

private:
    void drawModel(const vk::raii::CommandBuffer &commandBuffer, bool doPushConstants,
                   const GraphicsPipeline &pipeline) const;

    void captureCubemap() const;

    void updateGraphicsUniformBuffer() const;
};