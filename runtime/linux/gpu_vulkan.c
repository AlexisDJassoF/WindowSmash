/* Linux: the Vulkan renderer (openspec change vulkan-renderer), chosen with SSBU_RENDERER=vulkan
 * (gpu_backend_select.c). This file finds the device, makes it, and holds what the renderer's other
 * files share (gpu_vk.h); the gfx_* table is at its end.
 *
 * Vulkan itself is the loader SDL opened for the window (linux/window.c makes it with
 * SDL_WINDOW_VULKAN): its vkGetInstanceProcAddr, and from it every other function, so the program
 * links against no Vulkan library. The device must have Vulkan 1.3 with dynamic rendering,
 * synchronization2, maintenance4 and timeline semaphores, VK_EXT_robustness2's
 * robustBufferAccess2 and nullDescriptor (a read out of range gives 0, as in Direct3D 11), a
 * queue that draws, computes and presents to the window, and VK_KHR_swapchain. What only makes
 * it faster or smoother is optional, each with another way when it is missing: extended dynamic
 * state 3, graphics pipeline libraries, unified image layouts, descriptor buffers, custom border
 * colours, swapchain maintenance.
 *
 * SSBU_VK_VALIDATE=1 turns on the Khronos validation layer, with synchronization validation; its
 * messages go to the log as [vk] lines.
 *
 * The renderer draws the whole game (its videos not yet checked); it runs when asked for by name
 * (SSBU_RENDERER=vulkan, display.ini's renderer = vulkan), DXVK being the default. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_vk.h"
#include <SDL.h>
#include <SDL_vulkan.h>

#include "gpu_backend_select.h"
#include "gpu_overlay_quads.h"

void *rt_window(void);

/* ---- functions ---------------------------------------------------------------------- */
#define VK_GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties) \
    X(vkEnumerateInstanceVersion)
#define VK_PROBE_FNS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkDestroySurfaceKHR) X(vkCreateDebugUtilsMessengerEXT) \
    X(vkDestroyDebugUtilsMessengerEXT)

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define VK_DEFINE_STATIC(name) static PFN_##name name;
#define VK_DEFINE(name) PFN_##name name;
VK_GLOBAL_FNS(VK_DEFINE_STATIC)
VK_PROBE_FNS(VK_DEFINE_STATIC)
VK_INSTANCE_LEVEL_FNS(VK_DEFINE)
VK_DEVICE_FNS(VK_DEFINE)

Vk vk;
static VkDebugUtilsMessengerEXT messenger;

static VKAPI_ATTR VkBool32 VKAPI_CALL on_validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                                    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type; (void)user;
    fprintf(stderr, "[vk] %s: %s\n", severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning", data->pMessage);
    return VK_FALSE;
}

static int has_layer(const char *name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, NULL) != VK_SUCCESS || !n) return 0;
    VkLayerProperties *p = malloc(n * sizeof *p);
    int found = 0;
    if (p && vkEnumerateInstanceLayerProperties(&n, p) == VK_SUCCESS)
        for (uint32_t i = 0; i < n && !found; i++) found = !strcmp(p[i].layerName, name);
    free(p);
    return found;
}

/* ---- the instance --------------------------------------------------------------------- */
static int make_instance(char *why, size_t why_size) {
    SDL_Window *window = rt_window();
    if (!window) { snprintf(why, why_size, "no window"); return 0; }
    if (!(vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr())) {
        snprintf(why, why_size, "no Vulkan loader (%s)", SDL_GetError());
        return 0;
    }
#define VK_LOAD_GLOBAL(name) name = (PFN_##name)vkGetInstanceProcAddr(NULL, #name);
    VK_GLOBAL_FNS(VK_LOAD_GLOBAL)
    uint32_t version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3) {
        snprintf(why, why_size, "the Vulkan loader is %u.%u, not 1.3", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version));
        return 0;
    }

    const char *ext[16];
    unsigned n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &n, NULL) || n > 14 || !SDL_Vulkan_GetInstanceExtensions(window, &n, ext)) {
        snprintf(why, why_size, "SDL has no Vulkan surface for the window (%s)", SDL_GetError());
        return 0;
    }
    const char *layers[1];
    uint32_t nlayers = 0;
    const char *validate = getenv("SSBU_VK_VALIDATE");
    if (validate && atoi(validate)) {
        if (has_layer("VK_LAYER_KHRONOS_validation")) {
            layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
            ext[n++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        } else {
            fprintf(stderr, "[gfx] SSBU_VK_VALIDATE: the validation layer is not installed (vulkan-validationlayers)\n");
        }
    }
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features = {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, NULL, 1, &sync, 0, NULL};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "ssbu", 1, "WindowSmash", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nlayers ? &features : NULL, 0, &app, nlayers, layers, n, ext};
    VkResult r = vkCreateInstance(&ci, NULL, &vk.instance);
    if (r != VK_SUCCESS) { snprintf(why, why_size, "cannot create a Vulkan instance (%s)", vk_result(r)); return 0; }
