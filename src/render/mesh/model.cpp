#include "model.hpp"

#include <iostream>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "vertex.hpp"
#include "src/render/renderer.hpp"
#include "src/render/vk/image.hpp"
#include "src/render/vk/buffer.hpp"

namespace zrx {
static glm::vec3 assimpVecToGlm(const aiVector3D &v) {
    return {v.x, v.y, v.z};
}

static glm::mat4 assimpMatrixToGlm(const aiMatrix4x4 &m) {
    glm::mat4 res;

    res[0][0] = m.a1;
    res[1][0] = m.a2;
    res[2][0] = m.a3;
    res[3][0] = m.a4;

    res[0][1] = m.b1;
    res[1][1] = m.b2;
    res[2][1] = m.b3;
    res[3][1] = m.b4;

    res[0][2] = m.c1;
    res[1][2] = m.c2;
    res[2][2] = m.c3;
    res[3][2] = m.c4;

    res[0][3] = m.d1;
    res[1][3] = m.d2;
    res[2][3] = m.d3;
    res[3][3] = m.d4;

    return res;
}

Mesh::Mesh(const aiMesh *assimpMesh) : materialID(assimpMesh->mMaterialIndex) {
    std::unordered_map<ModelVertex, uint32_t> uniqueVertices;

    for (size_t faceIdx = 0; faceIdx < assimpMesh->mNumFaces; faceIdx++) {
        const auto &face = assimpMesh->mFaces[faceIdx];

        for (size_t i = 0; i < face.mNumIndices; i++) {
            ModelVertex vertex{};

            if (assimpMesh->HasPositions()) {
                vertex.pos = assimpVecToGlm(assimpMesh->mVertices[face.mIndices[i]]);
            }

            if (assimpMesh->HasTextureCoords(0)) {
                vertex.texCoord = {
                    assimpMesh->mTextureCoords[0][face.mIndices[i]].x,
                    1.0f - assimpMesh->mTextureCoords[0][face.mIndices[i]].y
                };
            }

            if (assimpMesh->HasTangentsAndBitangents()) {
                vertex.normal = assimpVecToGlm(assimpMesh->mNormals[face.mIndices[i]]);
            }

            if (assimpMesh->HasTangentsAndBitangents()) {
                vertex.tangent   = assimpVecToGlm(assimpMesh->mTangents[face.mIndices[i]]);
                vertex.bitangent = assimpVecToGlm(assimpMesh->mBitangents[face.mIndices[i]]);
            }

            if (!uniqueVertices.contains(vertex)) {
                uniqueVertices[vertex] = vertices.size();
                vertices.push_back(vertex);
            }

            indices.push_back(uniqueVertices.at(vertex));
        }
    }
}

Material::Material(const RendererContext &ctx, const aiMaterial *assimpMaterial,
                   const std::filesystem::path &basePath) {
    // base color

    aiString baseColorRelPath;
    aiReturn result = assimpMaterial->GetTexture(aiTextureType_BASE_COLOR, 0, &baseColorRelPath);

    if (result == aiReturn_SUCCESS) {
        auto path = basePath;
        path /= baseColorRelPath.C_Str();
        path.make_preferred();

        try {
            baseColor = TextureBuilder()
                    .makeMipmaps()
                    .fromPaths({path})
                    .create(ctx);
        } catch (std::exception &e) {
            std::cerr << "failed to allocate buffer for texture: " << path << std::endl;
            baseColor = nullptr;
        }
    }

    // normal map

    aiString normalRelPath;
    if (assimpMaterial->GetTexture(aiTextureType_NORMALS, 0, &normalRelPath) != aiReturn_SUCCESS) {
        result = assimpMaterial->GetTexture(aiTextureType_NORMAL_CAMERA, 0, &normalRelPath);
    }

    if (result == aiReturn_SUCCESS) {
        auto path = basePath;
        path /= normalRelPath.C_Str();
        path.make_preferred();

        normal = TextureBuilder()
                .useFormat(vk::Format::eR8G8B8A8Unorm)
                .fromPaths({path})
                .makeMipmaps()
                .create(ctx);
    }

    // orm

    std::filesystem::path aoPath, roughnessPath, metallicPath;

    aiString aoRelPath;
    if (assimpMaterial->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &aoRelPath) == aiReturn_SUCCESS) {
        aoPath = basePath;
        aoPath /= aoRelPath.C_Str();
        aoPath.make_preferred();
    }

