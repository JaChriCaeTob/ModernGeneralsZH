/*
**	Native Vulkan backend (experimental).
**
**	Implements the Direct3D 8 interfaces the engine talks to (device, textures, surfaces, buffers) directly on Vulkan 1.3, without
**	DXVK or any Direct3D library. Selected with the environment variable GENERALS_GFX=vulkan.
**
**	What it does today: window surface and swapchain, per frame clear and present, GPU textures uploaded on demand from the
**	in-memory copies, a fixed-function emulation shader pair (texture stage combiner, alpha test, pre-transformed and
**	transformed vertices), blend / depth / cull state as pipeline state.
**	What it does not do yet (those draws are skipped): programmable vertex and pixel shaders, stencil, fog, lighting, render
**	targets other than the back buffer, volume and cube textures, screen captures of the back buffer.
**
**	See docs/PORT_NOTES.md for the status and the list of milestones.
*/
#include "WWLib/always.h"
#include "gfx_vulkan.h"

#if defined(VK_NO_PROTOTYPES) && defined(VK_USE_PLATFORM_WIN32_KHR)

#include <vulkan/vulkan.h>
#include "gfx_mem.h"
#include "gfx_vk_shaders.h"
#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>
#include <algorithm>

using namespace gfxmem;

namespace
{

// ---- log ------------------------------------------------------------------------------------------------------------------

void Log(const char* fmt, ...)
{
	static FILE* f = nullptr;
	if (!f)
		f = fopen("gfx_vulkan.log", "w");
	if (!f)
		return;
	va_list a;
	va_start(a, fmt);
	vfprintf(f, fmt, a);
	va_end(a);
	fputc('\n', f);
	fflush(f);
}

// ---- function loading -----------------------------------------------------------------------------------------------------

#define VKFN_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VKFN_INSTANCE(X) \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
	X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) \
	X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
	X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VKFN_DEVICE(X) \
	X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
	X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) \
	X(vkDestroySemaphore) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
	X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) \
	X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateSampler) X(vkDestroySampler) \
	X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
	X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCmdBeginRendering) \
	X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdBindVertexBuffers) \
	X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdDraw) X(vkCmdPipelineBarrier2) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdCopyImage) \
	X(vkCmdClearAttachments) X(vkCmdSetStencilReference)

#define VKFN_DECLARE(n) PFN_##n n = nullptr;
VKFN_GLOBAL(VKFN_DECLARE)
VKFN_INSTANCE(VKFN_DECLARE)
VKFN_DEVICE(VKFN_DECLARE)
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR = nullptr;

// ---- state ----------------------------------------------------------------------------------------------------------------

struct Block
{
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
	void* mapped = nullptr;
	uint32_t type = 0;
	bool dedicated = false;
	std::vector<std::pair<VkDeviceSize, VkDeviceSize>> freeList;	// offset, size
};

struct Allocation
{
	int block = -1;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	VkDeviceSize size = 0;
	void* mapped = nullptr;
};

struct GpuTexture
{
	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	Allocation alloc;
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t width = 0, height = 0, levels = 0;
	bool isDepth = false;
	bool renderTarget = false, loaded = false;		// render target textures live on the GPU only; loaded once they hold rendered content
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;	// tracked for render targets
};

struct PipeKey
{
	uint32_t fvf = 0, stride = 0, topology = 0;
	uint32_t blendEnable = 0, srcBlend = 0, dstBlend = 0, blendOp = 0;
	uint32_t depthTest = 0, depthWrite = 0, depthFunc = 0;
	uint32_t cull = 0, colorMask = 0, pretransformed = 0;
	uint32_t shadow = 0;		// depth only pass into the shadow map
	uint32_t hdr = 0;		// 1 when drawing into the HDR scene image instead of an image of the swapchain format
	uint32_t ps = 0;		// 0 fixed function, 1 updated river water, 2 updated sea water
	uint32_t stencilEnable = 0, stencilFunc = 0, stencilFail = 0, stencilZFail = 0, stencilPass = 0, stencilMask = 0, stencilWriteMask = 0;
	bool operator<(const PipeKey& o) const { return memcmp(this, &o, sizeof(*this)) < 0; }
};

struct DrawUbo
{
	float wvp[16];
	float viewport[4];
	float textureFactor[4];
	uint32_t flags[4];
	float alphaRef[4];
	uint32_t stage[8][4];
	float world[16];
	float matDiffuse[4], matAmbient[4], matEmissive[4], sceneAmbient[4];
	uint32_t lightFlags[4];
	uint32_t lightInfo[4];
	float lightDiffuse[4][4], lightAmbient[4][4], lightPosType[4][4], lightDirRange[4][4], lightAtten[4][4];
	float worldView[16];
	float texMatrix[8][16];
	uint32_t texGen[8][4];
	float pointParams[4];
	float pointScale[4];
};
static_assert(sizeof(DrawUbo) == 256 + 64 + 64 + 16 + 16 + 5 * 64 + 64 + 8 * 64 + 8 * 16 + 32, "DrawUbo must match the std140 block in the shaders");

uint64_t g_uploads = 0, g_uploadBytes = 0, g_uploadFrameMark = 0;

struct Backend
{
	HMODULE lib = nullptr;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memProps{};
	VkPhysicalDeviceProperties props{};
	VkPhysicalDeviceFeatures features{};
	VkDevice device = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	HWND window = nullptr;

	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
	VkExtent2D extent{};
	std::vector<VkImage> swapImages;
	std::vector<VkImageView> swapViews;
	std::vector<VkSemaphore> renderDone;
	VkImage depthImage = VK_NULL_HANDLE;
	VkImageView depthView = VK_NULL_HANDLE;
	Allocation depthAlloc;

	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkCommandBuffer uploadCmd = VK_NULL_HANDLE;		// texture uploads of the frame, submitted before the frame's own commands
	bool uploadOpen = false;
	VkDeviceSize pendingStaging = 0;
	VkFence frameFence = VK_NULL_HANDLE;
	VkSemaphore imageAvailable = VK_NULL_HANDLE;
	uint32_t imageIndex = 0;
	bool acquired = false, rendering = false, cmdOpen = false;
	GpuTexture* curTarget = nullptr;				// where the device draws: nullptr = back buffer, else a render target texture
	GpuTexture* passTarget = nullptr;				// target of the open render pass
	bool targetUnsupported = false;					// the device target is a surface we cannot draw to: draws are skipped
	VkExtent2D passExtent{};
	GpuTexture* offDepth = nullptr;					// depth/stencil shared by all off-screen passes
	// HDR scene and post processing (gfx_vk_post.inl)
	GpuTexture* hdr = nullptr; GpuTexture* ldr = nullptr; GpuTexture* bloom[5] = {};
	GpuTexture* ao[2] = {}; GpuTexture* depthTex = nullptr; VkImageView depthSampleView = VK_NULL_HANDLE;
	float lastViewport[4] = {};
	float lastProj[16] = {}, lastView[16] = {}; bool haveProj = false;
	VkShaderModule postVs = VK_NULL_HANDLE, postFs[8] = {};
	std::map<uint64_t, VkPipeline> postPipes;
	bool postOn = true;								// false: draw straight to the swapchain
	int stage = 1;									// 0: 3D scene into the HDR image (between VkGfx_BeginScene3D and EndScene3D), 1: straight onto the swapchain
	bool scene3D = false;							// a 3D draw happened in this frame
	bool projCaptured = false;
	bool depthLoaded = false;						// the depth buffer already holds this frame's scene
	bool colorLoaded = false;						// the frame already has content in the swapchain image (resume with load, not clear)
	bool semaphoreUsed = false;						// imageAvailable was already waited on by an earlier submit of this frame
	VkBuffer readBuf = VK_NULL_HANDLE;				// host visible destination of back buffer read backs
	Allocation readAlloc;
	VkDeviceSize readSize = 0;

	VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;
	VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, fsWater[2] = {};
	std::map<PipeKey, VkPipeline> pipelines;
	std::map<uint64_t, VkSampler> samplers;

	// per frame data (vertices, indices, uniforms) and default vertex attributes
	VkBuffer ring = VK_NULL_HANDLE;
	Allocation ringAlloc;
	VkDeviceSize ringSize = 0, ringCursor = 0;
	VkBuffer defaults = VK_NULL_HANDLE;
	Allocation defaultsAlloc;
	VkDeviceSize uboAlign = 256;

	GpuTexture* white = nullptr;			// bound where a stage has no texture

	std::vector<Block> blocks;
	std::vector<GpuTexture*> deferredTextures;
	std::vector<std::pair<VkBuffer, Allocation>> deferredBuffers;

	bool ready = false;
	bool anisotropy = false;
};
void EndPass();

Backend B;

#define VKCHECK(call) do { VkResult _r = (call); if (_r != VK_SUCCESS) Log("Vulkan error %d at %s (%s:%d)", (int)_r, #call, __FILE__, __LINE__); } while (0)

// ---- memory ---------------------------------------------------------------------------------------------------------------

bool FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want, uint32_t& out)
{
	for (uint32_t i = 0; i < B.memProps.memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (B.memProps.memoryTypes[i].propertyFlags & want) == want)
		{
			out = i;
			return true;
		}
	return false;
}