#define VK_LOAD_INSTANCE(name) name = (PFN_##name)vkGetInstanceProcAddr(vk.instance, #name);
    VK_PROBE_FNS(VK_LOAD_INSTANCE)
    VK_INSTANCE_LEVEL_FNS(VK_LOAD_INSTANCE)
    if (nlayers && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
                                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                                 VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                                                 on_validation, NULL};
        vkCreateDebugUtilsMessengerEXT(vk.instance, &mi, NULL, &messenger);
        fprintf(stderr, "[gfx] Vulkan validation on (with synchronization validation)\n");
    }
    if (!SDL_Vulkan_CreateSurface(window, vk.instance, &vk.surface)) {
        snprintf(why, why_size, "cannot make a Vulkan surface for the window (%s)", SDL_GetError());
        return 0;
    }
    return 1;
}

static void close_instance(void) {
    if (vk.surface) vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    if (messenger) vkDestroyDebugUtilsMessengerEXT(vk.instance, messenger, NULL);
    if (vk.instance) vkDestroyInstance(vk.instance, NULL);
    vk.surface = VK_NULL_HANDLE; messenger = VK_NULL_HANDLE; vk.instance = VK_NULL_HANDLE;
}

/* ---- the device it runs on ------------------------------------------------------------- */
typedef struct DeviceInfo {
    VkPhysicalDevice pd;
    VkPhysicalDeviceProperties2 props;
    VkPhysicalDeviceDriverProperties driver;
    uint32_t queue;                     /* the family that draws, computes and presents */
    const char *robustness;             /* the robustness2 extension's name: KHR's or EXT's */
    char missing[160];                  /* the first required thing it lacks, if any */
    char optional_missing[256];         /* the optional ones it lacks */
    struct { int eds3, gpl, unified_layouts, descriptor_buffer, custom_border, swapchain_maintenance, vertex_input; } has;   /* (as Vk's) */
    const char *swapchain_maintenance;  /* that extension's name, KHR's or EXT's */
} DeviceInfo;

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static void note_optional(DeviceInfo *d, int have, const char *name) {
    size_t used = strlen(d->optional_missing);
    if (!have) snprintf(d->optional_missing + used, sizeof d->optional_missing - used, "%s%s", used ? ", " : "", name);
}

