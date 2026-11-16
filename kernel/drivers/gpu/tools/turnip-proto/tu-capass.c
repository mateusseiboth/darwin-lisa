/*
 * tu-capass: CoreAnimation's render passes, as the Metal -> Vulkan layer would hand them to turnip, recorded and
 * submitted through turnip with no loader (the ICD's own vk_icdGetInstanceProcAddr), on any msm device. On the host
 * that is Mesa's drm-shim (freedreno_noop, FD_GPU_ID=635: the Adreno 642L's A635 row, 512 KB of GMEM): nothing runs,
 * but turnip makes every decision it would make on the phone (GMEM or sysmem, bins, binning, UBWC, LRZ) and records
 * every packet, so its CPU cost and its choices can be read here. MESA_GPU_TRACES=print prints the choices
 * (render_pass: bins and their size, ubwc; draw_ib_gmem / draw_ib_sysmem / binning_ib).
 *
 *   tu-capass LIB SCENARIO [ITERATIONS]
 *   SCENARIO: main    8 full-screen textured, tinted, blended layers into the 1080x2400 BGRA8 framebuffer (Load/Store),
 *                     optimal tiling (turnip: UBWC), 2 full-screen RGBA8 layers sampled
 *             linear  the same into a LINEAR target (the IOSurface the display scans, drawn in place)
 *             home    80 icon quads of 180x180 over a full-screen wallpaper, 4 icon textures (SpringBoard's home pass)
 *             fetch   the main pass with the blend done in the shader from [[color(0)]] (fc_framebuffer_fetch, every
 *                     draw of CoreAnimation's uber shader): an input attachment in rasterization order
 *                     (VK_EXT_rasterization_order_attachment_access), no fixed-function blend
 *             fetch1  the fetch pass where only the last of the 8 layers reads [[color(0)]], the other 7 blending in fixed
 *                     function with a pipeline without the rasterization-order flag (metal2vulkan sets it only on a
 *                     pipeline whose fragment function reads a colour attachment): what CoreAnimation's mix costs
 *             fetchbar  the fetch pass without rasterization order: the input attachment ordered by a subpass
 *                     self-dependency and a by-region barrier between draws (metal2vulkan's fallback when the device
 *                     has no rasterization-order access)
 *             blur    a backdrop pass: 4 draws into a 540x1200 RGBA16F target (Clear/Store), then 8 layers into the
 *                     framebuffer that sample it (Liquid Glass' downsample + composite, in two passes)
 * Prints the CPU time to record one command buffer (median of ITERATIONS, default 200) and per draw, the time of one
 * vkQueueSubmit + wait, and the GPU time of the command buffer (two timestamps; 0 under drm-shim, real on pmOS).
 * Each iteration is a frame: turnip's autotune learns each pass's choice from the GPU's results of earlier frames.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "spirv.h"

#define W 1080
#define H 2400
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;

#define FN(name) static PFN_##name name
FN(vkCreateInstance); FN(vkEnumeratePhysicalDevices); FN(vkGetPhysicalDeviceProperties); FN(vkCreateDevice);
FN(vkGetPhysicalDeviceMemoryProperties); FN(vkGetPhysicalDeviceFormatProperties); FN(vkGetDeviceQueue);
FN(vkCreateImage); FN(vkGetImageMemoryRequirements); FN(vkAllocateMemory); FN(vkBindImageMemory); FN(vkCreateImageView);
FN(vkCreateBuffer); FN(vkGetBufferMemoryRequirements); FN(vkBindBufferMemory); FN(vkCreateSampler);
FN(vkCreateRenderPass); FN(vkCreateFramebuffer); FN(vkCreateShaderModule); FN(vkCreateDescriptorSetLayout);
FN(vkCreatePipelineLayout); FN(vkCreateGraphicsPipelines); FN(vkCreateDescriptorPool); FN(vkAllocateDescriptorSets);
FN(vkUpdateDescriptorSets); FN(vkCreateCommandPool); FN(vkAllocateCommandBuffers); FN(vkResetCommandPool);
FN(vkBeginCommandBuffer); FN(vkEndCommandBuffer); FN(vkCmdBeginRenderPass); FN(vkCmdEndRenderPass);
FN(vkCmdBindPipeline); FN(vkCmdBindDescriptorSets); FN(vkCmdBindVertexBuffers); FN(vkCmdPushConstants);
FN(vkCmdDraw); FN(vkCmdSetViewport); FN(vkCmdSetScissor); FN(vkCmdPipelineBarrier); FN(vkCreateFence);
FN(vkQueueSubmit); FN(vkWaitForFences); FN(vkResetFences); FN(vkGetPhysicalDeviceFeatures2);
FN(vkCreateQueryPool); FN(vkCmdResetQueryPool); FN(vkCmdWriteTimestamp); FN(vkGetQueryPoolResults);
FN(vkMapMemory); FN(vkCmdBeginQuery); FN(vkCmdEndQuery);

static void load_instance_functions(void) {
#define I(name) name = (PFN_##name)gipa(instance, #name)
    I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkCreateDevice);
    I(vkGetPhysicalDeviceMemoryProperties); I(vkGetPhysicalDeviceFormatProperties); I(vkGetPhysicalDeviceFeatures2);
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#undef I
}

static void load_device_functions(void) {
#define D(name) name = (PFN_##name)gdpa(device, #name)
    D(vkGetDeviceQueue); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory);
    D(vkCreateImageView); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory); D(vkCreateSampler);
    D(vkCreateRenderPass); D(vkCreateFramebuffer); D(vkCreateShaderModule); D(vkCreateDescriptorSetLayout);
    D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines); D(vkCreateDescriptorPool); D(vkAllocateDescriptorSets);
    D(vkUpdateDescriptorSets); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkResetCommandPool);
    D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkCmdBeginRenderPass); D(vkCmdEndRenderPass);
    D(vkCmdBindPipeline); D(vkCmdBindDescriptorSets); D(vkCmdBindVertexBuffers); D(vkCmdPushConstants); D(vkCmdDraw);
    D(vkCmdSetViewport); D(vkCmdSetScissor); D(vkCmdPipelineBarrier); D(vkCreateFence); D(vkQueueSubmit);
    D(vkWaitForFences); D(vkResetFences); D(vkCreateQueryPool); D(vkCmdResetQueryPool); D(vkCmdWriteTimestamp);
    D(vkGetQueryPoolResults); D(vkMapMemory); D(vkCmdBeginQuery); D(vkCmdEndQuery);
#undef D
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(physical, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    fprintf(stderr, "no memory type\n");
    exit(1);
}

typedef struct { VkImage image; VkImageView view; } Image;

static Image make_image(uint32_t w, uint32_t h, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage) {
    Image t;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = (VkExtent3D){ w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = tiling;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(device, &ci, NULL, &t.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, t.image, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
        memory_type(req.memoryTypeBits, tiling == VK_IMAGE_TILING_LINEAR ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0) };
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &ai, NULL, &memory));
    CHECK(vkBindImageMemory(device, t.image, memory, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    CHECK(vkCreateImageView(device, &vi, NULL, &t.view));
    return t;
}

typedef struct {
    VkRenderPass pass;
    VkFramebuffer framebuffer;
    VkPipeline pipeline;
    VkPipeline plain;       /* fixed-function blend, no rasterization-order flag, for the same pass (fetch1) */
    uint32_t w, h;
} Target;

