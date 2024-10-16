#include "renderer.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <iostream>
#include <stdexcept>
#include <optional>
#include <vector>
#include <filesystem>
#include <array>
#include <random>

#include "camera.hpp"
#include "gui/gui.hpp"
#include "mesh/model.hpp"
#include "mesh/vertex.hpp"
#include "src/utils/glfw-statics.hpp"
#include "vk/buffer.hpp"
#include "vk/swapchain.hpp"
#include "vk/cmd.hpp"
#include "vk/descriptor.hpp"
#include "vk/pipeline.hpp"
#include "vk/accel-struct.hpp"
#include "vk/ctx.hpp"

#include <vk-bootstrap/VkBootstrap.h>

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
        glm::mat4 viewInverse;
        glm::mat4 projInverse;
        glm::mat4 vpInverse;
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

namespace zrx {
VulkanRenderer::VulkanRenderer() {
    constexpr int INIT_WINDOW_WIDTH  = 1200;
    constexpr int INIT_WINDOW_HEIGHT = 800;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window = glfwCreateWindow(INIT_WINDOW_WIDTH, INIT_WINDOW_HEIGHT, "Rayzor", nullptr, nullptr);

    initGlfwUserPointer(window);
    auto *userData = static_cast<GlfwStaticUserData *>(glfwGetWindowUserPointer(window));
    if (!userData) throw std::runtime_error("unexpected null window user pointer");
    userData->renderer = this;

    glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);

    camera = make_unique<Camera>(window);

    inputManager = make_unique<InputManager>(window);
    bindMouseDragActions();

    const auto vkbInstance = createInstance();
    createSurface();
    const auto vkbPhysicalDevice = pickPhysicalDevice(vkbInstance);
    createLogicalDevice(vkbPhysicalDevice);

    ctx.allocator = make_unique<VmaAllocatorWrapper>(**ctx.physicalDevice, **ctx.device, **instance);

    swapChain = make_unique<SwapChain>(
        ctx,
        *surface,
        queueFamilyIndices,
        window,
        getMsaaSampleCount()
    );

    createCommandPool();
    createCommandBuffers();

    createDescriptorPool();

    createUniformBuffers();
    updateGraphicsUniformBuffer();

    createPrepassTextures();
    // createPrepassDescriptorSets();
    // createPrepassRenderInfo();
    //
    createSsaoTextures();
    // createSsaoDescriptorSets();
    // createSsaoRenderInfo();
    //
    createSkyboxVertexBuffer();
    createSkyboxTexture();
    // createSkyboxDescriptorSets();
    // createSkyboxRenderInfos();
    //
    // createCubemapCaptureDescriptorSet();
    // createCubemapCaptureRenderInfo();
    //
    createScreenSpaceQuadVertexBuffer();
    //
    createMaterialsDescriptorSet();
    // createSceneDescriptorSets();
    // createSceneRenderInfos();
    // createGuiRenderInfos();

    loadModel("../assets/example models/kettle/kettle.obj");
    loadBaseColorTexture("../assets/example models/kettle/kettle-albedo.png");
    loadNormalMap("../assets/example models/kettle/kettle-normal.png");
    loadOrmMap("../assets/example models/kettle/kettle-orm.png");
    createTLAS();

    // createMeshesDescriptorSet();
    //
    // createRtTargetTexture();
    // createRtDescriptorSets();
    // createRtPipeline();
    //
    // loadEnvironmentMap("../assets/envmaps/vienna.hdr");
    //
    // createDebugQuadDescriptorSet();
    // createDebugQuadRenderInfos();

    createBindlessDescriptorSets();

    createSyncObjects();

    initImgui();
}

VulkanRenderer::~VulkanRenderer() {
    glfwDestroyWindow(window);
}

void VulkanRenderer::framebufferResizeCallback(GLFWwindow *window, const int width, const int height) {
    (void) (width + height);
    const auto userData = static_cast<GlfwStaticUserData *>(glfwGetWindowUserPointer(window));
    if (!userData) throw std::runtime_error("unexpected null window user pointer");
    userData->renderer->framebufferResized = true;
}

void VulkanRenderer::bindMouseDragActions() {
    inputManager->bindMouseDragCallback(GLFW_MOUSE_BUTTON_RIGHT, [&](const double dx, const double dy) {
        static constexpr float speed = 0.002;
        const float cameraDistance   = glm::length(camera->getPos());

        const auto viewVectors = camera->getViewVectors();

        modelTranslate += cameraDistance * speed * viewVectors.right * static_cast<float>(dx);
        modelTranslate -= cameraDistance * speed * viewVectors.up * static_cast<float>(dy);
    });
}

// ==================== instance creation ====================

vkb::Instance VulkanRenderer::createInstance() {
    auto instanceResult = vkb::InstanceBuilder().set_app_name("Rayzor")
            .request_validation_layers()
            .enable_layer("VK_LAYER_KHRONOS_validation")
            .set_debug_callback([](const VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                   const VkDebugUtilsMessageTypeFlagsEXT messageType,
                                   const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
                                   void *pUserData) -> VkBool32 {
                    const auto severity = vkb::to_string_message_severity(messageSeverity);
                    const auto type = vkb::to_string_message_type(messageType);

                    std::stringstream ss;
                    ss << "[" << severity << ": " << type << "]\n" << pCallbackData->pMessage << "\n\n";

                    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
                        std::cerr << ss.str();
                    } else {
                        std::cout << ss.str();
                    }

                    return VK_FALSE;
                }
            )
            .require_api_version(1, 3)
            .set_minimum_instance_version(1, 3)
            .enable_extensions(getRequiredExtensions())
            .build();

    if (!instanceResult) {
        throw std::runtime_error("failed to create instance: " + instanceResult.error().message());
    }

    instance = make_unique<vk::raii::Instance>(vkCtx, instanceResult.value().instance);

    return instanceResult.value();
}

std::vector<const char *> VulkanRenderer::getRequiredExtensions() {
    uint32_t glfwExtensionCount = 0;
    const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::vector extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

    if (enableValidationLayers) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return extensions;
}

// ==================== startup ====================

void VulkanRenderer::createSurface() {
    VkSurfaceKHR _surface;

    if (glfwCreateWindowSurface(**instance, window, nullptr, &_surface) != VK_SUCCESS) {
        throw std::runtime_error("failed to create window surface!");
    }

    surface = make_unique<vk::raii::SurfaceKHR>(*instance, _surface);
}

vkb::PhysicalDevice VulkanRenderer::pickPhysicalDevice(const vkb::Instance &vkbInstance) {
    auto physicalDeviceResult = vkb::PhysicalDeviceSelector(vkbInstance, **surface)
            .set_minimum_version(1, 3)
            .require_dedicated_transfer_queue()
            .prefer_gpu_device_type()
            .require_present()
            .add_required_extensions(deviceExtensions)
            .set_required_features(vk::PhysicalDeviceFeatures{
                .fillModeNonSolid = vk::True,
                .samplerAnisotropy = vk::True,
            })
            .set_required_features_12(vk::PhysicalDeviceVulkan12Features{
                .descriptorIndexing = vk::True,
                .shaderUniformBufferArrayNonUniformIndexing = vk::True,
                .shaderSampledImageArrayNonUniformIndexing = vk::True,
                .shaderStorageBufferArrayNonUniformIndexing = vk::True,
                // .descriptorBindingUniformBufferUpdateAfterBind = vk::True,
                .descriptorBindingSampledImageUpdateAfterBind = vk::True,
                .descriptorBindingStorageBufferUpdateAfterBind = vk::True,
                .descriptorBindingPartiallyBound = vk::True,
                .timelineSemaphore = vk::True,
                .bufferDeviceAddress = vk::True,
            })
            .add_required_extension_features(vk::PhysicalDeviceDynamicRenderingFeatures{
                .dynamicRendering = vk::True,
            })
            .add_required_extension_features(vk::PhysicalDeviceSynchronization2FeaturesKHR{
                .synchronization2 = vk::True,
            })
            .add_required_extension_features(vk::PhysicalDeviceMultiviewFeatures{
                .multiview = vk::True,
            })
            .add_required_extension_features(vk::PhysicalDeviceAccelerationStructureFeaturesKHR{
                .accelerationStructure = vk::True,
            })
            .add_required_extension_features(vk::PhysicalDeviceRayTracingPipelineFeaturesKHR{
                .rayTracingPipeline = vk::True,
            })
            .select();

    if (!physicalDeviceResult) {
        throw std::runtime_error("failed to select physical device: " + physicalDeviceResult.error().message());
    }

    ctx.physicalDevice = make_unique<vk::raii::PhysicalDevice>(*instance, physicalDeviceResult.value().physical_device);
    msaaSampleCount    = getMaxUsableSampleCount();

    return physicalDeviceResult.value();
}

void VulkanRenderer::createLogicalDevice(const vkb::PhysicalDevice &vkbPhysicalDevice) {
    auto deviceResult = vkb::DeviceBuilder(vkbPhysicalDevice).build();

    if (!deviceResult) {
        throw std::runtime_error("failed to select logical device: " + deviceResult.error().message());
    }

    ctx.device = make_unique<vk::raii::Device>(*ctx.physicalDevice, deviceResult.value().device);

    auto graphicsQueueResult      = deviceResult.value().get_queue(vkb::QueueType::graphics);
    auto graphicsQueueIndexResult = deviceResult.value().get_queue_index(vkb::QueueType::graphics);
    if (!graphicsQueueResult || !graphicsQueueIndexResult) {
        throw std::runtime_error("failed to get graphics queue: " + deviceResult.error().message());
    }

    auto presentQueueResult      = deviceResult.value().get_queue(vkb::QueueType::present);
    auto presentQueueIndexResult = deviceResult.value().get_queue_index(vkb::QueueType::present);
    if (!presentQueueResult || !presentQueueIndexResult) {
        throw std::runtime_error("failed to get present queue: " + deviceResult.error().message());
    }

    ctx.graphicsQueue = make_unique<vk::raii::Queue>(*ctx.device, graphicsQueueResult.value());
    presentQueue      = make_unique<vk::raii::Queue>(*ctx.device, presentQueueResult.value());

    queueFamilyIndices = {
        .graphicsComputeFamily = graphicsQueueIndexResult.value(),
        .presentFamily = presentQueueIndexResult.value()
    };
}