static void examine(VkPhysicalDevice pd, DeviceInfo *d) {
    memset(d, 0, sizeof *d);
    d->pd = pd;
    d->driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    d->props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    d->props.pNext = &d->driver;
    vkGetPhysicalDeviceProperties2(pd, &d->props);
    if (d->props.properties.apiVersion < VK_API_VERSION_1_3) {
        snprintf(d->missing, sizeof d->missing, "Vulkan %u.%u, not 1.3", VK_API_VERSION_MAJOR(d->props.properties.apiVersion),
                 VK_API_VERSION_MINOR(d->props.properties.apiVersion));
        return;
    }
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    d->robustness = has_ext(e, n, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME) ? VK_KHR_ROBUSTNESS_2_EXTENSION_NAME
                  : has_ext(e, n, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) ? VK_EXT_ROBUSTNESS_2_EXTENSION_NAME : NULL;

    /* (an optional extension counts only with the features it is wanted for: a driver may list
     * one with them off) */
    int eds3 = has_ext(e, n, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME), gpl = has_ext(e, n, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    int unified = has_ext(e, n, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME), dbuf = has_ext(e, n, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    int border = has_ext(e, n, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME), vin = has_ext(e, n, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unifiedf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT dbuff = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT borderf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT rb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vinf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    void *chain = NULL;
#define CHAIN(have, s) if (have) { (s).pNext = chain; chain = &(s); }
    CHAIN(eds3, eds3f) CHAIN(gpl, gplf) CHAIN(unified, unifiedf) CHAIN(dbuf, dbuff) CHAIN(border, borderf) CHAIN(d->robustness != NULL, rb) CHAIN(vin, vinf)
#undef CHAIN
    VkPhysicalDeviceVulkan13Features v13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, chain};
    VkPhysicalDeviceVulkan12Features v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
    VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
    vkGetPhysicalDeviceFeatures2(pd, &f);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *q = calloc(nq ? nq : 1, sizeof *q);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, q);
    d->queue = UINT32_MAX;
    for (uint32_t i = 0; i < nq && d->queue == UINT32_MAX; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, vk.surface, &present);
        if ((q[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) && present)
            d->queue = i;
    }
    free(q);

    const char *lack = !v13.dynamicRendering ? "dynamic rendering"
                     : !v13.synchronization2 ? "synchronization2"
                     : !v13.maintenance4 ? "maintenance4"
                     : !v12.timelineSemaphore ? "timeline semaphores"
                     : !d->robustness ? "VK_EXT_robustness2"
                     : !rb.robustBufferAccess2 ? "robustBufferAccess2"
                     : !rb.nullDescriptor ? "nullDescriptor"
                     : !has_ext(e, n, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ? "VK_KHR_swapchain"
                     : d->queue == UINT32_MAX ? "a queue that draws, computes and presents to the window"
                     : NULL;
    if (lack) snprintf(d->missing, sizeof d->missing, "no %s", lack);

    /* extended dynamic state 3 is for blending and colour write masks as dynamic state */
    d->has.eds3 = eds3 && eds3f.extendedDynamicState3ColorBlendEnable && eds3f.extendedDynamicState3ColorBlendEquation &&
                  eds3f.extendedDynamicState3ColorWriteMask;
    d->has.gpl = gpl && gplf.graphicsPipelineLibrary;
    d->has.vertex_input = vin && vinf.vertexInputDynamicState;   /* (vertex attributes fetched by the device, their formats dynamic state) */
    d->has.unified_layouts = unified && unifiedf.unifiedImageLayouts;
    d->has.descriptor_buffer = dbuf && dbuff.descriptorBuffer;
    d->has.custom_border = border && borderf.customBorderColors && borderf.customBorderColorWithoutFormat;   /* (the colour as the header has it, for any format) */
    d->swapchain_maintenance = has_ext(e, n, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) ? VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
                             : has_ext(e, n, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) ? VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME : NULL;
    d->has.swapchain_maintenance = d->swapchain_maintenance != NULL;
    note_optional(d, d->has.eds3, "extended dynamic state 3 (blending)");
    note_optional(d, d->has.gpl, "graphics pipeline libraries");
    note_optional(d, d->has.vertex_input, "dynamic vertex input");
    note_optional(d, d->has.unified_layouts, "unified image layouts");
    note_optional(d, d->has.descriptor_buffer, "descriptor buffers");
    note_optional(d, d->has.custom_border, "custom border colours");
    note_optional(d, d->has.swapchain_maintenance, "swapchain maintenance");
    free(e);
}

/* Discrete first, then integrated, virtual, anything else; the CPU (llvmpipe) last. */
static int rank(const DeviceInfo *d) {
    switch (d->props.properties.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 0;
    default: return 1;
    }
}

static int choose_device(DeviceInfo *best, char *why, size_t why_size) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(vk.instance, &n, NULL);
    if (!n) { snprintf(why, why_size, "no Vulkan device"); return 0; }
    VkPhysicalDevice *pds = calloc(n, sizeof *pds);
    vkEnumeratePhysicalDevices(vk.instance, &n, pds);
    int found = 0;
    DeviceInfo d;
    for (uint32_t i = 0; i < n; i++) {
        examine(pds[i], &d);
        if (d.missing[0]) {
            fprintf(stderr, "[gfx] Vulkan: %s: %s\n", d.props.properties.deviceName, d.missing);
            if (!found) snprintf(why, why_size, "%s: %s", d.props.properties.deviceName, d.missing);
        } else if (!found || rank(&d) > rank(best)) {
            *best = d;
            found = 1;
        }
    }
    free(pds);
    return found;
}

/* The logical device, with the required features and the optional ones the device has on. */
static int make_device(const DeviceInfo *d, char *why, size_t why_size) {
    const char *ext[10];
    uint32_t n = 0;
    ext[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    ext[n++] = d->robustness;
    if (d->has.eds3) ext[n++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
    if (d->has.gpl) { ext[n++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME; ext[n++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME; }
    if (d->has.unified_layouts) ext[n++] = VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME;
    if (d->has.custom_border) ext[n++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    if (d->has.vertex_input) ext[n++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;

    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    eds3.extendedDynamicState3ColorBlendEnable = eds3.extendedDynamicState3ColorBlendEquation = eds3.extendedDynamicState3ColorWriteMask = VK_TRUE;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    gpl.graphicsPipelineLibrary = VK_TRUE;
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unified = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};
    unified.unifiedImageLayouts = VK_TRUE;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    border.customBorderColors = border.customBorderColorWithoutFormat = VK_TRUE;
    VkPhysicalDeviceRobustness2FeaturesEXT robust = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    robust.robustBufferAccess2 = robust.nullDescriptor = VK_TRUE;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vin = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    vin.vertexInputDynamicState = VK_TRUE;
    void *chain = &robust;
#define CHAIN(have, s) if (have) { (s).pNext = chain; chain = &(s); }
    CHAIN(d->has.eds3, eds3) CHAIN(d->has.gpl, gpl) CHAIN(d->has.unified_layouts, unified) CHAIN(d->has.custom_border, border) CHAIN(d->has.vertex_input, vin)
#undef CHAIN
    VkPhysicalDeviceVulkan13Features v13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, chain};
    v13.dynamicRendering = v13.synchronization2 = v13.maintenance4 = VK_TRUE;
    v13.shaderDemoteToHelperInvocation = VK_TRUE;   /* (Maxwell's KIL: required in 1.3) */
    VkPhysicalDeviceVulkan12Features v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
    v12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan11Features v11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12};
    VkPhysicalDeviceVulkan11Features have11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 have_11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &have11};
    vkGetPhysicalDeviceFeatures2(d->pd, &have_11);
    v11.shaderDrawParameters = have11.shaderDrawParameters;   /* (vkd3d-shader's SV_VertexID in the built-in passes) */
    VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
    f.features.robustBufferAccess = VK_TRUE;   /* (robustBufferAccess2 asks for it) */
    /* what the game's drawing uses, where the device has it (Direct3D 11 has each) */
    VkPhysicalDeviceFeatures2 have = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(d->pd, &have);
    f.features.samplerAnisotropy = have.features.samplerAnisotropy;
    f.features.independentBlend = have.features.independentBlend;
    f.features.dualSrcBlend = have.features.dualSrcBlend;
    f.features.imageCubeArray = have.features.imageCubeArray;
    f.features.shaderImageGatherExtended = have.features.shaderImageGatherExtended;
    f.features.fillModeNonSolid = have.features.fillModeNonSolid;
    f.features.vertexPipelineStoresAndAtomics = have.features.vertexPipelineStoresAndAtomics;
    f.features.fragmentStoresAndAtomics = have.features.fragmentStoresAndAtomics;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, d->queue, 1, &priority};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f, 0, 1, &qi, 0, NULL, n, ext, NULL};
    VkResult r = vkCreateDevice(d->pd, &ci, NULL, &vk.dev);
    if (r != VK_SUCCESS) { snprintf(why, why_size, "cannot create the Vulkan device (%s)", vk_result(r)); return 0; }
#define VK_LOAD_DEVICE(name) name = (PFN_##name)vkGetDeviceProcAddr(vk.dev, #name);
    VK_DEVICE_FNS(VK_LOAD_DEVICE)
    vk.pd = d->pd;
    vk.family = d->queue;
    vkGetPhysicalDeviceMemoryProperties(vk.pd, &vk.memory);
    vkGetDeviceQueue(vk.dev, vk.family, 0, &vk.queue);
    memcpy(&vk.has, &d->has, sizeof vk.has);
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti, 0};
    vk_check(vkCreateSemaphore(vk.dev, &si, NULL, &vk.timeline), "the timeline semaphore");
    return 1;
}