Allocation Allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags props, bool map)
{
	Allocation a;
	uint32_t type;
	if (!FindMemoryType(req.memoryTypeBits, props, type))
	{
		if (!FindMemoryType(req.memoryTypeBits, props & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
			return a;
	}
	const VkDeviceSize kBlock = 128ull * 1024 * 1024;
	if (req.size <= kBlock / 2)
	{
		for (size_t bi = 0; bi < B.blocks.size(); ++bi)
		{
			Block& bl = B.blocks[bi];
			if (bl.type != type || bl.dedicated || (map && !bl.mapped) || (!map && bl.mapped))
				continue;
			for (size_t fi = 0; fi < bl.freeList.size(); ++fi)
			{
				VkDeviceSize off = (bl.freeList[fi].first + req.alignment - 1) & ~(req.alignment - 1);
				VkDeviceSize pad = off - bl.freeList[fi].first;
				if (bl.freeList[fi].second >= pad + req.size)
				{
					VkDeviceSize rest = bl.freeList[fi].second - pad - req.size;
					VkDeviceSize start = bl.freeList[fi].first;
					bl.freeList.erase(bl.freeList.begin() + fi);
					if (pad) bl.freeList.push_back({ start, pad });
					if (rest) bl.freeList.push_back({ off + req.size, rest });
					a.block = (int)bi; a.memory = bl.memory; a.offset = off; a.size = req.size;
					a.mapped = bl.mapped ? (char*)bl.mapped + off : nullptr;
					return a;
				}
			}
		}
	}
	Block nb;
	nb.type = type;
	nb.dedicated = req.size > kBlock / 2;
	nb.size = nb.dedicated ? req.size : kBlock;
	VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	mai.allocationSize = nb.size;
	mai.memoryTypeIndex = type;
	if (vkAllocateMemory(B.device, &mai, nullptr, &nb.memory) != VK_SUCCESS)
	{
		Log("vkAllocateMemory failed for %llu bytes", (unsigned long long)nb.size);
		return a;
	}
	if (map && vkMapMemory(B.device, nb.memory, 0, VK_WHOLE_SIZE, 0, &nb.mapped) != VK_SUCCESS)
		nb.mapped = nullptr;
	if (!nb.dedicated)
		nb.freeList.push_back({ req.size, nb.size - req.size });
	B.blocks.push_back(nb);
	a.block = (int)B.blocks.size() - 1; a.memory = nb.memory; a.offset = 0; a.size = req.size;
	a.mapped = nb.mapped;
	return a;
}

void Free(Allocation& a)
{
	if (a.block < 0)
		return;
	Block& bl = B.blocks[a.block];
	if (bl.dedicated)
	{
		vkFreeMemory(B.device, bl.memory, nullptr);
		bl.memory = VK_NULL_HANDLE;
	}
	else
	{
		bl.freeList.push_back({ a.offset, a.size });
		std::sort(bl.freeList.begin(), bl.freeList.end());
		std::vector<std::pair<VkDeviceSize, VkDeviceSize>> merged;
		for (auto& r : bl.freeList)
		{
			if (!merged.empty() && merged.back().first + merged.back().second == r.first)
				merged.back().second += r.second;
			else
				merged.push_back(r);
		}
		bl.freeList.swap(merged);
	}
	a = Allocation();
}

bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buf, Allocation& alloc)
{
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(B.device, &ci, nullptr, &buf) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(B.device, buf, &req);
	alloc = Allocate(req, props, (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0);
	if (alloc.block < 0)
	{
		vkDestroyBuffer(B.device, buf, nullptr);
		buf = VK_NULL_HANDLE;
		return false;
	}
	vkBindBufferMemory(B.device, buf, alloc.memory, alloc.offset);
	return true;
}

// ---- command helpers ------------------------------------------------------------------------------------------------------

void ImageBarrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect,
	VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
	uint32_t levels = 1)
{
	VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	b.srcStageMask = srcStage; b.srcAccessMask = srcAccess; b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
	b.oldLayout = from; b.newLayout = to; b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img; b.subresourceRange = { aspect, 0, levels, 0, 1 };
	VkDependencyInfo d{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cb, &d);
}

void BeginCommandBuffer()
{
	VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VKCHECK(vkBeginCommandBuffer(B.cmd, &bi));
	B.cmdOpen = true;
}

bool CreatePostTargets();
void DestroyPostTargets();
bool AcquireSwap();
bool RunPostProcess();
bool CreatePostShaders();

// ---- swapchain ------------------------------------------------------------------------------------------------------------

void DestroySwapchain()
{
	if (B.device == VK_NULL_HANDLE)
		return;
	vkDeviceWaitIdle(B.device);
	DestroyPostTargets();
	for (VkSemaphore s : B.renderDone) vkDestroySemaphore(B.device, s, nullptr);
	B.renderDone.clear();
	for (VkImageView v : B.swapViews) vkDestroyImageView(B.device, v, nullptr);
	B.swapViews.clear();
	B.swapImages.clear();
	if (B.depthView) { vkDestroyImageView(B.device, B.depthView, nullptr); B.depthView = VK_NULL_HANDLE; }
	if (B.depthImage) { vkDestroyImage(B.device, B.depthImage, nullptr); B.depthImage = VK_NULL_HANDLE; Free(B.depthAlloc); }
	if (B.swapchain) { vkDestroySwapchainKHR(B.device, B.swapchain, nullptr); B.swapchain = VK_NULL_HANDLE; }
}

bool CreateSwapchain()
{
	VkSurfaceCapabilitiesKHR caps;
	VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(B.phys, B.surface, &caps));
	RECT rc;
	GetClientRect(B.window, &rc);
	VkExtent2D ext = caps.currentExtent;
	if (ext.width == 0xFFFFFFFFu)
	{
		ext.width = std::max<uint32_t>(caps.minImageExtent.width, std::min<uint32_t>(caps.maxImageExtent.width, (uint32_t)(rc.right - rc.left)));
		ext.height = std::max<uint32_t>(caps.minImageExtent.height, std::min<uint32_t>(caps.maxImageExtent.height, (uint32_t)(rc.bottom - rc.top)));
	}
	if (ext.width == 0 || ext.height == 0)
		return false;		// minimised

	uint32_t n = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(B.phys, B.surface, &n, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(n);
	vkGetPhysicalDeviceSurfaceFormatsKHR(B.phys, B.surface, &n, formats.data());
	VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
	for (auto& f : formats)
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			chosen = f;

	vkGetPhysicalDeviceSurfacePresentModesKHR(B.phys, B.surface, &n, nullptr);
	std::vector<VkPresentModeKHR> modes(n);
	vkGetPhysicalDeviceSurfacePresentModesKHR(B.phys, B.surface, &n, modes.data());
	VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
	for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
	for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;

	VkSwapchainCreateInfoKHR sc{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	sc.surface = B.surface;
	sc.minImageCount = std::max(2u, caps.minImageCount);
	if (caps.maxImageCount && sc.minImageCount > caps.maxImageCount) sc.minImageCount = caps.maxImageCount;
	sc.imageFormat = chosen.format; sc.imageColorSpace = chosen.colorSpace; sc.imageExtent = ext; sc.imageArrayLayers = 1;
	sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sc.preTransform = caps.currentTransform;
	sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	sc.presentMode = mode; sc.clipped = VK_TRUE;
	if (vkCreateSwapchainKHR(B.device, &sc, nullptr, &B.swapchain) != VK_SUCCESS)
		return false;
	B.swapFormat = chosen.format; B.extent = ext;

	vkGetSwapchainImagesKHR(B.device, B.swapchain, &n, nullptr);
	B.swapImages.resize(n);
	vkGetSwapchainImagesKHR(B.device, B.swapchain, &n, B.swapImages.data());
	for (VkImage img : B.swapImages)
	{
		VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = B.swapFormat;
		vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageView v; VKCHECK(vkCreateImageView(B.device, &vi, nullptr, &v)); B.swapViews.push_back(v);
		VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VkSemaphore s; VKCHECK(vkCreateSemaphore(B.device, &si, nullptr, &s)); B.renderDone.push_back(s);
	}

	VkImageCreateInfo di{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	di.imageType = VK_IMAGE_TYPE_2D; di.format = VK_FORMAT_D32_SFLOAT_S8_UINT; di.extent = { ext.width, ext.height, 1 };
	di.mipLevels = 1; di.arrayLayers = 1; di.samples = VK_SAMPLE_COUNT_1_BIT; di.tiling = VK_IMAGE_TILING_OPTIMAL;
	di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; di.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VKCHECK(vkCreateImage(B.device, &di, nullptr, &B.depthImage));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(B.device, B.depthImage, &req);
	B.depthAlloc = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
	vkBindImageMemory(B.device, B.depthImage, B.depthAlloc.memory, B.depthAlloc.offset);
	VkImageViewCreateInfo dv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	dv.image = B.depthImage; dv.viewType = VK_IMAGE_VIEW_TYPE_2D; dv.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
	dv.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
	VKCHECK(vkCreateImageView(B.device, &dv, nullptr, &B.depthView));
	Log("swapchain %ux%u, %u images, format %d, present mode %d", ext.width, ext.height, n, (int)B.swapFormat, (int)mode);
	if (B.postOn)
		CreatePostTargets();
	return true;
}

// ---- textures -------------------------------------------------------------------------------------------------------------

struct FormatInfo { VkFormat vk; VkComponentMapping swizzle; };

bool MapFormat(D3DFORMAT f, FormatInfo& out)
{
	const VkComponentSwizzle I = VK_COMPONENT_SWIZZLE_IDENTITY, R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G;
	const VkComponentSwizzle ONE = VK_COMPONENT_SWIZZLE_ONE, ZERO = VK_COMPONENT_SWIZZLE_ZERO;
	switch (f)
	{
	case D3DFMT_A8R8G8B8: out = { VK_FORMAT_B8G8R8A8_UNORM, { I, I, I, I } }; return true;
	case D3DFMT_R8G8B8: out = { VK_FORMAT_B8G8R8A8_UNORM, { I, I, I, ONE } }; return true;		// expanded to 4 bytes on upload
	case D3DFMT_X8R8G8B8: out = { VK_FORMAT_B8G8R8A8_UNORM, { I, I, I, ONE } }; return true;
	case D3DFMT_R5G6B5: out = { VK_FORMAT_R5G6B5_UNORM_PACK16, { I, I, I, ONE } }; return true;
	case D3DFMT_A1R5G5B5: out = { VK_FORMAT_A1R5G5B5_UNORM_PACK16, { I, I, I, I } }; return true;
	case D3DFMT_X1R5G5B5: out = { VK_FORMAT_A1R5G5B5_UNORM_PACK16, { I, I, I, ONE } }; return true;
	case D3DFMT_A4R4G4B4: out = { VK_FORMAT_A4R4G4B4_UNORM_PACK16, { I, I, I, I } }; return true;
	case D3DFMT_L8: out = { VK_FORMAT_R8_UNORM, { R, R, R, ONE } }; return true;
	case D3DFMT_A8: out = { VK_FORMAT_R8_UNORM, { ZERO, ZERO, ZERO, R } }; return true;
	case D3DFMT_A8L8: out = { VK_FORMAT_R8G8_UNORM, { R, R, R, G } }; return true;
	case D3DFMT_DXT1: out = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, { I, I, I, I } }; return true;
	case D3DFMT_DXT2: case D3DFMT_DXT3: out = { VK_FORMAT_BC2_UNORM_BLOCK, { I, I, I, I } }; return true;
	case D3DFMT_DXT4: case D3DFMT_DXT5: out = { VK_FORMAT_BC3_UNORM_BLOCK, { I, I, I, I } }; return true;
	default: return false;
	}
}

GpuTexture* CreateGpuTexture(uint32_t w, uint32_t h, uint32_t levels, const FormatInfo& fi, bool rt = false)
{
	GpuTexture* t = new GpuTexture();
	t->renderTarget = rt;
	t->format = fi.vk; t->width = w; t->height = h; t->levels = levels;
	VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fi.vk; ci.extent = { w, h, 1 }; ci.mipLevels = levels; ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (rt ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0); ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(B.device, &ci, nullptr, &t->image) != VK_SUCCESS) { delete t; return nullptr; }
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(B.device, t->image, &req);
	t->alloc = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
	if (t->alloc.block < 0) { vkDestroyImage(B.device, t->image, nullptr); delete t; return nullptr; }
	vkBindImageMemory(B.device, t->image, t->alloc.memory, t->alloc.offset);
	VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = t->image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fi.vk; vi.components = fi.swizzle;
	vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
	VKCHECK(vkCreateImageView(B.device, &vi, nullptr, &t->view));
	return t;
}

void FreeDeferredBuffers()
{
	for (auto& d : B.deferredBuffers)
	{
		vkDestroyBuffer(B.device, d.first, nullptr);
		Free(d.second);
	}
	B.deferredBuffers.clear();
	B.pendingStaging = 0;
}

// Submits the recorded uploads on their own and waits for them. Used when too much staging memory piles up before a Present.
void FlushUploadsSync()
{
	if (!B.uploadOpen)
		return;
	vkEndCommandBuffer(B.uploadCmd);
	B.uploadOpen = false;
	VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	vkCreateFence(B.device, &fci, nullptr, &fence);
	VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.commandBufferCount = 1; si.pCommandBuffers = &B.uploadCmd;
	vkQueueSubmit(B.queue, 1, &si, fence);
	vkWaitForFences(B.device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkDestroyFence(B.device, fence, nullptr);
	vkResetCommandBuffer(B.uploadCmd, 0);
	FreeDeferredBuffers();
}

void OpenUploadCmd()
{
	if (B.uploadOpen)
		return;
	vkResetCommandBuffer(B.uploadCmd, 0);
	VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(B.uploadCmd, &bi);
	B.uploadOpen = true;
}

// Records the upload of all levels into the frame's upload command buffer. The staging memory stays alive until the frame's fence.
void UploadLevels(GpuTexture* t, const std::vector<const NullSurface*>& levels)
{
	VkDeviceSize total = 0;
	std::vector<VkDeviceSize> offsets;
	auto sizeOf = [](const NullSurface* s) -> size_t { return s->m_format == D3DFMT_R8G8B8 ? s->m_data.size() / 3 * 4 : s->m_data.size(); };
	for (const NullSurface* s : levels) { offsets.push_back(total); total += (sizeOf(s) + 15) & ~15ull; }
	++g_uploads; g_uploadBytes += total;
	if (B.pendingStaging + total > 256ull * 1024 * 1024)
		FlushUploadsSync();
	VkBuffer staging; Allocation sa;
	if (!CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, sa))
	{
		Log("staging buffer of %llu bytes failed", (unsigned long long)total);
		return;
	}
	for (size_t i = 0; i < levels.size(); ++i)
		if (levels[i]->m_format == D3DFMT_R8G8B8)
		{
			uint8_t* d = (uint8_t*)sa.mapped + offsets[i];
			const uint8_t* s = levels[i]->m_data.data();
			for (size_t k = 0, n = levels[i]->m_data.size() / 3; k < n; ++k) { d[4 * k] = s[3 * k]; d[4 * k + 1] = s[3 * k + 1]; d[4 * k + 2] = s[3 * k + 2]; d[4 * k + 3] = 255; }
		}
		else
			memcpy((char*)sa.mapped + offsets[i], levels[i]->m_data.data(), levels[i]->m_data.size());
	B.deferredBuffers.push_back({ staging, sa });
	B.pendingStaging += total;

	if (!B.uploadOpen)
	{
		vkResetCommandBuffer(B.uploadCmd, 0);
		VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(B.uploadCmd, &bi);
		B.uploadOpen = true;
	}
	VkCommandBuffer cb = B.uploadCmd;
	ImageBarrier(cb, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, t->levels);
	std::vector<VkBufferImageCopy> regions;
	for (size_t i = 0; i < levels.size() && i < t->levels; ++i)
	{
		VkBufferImageCopy r{};
		r.bufferOffset = offsets[i];
		r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)i, 0, 1 };
		r.imageExtent = { levels[i]->m_width, levels[i]->m_height, 1 };
		regions.push_back(r);
	}
	vkCmdCopyBufferToImage(cb, staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
	ImageBarrier(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, t->levels);
}

