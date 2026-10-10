// Native Vulkan backend: HDR scene target and post processing. Included by gfx_vulkan.cpp (same translation unit, same globals).
//
// The 3D scene is drawn into a 16 bit float image ("stage 0"). When the first 2D interface draw arrives, or the frame is presented,
// the scene goes through bloom, a soft highlight roll-off, colour grading and FXAA and lands in the swapchain image; the interface
// is then drawn straight onto the swapchain ("stage 1") so text and icons are never filtered.

enum PostPass { PASS_BLOOM_DOWN0, PASS_BLOOM_DOWN, PASS_BLOOM_UP, PASS_COMPOSITE, PASS_FXAA, PASS_AO, PASS_AOBLUR, PASS_SHADOW, PASS_COUNT };

struct PostUbo;
GpuTexture* ShadowStage(const PostUbo& base);
bool ShadowsEnabled();

struct PostUbo
{
	float texel[4];
	float p0[4];
	float p1[4];
	float p2[4];
	float proj[4];
	float invView[16];
	float lightVP[16];
	float sunDir[4];
	float shadowParams[4];
	float viewRect[4];		// x, y, width, height in pixels of the viewport the 3D scene was drawn with
};

void Multiply(const float* a, const float* b, float* out)	// row-major 4x4: out = a * b
{
	float r[16];
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
	memcpy(out, r, sizeof(r));
}


struct ShadowCaster
{
	DrawUbo u;
	VkDeviceSize vOff, iOff;
	uint32_t numVertices, indexCount, wide, topology, fvf, stride;
	int minIndex;
	GpuTexture* tex0;
	float groundZ;
};

struct ShadowState
{
	bool on = true;
	float sun[3] = { 0.45f, 0.45f, 0.77f };		// world space direction towards the sun
	bool haveSun = false;
	bool suppress = false;
	std::vector<ShadowCaster> casters;
	float groundZ = 0.0f;
	float lightVP[16] = {};
	float range = 1.0f, mapWorld = 1.0f;
	GpuTexture* map = nullptr;
	GpuTexture* vis = nullptr;
	uint32_t mapSize = 4096;
	float lightSize = 0.050f, strength = 0.58f;
} g_sh;


static const int kBloomLevels = 5;

// ---- images ---------------------------------------------------------------------------------------------------------------

bool CreateShadowTargets();
void DestroyShadowTargets();

GpuTexture* NewTarget(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
{
	GpuTexture* t = new GpuTexture();
	t->format = fmt; t->width = w; t->height = h; t->levels = 1; t->renderTarget = true;
	VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = { w, h, 1 }; ci.mipLevels = 1; ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(B.device, &ci, nullptr, &t->image) != VK_SUCCESS) { delete t; return nullptr; }
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(B.device, t->image, &req);
	t->alloc = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
	if (t->alloc.block < 0) { vkDestroyImage(B.device, t->image, nullptr); delete t; return nullptr; }
	vkBindImageMemory(B.device, t->image, t->alloc.memory, t->alloc.offset);
	VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = t->image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.subresourceRange = { aspect, 0, 1, 0, 1 };
	if (vkCreateImageView(B.device, &vi, nullptr, &t->view) != VK_SUCCESS) { DestroyGpuTexture(t); return nullptr; }
	return t;
}