/* ---- what the renderer's files share ----------------------------------------------------- */
pthread_mutex_t vk_lock;
volatile DWORD vk_lock_thread, vk_last_enter_thread;
static u32 enter_depth;

void vk_enter(void) {
    pthread_mutex_lock(&vk_lock);
    if (!enter_depth++) vk_lock_thread = GetCurrentThreadId();
    vk_last_enter_thread = GetCurrentThreadId();
}

void vk_leave(void) {
    if (!--enter_depth) vk_lock_thread = 0;
    pthread_mutex_unlock(&vk_lock);
}

const char *vk_result(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "success";
    case VK_NOT_READY: return "not ready";
    case VK_TIMEOUT: return "timeout";
    case VK_SUBOPTIMAL_KHR: return "suboptimal";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "out of device memory";
    case VK_ERROR_INITIALIZATION_FAILED: return "initialization failed";
    case VK_ERROR_DEVICE_LOST: return "device lost";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "extension not present";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "feature not present";
    case VK_ERROR_SURFACE_LOST_KHR: return "surface lost";
    case VK_ERROR_OUT_OF_DATE_KHR: return "out of date";
    default: return "error";
    }
}

void vk_check(VkResult r, const char *what) {
    if (r >= 0) return;
    fprintf(stderr, "[gfx] Vulkan: %s: %s (%d)\n", what, vk_result(r), r);
    rt_fatal("the Vulkan renderer failed: %s: %s", what, vk_result(r));
}