// ==================== models ====================

void VulkanRenderer::loadModelWithMaterials(const std::filesystem::path &path) {
    waitIdle();

    model.reset();
    model = make_unique<Model>(ctx, path, true);

    const auto &materials = model->getMaterials();

    for (uint32_t i = 0; i < materials.size(); i++) {
        const auto &material = materials[i];

        if (material.baseColor) {
            materialsDescriptorSet->queueUpdate<0>(*material.baseColor, i);
        }

        if (material.normal) {
            materialsDescriptorSet->queueUpdate<1>(*material.normal, i);
        }

        if (material.orm) {
            materialsDescriptorSet->queueUpdate<2>(*material.orm, i);
        }
    }

    materialsDescriptorSet->commitUpdates();
}

void VulkanRenderer::loadModel(const std::filesystem::path &path) {
    waitIdle();

    model.reset();
    model = make_unique<Model>(ctx, path, false);
}

// ==================== assets ====================

void VulkanRenderer::loadBaseColorTexture(const std::filesystem::path &path) {
    waitIdle();

    separateMaterial.baseColor.reset();
    separateMaterial.baseColor = TextureBuilder()
            .fromPaths({path})
            .makeMipmaps()
            .create(ctx);

    materialsDescriptorSet->updateBinding<0>(*separateMaterial.baseColor);
}

void VulkanRenderer::loadNormalMap(const std::filesystem::path &path) {
    waitIdle();

    separateMaterial.normal.reset();
    separateMaterial.normal = TextureBuilder()
            .useFormat(vk::Format::eR8G8B8A8Unorm)
            .fromPaths({path})
            .create(ctx);

    materialsDescriptorSet->updateBinding<1>(*separateMaterial.normal);
}

void VulkanRenderer::loadOrmMap(const std::filesystem::path &path) {
    waitIdle();

    separateMaterial.orm.reset();
    separateMaterial.orm = TextureBuilder()
            .useFormat(vk::Format::eR8G8B8A8Unorm)
            .fromPaths({path})
            .create(ctx);

    materialsDescriptorSet->updateBinding<2>(*separateMaterial.orm);
}

void VulkanRenderer::loadOrmMap(const std::filesystem::path &aoPath, const std::filesystem::path &roughnessPath,
                                const std::filesystem::path &metallicPath) {
    waitIdle();

    separateMaterial.orm.reset();
    separateMaterial.orm = TextureBuilder()
            .useFormat(vk::Format::eR8G8B8A8Unorm)
            .asSeparateChannels()
            .fromPaths({aoPath, roughnessPath, metallicPath})
            .withSwizzle({
                aoPath.empty() ? SwizzleComponent::MAX : SwizzleComponent::R,
                SwizzleComponent::G,
                metallicPath.empty() ? SwizzleComponent::ZERO : SwizzleComponent::B,
                SwizzleComponent::A
            })
            .makeMipmaps()
            .create(ctx);

    materialsDescriptorSet->updateBinding<2>(*separateMaterial.orm);
}

void VulkanRenderer::loadRmaMap(const std::filesystem::path &path) {
    waitIdle();

    separateMaterial.orm.reset();
    separateMaterial.orm = TextureBuilder()
            .withSwizzle({
                SwizzleComponent::B, SwizzleComponent::R, SwizzleComponent::G, SwizzleComponent::A
            })
            .useFormat(vk::Format::eR8G8B8A8Unorm)
            .fromPaths({path})
            .create(ctx);

    materialsDescriptorSet->updateBinding<2>(*separateMaterial.orm);
}

void VulkanRenderer::loadEnvironmentMap(const std::filesystem::path &path) {
    waitIdle();

    envmapTexture = TextureBuilder()
            .asHdr()
            .useFormat(hdrEnvmapFormat)
            .fromPaths({path})
            .withSamplerAddressMode(vk::SamplerAddressMode::eClampToEdge)
            .makeMipmaps()
            .create(ctx);

    cubemapCaptureDescriptorSet->updateBinding<1>(*envmapTexture);

    captureCubemap();
}

void VulkanRenderer::createPrepassTextures() {
    const auto &[width, height] = swapChain->getExtent();

    const vk::Extent3D extent{
        .width = width,
        .height = height,
        .depth = 1
    };

    gBufferTextures.pos = TextureBuilder()
            .asUninitialized(extent)
            .useFormat(prepassColorFormat)
            .useUsage(vk::ImageUsageFlagBits::eTransferSrc
                      | vk::ImageUsageFlagBits::eTransferDst
                      | vk::ImageUsageFlagBits::eSampled
                      | vk::ImageUsageFlagBits::eColorAttachment)
            .create(ctx);

    gBufferTextures.normal = TextureBuilder()
            .asUninitialized(extent)
            .useFormat(prepassColorFormat)
            .useUsage(vk::ImageUsageFlagBits::eTransferSrc
                      | vk::ImageUsageFlagBits::eTransferDst
                      | vk::ImageUsageFlagBits::eSampled
                      | vk::ImageUsageFlagBits::eColorAttachment)
            .create(ctx);

    gBufferTextures.depth = TextureBuilder()
            .asUninitialized(extent)
            .useFormat(swapChain->getDepthFormat())
            .useUsage(vk::ImageUsageFlagBits::eTransferSrc
                      | vk::ImageUsageFlagBits::eTransferDst
                      | vk::ImageUsageFlagBits::eSampled
                      | vk::ImageUsageFlagBits::eDepthStencilAttachment)
            .create(ctx);

    for (auto &res: frameResources) {
        if (res.ssaoDescriptorSet) {
            res.ssaoDescriptorSet->queueUpdate<1>(*gBufferTextures.depth)
                    .queueUpdate<2>(*gBufferTextures.normal)
                    .queueUpdate<3>(*gBufferTextures.pos)
                    .commitUpdates();
        }
    }
}

void VulkanRenderer::createSkyboxTexture() {
    skyboxTexture = TextureBuilder()
            .asCubemap()
            .asUninitialized({2048, 2048, 1})
            .asHdr()
            .useFormat(hdrEnvmapFormat)
            .useUsage(vk::ImageUsageFlagBits::eTransferSrc
                      | vk::ImageUsageFlagBits::eTransferDst
                      | vk::ImageUsageFlagBits::eSampled
                      | vk::ImageUsageFlagBits::eColorAttachment)
            .makeMipmaps()
            .create(ctx);
}

static std::vector<glm::vec4> makeSsaoNoise() {
    std::uniform_real_distribution<float> randomFloats(0.0, 1.0); // random floats between [0.0, 1.0]
    std::default_random_engine generator;

    std::vector<glm::vec4> ssaoNoise;
    for (unsigned int i = 0; i < 16; i++) {
        glm::vec4 noise(
            randomFloats(generator) * 2.0 - 1.0,
            randomFloats(generator) * 2.0 - 1.0,
            0.0f,
            0.0f
        );
        ssaoNoise.push_back(noise);
    }

    return ssaoNoise;
}

void VulkanRenderer::createSsaoTextures() {
    const auto attachmentUsageFlags = vk::ImageUsageFlagBits::eTransferSrc
                                      | vk::ImageUsageFlagBits::eTransferDst
                                      | vk::ImageUsageFlagBits::eSampled
                                      | vk::ImageUsageFlagBits::eColorAttachment;

    const auto &[width, height] = swapChain->getExtent();

    const vk::Extent3D extent{
        .width = width,
        .height = height,
        .depth = 1
    };

    ssaoTexture = TextureBuilder()
            .asUninitialized(extent)
            .useFormat(vk::Format::eR8G8B8A8Unorm)
            .useUsage(attachmentUsageFlags)
            .create(ctx);

    auto noise = makeSsaoNoise();

    ssaoNoiseTexture = TextureBuilder()
            .fromMemory(noise.data(), {4, 4, 1})
            .useFormat(vk::Format::eR32G32B32A32Sfloat)
            .useUsage(attachmentUsageFlags)
            .withSamplerAddressMode(vk::SamplerAddressMode::eRepeat)
            .create(ctx);

    for (auto &res: frameResources) {
        if (res.sceneDescriptorSet) {
            res.sceneDescriptorSet->updateBinding<1>(*ssaoTexture);
        }

        if (res.ssaoDescriptorSet) {
            res.ssaoDescriptorSet->updateBinding<4>(*ssaoNoiseTexture);
        }
    }
}

void VulkanRenderer::createRtTargetTexture() {
    const auto &[width, height] = swapChain->getExtent();

    const vk::Extent3D extent{
        .width = width,
        .height = height,
        .depth = 1
    };

    rtTargetTexture = TextureBuilder()
            .asUninitialized(extent)
            .useFormat(vk::Format::eR32G32B32A32Sfloat)
            .useUsage(vk::ImageUsageFlagBits::eStorage
                      | vk::ImageUsageFlagBits::eSampled
                      | vk::ImageUsageFlagBits::eTransferSrc
                      | vk::ImageUsageFlagBits::eTransferDst)
            .useLayout(vk::ImageLayout::eGeneral)
            .create(ctx);
}

// ==================== swapchain ====================

void VulkanRenderer::recreateSwapChain() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);

    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(window, &width, &height);
        glfwWaitEvents();
    }

    waitIdle();

    swapChain.reset();
    swapChain = make_unique<SwapChain>(
        ctx,
        *surface,
        queueFamilyIndices,
        window,
        getMsaaSampleCount()
    );

    // todo - this shouldn't recreate pipelines
    createSceneRenderInfos();
    createSkyboxRenderInfos();
    createGuiRenderInfos();
    createDebugQuadRenderInfos();

    createPrepassTextures();
    createPrepassRenderInfo();

    createSsaoTextures();
    createSsaoRenderInfo();
}

// ==================== descriptors ====================