    aiString roughnessRelPath;
    if (assimpMaterial->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &roughnessRelPath) == aiReturn_SUCCESS) {
        roughnessPath = basePath;
        roughnessPath /= roughnessRelPath.C_Str();
        roughnessPath.make_preferred();
    }

    aiString metallicRelPath;
    if (assimpMaterial->GetTexture(aiTextureType_METALNESS, 0, &metallicRelPath) == aiReturn_SUCCESS) {
        metallicPath = basePath;
        metallicPath /= metallicRelPath.C_Str();
        metallicPath.make_preferred();
    }

    auto ormBuilder = TextureBuilder()
            .useFormat(vk::Format::eR8G8B8A8Unorm)
            .makeMipmaps()
            .withSwizzle({
                aoPath.empty() ? SwizzleComponent::MAX : SwizzleComponent::R,
                roughnessPath.empty() ? SwizzleComponent::MAX : SwizzleComponent::G,
                metallicPath.empty() ? SwizzleComponent::ZERO : SwizzleComponent::B,
                SwizzleComponent::MAX,
            });

    if (aoPath.empty() && roughnessPath.empty() && metallicPath.empty()) {
        ormBuilder.fromSwizzleFill({1, 1, 1});
    } else if (!aoPath.empty() && (aoPath == roughnessPath || aoPath == metallicPath)) {
        ormBuilder.fromPaths({aoPath});
    } else if (!roughnessPath.empty() && (roughnessPath == aoPath || roughnessPath == metallicPath)) {
        ormBuilder.fromPaths({roughnessPath});
    } else if (!metallicPath.empty() && (metallicPath == aoPath || metallicPath == roughnessPath)) {
        ormBuilder.fromPaths({metallicPath});
    } else {
        ormBuilder.asSeparateChannels().fromPaths({aoPath, roughnessPath, metallicPath});
    }

    orm = ormBuilder.create(ctx);
}

Model::Model(const RendererContext &ctx, const std::filesystem::path &path, const bool loadMaterials) {
    Assimp::Importer importer;

    const aiScene *scene = importer.ReadFile(
        path.string(),
        aiProcess_RemoveRedundantMaterials
        | aiProcess_FindInstances
        | aiProcess_OptimizeMeshes
        | aiProcess_OptimizeGraph
        | aiProcess_FixInfacingNormals
        | aiProcess_Triangulate
        | aiProcess_JoinIdenticalVertices
        | aiProcess_CalcTangentSpace
        | aiProcess_SortByPType
        | aiProcess_ImproveCacheLocality
        | aiProcess_ValidateDataStructure
    );

    if (!scene) {
        throw std::runtime_error(importer.GetErrorString());
    }

    if (loadMaterials) {
        constexpr size_t MAX_MATERIAL_COUNT = 32;
        if (scene->mNumMaterials > MAX_MATERIAL_COUNT) {
            throw std::runtime_error("Models with more than 32 materials are not supported");
        }

        for (size_t i = 0; i < scene->mNumMaterials; i++) {
            std::filesystem::path basePath = path.parent_path();
            materials.emplace_back(ctx, scene->mMaterials[i], basePath);
        }
    }

    for (size_t i = 0; i < scene->mNumMeshes; i++) {
        meshes.emplace_back(scene->mMeshes[i]);

        if (!loadMaterials) {
            meshes.back().materialID = 0;
        }
    }

    addInstances(scene->mRootNode, glm::identity<glm::mat4>());

    normalizeScale();

    createBuffers(ctx);
    createBLAS(ctx);
}

void Model::addInstances(const aiNode *node, const glm::mat4 &baseTransform) {
    const glm::mat4 transform = baseTransform * assimpMatrixToGlm(node->mTransformation);

    for (size_t i = 0; i < node->mNumMeshes; i++) {
        meshes[node->mMeshes[i]].instances.push_back(transform);
    }

    for (size_t i = 0; i < node->mNumChildren; i++) {
        addInstances(node->mChildren[i], transform);
    }
}

std::vector<ModelVertex> Model::getVertices() const {
    std::vector<ModelVertex> vertices;

    size_t totalSize = 0;
    for (const auto &mesh: meshes) {
        totalSize += mesh.vertices.size();
    }

    vertices.reserve(totalSize);

    for (const auto &mesh: meshes) {
        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
    }

    return vertices;
}

std::vector<uint32_t> Model::getIndices() const {
    std::vector<uint32_t> indices;

    size_t totalSize = 0;
    for (const auto &mesh: meshes) {
        totalSize += mesh.indices.size();
    }

    indices.reserve(totalSize);

    for (const auto &mesh: meshes) {
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
    }

    return indices;
}

std::vector<glm::mat4> Model::getInstanceTransforms() const {
    std::vector<glm::mat4> result;

    size_t totalSize = 0;
    for (const auto &mesh: meshes) {
        totalSize += mesh.instances.size();
    }

    result.reserve(totalSize);

    for (const auto &mesh: meshes) {
        result.insert(result.end(), mesh.instances.begin(), mesh.instances.end());
    }

    return result;
}

std::vector<MeshDescription> Model::getMeshDescriptions() const {
    std::vector<MeshDescription> result;

    uint32_t indexOffset = 0;
    uint32_t vertexOffset = 0;

    for (const auto &mesh: meshes) {
        result.emplace_back(MeshDescription {
            .materialID = mesh.materialID,
            .vertexOffset = vertexOffset,
            .indexOffset = indexOffset,
        });

        indexOffset += static_cast<uint32_t>(mesh.indices.size());
        vertexOffset += static_cast<std::int32_t>(mesh.vertices.size());
    }

    return result;
}