/* how the pass reads its colour attachment: not at all, in rasterization order, or ordered by barriers */
enum { FETCH_NONE, FETCH_ORDER, FETCH_BARRIER };

static VkPipelineLayout layout;
static VkDescriptorSetLayout set_layout;
static VkShaderModule vs, fs, fs_fetch;

static VkRenderPass make_pass(VkFormat format, VkAttachmentLoadOp load, int fetch_mode) {
    int fetch = fetch_mode != FETCH_NONE;
    VkImageLayout layout = fetch ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription a = { 0, format, VK_SAMPLE_COUNT_1_BIT, load, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
        load == VK_ATTACHMENT_LOAD_OP_LOAD ? layout : VK_IMAGE_LAYOUT_UNDEFINED, layout };
    VkAttachmentReference ref = { 0, layout };
    /* the attachment is also input attachment 0, read in rasterization order */
    VkSubpassDescription sub = { fetch_mode == FETCH_ORDER ? VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT : 0,
        VK_PIPELINE_BIND_POINT_GRAPHICS, fetch ? 1 : 0, fetch ? &ref : NULL, 1, &ref, NULL, NULL, 0, NULL };
    /* FETCH_BARRIER: a self-dependency, so that vkCmdPipelineBarrier may order one draw's writes before the next's reads */
    VkSubpassDependency self = { 0, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT };
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &a, 1, &sub,
        fetch_mode == FETCH_BARRIER ? 1 : 0, fetch_mode == FETCH_BARRIER ? &self : NULL };
    VkRenderPass pass;
    CHECK(vkCreateRenderPass(device, &ci, NULL, &pass));
    return pass;
}