void VulkanRenderer::createDescriptorPool() {
    const std::vector<vk::DescriptorPoolSize> poolSizes = {
        {
            .type = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = BINDLESS_DESCRIPTOR_ARRAY_COUNT + 100u,
        },
        {
            .type = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = BINDLESS_DESCRIPTOR_ARRAY_COUNT + 1000u,
        },
        {
            .type = vk::DescriptorType::eStorageImage,
            .descriptorCount = BINDLESS_DESCRIPTOR_ARRAY_COUNT + 100u,
        },
        {
            .type = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = BINDLESS_DESCRIPTOR_ARRAY_COUNT + 100u,
        },
        {
            .type = vk::DescriptorType::eAccelerationStructureKHR,
            .descriptorCount = 100u,
        },
    };

    const vk::DescriptorPoolCreateInfo poolInfo{
        .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet
                 | vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
        .maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) * 6 + 5,
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data(),
    };

    descriptorPool = make_unique<vk::raii::DescriptorPool>(*ctx.device, poolInfo);
}

void VulkanRenderer::createBindlessDescriptorSets() {
    const auto flags = vk::DescriptorBindingFlagBits::ePartiallyBound
                       | vk::DescriptorBindingFlagBits::eUpdateAfterBind;

    bindlessDescriptorSet = make_unique<DescriptorSet<Buffer, Buffer, Texture> >(
        ctx,
        *descriptorPool,
        ResourcePack<Buffer>{
            BINDLESS_DESCRIPTOR_ARRAY_COUNT,
            vk::ShaderStageFlagBits::eAll,
            vk::DescriptorType::eUniformBuffer,
            flags
        },
        ResourcePack<Buffer>{
            BINDLESS_DESCRIPTOR_ARRAY_COUNT,
            vk::ShaderStageFlagBits::eAll,
            vk::DescriptorType::eStorageBuffer,
            flags
        },
        ResourcePack<Texture>{
            BINDLESS_DESCRIPTOR_ARRAY_COUNT,
            vk::ShaderStageFlagBits::eAll,
            vk::DescriptorType::eCombinedImageSampler,
            flags
        }
    );
}

void VulkanRenderer::createSceneDescriptorSets() {
    for (auto &res: frameResources) {
        res.sceneDescriptorSet = make_unique<SceneDescriptorSet>(
            ctx,
            *descriptorPool,
            ResourcePack{
                *res.graphicsUniformBuffer,
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
            },
            ResourcePack{*ssaoTexture, vk::ShaderStageFlagBits::eFragment}
        );
    }
}

void VulkanRenderer::createMaterialsDescriptorSet() {
    constexpr auto scope           = vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eClosestHitKHR;
    constexpr auto type            = vk::DescriptorType::eCombinedImageSampler;
    constexpr auto descriptorCount = MATERIAL_TEX_ARRAY_SIZE;

    materialsDescriptorSet = make_unique<MaterialsDescriptorSet>(
        ctx,
        *descriptorPool,
        ResourcePack<Texture>{descriptorCount, scope, type}, // base colors
        ResourcePack<Texture>{descriptorCount, scope, type}, // normals
        ResourcePack<Texture>{descriptorCount, scope, type}, // orms
        ResourcePack{
            // skybox
            *skyboxTexture,
            vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eMissKHR,
            type
        }
    );
}

void VulkanRenderer::createSkyboxDescriptorSets() {
    for (auto &res: frameResources) {
        res.skyboxDescriptorSet = make_unique<SkyboxDescriptorSet>(
            ctx,
            *descriptorPool,
            ResourcePack{
                *res.graphicsUniformBuffer,
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
            },
            ResourcePack{*skyboxTexture, vk::ShaderStageFlagBits::eFragment}
        );
    }
}

void VulkanRenderer::createPrepassDescriptorSets() {
    for (auto &res: frameResources) {
        res.prepassDescriptorSet = make_unique<PrepassDescriptorSet>(
            ctx,
            *descriptorPool,
            ResourcePack{
                *res.graphicsUniformBuffer,
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
            }
        );
    }
}

void VulkanRenderer::createSsaoDescriptorSets() {
    for (auto &res: frameResources) {
        res.ssaoDescriptorSet = make_unique<SsaoDescriptorSet>(
            ctx,
            *descriptorPool,
            ResourcePack{
                *res.graphicsUniformBuffer,
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
            },
            ResourcePack{*gBufferTextures.depth, vk::ShaderStageFlagBits::eFragment},
            ResourcePack{*gBufferTextures.normal, vk::ShaderStageFlagBits::eFragment},
            ResourcePack{*gBufferTextures.pos, vk::ShaderStageFlagBits::eFragment},
            ResourcePack{*ssaoNoiseTexture, vk::ShaderStageFlagBits::eFragment}
        );
    }
}

void VulkanRenderer::createCubemapCaptureDescriptorSet() {
    cubemapCaptureDescriptorSet = make_unique<CubemapCaptureDescriptorSet>(
        ctx,
        *descriptorPool,
        ResourcePack{
            *frameResources[0].graphicsUniformBuffer,
            vk::ShaderStageFlagBits::eVertex,
        },
        envmapTexture
            ? ResourcePack{*envmapTexture, vk::ShaderStageFlagBits::eFragment}
            : ResourcePack<Texture>{1, vk::ShaderStageFlagBits::eFragment}
    );
}

void VulkanRenderer::createDebugQuadDescriptorSet() {
    debugQuadDescriptorSet = make_unique<DebugQuadDescriptorSet>(
        ctx,
        *descriptorPool,
        ResourcePack<Texture>{*rtTargetTexture, vk::ShaderStageFlagBits::eFragment}
    );
}

void VulkanRenderer::createRtDescriptorSets() {
    for (auto &res: frameResources) {
        res.rtDescriptorSet = make_unique<RtDescriptorSet>(
            ctx,
            *descriptorPool,
            ResourcePack{
                *res.graphicsUniformBuffer,
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eRaygenKHR,
            },
            ResourcePack{
                *tlas, vk::ShaderStageFlagBits::eRaygenKHR
            },
            ResourcePack{
                *rtTargetTexture,
                vk::ShaderStageFlagBits::eRaygenKHR,
                vk::DescriptorType::eStorageImage
            }
        );
    }
}

void VulkanRenderer::createMeshesDescriptorSet() {
    meshesDescriptorSet = make_unique<MeshesDescriptorSet>(
        ctx,
        *descriptorPool,
        ResourcePack{
            model->getMeshDescriptionsBuffer(),
            vk::ShaderStageFlagBits::eClosestHitKHR,
            vk::DescriptorType::eStorageBuffer
        },
        ResourcePack{
            model->getVertexBuffer(),
            vk::ShaderStageFlagBits::eClosestHitKHR,
            vk::DescriptorType::eStorageBuffer
        },
        ResourcePack{
            model->getIndexBuffer(),
            vk::ShaderStageFlagBits::eClosestHitKHR,
            vk::DescriptorType::eStorageBuffer
        }
    );
}

// ==================== render infos ====================

RenderInfo::RenderInfo(GraphicsPipelineBuilder builder, shared_ptr<GraphicsPipeline> pipeline,
                       std::vector<RenderTarget> colors)
    : cachedPipelineBuilder(std::move(builder)), pipeline(std::move(pipeline)), colorTargets(std::move(colors)) {
    makeAttachmentInfos();
}

RenderInfo::RenderInfo(GraphicsPipelineBuilder builder, shared_ptr<GraphicsPipeline> pipeline,
                       std::vector<RenderTarget> colors, RenderTarget depth)
    : cachedPipelineBuilder(std::move(builder)), pipeline(std::move(pipeline)),
      colorTargets(std::move(colors)), depthTarget(std::move(depth)) {
    makeAttachmentInfos();
}

RenderInfo::RenderInfo(std::vector<RenderTarget> colors) : colorTargets(std::move(colors)) {
    makeAttachmentInfos();
}

RenderInfo::RenderInfo(std::vector<RenderTarget> colors, RenderTarget depth)
    : colorTargets(std::move(colors)), depthTarget(std::move(depth)) {
    makeAttachmentInfos();
}

vk::RenderingInfo RenderInfo::get(const vk::Extent2D extent, const uint32_t views,
                                  const vk::RenderingFlags flags) const {
    return {
        .flags = flags,
        .renderArea = {
            .offset = {0, 0},
            .extent = extent
        },
        .layerCount = views == 1 ? 1u : 0u,
        .viewMask = views == 1 ? 0 : (1u << views) - 1,
        .colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size()),
        .pColorAttachments = colorAttachments.data(),
        .pDepthAttachment = depthAttachment ? &depthAttachment.value() : nullptr
    };
}

vk::CommandBufferInheritanceRenderingInfo RenderInfo::getInheritanceRenderingInfo() const {
    return vk::CommandBufferInheritanceRenderingInfo {
        .colorAttachmentCount = static_cast<uint32_t>(cachedColorAttachmentFormats.size()),
        .pColorAttachmentFormats = cachedColorAttachmentFormats.data(),
        .depthAttachmentFormat = depthTarget ? depthTarget->getFormat() : static_cast<vk::Format>(0),
        .rasterizationSamples = pipeline->getSampleCount(),
    };
}

void RenderInfo::reloadShaders(const RendererContext &ctx) const {
    *pipeline = cachedPipelineBuilder.create(ctx);
}

void RenderInfo::makeAttachmentInfos() {
    for (const auto &target: colorTargets) {
        colorAttachments.emplace_back(target.getAttachmentInfo());
        cachedColorAttachmentFormats.push_back(target.getFormat());
    }

    if (depthTarget) {
        depthAttachment = depthTarget->getAttachmentInfo();
    }
}