void Model::bindBuffers(const vk::raii::CommandBuffer &commandBuffer) const {
    commandBuffer.bindVertexBuffers(0, **vertexBuffer, {0});
    commandBuffer.bindVertexBuffers(1, **instanceDataBuffer, {0});
    commandBuffer.bindIndexBuffer(**indexBuffer, 0, vk::IndexType::eUint32);
}

void Model::createBuffers(const RendererContext &ctx) {
    constexpr auto rayTracingFlags = vk::BufferUsageFlagBits::eStorageBuffer
                                     | vk::BufferUsageFlagBits::eShaderDeviceAddress
                                     | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;

    vertexBuffer = utils::buf::createLocalBuffer(
        ctx,
        getVertices(),
        vk::BufferUsageFlagBits::eVertexBuffer | rayTracingFlags
    );

    instanceDataBuffer = utils::buf::createLocalBuffer(
        ctx,
        getInstanceTransforms(),
        vk::BufferUsageFlagBits::eVertexBuffer | rayTracingFlags
    );

    indexBuffer = utils::buf::createLocalBuffer(
        ctx,
        getIndices(),
        vk::BufferUsageFlagBits::eIndexBuffer | rayTracingFlags
    );

    meshDescriptionsBuffer = utils::buf::createLocalBuffer(
        ctx,
        getMeshDescriptions(),
        rayTracingFlags
    );
}

void Model::createBLAS(const RendererContext &ctx) {
    const vk::DeviceAddress vertexAddress = ctx.device->getBufferAddress({.buffer = **vertexBuffer});
    const vk::DeviceAddress indexAddress  = ctx.device->getBufferAddress({.buffer = **indexBuffer});

    const uint32_t maxPrimitiveCount = getIndices().size() / 3;

    const vk::AccelerationStructureGeometryTrianglesDataKHR geometryTriangles{
        .vertexFormat = vk::Format::eR32G32B32Sfloat,
        .vertexData = vertexAddress,
        .vertexStride = sizeof(ModelVertex),
        .maxVertex = static_cast<uint32_t>(getVertices().size() - 1),
        .indexType = vk::IndexType::eUint32,
        .indexData = indexAddress,
    };

    const vk::AccelerationStructureGeometryKHR geometry{
        .geometryType = vk::GeometryTypeKHR::eTriangles,
        .geometry = geometryTriangles,
        .flags = vk::GeometryFlagBitsKHR::eOpaque,
    };

    vk::AccelerationStructureBuildGeometryInfoKHR geometryInfo{
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1u,
        .pGeometries = &geometry,
    };

    const vk::AccelerationStructureBuildRangeInfoKHR rangeInfo{
        .primitiveCount = maxPrimitiveCount,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0,
    };

    const auto buildSizes = ctx.device->getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice,
        geometryInfo,
        maxPrimitiveCount
    );

    // scratch buffer creation

    const Buffer scratchBuffer{
        **ctx.allocator,
        buildSizes.buildScratchSize,
        vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal
    };

    geometryInfo.scratchData = ctx.device->getBufferAddress({.buffer = *scratchBuffer});

    // acceleration structure creation

    const uint32_t accelerationStructureSize = buildSizes.accelerationStructureSize;

    auto blasBuffer = make_unique<Buffer>(
        **ctx.allocator,
        accelerationStructureSize,
        vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR,
        vk::MemoryPropertyFlagBits::eDeviceLocal
    );

    const vk::AccelerationStructureCreateInfoKHR asCreateInfo{
        .buffer = **blasBuffer,
        .size = accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
    };

    auto blasHandle = make_unique<vk::raii::AccelerationStructureKHR>(
        ctx.device->createAccelerationStructureKHR(asCreateInfo)
    );

    geometryInfo.dstAccelerationStructure = **blasHandle;

    blas = make_unique<AccelerationStructure>(
        std::move(blasHandle),
        std::move(blasBuffer)
    );

    // todo - compact

    utils::cmd::doSingleTimeCommands(ctx, [&](const vk::raii::CommandBuffer &commandBuffer) {
        commandBuffer.buildAccelerationStructuresKHR(geometryInfo, &rangeInfo);
    });
}

void Model::normalizeScale() {
    constexpr float standardScale = 10.0f;
    const float largestDistance   = getMaxVertexDistance();
    const glm::mat4 scaleMatrix   = glm::scale(glm::identity<glm::mat4>(), glm::vec3(standardScale / largestDistance));

    for (auto &mesh: meshes) {
        for (auto &transform: mesh.instances) {
            transform = scaleMatrix * transform;
        }
    }
}

float Model::getMaxVertexDistance() const {
    float largestDistance = 0.0;

    for (const auto &mesh: meshes) {
        for (const auto &vertex: mesh.vertices) {
            for (const auto &transform: mesh.instances) {
                largestDistance = std::max(
                    largestDistance,
                    glm::length(glm::vec3(transform * glm::vec4(vertex.pos, 1.0)))
                );
            }
        }
    }

    return largestDistance;
}
}