bool CreatePostTargets()
{
	const VkImageUsageFlags sampled = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	B.hdr = NewTarget(B.extent.width, B.extent.height, VK_FORMAT_R16G16B16A16_SFLOAT, sampled);
	B.ldr = NewTarget(B.extent.width, B.extent.height, B.swapFormat, sampled);
	uint32_t w = B.extent.width, h = B.extent.height;
	for (int i = 0; i < kBloomLevels; ++i)
	{
		w = std::max(1u, w / 2); h = std::max(1u, h / 2);
		B.bloom[i] = NewTarget(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, sampled);
	}
	B.depthCopy = NewTarget(B.extent.width, B.extent.height, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);		// same format as the scene depth: a copy needs compatible formats
	if (B.depthCopy) B.depthCopy->isDepth = true;
	B.ao[0] = NewTarget(B.extent.width, B.extent.height, VK_FORMAT_R8_UNORM, sampled);
	B.ao[1] = NewTarget(B.extent.width, B.extent.height, VK_FORMAT_R8_UNORM, sampled);
	// a view of the depth buffer for sampling; it is not owned by this wrapper
	B.depthTex = new GpuTexture();
	B.depthTex->image = B.depthImage; B.depthTex->width = B.extent.width; B.depthTex->height = B.extent.height; B.depthTex->isDepth = true;
	{
		VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = B.depthImage; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
		vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
		VKCHECK(vkCreateImageView(B.device, &vi, nullptr, &B.depthSampleView));
		B.depthTex->view = B.depthSampleView;
	}
	bool ok = B.hdr && B.ldr && B.ao[0] && B.ao[1];
	CreateShadowTargets();
	for (int i = 0; i < kBloomLevels; ++i) ok = ok && B.bloom[i];
	if (!ok)
	{
		Log("post processing targets could not be created: the scene is drawn directly");
		B.postOn = false;
	}
	return ok;
}

void DestroyPostTargets()
{
	// called after vkDeviceWaitIdle
	if (B.hdr) { DestroyGpuTexture(B.hdr); B.hdr = nullptr; }
	DestroyShadowTargets();
	if (B.depthCopy) { DestroyGpuTexture(B.depthCopy); B.depthCopy = nullptr; }
	if (B.ldr) { DestroyGpuTexture(B.ldr); B.ldr = nullptr; }
	for (int i = 0; i < 2; ++i)
		if (B.ao[i]) { DestroyGpuTexture(B.ao[i]); B.ao[i] = nullptr; }
	if (B.depthSampleView) { vkDestroyImageView(B.device, B.depthSampleView, nullptr); B.depthSampleView = VK_NULL_HANDLE; }
	if (B.depthTex) { delete B.depthTex; B.depthTex = nullptr; }
	for (int i = 0; i < kBloomLevels; ++i)
		if (B.bloom[i]) { DestroyGpuTexture(B.bloom[i]); B.bloom[i] = nullptr; }
}

// ---- pipelines ------------------------------------------------------------------------------------------------------------