void VulkanRenderer::createSceneRenderInfos() {
    sceneRenderInfos.clear();

    auto builder = GraphicsPipelineBuilder()
            .withVertexShader("../shaders/obj/main-vert.spv")
            .withFragmentShader("../shaders/obj/main-frag.spv")
            .withVertices<ModelVertex>()
            .withRasterizer({
                .polygonMode = wireframeMode ? vk::PolygonMode::eLine : vk::PolygonMode::eFill,
                .cullMode = cullBackFaces ? vk::CullModeFlagBits::eBack : vk::CullModeFlagBits::eNone,
                .frontFace = vk::FrontFace::eCounterClockwise,
                .lineWidth = 1.0f,
            })
            .withMultisampling({
                .rasterizationSamples = getMsaaSampleCount(),
                .minSampleShading = 1.0f,
            })
            .withDescriptorLayouts({
                *frameResources[0].sceneDescriptorSet->getLayout(),
                *materialsDescriptorSet->getLayout(),
            })
            .withPushConstants({
                vk::PushConstantRange{
                    .stageFlags = vk::ShaderStageFlagBits::eFragment,
                    .offset = 0,
                    .size = sizeof(ScenePushConstants),
                }
            })
            .withColorFormats({swapChain->getImageFormat()})
            .withDepthFormat(swapChain->getDepthFormat());

    auto pipeline = make_shared<GraphicsPipeline>(builder.create(ctx));

    for (auto &target: swapChain->getRenderTargets(ctx)) {
        std::vector<RenderTarget> colorTargets;
        colorTargets.emplace_back(std::move(target.colorTarget));

        target.depthTarget.overrideAttachmentConfig(vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare);

        sceneRenderInfos.emplace_back(
            builder,
            pipeline,
            std::move(colorTargets),
            std::move(target.depthTarget)
        );
    }
}

void VulkanRenderer::createSkyboxRenderInfos() {
    skyboxRenderInfos.clear();

    auto builder = GraphicsPipelineBuilder()
            .withVertexShader("../shaders/obj/skybox-vert.spv")
            .withFragmentShader("../shaders/obj/skybox-frag.spv")
            .withVertices<SkyboxVertex>()
            .withRasterizer({
                .polygonMode = vk::PolygonMode::eFill,
                .cullMode = vk::CullModeFlagBits::eNone,
                .frontFace = vk::FrontFace::eCounterClockwise,
                .lineWidth = 1.0f,
            })
            .withMultisampling({
                .rasterizationSamples = getMsaaSampleCount(),
                .minSampleShading = 1.0f,
            })
            .withDepthStencil({
                .depthTestEnable = vk::False,
                .depthWriteEnable = vk::False,
            })
            .withDescriptorLayouts({
                *frameResources[0].skyboxDescriptorSet->getLayout(),
            })
            .withColorFormats({swapChain->getImageFormat()})
            .withDepthFormat(swapChain->getDepthFormat());

    auto pipeline = make_shared<GraphicsPipeline>(builder.create(ctx));

    for (auto &target: swapChain->getRenderTargets(ctx)) {
        std::vector<RenderTarget> colorTargets;
        colorTargets.emplace_back(std::move(target.colorTarget));

        skyboxRenderInfos.emplace_back(
            builder,
            pipeline,
            std::move(colorTargets),
            std::move(target.depthTarget)
        );
    }
}

void VulkanRenderer::createGuiRenderInfos() {
    guiRenderInfos.clear();

    for (auto &target: swapChain->getRenderTargets(ctx)) {
        target.colorTarget.overrideAttachmentConfig(vk::AttachmentLoadOp::eLoad);

        std::vector<RenderTarget> colorTargets;
        colorTargets.emplace_back(std::move(target.colorTarget));

        guiRenderInfos.emplace_back(std::move(colorTargets));
    }
}

void VulkanRenderer::createPrepassRenderInfo() {
    std::vector<RenderTarget> colorTargets;
    colorTargets.emplace_back(ctx, *gBufferTextures.normal);
    colorTargets.emplace_back(ctx, *gBufferTextures.pos);

    RenderTarget depthTarget{ctx, *gBufferTextures.depth};

    std::vector<vk::Format> colorFormats;
    for (const auto &target: colorTargets) colorFormats.emplace_back(target.getFormat());

    auto builder = GraphicsPipelineBuilder()
            .withVertexShader("../shaders/obj/prepass-vert.spv")
            .withFragmentShader("../shaders/obj/prepass-frag.spv")
            .withVertices<ModelVertex>()
            .withRasterizer({
                .polygonMode = vk::PolygonMode::eFill,
                .cullMode = vk::CullModeFlagBits::eNone,
                .frontFace = vk::FrontFace::eCounterClockwise,
                .lineWidth = 1.0f,
            })
            .withDescriptorLayouts({
                *frameResources[0].prepassDescriptorSet->getLayout(),
            })
            .withColorFormats(colorFormats)
            .withDepthFormat(depthTarget.getFormat());

    auto pipeline = make_shared<GraphicsPipeline>(builder.create(ctx));

    prepassRenderInfo = make_unique<RenderInfo>(
        builder,
        pipeline,
        std::move(colorTargets),
        std::move(depthTarget)
    );
}

void VulkanRenderer::createSsaoRenderInfo() {
    RenderTarget target{ctx, *ssaoTexture};

    auto builder = GraphicsPipelineBuilder()
            .withVertexShader("../shaders/obj/ssao-vert.spv")
            .withFragmentShader("../shaders/obj/ssao-frag.spv")
            .withVertices<ScreenSpaceQuadVertex>()
            .withRasterizer({
                .polygonMode = vk::PolygonMode::eFill,
                .cullMode = vk::CullModeFlagBits::eNone,
                .frontFace = vk::FrontFace::eCounterClockwise,
                .lineWidth = 1.0f,
            })
            .withDescriptorLayouts({
                *frameResources[0].ssaoDescriptorSet->getLayout(),
            })
            .withColorFormats({target.getFormat()});

    auto pipeline = make_shared<GraphicsPipeline>(builder.create(ctx));

    std::vector<RenderTarget> targets;
    targets.emplace_back(std::move(target));

    ssaoRenderInfo = make_unique<RenderInfo>(
        builder,
        pipeline,
        std::move(targets)
    );
}

void VulkanRenderer::createCubemapCaptureRenderInfo() {
    RenderTarget target{
        skyboxTexture->getImage().getMipView(ctx, 0),
        skyboxTexture->getFormat()
    };

    auto builder = GraphicsPipelineBuilder()
            .withVertexShader("../shaders/obj/sphere-cube-vert.spv")
            .withFragmentShader("../shaders/obj/sphere-cube-frag.spv")
            .withVertices<SkyboxVertex>()
            .withRasterizer({
                .polygonMode = vk::PolygonMode::eFill,
                .cullMode = vk::CullModeFlagBits::eNone,
                .frontFace = vk::FrontFace::eCounterClockwise,
                .lineWidth = 1.0f,
            })
            .withDepthStencil({
                .depthTestEnable = vk::False,
                .depthWriteEnable = vk::False,
            })
            .withDescriptorLayouts({
                *cubemapCaptureDescriptorSet->getLayout(),
            })
            .forViews(6)
            .withColorFormats({target.getFormat()});

    auto pipeline = make_shared<GraphicsPipeline>(builder.create(ctx));

    std::vector<RenderTarget> targets;
    targets.emplace_back(std::move(target));

    cubemapCaptureRenderInfo = make_unique<RenderInfo>(
        builder,
        pipeline,
        std::move(targets)
    );
}

void VulkanRenderer::createDebugQuadRenderInfos() {
    debugQuadRenderInfos.clear();

    auto builder = GraphicsPipelineBuilder()
            .withVertexShader("../shaders/obj/ss-quad-vert.spv")
            .withFragmentShader("../shaders/obj/ss-quad-frag.spv")
            .withVertices<ScreenSpaceQuadVertex>()
            .withRasterizer({
                .polygonMode = vk::PolygonMode::eFill,
                .cullMode = vk::CullModeFlagBits::eNone,
                .frontFace = vk::FrontFace::eCounterClockwise,
                .lineWidth = 1.0f,
            })
            .withMultisampling({
                .rasterizationSamples = getMsaaSampleCount(),
                .minSampleShading = 1.0f,
            })
            .withDepthStencil({
                .depthTestEnable = vk::False,
                .depthWriteEnable = vk::False,
            })
            .withDescriptorLayouts({
                *debugQuadDescriptorSet->getLayout(),
            })
            .withColorFormats({swapChain->getImageFormat()})
            .withDepthFormat(swapChain->getDepthFormat());

    auto pipeline = make_shared<GraphicsPipeline>(builder.create(ctx));

    for (auto &target: swapChain->getRenderTargets(ctx)) {
        std::vector<RenderTarget> colorTargets;
        colorTargets.emplace_back(std::move(target.colorTarget));

        debugQuadRenderInfos.emplace_back(
            builder,
            pipeline,
            std::move(colorTargets),
            std::move(target.depthTarget)
        );
    }
}

// ==================== pipelines ====================

void VulkanRenderer::reloadShaders() const {
    waitIdle();

    sceneRenderInfos[0].reloadShaders(ctx);
    skyboxRenderInfos[0].reloadShaders(ctx);
    prepassRenderInfo->reloadShaders(ctx);
    ssaoRenderInfo->reloadShaders(ctx);
    cubemapCaptureRenderInfo->reloadShaders(ctx);
    debugQuadRenderInfos[0].reloadShaders(ctx);
}

// ==================== multisampling ====================

vk::SampleCountFlagBits VulkanRenderer::getMaxUsableSampleCount() const {
    const vk::PhysicalDeviceProperties physicalDeviceProperties = ctx.physicalDevice->getProperties();

    const vk::SampleCountFlags counts = physicalDeviceProperties.limits.framebufferColorSampleCounts
                                        & physicalDeviceProperties.limits.framebufferDepthSampleCounts;

    if (counts & vk::SampleCountFlagBits::e64) { return vk::SampleCountFlagBits::e64; }
    if (counts & vk::SampleCountFlagBits::e32) { return vk::SampleCountFlagBits::e32; }
    if (counts & vk::SampleCountFlagBits::e16) { return vk::SampleCountFlagBits::e16; }
    if (counts & vk::SampleCountFlagBits::e8) { return vk::SampleCountFlagBits::e8; }
    if (counts & vk::SampleCountFlagBits::e4) { return vk::SampleCountFlagBits::e4; }
    if (counts & vk::SampleCountFlagBits::e2) { return vk::SampleCountFlagBits::e2; }

    return vk::SampleCountFlagBits::e1;
}

// ==================== buffers ====================

void VulkanRenderer::createSkyboxVertexBuffer() {
    skyboxVertexBuffer = createLocalBuffer<SkyboxVertex>(skyboxVertices, vk::BufferUsageFlagBits::eVertexBuffer);
}

void VulkanRenderer::createScreenSpaceQuadVertexBuffer() {
    screenSpaceQuadVertexBuffer = createLocalBuffer<ScreenSpaceQuadVertex>(
        screenSpaceQuadVertices,
        vk::BufferUsageFlagBits::eVertexBuffer
    );
}

