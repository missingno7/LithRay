#include "renderer.h"
#include "shaders.h"
#include "image_io.h"
#include <algorithm>
#include <stdarg.h>

static constexpr uint32_t kStaticVerts = 600000, kDynVerts = 500000;
static constexpr uint32_t kStaticIndices = 2400000, kDynIndices = 1500000;
static constexpr uint32_t kMaxInstances = 16384, kMaxSurfaces = 131072, kMaxLights = 64, kMaxTlas = 4096;
// aux carries a 24-bit packed normal in one channel, hence 32-bit floats
// the scene is rendered and lit in 16-bit float: no banding, and room above white for the modern look
static constexpr VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT, kAuxFmt = VK_FORMAT_R32G32B32A32_SFLOAT, kDepthFmt = VK_FORMAT_D32_SFLOAT,
                          kLightFmt = VK_FORMAT_R16G16B16A16_SFLOAT;

// ------------------------------------------------------------------ setup
void Renderer::init() {
    gfx.init();
    createStatic();
}

void Renderer::writeTextureSlot(uint32_t slot, VkImageView view) {
    VkDescriptorImageInfo ii = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    if (slot == kTexLightmap) ii.sampler = clampSampler;
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sceneSet; w.dstBinding = 0; w.dstArrayElement = slot; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(gfx.dev, 1, &w, 0, nullptr);
}