/* fetch: the shader reads the attachment (no fixed-function blend); order: with the rasterization-order flag */
static VkPipeline make_pipeline(VkRenderPass pass, int fetch, int order) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fetch ? fs_fetch : fs,
          "main", NULL },
    };
    VkVertexInputBindingDescription binding = { 0, 8, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attribute = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1,
        &binding, 1, &attribute };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_FALSE };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    /* CoreAnimation's premultiplied "source over" */
    VkPipelineColorBlendAttachmentState blend = { fetch ? VK_FALSE : VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.flags = order ? VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT : 0;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = layout;
    ci.renderPass = pass;
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    return p;
}

static Target make_target(Image img, uint32_t w, uint32_t h, VkFormat format, VkAttachmentLoadOp load, int fetch_mode) {
    Target t = { make_pass(format, load, fetch_mode), VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, w, h };
    VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, t.pass, 1, &img.view, w, h, 1 };
    CHECK(vkCreateFramebuffer(device, &ci, NULL, &t.framebuffer));
    t.pipeline = make_pipeline(t.pass, fetch_mode != FETCH_NONE, fetch_mode == FETCH_ORDER);
    t.plain = fetch_mode == FETCH_NONE ? t.pipeline : make_pipeline(t.pass, 0, 0);
    return t;
}

static VkDescriptorSet make_set(VkSampler sampler, VkImageView view, VkImageView input) {
    static VkDescriptorPool pool;
    if (!pool) {
        VkDescriptorPoolSize size[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 16 } };
        VkDescriptorPoolCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 16, 2, size };
        CHECK(vkCreateDescriptorPool(device, &ci, NULL, &pool));
    }
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &set_layout };
    CHECK(vkAllocateDescriptorSets(device, &ai, &set));
    VkDescriptorImageInfo ii = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL };
    vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
    if (input) {
        VkDescriptorImageInfo ia = { VK_NULL_HANDLE, input, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wi = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1,
            VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &ia, NULL, NULL };
        vkUpdateDescriptorSets(device, 1, &wi, 0, NULL);
    }
    return set;
}

/* one layer: its texture, its tint (setFragmentBytes), its quad (4 vertices from `first` in the vertex buffer) */
static void layer(VkCommandBuffer cb, VkDescriptorSet set, uint32_t first, float alpha) {
    float tint[4] = { alpha, alpha, alpha, alpha };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tint), tint);
    vkCmdDraw(cb, 4, 1, first, 0);
}

static void begin(VkCommandBuffer cb, const Target* t, VkBuffer vb) {
    VkClearValue clear = { { { 0, 0, 0, 0 } } };
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, t->pass, t->framebuffer,
        { { 0, 0 }, { t->w, t->h } }, 1, &clear };
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, t->pipeline);
    VkViewport v = { 0, 0, (float)t->w, (float)t->h, 0, 1 };
    VkRect2D s = { { 0, 0 }, { t->w, t->h } };
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vb, &zero);
}