template<typename ElemType>
unique_ptr<Buffer>
VulkanRenderer::createLocalBuffer(const std::vector<ElemType> &contents, const vk::BufferUsageFlags usage) {
    const vk::DeviceSize bufferSize = sizeof(contents[0]) * contents.size();

    Buffer stagingBuffer{
        **ctx.allocator,
        bufferSize,
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
    };

    void *data = stagingBuffer.map();
    memcpy(data, contents.data(), static_cast<size_t>(bufferSize));
    stagingBuffer.unmap();

    auto resultBuffer = make_unique<Buffer>(
        **ctx.allocator,
        bufferSize,
        vk::BufferUsageFlagBits::eTransferDst | usage,
        vk::MemoryPropertyFlagBits::eDeviceLocal
    );

    resultBuffer->copyFromBuffer(ctx, stagingBuffer, bufferSize);

    return resultBuffer;
}

void VulkanRenderer::createUniformBuffers() {
    for (auto &res: frameResources) {
        res.graphicsUniformBuffer = make_unique<Buffer>(
            **ctx.allocator,
            sizeof(GraphicsUBO),
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
        );

        res.graphicsUboMapped = res.graphicsUniformBuffer->map();
    }
}

// ==================== commands ====================

void VulkanRenderer::createCommandPool() {
    const vk::CommandPoolCreateInfo poolInfo{
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = queueFamilyIndices.graphicsComputeFamily.value()
    };

    ctx.commandPool = make_unique<vk::raii::CommandPool>(*ctx.device, poolInfo);
}

void VulkanRenderer::createCommandBuffers() {
    const vk::CommandBufferAllocateInfo primaryAllocInfo{
        .commandPool = **ctx.commandPool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = static_cast<uint32_t>(frameResources.size()),
    };

    const vk::CommandBufferAllocateInfo secondaryAllocInfo{
        .commandPool = **ctx.commandPool,
        .level = vk::CommandBufferLevel::eSecondary,
        .commandBufferCount = static_cast<uint32_t>(frameResources.size()),
    };

    vk::raii::CommandBuffers graphicsCommandBuffers{*ctx.device, primaryAllocInfo};

    vk::raii::CommandBuffers sceneCommandBuffers{*ctx.device, secondaryAllocInfo};
    vk::raii::CommandBuffers rtCommandBuffers{*ctx.device, secondaryAllocInfo};
    vk::raii::CommandBuffers guiCommandBuffers{*ctx.device, secondaryAllocInfo};
    vk::raii::CommandBuffers prepassCommandBuffers{*ctx.device, secondaryAllocInfo};
    vk::raii::CommandBuffers debugCommandBuffers{*ctx.device, secondaryAllocInfo};
    vk::raii::CommandBuffers ssaoCommandBuffers{*ctx.device, secondaryAllocInfo};

    for (size_t i = 0; i < graphicsCommandBuffers.size(); i++) {
        frameResources[i].graphicsCmdBuffer =
                make_unique<vk::raii::CommandBuffer>(std::move(graphicsCommandBuffers[i]));
        frameResources[i].rtCmdBuffer =
                {make_unique<vk::raii::CommandBuffer>(std::move(rtCommandBuffers[i]))};
        frameResources[i].sceneCmdBuffer =
                {make_unique<vk::raii::CommandBuffer>(std::move(sceneCommandBuffers[i]))};
        frameResources[i].guiCmdBuffer =
                {make_unique<vk::raii::CommandBuffer>(std::move(guiCommandBuffers[i]))};
        frameResources[i].prepassCmdBuffer =
                {make_unique<vk::raii::CommandBuffer>(std::move(prepassCommandBuffers[i]))};
        frameResources[i].debugCmdBuffer =
                {make_unique<vk::raii::CommandBuffer>(std::move(debugCommandBuffers[i]))};
        frameResources[i].ssaoCmdBuffer =
                {make_unique<vk::raii::CommandBuffer>(std::move(ssaoCommandBuffers[i]))};
    }
}

void VulkanRenderer::recordGraphicsCommandBuffer() {
    const auto &commandBuffer = *frameResources[currentFrameIdx].graphicsCmdBuffer;

    const vk::ImageMemoryBarrier2 barrierTo{
        .srcStageMask = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = **rtTargetTexture->getImage(),
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        }
    };

    const vk::ImageMemoryBarrier2 barrierFrom{
        .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = **rtTargetTexture->getImage(),
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        }
    };

    constexpr auto renderingFlags = vk::RenderingFlagBits::eContentsSecondaryCommandBuffers;

    constexpr vk::CommandBufferBeginInfo beginInfo;
    commandBuffer.begin(beginInfo);

    swapChain->transitionToAttachmentLayout(commandBuffer);

    // prepass

    if (frameResources[currentFrameIdx].prepassCmdBuffer.wasRecordedThisFrame) {
        commandBuffer.beginRendering(prepassRenderInfo->get(swapChain->getExtent(), 1, renderingFlags));
        commandBuffer.executeCommands(**frameResources[currentFrameIdx].prepassCmdBuffer);
        commandBuffer.endRendering();
    }

    // ssao pass

    if (frameResources[currentFrameIdx].ssaoCmdBuffer.wasRecordedThisFrame) {
        commandBuffer.beginRendering(ssaoRenderInfo->get(swapChain->getExtent(), 1, renderingFlags));
        commandBuffer.executeCommands(**frameResources[currentFrameIdx].ssaoCmdBuffer);
        commandBuffer.endRendering();
    }

    // rt pass

    if (frameResources[currentFrameIdx].rtCmdBuffer.wasRecordedThisFrame) {
        commandBuffer.executeCommands(**frameResources[currentFrameIdx].rtCmdBuffer);
    }

    // main pass

    if (frameResources[currentFrameIdx].sceneCmdBuffer.wasRecordedThisFrame) {
        const auto &renderInfo = sceneRenderInfos[swapChain->getCurrentImageIndex()];
        commandBuffer.beginRendering(renderInfo.get(swapChain->getExtent(), 1, renderingFlags));
        commandBuffer.executeCommands(**frameResources[currentFrameIdx].sceneCmdBuffer);
        commandBuffer.endRendering();
    }

    // debug quad pass

    if (frameResources[currentFrameIdx].debugCmdBuffer.wasRecordedThisFrame) {
        commandBuffer.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1u,
            .pImageMemoryBarriers = &barrierTo
        });

        const auto &renderInfo = sceneRenderInfos[swapChain->getCurrentImageIndex()];
        commandBuffer.beginRendering(renderInfo.get(swapChain->getExtent(), 1, renderingFlags));
        commandBuffer.executeCommands(**frameResources[currentFrameIdx].debugCmdBuffer);
        commandBuffer.endRendering();

        commandBuffer.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1u,
            .pImageMemoryBarriers = &barrierFrom
        });
    }

    // gui pass

    if (frameResources[currentFrameIdx].guiCmdBuffer.wasRecordedThisFrame) {
        const auto &renderInfo = guiRenderInfos[swapChain->getCurrentImageIndex()];
        commandBuffer.beginRendering(renderInfo.get(swapChain->getExtent(), 1, renderingFlags));
        commandBuffer.executeCommands(**frameResources[currentFrameIdx].guiCmdBuffer);
        commandBuffer.endRendering();
    }

    swapChain->transitionToPresentLayout(commandBuffer);

    commandBuffer.end();
}

// ==================== sync ====================

void VulkanRenderer::createSyncObjects() {
    const vk::StructureChain<vk::SemaphoreCreateInfo, vk::SemaphoreTypeCreateInfo> timelineSemaphoreInfo{
        {},
        {
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        }
    };

    constexpr vk::SemaphoreCreateInfo binarySemaphoreInfo;

    for (auto &res: frameResources) {
        res.sync = {
            .imageAvailableSemaphore = make_unique<vk::raii::Semaphore>(*ctx.device, binarySemaphoreInfo),
            .readyToPresentSemaphore = make_unique<vk::raii::Semaphore>(*ctx.device, binarySemaphoreInfo),
            .renderFinishedTimeline = {
                make_unique<vk::raii::Semaphore>(*ctx.device, timelineSemaphoreInfo.get<vk::SemaphoreCreateInfo>())
            },
        };
    }
}

// ==================== ray tracing ====================

