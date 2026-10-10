// Native Vulkan backend: sun shadows from a shadow map with contact hardening (PCSS). Included by gfx_vulkan.cpp after gfx_vk_post.inl.
//
// While the 3D scene is drawn, every opaque draw is also remembered (vertex data stays in the frame's ring buffer). Before the post chain
// runs, these draws are replayed from the sun into a depth map. A full screen pass then reconstructs the world position of each pixel from the
// scene depth, searches the map for blockers and filters over a width that grows with the distance to the nearest blocker: sharp next to the
// object that casts the shadow, softer further away, like a sun that is not a point light.

bool ShadowsEnabled() { return g_sh.on && B.postOn && g_sh.haveSun; }

void RecordCaster(const DrawUbo& u, VkDeviceSize vOff, VkDeviceSize iOff, uint32_t nv, uint32_t ni, int wide, uint32_t topology, uint32_t fvf, uint32_t stride, int minIndex, GpuTexture* tex0, float worldZ)
{
	if (g_sh.casters.size() > 12000) return;
	ShadowCaster c;
	memcpy(&c.u, &u, sizeof(u));
	c.vOff = vOff; c.iOff = iOff; c.numVertices = nv; c.indexCount = ni; c.wide = (uint32_t)wide; c.topology = topology; c.fvf = fvf; c.stride = stride;
	c.minIndex = minIndex; c.tex0 = tex0; c.groundZ = worldZ;
	g_sh.casters.push_back(c);
}

