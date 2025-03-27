#include "graph.hpp"

namespace zrx {
[[nodiscard]] std::set<ResourceHandle> ShaderPack::get_bound_resources_set() const {
    std::set<ResourceHandle> result;

    for (const auto &set: descriptor_set_descs) {
        for (const auto &binding: set) {
            if (std::holds_alternative<ResourceHandle>(binding)) {
                result.insert(std::get<ResourceHandle>(binding));
            } else if (std::holds_alternative<ResourceHandleArray>(binding)) {
                result.insert(std::get<ResourceHandleArray>(binding).begin(),
                              std::get<ResourceHandleArray>(binding).end());
            }
        }
    }

    return result;
}

void RenderPassContext::draw_model(const ResourceHandle model_handle) const {
    uint32_t index_offset    = 0;
    int32_t vertex_offset    = 0;
    uint32_t instance_offset = 0;

    const Model &model = *models.get().at(model_handle);
    model.bind_buffers(command_buffer);

    for (const auto &mesh: model.get_meshes()) {
        command_buffer.get().drawIndexed(
            static_cast<uint32_t>(mesh.indices.size()),
            static_cast<uint32_t>(mesh.instances.size()),
            index_offset,
            vertex_offset,
            instance_offset
        );

        index_offset += static_cast<uint32_t>(mesh.indices.size());
        vertex_offset += static_cast<int32_t>(mesh.vertices.size());
        instance_offset += static_cast<uint32_t>(mesh.instances.size());
    }
}

void RenderPassContext::draw_screenspace_quad() const {
    command_buffer.get().bindVertexBuffers(0, *ss_quad_vertex_buffer.get(), {0});
    command_buffer.get().draw(screen_space_quad_vertices.size(), 1, 0, 0);
}

void RenderPassContext::draw_skybox() const {
    command_buffer.get().bindVertexBuffers(0, *skybox_vertex_buffer.get(), {0});
    command_buffer.get().draw(skybox_vertices.size(), 1, 0, 0);
}

std::set<ResourceHandle> RenderNode::get_all_targets_set() const {
    std::set result(color_targets.begin(), color_targets.end());
    if (depth_target) result.insert(*depth_target);
    return result;
}

// std::set<ResourceHandle> RenderNode::get_all_shader_resources_set() const {
//     const auto frag_resources = fragment_shader->get_bound_resources_set();
//     const auto vert_resources = vertex_shader->get_bound_resources_set();
//
//     std::set result(frag_resources.begin(), frag_resources.end());
//     result.insert(vert_resources.begin(), vert_resources.end());
//     return result;
// }

vector<RenderNodeHandle> RenderGraph::get_topo_sorted() const {
    vector<RenderNodeHandle> result;

    std::set<RenderNodeHandle> remaining;

    for (const auto &[handle, _]: nodes) {
        remaining.emplace(handle);
    }

    while (!remaining.empty()) {
        for (const auto &handle: remaining) {
            if (std::ranges::all_of(dependency_graph.at(handle), [&](const RenderNodeHandle &dep) {
                return !remaining.contains(dep);
            })) {
                result.push_back(handle);
                remaining.erase(handle);
                break;
            }
        }
    }

    return result;
}

RenderNodeHandle RenderGraph::add_node(const RenderNode &node) {
    const auto handle = get_new_node_handle();
    nodes.emplace(handle, node);

    const auto targets_set      = node.get_all_targets_set();
    const auto shader_resources = node.get_all_shader_resources_set();

    if (!detail::empty_intersection(targets_set, shader_resources)) {
        throw std::invalid_argument("invalid render node: cannot use a target as a shader resource!");
    }

    std::set<RenderNodeHandle> dependencies;

    // for each existing node A...
    for (const auto &[other_handle, other_node]: nodes) {
        const auto other_targets_set      = other_node.get_all_targets_set();
        const auto other_shader_resources = other_node.get_all_shader_resources_set();

        // ...if any of the new node's targets is sampled in A,
        // then the new node is A's dependency.
        if (!detail::empty_intersection(targets_set, other_shader_resources)) {
            dependency_graph.at(other_handle).emplace(handle);
        }

        // and if the new node samples any of A's targets,
        // then A is the new node's dependency.
        if (!detail::empty_intersection(other_targets_set, shader_resources)) {
            dependencies.emplace(other_handle);
        }
    }

    dependency_graph.emplace(handle, std::move(dependencies));

    check_dependency_cycles();

    return handle;
}

ResourceHandle RenderGraph::add_resource(UniformBufferResource &&resource) {
    return add_resource_generic(std::move(resource), uniform_buffers);
}

ResourceHandle RenderGraph::add_resource(ExternalTextureResource &&resource) {
    return add_resource_generic(std::move(resource), external_tex_resources);
}

ResourceHandle RenderGraph::add_resource(EmptyTextureResource &&resource) {
    return add_resource_generic(std::move(resource), empty_tex_resources);
}

ResourceHandle RenderGraph::add_resource(TransientTextureResource &&resource) {
    return add_resource_generic(std::move(resource), transient_tex_resources);
}

ResourceHandle RenderGraph::add_resource(ModelResource &&resource) {
    return add_resource_generic(std::move(resource), model_resources);
}

ResourceHandle RenderGraph::add_pipeline(ShaderPack &&resource) {
    return add_resource_generic(std::move(resource), pipelines);
}

void RenderGraph::add_frame_begin_action(FrameBeginCallback &&callback) {
    frame_begin_callbacks.emplace_back(std::move(callback));
}

void RenderGraph::cycles_helper(const RenderNodeHandle handle, std::set<RenderNodeHandle> &discovered,
                                std::set<RenderNodeHandle> &finished) const {
    discovered.emplace(handle);

    for (const auto &neighbour: dependency_graph.at(handle)) {
        if (discovered.contains(neighbour)) {
            throw std::invalid_argument("invalid render graph: illegal cycle in dependency graph!");
        }

        if (!finished.contains(neighbour)) {
            cycles_helper(neighbour, discovered, finished);
        }
    }

    discovered.erase(handle);
    finished.emplace(handle);
};

void RenderGraph::check_dependency_cycles() const {
    std::set<RenderNodeHandle> discovered, finished;

    for (const auto &[handle, _]: nodes) {
        if (!discovered.contains(handle) && !finished.contains(handle)) {
            cycles_helper(handle, discovered, finished);
        }
    }
}

ResourceHandle RenderGraph::get_new_node_handle() {
    static RenderNodeHandle next_free_node_handle = 0;
    return next_free_node_handle++;
}

ResourceHandle RenderGraph::get_new_resource_handle() {
    static ResourceHandle next_free_resource_handle = 0;
    return next_free_resource_handle++;
}
} // zrx