void VulkanRenderer::createTLAS() {
    const uint32_t instanceCount = 1; // todo
    std::vector<vk::AccelerationStructureInstanceKHR> instances;

    decltype(vk::TransformMatrixKHR::matrix) matrix;
    matrix[0] = {{1.0f, 0.0f, 0.0f, 0.0f}};
    matrix[1] = {{0.0f, 1.0f, 0.0f, 0.0f}};
    matrix[2] = {{0.0f, 0.0f, 1.0f, 0.0f}};
    const vk::TransformMatrixKHR transformMatrix{matrix};

    const vk::AccelerationStructureDeviceAddressInfoKHR blasAddressInfo{.accelerationStructure = *model->getBLAS()};
    const vk::DeviceAddress blasReference = ctx.device->getAccelerationStructureAddressKHR(blasAddressInfo);
    constexpr auto flags                  = vk::GeometryInstanceFlagBitsKHR::eTriangleCullDisable; // todo

    instances.emplace_back(vk::AccelerationStructureInstanceKHR{
        .transform = transformMatrix,
        .instanceCustomIndex = 0, // todo
        .mask = 0xFFu,
        .instanceShaderBindingTableRecordOffset = 0,
        .flags = static_cast<VkGeometryInstanceFlagsKHR>(flags),
        .accelerationStructureReference = blasReference,
    });

    const size_t instancesBufferSize = instances.size() * sizeof(vk::AccelerationStructureInstanceKHR);

    Buffer instancesBuffer{
        **ctx.allocator,
        instancesBufferSize,
        vk::BufferUsageFlagBits::eShaderDeviceAddress
        | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR
        | vk::BufferUsageFlagBits::eTransferSrc
        | vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostCoherent
        | vk::MemoryPropertyFlagBits::eHostVisible
    };

    void *instancesBufferMapped = instancesBuffer.map();
    memcpy(instancesBufferMapped, instances.data(), instancesBufferSize);
    instancesBuffer.unmap();

    const vk::AccelerationStructureGeometryInstancesDataKHR geometryInstancesData{
        .data = ctx.device->getBufferAddress({.buffer = *instancesBuffer}),
    };

    const vk::AccelerationStructureGeometryKHR geometry{
        .geometryType = vk::GeometryTypeKHR::eInstances,
        .geometry = geometryInstancesData,
    };

    vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1u,
        .pGeometries = &geometry,
    };

    const vk::AccelerationStructureBuildSizesInfoKHR sizeInfo = ctx.device->getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice,
        buildInfo,
        instanceCount
    );

    const vk::DeviceSize tlasSize = sizeInfo.accelerationStructureSize;

    auto tlasBuffer = make_unique<Buffer>(
        **ctx.allocator,
        tlasSize,
        vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR,
        vk::MemoryPropertyFlagBits::eDeviceLocal
    );

    const vk::AccelerationStructureCreateInfoKHR createInfo{
        .buffer = **tlasBuffer,
        .size = tlasSize,
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
    };

    tlas = make_unique<AccelerationStructure>(
        make_unique<vk::raii::AccelerationStructureKHR>(*ctx.device, createInfo),
        std::move(tlasBuffer)
    );

    const Buffer scratchBuffer{
        **ctx.allocator,
        sizeInfo.buildScratchSize,
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
        vk::MemoryPropertyFlagBits::eDeviceLocal
    };

    buildInfo.srcAccelerationStructure = nullptr;
    buildInfo.dstAccelerationStructure = ***tlas;
    buildInfo.scratchData              = ctx.device->getBufferAddress({.buffer = *scratchBuffer});

    static constexpr vk::AccelerationStructureBuildRangeInfoKHR rangeInfo{
        .primitiveCount = instanceCount,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0,
    };

    static constexpr vk::MemoryBarrier2 memoryBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR
    };

    utils::cmd::doSingleTimeCommands(ctx, [&](const vk::raii::CommandBuffer &commandBuffer) {
        commandBuffer.pipelineBarrier2({
            .memoryBarrierCount = 1u,
            .pMemoryBarriers = &memoryBarrier,
        });

        commandBuffer.buildAccelerationStructuresKHR(buildInfo, &rangeInfo);
    });
}

void VulkanRenderer::createRtPipeline() {
    const auto builder = RtPipelineBuilder()
            .withRayGenShader("../shaders/obj/raytrace-rgen.spv")
            .withMissShader("../shaders/obj/raytrace-rmiss.spv")
            .withClosestHitShader("../shaders/obj/raytrace-rchit.spv")
            .withDescriptorLayouts({
                *frameResources[0].rtDescriptorSet->getLayout(),
                *materialsDescriptorSet->getLayout(),
                *meshesDescriptorSet->getLayout(),
            });

    rtPipeline = make_unique<RtPipeline>(builder.create(ctx));
}

// ==================== gui ====================

void VulkanRenderer::initImgui() {
    const std::vector<vk::DescriptorPoolSize> poolSizes = {
        {vk::DescriptorType::eSampler, 1000},
        {vk::DescriptorType::eCombinedImageSampler, 1000},
        {vk::DescriptorType::eSampledImage, 1000},
        {vk::DescriptorType::eStorageImage, 1000},
        {vk::DescriptorType::eUniformTexelBuffer, 1000},
        {vk::DescriptorType::eStorageTexelBuffer, 1000},
        {vk::DescriptorType::eUniformBuffer, 1000},
        {vk::DescriptorType::eStorageBuffer, 1000},
        {vk::DescriptorType::eUniformBufferDynamic, 1000},
        {vk::DescriptorType::eStorageBufferDynamic, 1000},
        {vk::DescriptorType::eInputAttachment, 1000}
    };

    const vk::DescriptorPoolCreateInfo poolInfo = {
        .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        .maxSets = 1000,
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data(),
    };

    imguiDescriptorPool = make_unique<vk::raii::DescriptorPool>(*ctx.device, poolInfo);

    const uint32_t imageCount = SwapChain::getImageCount(ctx, *surface);

    ImGui_ImplVulkan_InitInfo imguiInitInfo = {
        .Instance = **instance,
        .PhysicalDevice = **ctx.physicalDevice,
        .Device = **ctx.device,
        .Queue = **ctx.graphicsQueue,
        .DescriptorPool = static_cast<VkDescriptorPool>(**imguiDescriptorPool),
        .MinImageCount = imageCount,
        .ImageCount = imageCount,
        .MSAASamples = static_cast<VkSampleCountFlagBits>(getMsaaSampleCount()),
        .UseDynamicRendering = true,
        .ColorAttachmentFormat = static_cast<VkFormat>(swapChain->getImageFormat()),
    };

    guiRenderer = make_unique<GuiRenderer>(window, imguiInitInfo);
}

void VulkanRenderer::renderGuiSection() {
    constexpr auto sectionFlags = ImGuiTreeNodeFlags_DefaultOpen;

    if (ImGui::CollapsingHeader("Model ", sectionFlags)) {
        if (ImGui::Button("Load model...")) {
            ImGui::OpenPopup("Load model");
        }

        ImGui::Separator();

        ImGui::DragFloat("Model scale", &modelScale, 0.01, 0, std::numeric_limits<float>::max());

        ImGui::gizmo3D("Model rotation", modelRotation, 160);

        if (ImGui::Button("Reset scale")) { modelScale = 1; }
        ImGui::SameLine();
        if (ImGui::Button("Reset rotation")) { modelRotation = {1, 0, 0, 0}; }
        ImGui::SameLine();
        if (ImGui::Button("Reset position")) { modelTranslate = {0, 0, 0}; }
    }

    if (ImGui::CollapsingHeader("Advanced ", sectionFlags)) {
        // todo - convert these 2 to dynamic states
        if (ImGui::Checkbox("Cull backfaces", &cullBackFaces)) {
            queuedFrameBeginActions.emplace([&] {
                waitIdle();
                sceneRenderInfos[0].reloadShaders(ctx);
            });
        }

        if (ImGui::Checkbox("Wireframe mode", &wireframeMode)) {
            queuedFrameBeginActions.emplace([&] {
                waitIdle();
                sceneRenderInfos[0].reloadShaders(ctx);
            });
        }

        ImGui::Checkbox("SSAO", &useSsao);

        static bool useMsaaDummy = useMsaa;
        if (ImGui::Checkbox("MSAA", &useMsaaDummy)) {
            queuedFrameBeginActions.emplace([this] {
                useMsaa = useMsaaDummy;

                waitIdle();
                recreateSwapChain();

                createSceneRenderInfos();
                createSkyboxRenderInfos();
                createDebugQuadRenderInfos();

                guiRenderer.reset();
                initImgui();
            });
        }

#ifndef NDEBUG
        ImGui::Separator();
        ImGui::DragFloat("Debug number", &debugNumber, 0.01, 0, std::numeric_limits<float>::max());
#endif
    }

    if (ImGui::CollapsingHeader("Lighting ", sectionFlags)) {
        ImGui::SliderFloat("Light intensity", &lightIntensity, 0.0f, 100.0f, "%.2f");
        ImGui::ColorEdit3("Light color", &lightColor.x);
        ImGui::gizmo3D("Light direction", lightDirection, 160, imguiGizmo::modeDirection);
    }

    camera->renderGuiSection();
}

// ==================== render graph ====================

void VulkanRenderer::registerRenderGraph(const RenderGraph &graph) {
    renderGraphInfo.renderGraph = make_unique<RenderGraph>(graph);
    bindlessParamSet = make_unique<BindlessParamSet>(ctx);

    const auto topoSortedHandles = renderGraphInfo.renderGraph->getTopoSorted();
    const uint32_t nNodes        = topoSortedHandles.size();

    const vk::CommandBufferAllocateInfo secondaryAllocInfo{
        .commandPool = **ctx.commandPool,
        .level = vk::CommandBufferLevel::eSecondary,
        .commandBufferCount = nNodes,
    };

    vk::raii::CommandBuffers commandBuffers{*ctx.device, secondaryAllocInfo};

    std::vector<uint32_t> vertexRangeOffsets;
    std::vector<uint32_t> fragmentRangeOffsets;

    for (uint32_t i = 0; i < nNodes; i++) {
        const auto handle = topoSortedHandles[i];
        const auto& nodeInfo = renderGraphInfo.renderGraph->getNodeInfo(handle);

        vertexRangeOffsets.emplace_back(bindlessParamSet->addRange(nodeInfo.vertexShader->bindings));
        fragmentRangeOffsets.emplace_back(bindlessParamSet->addRange(nodeInfo.fragmentShader->bindings));
    }

    bindlessParamSet->build(*descriptorPool);

    for (uint32_t i = 0; i < nNodes; i++) {
        const auto handle = topoSortedHandles[i];

        renderGraphInfo.topoSortedNodes.emplace_back(RenderNodeResources{
            .handle = handle,
            .commandBuffer = std::move(commandBuffers[i]),
            .pipeline = createNodePipeline(handle),
            .vertexParamsOffset = vertexRangeOffsets[i],
            .fragmentParamsOffset = fragmentRangeOffsets[i],
        });
    }
}

GraphicsPipeline VulkanRenderer::createNodePipeline(const RenderNodeHandle handle) const {
    const auto& nodeInfo = renderGraphInfo.renderGraph->getNodeInfo(handle);

    std::vector<vk::Format> colorFormats;
    for (const auto &target: nodeInfo.colorTargets) {
        colorFormats.push_back(renderGraphInfo.renderGraph->getTransientTextureFormat(target));
    }

    auto builder = GraphicsPipelineBuilder()
            .withVertexShader(nodeInfo.vertexShader->path)
            .withFragmentShader(nodeInfo.fragmentShader->path)
            .withVertices<ModelVertex>()
            .withRasterizer({
                .polygonMode = vk::PolygonMode::eFill,
                .cullMode = nodeInfo.customConfig.cullMode,
                .frontFace = vk::FrontFace::eCounterClockwise,
                .lineWidth = 1.0f,
            })
            .withMultisampling({
                .rasterizationSamples = nodeInfo.customConfig.useMsaa
                                            ? getMsaaSampleCount()
                                            : vk::SampleCountFlagBits::e1,
                .minSampleShading = 1.0f,
            })
            .withDescriptorLayouts({
                *bindlessDescriptorSet->getLayout(),
                *bindlessParamSet->getDescriptorSet().getLayout(),
                *bindlessParamSet->getDescriptorSet().getLayout(),
            })
            .withColorFormats(colorFormats);

    if (nodeInfo.depthTarget) {
        builder.withDepthFormat(renderGraphInfo.renderGraph->getTransientTextureFormat(*nodeInfo.depthTarget));
    } else {
        builder.withDepthStencil({
            .depthTestEnable = vk::False,
            .depthWriteEnable = vk::False,
        });
    }

    return builder.create(ctx);
}