u64 vk_stat_allocs, vk_stat_alloc_bytes, vk_stat_alloc_ns;   /* (SSBU_LOG_STATS) */
VkDeviceMemory vk_alloc(VkMemoryRequirements req, VkMemoryPropertyFlags want) {
    vk_stat_allocs++;
    vk_stat_alloc_bytes += req.size;
    for (int pass = 0; pass < 2; pass++)   /* (what is asked for; then without DEVICE_LOCAL, for a host-visible one) */
        for (uint32_t i = 0; i < vk.memory.memoryTypeCount; i++) {
            VkMemoryPropertyFlags need = pass ? want & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : want;
            if (!(req.memoryTypeBits & 1u << i) || (vk.memory.memoryTypes[i].propertyFlags & need) != need) continue;
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, i};
            VkDeviceMemory m;
            u64 t0 = rt_now_ns();
            VkResult r = vkAllocateMemory(vk.dev, &ai, NULL, &m);
            vk_stat_alloc_ns += rt_now_ns() - t0;
            if (r == VK_SUCCESS) return m;
        }
    vk_check(VK_ERROR_OUT_OF_DEVICE_MEMORY, "memory");
    return VK_NULL_HANDLE;
}

VkBuffer vk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkDeviceMemory *memory, void **mapped) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b;
    vk_check(vkCreateBuffer(vk.dev, &bi, NULL, &b), "a buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.dev, b, &req);
    *memory = vk_alloc(req, want);
    vk_check(vkBindBufferMemory(vk.dev, b, *memory, 0), "a buffer's memory");
    if (mapped) {
        *mapped = NULL;
        if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) vk_check(vkMapMemory(vk.dev, *memory, 0, VK_WHOLE_SIZE, 0, mapped), "mapping a buffer");
    }
    return b;
}

static VkCommandPool once_pool;

VkCommandBuffer vk_once_begin(void) {
    if (!once_pool) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, vk.family};
        vk_check(vkCreateCommandPool(vk.dev, &pi, NULL, &once_pool), "a command pool");
    }
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, once_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vk_check(vkAllocateCommandBuffers(vk.dev, &ai, &cb), "a command buffer");
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

void vk_once_end(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    u64 value = ++vk.submitted;
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb, 0};
    VkSemaphoreSubmitInfo done = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, vk.timeline, value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &cbi, 1, &done};
    vk_check(vkQueueSubmit2(vk.queue, 1, &si, VK_NULL_HANDLE), "a submission");
    vk_wait(value);
    vkResetCommandPool(vk.dev, once_pool, 0);
}