VkPipeline GetPostPipe(int pass, VkFormat fmt, bool additive)
{
	const uint64_t key = (uint64_t)pass | ((uint64_t)fmt << 8) | ((uint64_t)additive << 40);
	auto it = B.postPipes.find(key);
	if (it != B.postPipes.end())
		return it->second;
	VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1; vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	if (additive)
	{
		cba.blendEnable = VK_TRUE; cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.colorBlendOp = VK_BLEND_OP_ADD;
		cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1; cb.pAttachments = &cba;
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dys{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dys.dynamicStateCount = 2; dys.pDynamicStates = dyn;
	VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = B.postVs; stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = B.postFs[pass]; stages[1].pName = "main";
	VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
	VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stages; pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp; pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &cb; pi.pDynamicState = &dys; pi.layout = B.pipeLayout;
	VkPipeline p = VK_NULL_HANDLE;
	VKCHECK(vkCreateGraphicsPipelines(B.device, VK_NULL_HANDLE, 1, &pi, nullptr, &p));
	B.postPipes[key] = p;
	return p;
}

bool CreatePostShaders()
{
	VkShaderModuleCreateInfo smi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	smi.codeSize = sizeof(g_gfxPostVert); smi.pCode = g_gfxPostVert;
	VKCHECK(vkCreateShaderModule(B.device, &smi, nullptr, &B.postVs));
	struct { const unsigned int* code; size_t size; } fs[PASS_COUNT] = {
		{ g_gfxPostBloomDown0Frag, sizeof(g_gfxPostBloomDown0Frag) }, { g_gfxPostBloomDownFrag, sizeof(g_gfxPostBloomDownFrag) },
		{ g_gfxPostBloomUpFrag, sizeof(g_gfxPostBloomUpFrag) }, { g_gfxPostCompositeFrag, sizeof(g_gfxPostCompositeFrag) },
		{ g_gfxPostFxaaFrag, sizeof(g_gfxPostFxaaFrag) }, { g_gfxPostAoFrag, sizeof(g_gfxPostAoFrag) }, { g_gfxPostAoBlurFrag, sizeof(g_gfxPostAoBlurFrag) }, { g_gfxPostShadowFrag, sizeof(g_gfxPostShadowFrag) } };
	for (int i = 0; i < PASS_COUNT; ++i)
	{
		smi.codeSize = fs[i].size; smi.pCode = fs[i].code;
		VKCHECK(vkCreateShaderModule(B.device, &smi, nullptr, &B.postFs[i]));
	}
	return true;
}

// ---- passes ---------------------------------------------------------------------------------------------------------------

VkDeviceSize RingTake(VkDeviceSize bytes, VkDeviceSize align)
{
	VkDeviceSize at = (B.ringCursor + align - 1) & ~(align - 1);
	if (at + bytes > B.ringSize) return ~0ull;
	B.ringCursor = at + bytes;
	return at;
}

void ToSampled(GpuTexture* t)
{
	if (t->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		return;
	ImageBarrier(B.cmd, t->image, t->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// One full screen pass. Inputs must be in the sampled layout. The target is left as a colour attachment (render targets go on to the
// sampled layout through ToSampled; the swapchain stays an attachment).
void PostDraw(int pass, bool additive, GpuTexture* const in[4], PostUbo& ub, VkImage tImg, VkImageView tView, VkFormat tFmt, VkExtent2D ext,
	VkImageLayout tOld, bool loadTarget)
{
	ImageBarrier(B.cmd, tImg, tOld, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, loadTarget ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : 0,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
	VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	color.imageView = tView; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = loadTarget ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea = { { 0, 0 }, ext }; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &color;
	vkCmdBeginRendering(B.cmd, &ri);
	vkCmdBindPipeline(B.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, GetPostPipe(pass, tFmt, additive));
	VkViewport vp{ 0, 0, (float)ext.width, (float)ext.height, 0.0f, 1.0f };
	VkRect2D sc{ { 0, 0 }, ext };
	vkCmdSetViewport(B.cmd, 0, 1, &vp);
	vkCmdSetScissor(B.cmd, 0, 1, &sc);
	const VkDeviceSize off = RingTake(sizeof(PostUbo), B.uboAlign);
	if (off != ~0ull)
	{
		memcpy((char*)B.ringAlloc.mapped + off, &ub, sizeof(ub));
		VkDescriptorBufferInfo bi{ B.ring, off, sizeof(PostUbo) };
		VkDescriptorImageInfo imgs[4];
		VkWriteDescriptorSet w[5] = {};
		w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &bi;
		const VkSampler samp = GetSampler(2, 2, 0, 3, 3, 1);
		const VkSampler sampNearest = GetSampler(1, 1, 0, 3, 3, 1);
		for (int i = 0; i < 4; ++i)
		{
			GpuTexture* t = in[i] ? in[i] : B.white;
			imgs[i] = { t->isDepth ? sampNearest : samp, t->view, t->isDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
			w[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1 + i].dstBinding = 1 + i; w[1 + i].descriptorCount = 1;
			w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1 + i].pImageInfo = &imgs[i];
		}
		vkCmdPushDescriptorSetKHR(B.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, B.pipeLayout, 0, 5, w);
		vkCmdDraw(B.cmd, 3, 1, 0, 0);
	}
	vkCmdEndRendering(B.cmd);
}

bool AcquireSwap()
{
	if (B.acquired)
		return true;
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		if (B.swapchain == VK_NULL_HANDLE && !CreateSwapchain())
			return false;
		VkResult r = vkAcquireNextImageKHR(B.device, B.swapchain, UINT64_MAX, B.imageAvailable, VK_NULL_HANDLE, &B.imageIndex);
		if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { B.acquired = true; return true; }
		if (r == VK_ERROR_OUT_OF_DATE_KHR) { DestroySwapchain(); continue; }
		Log("vkAcquireNextImageKHR failed: %d", (int)r);
		return false;
	}
	return false;
}

// Runs the post processing chain and leaves the swapchain image holding the finished scene, in stage 1 (interface).
bool RunPostProcess()
{
	{ static int n = 0; if (n++ < 6) Log("post: scene3D %d, draws so far this frame %u, rendering %d, stage %d", (int)B.scene3D, g_cnt[14], (int)B.rendering, B.stage); }
	EndPass();
	if (!B.hdr || !AcquireSwap())
		return false;
	// the scene may have been recorded for a swapchain of another size
	if (B.hdr->width != B.extent.width || B.hdr->height != B.extent.height)
		return false;
	ToSampled(B.hdr);

	PostUbo ub;
	memset(&ub, 0, sizeof(ub));
	const bool has3D = B.scene3D;
	const float w = (float)B.extent.width, h = (float)B.extent.height;
	ub.texel[0] = 1.0f / w; ub.texel[1] = 1.0f / h; ub.texel[2] = w; ub.texel[3] = h;
	ub.p0[0] = g_postCfg.bloomThreshold; ub.p0[1] = g_postCfg.bloomKnee; ub.p0[2] = g_postCfg.bloomIntensity;
	ub.p1[0] = g_postCfg.saturation; ub.p1[1] = g_postCfg.contrast;
	ub.p2[0] = g_postCfg.fxaa ? 1.0f : 0.0f; ub.p2[1] = has3D ? 1.0f : 0.0f;

	GpuTexture* none[4] = { nullptr, nullptr, nullptr, nullptr };
	bool haveAo = false;
	GpuTexture* shadowVis = nullptr;
	if (has3D && B.haveProj && B.depthTex && (g_postCfg.aoStrength > 0.0f || ShadowsEnabled()))
	{
		// the scene's depth buffer becomes readable
		ImageBarrier(B.cmd, B.depthImage, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
			VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		shadowVis = ShadowStage(ub);
	}
	if (has3D && B.haveProj && g_postCfg.aoStrength > 0.0f && B.depthTex && B.ao[0])
	{
		PostUbo a = ub;
		a.proj[0] = B.lastProj[0]; a.proj[1] = B.lastProj[5]; a.proj[2] = B.lastProj[10]; a.proj[3] = B.lastProj[14];
		a.p2[3] = B.lastProj[11] < 0.0f ? -1.0f : 1.0f; memcpy(a.viewRect, B.lastViewport, 16);
		a.p1[2] = g_postCfg.aoStrength; a.p1[3] = g_postCfg.aoRadius;
		GpuTexture* inD[4] = { B.depthTex, nullptr, nullptr, nullptr };
		PostDraw(PASS_AO, false, inD, a, B.ao[0]->image, B.ao[0]->view, B.ao[0]->format, B.extent, VK_IMAGE_LAYOUT_UNDEFINED, false);
		B.ao[0]->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ToSampled(B.ao[0]);
		a.p2[0] = 1.0f / w; a.p2[1] = 0.0f;
		GpuTexture* inH[4] = { B.ao[0], B.depthTex, nullptr, nullptr };
		PostDraw(PASS_AOBLUR, false, inH, a, B.ao[1]->image, B.ao[1]->view, B.ao[1]->format, B.extent, VK_IMAGE_LAYOUT_UNDEFINED, false);
		B.ao[1]->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ToSampled(B.ao[1]);
		a.p2[0] = 0.0f; a.p2[1] = 1.0f / h;
		GpuTexture* inV[4] = { B.ao[1], B.depthTex, nullptr, nullptr };
		PostDraw(PASS_AOBLUR, false, inV, a, B.ao[0]->image, B.ao[0]->view, B.ao[0]->format, B.extent, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
		B.ao[0]->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ToSampled(B.ao[0]);
		haveAo = true;
	}
	if (has3D && g_postCfg.bloomIntensity > 0.0f)
	{
		// bright pass and down sampling chain
		GpuTexture* in[4] = { B.hdr, nullptr, nullptr, nullptr };
		PostUbo d = ub;
		d.texel[0] = 1.0f / (float)B.hdr->width; d.texel[1] = 1.0f / (float)B.hdr->height;
		PostDraw(PASS_BLOOM_DOWN0, false, in, d, B.bloom[0]->image, B.bloom[0]->view, B.bloom[0]->format, { B.bloom[0]->width, B.bloom[0]->height },
			VK_IMAGE_LAYOUT_UNDEFINED, false);
		B.bloom[0]->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ToSampled(B.bloom[0]);
		for (int i = 1; i < kBloomLevels; ++i)
		{
			GpuTexture* src = B.bloom[i - 1];
			GpuTexture* inn[4] = { src, nullptr, nullptr, nullptr };
			d.texel[0] = 1.0f / (float)src->width; d.texel[1] = 1.0f / (float)src->height;
			PostDraw(PASS_BLOOM_DOWN, false, inn, d, B.bloom[i]->image, B.bloom[i]->view, B.bloom[i]->format, { B.bloom[i]->width, B.bloom[i]->height },
				VK_IMAGE_LAYOUT_UNDEFINED, false);
			B.bloom[i]->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ToSampled(B.bloom[i]);
		}
		// up sampling: every level is added onto the next larger one
		for (int i = kBloomLevels - 1; i >= 1; --i)
		{
			GpuTexture* src = B.bloom[i];
			GpuTexture* inn[4] = { src, nullptr, nullptr, nullptr };
			d.texel[0] = 1.0f / (float)src->width; d.texel[1] = 1.0f / (float)src->height;
			GpuTexture* dst = B.bloom[i - 1];
			PostDraw(PASS_BLOOM_UP, true, inn, d, dst->image, dst->view, dst->format, { dst->width, dst->height }, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
			dst->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ToSampled(dst);
		}
	}

	// composite into the LDR image, then FXAA into the swapchain (or straight into the swapchain when there is no 3D scene)
	GpuTexture* inC[4] = { B.hdr, B.bloom[0], haveAo ? B.ao[0] : nullptr, shadowVis };
	if (has3D)
	{
		PostUbo c = ub;
		if (g_postCfg.bloomIntensity <= 0.0f) c.p0[2] = 0.0f;
		c.p1[2] = haveAo ? g_postCfg.aoStrength : 0.0f;
		c.sunDir[3] = shadowVis ? g_sh.strength : 0.0f;
		PostDraw(PASS_COMPOSITE, false, inC, c, B.ldr->image, B.ldr->view, B.ldr->format, B.extent, VK_IMAGE_LAYOUT_UNDEFINED, false);
		B.ldr->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ToSampled(B.ldr);
		GpuTexture* inF[4] = { B.ldr, nullptr, nullptr, nullptr };
		PostDraw(PASS_FXAA, false, inF, ub, B.swapImages[B.imageIndex], B.swapViews[B.imageIndex], B.swapFormat, B.extent, VK_IMAGE_LAYOUT_UNDEFINED, false);
	}
	else
	{
		PostDraw(PASS_COMPOSITE, false, inC, ub, B.swapImages[B.imageIndex], B.swapViews[B.imageIndex], B.swapFormat, B.extent, VK_IMAGE_LAYOUT_UNDEFINED, false);
	}
	(void)none;
	B.stage = 1;
	B.colorLoaded = true;			// the swapchain image now holds the scene
	B.depthLoaded = false;			// the interface gets a fresh depth buffer
	return true;
}

// Called before every draw to the back buffer: the first interface draw after the 3D scene closes the scene.
bool PrepareStage(bool pretransformed)
{
	if (!B.postOn || B.curTarget || B.stage != 0)
		return true;
	if (pretransformed && B.scene3D)
		return RunPostProcess();
	return true;
}

#include "gfx_vk_shadow.inl"

// Copies the depth buffer of the scene drawn so far into a separate image the particle shader can read (it cannot read the depth buffer it is testing against).
void MakeDepthCopy()
{
	EndPass();
	B.depthCopyDone = false;
	if (!B.depthCopy || !B.depthLoaded)
		return;
	ImageBarrier(B.cmd, B.depthImage, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	ImageBarrier(B.cmd, B.depthCopy->image, B.depthCopy->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkImageCopy ic{};
	ic.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }; ic.dstSubresource = ic.srcSubresource;
	ic.extent = { B.extent.width, B.extent.height, 1 };
	vkCmdCopyImage(B.cmd, B.depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, B.depthCopy->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
	ImageBarrier(B.cmd, B.depthCopy->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	ImageBarrier(B.cmd, B.depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
	B.depthCopy->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	B.depthCopyDone = true;
}