/* a quad as a 4-vertex strip, clip-space corners (x0,y0)-(x1,y1) */
static void write_quad(float* v, float x0, float y0, float x1, float y1) {
    const float q[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    memcpy(v, q, sizeof(q));
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int by_value(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s libvulkan_freedreno.so caps|main|linear|home|fetch|fetch1|fetchbar|blur [iterations]\n", argv[0]);
        return 2;
    }
    const char* scenario = argv[2];
    int iterations = argc > 3 ? atoi(argv[3]) : 200;
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "tu-capass", 1, NULL, 0, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    load_instance_functions();
    uint32_t n = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, &physical));
    if (n == 0) {
        fprintf(stderr, "no Vulkan device (no /dev/dri render node, or no drm-shim)\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT order = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT };
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &order };
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (strcmp(scenario, "caps") == 0) {
        printf("%s (Vulkan %u.%u.%u): rasterizationOrderColorAttachmentAccess %u\n", props.deviceName,
               VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
               VK_API_VERSION_PATCH(props.apiVersion), order.rasterizationOrderColorAttachmentAccess);
        return 0;
    }
    order.rasterizationOrderDepthAttachmentAccess = order.rasterizationOrderStencilAttachmentAccess = VK_FALSE;
    float priority = 1;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &priority };
    const char* extensions[] = { VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME };
    VkPhysicalDeviceFeatures2 enable = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &order };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enable, 0, 1, &qci, 0, NULL, 1, extensions };
    CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    load_device_functions();
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(vs_spv), vs_spv };
    CHECK(vkCreateShaderModule(device, &smi, NULL, &vs));
    smi.codeSize = sizeof(fs_spv);
    smi.pCode = fs_spv;
    CHECK(vkCreateShaderModule(device, &smi, NULL, &fs));
    smi.codeSize = sizeof(fs_fetch_spv);
    smi.pCode = fs_fetch_spv;
    CHECK(vkCreateShaderModule(device, &smi, NULL, &fs_fetch));
    VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                                          { 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo sli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b };
    CHECK(vkCreateDescriptorSetLayout(device, &sli, NULL, &set_layout));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &set_layout, 1, &pcr };
    CHECK(vkCreatePipelineLayout(device, &pli, NULL, &layout));
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    VkSampler sampler;
    CHECK(vkCreateSampler(device, &sci, NULL, &sampler));

    /* the vertex buffer: quad 0 full screen, quads 1..80 the icon grid (4 x 20 of 180x180 px), strip order */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 81 * 4 * 8, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT };
    VkBuffer vb;
    CHECK(vkCreateBuffer(device, &bci, NULL, &vb));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, vb, &req);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory vbm;
    CHECK(vkAllocateMemory(device, &mai, NULL, &vbm));
    CHECK(vkBindBufferMemory(device, vb, vbm, 0));
    /* Until 2026-11-16 this buffer was never written: every quad was four vertices at (0,0), nothing was rasterised,
     * and the GPU column of the first T0 run on lisa (main 9.4 us) measured only the command processor. */
    float* v;
    CHECK(vkMapMemory(device, vbm, 0, VK_WHOLE_SIZE, 0, (void**)&v));
    write_quad(v, -1, -1, 1, 1);
    for (uint32_t i = 1; i <= 80; i++) {
        /* 4 columns with a pitch of 270 px, 20 rows with a pitch of 120 px: 80 icons of 180x180 covering ~1.1 screens */
        float x = 45 + 270 * ((i - 1) % 4), y = 0 + 120 * ((i - 1) / 4);
        write_quad(v + 8 * i, 2 * x / W - 1, 2 * y / H - 1, 2 * (x + 180) / W - 1, 2 * (y + 180) / H - 1);
    }

    const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    int linear = strcmp(scenario, "linear") == 0;
    int fetch_mode = strcmp(scenario, "fetch") == 0 || strcmp(scenario, "fetch1") == 0 ? FETCH_ORDER
                   : strcmp(scenario, "fetchbar") == 0 ? FETCH_BARRIER : FETCH_NONE;
    int fetch = fetch_mode != FETCH_NONE;
    if (linear) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_B8G8R8A8_UNORM, &fp);
        printf("BGRA8 linear tiling: colour attachment %s, blend %s\n",
               (fp.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? "yes" : "NO",
               (fp.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) ? "yes" : "NO");
    }
    Image fb = make_image(W, H, VK_FORMAT_B8G8R8A8_UNORM, linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL, rt);
    Image layers[2] = { make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL, rt),
                        make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL, rt) };
    Image icons[4];
    for (int i = 0; i < 4; i++) {
        icons[i] = make_image(256, 256, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL, rt);
    }
    Image backdrop = make_image(W / 2, H / 2, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL, rt);
    VkImageView input = fetch ? fb.view : VK_NULL_HANDLE;
    VkDescriptorSet layer_set[2] = { make_set(sampler, layers[0].view, input), make_set(sampler, layers[1].view, input) };
    VkDescriptorSet icon_set[4];
    for (int i = 0; i < 4; i++) {
        icon_set[i] = make_set(sampler, icons[i].view, VK_NULL_HANDLE);
    }
    VkDescriptorSet backdrop_set = make_set(sampler, backdrop.view, VK_NULL_HANDLE);
    Target main_target = make_target(fb, W, H, VK_FORMAT_B8G8R8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD, fetch_mode);
    Target blur_target = make_target(backdrop, W / 2, H / 2, VK_FORMAT_R16G16B16A16_SFLOAT, VK_ATTACHMENT_LOAD_OP_CLEAR, 0);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0 };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(device, &cai, &cb));
    VkFence fence;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK(vkCreateFence(device, &fci, NULL, &fence));

    VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2 };
    VkQueryPool queries;
    CHECK(vkCreateQueryPool(device, &qpi, NULL, &queries));
    /* samples the main pass shaded (one occlusion query, first iteration only): proof that the quads cover the screen */
    VkQueryPoolCreateInfo oqi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_OCCLUSION, 1 };
    VkQueryPool samples_query;
    CHECK(vkCreateQueryPool(device, &oqi, NULL, &samples_query));
    double* gpu = calloc(iterations, sizeof(double));
    double* record = calloc(iterations, sizeof(double));
    double* submit = calloc(iterations, sizeof(double));
    uint32_t draws = 0;
    for (int it = 0; it < iterations; it++) {
        CHECK(vkResetCommandPool(device, pool, 0));
        double t0 = now_us();
        VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        CHECK(vkBeginCommandBuffer(cb, &cbi));
        draws = 0;
        vkCmdResetQueryPool(cb, queries, 0, 2);
        if (it == 0) {
            vkCmdResetQueryPool(cb, samples_query, 0, 1);
        }
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        if (strcmp(scenario, "blur") == 0) {
            begin(cb, &blur_target, vb);
            for (int i = 0; i < 4; i++, draws++) {
                layer(cb, layer_set[i & 1], 0, 0.25f);
            }
            vkCmdEndRenderPass(cb);
            VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, backdrop.image,
                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                 0, NULL, 0, NULL, 1, &ib);
        }
        begin(cb, &main_target, vb);
        if (it == 0) {
            vkCmdBeginQuery(cb, samples_query, 0, 0);
        }
        if (strcmp(scenario, "home") == 0) {
            layer(cb, layer_set[0], 0, 1.0f);
            draws++;
            for (uint32_t i = 1; i <= 80; i++, draws++) {
                layer(cb, icon_set[i & 3], 4 * i, 1.0f);
            }
        } else {
            for (int i = 0; i < 8; i++, draws++) {
                if (fetch_mode == FETCH_BARRIER && i > 0) {
                    /* the previous draw's colour writes before this draw's input-attachment reads, per region */
                    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_INPUT_ATTACHMENT_READ_BIT };
                    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                         VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
                }
                if (strcmp(scenario, "fetch1") == 0) {
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, i < 7 ? main_target.plain : main_target.pipeline);
                }
                layer(cb, strcmp(scenario, "blur") == 0 && i == 0 ? backdrop_set : layer_set[i & 1], 0, 0.5f);
            }
        }
        if (it == 0) {
            vkCmdEndQuery(cb, samples_query, 0);
        }
        vkCmdEndRenderPass(cb);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        CHECK(vkEndCommandBuffer(cb));
        double t1 = now_us();
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(device, 1, &fence));
        submit[it] = now_us() - t1;
        uint64_t ts[2] = { 0, 0 };
        vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        gpu[it] = (ts[1] - ts[0]) * props.limits.timestampPeriod / 1000.0;
        record[it] = t1 - t0;
        if (it == 0) {
            uint64_t shaded = 0;
            vkGetQueryPoolResults(device, samples_query, 0, 1, sizeof(shaded), &shaded, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT);
            /* main/linear/fetch/blur: 8 full screens = 20736000; home: 1 screen + 76 x 32400 + the last row clipped to
             * 120 lines (4 x 21600) = 5140800 */
            printf("samples shaded in the main pass: %llu\n", (unsigned long long)shaded);
        }
    }
    qsort(record, iterations, sizeof(double), by_value);
    qsort(submit, iterations, sizeof(double), by_value);
    qsort(gpu, iterations, sizeof(double), by_value);
    printf("%s on %s: %u draws, record %.1f us (%.2f us a draw), submit+wait %.1f us, GPU %.1f us (medians of %d)\n",
           scenario, props.deviceName, draws, record[iterations / 2], record[iterations / 2] / draws, submit[iterations / 2],
           gpu[iterations / 2], iterations);
    return 0;
}