uint64_t HashLevels(const std::vector<NullSurface*>& levels)
{
	uint64_t h = 1469598103934665603ull;
	for (const NullSurface* s : levels)
	{
		const uint64_t* w = (const uint64_t*)s->m_data.data();
		const size_t n = s->m_data.size() / 8;
		for (size_t i = 0; i < n; ++i)
			h = (h ^ w[i]) * 1099511628211ull;
		for (size_t i = n * 8; i < s->m_data.size(); ++i)
			h = (h ^ s->m_data[i]) * 1099511628211ull;
	}
	return h;
}

GpuTexture* EnsureGpuTexture(NullTexture* tex)
{
	if (!tex)
		return B.white;
	GpuTexture* g = (GpuTexture*)tex->m_gpu;
	if (g && (!tex->m_dirty || g->renderTarget))
		return g;
	FormatInfo fi;
	NullSurface* l0 = tex->m_levels[0];
	if (!MapFormat(l0->m_format, fi))
	{
		static int n = 0; if (n++ < 20) Log("texture format %d (%ux%u) not supported, drawn white", (int)l0->m_format, l0->m_width, l0->m_height);
		return B.white;
	}
	if (l0->m_usage & D3DUSAGE_RENDERTARGET)
	{
		// off-screen render target: an image that is only ever drawn to and sampled, matching the swapchain format
		const VkComponentSwizzle I = VK_COMPONENT_SWIZZLE_IDENTITY;
		fi.vk = B.swapFormat; fi.swizzle = { I, I, I, I };
		g = CreateGpuTexture(l0->m_width, l0->m_height, 1, fi, true);
		if (!g)
			return B.white;
		tex->m_gpu = g; tex->m_dirty = false;
		OpenUploadCmd();
		ImageBarrier(B.uploadCmd, g->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		g->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		return g;
	}
	if (g && (g->width != l0->m_width || g->height != l0->m_height || g->levels != tex->m_levels.size() || g->format != fi.vk))
	{
		B.deferredTextures.push_back(g);
		g = nullptr;
		tex->m_gpu = nullptr;
	}
	if (!g)
	{
		g = CreateGpuTexture(l0->m_width, l0->m_height, (uint32_t)tex->m_levels.size(), fi);
		if (!g)
			return B.white;
		tex->m_gpu = g;
		tex->m_hash = 0;		// a new image has to be filled
	}
	const uint64_t hash = HashLevels(tex->m_levels);
	const bool unchanged = tex->m_hash == hash;
	tex->m_dirty = false;
	if (unchanged)
		return g;
	std::vector<const NullSurface*> lv(tex->m_levels.begin(), tex->m_levels.end());
	UploadLevels(g, lv);
	tex->m_hash = hash;
	return g;
}

void DestroyGpuTexture(GpuTexture* g)
{
	if (g->view) vkDestroyImageView(B.device, g->view, nullptr);
	if (g->image) vkDestroyImage(B.device, g->image, nullptr);
	Free(g->alloc);
	delete g;
}

void OnTextureDestroyed(NullTexture* tex)
{
	GpuTexture* g = (GpuTexture*)tex->m_gpu;
	if (g && g->renderTarget)
	{
		if (B.rendering && B.passTarget == g)
			EndPass();
		if (B.curTarget == g) { B.curTarget = nullptr; B.targetUnsupported = true; }
		if (B.passTarget == g) B.passTarget = nullptr;
	}
	if (tex->m_gpu && B.ready)
		B.deferredTextures.push_back((GpuTexture*)tex->m_gpu);
	tex->m_gpu = nullptr;
}

VkSampler GetSampler(DWORD minF, DWORD magF, DWORD mipF, DWORD addrU, DWORD addrV, DWORD maxAniso)
{
	uint64_t key = (uint64_t)minF | ((uint64_t)magF << 4) | ((uint64_t)mipF << 8) | ((uint64_t)addrU << 12) | ((uint64_t)addrV << 16) | ((uint64_t)maxAniso << 20);
	auto it = B.samplers.find(key);
	if (it != B.samplers.end())
		return it->second;
	auto filter = [](DWORD f) { return f == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; };
	auto address = [](DWORD a) {
		switch (a) { case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; case 5: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
		default: return VK_SAMPLER_ADDRESS_MODE_REPEAT; } };
	VkSamplerCreateInfo ci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	ci.minFilter = filter(minF); ci.magFilter = filter(magF);
	ci.mipmapMode = mipF == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	ci.addressModeU = address(addrU); ci.addressModeV = address(addrV); ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	ci.minLod = 0.0f; ci.maxLod = (mipF == 0) ? 0.25f : VK_LOD_CLAMP_NONE;	// no mip filter: base level only
	if (B.anisotropy && (minF == 3 || magF == 3) && maxAniso > 1)
	{
		ci.anisotropyEnable = VK_TRUE;
		ci.maxAnisotropy = std::min<float>((float)maxAniso, B.props.limits.maxSamplerAnisotropy);
	}
	ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	VkSampler s = VK_NULL_HANDLE;
	VKCHECK(vkCreateSampler(B.device, &ci, nullptr, &s));
	B.samplers[key] = s;
	return s;
}

// ---- pipelines ------------------------------------------------------------------------------------------------------------

VkBlendFactor BlendFactor(DWORD d3d)
{
	switch (d3d)
	{
	case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR;
	case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
	case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_DST_ALPHA;
	case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; case 9: return VK_BLEND_FACTOR_DST_COLOR;
	case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	default: return VK_BLEND_FACTOR_ONE;
	}
}

VkBlendOp BlendOperation(DWORD d3d)
{
	switch (d3d) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN;
		case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
}

VkFormat TexcoordFormat(DWORD fvf, int set)
{
	switch ((fvf >> (16 + 2 * set)) & 3)
	{
	case 1: return VK_FORMAT_R32G32B32_SFLOAT;
	case 2: return VK_FORMAT_R32G32B32A32_SFLOAT;
	case 3: return VK_FORMAT_R32_SFLOAT;
	default: return VK_FORMAT_R32G32_SFLOAT;
	}
}

UINT TexcoordSize(DWORD fvf, int set)
{
	switch ((fvf >> (16 + 2 * set)) & 3) { case 1: return 12; case 2: return 16; case 3: return 4; default: return 8; }
}

VkPipeline GetPipeline(const PipeKey& key)
{
	auto it = B.pipelines.find(key);
	if (it != B.pipelines.end())
		return it->second;

	// vertex layout from the FVF code; attributes the vertices do not carry come from a constant buffer (binding 1, stride 0)
	std::vector<VkVertexInputAttributeDescription> attrs(8);
	for (uint32_t i = 0; i < 8; ++i) attrs[i] = { i, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
	attrs[1] = { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 };		// normal 0,0,1
	attrs[2] = { 2, 1, VK_FORMAT_B8G8R8A8_UNORM, 32 };			// white
	attrs[3] = { 3, 1, VK_FORMAT_B8G8R8A8_UNORM, 48 };			// black
	const DWORD fvf = key.fvf;
	UINT off = 0;
	const DWORD pos = fvf & 0xE;
	if (pos == 0x4) { attrs[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }; off = 16; }
	else if (pos >= 0x2) { attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }; off = 12 + (pos > 0x2 ? ((pos - 4) / 2) * 4 : 0); }
	if (fvf & 0x10) { attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, off }; off += 12; }
	if (fvf & 0x20) off += 4;
	if (fvf & 0x40) { attrs[2] = { 2, 0, VK_FORMAT_B8G8R8A8_UNORM, off }; off += 4; }
	if (fvf & 0x80) { attrs[3] = { 3, 0, VK_FORMAT_B8G8R8A8_UNORM, off }; off += 4; }
	const int texCount = (int)((fvf >> 8) & 0xF);
	for (int i = 0; i < texCount && i < 4; ++i)
	{
		attrs[4 + i] = { (uint32_t)(4 + i), 0, TexcoordFormat(fvf, i), off };
		off += TexcoordSize(fvf, i);
	}
	VkVertexInputBindingDescription binds[2] = { { 0, key.stride ? key.stride : 16, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, 0, VK_VERTEX_INPUT_RATE_VERTEX } };
	VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vin.vertexBindingDescriptionCount = 2; vin.pVertexBindingDescriptions = binds;
	vin.vertexAttributeDescriptionCount = 8; vin.pVertexAttributeDescriptions = attrs.data();

	VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = (VkPrimitiveTopology)key.topology;
	VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1; vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
	rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rs.cullMode = key.cull == 2 ? VK_CULL_MODE_FRONT_BIT : key.cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;	// D3DCULL_CW culls clockwise faces
	VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	ds.depthTestEnable = key.depthTest; ds.depthWriteEnable = key.depthWrite; ds.depthCompareOp = (VkCompareOp)key.depthFunc;
	if (key.stencilEnable)
	{
		auto op = [](uint32_t d) {
			switch (d) { case 2: return VK_STENCIL_OP_ZERO; case 3: return VK_STENCIL_OP_REPLACE; case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
			case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP; case 6: return VK_STENCIL_OP_INVERT; case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
			case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP; } };
		VkStencilOpState so{};
		so.failOp = op(key.stencilFail); so.depthFailOp = op(key.stencilZFail); so.passOp = op(key.stencilPass);
		so.compareOp = (VkCompareOp)(key.stencilFunc ? key.stencilFunc - 1 : VK_COMPARE_OP_ALWAYS);
		so.compareMask = key.stencilMask; so.writeMask = key.stencilWriteMask;
		ds.stencilTestEnable = VK_TRUE; ds.front = so; ds.back = so;
	}
	VkPipelineColorBlendAttachmentState cba{};
	cba.blendEnable = key.blendEnable; cba.srcColorBlendFactor = BlendFactor(key.srcBlend); cba.dstColorBlendFactor = BlendFactor(key.dstBlend);
	cba.colorBlendOp = BlendOperation(key.blendOp); cba.srcAlphaBlendFactor = cba.srcColorBlendFactor; cba.dstAlphaBlendFactor = cba.dstColorBlendFactor;
	cba.alphaBlendOp = cba.colorBlendOp; cba.colorWriteMask = key.colorMask & 0xF;
	VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1; cb.pAttachments = &cba;
	VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
	VkPipelineDynamicStateCreateInfo dys{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dys.dynamicStateCount = 3; dys.pDynamicStates = dyn;
	VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = B.vs; stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = key.ps ? B.fsWater[key.ps - 1] : B.fs; stages[1].pName = "main";
	VkFormat colorFormat = key.hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : B.swapFormat;
	VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &colorFormat; ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; ri.stencilAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
	if (key.shadow)
	{
		ri.colorAttachmentCount = 0; ri.pColorAttachmentFormats = nullptr; ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT; ri.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
		cb.attachmentCount = 0; cb.pAttachments = nullptr;
		rs.cullMode = VK_CULL_MODE_NONE; rs.depthBiasEnable = VK_TRUE; rs.depthBiasConstantFactor = 2.0f; rs.depthBiasSlopeFactor = 2.5f;
	}
	VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stages; pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp; pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &cb; pi.pDynamicState = &dys; pi.layout = B.pipeLayout;
	VkPipeline p = VK_NULL_HANDLE;
	VKCHECK(vkCreateGraphicsPipelines(B.device, VK_NULL_HANDLE, 1, &pi, nullptr, &p));
	B.pipelines[key] = p;
	return p;
}

// ---- backend start-up -----------------------------------------------------------------------------------------------------

bool LoadVulkan()
{
	B.lib = LoadLibraryA("vulkan-1.dll");
	if (!B.lib) { Log("vulkan-1.dll not found"); return false; }
	vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(B.lib, "vkGetInstanceProcAddr");
	if (!vkGetInstanceProcAddr) return false;
#define VKFN_LOAD_GLOBAL(n) n = (PFN_##n)vkGetInstanceProcAddr(nullptr, #n);
	VKFN_GLOBAL(VKFN_LOAD_GLOBAL)
	return vkCreateInstance != nullptr;
}

bool CreateInstanceAndDevice(HWND window)
{
	B.window = window;
	const char* instExt[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "Generals Zero Hour"; app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.pApplicationInfo = &app; ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = instExt;
	if (vkCreateInstance(&ici, nullptr, &B.instance) != VK_SUCCESS) { Log("vkCreateInstance failed"); return false; }
#define VKFN_LOAD_INSTANCE(n) n = (PFN_##n)vkGetInstanceProcAddr(B.instance, #n);
	VKFN_INSTANCE(VKFN_LOAD_INSTANCE)

	VkWin32SurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
	sci.hinstance = GetModuleHandle(nullptr); sci.hwnd = window;
	if (vkCreateWin32SurfaceKHR(B.instance, &sci, nullptr, &B.surface) != VK_SUCCESS) { Log("surface creation failed"); return false; }

	uint32_t n = 0;
	vkEnumeratePhysicalDevices(B.instance, &n, nullptr);
	std::vector<VkPhysicalDevice> devs(n);
	vkEnumeratePhysicalDevices(B.instance, &n, devs.data());
	int bestScore = -1;
	for (VkPhysicalDevice d : devs)
	{
		VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
		if (p.apiVersion < VK_API_VERSION_1_3) continue;
		uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
		std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
		for (uint32_t q = 0; q < qn; ++q)
		{
			VkBool32 present = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(d, q, B.surface, &present);
			if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
			{
				int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
				if (score > bestScore) { bestScore = score; B.phys = d; B.family = q; }
				break;
			}
		}
	}
	if (!B.phys) { Log("no Vulkan 1.3 device with graphics and present support"); return false; }
	vkGetPhysicalDeviceProperties(B.phys, &B.props);
	vkGetPhysicalDeviceMemoryProperties(B.phys, &B.memProps);
	vkGetPhysicalDeviceFeatures(B.phys, &B.features);
	B.uboAlign = std::max<VkDeviceSize>(256, B.props.limits.minUniformBufferOffsetAlignment);
	B.anisotropy = B.features.samplerAnisotropy != 0;
	Log("device: %s (Vulkan %u.%u)", B.props.deviceName, VK_VERSION_MAJOR(B.props.apiVersion), VK_VERSION_MINOR(B.props.apiVersion));

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qci.queueFamilyIndex = B.family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
	VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE;
	VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	f2.pNext = &f13; f2.features.samplerAnisotropy = B.features.samplerAnisotropy; f2.features.textureCompressionBC = B.features.textureCompressionBC; f2.features.largePoints = B.features.largePoints;
	const char* devExt[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
	VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.pNext = &f2; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = devExt;
	if (vkCreateDevice(B.phys, &dci, nullptr, &B.device) != VK_SUCCESS) { Log("vkCreateDevice failed"); return false; }
#define VKFN_LOAD_DEVICE(n) n = (PFN_##n)vkGetDeviceProcAddr(B.device, #n);
	VKFN_DEVICE(VKFN_LOAD_DEVICE)
	vkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(B.device, "vkCmdPushDescriptorSetKHR");
	vkGetDeviceQueue(B.device, B.family, 0, &B.queue);

	VkCommandPoolCreateInfo cpi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	cpi.queueFamilyIndex = B.family; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VKCHECK(vkCreateCommandPool(B.device, &cpi, nullptr, &B.pool));
	VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	cai.commandPool = B.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
	VKCHECK(vkAllocateCommandBuffers(B.device, &cai, &B.cmd));
	VKCHECK(vkAllocateCommandBuffers(B.device, &cai, &B.uploadCmd));
	VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VKCHECK(vkCreateFence(B.device, &fci, nullptr, &B.frameFence));
	VkSemaphoreCreateInfo sei{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VKCHECK(vkCreateSemaphore(B.device, &sei, nullptr, &B.imageAvailable));

	// pipeline layout: one push descriptor set (uniform block + 4 combined image samplers)
	VkDescriptorSetLayoutBinding lb[10] = {};
	lb[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
	for (uint32_t i = 1; i < 9; ++i) lb[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
	lb[9] = { 9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };		// pixel shader constants of the water shaders
	VkDescriptorSetLayoutCreateInfo dli{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; dli.bindingCount = 10; dli.pBindings = lb;
	VKCHECK(vkCreateDescriptorSetLayout(B.device, &dli, nullptr, &B.dsLayout));
	VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pli.setLayoutCount = 1; pli.pSetLayouts = &B.dsLayout;
	VKCHECK(vkCreatePipelineLayout(B.device, &pli, nullptr, &B.pipeLayout));
	VkShaderModuleCreateInfo smi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	smi.codeSize = sizeof(g_gfxFixedVert); smi.pCode = g_gfxFixedVert;
	VKCHECK(vkCreateShaderModule(B.device, &smi, nullptr, &B.vs));
	smi.codeSize = sizeof(g_gfxFixedFrag); smi.pCode = g_gfxFixedFrag;
	VKCHECK(vkCreateShaderModule(B.device, &smi, nullptr, &B.fs));
	smi.codeSize = sizeof(g_gfxWaterRiverFrag); smi.pCode = g_gfxWaterRiverFrag;
	VKCHECK(vkCreateShaderModule(B.device, &smi, nullptr, &B.fsWater[0]));
	smi.codeSize = sizeof(g_gfxWaterTrapezoidFrag); smi.pCode = g_gfxWaterTrapezoidFrag;
	VKCHECK(vkCreateShaderModule(B.device, &smi, nullptr, &B.fsWater[1]));
	CreatePostShaders();

	// per frame ring buffer and the constant default vertex attributes
	B.ringSize = 64ull * 1024 * 1024;
	if (!CreateBuffer(B.ringSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, B.ring, B.ringAlloc))
		return false;
	if (!CreateBuffer(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, B.defaults, B.defaultsAlloc))
		return false;
	float* d = (float*)B.defaultsAlloc.mapped;
	d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 1;
	d[4] = 0; d[5] = 0; d[6] = 1; d[7] = 0;
	((uint32_t*)d)[8] = 0xFFFFFFFFu;
	((uint32_t*)d)[12] = 0;

	// white 1x1 texture for stages without a texture
	FormatInfo fi;
	MapFormat(D3DFMT_A8R8G8B8, fi);
	B.white = CreateGpuTexture(1, 1, 1, fi);
	if (B.white)
	{
		NullSurface px(1, 1, D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED, nullptr);
		memset(px.m_data.data(), 0xFF, px.m_data.size());
		std::vector<const NullSurface*> lv{ &px };
		UploadLevels(B.white, lv);
	}

	NullTexture::s_destroyHook = &OnTextureDestroyed;
	return true;
}

// ---- per frame ------------------------------------------------------------------------------------------------------------

// Ends the open render pass. Off-screen targets go back to the sampled layout, the back buffer stays an attachment.
void EndPass()
{
	if (!B.rendering)
		return;
	vkCmdEndRendering(B.cmd);
	B.rendering = false;
	if (B.passTarget)
	{
		ImageBarrier(B.cmd, B.passTarget->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
		B.passTarget->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		B.passTarget->loaded = true;
	}
	else
	{
		B.colorLoaded = true;
		B.depthLoaded = true;
	}
}

bool EnsureOffscreenDepth(uint32_t w, uint32_t h)
{
	if (B.offDepth && B.offDepth->width >= w && B.offDepth->height >= h)
		return true;
	if (B.offDepth)
	{
		w = std::max(w, B.offDepth->width); h = std::max(h, B.offDepth->height);
		B.deferredTextures.push_back(B.offDepth);
		B.offDepth = nullptr;
	}
	GpuTexture* d = new GpuTexture();
	d->width = w; d->height = h;
	VkImageCreateInfo di{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	di.imageType = VK_IMAGE_TYPE_2D; di.format = VK_FORMAT_D32_SFLOAT_S8_UINT; di.extent = { w, h, 1 };
	di.mipLevels = 1; di.arrayLayers = 1; di.samples = VK_SAMPLE_COUNT_1_BIT; di.tiling = VK_IMAGE_TILING_OPTIMAL;
	di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; di.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(B.device, &di, nullptr, &d->image) != VK_SUCCESS) { delete d; return false; }
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(B.device, d->image, &req);
	d->alloc = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
	if (d->alloc.block < 0) { vkDestroyImage(B.device, d->image, nullptr); delete d; return false; }
	vkBindImageMemory(B.device, d->image, d->alloc.memory, d->alloc.offset);
	VkImageViewCreateInfo dv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	dv.image = d->image; dv.viewType = VK_IMAGE_VIEW_TYPE_2D; dv.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
	dv.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
	VKCHECK(vkCreateImageView(B.device, &dv, nullptr, &d->view));
	B.offDepth = d;
	return true;
}

bool EnsureRendering()
{
	if (B.rendering && B.passTarget == B.curTarget)
		return true;
	EndPass();
	if (B.curTarget)
	{
		GpuTexture* rt = B.curTarget;
		if (!EnsureOffscreenDepth(rt->width, rt->height))
			return false;
		ImageBarrier(B.cmd, rt->image, rt->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
		ImageBarrier(B.cmd, B.offDepth->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
			VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
		VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		color.imageView = rt->view; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color.loadOp = rt->loaded ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE; color.clearValue.color = { { 0.f, 0.f, 0.f, 0.f } };
		VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		depth.imageView = B.offDepth->view; depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; depth.clearValue.depthStencil = { 1.0f, 0 };
		VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
		ri.renderArea = { { 0, 0 }, { rt->width, rt->height } }; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &color;
		ri.pDepthAttachment = &depth; ri.pStencilAttachment = &depth;
		vkCmdBeginRendering(B.cmd, &ri);
		B.rendering = true; B.passTarget = rt; B.passExtent = { rt->width, rt->height };
		return true;
	}
	const bool hdrStage = B.postOn && B.stage == 0 && B.hdr;
	if (!hdrStage && !AcquireSwap())
		return false;
	const VkAttachmentLoadOp loadOp = B.colorLoaded ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
	const VkAttachmentLoadOp depthLoadOp = B.depthLoaded ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
	const VkImageLayout colorOld = B.colorLoaded ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	const VkImageLayout depthOld = B.depthLoaded ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage colorImage = hdrStage ? B.hdr->image : B.swapImages[B.imageIndex];
	VkImageView colorView = hdrStage ? B.hdr->view : B.swapViews[B.imageIndex];
	ImageBarrier(B.cmd, colorImage, colorOld, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, B.colorLoaded ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
	if (hdrStage)
		B.hdr->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ImageBarrier(B.cmd, B.depthImage, depthOld, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, B.depthLoaded ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
	VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	color.imageView = colorView; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = loadOp; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE; color.clearValue.color = { { 0.f, 0.f, 0.f, 1.f } };
	VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	depth.imageView = B.depthView; depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depth.loadOp = depthLoadOp; depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE; depth.clearValue.depthStencil = { 1.0f, 0 };
	VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea = { { 0, 0 }, B.extent }; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &color; ri.pDepthAttachment = &depth; ri.pStencilAttachment = &depth;
	{ static int n = 0; if (B.colorLoaded && n++ < 20) Log("resume backbuffer pass: hdr %d colorLoaded %d depthLoaded %d stage %d", (int)hdrStage, (int)B.colorLoaded, (int)B.depthLoaded, B.stage); }
	vkCmdBeginRendering(B.cmd, &ri);
	B.rendering = true; B.passTarget = nullptr; B.passExtent = B.extent;
	return true;
}

uint32_t g_cnt[16] = {};
struct PostCfg
{
	float bloomThreshold = 0.92f, bloomKnee = 0.25f, bloomIntensity = 0.16f, saturation = 1.04f, contrast = 1.03f;
	bool fxaa = false;
	float aoStrength = 0.85f, aoRadius = 22.0f;
} g_postCfg;

#include "gfx_vk_post.inl"

uint32_t g_frame = 0;
int g_frameDrawLog = 0;

std::vector<std::string> g_drawTrace;		// description of every draw of the sampled frame			// drawn, offscreen, pixel shader, vertex shader, stencil, ring full, other

void PresentFrame()
{
	if (!B.ready)
		return;
	struct RestoreTarget { GpuTexture* t; ~RestoreTarget() { B.curTarget = t; } } restoreTarget{ B.curTarget };
	B.curTarget = nullptr;
	++g_frame;
	g_frameDrawLog = 0;
	if (!g_drawTrace.empty())
	{
		const size_t from = 0;
		Log("last %u of %u draws of frame %u:", (unsigned)(g_drawTrace.size() - from), (unsigned)g_drawTrace.size(), g_frame);
		for (size_t i = from; i < g_drawTrace.size(); ++i)
			Log("  %s", g_drawTrace[i].c_str());
		g_drawTrace.clear();
	}
	{
		static DWORD lastTick = 0;
		const DWORD now = GetTickCount();
		if (now - lastTick > 2000)
		{
			lastTick = now;
			Log("uploads so far: %llu textures, %.1f MB (frame %u)", (unsigned long long)g_uploads, g_uploadBytes / 1048576.0, g_frame);
		}
	}
	if (g_frame % 4000 == 0)
		Log("frame %u: drawn %u, skipped: offscreen %u, pixel shader %u, vertex shader %u, stencil %u, ring full %u | calls: DrawPrimitive %u DrawIndexed %u UP %u IndexedUP %u | no vb %u no ib %u, no vertices %u, no rendering %u", g_frame, g_cnt[0], g_cnt[1], g_cnt[2], g_cnt[3], g_cnt[4], g_cnt[5], g_cnt[6], g_cnt[7], g_cnt[10], g_cnt[11], g_cnt[8], g_cnt[9], g_cnt[12], g_cnt[13]);
	memset(g_cnt, 0, sizeof(g_cnt));
	const bool ready = (B.postOn && B.stage == 0 && B.hdr) ? RunPostProcess() : EnsureRendering();
	if (!ready)
	{
		// minimised or the swapchain could not be made: just drop the frame's recorded work (uploads still have to happen)
		EndPass();
		FlushUploadsSync();
		B.colorLoaded = false; B.depthLoaded = false; B.semaphoreUsed = false; B.stage = 1; B.scene3D = false;
		vkEndCommandBuffer(B.cmd);
		vkResetCommandBuffer(B.cmd, 0);
		BeginCommandBuffer();
		B.ringCursor = 0;
		return;
	}
	EndPass();
	ImageBarrier(B.cmd, B.swapImages[B.imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
	VKCHECK(vkEndCommandBuffer(B.cmd));
	B.cmdOpen = false;

	VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	const bool hasUploads = B.uploadOpen;
	if (hasUploads)
	{
		VKCHECK(vkEndCommandBuffer(B.uploadCmd));
		B.uploadOpen = false;
	}
	VkCommandBuffer submitted[2] = { B.uploadCmd, B.cmd };
	VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.waitSemaphoreCount = (B.acquired && !B.semaphoreUsed) ? 1 : 0; si.pWaitSemaphores = &B.imageAvailable; si.pWaitDstStageMask = &wait;
	si.commandBufferCount = hasUploads ? 2 : 1; si.pCommandBuffers = hasUploads ? submitted : &B.cmd;
	si.signalSemaphoreCount = 1; si.pSignalSemaphores = &B.renderDone[B.imageIndex];
	VKCHECK(vkQueueSubmit(B.queue, 1, &si, B.frameFence));
	VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &B.renderDone[B.imageIndex]; pi.swapchainCount = 1; pi.pSwapchains = &B.swapchain; pi.pImageIndices = &B.imageIndex;
	VkResult pr = vkQueuePresentKHR(B.queue, &pi);
	B.acquired = false; B.colorLoaded = false; B.depthLoaded = false; B.semaphoreUsed = false; B.stage = 1; B.scene3D = false;

	vkWaitForFences(B.device, 1, &B.frameFence, VK_TRUE, UINT64_MAX);
	vkResetFences(B.device, 1, &B.frameFence);
	vkResetCommandBuffer(B.cmd, 0);
	BeginCommandBuffer();
	B.ringCursor = 0;
	FreeDeferredBuffers();
	for (GpuTexture* g : B.deferredTextures) DestroyGpuTexture(g);
	B.deferredTextures.clear();
	if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
		DestroySwapchain();
}

// Copies a rectangle of the current frame's colour image into a CPU surface (4 byte formats only). Ends the pass, runs the work recorded so far and
// waits for it, like a Direct3D 8 read of the render target would; rendering then resumes on top of the existing content.
bool ReadBackBuffer(NullSurface* dst, RECT r)
{
	if (!B.ready || !dst || BytesPerPixel(dst->m_format) != 4)
		return false;
	struct RestoreTarget { GpuTexture* t; ~RestoreTarget() { B.curTarget = t; } } restoreTarget{ B.curTarget };
	B.curTarget = nullptr;
	if (!EnsureRendering())
		return false;
	r.left = std::max<LONG>(r.left, 0); r.top = std::max<LONG>(r.top, 0);
	r.right = std::min<LONG>(r.right, (LONG)std::min<UINT>(B.extent.width, dst->m_width));
	r.bottom = std::min<LONG>(r.bottom, (LONG)std::min<UINT>(B.extent.height, dst->m_height));
	const uint32_t w = (uint32_t)std::max<LONG>(r.right - r.left, 0), h = (uint32_t)std::max<LONG>(r.bottom - r.top, 0);
	if (w == 0 || h == 0)
		return false;
	const VkDeviceSize need = (VkDeviceSize)w * h * 4;
	if (B.readSize < need)
	{
		if (B.readBuf) { vkDestroyBuffer(B.device, B.readBuf, nullptr); Free(B.readAlloc); B.readBuf = VK_NULL_HANDLE; }
		B.readSize = 0;
		if (!CreateBuffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, B.readBuf, B.readAlloc))
			return false;
		B.readSize = need;
	}
	const bool fromHdr = B.postOn && B.stage == 0 && B.hdr;
	EndPass();
	VkImage img = fromHdr ? B.ldr->image : B.swapImages[B.imageIndex];
	if (fromHdr)
	{
		// the HDR scene is converted to the swapchain format first
		ImageBarrier(B.cmd, B.hdr->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		ImageBarrier(B.cmd, B.ldr->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkImageBlit bl{};
		bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; bl.dstSubresource = bl.srcSubresource;
		bl.srcOffsets[1] = { (int32_t)B.extent.width, (int32_t)B.extent.height, 1 }; bl.dstOffsets[1] = bl.srcOffsets[1];
		vkCmdBlitImage(B.cmd, B.hdr->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, B.ldr->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
		ImageBarrier(B.cmd, B.hdr->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
		ImageBarrier(B.cmd, B.ldr->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	}
	else
		ImageBarrier(B.cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	VkBufferImageCopy bic{};
	bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	bic.imageOffset = { (int32_t)r.left, (int32_t)r.top, 0 }; bic.imageExtent = { w, h, 1 };
	vkCmdCopyImageToBuffer(B.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, B.readBuf, 1, &bic);
	if (!fromHdr)
		ImageBarrier(B.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	VKCHECK(vkEndCommandBuffer(B.cmd));
	B.cmdOpen = false;

	VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	const bool hasUploads = B.uploadOpen;
	if (hasUploads)
	{
		VKCHECK(vkEndCommandBuffer(B.uploadCmd));
		B.uploadOpen = false;
	}
	VkCommandBuffer submitted[2] = { B.uploadCmd, B.cmd };
	VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	const bool waits = B.acquired && !B.semaphoreUsed;
	si.waitSemaphoreCount = waits ? 1 : 0; si.pWaitSemaphores = &B.imageAvailable; si.pWaitDstStageMask = &wait;
	si.commandBufferCount = hasUploads ? 2 : 1; si.pCommandBuffers = hasUploads ? submitted : &B.cmd;
	VKCHECK(vkQueueSubmit(B.queue, 1, &si, B.frameFence));
	if (waits) B.semaphoreUsed = true;
	B.colorLoaded = true; B.depthLoaded = true;
	vkWaitForFences(B.device, 1, &B.frameFence, VK_TRUE, UINT64_MAX);
	vkResetFences(B.device, 1, &B.frameFence);
	vkResetCommandBuffer(B.cmd, 0);
	BeginCommandBuffer();
	FreeDeferredBuffers();

	const uint8_t* src = (const uint8_t*)B.readAlloc.mapped;
	for (uint32_t y = 0; y < h; ++y)
		memcpy(dst->m_data.data() + (size_t)(r.top + y - 0) * dst->m_pitch + (size_t)r.left * 4, src + (size_t)y * w * 4, (size_t)w * 4);
	return true;
}

// GPU side copy of the whole back buffer into a render target texture (used by the water shaders to refract the scene).
bool CopyBackBufferToTexture(GpuTexture* g)
{
	static const bool noBlit = getenv("GENERALS_NOBLIT") != nullptr;
	if (noBlit) return true;
	struct RestoreTarget { GpuTexture* t; ~RestoreTarget() { B.curTarget = t; } } restoreTarget{ B.curTarget };
	B.curTarget = nullptr;
	if (!EnsureRendering())
		return false;
	EndPass();
	const bool fromHdr = B.postOn && B.stage == 0 && B.hdr;
	VkImage src = fromHdr ? B.hdr->image : B.swapImages[B.imageIndex];
	ImageBarrier(B.cmd, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	ImageBarrier(B.cmd, g->image, g->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	if (fromHdr)
	{
		VkImageBlit bl{};
		bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; bl.dstSubresource = bl.srcSubresource;
		const int32_t cw = (int32_t)std::min(B.extent.width, g->width), ch = (int32_t)std::min(B.extent.height, g->height);
		bl.srcOffsets[1] = { cw, ch, 1 }; bl.dstOffsets[1] = { cw, ch, 1 };
		vkCmdBlitImage(B.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
	}
	else
	{
		VkImageCopy ic{};
		ic.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; ic.dstSubresource = ic.srcSubresource;
		ic.extent = { std::min(B.extent.width, g->width), std::min(B.extent.height, g->height), 1 };
		vkCmdCopyImage(B.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
	}
	ImageBarrier(B.cmd, g->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	ImageBarrier(B.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	g->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; g->loaded = true;
	return true;
}

// ---- the device -----------------------------------------------------------------------------------------------------------

class VkDevice8 : public NullDevice
{
public:
	VkDevice8(IDirect3D8* d3d, const D3DPRESENT_PARAMETERS& pp, HWND focus, UINT adapter, D3DDEVTYPE type, DWORD behavior)
		: NullDevice(d3d, pp, focus, adapter, type, behavior)
	{
		for (int i = 0; i < 512; ++i) { memset(m_matrix[i], 0, sizeof(m_matrix[i])); m_matrix[i][0] = m_matrix[i][5] = m_matrix[i][10] = m_matrix[i][15] = 1.0f; }
		m_renderStates[D3DRS_COLORWRITEENABLE] = 0xF;
		{ const char* e = getenv("GENERALS_SHADOWS"); if (e && e[0] == '0') g_sh.on = false; }
		{ const char* e = getenv("GENERALS_POST"); B.postOn = !(e && e[0] == '0'); }
		{ const char* e = getenv("GENERALS_BLOOM"); if (e) g_postCfg.bloomIntensity = (float)atof(e); e = getenv("GENERALS_FXAA"); if (e && e[0] == '0') g_postCfg.fxaa = false; e = getenv("GENERALS_AO"); if (e) g_postCfg.aoStrength = (float)atof(e); }
		HWND w = pp.hDeviceWindow ? pp.hDeviceWindow : focus;
		if (!B.ready)
		{
			if (LoadVulkan() && CreateInstanceAndDevice(w) && CreateSwapchain())
			{
				BeginCommandBuffer();
				B.ready = true;
				Log("native Vulkan backend ready");
			}
			else
				Log("native Vulkan backend could not start");
		}
	}

	// ---- transforms and bound objects
	STDMETHOD(SetTransform)(THIS_ D3DTRANSFORMSTATETYPE s, CONST D3DMATRIX* m) override { if ((UINT)s < 512) memcpy(m_matrix[s], m, 64); return D3D_OK; }
	STDMETHOD(GetTransform)(THIS_ D3DTRANSFORMSTATETYPE s, D3DMATRIX* m) override { if ((UINT)s < 512) memcpy(m, m_matrix[s], 64); return D3D_OK; }
	STDMETHOD(SetTexture)(THIS_ DWORD stage, IDirect3DBaseTexture8* tex) override
	{
		if (stage >= 8) return D3D_OK;
		if (tex) tex->AddRef();
		if (m_tex[stage]) m_tex[stage]->Release();
		m_tex[stage] = tex;
		return D3D_OK;
	}
	STDMETHOD(GetTexture)(THIS_ DWORD stage, IDirect3DBaseTexture8** pp) override { *pp = stage < 8 ? m_tex[stage] : nullptr; if (*pp) (*pp)->AddRef(); return D3D_OK; }
	STDMETHOD(SetStreamSource)(THIS_ UINT n, IDirect3DVertexBuffer8* vb, UINT stride) override
	{
		if (n != 0) return D3D_OK;
		if (vb) vb->AddRef();
		if (m_vb) m_vb->Release();
		m_vb = static_cast<NullVertexBuffer*>(vb); m_stride = stride;
		return D3D_OK;
	}
	STDMETHOD(SetIndices)(THIS_ IDirect3DIndexBuffer8* ib, UINT base) override
	{
		if (ib) ib->AddRef();
		if (m_ib) m_ib->Release();
		m_ib = static_cast<NullIndexBuffer*>(ib); m_baseVertex = base;
		return D3D_OK;
	}
	STDMETHOD(SetMaterial)(THIS_ CONST D3DMATERIAL8* m) override { m_material = *m; return D3D_OK; }
	STDMETHOD(GetMaterial)(THIS_ D3DMATERIAL8* m) override { *m = m_material; return D3D_OK; }
	STDMETHOD(SetLight)(THIS_ DWORD i, CONST D3DLIGHT8* l) override { if (i < 8) m_light[i] = *l; return D3D_OK; }
	STDMETHOD(GetLight)(THIS_ DWORD i, D3DLIGHT8* l) override { if (i < 8) *l = m_light[i]; return D3D_OK; }
	STDMETHOD(LightEnable)(THIS_ DWORD i, BOOL enable) override { if (i < 8) m_lightOn[i] = enable != 0; return D3D_OK; }
	STDMETHOD(GetLightEnable)(THIS_ DWORD i, BOOL* enable) override { *enable = i < 8 ? m_lightOn[i] : FALSE; return D3D_OK; }
	STDMETHOD(SetVertexShader)(THIS_ DWORD h) override { m_vertexShader = h; return D3D_OK; }
	STDMETHOD(GetVertexShader)(THIS_ DWORD* h) override { *h = m_vertexShader; return D3D_OK; }
	STDMETHOD(SetPixelShader)(THIS_ DWORD h) override { m_pixelShader = h; return D3D_OK; }
	STDMETHOD(GetPixelShader)(THIS_ DWORD* h) override { *h = m_pixelShader; return D3D_OK; }

	// The updated water shaders (ps_3_0) are the only pixel shaders this backend runs; they are recognised by their bytecode and
	// replaced by the GLSL port in Shaders/gfx_water.frag. The first one the engine creates is the river shader, the second the sea shader.
	int PixelShaderKind(DWORD h) const { auto it = m_psKind.find(h); return it == m_psKind.end() ? 0 : it->second; }
	STDMETHOD(CreatePixelShader)(THIS_ CONST DWORD* fn, DWORD* h) override
	{
		HRESULT hr = NullDevice::CreatePixelShader(fn, h);
		if (fn && *fn == 0xFFFF0300u)
		{
			uint64_t hash = 1469598103934665603ull;
			for (const DWORD* p = fn; *p != 0x0000FFFFu; )
			{
				hash = (hash ^ *p) * 1099511628211ull;
				p += ((*p & 0xFFFF) == 0xFFFE) ? 1 + ((*p >> 16) & 0x7FFF) : 1 + ((*p >> 24) & 0xF);
			}
			static std::map<uint64_t, int> known;
			auto it = known.find(hash);
			if (it == known.end())
				it = known.insert({ hash, (int)known.size() + 1 }).first;
			if (it->second <= 2)
				m_psKind[*h] = it->second;
			Log("ps_3_0 shader %08X recognised as updated water kind %d", (unsigned)*h, it->second);
		}
		return hr;
	}
	STDMETHOD(DeletePixelShader)(THIS_ DWORD h) override { m_psKind.erase(h); return D3D_OK; }
	STDMETHOD(SetPixelShaderConstant)(THIS_ DWORD reg, CONST void* data, DWORD count) override
	{
		if (data && reg < 32) memcpy(m_psConst[reg], data, std::min<size_t>(count, 32 - reg) * 16);
		return D3D_OK;
	}

	void SyncTarget()
	{
		B.curTarget = nullptr; B.targetUnsupported = false;
		if (!B.ready || OnBackBuffer())
			return;
		NullSurface* s = static_cast<NullSurface*>(m_target);
		NullTexture* tex = s ? (NullTexture*)s->m_texture : nullptr;
		if (tex && (s->m_usage & D3DUSAGE_RENDERTARGET))
		{
			GpuTexture* g = EnsureGpuTexture(tex);
			if (g && g->renderTarget) { B.curTarget = g; return; }
		}
		B.targetUnsupported = true;
	}
	STDMETHOD(SetRenderTarget)(THIS_ IDirect3DSurface8* rt, IDirect3DSurface8* z) override
	{
		HRESULT hr = NullDevice::SetRenderTarget(rt, z);
		SyncTarget();
		return hr;
	}

	// reading the back buffer (heat haze, shockwave) needs the GPU contents
	STDMETHOD(CopyRects)(THIS_ IDirect3DSurface8* src, CONST RECT* rects, UINT count, IDirect3DSurface8* dst, CONST POINT* points) override
	{
		if (src == static_cast<IDirect3DSurface8*>(m_back) && B.ready && dst)
		{
			NullSurface* d = static_cast<NullSurface*>(dst);
			NullTexture* dt = d->m_texture ? (NullTexture*)d->m_texture : nullptr;
			if (dt && (d->m_usage & D3DUSAGE_RENDERTARGET))
			{
				GpuTexture* g = EnsureGpuTexture(dt);
				if (g && g->renderTarget && CopyBackBufferToTexture(g))
					return D3D_OK;
			}
		}
		if (src == static_cast<IDirect3DSurface8*>(m_back) || src == m_target)
		{
			NullSurface* s = static_cast<NullSurface*>(src);
			if (s == m_back)
			{
				if (!rects || count == 0)
					ReadBackBuffer(s, RECT{ 0, 0, (LONG)s->m_width, (LONG)s->m_height });
				else
				{
					RECT all = rects[0];
					for (UINT i = 1; i < count; ++i)
					{
						all.left = std::min(all.left, rects[i].left); all.top = std::min(all.top, rects[i].top);
						all.right = std::max(all.right, rects[i].right); all.bottom = std::max(all.bottom, rects[i].bottom);
					}
					ReadBackBuffer(s, all);
				}
			}
		}
		return NullDevice::CopyRects(src, rects, count, dst, points);
	}

	// ---- frame
	STDMETHOD(BeginScene)(THIS) override { return D3D_OK; }
	STDMETHOD(EndScene)(THIS) override { return D3D_OK; }
	STDMETHOD(Present)(THIS_ CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*) override { PresentFrame(); LogFrame(); return D3D_OK; }
	STDMETHOD(Reset)(THIS_ D3DPRESENT_PARAMETERS* pp) override
	{
		NullDevice::Reset(pp);
		if (B.ready) { DestroySwapchain(); B.acquired = false; }
		return D3D_OK;
	}
	STDMETHOD(Clear)(THIS_ DWORD, CONST D3DRECT*, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override
	{
		if (!B.ready || B.targetUnsupported || !EnsureRendering()) return D3D_OK;
		VkClearAttachment att[3];
		uint32_t n = 0;
		if (flags & 1)
		{
			att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; att[n].colorAttachment = 0;
			att[n].clearValue.color = { { ((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, ((color >> 24) & 255) / 255.f } };
			++n;
		}
		if (flags & 2)
		{
			att[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; att[n].colorAttachment = 0; att[n].clearValue.depthStencil = { z, 0 };
			++n;
		}
		if (flags & 4)
		{
			att[n].aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; att[n].colorAttachment = 0; att[n].clearValue.depthStencil = { 0.0f, stencil };
			++n;
		}
		if (n)
		{
			VkClearRect cr{ { { 0, 0 }, B.passExtent }, 0, 1 };
			vkCmdClearAttachments(B.cmd, n, att, 1, &cr);
		}
		return D3D_OK;
	}

	// ---- drawing
	STDMETHOD(DrawPrimitive)(THIS_ D3DPRIMITIVETYPE type, UINT start, UINT count) override
	{
		++g_cnt[6];
		if (!m_vb) { ++g_cnt[8]; return D3D_OK; }
		const UINT verts = VertexCount(type, count);
		return Submit(type, count, m_vb->m_data.data() + (size_t)start * m_stride, verts, nullptr, 0, 0, 0);
	}
	STDMETHOD(DrawIndexedPrimitive)(THIS_ D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT startIndex, UINT count) override
	{
		++g_cnt[7];
		if (!m_vb || !m_ib) { ++g_cnt[9]; return D3D_OK; }
		const bool wide = m_ib->m_format == D3DFMT_INDEX32;
		const UINT indices = IndexCount(type, count);
		const UINT firstVertex = m_baseVertex + minIndex;
		return Submit(type, count, m_vb->m_data.data() + (size_t)firstVertex * m_stride, numVertices,
			m_ib->m_data.data() + (size_t)startIndex * (wide ? 4 : 2), indices, wide, (int)minIndex);
	}
	STDMETHOD(DrawPrimitiveUP)(THIS_ D3DPRIMITIVETYPE type, UINT count, CONST void* data, UINT stride) override
	{
		++g_cnt[10];
		m_stride = stride;
		return SubmitUP(type, count, data, VertexCount(type, count), nullptr, 0, 0, stride);
	}
	STDMETHOD(DrawIndexedPrimitiveUP)(THIS_ D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT count, CONST void* indices, D3DFORMAT fmt, CONST void* data, UINT stride) override
	{
		++g_cnt[11];
		const bool wide = fmt == D3DFMT_INDEX32;
		return SubmitUP(type, count, (const char*)data + (size_t)minIndex * stride, numVertices, indices, IndexCount(type, count), wide ? 1 : 0, stride, (int)minIndex);
	}

private:
	static UINT VertexCount(D3DPRIMITIVETYPE t, UINT c)
	{
		switch (t) { case D3DPT_POINTLIST: return c; case D3DPT_LINELIST: return c * 2; case D3DPT_LINESTRIP: return c + 1;
			case D3DPT_TRIANGLELIST: return c * 3; default: return c + 2; }
	}
	static UINT IndexCount(D3DPRIMITIVETYPE t, UINT c) { return VertexCount(t, c); }

	bool OnBackBuffer() const { return m_target == static_cast<IDirect3DSurface8*>(m_back); }

	HRESULT SubmitUP(D3DPRIMITIVETYPE type, UINT count, const void* vertices, UINT numVertices, const void* indices, UINT indexCount, int wide, UINT stride, int minIndex = 0)
	{
		m_stride = stride;
		return Submit(type, count, vertices, numVertices, indices, indexCount, wide, minIndex);
	}

	// diagnostics: why draws are skipped, and which vertex formats are drawn (first sightings are written to the log)
	void Note(const char* what, DWORD fvf)
	{
		static std::map<std::pair<std::string, DWORD>, int> seen;
		int& n = seen[std::make_pair(std::string(what), fvf)];
		if (n++ == 0)
		{
			Log("draw %s: vertex shader/FVF 0x%08X, pixel shader 0x%X, stencil %u, zenable %u, lighting %u, texture0 %s", what, fvf, m_pixelShader,
				m_renderStates[D3DRS_STENCILENABLE], m_renderStates[D3DRS_ZENABLE], m_renderStates[D3DRS_LIGHTING], m_tex[0] ? "yes" : "no");
			Log("    cull %u, blend %u (%u,%u), zwrite %u, zfunc %u, alphatest %u, stride %u, viewport %u,%u %ux%u", m_renderStates[D3DRS_CULLMODE],
				m_renderStates[D3DRS_ALPHABLENDENABLE], m_renderStates[D3DRS_SRCBLEND], m_renderStates[D3DRS_DESTBLEND], m_renderStates[D3DRS_ZWRITEENABLE],
				m_renderStates[D3DRS_ZFUNC], m_renderStates[D3DRS_ALPHATESTENABLE], m_stride, m_viewport.X, m_viewport.Y, m_viewport.Width, m_viewport.Height);
			for (int s = 0; s < 2; ++s)
			{
				const DWORD* ts = m_stageStates[s];
				D3DSURFACE_DESC d{};
				if (m_tex[s]) static_cast<NullTexture*>(m_tex[s])->GetLevelDesc(0, &d);
				Log("    stage %d: colorop %u args %u,%u alphaop %u args %u,%u texcoord %u  texture %s %ux%u format %d", s, ts[D3DTSS_COLOROP], ts[D3DTSS_COLORARG1], ts[D3DTSS_COLORARG2],
					ts[D3DTSS_ALPHAOP], ts[D3DTSS_ALPHAARG1], ts[D3DTSS_ALPHAARG2], ts[D3DTSS_TEXCOORDINDEX], m_tex[s] ? "yes" : "no", d.Width, d.Height, (int)d.Format);
			}
			const float* w = m_matrix[256]; const float* v = m_matrix[2]; const float* pj = m_matrix[3];
			Log("    world row3 %.1f %.1f %.1f | view row3 %.1f %.1f %.1f | proj [0][0] %.3f [1][1] %.3f [2][2] %.3f [2][3] %.3f [3][2] %.3f", w[12], w[13], w[14], v[12], v[13], v[14], pj[0], pj[5], pj[10], pj[11], pj[14]);
		}
	}

	HRESULT Submit(D3DPRIMITIVETYPE type, UINT, const void* vertices, UINT numVertices, const void* indices, UINT indexCount, int wide, int minIndex)
	{
		if (!B.ready || numVertices == 0) { ++g_cnt[12]; return D3D_OK; }
		if (B.targetUnsupported) { ++g_cnt[1]; Note("skipped, render target cannot be drawn to", m_vertexShader); return D3D_OK; }
		if (g_sh.suppress && !B.curTarget) return D3D_OK;
		const int waterKind = PixelShaderKind(m_pixelShader);
		static const int dbg = getenv("GENERALS_SHDBG") ? atoi(getenv("GENERALS_SHDBG")) : 0;
		if (!(dbg & 1) && m_renderStates[D3DRS_STENCILENABLE] && ShadowsEnabled() && B.stage == 0)
		{
			// shadow volumes (no colour, or position only vertices) and the full screen darkening quad belong to the stencil shadows; models that only have stencil on stay
			const DWORD cm = m_renderStates[D3DRS_COLORWRITEENABLE];
			if ((cm & 0xF) == 0 || (m_vertexShader & 0xE) == 0x2 && !(m_vertexShader & 0x40) && !(m_vertexShader & 0xF00) || (m_vertexShader & 0xE) == 0x4 && numVertices == 4)
				return D3D_OK;
		}		// the engine's stencil shadows are replaced by the shadow map
		static const bool noWater = getenv("GENERALS_NOWATER") != nullptr;
		if (waterKind && noWater) return D3D_OK;
		if (m_pixelShader != 0 && !waterKind) { ++g_cnt[2]; Note("skipped, pixel shader", m_vertexShader); return D3D_OK; }
		if ((m_vertexShader & 0x80000000u) || m_vertexShader == 0) { ++g_cnt[3]; Note("skipped, vertex shader", m_vertexShader); return D3D_OK; }
		++g_cnt[0]; ++g_cnt[14];
		Note("drawn", m_vertexShader);
		const DWORD fvf = m_vertexShader;
		const bool pretransformed = (fvf & 0xE) == 0x4;
		// the full screen shadow quad of the stencil shadows is pre-transformed too but still belongs to the 3D scene

		if (!EnsureRendering()) { ++g_cnt[13]; return D3D_OK; }
		if (!pretransformed && !B.curTarget)
		{
			// the camera of the scene: taken from the first opaque draw (sky boxes and similar draws use other view matrices)
			if (B.stage == 0 && !B.projCaptured && m_renderStates[D3DRS_ZWRITEENABLE] && !m_renderStates[D3DRS_ALPHABLENDENABLE] && numVertices > 8)
			{
				B.haveProj = true; B.projCaptured = true; memcpy(B.lastProj, m_matrix[3], 64); memcpy(B.lastView, m_matrix[2], 64);
				B.lastViewport[0] = (float)m_viewport.X; B.lastViewport[1] = (float)m_viewport.Y; B.lastViewport[2] = (float)m_viewport.Width; B.lastViewport[3] = (float)m_viewport.Height;
			}
		}
		const UINT stride = m_stride ? m_stride : 16;

		// copy this draw's data into the ring buffer
		const VkDeviceSize vbytes = (VkDeviceSize)numVertices * stride;
		const VkDeviceSize ibytes = (VkDeviceSize)indexCount * (wide ? 4 : 2);
		auto take = [&](VkDeviceSize bytes, VkDeviceSize align) -> VkDeviceSize
		{
			VkDeviceSize at = (B.ringCursor + align - 1) & ~(align - 1);
			if (at + bytes > B.ringSize) return ~0ull;
			B.ringCursor = at + bytes;
			return at;
		};
		const VkDeviceSize vOff = take(vbytes, 16), iOff = indexCount ? take(ibytes, 4) : 0, uOff = take(sizeof(DrawUbo), B.uboAlign);
		if (vOff == ~0ull || iOff == ~0ull || uOff == ~0ull) { ++g_cnt[5]; return D3D_OK; }
		memcpy((char*)B.ringAlloc.mapped + vOff, vertices, (size_t)vbytes);
		if (indexCount)
		{
			// indices are relative to the first copied vertex when the offset is applied through vertexOffset
			memcpy((char*)B.ringAlloc.mapped + iOff, indices, (size_t)ibytes);
		}

		DrawUbo u;
		memset(&u, 0, sizeof(u));
		float wv[16], wvp[16];
		Multiply(m_matrix[256], m_matrix[2], wv);
		Multiply(wv, m_matrix[3], wvp);
		{
			static std::map<DWORD, int> sampled;
			if (sampled[fvf]++ == 0 && !pretransformed)
			{
				for (UINT k = 0; k < 3 && k < numVertices; ++k)
				{
					const float* v = (const float*)((const char*)vertices + (size_t)k * stride);
					const float cx = v[0] * wvp[0] + v[1] * wvp[4] + v[2] * wvp[8] + wvp[12];
					const float cy = v[0] * wvp[1] + v[1] * wvp[5] + v[2] * wvp[9] + wvp[13];
					const float cz = v[0] * wvp[2] + v[1] * wvp[6] + v[2] * wvp[10] + wvp[14];
					const float cw = v[0] * wvp[3] + v[1] * wvp[7] + v[2] * wvp[11] + wvp[15];
					DWORD diffuse = 0;
					UINT off = 12 + (fvf & 0x10 ? 12 : 0);
					if (fvf & 0x40) memcpy(&diffuse, (const char*)v + off, 4);
					Log("    vertex %u: pos %.1f %.1f %.1f  clip %.2f %.2f %.2f %.2f  ndc %.3f %.3f %.3f  diffuse %08X  indices %u", k, v[0], v[1], v[2], cx, cy, cz, cw,
						cw != 0 ? cx / cw : 0, cw != 0 ? cy / cw : 0, cw != 0 ? cz / cw : 0, diffuse, indexCount);
				}
			}
		}
		memcpy(u.wvp, wvp, 64);
		u.viewport[0] = 0; u.viewport[1] = 0; u.viewport[2] = (float)B.passExtent.width; u.viewport[3] = (float)B.passExtent.height;
		const DWORD tf = m_renderStates[D3DRS_TEXTUREFACTOR];
		u.textureFactor[0] = ((tf >> 16) & 255) / 255.f; u.textureFactor[1] = ((tf >> 8) & 255) / 255.f; u.textureFactor[2] = (tf & 255) / 255.f; u.textureFactor[3] = ((tf >> 24) & 255) / 255.f;
		{
			auto f = [&](D3DRENDERSTATETYPE s) { float v; DWORD d = m_renderStates[s]; memcpy(&v, &d, 4); return v; };
			u.pointParams[0] = f(D3DRS_POINTSIZE); u.pointParams[1] = f(D3DRS_POINTSIZE_MIN); u.pointParams[2] = m_renderStates[D3DRS_POINTSIZE_MAX] ? f(D3DRS_POINTSIZE_MAX) : 64.0f;
			u.pointParams[3] = m_renderStates[D3DRS_POINTSPRITEENABLE] ? 1.0f : 0.0f;
			u.pointScale[0] = f(D3DRS_POINTSCALE_A); u.pointScale[1] = f(D3DRS_POINTSCALE_B); u.pointScale[2] = f(D3DRS_POINTSCALE_C);
			u.pointScale[3] = m_renderStates[D3DRS_POINTSCALEENABLE] ? 1.0f : 0.0f;
			if (u.pointParams[0] == 0.0f) u.pointParams[0] = 1.0f;
		}
		u.flags[0] = pretransformed ? 1 : 0;
		u.flags[2] = m_renderStates[D3DRS_ALPHAFUNC]; u.flags[3] = m_renderStates[D3DRS_ALPHATESTENABLE] ? 1 : 0;
		u.alphaRef[0] = (m_renderStates[D3DRS_ALPHAREF] & 255) / 255.f;
		for (int s = 0; s < 4; ++s)
		{
			const DWORD* t = m_stageStates[s];
			u.stage[2 * s][0] = t[D3DTSS_COLOROP]; u.stage[2 * s][1] = t[D3DTSS_COLORARG1]; u.stage[2 * s][2] = t[D3DTSS_COLORARG2]; u.stage[2 * s][3] = t[D3DTSS_ALPHAOP];
			u.stage[2 * s + 1][0] = t[D3DTSS_ALPHAARG1]; u.stage[2 * s + 1][1] = t[D3DTSS_ALPHAARG2];
			u.stage[2 * s + 1][2] = t[D3DTSS_TEXCOORDINDEX] & 3; u.stage[2 * s + 1][3] = m_tex[s] ? 1 : 0;
			// untouched stage 0 defaults of Direct3D: modulate texture and diffuse; later stages are disabled
			if (s == 0 && u.stage[0][0] == 0) { u.stage[0][0] = 4; u.stage[0][1] = 2; u.stage[0][2] = 0; u.stage[0][3] = 2; u.stage[1][0] = 2; u.stage[1][1] = 0; }
			if (s > 0 && u.stage[2 * s][0] == 0) u.stage[2 * s][0] = 1;
			if (u.stage[2 * s][3] == 0) u.stage[2 * s][3] = 1;
		}
		memcpy(u.world, m_matrix[256], 64);
		memcpy(u.worldView, wv, 64);
		for (int s = 0; s < 8; ++s)
		{
			const DWORD index = m_stageStates[s][D3DTSS_TEXCOORDINDEX];
			const DWORD flags = m_stageStates[s][D3DTSS_TEXTURETRANSFORMFLAGS];
			memcpy(u.texMatrix[s], m_matrix[D3DTS_TEXTURE0 + s], 64);
			u.texGen[s][0] = (index >> 16) & 3;						// 0 pass through, 1 camera space normal, 2 camera space position, 3 reflection vector
			u.texGen[s][1] = index & 3;
			u.texGen[s][2] = flags & 7;								// D3DTTFF_COUNT1..4, 0 = transform off
			u.texGen[s][3] = (flags & D3DTTFF_PROJECTED) ? 1 : 0;
		}
		{
			// fixed-function lighting needs normals in the vertices
			const bool lighting = m_renderStates[D3DRS_LIGHTING] != 0 && (fvf & 0x10) != 0 && !pretransformed;
			u.lightFlags[0] = lighting ? 1 : 0;
			const bool colorVertex = m_renderStates[D3DRS_COLORVERTEX] != 0;
			const bool hasDiffuse = (fvf & 0x40) != 0, hasSpecular = (fvf & 0x80) != 0;
			auto source = [&](DWORD mcs) -> uint32_t { return (colorVertex && ((mcs == D3DMCS_COLOR1 && hasDiffuse) || (mcs == D3DMCS_COLOR2 && hasSpecular))) ? 1u : 0u; };
			u.lightFlags[1] = source(m_renderStates[D3DRS_DIFFUSEMATERIALSOURCE]);
			u.lightFlags[2] = source(m_renderStates[D3DRS_AMBIENTMATERIALSOURCE]);
			u.lightFlags[3] = source(m_renderStates[D3DRS_EMISSIVEMATERIALSOURCE]);
			auto color4 = [](float* out, const D3DCOLORVALUE& c) { out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = c.a; };
			color4(u.matDiffuse, m_material.Diffuse); color4(u.matAmbient, m_material.Ambient); color4(u.matEmissive, m_material.Emissive);
			const DWORD amb = m_renderStates[D3DRS_AMBIENT];
			u.sceneAmbient[0] = ((amb >> 16) & 255) / 255.f; u.sceneAmbient[1] = ((amb >> 8) & 255) / 255.f; u.sceneAmbient[2] = (amb & 255) / 255.f; u.sceneAmbient[3] = 1.f;
			uint32_t n = 0;
			for (int i = 0; i < 8 && n < 4; ++i)
			{
				if (!m_lightOn[i])
					continue;
				const D3DLIGHT8& l = m_light[i];
				color4(u.lightDiffuse[n], l.Diffuse); color4(u.lightAmbient[n], l.Ambient);
				u.lightPosType[n][0] = l.Position.x; u.lightPosType[n][1] = l.Position.y; u.lightPosType[n][2] = l.Position.z; u.lightPosType[n][3] = (float)l.Type;
				u.lightDirRange[n][0] = l.Direction.x; u.lightDirRange[n][1] = l.Direction.y; u.lightDirRange[n][2] = l.Direction.z; u.lightDirRange[n][3] = l.Range;
				u.lightAtten[n][0] = l.Attenuation0; u.lightAtten[n][1] = l.Attenuation1; u.lightAtten[n][2] = l.Attenuation2;
				++n;
			}
			u.lightInfo[0] = n;
		}
		memcpy((char*)B.ringAlloc.mapped + uOff, &u, sizeof(u));

		PipeKey key;
		memset(&key, 0, sizeof(key));
		key.fvf = fvf; key.stride = stride; key.pretransformed = pretransformed;
		switch (type)
		{
		case D3DPT_POINTLIST: key.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
		case D3DPT_LINELIST: key.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
		case D3DPT_LINESTRIP: key.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
		case D3DPT_TRIANGLELIST: key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
		case D3DPT_TRIANGLESTRIP: key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
		default: key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
		}
		key.blendEnable = m_renderStates[D3DRS_ALPHABLENDENABLE] ? 1 : 0;
		key.srcBlend = m_renderStates[D3DRS_SRCBLEND]; key.dstBlend = m_renderStates[D3DRS_DESTBLEND]; key.blendOp = m_renderStates[D3DRS_BLENDOP];
		key.depthTest = m_renderStates[D3DRS_ZENABLE] ? 1 : 0; key.depthWrite = m_renderStates[D3DRS_ZWRITEENABLE] ? 1 : 0;
		key.depthFunc = m_renderStates[D3DRS_ZFUNC] ? m_renderStates[D3DRS_ZFUNC] - 1 : VK_COMPARE_OP_LESS_OR_EQUAL;
		key.stencilEnable = m_renderStates[D3DRS_STENCILENABLE] ? 1 : 0;
		if (key.stencilEnable)
		{
			key.stencilFunc = m_renderStates[D3DRS_STENCILFUNC]; key.stencilFail = m_renderStates[D3DRS_STENCILFAIL];
			key.stencilZFail = m_renderStates[D3DRS_STENCILZFAIL]; key.stencilPass = m_renderStates[D3DRS_STENCILPASS];
			key.stencilMask = m_renderStates[D3DRS_STENCILMASK]; key.stencilWriteMask = m_renderStates[D3DRS_STENCILWRITEMASK];
		}
		key.ps = (uint32_t)waterKind;
		key.hdr = (B.postOn && B.stage == 0 && !B.curTarget && B.hdr) ? 1 : 0;
		key.cull = m_renderStates[D3DRS_CULLMODE]; key.colorMask = m_renderStates[D3DRS_COLORWRITEENABLE] & 0xF;
		VkPipeline pipe = GetPipeline(key);
		if (!pipe) return D3D_OK;
		if (B.stage == 0 && !B.curTarget && !pretransformed && !waterKind && ShadowsEnabled())
		{
			static uint32_t cnt[8]; static int n = 0;
			++cnt[0]; if (!key.depthWrite) ++cnt[1]; if (key.blendEnable) ++cnt[2]; if (key.stencilEnable) ++cnt[3]; if (!key.colorMask) ++cnt[4]; if (key.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && key.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) ++cnt[5];
			if (++n % 3000 == 0) Log("scene draws %u: no zwrite %u, blend %u, stencil %u, no colour %u, other topology %u", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5]);
		}
		if (!(dbg & 2) && B.stage == 0 && !B.curTarget && !pretransformed && !waterKind && ShadowsEnabled() && key.depthWrite && !key.blendEnable && key.colorMask
			&& (key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP))
		{
			GpuTexture* t0 = (m_tex[0] && u.flags[3]) ? EnsureGpuTexture(static_cast<NullTexture*>(m_tex[0])) : nullptr;
			RecordCaster(u, vOff, iOff, (uint32_t)numVertices, (uint32_t)indexCount, wide, key.topology, fvf, stride, minIndex, t0, m_matrix[256][14]);
		}
		if (g_frame == 50000)
		{
			char line[300];
			_snprintf(line, sizeof(line), "fvf %03X verts %u idx %u tex0 %s | world %.0f %.0f %.0f | zen %u zw %u blend %u cull %u | stencil %u", fvf, (unsigned)numVertices, indexCount,
				m_tex[0] ? "y" : "n", m_matrix[256][12], m_matrix[256][13], m_matrix[256][14], key.depthTest, key.depthWrite, key.blendEnable, key.cull, m_renderStates[D3DRS_STENCILENABLE]);
			g_drawTrace.push_back(line);
		}

		vkCmdBindPipeline(B.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		VkViewport vp{ 0, 0, (float)B.passExtent.width, (float)B.passExtent.height, 0.0f, 1.0f };
		VkRect2D sc{ { 0, 0 }, B.passExtent };
		if (!pretransformed)
		{
			vp.x = (float)m_viewport.X; vp.y = (float)m_viewport.Y; vp.width = (float)m_viewport.Width; vp.height = (float)m_viewport.Height;
			vp.minDepth = m_viewport.MinZ; vp.maxDepth = m_viewport.MaxZ;
		}
		sc.offset = { (int32_t)m_viewport.X, (int32_t)m_viewport.Y };
		sc.extent = { std::min<uint32_t>(m_viewport.Width, B.passExtent.width - std::min<uint32_t>(m_viewport.X, B.passExtent.width)),
			std::min<uint32_t>(m_viewport.Height, B.passExtent.height - std::min<uint32_t>(m_viewport.Y, B.passExtent.height)) };
		vkCmdSetViewport(B.cmd, 0, 1, &vp);
		vkCmdSetScissor(B.cmd, 0, 1, &sc);
		vkCmdSetStencilReference(B.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, m_renderStates[D3DRS_STENCILREF]);

		VkDescriptorBufferInfo ubo{ B.ring, uOff, sizeof(DrawUbo) };
		VkDescriptorImageInfo imgs[8];
		VkWriteDescriptorSet w[10] = {};
		const int stageCount = waterKind ? 7 : 4;
		w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ubo;
		for (int s = 0; s < stageCount; ++s)
		{
			GpuTexture* g = (m_tex[s] && (waterKind || u.stage[2 * s][0] != 1)) ? EnsureGpuTexture(static_cast<NullTexture*>(m_tex[s])) : B.white;
			if (!g || g == B.passTarget) g = B.white;
			const DWORD* t = m_stageStates[s];
			imgs[s] = { GetSampler(t[D3DTSS_MINFILTER], t[D3DTSS_MAGFILTER], t[D3DTSS_MIPFILTER], t[D3DTSS_ADDRESSU] ? t[D3DTSS_ADDRESSU] : 1, t[D3DTSS_ADDRESSV] ? t[D3DTSS_ADDRESSV] : 1, t[D3DTSS_MAXANISOTROPY]),
				g->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
			w[1 + s].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1 + s].dstBinding = 1 + s; w[1 + s].descriptorCount = 1;
			w[1 + s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1 + s].pImageInfo = &imgs[s];
		}
		VkDescriptorBufferInfo psUbo{ B.ring, 0, 512 };
		uint32_t writes = 1 + stageCount;
		if (waterKind)
		{
			const VkDeviceSize pOff = ((B.ringCursor + B.uboAlign - 1) / B.uboAlign) * B.uboAlign;
			if (pOff + 512 <= B.ringSize)
			{
				memcpy((char*)B.ringAlloc.mapped + pOff, m_psConst, 512);
				B.ringCursor = pOff + 512;
				psUbo.offset = pOff;
				w[writes].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[writes].dstBinding = 9; w[writes].descriptorCount = 1;
				w[writes].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[writes].pBufferInfo = &psUbo;
				++writes;
			}
			else
				return D3D_OK;
		}
		vkCmdPushDescriptorSetKHR(B.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, B.pipeLayout, 0, writes, w);

		VkBuffer bufs[2] = { B.ring, B.defaults };
		VkDeviceSize offs[2] = { vOff, 0 };
		vkCmdBindVertexBuffers(B.cmd, 0, 2, bufs, offs);
		if (indexCount)
		{
			vkCmdBindIndexBuffer(B.cmd, B.ring, iOff, wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
			vkCmdDrawIndexed(B.cmd, indexCount, 1, 0, -minIndex, 0);
		}
		else
			vkCmdDraw(B.cmd, numVertices, 1, 0, 0);
		return D3D_OK;
	}

	float m_matrix[512][16];
	D3DMATERIAL8 m_material{};
	D3DLIGHT8 m_light[8]{};
	bool m_lightOn[8] = {};
	IDirect3DBaseTexture8* m_tex[8] = {};
	NullVertexBuffer* m_vb = nullptr;
	NullIndexBuffer* m_ib = nullptr;
	UINT m_stride = 0, m_baseVertex = 0;
	DWORD m_vertexShader = 0, m_pixelShader = 0;
	float m_psConst[32][4] = {};
	std::map<DWORD, int> m_psKind;		// pixel shader handle -> 1 updated river water, 2 updated sea water
};

class VkD3D : public IDirect3D8NullBase
{
public:
	NULLGFX_REFCOUNT
	STDMETHOD_(UINT, GetAdapterCount)(THIS) override { return 1; }
	STDMETHOD(GetAdapterIdentifier)(THIS_ UINT, DWORD, D3DADAPTER_IDENTIFIER8* id) override
	{
		memset(id, 0, sizeof(*id));
		strcpy(id->Driver, "vulkan"); strcpy(id->Description, "Native Vulkan backend");
		return D3D_OK;
	}
	STDMETHOD_(UINT, GetAdapterModeCount)(THIS_ UINT) override { return NullGfx_ModeCount(); }
	STDMETHOD(EnumAdapterModes)(THIS_ UINT, UINT mode, D3DDISPLAYMODE* m) override { return NullGfx_Mode(mode, m); }
	STDMETHOD(GetAdapterDisplayMode)(THIS_ UINT, D3DDISPLAYMODE* m) override { return NullGfx_GetDesktopMode(m); }
	STDMETHOD(GetDeviceCaps)(THIS_ UINT adapter, D3DDEVTYPE, D3DCAPS8* caps) override { return NullGfx_FillCaps(adapter, caps); }
	STDMETHOD(CreateDevice)(THIS_ UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice8** out) override
	{
		*out = new VkDevice8(this, *pp, focus, adapter, type, behavior);
		return D3D_OK;
	}
};

} // namespace

bool VkGfx_Requested()
{
	char value[32] = {};
	DWORD n = GetEnvironmentVariableA("GENERALS_GFX", value, sizeof(value));
	return n > 0 && n < sizeof(value) && _stricmp(value, "vulkan") == 0;
}

IDirect3D8* WINAPI VkGfx_Direct3DCreate8(UINT)
{
	g_nullGfxAdvertiseShaders = false;
	return new VkD3D();
}

// Scene boundaries, called by the engine around the 3D views (W3DDisplay::draw): everything in between is drawn into the HDR image and goes
// through the post processing chain; the interface drawn afterwards goes straight onto the swapchain and stays sharp.
void VkGfx_BeginScene3D(float sunX, float sunY, float sunZ)
{
	if (!g_sh.haveSun)
	{
		// One fixed sun for the whole session: azimuth from the map's light the first time, elevation fixed. GENERALS_SUN="azimuth,elevation" (degrees) overrides.
		float az = atan2f(sunY, sunX), el = 50.0f * 3.14159265f / 180.0f;
		if (const char* e = getenv("GENERALS_SUN")) { float a = 0, b = 0; if (sscanf(e, "%f,%f", &a, &b) == 2) { az = a * 3.14159265f / 180.0f; el = b * 3.14159265f / 180.0f; } }
		g_sh.sun[0] = cosf(el) * cosf(az); g_sh.sun[1] = cosf(el) * sinf(az); g_sh.sun[2] = sinf(el);
		g_sh.haveSun = true;
	}
	g_sh.casters.clear();
	B.projCaptured = false;
	{ static int n = 0; if (n++ < 3) Log("BeginScene3D: ready %d post %d hdr %d stage %d", (int)B.ready, (int)B.postOn, B.hdr != nullptr, B.stage); }
	if (!B.ready || !B.postOn || !B.hdr || B.stage == 0) return;
	EndPass();
	B.stage = 0; B.scene3D = true; B.colorLoaded = false; B.depthLoaded = false;
}
bool VkGfx_ShadowMapsActive() { return g_sh.on && B.postOn && B.ready; }
void VkGfx_SuppressSceneDraws(bool s) { g_sh.suppress = s; }
void VkGfx_EndScene3D()
{
	if (!B.ready || !B.postOn || B.stage != 0) return;
	if (!RunPostProcess()) { B.stage = 1; B.scene3D = false; }
}

#else	// no Vulkan headers in this configuration

bool VkGfx_Requested() { return false; }
IDirect3D8* WINAPI VkGfx_Direct3DCreate8(UINT) { return nullptr; }
void VkGfx_BeginScene3D(float, float, float) {}
bool VkGfx_ShadowMapsActive() { return false; }
void VkGfx_SuppressSceneDraws(bool) {}
void VkGfx_EndScene3D() {}

#endif

