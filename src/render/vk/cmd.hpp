#pragma once

#include "src/render/libs.hpp"
#include "src/render/globals.hpp"

namespace zrx {
struct RendererContext;

struct SecondaryCommandBuffer {
    unique_ptr<vk::raii::CommandBuffer> buffer;
    bool wasRecordedThisFrame = false;

    vk::raii::CommandBuffer& operator*() const { return *buffer; }
};

namespace utils::cmd {
    /**
    * Allocates and begins a new command buffer which is supposed to be recorded once
    * and destroyed after submission.
    *
    * @param ctx Renderer context.
    * @return The created single-use command buffer.
    */
    [[nodiscard]] vk::raii::CommandBuffer
    beginSingleTimeCommands(const RendererContext& ctx);

    /**
    * Ends a single-time command buffer created beforehand by `beginSingleTimeCommands`.
    * The buffer is then submitted and execution stops until the commands are fully processed.
    *
    * @param commandBuffer The single-use command buffer which should be ended.
    * @param queue The queue to which the buffer should be submitted.
    */
    void endSingleTimeCommands(const vk::raii::CommandBuffer &commandBuffer, const vk::raii::Queue &queue);

    /**
     * Convenience wrapper over `beginSingleTimeCommands` and `endSingleTimeCommands`.
     *
     * @param ctx Renderer context.
     * @param func Lambda containing commands with which the command buffer will be filled.
     */
    void doSingleTimeCommands(const RendererContext& ctx,
                              const std::function<void(const vk::raii::CommandBuffer &)> &func);

    /**
     * Shorthand function to set all dynamic states used in rendering.
     * This currently includes only viewport and scissor, but might be extended later.
     */
    void setDynamicStates(const vk::raii::CommandBuffer &commandBuffer, vk::Extent2D drawExtent);
}
} // zrx