void VulkanRenderer::runRenderGraph() {
    const size_t nPasses = renderGraphInfo.topoSortedNodes.size();

    for (size_t i = 0; i < nPasses; i++) {
        const auto &nodeResources = renderGraphInfo.topoSortedNodes[i];
        recordRenderGraphNodeCommands(nodeResources);
    }
}

void VulkanRenderer::recordRenderGraphNodeCommands(const RenderNodeResources &nodeResources) {
    const auto &[handle, commandBuffer, pipeline, vertexParamsOffset, fragmentParamsOffset] = nodeResources;

    const auto &nodeInfo = renderGraphInfo.renderGraph->getNodeInfo(handle);

    std::vector<vk::Format> colorFormats;
    for (const auto &target: nodeInfo.colorTargets) {
        colorFormats.push_back(renderGraphInfo.renderGraph->getTransientTextureFormat(target));
    }

    const vk::Format depthFormat = nodeInfo.depthTarget
        ? renderGraphInfo.renderGraph->getTransientTextureFormat(*nodeInfo.depthTarget)
        : static_cast<vk::Format>(0);

    const vk::StructureChain inheritanceInfo{
        vk::CommandBufferInheritanceInfo{},
        vk::CommandBufferInheritanceRenderingInfo {
            .colorAttachmentCount = static_cast<uint32_t>(nodeInfo.colorTargets.size()),
            .pColorAttachmentFormats = colorFormats.data(),
            .depthAttachmentFormat = depthFormat,
            .rasterizationSamples = nodeInfo.customConfig.useMsaa ? getMsaaSampleCount() : vk::SampleCountFlagBits::e1,
        }
    };

    const vk::CommandBufferBeginInfo beginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eRenderPassContinue,
        .pInheritanceInfo = &inheritanceInfo.get<vk::CommandBufferInheritanceInfo>(),
    };

    commandBuffer.begin(beginInfo);

    utils::cmd::setDynamicStates(commandBuffer, swapChain->getExtent());

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, **pipeline);

    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        *pipeline.getLayout(),
        0,
        {
            ***bindlessDescriptorSet,
            **bindlessParamSet->getDescriptorSet(),
            **bindlessParamSet->getDescriptorSet(),
        },
        {
            vertexParamsOffset,
            fragmentParamsOffset
        }
    );

    RenderPassContext passCtx{commandBuffer};
    nodeInfo.body(passCtx);

    commandBuffer.end();
}

// ==================== render loop ====================

void VulkanRenderer::tick(const float deltaTime) {
    glfwPollEvents();
    camera->tick(deltaTime);

    if (
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)
        && !ImGui::IsAnyItemActive()
        && !ImGui::IsAnyItemFocused()
    ) {
        inputManager->tick(deltaTime);
    }
}

void VulkanRenderer::renderGui(const std::function<void()> &renderCommands) {
    const auto &commandBuffer = *frameResources[currentFrameIdx].guiCmdBuffer.buffer;

    const std::vector colorAttachmentFormats{swapChain->getImageFormat()};

    const vk::StructureChain<
        vk::CommandBufferInheritanceInfo,
        vk::CommandBufferInheritanceRenderingInfo
    > inheritanceInfo{
        {},
        {
            .colorAttachmentCount = static_cast<uint32_t>(colorAttachmentFormats.size()),
            .pColorAttachmentFormats = colorAttachmentFormats.data(),
            .rasterizationSamples = getMsaaSampleCount(),
        }
    };

    const vk::CommandBufferBeginInfo beginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eRenderPassContinue,
        .pInheritanceInfo = &inheritanceInfo.get<vk::CommandBufferInheritanceInfo>(),
    };

    commandBuffer.begin(beginInfo);

    guiRenderer->beginRendering();
    renderCommands();
    guiRenderer->endRendering(commandBuffer);

    commandBuffer.end();

    frameResources[currentFrameIdx].guiCmdBuffer.wasRecordedThisFrame = true;
}

bool VulkanRenderer::startFrame() {
    while (!queuedFrameBeginActions.empty()) {
        queuedFrameBeginActions.front()();
        queuedFrameBeginActions.pop();
    }

    const auto &sync = frameResources[currentFrameIdx].sync;

    const std::vector waitSemaphores = {
        **sync.renderFinishedTimeline.semaphore,
    };

    const std::vector waitSemaphoreValues = {
        sync.renderFinishedTimeline.timeline,
    };

    const vk::SemaphoreWaitInfo waitInfo{
        .semaphoreCount = static_cast<uint32_t>(waitSemaphores.size()),
        .pSemaphores = waitSemaphores.data(),
        .pValues = waitSemaphoreValues.data(),
    };

    if (ctx.device->waitSemaphores(waitInfo, UINT64_MAX) != vk::Result::eSuccess) {
        throw std::runtime_error("waitSemaphores on renderFinishedTimeline failed");
    }

    updateGraphicsUniformBuffer();

    const auto &[result, imageIndex] = swapChain->acquireNextImage(*sync.imageAvailableSemaphore);

    if (result == vk::Result::eErrorOutOfDateKHR) {
        recreateSwapChain();
        return false;
    }
    if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    frameResources[currentFrameIdx].sceneCmdBuffer.wasRecordedThisFrame   = false;
    frameResources[currentFrameIdx].prepassCmdBuffer.wasRecordedThisFrame = false;
    frameResources[currentFrameIdx].ssaoCmdBuffer.wasRecordedThisFrame    = false;
    frameResources[currentFrameIdx].guiCmdBuffer.wasRecordedThisFrame     = false;
    frameResources[currentFrameIdx].debugCmdBuffer.wasRecordedThisFrame   = false;

    return true;
}

void VulkanRenderer::endFrame() {
    recordGraphicsCommandBuffer();

    auto &sync = frameResources[currentFrameIdx].sync;

    const std::vector waitSemaphores = {
        **sync.imageAvailableSemaphore
    };

    const std::vector<TimelineSemValueType> waitSemaphoreValues = {
        0
    };

    static constexpr vk::PipelineStageFlags waitStages[] = {
        vk::PipelineStageFlagBits::eEarlyFragmentTests,
        vk::PipelineStageFlagBits::eVertexInput,
    };

    const std::array signalSemaphores = {
        **sync.renderFinishedTimeline.semaphore,
        **sync.readyToPresentSemaphore
    };

    sync.renderFinishedTimeline.timeline++;
    const std::vector<TimelineSemValueType> signalSemaphoreValues{
        sync.renderFinishedTimeline.timeline,
        0
    };

    const vk::StructureChain<vk::SubmitInfo, vk::TimelineSemaphoreSubmitInfo> submitInfo{
        {
            .waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size()),
            .pWaitSemaphores = waitSemaphores.data(),
            .pWaitDstStageMask = waitStages,
            .commandBufferCount = 1,
            .pCommandBuffers = &**frameResources[currentFrameIdx].graphicsCmdBuffer,
            .signalSemaphoreCount = signalSemaphores.size(),
            .pSignalSemaphores = signalSemaphores.data(),
        },
        {
            .waitSemaphoreValueCount = static_cast<uint32_t>(waitSemaphoreValues.size()),
            .pWaitSemaphoreValues = waitSemaphoreValues.data(),
            .signalSemaphoreValueCount = static_cast<uint32_t>(signalSemaphoreValues.size()),
            .pSignalSemaphoreValues = signalSemaphoreValues.data(),
        }
    };

    try {
        ctx.graphicsQueue->submit(submitInfo.get<vk::SubmitInfo>());
    } catch (std::exception &e) {
        std::cerr << e.what() << std::endl;
        throw;
    }

    const std::array presentWaitSemaphores = {**sync.readyToPresentSemaphore};

    const std::array imageIndices = {swapChain->getCurrentImageIndex()};

    const vk::PresentInfoKHR presentInfo{
        .waitSemaphoreCount = presentWaitSemaphores.size(),
        .pWaitSemaphores = presentWaitSemaphores.data(),
        .swapchainCount = 1U,
        .pSwapchains = &***swapChain,
        .pImageIndices = imageIndices.data(),
    };

    auto presentResult = vk::Result::eSuccess;

    try {
        presentResult = presentQueue->presentKHR(presentInfo);
    } catch (...) {
    }

    const bool didResize = presentResult == vk::Result::eErrorOutOfDateKHR
                           || presentResult == vk::Result::eSuboptimalKHR
                           || framebufferResized;
    if (didResize) {
        framebufferResized = false;
        recreateSwapChain();
    } else if (presentResult != vk::Result::eSuccess) {
        throw std::runtime_error("failed to present swap chain image!");
    }

    currentFrameIdx = (currentFrameIdx + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanRenderer::runPrepass() {
    if (!model) {
        return;
    }

    const auto &commandBuffer = *frameResources[currentFrameIdx].prepassCmdBuffer.buffer;

    const vk::StructureChain inheritanceInfo{
        vk::CommandBufferInheritanceInfo{},
        prepassRenderInfo->getInheritanceRenderingInfo()
    };

    const vk::CommandBufferBeginInfo beginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eRenderPassContinue,
        .pInheritanceInfo = &inheritanceInfo.get<vk::CommandBufferInheritanceInfo>(),
    };

    commandBuffer.begin(beginInfo);

    utils::cmd::setDynamicStates(commandBuffer, swapChain->getExtent());

    auto &pipeline = prepassRenderInfo->getPipeline();
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, **pipeline);

    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        *pipeline.getLayout(),
        0,
        ***frameResources[currentFrameIdx].prepassDescriptorSet,
        nullptr
    );

    drawModel(commandBuffer, false, pipeline);

    commandBuffer.end();

    frameResources[currentFrameIdx].prepassCmdBuffer.wasRecordedThisFrame = true;
}