bool CreateShadowTargets()
{
	g_sh.map = NewTarget(g_sh.mapSize, g_sh.mapSize, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
	g_sh.vis = NewTarget(B.extent.width, B.extent.height, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	g_sh.lmap = NewTarget(2048, 2048, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
	if (g_sh.map) g_sh.map->isDepth = true;
	if (g_sh.lmap) g_sh.lmap->isDepth = true;
	if (!g_sh.map || !g_sh.vis)
	{
		Log("shadow map targets could not be created: shadows are off");
		g_sh.on = false;
		return false;
	}
	return true;
}

void DestroyShadowTargets()
{
	if (g_sh.map) { DestroyGpuTexture(g_sh.map); g_sh.map = nullptr; }
	if (g_sh.lmap) { DestroyGpuTexture(g_sh.lmap); g_sh.lmap = nullptr; }
	if (g_sh.vis) { DestroyGpuTexture(g_sh.vis); g_sh.vis = nullptr; }
}

// ---- light matrix ---------------------------------------------------------------------------------------------------------

static void RigidInverse(const float* m, float* out)		// row-major rotation + translation (row vector convention)
{
	memset(out, 0, 64);
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			out[i * 4 + j] = m[j * 4 + i];
	for (int j = 0; j < 3; ++j)
		out[12 + j] = -(m[12] * out[0 * 4 + j] + m[13] * out[1 * 4 + j] + m[14] * out[2 * 4 + j]);
	out[15] = 1.0f;
}

static void Cross3(const float* a, const float* b, float* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static void Norm3(float* v) { const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; } }

// Fits an orthographic projection from the sun around the part of the world the camera sees. invView is the camera to world matrix.
bool BuildLightMatrix(const float* proj, const float* invView, float groundZ, float* outVP, float& range, float& mapWorld)
{
	const float* cam = invView + 12;
	float s[3] = { g_sh.sun[0], g_sh.sun[1], g_sh.sun[2] };
	Norm3(s);
	if (s[2] < 0.15f) { s[2] = 0.15f; Norm3(s); }
	const float d[3] = { -s[0], -s[1], -s[2] };			// the way the light travels
	float up[3] = { 0, 0, 1 }, r[3], u[3];
	Cross3(d, up, r); Norm3(r);
	Cross3(r, d, u); Norm3(u);

	float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
	auto add = [&](const float* p)
	{
		const float a[3] = { p[0] * r[0] + p[1] * r[1] + p[2] * r[2], p[0] * u[0] + p[1] * u[1] + p[2] * u[2], p[0] * d[0] + p[1] * d[1] + p[2] * d[2] };
		for (int i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], a[i]); hi[i] = std::max(hi[i], a[i]); }
	};
	const float sx[4] = { -1, 1, -1, 1 }, sy[4] = { -1, -1, 1, 1 };
	const float heights[2] = { groundZ - 15.0f, groundZ + 90.0f };
	for (int k = 0; k < 4; ++k)
	{
		// ray through the screen corner, view space (the projection looks down -z), then to world space
		const float vx = sx[k] / proj[0], vy = sy[k] / proj[5];
		float dir[3];
		for (int j = 0; j < 3; ++j) dir[j] = vx * invView[0 * 4 + j] + vy * invView[1 * 4 + j] - 1.0f * invView[2 * 4 + j];
		for (int h = 0; h < 2; ++h)
		{
			float t = 3200.0f;
			if (dir[2] < -0.03f) t = std::min(t, (heights[h] - cam[2]) / dir[2]);
			if (t < 1.0f) t = 1.0f;
			const float p[3] = { cam[0] + dir[0] * t, cam[1] + dir[1] * t, cam[2] + dir[2] * t };
			add(p);
		}
	}
	add(cam);
	// casters between the sun and the visible area also matter
	lo[2] -= 700.0f; hi[2] += 60.0f;
	// keep the size steady and snap to the texel grid so shadows do not shimmer when the camera moves
	float ext[2] = { hi[0] - lo[0], hi[1] - lo[1] };
	for (int i = 0; i < 2; ++i) ext[i] = ceilf(ext[i] / 256.0f) * 256.0f;
	const float texel = std::max(ext[0], ext[1]) / (float)g_sh.mapSize;
	lo[0] = floorf(lo[0] / texel) * texel; lo[1] = floorf(lo[1] / texel) * texel;
	const float side = std::max(ext[0], ext[1]);
	hi[0] = lo[0] + side; hi[1] = lo[1] + side;
	range = hi[2] - lo[2];
	mapWorld = side;

	const float sa = 2.0f / side, sb = 2.0f / side, sc = 1.0f / range;
	memset(outVP, 0, 64);
	for (int i = 0; i < 3; ++i) { outVP[i * 4 + 0] = r[i] * sa; outVP[i * 4 + 1] = u[i] * sb; outVP[i * 4 + 2] = d[i] * sc; }
	outVP[12] = -lo[0] * sa - 1.0f; outVP[13] = -lo[1] * sb - 1.0f; outVP[14] = -lo[2] * sc; outVP[15] = 1.0f;
	return true;
}

// ---- shadow map pass ------------------------------------------------------------------------------------------------------

// Replays the recorded opaque draws into a depth map. cullCenter/cullRadius (optional) skip draws whose object origin is farther away; draws in world space (terrain, props) are skipped then.
bool RenderShadowMap(GpuTexture* m, const float* lightVP, const float* cullCenter = nullptr, float cullRadius = 0.0f)
{
	if (!m || g_sh.casters.empty())
		return false;
	ImageBarrier(B.cmd, m->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	depth.imageView = m->view; depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
	depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE; depth.clearValue.depthStencil = { 1.0f, 0 };
	VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea = { { 0, 0 }, { m->width, m->height } }; ri.layerCount = 1; ri.pDepthAttachment = &depth;
	vkCmdBeginRendering(B.cmd, &ri);
	VkViewport vp{ 0, 0, (float)m->width, (float)m->height, 0.0f, 1.0f };
	VkRect2D sc{ { 0, 0 }, { m->width, m->height } };
	vkCmdSetViewport(B.cmd, 0, 1, &vp);
	vkCmdSetScissor(B.cmd, 0, 1, &sc);
	vkCmdSetStencilReference(B.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);

	VkPipeline last = VK_NULL_HANDLE;
	const VkSampler samp = GetSampler(2, 2, 0, 1, 1, 1);
	for (const ShadowCaster& c : g_sh.casters)
	{
		if (cullCenter)
		{
			const float* w = c.u.world;
			if (w[12] == 0.0f && w[13] == 0.0f && w[14] == 0.0f) continue;
			const float dx = w[12] - cullCenter[0], dy = w[13] - cullCenter[1];
			if (dx * dx + dy * dy > cullRadius * cullRadius) continue;
		}
		PipeKey key;
		memset(&key, 0, sizeof(key));
		key.fvf = c.fvf; key.stride = c.stride; key.topology = c.topology; key.shadow = 1; key.depthTest = 1; key.depthWrite = 1; key.depthFunc = VK_COMPARE_OP_LESS_OR_EQUAL;
		key.colorMask = 0; key.cull = 1;
		VkPipeline pipe = GetPipeline(key);
		if (!pipe) continue;
		DrawUbo u;
		memcpy(&u, &c.u, sizeof(u));
		float wvp[16];
		Multiply(c.u.world, lightVP, wvp);
		memcpy(u.wvp, wvp, 64);
		u.viewport[2] = (float)m->width; u.viewport[3] = (float)m->height;
		u.lightFlags[0] = 0;
		const VkDeviceSize uOff = RingTake(sizeof(DrawUbo), B.uboAlign);
		if (uOff == ~0ull) break;
		memcpy((char*)B.ringAlloc.mapped + uOff, &u, sizeof(u));
		if (pipe != last) { vkCmdBindPipeline(B.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); last = pipe; }
		VkDescriptorBufferInfo ubo{ B.ring, uOff, sizeof(DrawUbo) };
		VkDescriptorImageInfo imgs[4];
		VkWriteDescriptorSet w[6] = {};
		VkDescriptorImageInfo depthImg{ samp, B.white->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ubo;
		for (int s = 0; s < 4; ++s)
		{
			GpuTexture* g = (s == 0 && c.tex0) ? c.tex0 : B.white;
			imgs[s] = { samp, g->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
			w[1 + s].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1 + s].dstBinding = 1 + s; w[1 + s].descriptorCount = 1;
			w[1 + s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1 + s].pImageInfo = &imgs[s];
		}
		w[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[5].dstBinding = 8; w[5].descriptorCount = 1;
		w[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[5].pImageInfo = &depthImg;
		vkCmdPushDescriptorSetKHR(B.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, B.pipeLayout, 0, 6, w);
		VkBuffer bufs[2] = { B.ring, B.defaults };
		VkDeviceSize offs[2] = { c.vOff, 0 };
		vkCmdBindVertexBuffers(B.cmd, 0, 2, bufs, offs);
		if (c.indexCount)
		{
			vkCmdBindIndexBuffer(B.cmd, B.ring, c.iOff, c.wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
			vkCmdDrawIndexed(B.cmd, c.indexCount, 1, 0, -c.minIndex, 0);
		}
		else
			vkCmdDraw(B.cmd, c.numVertices, 1, 0, 0);
	}
	vkCmdEndRendering(B.cmd);
	ImageBarrier(B.cmd, m->image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT,
		VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	return true;
}

// Shadow map of the strongest light pulse: a perspective depth map from a virtual lamp above the light, looking straight down. Fills the light matrix and the
// depth constants for the lights pass (lightVP, shadowParams: size, A, B with depth = A + B / distance). Returns null when there is nothing to do.
GpuTexture* RenderLightShadow(PostUbo& c)
{
	c.shadowParams[0] = 0.0f;
	if (!g_sh.lmap || g_sh.casters.empty() || c.lightCfg[0] < 0.5f || c.lightPos[0][3] < 20.0f)
		return nullptr;
	const float lx = c.lightPos[0][0], ly = c.lightPos[0][1], range = c.lightPos[0][3];
	const float h = 0.65f * range;
	const float lampZ = c.lightPos[0][2] + h;
	const float nearD = 3.0f, farD = h + 90.0f;
	const float A = farD / (farD - nearD), Bq = -farD * nearD / (farD - nearD);
	const float sx = 1.0f / 1.92f;
	float vp[16] = {};
	vp[0] = sx; vp[5] = sx; vp[10] = -A; vp[11] = -1.0f;
	vp[12] = -lx * sx; vp[13] = -ly * sx; vp[14] = A * lampZ + Bq; vp[15] = lampZ;
	const float center[3] = { lx, ly, c.lightPos[0][2] };
	if (!RenderShadowMap(g_sh.lmap, vp, center, range * 1.3f + 70.0f))
		return nullptr;
	memcpy(c.lightVP, vp, 64);
	c.shadowParams[0] = (float)g_sh.lmap->width; c.shadowParams[1] = A; c.shadowParams[2] = Bq;
	return g_sh.lmap;
}

// ---- receiver pass --------------------------------------------------------------------------------------------------------

// Renders the shadow map and the full screen visibility image (1 = lit). The scene depth must already be readable. Returns the image or null.
GpuTexture* ShadowStage(const PostUbo& base)
{
	{ static int n = 0; if (n++ % 600 == 0) Log("shadow stage: enabled %d map %d vis %d casters %u proj %d depthTex %d sun %.2f %.2f %.2f ground %.1f", (int)ShadowsEnabled(), g_sh.map != nullptr, g_sh.vis != nullptr, (unsigned)g_sh.casters.size(), (int)B.haveProj, B.depthTex != nullptr, g_sh.sun[0], g_sh.sun[1], g_sh.sun[2], g_sh.groundZ); }
	static const int dbg = getenv("GENERALS_SHDBG") ? atoi(getenv("GENERALS_SHDBG")) : 0;
	const bool mapOk = ShadowsEnabled() && g_sh.map && !g_sh.casters.empty();
	const bool cloudOnly = !mapOk && g_cloud.shadows && B.postOn && g_sh.haveSun;
	if ((dbg & 4) || (!mapOk && !cloudOnly) || !g_sh.vis || !B.haveProj || !B.depthTex)
		return nullptr;
	float invView[16];
	RigidInverse(B.lastView, invView);
	// ground height: average of what the draws of this frame stand on
	{
		double sum = 0; int n = 0;
		for (const ShadowCaster& c : g_sh.casters)
			if (c.groundZ > -500.0f && c.groundZ < 2000.0f && c.groundZ != 0.0f) { sum += c.groundZ; ++n; if (n >= 400) break; }
		if (n) g_sh.groundZ = g_sh.groundZ * 0.9f + (float)(sum / n) * 0.1f;
	}
	{ static int k = 0; if (k++ % 300 == 0) { const float* P = B.lastProj; const float* V = B.lastView; Log("cam proj %.3f %.3f %.3f %.3f %.3f %.3f | view row3 %.1f %.1f %.1f | row0 %.2f %.2f %.2f row1 %.2f %.2f %.2f row2 %.2f %.2f %.2f", P[0], P[5], P[10], P[11], P[14], P[15], V[12], V[13], V[14], V[0], V[1], V[2], V[4], V[5], V[6], V[8], V[9], V[10]); } }
	float vp[16], range, mapWorld;
	range = 1.0f; mapWorld = 1.0f;
	if (mapOk)
	{
		if (!BuildLightMatrix(B.lastProj, invView, g_sh.groundZ, vp, range, mapWorld))
			return nullptr;
		memcpy(g_sh.lightVP, vp, 64);
		if (!RenderShadowMap(g_sh.map, vp))
			return nullptr;
	}
	else
		memset(vp, 0, sizeof(vp));

	PostUbo a = base;
	a.proj[0] = B.lastProj[0]; a.proj[1] = B.lastProj[5]; a.proj[2] = B.lastProj[10]; a.proj[3] = B.lastProj[14];
	a.p2[3] = B.lastProj[11] < 0.0f ? -1.0f : 1.0f; memcpy(a.viewRect, B.lastViewport, 16);
	memcpy(a.invView, invView, 64);
	memcpy(a.lightVP, vp, 64);
	FillCloudUbo(a, invView);
	a.p0[3] = g_sh.strength;
	a.shadowParams[0] = mapOk ? (float)g_sh.mapSize : 0.0f; a.shadowParams[1] = g_sh.lightSize; a.shadowParams[2] = range; a.shadowParams[3] = mapWorld;
	GpuTexture* in[4] = { B.depthTex, mapOk ? g_sh.map : nullptr, nullptr, nullptr };
	PostDraw(PASS_SHADOW, false, in, a, g_sh.vis->image, g_sh.vis->view, g_sh.vis->format, B.extent, VK_IMAGE_LAYOUT_UNDEFINED, false);
	g_sh.vis->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ToSampled(g_sh.vis);
	return g_sh.vis;
}