u64 vk_stat_wait_ns;   /* (SSBU_LOG_STATS: time waiting for the GPU) */
void vk_wait(u64 value) {
    u64 done = 0;
    vkGetSemaphoreCounterValue(vk.dev, vk.timeline, &done);
    if (done >= value) return;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &vk.timeline, &value};
    u64 t0 = rt_now_ns();
    vk_check(vkWaitSemaphores(vk.dev, &wi, UINT64_MAX), "waiting for the GPU");
    vk_stat_wait_ns += rt_now_ns() - t0;
}

void vk_image_barrier(VkCommandBuffer cb, VkImage image, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                      VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, src_stage, src_access, dst_stage, dst_access, from, to,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
}

/* ---- the gfx_* calls --------------------------------------------------------------------- */
/* gpu_vk_draw.c */
void vk_gfx_clear(const u32 *regs, u32 arg);
void vk_gfx_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed);
void vk_gfx_blit2d(const u32 *regs);
void vk_gfx_stream_wrote(u64 va, u64 bytes);
void vk_gfx_stream_end(void);
int vk_gfx_draw_logging(void);

void vk_gfx_dispatch(const u32 *regs, const u32 *qmd);
int vk_gfx_copy_resident(u64 dst_va, u64 src_va, u64 bytes);
void vk_gfx_resident_flush(u64 va, u64 bytes);
void vk_gfx_resident_written(u64 va, u64 bytes);
static void *vk_gfx_window_handle(void) { return rt_window(); }
static void *vk_gfx_display_output(void) { return NULL; }
static int vk_gfx_window_focused(void) { void *w = rt_window(); return w && GetForegroundWindow() == (HWND)w; }
static DWORD vk_gfx_lock_owner(void) { return vk_lock_thread; }
static DWORD vk_gfx_last_enter_thread(void) { return vk_last_enter_thread; }
void vk_draw_report_compiles(void (*line)(const char *text));   /* gpu_vk_draw.c */
static void vk_gfx_report_compiles(void (*line)(const char *text)) { vk_draw_report_compiles(line); }

volatile LONG vk_hotkeys;
static void vk_gfx_hotkey(int n) { InterlockedOr(&vk_hotkeys, 1 << n); }

/* gpu_vk_frame.c */
void vk_gfx_present(u64 guest, u32 x, u32 y, u32 w, u32 h);
int vk_gfx_progress(const char *title, unsigned done, unsigned total, const char *status);
int vk_gfx_menu(const char *title, int n, const char *const labels[], const char *const values[], const int shown[], int selected, int button,
                const char *footer, float *rects, float *value_x);

static const GfxBackend table = {
    "Vulkan",
    vk_gfx_clear, vk_gfx_draw, vk_gfx_present, vk_gfx_progress, vk_gfx_menu, vk_gfx_blit2d, vk_gfx_dispatch, vk_gfx_copy_resident,
    vk_gfx_resident_flush, vk_gfx_resident_written, vk_gfx_stream_wrote, vk_gfx_stream_end, vk_gfx_draw_logging, vk_gfx_window_handle,
    vk_gfx_display_output, vk_gfx_hotkey, vk_gfx_window_focused, vk_gfx_lock_owner, vk_gfx_last_enter_thread, vk_gfx_report_compiles,
};

const GfxBackend *vk_backend_open(char *why, size_t why_size) {
    DeviceInfo d;
    if (!make_instance(why, why_size) || !choose_device(&d, why, why_size)) {
        close_instance();
        return NULL;
    }
    uint32_t v = d.props.properties.apiVersion;
    fprintf(stderr, "[gfx] Vulkan %u.%u on %s (%s %s)\n", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), d.props.properties.deviceName,
            d.driver.driverName, d.driver.driverInfo);
    if (d.props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        fprintf(stderr, "[gfx] note: this is a software renderer (no Vulkan GPU driver found): expect a low frame rate\n");
    if (d.optional_missing[0]) fprintf(stderr, "[gfx] Vulkan: without %s (another way is used for each)\n", d.optional_missing);
    if (!make_device(&d, why, why_size)) {
        close_instance();
        return NULL;
    }
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&vk_lock, &ma);
    fprintf(stderr, "[gfx] Vulkan ready (experimental)\n");
    return &table;
}