void VulkanRenderer::runSsaoPass() {
    if (!model || !useSsao) {
        return;
    }

    const auto &commandBuffer = *frameResources[currentFrameIdx].ssaoCmdBuffer.buffer;

    const vk::StructureChain inheritanceInfo{
        vk::CommandBufferInheritanceInfo{},
        ssaoRenderInfo->getInheritanceRenderingInfo()
    };

    const vk::CommandBufferBeginInfo beginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eRenderPassContinue,
        .pInheritanceInfo = &inheritanceInfo.get<vk::CommandBufferInheritanceInfo>(),
    };

    commandBuffer.begin(beginInfo);

    utils::cmd::setDynamicStates(commandBuffer, swapChain->getExtent());

    auto &pipeline = ssaoRenderInfo->getPipeline();
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, **pipeline);

    commandBuffer.bindVertexBuffers(0, **screenSpaceQuadVertexBuffer, {0});

    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        *pipeline.getLayout(),
        0,
        ***frameResources[currentFrameIdx].ssaoDescriptorSet,
        nullptr
    );

    commandBuffer.draw(screenSpaceQuadVertices.size(), 1, 0, 0);

    commandBuffer.end();

    frameResources[currentFrameIdx].ssaoCmdBuffer.wasRecordedThisFrame = true;
}

void VulkanRenderer::raytrace() {
    const auto &commandBuffer = *frameResources[currentFrameIdx].rtCmdBuffer.buffer;

    static constexpr vk::CommandBufferInheritanceInfo inheritanceInfo{};

    static constexpr vk::CommandBufferBeginInfo beginInfo{
        .pInheritanceInfo = &inheritanceInfo,
    };

    commandBuffer.begin(beginInfo);

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, ***rtPipeline);

    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eRayTracingKHR,
        *rtPipeline->getLayout(),
        0,
        {
            ***frameResources[currentFrameIdx].rtDescriptorSet,
            ***materialsDescriptorSet,
            ***meshesDescriptorSet,
        },
        nullptr
    );

    const auto &sbt    = rtPipeline->getSbt();
    const auto &extent = rtTargetTexture->getImage().getExtent();

    commandBuffer.traceRaysKHR(
        sbt.rgenRegion,
        sbt.missRegion,
        sbt.hitRegion,
        sbt.callRegion,
        extent.width,
        extent.height,
        extent.depth
    );

    commandBuffer.end();

    frameResources[currentFrameIdx].rtCmdBuffer.wasRecordedThisFrame = true;
}

void VulkanRenderer::drawScene() {
    if (!model) {
        return;
    }

    const auto &commandBuffer = *frameResources[currentFrameIdx].sceneCmdBuffer.buffer;

    const vk::StructureChain inheritanceInfo{
        vk::CommandBufferInheritanceInfo{},
        sceneRenderInfos[0].getInheritanceRenderingInfo()
    };

    const vk::CommandBufferBeginInfo beginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eRenderPassContinue,
        .pInheritanceInfo = &inheritanceInfo.get<vk::CommandBufferInheritanceInfo>(),
    };

    commandBuffer.begin(beginInfo);

    utils::cmd::setDynamicStates(commandBuffer, swapChain->getExtent());

    // skybox

    const auto &skyboxPipeline = skyboxRenderInfos[swapChain->getCurrentImageIndex()].getPipeline();
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, **skyboxPipeline);

    commandBuffer.bindVertexBuffers(0, **skyboxVertexBuffer, {0});

    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        *skyboxPipeline.getLayout(),
        0,
        ***frameResources[currentFrameIdx].skyboxDescriptorSet,
        nullptr
    );

    commandBuffer.draw(static_cast<uint32_t>(skyboxVertices.size()), 1, 0, 0);

    // scene

    const auto &scenePipeline = sceneRenderInfos[swapChain->getCurrentImageIndex()].getPipeline();
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, **scenePipeline);

    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        *scenePipeline.getLayout(),
        0,
        {
            ***frameResources[currentFrameIdx].sceneDescriptorSet,
            ***materialsDescriptorSet,
        },
        nullptr
    );

    drawModel(commandBuffer, true, scenePipeline);

    commandBuffer.end();

    frameResources[currentFrameIdx].sceneCmdBuffer.wasRecordedThisFrame = true;
}

void VulkanRenderer::drawDebugQuad() {
    const auto &commandBuffer = *frameResources[currentFrameIdx].debugCmdBuffer.buffer;

    const vk::StructureChain inheritanceInfo{
        vk::CommandBufferInheritanceInfo{},
        debugQuadRenderInfos[0].getInheritanceRenderingInfo()
    };

    const vk::CommandBufferBeginInfo beginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eRenderPassContinue,
        .pInheritanceInfo = &inheritanceInfo.get<vk::CommandBufferInheritanceInfo>(),
    };

    commandBuffer.begin(beginInfo);

    utils::cmd::setDynamicStates(commandBuffer, swapChain->getExtent());

    auto &pipeline = debugQuadRenderInfos[swapChain->getCurrentImageIndex()].getPipeline();
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, **pipeline);

    commandBuffer.bindVertexBuffers(0, **screenSpaceQuadVertexBuffer, {0});

    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        *pipeline.getLayout(),
        0,
        ***debugQuadDescriptorSet,
        nullptr
    );

    commandBuffer.draw(screenSpaceQuadVertices.size(), 1, 0, 0);

    commandBuffer.end();

    frameResources[currentFrameIdx].debugCmdBuffer.wasRecordedThisFrame = true;
}

void VulkanRenderer::drawModel(const vk::raii::CommandBuffer &commandBuffer, const bool doPushConstants,
                               const GraphicsPipeline &pipeline) const {
    uint32_t indexOffset    = 0;
    int32_t vertexOffset    = 0;
    uint32_t instanceOffset = 0;

    model->bindBuffers(commandBuffer);

    for (const auto &mesh: model->getMeshes()) {
        // todo - make this a bit nicer (without the ugly bool)
        if (doPushConstants) {
            commandBuffer.pushConstants<ScenePushConstants>(
                *pipeline.getLayout(),
                vk::ShaderStageFlagBits::eFragment,
                0,
                ScenePushConstants{
                    .materialID = mesh.materialID
                }
            );
        }

        commandBuffer.drawIndexed(
            static_cast<uint32_t>(mesh.indices.size()),
            static_cast<uint32_t>(mesh.instances.size()),
            indexOffset,
            vertexOffset,
            instanceOffset
        );

        indexOffset += static_cast<uint32_t>(mesh.indices.size());
        vertexOffset += static_cast<int32_t>(mesh.vertices.size());
        instanceOffset += static_cast<uint32_t>(mesh.instances.size());
    }
}

void VulkanRenderer::captureCubemap() const {
    const vk::Extent2D extent = skyboxTexture->getImage().getExtent2d();

    const auto commandBuffer = utils::cmd::beginSingleTimeCommands(ctx);

    utils::cmd::setDynamicStates(commandBuffer, extent);

    commandBuffer.beginRendering(cubemapCaptureRenderInfo->get(extent, 6));

    commandBuffer.bindVertexBuffers(0, **skyboxVertexBuffer, {0});

    const auto &pipeline = cubemapCaptureRenderInfo->getPipeline();
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, **pipeline);

    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        *pipeline.getLayout(),
        0,
        ***cubemapCaptureDescriptorSet,
        nullptr
    );

    commandBuffer.draw(skyboxVertices.size(), 1, 0, 0);

    commandBuffer.endRendering();

    skyboxTexture->getImage().transitionLayout(
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageLayout::eTransferDstOptimal,
        commandBuffer
    );

    utils::cmd::endSingleTimeCommands(commandBuffer, *ctx.graphicsQueue);

    skyboxTexture->generateMipmaps(ctx, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderer::updateGraphicsUniformBuffer() const {
    const glm::mat4 model = glm::translate(modelTranslate)
                            * mat4_cast(modelRotation)
                            * glm::scale(glm::vec3(modelScale));
    const glm::mat4 view = camera->getViewMatrix();
    const glm::mat4 proj = camera->getProjectionMatrix();

    glm::ivec2 windowSize{};
    glfwGetWindowSize(window, &windowSize.x, &windowSize.y);

    const auto &[zNear, zFar] = camera->getClippingPlanes();

    static const glm::mat4 cubemapFaceProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);

    GraphicsUBO graphicsUbo{
        .window = {
            .windowWidth = static_cast<uint32_t>(windowSize.x),
            .windowHeight = static_cast<uint32_t>(windowSize.y),
        },
        .matrices = {
            .model = model,
            .view = view,
            .proj = proj,
            .viewInverse = glm::inverse(view),
            .projInverse = glm::inverse(proj),
            .vpInverse = glm::inverse(proj * view),
            .staticView = camera->getStaticViewMatrix(),
            .cubemapCaptureProj = cubemapFaceProjection
        },
        .misc = {
            .debugNumber = debugNumber,
            .zNear = zNear,
            .zFar = zFar,
            .useSsao = useSsao ? 1u : 0,
            .lightIntensity = lightIntensity,
            .lightDir = glm::vec3(mat4_cast(lightDirection) * glm::vec4(-1, 0, 0, 0)),
            .lightColor = lightColor,
            .cameraPos = camera->getPos(),
        }
    };

    static const std::array cubemapFaceViews{
        glm::lookAt(glm::vec3(0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0)),
        glm::lookAt(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)),
        glm::lookAt(glm::vec3(0), glm::vec3(0, 1, 0), glm::vec3(0, 0, -1)),
        glm::lookAt(glm::vec3(0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1)),
        glm::lookAt(glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0)),
        glm::lookAt(glm::vec3(0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0))
    };

    for (size_t i = 0; i < 6; i++) {
        graphicsUbo.matrices.cubemapCaptureViews[i] = cubemapFaceViews[i];
    }

    memcpy(frameResources[currentFrameIdx].graphicsUboMapped, &graphicsUbo, sizeof(graphicsUbo));
}
} // zrx