void Renderer::createStatic() {
    VkDevice dev = gfx.dev;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.anisotropyEnable = VK_TRUE; sci.maxAnisotropy = 8; sci.maxLod = VK_LOD_CLAMP_NONE;
    VKC(vkCreateSampler(dev, &sci, nullptr, &sampler));
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.anisotropyEnable = VK_FALSE;
    VKC(vkCreateSampler(dev, &sci, nullptr, &clampSampler));
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VKC(vkCreateSampler(dev, &sci, nullptr, &nearestSampler));

    // scene set
    {
        VkDescriptorSetLayoutBinding b[12] = {};
        VkDescriptorBindingFlags bf[12] = {};
        auto stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bf[0] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        b[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, (VkShaderStageFlags)stages, nullptr};
        for (uint32_t i = 2; i <= 8; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, (VkShaderStageFlags)stages, nullptr};
        b[9] = {9, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bf[9] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        b[10] = {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        b[11] = {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        uint32_t n = 12;
        if (!gfx.rt) { b[9] = b[10]; b[10] = b[11]; bf[9] = 0; n = 11; }
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        fi.bindingCount = n; fi.pBindingFlags = bf;
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.pNext = &fi; li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT; li.bindingCount = n; li.pBindings = b;
        VKC(vkCreateDescriptorSetLayout(dev, &li, nullptr, &sceneLayout));
    }
    {
        VkDescriptorSetLayoutBinding b[8];
        for (uint32_t i = 0; i < 8; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 8; li.pBindings = b;
        VKC(vkCreateDescriptorSetLayout(dev, &li, nullptr, &postLayout));
    }
    VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures + 160}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                 {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 24}, {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT; pi.maxSets = 16;
    pi.poolSizeCount = gfx.rt ? 4 : 3; pi.pPoolSizes = ps;
    VKC(vkCreateDescriptorPool(dev, &pi, nullptr, &descPool));
    VkDescriptorSetLayout layouts[10] = {sceneLayout, postLayout, postLayout, postLayout, postLayout, postLayout, postLayout, postLayout, postLayout, postLayout};
    VkDescriptorSet sets[10];
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool; ai.descriptorSetCount = 10; ai.pSetLayouts = layouts;
    VKC(vkAllocateDescriptorSets(dev, &ai, sets));
    sceneSet = sets[0]; postSet[0] = sets[1]; postSet[1] = sets[2]; composeSet[0] = sets[3]; composeSet[1] = sets[4];
    for (int i = 0; i < 5; i++) bloomSet[i] = sets[5 + i];

    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &sceneLayout; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKC(vkCreatePipelineLayout(dev, &pli, nullptr, &scenePL));
    VkPushConstantRange ppcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
    pli.pSetLayouts = &postLayout; pli.pPushConstantRanges = &ppcr;
    VKC(vkCreatePipelineLayout(dev, &pli, nullptr, &postPL));

    // buffers
    auto geo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    vbuf = gfx.createBuffer((VkDeviceSize)(kStaticVerts + 2 * kDynVerts) * sizeof(GVertex), geo | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    ibuf = gfx.createBuffer((VkDeviceSize)(kStaticIndices + 2 * kDynIndices) * 4, geo | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    triBuf = gfx.createBuffer((VkDeviceSize)(kStaticIndices + kDynIndices) / 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    instBuf = gfx.createBuffer((VkDeviceSize)kMaxInstances * sizeof(GInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    surfBuf = gfx.createBuffer((VkDeviceSize)kMaxSurfaces * sizeof(GSurface), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    lightBuf = gfx.createBuffer(kMaxLights * sizeof(GLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    texInfoBuf = gfx.createBuffer(kMaxTextures * sizeof(GTexInfo), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    frameBuf = gfx.createBuffer(sizeof(GFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    texInfoCpu.assign(kMaxTextures, GTexInfo{0, 0, 0, 0});
    memset(texInfoBuf.mapped, 0, kMaxTextures * sizeof(GTexInfo));
    if (gfx.rt)
        tlasInstBuf = gfx.createBuffer(kMaxTlas * sizeof(VkAccelerationStructureInstanceKHR),
                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);

    lampBuf = gfx.createBuffer(kMaxLamps * sizeof(GLamp), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    lampDataBuf = gfx.createBuffer((48 * 48 * 48 + 1 + 1024 * 1024) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    Buffer *bufs[] = {&frameBuf, &vbuf, &ibuf, &instBuf, &triBuf, &surfBuf, &lightBuf, &texInfoBuf, &lampBuf, &lampDataBuf};
    for (uint32_t i = 0; i < 10; i++) {
        VkDescriptorBufferInfo bi = {bufs[i]->buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sceneSet; w.dstBinding = i < 8 ? i + 1 : i + 2; w.descriptorCount = 1;
        w.descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    white = gfx.createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    uint8_t wpx[4] = {255, 255, 255, 255};
    gfx.uploadImage(white, wpx, 1);
    flatNormal = gfx.createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    uint8_t fpx[4] = {128, 128, 255, 255};
    gfx.uploadImage(flatNormal, fpx, 1);
    for (uint32_t i = 0; i < kMaxTextures; i++) writeTextureSlot(i, (i >= kMaxTextures / 2 && i != kTexLightmap) ? flatNormal.view : white.view);

    for (int r = 0; r < (gfx.rt ? 2 : 1); r++)
        for (int p = 0; p < P_COUNT; p++) pipes[r][p] = makeScenePipeline((Pipe)p, r == 1);

    // the post pipeline depends on the swapchain format: created with the targets
    bloomPipe = makeFullscreenPipeline(bloom_frag_spv, sizeof(bloom_frag_spv), kLightFmt, VK_FORMAT_UNDEFINED);
    composePipe = makeFullscreenPipeline(compose_frag_spv, sizeof(compose_frag_spv), kColorFmt, kLightFmt, kLightFmt);

    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = gfx.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VKC(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKC(vkCreateFence(dev, &fci, nullptr, &fence));
    VkSemaphoreCreateInfo sci2 = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKC(vkCreateSemaphore(dev, &sci2, nullptr, &acquireSem));
    VKC(vkCreateSemaphore(dev, &sci2, nullptr, &renderSem));
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
    VKC(vkCreateQueryPool(dev, &qi, nullptr, &queryPool));
    QueryPerformanceCounter(&statStart);
}

VkPipeline Renderer::makeFullscreenPipeline(const uint32_t *fsCode, size_t fsBytes, VkFormat f0, VkFormat f1, VkFormat f2) {
    VkDevice dev = gfx.dev;
    VkShaderModule vs = gfx.shader(post_vert_spv, sizeof(post_vert_spv)), fs = gfx.shader(fsCode, fsBytes);
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba[3] = {};
    ba[0].colorWriteMask = ba[1].colorWriteMask = ba[2].colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    uint32_t nTargets = f1 == VK_FORMAT_UNDEFINED ? 1 : f2 == VK_FORMAT_UNDEFINED ? 2 : 3;
    cb.attachmentCount = nTargets; cb.pAttachments = ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkFormat fmts[3] = {f0, f1, f2};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = nTargets; ri.pColorAttachmentFormats = fmts;
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &ri; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dy; gp.layout = postPL;
    VkPipeline pipe;
    VKC(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe));
    vkDestroyShaderModule(dev, vs, nullptr); vkDestroyShaderModule(dev, fs, nullptr);
    return pipe;
}

VkPipeline Renderer::makeScenePipeline(Pipe p, bool rt) {
    VkDevice dev = gfx.dev;
    VkShaderModule vs = gfx.shader(scene_vert_spv, sizeof(scene_vert_spv));
    VkShaderModule fs = rt ? gfx.shader(scene_rt_frag_spv, sizeof(scene_rt_frag_spv)) : gfx.shader(scene_frag_spv, sizeof(scene_frag_spv));
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkVertexInputBindingDescription bind = {0, sizeof(GVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription at[5] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 16}, {3, 0, VK_FORMAT_R32G32_SFLOAT, 24}, {4, 0, VK_FORMAT_R32G32B32_SFLOAT, 32}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind; vi.vertexAttributeDescriptionCount = 5; vi.pVertexAttributeDescriptions = at;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = p != P_NOZ; ds.depthWriteEnable = p == P_OPAQUE || p == P_SKYMASK || p == P_BLEND_ZWRITE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState ba[4] = {};
    bool blend = p == P_BLEND || p == P_NOZ || p == P_BLEND_ZWRITE || p == P_ADD;
    ba[0].colorWriteMask = p == P_SKYMASK ? 0 : 0xf;
    ba[1].colorWriteMask = (p == P_OPAQUE) ? 0xf : 0;
    ba[2].colorWriteMask = (p == P_OPAQUE) ? 0xf : 0;
    ba[3].colorWriteMask = (p == P_OPAQUE) ? 0xf : 0;
    if (blend) {
        ba[0].blendEnable = VK_TRUE;
        ba[0].srcColorBlendFactor = p == P_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
        ba[0].dstColorBlendFactor = p == P_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba[0].colorBlendOp = VK_BLEND_OP_ADD;
        ba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 4; cb.pAttachments = ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkFormat fmts[4] = {kColorFmt, kAuxFmt, kLightFmt, kLightFmt};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 4; ri.pColorAttachmentFormats = fmts; ri.depthAttachmentFormat = kDepthFmt;
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &ri; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dy; gp.layout = scenePL;
    VkPipeline pipe;
    VKC(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe));
    vkDestroyShaderModule(dev, vs, nullptr); vkDestroyShaderModule(dev, fs, nullptr);
    return pipe;
}

void Renderer::destroyTargets() {
    gfx.destroy(sceneColor); gfx.destroy(sceneAux); gfx.destroy(sceneDepth); gfx.destroy(history[0]); gfx.destroy(history[1]);
    for (Image &b : bloom) gfx.destroy(b);
    gfx.destroy(directHist[0]); gfx.destroy(directHist[1]);
    gfx.destroy(sceneIndirect); gfx.destroy(sceneLight); gfx.destroy(lightHist[0]); gfx.destroy(lightHist[1]); gfx.destroy(litColor);
}

void Renderer::createTargets() {
    destroyTargets();
    uint32_t w = gfx.extent.width, h = gfx.extent.height;
    auto use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    sceneColor = gfx.createImage(w, h, kColorFmt, use);
    sceneAux = gfx.createImage(w, h, kAuxFmt, use);
    history[0] = gfx.createImage(w, h, kAuxFmt, use);
    history[1] = gfx.createImage(w, h, kAuxFmt, use);
    sceneDepth = gfx.createImage(w, h, kDepthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    sceneLight = gfx.createImage(w, h, kLightFmt, use);
    sceneIndirect = gfx.createImage(w, h, kLightFmt, use);
    directHist[0] = gfx.createImage(w, h, kLightFmt, use);
    directHist[1] = gfx.createImage(w, h, kLightFmt, use);
    bloom[0] = gfx.createImage(std::max(w / 4, 1u), std::max(h / 4, 1u), kLightFmt, use);
    bloom[1] = gfx.createImage(std::max(w / 4, 1u), std::max(h / 4, 1u), kLightFmt, use);
    bloom[2] = gfx.createImage(std::max(w / 16, 1u), std::max(h / 16, 1u), kLightFmt, use);
    bloom[3] = gfx.createImage(std::max(w / 16, 1u), std::max(h / 16, 1u), kLightFmt, use);
    if (postPipeFormat != gfx.swapFormat) {
        if (postPipe) vkDestroyPipeline(gfx.dev, postPipe, nullptr);
        postPipe = makeFullscreenPipeline(post_frag_spv, sizeof(post_frag_spv), gfx.swapFormat, kAuxFmt);
        postPipeFormat = gfx.swapFormat;
    }
    lightHist[0] = gfx.createImage(w, h, kLightFmt, use);
    lightHist[1] = gfx.createImage(w, h, kLightFmt, use);
    litColor = gfx.createImage(w, h, kColorFmt, use);
    lightHistValid = false;
    VkCommandBuffer c = gfx.beginOnce();
    for (Image *i : {&sceneColor, &sceneAux, &history[0], &history[1], &sceneLight, &sceneIndirect, &lightHist[0], &lightHist[1], &litColor,
                     &bloom[0], &bloom[1], &bloom[2], &bloom[3], &directHist[0], &directHist[1]})
        transition(c, i->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(c, sceneDepth.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    gfx.endOnce(c);
    historyValid = false;
    if (!overlayImg.img) {
        overlayImg = gfx.createImage(modeW, modeH, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        std::vector<uint8_t> zero((size_t)modeW * modeH * 4, 0);
        gfx.uploadImage(overlayImg, zero.data(), 1);
    }
    for (int s = 0; s < 2; s++) {
        VkDescriptorImageInfo ii[5] = {{clampSampler, litColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {nearestSampler, sceneAux.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {nearestSampler, sceneDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {clampSampler, history[1 - s].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {clampSampler, overlayImg.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w5[5];
        for (uint32_t i = 0; i < 5; i++) {
            w5[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w5[i].dstSet = postSet[s]; w5[i].dstBinding = i; w5[i].descriptorCount = 1;
            w5[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w5[i].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets(gfx.dev, 5, w5, 0, nullptr);
        VkDescriptorImageInfo ci[5] = {{nearestSampler, sceneColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {nearestSampler, sceneAux.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {nearestSampler, sceneDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {nearestSampler, sceneLight.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {clampSampler, lightHist[1 - s].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        for (uint32_t i = 0; i < 5; i++) { w5[i].dstSet = composeSet[s]; w5[i].pImageInfo = &ci[i]; }
        vkUpdateDescriptorSets(gfx.dev, 5, w5, 0, nullptr);
        VkDescriptorImageInfo ind = {nearestSampler, sceneIndirect.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wi = w5[0];
        wi.dstBinding = 5; wi.pImageInfo = &ind;
        vkUpdateDescriptorSets(gfx.dev, 1, &wi, 0, nullptr);
        VkDescriptorImageInfo dh = {clampSampler, directHist[1 - s].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        wi.dstSet = composeSet[s]; wi.dstBinding = 6; wi.pImageInfo = &dh;
        vkUpdateDescriptorSets(gfx.dev, 1, &wi, 0, nullptr);
        // post: bloom at bindings 5 and 6
        VkDescriptorImageInfo bn = {clampSampler, bloom[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo bw = {clampSampler, bloom[2].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        wi.dstSet = postSet[s]; wi.dstBinding = 5; wi.pImageInfo = &bn;
        vkUpdateDescriptorSets(gfx.dev, 1, &wi, 0, nullptr);
        wi.dstBinding = 6; wi.pImageInfo = &bw;
        vkUpdateDescriptorSets(gfx.dev, 1, &wi, 0, nullptr);
    }
    {   // bloom chain sources
        Image *src[5] = {&litColor, &bloom[0], &bloom[1], &bloom[2], &bloom[3]};
        for (int i = 0; i < 5; i++) {
            VkDescriptorImageInfo di = {clampSampler, src[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet wb = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wb.dstSet = bloomSet[i]; wb.dstBinding = 0; wb.descriptorCount = 1; wb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wb.pImageInfo = &di;
            vkUpdateDescriptorSets(gfx.dev, 1, &wb, 0, nullptr);
        }
    }
}

void Renderer::shutdown() {
    if (!gfx.dev) return;
    vkDeviceWaitIdle(gfx.dev);
    onTerm();
    gfx.shutdown();     // remaining objects die with the device/process
}

// ------------------------------------------------------------------ window
static LRESULT CALLBACK ViewProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;       // all input belongs to the game window
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcA(w, m, wp, lp);
}
struct WindowThreadArgs { HWND parent; HWND result; HANDLE ready; };
static DWORD WINAPI WindowThread(void *p) {
    WindowThreadArgs *a = (WindowThreadArgs *)p;
    WNDCLASSA wc = {};
    wc.lpfnWndProc = ViewProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "LithRayView";
    RegisterClassA(&wc);
    RECT rc; GetClientRect(a->parent, &rc);
    // A disabled child of the game's window: it covers the client area, never takes focus or input, and dies
    // with its parent. The parent belongs to another (32-bit) process; this thread keeps pumping so cross-process
    // window messages never wait on the render thread.
    HWND w = CreateWindowExA(WS_EX_NOPARENTNOTIFY | WS_EX_NOACTIVATE, "LithRayView", "", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                             0, 0, rc.right, rc.bottom, a->parent, nullptr, wc.hInstance, nullptr);
    a->result = w;
    SetEvent(a->ready);
    if (!w) return 1;
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

bool Renderer::ensureWindow() {
    if (!gameWindow || !IsWindow(gameWindow)) return false;
    if (!viewWindow || !IsWindow(viewWindow)) {
        static WindowThreadArgs args;
        args = {gameWindow, nullptr, CreateEventA(nullptr, TRUE, FALSE, nullptr)};
        CloseHandle(CreateThread(nullptr, 0, WindowThread, &args, 0, nullptr));
        WaitForSingleObject(args.ready, 5000);
        CloseHandle(args.ready);
        viewWindow = args.result;
        if (!viewWindow) { Log("could not create the view window: %lu", GetLastError()); return false; }
        gfx.destroySwapchain(true);
    }
    RECT pr, cr;
    GetClientRect(gameWindow, &pr); GetClientRect(viewWindow, &cr);
    if (pr.right != cr.right || pr.bottom != cr.bottom)
        SetWindowPos(viewWindow, HWND_TOP, 0, 0, pr.right, pr.bottom, SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    bool wantVsync = settings.vsync != 0 || !sceneValid;
    bool wantHdr = settings.hdr != 0;
    if (!gfx.swapchain || gfx.extent.width != (uint32_t)pr.right || gfx.extent.height != (uint32_t)pr.bottom || gfx.vsync != wantVsync ||
        gfx.hdrWanted != wantHdr) {
        if (pr.right <= 0 || pr.bottom <= 0) return false;
        if (frameInFlight) { vkWaitForFences(gfx.dev, 1, &fence, VK_TRUE, UINT64_MAX); frameInFlight = false; }
        if (!gfx.createSwapchain(viewWindow, wantVsync, wantHdr)) return false;
        createTargets();
    }
    return gfx.swapchain != VK_NULL_HANDLE;
}

// ------------------------------------------------------------------ decode
void Renderer::onInit(const lr::InitCmd &c) {
    Log("init: mode %ux%u windowed %u hwnd %llx", c.modeWidth, c.modeHeight, c.windowed, (unsigned long long)c.hwnd);
    gameWindow = (HWND)(uintptr_t)c.hwnd;
    modeW = c.modeWidth; modeH = c.modeHeight; windowed = c.windowed != 0;
    vkDeviceWaitIdle(gfx.dev);
    gfx.destroy(overlayImg);
    overlayEmpty = true; sceneValid = false;
    ensureWindow();
}

void Renderer::onTerm() {
    Log("term");
    vkDeviceWaitIdle(gfx.dev);
    frameInFlight = false;
    onWorldEnd();
    for (uint32_t i = 1; i < kMaxTextures; i++) if (textures[i].img) { gfx.destroy(textures[i]); writeTextureSlot(i, white.view); }
    for (uint32_t i = 1; i < kMaxTextures / 2 - 1; i++) if (normalMaps[i].img) { gfx.destroy(normalMaps[i]); writeTextureSlot(i + kMaxTextures / 2, flatNormal.view); }
    models.clear(); historyMap.clear();
    sceneValid = false; extraScenes.clear();
    gfx.destroy(dynBlas); gfx.destroy(tlas);
    destroyTargets(); gfx.destroy(overlayImg);
    gfx.destroySwapchain(true);
    if (viewWindow) { PostMessageA(viewWindow, WM_CLOSE, 0, 0); viewWindow = nullptr; }
}

// Highlights are not part of LT1 data either: specular strength and shininess per texture from name keywords.
static void SpecFromName(const char *name, float &spec, float &shininess) {
    static const struct { const char *key; float spec, shin; } table[] = {
        {"chrome", 0.9f, 80}, {"metal", 0.6f, 40}, {"steel", 0.65f, 48}, {"iron", 0.45f, 28}, {"pipe", 0.5f, 32}, {"rail", 0.5f, 32},
        {"glass", 0.8f, 96}, {"window", 0.7f, 80}, {"tile", 0.4f, 56}, {"marble", 0.5f, 64}, {"floor", 0.12f, 28}, {"lino", 0.3f, 40},
        {"water", 0.8f, 96}, {"ice", 0.7f, 64}, {"wet", 0.6f, 48}, {"plastic", 0.3f, 32}, {"leather", 0.15f, 20}, {"wood", 0.06f, 16},
        {"weapon", 0.5f, 40}, {"gun", 0.5f, 40}, {"_pv", 0.5f, 40}};
    char lower[96];
    size_t i = 0;
    for (; name[i] && i < sizeof(lower) - 1; i++) lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = 0;
    // no keyword, no highlight: most of the game's surfaces (and every character skin) are matte
    spec = 0.0f; shininess = 16;
    for (auto &t : table) if (strstr(lower, t.key)) { spec = t.spec; shininess = t.shin; return; }
}

// Reflectivity is not part of LT1 data: a keyword table on the texture's file name stands in for materials.
static float GlossFromName(const char *name) {
    static const struct { const char *key; float gloss; } table[] = {
        {"chrome", 0.9f}, {"mirror", 1.0f}, {"glass", 0.6f}, {"window", 0.5f}, {"metal", 0.35f}, {"steel", 0.4f}, {"marble", 0.4f},
        {"tile", 0.3f}, {"water", 0.6f}, {"ice", 0.5f}, {"wet", 0.5f}, {"polish", 0.4f}, {"lino", 0.3f}, {"chkr", 0.3f}, {"floor", 0.12f}};
    char lower[96];
    size_t i = 0;
    for (; name[i] && i < sizeof(lower) - 1; i++) lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = 0;
    for (auto &t : table) if (strstr(lower, t.key)) return t.gloss;
    return 0.0f;
}

void Renderer::onTexUpload(const lr::TexUploadCmd &c, const uint8_t *p) {
    if (c.id == 0 || c.id >= kTexLightmap || !c.width || !c.height || !c.nMips) return;
    const uint8_t *pal = p; p += 1024;
    bool alphaMask = (c.dtxFlags & lr::DTX_ALPHAMASKS) != 0, fullbright = (c.dtxFlags & lr::DTX_FULLBRITE) != 0;
    std::vector<uint8_t> px;
    for (uint32_t m = 0; m < c.nMips; m++) {
        uint32_t w = std::max(1u, c.width >> m), h = std::max(1u, c.height >> m), n = w * h;
        const uint8_t *idx = p; p += n;
        const uint8_t *alpha = alphaMask ? p : nullptr;
        if (alphaMask) p += (n + 1) / 2;
        size_t at = px.size(); px.resize(at + (size_t)n * 4);
        for (uint32_t i = 0; i < n; i++) {
            uint8_t ix = idx[i];
            const uint8_t *col = pal + ix * 4;      // a, r, g, b
            uint8_t a = 255;
            if (fullbright) a = ix >= 246 ? 255 : 0;        // d3d.ren: palette entries 246..255 are fullbright
            if (alphaMask) {
                a = alpha[i / 2]; if (i & 1) a >>= 4;
                a &= 15; a |= a << 4;
                if (ix >= 246) a = 255;
            }
            uint8_t *o = &px[at + (size_t)i * 4];
            o[0] = col[1]; o[1] = col[2]; o[2] = col[3]; o[3] = a;
        }
    }
    if (frameInFlight) { vkWaitForFences(gfx.dev, 1, &fence, VK_TRUE, UINT64_MAX); }
    if (textures[c.id].img) gfx.destroy(textures[c.id]);
    textures[c.id] = gfx.createImage(c.width, c.height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, c.nMips);
    gfx.uploadImage(textures[c.id], px.data(), c.nMips);
    writeTextureSlot(c.id, textures[c.id].view);
    {   // what the texture shows on average, and what its fullbright texels show (for emitters)
        if (texStats.size() < kMaxTextures) texStats.resize(kMaxTextures);
        TexStats ts = {};
        uint32_t n0 = c.width * c.height, nfb = 0, nSoft = 0, nClear = 0, nSolid = 0;
        double sum[3] = {}, fb[3] = {};
        const uint8_t *idx0 = pal + 1024;
        for (uint32_t i = 0; i < n0; i++) {
            const uint8_t *o = &px[(size_t)i * 4];
            for (int k = 0; k < 3; k++) sum[k] += o[k];
            if (fullbright && !alphaMask && idx0[i] >= 246) { for (int k = 0; k < 3; k++) fb[k] += o[k]; nfb++; }
            if (alphaMask) { if (o[3] <= 24) nClear++; else if (o[3] >= 232) nSolid++; else nSoft++; }
        }
        ts.softAlpha = (float)nSoft / n0; ts.clearAlpha = (float)nClear / n0; ts.solidAlpha = (float)nSolid / n0;
        for (int k = 0; k < 3; k++) { ts.avg[k] = (float)(sum[k] / (255.0 * n0)); ts.fbAvg[k] = nfb ? (float)(fb[k] / (255.0 * nfb)) : 0; }
        ts.fbCoverage = (float)nfb / n0;
        texStats[c.id] = ts;
    }
    GTexInfo ti = {alphaMask ? T_ALPHA : (fullbright ? T_FULLBRIGHT : 0u), GlossFromName(c.name), 0, 0};
    SpecFromName(c.name, ti.spec, ti.shininess);
    {   // Material rule from the texture's own content: a texture with little texel-to-texel luminance noise (tiles,
        // painted metal, polished stone) is a smooth surface and gets a highlight and a faint reflection; a noisy one
        // (concrete, dirt, brick) stays matte. Name keywords can only raise it. The shader scales this by how much the
        // surface faces up (floors more than walls) and ignores it on character models.
        double hf = 0; uint32_t n = 0;
        for (uint32_t y = 0; y + 1 < c.height; y++)
            for (uint32_t x = 0; x + 1 < c.width; x++) {
                const uint8_t *p0 = &px[((size_t)y * c.width + x) * 4], *p1 = p0 + 4, *p2 = p0 + (size_t)c.width * 4;
                auto lum = [](const uint8_t *p) { return (p[0] * 0.299 + p[1] * 0.587 + p[2] * 0.114) / 255.0; };
                double l0 = lum(p0);
                hf += fabs(lum(p1) - l0) + fabs(lum(p2) - l0); n += 2;
            }
        float noise = n ? (float)(hf / n) : 1.0f;
        float smooth = std::clamp(1.0f - noise / 0.055f, 0.0f, 1.0f);
        ti.spec = std::max(ti.spec, 0.06f + 0.5f * smooth * smooth);
        ti.shininess = std::max(ti.shininess, 14.0f + 60.0f * smooth);
        ti.gloss = std::max(ti.gloss, 0.22f * smooth * smooth);
        // material classes: by name, and metal also by content (smooth and nearly colourless = bare or painted metal)
        char lower[96]; size_t li = 0;
        for (; c.name[li] && li < sizeof(lower) - 1; li++) lower[li] = (char)tolower((unsigned char)c.name[li]);
        lower[li] = 0;
        auto has = [&](std::initializer_list<const char *> keys) { for (const char *k : keys) if (strstr(lower, k)) return true; return false; };
        const TexStats &st = texStats[c.id];
        float mx = std::max({st.avg[0], st.avg[1], st.avg[2]}), mn = std::min({st.avg[0], st.avg[1], st.avg[2]});
        float sat = mx > 0.001f ? (mx - mn) / mx : 0.0f;
        // (Blood II names its texture folders by colour family, not material, so the file names carry most of it)
        bool glass = has({"glass", "window", "mirror"});
        bool mineral = has({"crete", "conc", "brick", "stone", "insul", "plaster", "cement", "asphalt", "tile", "marble", "wood", "plank"});
        bool matte = !glass && has({"dirt", "grass", "ground", "mud", "sand", "snow", "rock", "carpet", "cloth", "rug", "gravel", "rubble", "trash", "flesh", "blood"});
        bool metal = !matte && !glass && !mineral && !has({"environmentmaps", "sprite", "interface", "skins"}) &&
                     (has({"metal", "steel", "iron", "chrome", "pipe", "rail", "grate", "vent", "girder", "duct", "mtl", "alum", "copper", "brass", "rust",
                           "subcar", "subinterior", "subdoor", "hull", "train", "tracks", "rivet", "dull", "locker", "tank", "boiler", "elevator"}) ||
                      (sat < 0.12f && smooth > 0.6f && mx > 0.15f && mx < 0.7f));
        if (matte) { ti.flags |= T_MATTE; ti.spec = std::min(ti.spec, 0.04f); ti.gloss = 0; }
        if (metal) { ti.flags |= T_METAL; ti.spec = std::max(ti.spec, 0.55f); ti.shininess = std::max(ti.shininess, 36.0f); ti.gloss = std::max(ti.gloss, 0.3f); }
        Log("tex %u %s %ux%u noise %.3f smooth %.2f sat %.2f avg %.2f alpha soft %.2f clear %.2f solid %.2f%s%s -> spec %.2f shin %.0f gloss %.2f", c.id, c.name, c.width, c.height, noise, smooth, sat, mx, st.softAlpha,
            st.clearAlpha, st.solidAlpha,
            metal ? " METAL" : "", matte ? " MATTE" : "", ti.spec, ti.shininess, ti.gloss);
    }
    // ---- generated normal map: the texture's luminance as a height field (slightly smoothed), central differences
    if (c.id < kMaxTextures / 2 - 1) {
        std::vector<uint8_t> npx;
        size_t src = 0;
        for (uint32_t m = 0; m < c.nMips; m++) {
            int w = (int)std::max(1u, c.width >> m), h = (int)std::max(1u, c.height >> m);
            std::vector<float> ht((size_t)w * h), sm((size_t)w * h);
            for (int i = 0; i < w * h; i++) { const uint8_t *o = &px[src + (size_t)i * 4]; ht[i] = (o[0] * 0.299f + o[1] * 0.587f + o[2] * 0.114f) / 255.0f; }
            auto at = [&](const std::vector<float> &v, int x, int y) { return v[(size_t)((y + h) % h) * w + ((x + w) % w)]; };
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
                sm[(size_t)y * w + x] = (at(ht, x, y) * 4 + at(ht, x - 1, y) + at(ht, x + 1, y) + at(ht, x, y - 1) + at(ht, x, y + 1)) / 8.0f;
            size_t o0 = npx.size(); npx.resize(o0 + (size_t)w * h * 4);
            float k = 2.2f / (float)(1 << m);       // keep the relief's world-space slope across mips
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                float dx = (at(sm, x + 1, y) - at(sm, x - 1, y)) * k, dy = (at(sm, x, y + 1) - at(sm, x, y - 1)) * k;
                float il = 1.0f / sqrtf(dx * dx + dy * dy + 1.0f);
                uint8_t *o = &npx[o0 + ((size_t)y * w + x) * 4];
                o[0] = (uint8_t)std::clamp(-dx * il * 127.5f + 127.5f, 0.0f, 255.0f); o[1] = (uint8_t)std::clamp(-dy * il * 127.5f + 127.5f, 0.0f, 255.0f);
                o[2] = (uint8_t)std::clamp(il * 127.5f + 127.5f, 0.0f, 255.0f); o[3] = (uint8_t)(sm[(size_t)y * w + x] * 255.0f);
            }
            src += (size_t)w * h * 4;
        }
        if (normalMaps[c.id].img) gfx.destroy(normalMaps[c.id]);
        normalMaps[c.id] = gfx.createImage(c.width, c.height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, c.nMips);
        gfx.uploadImage(normalMaps[c.id], npx.data(), c.nMips);
        writeTextureSlot(c.id + kMaxTextures / 2, normalMaps[c.id].view);
    }
    texInfoCpu[c.id] = ti;
    ((GTexInfo *)texInfoBuf.mapped)[c.id] = ti;
}

void Renderer::onTexFree(uint32_t id) {
    if (id == 0 || id >= kTexLightmap || !textures[id].img) return;
    if (frameInFlight) { vkWaitForFences(gfx.dev, 1, &fence, VK_TRUE, UINT64_MAX); }
    writeTextureSlot(id, white.view);
    gfx.destroy(textures[id]);
    if (id < kMaxTextures / 2 - 1 && normalMaps[id].img) { writeTextureSlot(id + kMaxTextures / 2, flatNormal.view); gfx.destroy(normalMaps[id]); }
    ((GTexInfo *)texInfoBuf.mapped)[id] = texInfoCpu[id] = GTexInfo{0, 0, 0, 0};
}

void Renderer::onWorldBegin(const lr::WorldBeginCmd &c, const uint8_t *p) {
    onWorldEnd();
    const lr::WorldBsp *wb = (const lr::WorldBsp *)p; p += c.nBsps * sizeof(lr::WorldBsp);
    const lr::WorldSurface *ws = (const lr::WorldSurface *)p; p += c.nSurfaces * sizeof(lr::WorldSurface);
    const lr::WorldPoly *wp = (const lr::WorldPoly *)p; p += c.nPolys * sizeof(lr::WorldPoly);
    const lr::WorldVertex *wv = (const lr::WorldVertex *)p; p += c.nVertices * sizeof(lr::WorldVertex);
    const uint8_t *lm = p; p += c.lightmapBytes;
    const uint8_t *gridBytes = p;
    worldName = c.worldName; worldContext = c.contextId;
    if (c.nVertices > kStaticVerts || c.nSurfaces > kMaxSurfaces) { Log("world too large: %u vertices, %u surfaces", c.nVertices, c.nSurfaces); return; }

    // ---- lightmap atlas: shelf packing, one texel of replicated border around every lightmap
    const uint32_t AW = 2048;
    struct Slot { uint32_t x, y; };
    std::vector<Slot> slots(c.nPolys);
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < c.nPolys; i++) if (wp[i].lmOffset != 0xffffffffu) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return wp[a].lmHeight > wp[b].lmHeight; });
    uint32_t x = 0, y = 0, rowH = 0;
    for (uint32_t i : order) {
        uint32_t w = wp[i].lmWidth + 2, h = wp[i].lmHeight + 2;
        if (x + w > AW) { x = 0; y += rowH; rowH = 0; }
        slots[i] = {x + 1, y + 1};
        x += w; rowH = std::max(rowH, h);
    }
    uint32_t minTexel[3] = {31, 63, 31};
    for (uint32_t i : order) {
        const uint16_t *src = (const uint16_t *)(lm + wp[i].lmOffset);
        for (uint32_t t = 0, n = (uint32_t)wp[i].lmWidth * wp[i].lmHeight; t < n; t++) {
            minTexel[0] = std::min<uint32_t>(minTexel[0], src[t] >> 11); minTexel[1] = std::min<uint32_t>(minTexel[1], (src[t] >> 5) & 63);
            minTexel[2] = std::min<uint32_t>(minTexel[2], src[t] & 31);
        }
    }
    levelAmbient[0] = order.empty() ? 0 : minTexel[0] / 31.0f; levelAmbient[1] = order.empty() ? 0 : minTexel[1] / 63.0f;
    levelAmbient[2] = order.empty() ? 0 : minTexel[2] / 31.0f;
    uint32_t AH = 64;
    while (AH < y + rowH) AH *= 2;
    std::vector<uint8_t> atlas((size_t)AW * AH * 4, 255);
    for (uint32_t i : order) {
        const uint16_t *src = (const uint16_t *)(lm + wp[i].lmOffset);
        int w = wp[i].lmWidth, h = wp[i].lmHeight;
        for (int ty = -1; ty <= h; ty++)
            for (int tx = -1; tx <= w; tx++) {
                uint16_t v = src[std::clamp(ty, 0, h - 1) * w + std::clamp(tx, 0, w - 1)];
                uint8_t *o = &atlas[((size_t)(slots[i].y + ty) * AW + slots[i].x + tx) * 4];
                uint32_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
                o[0] = (uint8_t)((r << 3) | (r >> 2)); o[1] = (uint8_t)((g << 2) | (g >> 4)); o[2] = (uint8_t)((b << 3) | (b >> 2)); o[3] = 255;
            }
    }
    if (frameInFlight) { vkWaitForFences(gfx.dev, 1, &fence, VK_TRUE, UINT64_MAX); }
    textures[kTexLightmap] = gfx.createImage(AW, AH, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    gfx.uploadImage(textures[kTexLightmap], atlas.data(), 1);
    writeTextureSlot(kTexLightmap, textures[kTexLightmap].view);

    // ---- vertices (one per protocol vertex, same order: UV updates address them by poly)
    GVertex *gv = (GVertex *)vbuf.mapped;
    polyFirstVertex.resize(c.nPolys); polyVertexCount.resize(c.nPolys);
    for (uint32_t i = 0; i < c.nPolys; i++) {
        const lr::WorldPoly &poly = wp[i];
        const lr::WorldSurface &sf = ws[poly.surface];
        polyFirstVertex[i] = poly.firstVertex; polyVertexCount[i] = poly.nVertices;
        for (uint32_t k = 0; k < poly.nVertices; k++) {
            const lr::WorldVertex &s = wv[poly.firstVertex + k];
            GVertex &d = gv[poly.firstVertex + k];
            d.pos[0] = s.pos.x; d.pos[1] = s.pos.y; d.pos[2] = s.pos.z;
            d.color = (uint32_t)s.color[2] | ((uint32_t)s.color[1] << 8) | ((uint32_t)s.color[0] << 16) | 0xff000000u;
            d.uv[0] = s.u; d.uv[1] = s.v;
            if (poly.lmOffset != 0xffffffffu) { d.lm[0] = (slots[i].x + s.lmu) / AW; d.lm[1] = (slots[i].y + s.lmv) / AH; }
            else d.lm[0] = d.lm[1] = 0;
            d.n[0] = sf.normal.x; d.n[1] = sf.normal.y; d.n[2] = sf.normal.z; d.pad = 0;
        }
    }
    staticVerts = c.nVertices;

    // ---- surfaces, indices, per-triangle surface table; per BSP: opaque range then sky-portal range
    surfaces.resize(c.nSurfaces);
    for (uint32_t i = 0; i < c.nSurfaces; i++) surfaces[i] = GSurface{0, ws[i].flags, 0, 0};
    uint32_t *gi = (uint32_t *)ibuf.mapped, *tri = (uint32_t *)triBuf.mapped;
    uint32_t ni = 0;
    // "bright" polygons: the editor's bright surface tag (flag 0x20, never read by d3d.ren) or a lightmap the
    // preprocessor saturated completely. They are self-lit; in the main world they also become emitters.
    emitPolys.clear();
    std::vector<uint8_t> polyBright(c.nPolys, 0);
    uint32_t nFlagBright = 0, nBakedBright = 0, nVertexBright = 0;
    for (uint32_t i = 0; i < c.nPolys; i++) {
        const lr::WorldPoly &poly = wp[i];
        bool flag = (ws[poly.surface].flags & lr::SURF_BRIGHT) != 0, baked = false;
        if (poly.lmOffset != 0xffffffffu) {
            const uint16_t *src = (const uint16_t *)(lm + poly.lmOffset);
            baked = true;
            for (uint32_t t = 0, n = (uint32_t)poly.lmWidth * poly.lmHeight; t < n && baked; t++)
                baked = (src[t] >> 11) >= 29 && ((src[t] >> 5) & 63) >= 58 && (src[t] & 31) >= 29;
        }
        // no lightmap at all and fully bright baked vertex colours: the preprocessor's other way of writing "bright"
        bool vertexBright = false;
        if (poly.lmOffset == 0xffffffffu && !(ws[poly.surface].flags & lr::SURF_LIGHTMAP) && poly.nVertices >= 3) {
            vertexBright = true;
            for (uint32_t k = 0; k < poly.nVertices && vertexBright; k++) {
                const uint8_t *col = wv[poly.firstVertex + k].color;
                vertexBright = col[0] >= 235 && col[1] >= 235 && col[2] >= 235;
            }
        }
        nVertexBright += vertexBright && !flag;
        polyBright[i] = flag || baked || vertexBright;
        nFlagBright += flag; nBakedBright += baked && !flag;
    }
    {
        uint32_t nLm = 0, nNoLm = 0, hist[16] = {};
        for (uint32_t i = 0; i < c.nSurfaces; i++) for (int b = 0; b < 16; b++) if (ws[i].flags & (1u << b)) hist[b]++;
        for (uint32_t i = 0; i < c.nPolys; i++) (wp[i].lmOffset != 0xffffffffu ? nLm : nNoLm)++;
        Log("surface flag counts (bit:count) 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u 12:%u 14:%u 15:%u | polys with lightmap %u, without %u",
            hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[12], hist[14], hist[15], nLm, nNoLm);
    }
    // every BSP: world models (signs, panels) sit where the level file put them; a moved door keeps its emitter in place
    for (uint32_t i = 0; i < c.nPolys; i++) {
        const lr::WorldPoly &poly = wp[i];
        uint32_t sflags = ws[poly.surface].flags;
        if (poly.nVertices < 3 || (sflags & (lr::SURF_INVISIBLE | lr::SURF_NONEXISTANT | lr::SURF_SKY | lr::SURF_TRANSPARENT))) continue;
        EmitPoly e = {};
        V3 p0{wv[poly.firstVertex].pos.x, wv[poly.firstVertex].pos.y, wv[poly.firstVertex].pos.z};
        for (uint32_t k = 0; k < poly.nVertices; k++) {
            V3 q{wv[poly.firstVertex + k].pos.x, wv[poly.firstVertex + k].pos.y, wv[poly.firstVertex + k].pos.z};
            e.center = e.center + q * (1.0f / poly.nVertices);
            if (k >= 2) {
                V3 q1{wv[poly.firstVertex + k - 1].pos.x, wv[poly.firstVertex + k - 1].pos.y, wv[poly.firstVertex + k - 1].pos.z};
                e.area += 0.5f * length(cross(q1 - p0, q - p0));
            }
        }
        e.normal = V3{ws[poly.surface].normal.x, ws[poly.surface].normal.y, ws[poly.surface].normal.z};
        e.surface = poly.surface; e.bright = polyBright[i] != 0;
        e.hasLm = poly.lmOffset != 0xffffffffu;
        if (e.hasLm) {
            const uint16_t *src = (const uint16_t *)(lm + poly.lmOffset);
            uint32_t n = (uint32_t)poly.lmWidth * poly.lmHeight;
            double acc[3] = {};
            for (uint32_t t = 0; t < n; t++) { acc[0] += (src[t] >> 11) / 31.0; acc[1] += ((src[t] >> 5) & 63) / 63.0; acc[2] += (src[t] & 31) / 31.0; }
            for (int k = 0; k < 3; k++) e.lm[k] = (float)(acc[k] / n);
        }
        emitPolys.push_back(e);
    }
    Log("bright polygons: %u by surface flag, %u by saturated lightmap, %u by bright vertex colours without a lightmap", nFlagBright, nBakedBright, nVertexBright);
    bsps.assign(c.nBsps, BspGpu());
    for (uint32_t b = 0; b < c.nBsps; b++) {
        BspGpu &g = bsps[b];
        g.firstSurface = wb[b].firstSurface; g.nSurfaces = wb[b].nSurfaces;
        g.bmin = V3{1e30f, 1e30f, 1e30f}; g.bmax = V3{-1e30f, -1e30f, -1e30f};
        if (wb[b].nPolys && b > 0) g.translucent = (ws[wp[wb[b].firstPoly].surface].flags & lr::SURF_TRANSPARENT) != 0;
        for (int pass = 0; pass < 2; pass++) {
            uint32_t first = ni;
            for (uint32_t i = wb[b].firstPoly; i < wb[b].firstPoly + wb[b].nPolys; i++) {
                const lr::WorldPoly &poly = wp[i];
                uint32_t sflags = ws[poly.surface].flags;
                if (sflags & (lr::SURF_INVISIBLE | lr::SURF_NONEXISTANT)) continue;
                bool sky = (sflags & lr::SURF_SKY) != 0;
                if (sky != (pass == 1) || poly.nVertices < 3) continue;
                if (ni + (poly.nVertices - 2) * 3 > kStaticIndices) break;
                for (uint32_t k = 2; k < poly.nVertices; k++) {
                    tri[ni / 3] = poly.surface | (polyBright[i] ? 0x80000000u : 0u);
                    gi[ni++] = poly.firstVertex; gi[ni++] = poly.firstVertex + k - 1; gi[ni++] = poly.firstVertex + k;
                }
                for (uint32_t k = 0; k < poly.nVertices; k++) {
                    V3 q{wv[poly.firstVertex + k].pos.x, wv[poly.firstVertex + k].pos.y, wv[poly.firstVertex + k].pos.z};
                    g.bmin = vmin(g.bmin, q); g.bmax = vmax(g.bmax, q);
                }
            }
            if (pass == 0) { g.firstIndex = first; g.nTris = (ni - first) / 3; g.triBase = first / 3; }
            else { g.skyFirstIndex = first; g.skyTris = (ni - first) / 3; g.skyTriBase = first / 3; }
        }
    }
    staticIndices = ni; staticTris = ni / 3;

    // ---- static acceleration structures: one BLAS per BSP (world models are instanced with their transform)
    if (gfx.rt) {
        for (BspGpu &g : bsps) {
            if (!g.nTris) continue;
            VkCommandBuffer c2 = gfx.beginOnce();
            g.blas = gfx.buildBlas(c2, vbuf.addr, sizeof(GVertex), staticVerts ? staticVerts - 1 : 0, ibuf.addr + (VkDeviceAddress)g.firstIndex * 4,
                                   g.nTris, false, scratch, !g.translucent);
            gfx.endOnce(c2);
        }
    }
    worldMin = V3{c.worldMin.x, c.worldMin.y, c.worldMin.z}; worldMax = V3{c.worldMax.x, c.worldMax.y, c.worldMax.z};
    grid = c.grid;
    gridData.assign(gridBytes, gridBytes + (size_t)c.grid.nSamples * 4);
    haveWorld = true;
    historyMap.clear(); historyValid = false; prevValid = false; loggedKeys.clear();
    loadLamps(c.worldFile, c.rezSources);
    calibrateLamps();
    buildLampGrid();
    emittersBuilt = false;
    Log("world '%s': %u bsps, %u polys, %u vertices, %u triangles, lightmap atlas %ux%u (%zu lightmaps)", c.worldName, c.nBsps, c.nPolys,
        c.nVertices, staticTris, AW, AH, order.size());
}

void Renderer::onWorldEnd() {
    if (!haveWorld && bsps.empty()) return;
    vkDeviceWaitIdle(gfx.dev);
    frameInFlight = false;
    for (BspGpu &g : bsps) gfx.destroy(g.blas);
    bsps.clear(); surfaces.clear(); polyFirstVertex.clear(); polyVertexCount.clear();
    if (textures[kTexLightmap].img) { writeTextureSlot(kTexLightmap, white.view); gfx.destroy(textures[kTexLightmap]); }
    staticVerts = staticIndices = staticTris = 0;
    haveWorld = false; sceneValid = false; gridData.clear(); historyMap.clear(); lamps.clear(); lampGrid = GLampGrid();
}

void Renderer::onModelDef(const lr::ModelDefCmd &c, const uint8_t *p) {
    ModelDef &m = models[c.id];
    m.nNodes = c.nNodes; m.name = c.name;
    m.verts.assign((const lr::ModelVertex *)p, (const lr::ModelVertex *)p + c.nVertices);
    p += c.nVertices * sizeof(lr::ModelVertex);
    m.faces.assign((const lr::ModelFace *)p, (const lr::ModelFace *)p + c.nFaces);
}
void Renderer::onModelFree(uint32_t id) { models.erase(id); }

void Renderer::onScene(const lr::SceneCmd &c, uint32_t size) {
    const uint8_t *p = (const uint8_t *)&c;
    if (c.drawMode == 1 && !sceneValid) { sceneCopy.assign(p, p + size); sceneValid = true; }
    else extraScenes.emplace_back(p, p + size);
}

void Renderer::onOverlay(const lr::OverlayCmd &c, const uint8_t *p) {
    overlayEmpty = c.empty != 0;
    if (c.empty || c.width != modeW || c.height != modeH || !overlayImg.img) return;
    size_t n = (size_t)c.width * c.height;
    const uint16_t *px = (const uint16_t *)p; const uint8_t *mask = p + n * 2;
    static std::vector<uint8_t> rgba;
    rgba.resize(n * 4);
    for (size_t i = 0; i < n; i++) {
        uint32_t v = px[i], r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        rgba[i * 4] = (uint8_t)((r << 3) | (r >> 2)); rgba[i * 4 + 1] = (uint8_t)((g << 2) | (g >> 4));
        rgba[i * 4 + 2] = (uint8_t)((b << 3) | (b >> 2)); rgba[i * 4 + 3] = mask[i];
    }
    if (frameInFlight) { vkWaitForFences(gfx.dev, 1, &fence, VK_TRUE, UINT64_MAX); }
    gfx.uploadImage(overlayImg, rgba.data(), 1);
}

void Renderer::onScreenshot(const lr::ScreenshotCmd &c) { shotPath = c.path; shotFormat = c.format; }

void Renderer::logOnce(uint32_t key, const char *fmt, ...) {
    if (loggedKeys.size() >= 400 || loggedKeys[key]++) return;
    char buf[512];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    Log("%s", buf);
}

// ------------------------------------------------------------------ frame building
uint32_t Renderer::pushInstance(const GInstance &i) {
    if (instances.size() >= kMaxInstances) return 0;
    instances.push_back(i); instOwner.push_back(curOwner);
    return (uint32_t)instances.size() - 1;
}
void Renderer::pushDraw(Group g, Pipe p, uint32_t instance, uint32_t firstIndex, uint32_t indexCount, uint8_t view, float sortKey) {
    if (indexCount) draws.push_back(Draw{instance, firstIndex, indexCount, (uint8_t)p, view, (uint8_t)g, sortKey});
}
GVertex *Renderer::allocDyn(uint32_t n, uint32_t &first) {
    if (dynVerts + n > kDynVerts) return nullptr;
    first = kStaticVerts + dynHalf * kDynVerts + dynVerts;
    dynVerts += n;
    return (GVertex *)vbuf.mapped + first;
}
uint32_t *Renderer::allocDynIndices(uint32_t n, uint32_t &first) {
    if (dynIndices + n > kDynIndices) return nullptr;
    first = kStaticIndices + dynHalf * kDynIndices + dynIndices;
    dynIndices += n;
    return (uint32_t *)ibuf.mapped + first;
}

V3 Renderer::sampleGrid(V3 pos) const {
    if (gridData.empty() || grid.dims[0] < 2 || grid.dims[1] < 2 || grid.dims[2] < 2) return V3{0, 0, 0};
    float f[3]; int c[3];
    const float p[3] = {pos.x, pos.y, pos.z}, start[3] = {grid.lookupStart.x, grid.lookupStart.y, grid.lookupStart.z},
                bs[3] = {grid.blockSize.x, grid.blockSize.y, grid.blockSize.z};
    for (int a = 0; a < 3; a++) {
        float s = (p[a] - start[a]) / (bs[a] != 0 ? bs[a] : 1);
        int ci = (int)s;
        ci = std::clamp(ci, 0, grid.dims[a] - 2);
        c[a] = ci; f[a] = std::clamp(s - floorf(s), 0.0f, 1.0f);
    }
    int dx = 1, dy = grid.dims[0], dz = grid.dims[0] * grid.dims[1];
    const uint8_t *base = gridData.data() + ((size_t)c[0] + (size_t)c[1] * dy + (size_t)c[2] * dz) * 4;
    float out[3];
    for (int ch = 0; ch < 3; ch++) {
        auto g = [&](int ox, int oy, int oz) { return (float)base[(ox * dx + oy * dy + oz * dz) * 4 + (2 - ch)]; };   // stored b, g, r
        float x00 = g(0, 0, 0) * (1 - f[0]) + g(1, 0, 0) * f[0], x10 = g(0, 1, 0) * (1 - f[0]) + g(1, 1, 0) * f[0];
        float x01 = g(0, 0, 1) * (1 - f[0]) + g(1, 0, 1) * f[0], x11 = g(0, 1, 1) * (1 - f[0]) + g(1, 1, 1) * f[0];
        float y0 = x00 * (1 - f[1]) + x10 * f[1], y1 = x01 * (1 - f[1]) + x11 * f[1];
        out[ch] = y0 * (1 - f[2]) + y1 * f[2];
    }
    return V3{out[0], out[1], out[2]};
}

static void setRows(float dst[3][4], const M4 &m) { memcpy(dst, m.m, 48); }

static M4 objectMatrix(const lr::ObjectRecord &o) {
    // d3d.ren: quat * diag(scale), then the third column negated (handedness), then the position.
    float q[4] = {o.rot.x, o.rot.y, o.rot.z, o.rot.w};
    for (int i = 0; i < 4; i++) if (!(q[i] >= -100000.0f && q[i] <= 100000.0f)) q[i] = i == 3 ? 1.0f : 0.0f;
    M4 m = quatToMatrix(q[0], q[1], q[2], q[3]);
    const float s[3] = {o.scale.x, o.scale.y, o.scale.z};
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m.m[r][c] *= s[c];
    for (int r = 0; r < 3; r++) m.m[r][2] = -m.m[r][2];
    m.m[0][3] = o.pos.x; m.m[1][3] = o.pos.y; m.m[2][3] = o.pos.z;
    return m;
}

void Renderer::addModel(const lr::SceneCmd &sc, const lr::ObjectRecord &o, const uint8_t *payload) {
    if (settings.drawModels == 0) return;
    const lr::ModelPayload &mp = *(const lr::ModelPayload *)payload;
    auto it = models.find(mp.modelId);
    if (it == models.end()) return;
    const ModelDef &def = it->second;
    const float *nodeM = (const float *)(payload + sizeof(lr::ModelPayload));
    const lr::Vec3 *deform = mp.nDeform ? (const lr::Vec3 *)(nodeM + mp.nNodes * 12) : nullptr;
    const uint8_t *hiddenList = (const uint8_t *)(nodeM + mp.nNodes * 12) + mp.nDeform * 12;
    bool hidden[256] = {};
    for (uint32_t i = 0; i < mp.nHidden; i++) hidden[hiddenList[i]] = true;

    M4 obj = objectMatrix(o);
    static std::vector<M4> nodes;
    nodes.resize(mp.nNodes);
    for (uint32_t i = 0; i < mp.nNodes; i++) nodes[i] = obj * fromM34(nodeM + i * 12);

    uint32_t nFaces = 0;
    for (auto &f : def.faces) if (!hidden[f.nodeSelector & 255]) nFaces++;
    logOnce(o.key, "model obj %08x '%s' faces %u hidden %u nodes %u rgba %u,%u,%u,%u flags %08x role %u skin %u amb %.0f,%.0f,%.0f pos %.0f,%.0f,%.0f",
            o.key, def.name.c_str(), nFaces, mp.nHidden, mp.nNodes, o.rgba[0], o.rgba[1], o.rgba[2], o.rgba[3], o.flags, o.role,
            mp.skinTex, mp.lightAdd.x, mp.lightAdd.y, mp.lightAdd.z, o.pos.x, o.pos.y, o.pos.z);
    if (!nFaces) return;
    bool sky = (o.role & lr::OBJ_SKY) != 0, close = (o.flags & lr::FLAG_REALLYCLOSE) != 0 || (o.role & lr::OBJ_VIEWLIST) != 0;
    // d3d.ren "chrome": a model with FLAG_ENVIRONMENTMAP is drawn with the environment map first and its skin blended
    // over it by the object alpha, so (1 - alpha) is how much reflection shows. Such a model is solid, not translucent.
    bool chrome = (o.flags & lr::FLAG_ENVIRONMENTMAP) != 0 && !sky;
    bool translucent = o.rgba[3] != 255 && !chrome;
    bool caster = gfx.rt && !sky && !close && !translucent;

    uint32_t firstVertex, firstIndex;
    GVertex *v = allocDyn(nFaces * 3, firstVertex);
    uint32_t *idx = v ? allocDynIndices(nFaces * 3, firstIndex) : nullptr;
    if (!v || !idx) return;
    // model UVs are normalised; the shaders work in texels (like world surfaces)
    const Image &skin = textures[mp.skinTex < kMaxTextures ? mp.skinTex : 0];
    float texW = skin.img ? (float)skin.width : 1.0f, texH = skin.img ? (float)skin.height : 1.0f;
    uint32_t n = 0;
    for (auto &f : def.faces) {
        if (hidden[f.nodeSelector & 255]) continue;
        for (int k = 0; k < 3; k++) {
            const lr::ModelVertex &s = def.verts[f.v[k]];
            const M4 &m = nodes[s.node < mp.nNodes ? s.node : 0];
            V3 p = deform && f.v[k] < mp.nDeform ? V3{deform[f.v[k]].x, deform[f.v[k]].y, deform[f.v[k]].z} : V3{s.pos.x, s.pos.y, s.pos.z};
            V3 wp = m.point(p), wn = normalize(m.dir(V3{s.normal[0] / 127.0f, s.normal[1] / 127.0f, s.normal[2] / 127.0f}));
            GVertex &d = v[n];
            d.pos[0] = wp.x; d.pos[1] = wp.y; d.pos[2] = wp.z; d.color = 0xffffffffu;
            d.uv[0] = f.uv[k * 2] * texW; d.uv[1] = f.uv[k * 2 + 1] * texH; d.lm[0] = d.lm[1] = 0;
            d.n[0] = wn.x; d.n[1] = wn.y; d.n[2] = wn.z; d.pad = 0;
            idx[n] = firstVertex + n;
            n++;
        }
    }

    // ambient from the shim (object colour + ModelAdd, light scale, model hook); directional from the light grid
    GInstance gi = {};
    M4 id = M4::identity();
    setRows(gi.r, id); setRows(gi.p, id);
    gi.firstIndex = firstIndex; gi.tex = mp.skinTex; gi.flags = I_MODEL;
    float amb[3] = {mp.lightAdd.x, mp.lightAdd.y, mp.lightAdd.z};
    const float scale[3] = {sc.lightScale.x, sc.lightScale.y, sc.lightScale.z}, dirAdd[3] = {sc.modelDirAdd.x, sc.modelDirAdd.y, sc.modelDirAdd.z};
    V3 g = (o.flags & lr::FLAG_NOLIGHT) ? V3{0, 0, 0} : sampleGrid(V3{o.pos.x, o.pos.y, o.pos.z});
    const float gridv[3] = {g.x, g.y, g.z};
    for (int i = 0; i < 3; i++) {
        float a = std::clamp(amb[i], 0.0f, 255.0f);
        float cap = 255.0f - a;
        float dir = (o.flags & lr::FLAG_NOLIGHT) ? cap * scale[i] : std::min((gridv[i] + dirAdd[i]) * scale[i], cap);
        gi.color[i] = a / 255.0f; gi.light[i] = std::max(dir, 0.0f) / 255.0f;
    }
    gi.color[3] = chrome ? 1.0f : o.rgba[3] / 255.0f;
    if (!(mp.hookFlags & lr::MHF_USETEXTURE) || !mp.skinTex) gi.flags |= I_NOTEXTURE;
    // no alpha test: d3d.ren never enables it. A solid model ignores its skin's alpha mask; a translucent one blends by it.
    // the shader derives reflectivity per texel: 1 - skin alpha x object alpha (light.a carries the object alpha)
    if (chrome) { gi.flags |= I_CHROME; gi.light[3] = o.rgba[3] / 255.0f; }
    if (close) gi.flags |= I_CLOSE;
    if (sky) gi.flags |= I_SKY | I_UNLIT;

    // previous frame's vertices of the same object, for motion vectors
    auto h = historyMap.find(o.key);
    if (h != historyMap.end() && h->second.frame + 1 == frameIndex && h->second.nVertices == n)
        gi.prevVertexDelta = (int32_t)h->second.firstVertex - (int32_t)firstVertex;
    History &hist = historyMap[o.key];
    hist.firstVertex = firstVertex; hist.nVertices = n; hist.frame = frameIndex;

    uint32_t inst = pushInstance(gi);
    Group grp = sky ? G_SKY : close ? G_CLOSE : translucent ? G_TRANS_MODELS : G_SOLID_MODELS;
    Pipe pipe = sky ? P_NOZ : translucent ? P_BLEND : P_OPAQUE;
    pushDraw(grp, pipe, inst, firstIndex, n, sky ? 1 : close ? 2 : 0);
    if (caster) {
        // dynamic BLAS: casters are appended in a second index range rebuilt at the end of scene building
        for (uint32_t t = 0; t < n / 3; t++) casterTris.push_back({firstIndex + t * 3, inst});
    }
}

void Renderer::addObject(const lr::SceneCmd &sc, const lr::ObjectRecord &o) {
    const uint8_t *payload = (const uint8_t *)(&o + 1);
    curOwner = ((uint64_t)o.type << 32) | o.key;
    switch (o.type) {
    case lr::OT_MODEL: addModel(sc, o, payload); break;
    case lr::OT_WORLDMODEL: case lr::OT_CONTAINER: {
        if (settings.drawWorldModels == 0) break;
        const lr::WorldModelPayload &wm = *(const lr::WorldModelPayload *)payload;
        if (wm.bsp >= bsps.size()) break;
        const BspGpu &b = bsps[wm.bsp];
        bool sky = (o.role & lr::OBJ_SKY) != 0;
        M4 xf; memcpy(xf.m, wm.transform, 64);
        GInstance gi = {};
        setRows(gi.r, xf);
        auto h = historyMap.find(o.key);
        setRows(gi.p, (h != historyMap.end() && h->second.frame + 1 == frameIndex) ? h->second.world : xf);
        History &hist = historyMap[o.key]; hist.world = xf; hist.frame = frameIndex; hist.nVertices = 0;
        // d3d.ren: translucent iff the first surface has the translucent flag (object alpha is not consulted for the
        // choice); translucent and sky world models are one pre-lit Gouraud pass, two-sided, without lightmaps.
        bool translucent = b.translucent;
        // Masked walls (any surface texture with an alpha mask) are split PER TEXEL, with no guess about what the
        // texture depicts: solid texels are opaque matter and are drawn in the opaque pass (alpha test, depth, the full
        // ray-traced lighting, lit from either side); partially transparent texels are drawn in the blended pass. A
        // fence comes out as solid bars with a thin blended fringe, a glass pane has no solid texels and stays blended.
        // The original blends the whole wall without depth in list order, so a far one, or a decal behind it, can
        // cover a near one.
        bool masked = false;
        if (translucent && !sky)
            for (uint32_t si = b.firstSurface; si < b.firstSurface + b.nSurfaces && si < surfaces.size() && !masked; si++)
                masked = (texInfoCpu[surfaces[si].tex < kMaxTextures ? surfaces[si].tex : 0].flags & T_ALPHA) != 0;
        bool split = masked && o.rgba[3] >= 250;
        gi.firstIndex = b.firstIndex; gi.triBase = b.triBase;
        gi.color[0] = gi.color[1] = gi.color[2] = 1; gi.color[3] = translucent ? o.rgba[3] / 255.0f : 1.0f;
        V3 centre = xf.point((b.bmin + b.bmax) * 0.5f);
        float dist = length(centre - camPos);
        uint32_t inst;
        if (split) {
            gi.flags = I_WORLD | I_PRELIT | I_TWOSIDED | I_ALPHATEST;
            inst = pushInstance(gi);
            pushDraw(G_SOLID_WORLDMODELS, P_OPAQUE, inst, b.firstIndex, b.nTris * 3, 0);
            GInstance soft = gi;
            soft.flags = I_WORLD | I_PRELIT | I_TWOSIDED | I_BLEND | I_SOFT;
            pushDraw(G_TRANS_WORLDMODELS, P_BLEND, pushInstance(soft), b.firstIndex, b.nTris * 3, 0, dist);
        } else {
            // sky: always front faces only (seen from the sky camera), even when translucent (d3d.ren 0x1b9c0)
            gi.flags = I_WORLD | (sky ? (I_SKY | I_PRELIT) : 0) | (translucent ? (I_PRELIT | I_BLEND | (sky ? 0 : I_TWOSIDED)) : 0);
            inst = pushInstance(gi);
            pushDraw(sky ? G_SKY : translucent ? G_TRANS_WORLDMODELS : G_SOLID_WORLDMODELS, translucent ? P_BLEND : P_OPAQUE,
                     inst, b.firstIndex, b.nTris * 3, sky ? 1 : 0, (translucent && !sky) ? dist : 0.0f);
        }
        if (gfx.rt && !sky && (!translucent || masked) && b.blas.as && tlasInstances.size() < kMaxTlas) {
            VkAccelerationStructureInstanceKHR ti = {};
            memcpy(ti.transform.matrix, xf.m, 48);
            ti.instanceCustomIndex = inst; ti.mask = translucent ? 0x08 : 0x04;     // (glass never passes the rays' 50% alpha test: no shadow) ti.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            ti.accelerationStructureReference = b.blas.addr;
            tlasInstances.push_back(ti);
        }
        {
            V3 ext = b.bmax - b.bmin;
            logOnce(o.key, "worldmodel obj %08x bsp %u tris %u size %.0f,%.0f,%.0f pos %.0f,%.0f,%.0f alpha %u flags %08x role %u%s", o.key, wm.bsp, b.nTris,
                    ext.x, ext.y, ext.z, xf.m[0][3], xf.m[1][3], xf.m[2][3], o.rgba[3], o.flags, o.role, translucent ? " translucent" : "");
        }
        break;
    }
    case lr::OT_LIGHT: {
        if (settings.dynamicLight == 0 || (o.flags & lr::FLAG_FOGLIGHT) || (o.role & lr::OBJ_SKY) || lights.size() >= kMaxLights) break;
        const lr::LightPayload &lp = *(const lr::LightPayload *)payload;
        if (lp.radius <= 1) break;
        GLight l = {{o.pos.x, o.pos.y, o.pos.z, lp.radius},
                    {(2.0f * o.rgba[0] - 255.0f) / 255.0f, (2.0f * o.rgba[1] - 255.0f) / 255.0f, (2.0f * o.rgba[2] - 255.0f) / 255.0f, 0}};
        lights.push_back(l);
        break;
    }
    case lr::OT_SPRITE: addSprite(sc, o, payload); break;
    case lr::OT_PARTICLESYSTEM: addParticles(sc, o, payload); break;
    case lr::OT_POLYGRID: addPolyGrid(sc, o, payload); break;
    case lr::OT_LINESYSTEM: addLines(sc, o, payload); break;
    default: break;
    }
}

void Renderer::addWorldDraws(const lr::SceneCmd &sc) {
    if (!haveWorld || bsps.empty() || settings.drawWorld == 0) return;
    const BspGpu &b = bsps[0];
    GInstance gi = {};
    M4 id = M4::identity();
    setRows(gi.r, id); setRows(gi.p, id);
    gi.firstIndex = b.firstIndex; gi.triBase = b.triBase; gi.flags = I_WORLD;
    gi.color[0] = gi.color[1] = gi.color[2] = gi.color[3] = 1;
    uint32_t inst = pushInstance(gi);
    pushDraw(G_WORLD, P_OPAQUE, inst, b.firstIndex, b.nTris * 3);
    if (gfx.rt && b.blas.as) {
        VkAccelerationStructureInstanceKHR ti = {};
        memcpy(ti.transform.matrix, id.m, 48);
        ti.instanceCustomIndex = inst; ti.mask = 0x01; ti.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        ti.accelerationStructureReference = b.blas.addr;
        tlasInstances.push_back(ti);
    }
    if (b.skyTris) {
        gi.firstIndex = b.skyFirstIndex; gi.triBase = b.skyTriBase;
        pushDraw(G_WORLD, P_SKYMASK, pushInstance(gi), b.skyFirstIndex, b.skyTris * 3);
    }
}

void Renderer::buildScene(const lr::SceneCmd &sc) {
    settings = sc.settings;
    instances.clear(); draws.clear(); lights.clear(); tlasInstances.clear(); casterTris.clear();
    dynHalf ^= 1; dynVerts = dynIndices = 0; rtDynTris = 0;
    instOwner.clear(); curOwner = 0;
    instances.push_back(GInstance{}); instOwner.push_back(0);     // index 0: unused

    // ---- polled world state
    const uint8_t *p = (const uint8_t *)(&sc + 1);
    const uint32_t *surfTex = (const uint32_t *)p; p += (size_t)sc.nSurfaceTex * 4;
    bool worldOk = haveWorld && sc.contextId == worldContext;
    if (worldOk && sc.nSurfaceTex == surfaces.size()) {
        for (uint32_t i = 0; i < sc.nSurfaceTex; i++) surfaces[i].tex = surfTex[i] < kTexLightmap ? surfTex[i] : 0;
        memcpy(surfBuf.mapped, surfaces.data(), surfaces.size() * sizeof(GSurface));
    }
    if (worldOk && gfx.rt && (!emittersBuilt || emittersStrength != settings.giEmissive)) {
        bool textured = false;
        for (const GSurface &sf : surfaces) if (sf.tex) { textured = true; break; }
        if (textured) buildEmitters();
    }
    const uint8_t *uvEnd = p + sc.uvBytes;
    if (worldOk)
        for (uint32_t i = 0; i < sc.nUvPolys && p < uvEnd; i++) {
            const lr::UvUpdate &u = *(const lr::UvUpdate *)p; p += sizeof(lr::UvUpdate);
            const float *uv = (const float *)p; p += (size_t)u.nVertices * 8;
            if (u.poly >= polyFirstVertex.size() || polyVertexCount[u.poly] != u.nVertices) continue;
            GVertex *v = (GVertex *)vbuf.mapped + polyFirstVertex[u.poly];
            for (uint32_t k = 0; k < u.nVertices; k++) { v[k].uv[0] = uv[k * 2]; v[k].uv[1] = uv[k * 2 + 1]; }
        }
    p = uvEnd;

    // ---- camera. LithTech: x right, y up, z forward. Clip: x / tan, -y / tan (Vulkan y down), w = z.
    M4 rot = quatToMatrix(sc.camRot.x, sc.camRot.y, sc.camRot.z, sc.camRot.w);
    camRight = V3{rot.m[0][0], rot.m[1][0], rot.m[2][0]}; camUp = V3{rot.m[0][1], rot.m[1][1], rot.m[2][1]};
    camFwd = V3{rot.m[0][2], rot.m[1][2], rot.m[2][2]};
    camPos = V3{sc.camPos.x, sc.camPos.y, sc.camPos.z};
    // the mode area inside the output (letterboxed), then the scene's view rect inside it
    float sx = (float)gfx.extent.width / modeW, sy = (float)gfx.extent.height / modeH, s = std::min(sx, sy);
    float ox = (gfx.extent.width - modeW * s) * 0.5f, oy = (gfx.extent.height - modeH * s) * 0.5f;
    bool full = sc.rect.left <= 0 && sc.rect.top <= 0 && sc.rect.right >= (int)modeW && sc.rect.bottom >= (int)modeH;
    if (full && settings.widescreen != 0) sceneViewport = {0, 0, (float)gfx.extent.width, (float)gfx.extent.height, 0, 1};
    else sceneViewport = {ox + sc.rect.left * s, oy + sc.rect.top * s, (sc.rect.right - sc.rect.left) * s, (sc.rect.bottom - sc.rect.top) * s, 0, 1};
    float fx = sc.xFov > 0.01f ? sc.xFov : 1.5708f, fy = sc.yFov > 0.01f ? sc.yFov : 1.178f;
    tanX = tanf(fx * 0.5f); tanY = tanf(fy * 0.5f);
    if (settings.widescreen != 0 && sceneViewport.height > 0) {
        // Blood II computes its FOVs for 4:3: keep the vertical one and widen the horizontal one (Hor+)
        float aspect = sceneViewport.width / sceneViewport.height;
        if (fabsf(tanX / tanY - aspect) > 0.01f) tanX = tanY * aspect;
    }
    nearZ = 7.0f; farZ = sc.farZ > nearZ ? std::min(sc.farZ, 50000.0f) : 15000.0f;
    auto makeViewProj = [&](V3 eye, float zn, float zf) {
        M4 view = M4::identity();
        const V3 ax[3] = {camRight, camUp, camFwd};
        for (int i = 0; i < 3; i++) { view.m[i][0] = ax[i].x; view.m[i][1] = ax[i].y; view.m[i][2] = ax[i].z; view.m[i][3] = -dot(ax[i], eye); }
        M4 proj = {};
        proj.m[0][0] = 1.0f / tanX; proj.m[1][1] = -1.0f / tanY;
        proj.m[2][2] = zf / (zf - zn); proj.m[2][3] = -zf * zn / (zf - zn); proj.m[3][2] = 1;
        return proj * view;
    };
    // sky camera (d3d.ren): the camera's place in the world box mapped into the sky view box
    skyEye = camPos;
    {
        // r_SetupSkyStuff: the fraction of the camera's way through the WORLD's box, per axis, unclamped
        const float c[3] = {camPos.x, camPos.y, camPos.z}, mn[3] = {worldMin.x, worldMin.y, worldMin.z}, mx[3] = {worldMax.x, worldMax.y, worldMax.z},
                    vmn[3] = {sc.skyViewMin.x, sc.skyViewMin.y, sc.skyViewMin.z}, vmx[3] = {sc.skyViewMax.x, sc.skyViewMax.y, sc.skyViewMax.z};
        float e[3];
        for (int i = 0; i < 3; i++) {
            float t = mx[i] - mn[i] != 0.0f ? (c[i] - mn[i]) / (mx[i] - mn[i]) : 0.5f;
            e[i] = vmn[i] + (vmx[i] - vmn[i]) * t;
        }
        skyEye = V3{e[0], e[1], e[2]};
    }
    memset(&frameData, 0, sizeof(frameData));
    frameData.viewProj[0] = makeViewProj(camPos, nearZ, farZ);
    frameData.viewProj[1] = makeViewProj(skyEye, 1.0f, 200000.0f);
    frameData.viewProj[2] = makeViewProj(camPos, 0.05f, farZ);
    for (int i = 0; i < 3; i++) frameData.prevViewProj[i] = prevValid ? prevViewProj[i] : frameData.viewProj[i];
    for (int i = 0; i < 3; i++) prevViewProj[i] = frameData.viewProj[i];
    prevValid = true;

    addWorldDraws(sc);
    auto walk = [&](const lr::SceneCmd &scene) {
        const uint8_t *q = (const uint8_t *)(&scene + 1) + (size_t)scene.nSurfaceTex * 4 + scene.uvBytes;
        const uint8_t *end = q + scene.objectBytes;
        for (uint32_t i = 0; i < scene.nObjects && q + sizeof(lr::ObjectRecord) <= end; i++) {
            const lr::ObjectRecord &o = *(const lr::ObjectRecord *)q;
            if (o.size < sizeof(lr::ObjectRecord) || q + o.size > end) break;
            addObject(scene, o);
            q += o.size;
        }
    };
    walk(sc);
    for (auto &e : extraScenes) {       // object-list scenes of the same frame (view weapon etc.)
        const lr::SceneCmd &es = *(const lr::SceneCmd *)e.data();
        if (es.drawMode == 2) walk(es);
    }

    // ---- dynamic BLAS input: the casters' triangles gathered into one contiguous index range
    if (gfx.rt && !casterTris.empty()) {
        uint32_t first;
        uint32_t n = (uint32_t)casterTris.size();
        uint32_t *idx = allocDynIndices(n * 3, first);
        if (idx) {
            const uint32_t *all = (const uint32_t *)ibuf.mapped;
            uint32_t *tri = (uint32_t *)triBuf.mapped + kStaticIndices / 3;
            for (uint32_t t = 0; t < n; t++) {
                memcpy(idx + t * 3, all + casterTris[t].firstIndex, 12);
                tri[t] = casterTris[t].instance;
            }
            rtDynFirstIndex = first; rtDynTris = n;
        }
    }

    // ---- frame constants
    lightAdd[0] = sc.lightAdd.x / 255.0f; lightAdd[1] = sc.lightAdd.y / 255.0f; lightAdd[2] = sc.lightAdd.z / 255.0f;
    GFrame &f = frameData;
    f.camPos[0] = camPos.x; f.camPos[1] = camPos.y; f.camPos[2] = camPos.z;
    f.fogColor[0] = settings.fogR / 255.0f; f.fogColor[1] = settings.fogG / 255.0f; f.fogColor[2] = settings.fogB / 255.0f;
    f.fogColor[3] = settings.fogEnable != 0 ? 1.0f : 0.0f;
    f.fogParams[0] = settings.fogNearZ; f.fogParams[1] = settings.fogFarZ; f.fogParams[2] = settings.skyFogNearZ; f.fogParams[3] = settings.skyFogFarZ;
    f.lightScale[0] = sc.lightScale.x; f.lightScale[1] = sc.lightScale.y; f.lightScale[2] = sc.lightScale.z;
    f.lightScale[3] = settings.saturate != 0 ? 2.0f : 1.0f;
    f.modelLightDir[0] = 0; f.modelLightDir[1] = 0.894427191f; f.modelLightDir[2] = 0.4472135955f;   // normalize(0, 2, 1), world-fixed
    bool rtOn = gfx.rt && settings.rt != 0;
    f.rt[0] = rtOn ? 1.0f : 0.0f; f.rt[1] = settings.rtShadows; f.rt[2] = settings.rtReflections; f.rt[3] = settings.rtAO;
    f.misc[0] = (float)frameIndex; f.misc[1] = settings.debugView; f.misc[3] = settings.exposure;
    f.skyColor[0] = settings.fogEnable != 0 ? f.fogColor[0] : 0.25f; f.skyColor[1] = settings.fogEnable != 0 ? f.fogColor[1] : 0.27f;
    f.skyColor[2] = settings.fogEnable != 0 ? f.fogColor[2] : 0.32f;
    f.viewport[0] = sceneViewport.x; f.viewport[1] = sceneViewport.y; f.viewport[2] = sceneViewport.width; f.viewport[3] = sceneViewport.height;
    f.mat[0] = rtOn ? settings.bump : 0.0f; f.mat[1] = settings.specular;
    modernLook = rtOn && settings.look != 0 && settings.debugView == 0;
    f.mat[2] = modernLook ? 1.0f : 0.0f;
    f.gi[0] = (rtOn && settings.rtGI != 0 && lampGrid.count) ? 1.0f : 0.0f;
    f.gi[1] = settings.giIndirect; f.gi[2] = settings.giDirect > 0 ? settings.giDirect : 1.0f; f.gi[3] = std::max(settings.shadowSoftness, 0.0f);
    // light shafts: stronger where the level itself is foggy
    f.vol[0] = (rtOn && settings.rtGI != 0 && lampGrid.count) ? settings.volumetric * 0.0004f * (settings.fogEnable != 0 ? 2.5f : 1.0f) : 0.0f;
    f.vol[1] = 1600.0f;
    f.skyEye[0] = skyEye.x; f.skyEye[1] = skyEye.y; f.skyEye[2] = skyEye.z;
    f.ambient[0] = levelAmbient[0]; f.ambient[1] = levelAmbient[1]; f.ambient[2] = levelAmbient[2];
    f.ambient[3] = (rtOn && settings.rtShadows != 0 && lampGrid.count) ? 1.0f : 0.0f;
    f.lampGrid = lampGrid;
    f.nLights = (uint32_t)lights.size();
    memcpy(frameBuf.mapped, &f, sizeof(f));
    memcpy(instBuf.mapped, instances.data(), instances.size() * sizeof(GInstance));
    if (!lights.empty()) memcpy(lightBuf.mapped, lights.data(), lights.size() * sizeof(GLight));
}

// ------------------------------------------------------------------ acceleration structures
void Renderer::buildAccel(VkCommandBuffer c) {
    gfx.destroy(dynBlas);
    if (rtDynTris) {
        dynBlas = gfx.buildBlas(c, vbuf.addr, sizeof(GVertex), kStaticVerts + 2 * kDynVerts - 1, ibuf.addr + (VkDeviceAddress)rtDynFirstIndex * 4,
                                rtDynTris, true, scratch);
        GInstance cont = {};
        M4 id = M4::identity();
        setRows(cont.r, id); setRows(cont.p, id);
        cont.firstIndex = rtDynFirstIndex; cont.triBase = kStaticIndices / 3; cont.flags = I_CONTAINER;
        uint32_t inst = (uint32_t)instances.size();
        if (inst < kMaxInstances) {
            ((GInstance *)instBuf.mapped)[inst] = cont;
            VkAccelerationStructureInstanceKHR ti = {};
            memcpy(ti.transform.matrix, id.m, 48);
            ti.instanceCustomIndex = inst; ti.mask = 0x02; ti.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            ti.accelerationStructureReference = dynBlas.addr;
            tlasInstances.push_back(ti);
        }
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR; mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(c, &di);
    }
    uint32_t count = (uint32_t)tlasInstances.size();
    if (count) memcpy(tlasInstBuf.mapped, tlasInstances.data(), count * sizeof(VkAccelerationStructureInstanceKHR));
    VkAccelerationStructureGeometryKHR geo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geo.geometry.instances.data.deviceAddress = tlasInstBuf.addr;
    VkAccelerationStructureBuildGeometryInfoKHR bi = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; bi.geometryCount = 1; bi.pGeometries = &geo;
    if (!tlas.as) {
        uint32_t maxCount = kMaxTlas;
        VkAccelerationStructureBuildSizesInfoKHR sz = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(gfx.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &maxCount, &sz);
        tlas.buffer = gfx.createBuffer(sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        VkAccelerationStructureCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = tlas.buffer.buf; ci.size = sz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VKC(vkCreateAccelerationStructureKHR(gfx.dev, &ci, nullptr, &tlas.as));
        if (scratch.size < sz.buildScratchSize + 256) {
            gfx.destroy(scratch);
            scratch = gfx.createBuffer(sz.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
        VkWriteDescriptorSetAccelerationStructureKHR wa = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &tlas.as;
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.pNext = &wa; w.dstSet = sceneSet; w.dstBinding = 9; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(gfx.dev, 1, &w, 0, nullptr);
    }
    bi.dstAccelerationStructure = tlas.as;
    bi.scratchData.deviceAddress = (scratch.addr + 255) & ~(VkDeviceAddress)255;
    VkAccelerationStructureBuildRangeInfoKHR range = {count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR *pr = &range;
    vkCmdBuildAccelerationStructuresKHR(c, 1, &bi, &pr);
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR; mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(c, &di);
}

// ------------------------------------------------------------------ frame
// Draws one half of the frame's sorted draw list. Opaque pass: sky, world, solid groups and the view model.
// Translucent pass: everything blended, in d3d.ren's group order.
void Renderer::drawGroups(bool opaquePass, bool rtOn) {
    VkRect2D fullRect = {{0, 0}, gfx.extent};
    auto isOpaqueGroup = [](uint8_t g) { return g <= G_SOLID_POLYGRIDS || g == G_CLOSE; };
    auto clearDepth = [&]() {
        VkClearAttachment ca = {}; ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; ca.clearValue.depthStencil = {1.0f, 0};
        VkClearRect cr = {fullRect, 0, 1};
        vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
    };
    int boundPipe = -1, boundView = -1;
    bool inSky = false;
    for (const Draw &d : draws) {
        if (isOpaqueGroup(d.group) != opaquePass) continue;
        if (d.group == G_SKY) {
            // The sky is a painter's-algorithm pass in the original (no depth at all, objects in list order, each world
            // model walked back to front). Clearing depth before every sky object gives the same result: later objects
            // always cover earlier ones, and each object sorts itself.
            clearDepth(); inSky = true;
        } else if (inSky) { clearDepth(); inSky = false; }
        if (d.pipe != boundPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[rtOn ? 1 : 0][d.pipe]); boundPipe = d.pipe; }
        if (d.view != boundView) {
            // the view model draws into the front sliver of the depth range, so it never intersects the world
            VkViewport vp = sceneViewport;
            if (d.view == 2) { vp.minDepth = 0.0f; vp.maxDepth = 0.02f; }
            else if (d.view == 0) { vp.minDepth = 0.02f; vp.maxDepth = 1.0f; }
            vkCmdSetViewport(cmd, 0, 1, &vp);
            boundView = d.view;
        }
        uint32_t pc[2] = {d.instance, d.view};
        vkCmdPushConstants(cmd, scenePL, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, pc);
        vkCmdDrawIndexed(cmd, d.indexCount, 1, d.firstIndex, 0, 0);
    }
    if (inSky) clearDepth();
}

void Renderer::renderFrame(bool hadScene) {
    LARGE_INTEGER t0, t1, freq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&freq);
    VkDevice dev = gfx.dev;
    if (frameInFlight) {
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        frameInFlight = false;
        uint64_t ts[2] = {};
        if (vkGetQueryPoolResults(dev, queryPool, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            lastGpuMs = (float)((ts[1] - ts[0]) * (double)gfx.timestampPeriod * 1e-6);
    }
    bool drawScene = hadScene && sceneValid && haveWorld;
    if (!ensureWindow()) { sceneValid = false; extraScenes.clear(); return; }

    frameIndex++;
    if (drawScene) buildScene(*(const lr::SceneCmd *)sceneCopy.data());
    else { lightAdd[0] = lightAdd[1] = lightAdd[2] = 0; }
    sceneValid = false; extraScenes.clear();

    uint32_t imageIndex = 0;
    VkResult ar = vkAcquireNextImageKHR(dev, gfx.swapchain, UINT64_MAX, acquireSem, VK_NULL_HANDLE, &imageIndex);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { gfx.destroySwapchain(false); return; }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { Log("acquire failed: %d", (int)ar); gfx.destroySwapchain(false); return; }

    vkResetFences(dev, 1, &fence);
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(cmd, &bi));
    vkCmdResetQueryPool(cmd, queryPool, 0, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);

    bool rtOn = gfx.rt && settings.rt != 0 && drawScene;
    if (gfx.rt && drawScene) buildAccel(cmd);

    VkRect2D fullRect = {{0, 0}, gfx.extent};
    int lh = frameIndex & 1;
    if (drawScene) {
        // d3d.ren's group order; inside a group the submission order, except where a sort key asks for far-to-near
        std::stable_sort(draws.begin(), draws.end(), [](const Draw &a, const Draw &b) { return a.group != b.group ? a.group < b.group : a.sortKey > b.sortKey; });
        auto beginScenePass = [&](VkImageView color0, bool clear) {
            VkRenderingAttachmentInfo ca[4] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                                               {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
            VkImageView views[4] = {color0, sceneAux.view, sceneLight.view, sceneIndirect.view};
            for (int i = 0; i < 4; i++) {
                ca[i].imageView = views[i]; ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ca[i].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            ca[1].clearValue.color = {{0, 0, 1, 0}};
            VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            da.imageView = sceneDepth.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.clearValue.depthStencil = {1.0f, 0};
            VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = fullRect; ri.layerCount = 1; ri.colorAttachmentCount = 4; ri.pColorAttachments = ca; ri.pDepthAttachment = &da;
            vkCmdBeginRendering(cmd, &ri);
            VkRect2D sc = {{(int32_t)sceneViewport.x, (int32_t)sceneViewport.y}, {(uint32_t)sceneViewport.width, (uint32_t)sceneViewport.height}};
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePL, 0, 1, &sceneSet, 0, nullptr);
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf.buf, &zero);
            vkCmdBindIndexBuffer(cmd, ibuf.buf, 0, VK_INDEX_TYPE_UINT32);
        };
        auto toAttachment = [&](Image &i) { transition(cmd, i.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); };
        auto toSampled = [&](Image &i) { transition(cmd, i.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); };

        // ---- 1. opaque pass: sky, world, solid objects, view model -> colour (albedo or final), aux, light, depth
        toAttachment(sceneColor); toAttachment(sceneAux); toAttachment(sceneLight); toAttachment(sceneIndirect);
        transition(cmd, sceneDepth.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        beginScenePass(sceneColor.view, true);
        drawGroups(true, rtOn);
        vkCmdEndRendering(cmd);
        toSampled(sceneColor); toSampled(sceneAux); toSampled(sceneLight); toSampled(sceneIndirect);
        transition(cmd, sceneDepth.img, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

        // ---- 2. lighting resolve: filter + accumulate the ray-traced light, compose with the albedo, fog
        toAttachment(litColor); toAttachment(lightHist[lh]); toAttachment(directHist[lh]);
        {
            VkRenderingAttachmentInfo ca[3] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                                               {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
            ca[0].imageView = litColor.view; ca[1].imageView = lightHist[lh].view; ca[2].imageView = directHist[lh].view;
            for (int i = 0; i < 3; i++) { ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE; }
            VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = fullRect; ri.layerCount = 1; ri.colorAttachmentCount = 3; ri.pColorAttachments = ca;
            vkCmdBeginRendering(cmd, &ri);
            VkViewport vp = {0, 0, (float)gfx.extent.width, (float)gfx.extent.height, 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &fullRect);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composePipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPL, 0, 1, &composeSet[lh], 0, nullptr);
            float hw = settings.giHistory > 0 && settings.giHistory < 0.99f ? settings.giHistory : 0.9f;
            float pc[24] = {frameData.fogColor[0], frameData.fogColor[1], frameData.fogColor[2], frameData.fogColor[3],
                            settings.fogNearZ, settings.fogFarZ, nearZ, farZ,
                            lightHistValid ? 1.0f : 0.0f, hw, tanX, tanY,
                            sceneViewport.x / gfx.extent.width, sceneViewport.y / gfx.extent.height, sceneViewport.width / gfx.extent.width,
                            sceneViewport.height / gfx.extent.height,
                            modernLook ? 1.0f : 0.0f, modernLook ? 3.2f : 1.0f, settings.shadowSoftness > 0 ? 0.8f : 0.0f, 0,
                            // in-scattered light takes the level's fog colour when it has fog, a warm white otherwise
                            settings.fogEnable != 0 ? frameData.fogColor[0] * 0.6f + 0.4f : 1.0f, settings.fogEnable != 0 ? frameData.fogColor[1] * 0.6f + 0.4f : 0.95f,
                            settings.fogEnable != 0 ? frameData.fogColor[2] * 0.6f + 0.4f : 0.85f, 0};
            vkCmdPushConstants(cmd, postPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96, pc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
        }
        toSampled(lightHist[lh]); toSampled(directHist[lh]);
        lightHistValid = true;

        // ---- 3. translucent pass over the resolved scene (depth tested, never written to aux / light)
        toAttachment(sceneAux); toAttachment(sceneLight); toAttachment(sceneIndirect);
        transition(cmd, sceneDepth.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        beginScenePass(litColor.view, false);
        drawGroups(false, rtOn);
        vkCmdEndRendering(cmd);
        toSampled(litColor); toSampled(sceneAux); toSampled(sceneLight); toSampled(sceneIndirect);
        transition(cmd, sceneDepth.img, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

        // ---- 4. bloom (modern look): bright parts of the HDR scene, blurred at 1/4 and 1/16 resolution
        if (modernLook && settings.bloom > 0) {
            auto bloomPass = [&](int srcSet, Image &dst, float mode) {
                toAttachment(dst);
                VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                ca.imageView = dst.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRect2D area = {{0, 0}, {dst.width, dst.height}};
                VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea = area; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
                vkCmdBeginRendering(cmd, &ri);
                VkViewport vp = {0, 0, (float)dst.width, (float)dst.height, 0, 1};
                vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &area);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomPipe);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPL, 0, 1, &bloomSet[srcSet], 0, nullptr);
                float pc[24] = {mode, 1.5f};
                vkCmdPushConstants(cmd, postPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96, pc);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRendering(cmd);
                toSampled(dst);
            };
            bloomPass(0, bloom[0], 0);      // lit -> 1/4, thresholded
            bloomPass(1, bloom[1], 1); bloomPass(2, bloom[0], 2);
            bloomPass(1, bloom[2], 3);      // 1/4 -> 1/16
            bloomPass(3, bloom[3], 1); bloomPass(4, bloom[2], 2);
        }
    } else
        lightHistValid = false;

    // ---- resolve + 2D layer -> swapchain (and the AO history for the next frame)
    int h = frameIndex & 1;
    transition(cmd, gfx.swapImages[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cmd, history[h].img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    {
        VkRenderingAttachmentInfo ca[2] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
        ca[0].imageView = gfx.swapViews[imageIndex]; ca[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca[1].imageView = history[h].view; ca[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = fullRect; ri.layerCount = 1; ri.colorAttachmentCount = 2; ri.pColorAttachments = ca;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport vp = {0, 0, (float)gfx.extent.width, (float)gfx.extent.height, 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &fullRect);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPL, 0, 1, &postSet[h], 0, nullptr);
        float sx = (float)gfx.extent.width / modeW, sy = (float)gfx.extent.height / modeH, s = std::min(sx, sy);
        bool look = modernLook && drawScene;
        float pc[24] = {(gfx.extent.width - modeW * s) * 0.5f / gfx.extent.width, (gfx.extent.height - modeH * s) * 0.5f / gfx.extent.height,
                        modeW * s / gfx.extent.width, modeH * s / gfx.extent.height,
                        lightAdd[0], lightAdd[1], lightAdd[2], drawScene ? 1.0f : 0.0f,
                        (rtOn && settings.rtAO != 0) ? 1.0f : 0.0f, historyValid ? 1.0f : 0.0f, nearZ, farZ,
                        (settings.exposure > 0 ? settings.exposure : 1.0f) * (look ? 1.35f : 1.0f), settings.debugView, overlayEmpty ? 0.0f : 1.0f, 0,
                        look ? 1.0f : 0.0f, (look && settings.bloom > 0) ? settings.bloom * 0.18f : 0.0f, gfx.hdrOutput ? 1.0f : 0.0f, 200.0f};
        vkCmdPushConstants(cmd, postPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96, pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }
    transition(cmd, history[h].img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    historyValid = drawScene;

    bool shot = !shotPath.empty();
    if (shot) {
        VkDeviceSize bytes = (VkDeviceSize)gfx.extent.width * gfx.extent.height * 4;
        if (shotBuf.size < bytes) { gfx.destroy(shotBuf); shotBuf = gfx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true); }
        transition(cmd, gfx.swapImages[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r = {};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = {gfx.extent.width, gfx.extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, gfx.swapImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shotBuf.buf, 1, &r);
        transition(cmd, gfx.swapImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else
        transition(cmd, gfx.swapImages[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
    VKC(vkEndCommandBuffer(cmd));

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acquireSem; si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &renderSem;
    VKC(vkQueueSubmit(gfx.queue, 1, &si, fence));
    frameInFlight = true;
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &renderSem; pi.swapchainCount = 1; pi.pSwapchains = &gfx.swapchain; pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(gfx.queue, &pi);
    if (shot) { vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX); frameInFlight = false; saveScreenshot(); }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX); frameInFlight = false;
        gfx.destroySwapchain(false);
    }

    QueryPerformanceCounter(&t1);
    lastFrameMs = (float)((t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
    statFrames++; statMs += lastFrameMs;
    double elapsed = (double)(t1.QuadPart - statStart.QuadPart) / freq.QuadPart;
    if (elapsed >= 5.0) {
        Log("%.1f fps | host cpu %.2f ms, gpu %.2f ms | %zu draws, %zu instances, %u dyn verts, %u rt dyn tris, %zu tlas, %zu lights%s",
            statFrames / elapsed, statMs / statFrames, lastGpuMs, draws.size(), instances.size(), dynVerts, rtDynTris, tlasInstances.size(),
            lights.size(), rtOn ? " | RT" : "");
        statFrames = 0; statMs = 0; statStart = t1;
    }
}

void Renderer::saveScreenshot() {
    uint32_t w = gfx.extent.width, h = gfx.extent.height;
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    const uint8_t *src = (const uint8_t *)shotBuf.mapped;
    bool bgr = gfx.swapFormat == VK_FORMAT_B8G8R8A8_UNORM || gfx.swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
    if (gfx.swapFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
        const uint32_t *p = (const uint32_t *)src;      // an HDR10 image is saved as its PQ code values (dark in a viewer)
        for (size_t i = 0; i < (size_t)w * h; i++) {
            rgb[i * 3] = (uint8_t)((p[i] & 1023) >> 2); rgb[i * 3 + 1] = (uint8_t)(((p[i] >> 10) & 1023) >> 2); rgb[i * 3 + 2] = (uint8_t)(((p[i] >> 20) & 1023) >> 2);
        }
    } else
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgb[i * 3] = src[i * 4 + (bgr ? 2 : 0)]; rgb[i * 3 + 1] = src[i * 4 + 1]; rgb[i * 3 + 2] = src[i * 4 + (bgr ? 0 : 2)];
    }
    bool ok = shotFormat == 1 ? WritePng(shotPath.c_str(), w, h, rgb.data()) : WriteBmp(shotPath.c_str(), w, h, rgb.data());
    Log("screenshot %s: %s | cam %.1f,%.1f,%.1f fwd %.2f,%.2f,%.2f | %zu draws", shotPath.c_str(), ok ? "written" : "FAILED", camPos.x, camPos.y, camPos.z, camFwd.x, camFwd.y, camFwd.z, draws.size());
    if (settings.debugView == 6 || settings.debugView == 7)
        for (size_t i = 1; i < instances.size() && i < instOwner.size(); i++) {
            const GInstance &g = instances[i];
            Log("  instance %zu: type %u obj %08x flags %x firstIndex %u tex %u pos %.0f,%.0f,%.0f alpha %.2f", i, (unsigned)(instOwner[i] >> 32),
                (unsigned)instOwner[i], g.flags, g.firstIndex, g.tex, g.r[0][3], g.r[1][3], g.r[2][3], g.color[3]);
        }
    shotPath.clear();
}

// ------------------------------------------------------------------ effects (implemented in effects.cpp)
