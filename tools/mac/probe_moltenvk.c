// probe_moltenvk.c: opens a MoltenVK dylib as the game's renderer does (dlopen, vkGetInstanceProcAddr), makes a Vulkan instance and lists the
// GPUs. No window, no surface. Built and run by tools/mac/check_mac_deps.sh when it is given a MAC_DEPS folder:
//   clang -I<deps>/include probe_moltenvk.c -o probe && probe <path of libMoltenVK.dylib>
// Exit 0 with a GPU listed; 1 otherwise.
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <libMoltenVK.dylib>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) { fprintf(stderr, "no vkGetInstanceProcAddr\n"); return 1; }
    PFN_vkEnumerateInstanceVersion ver = (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
    uint32_t api = VK_API_VERSION_1_0;
    if (ver) ver(&api);
    printf("instance version %u.%u.%u\n", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "probe";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = create(&ci, NULL, &inst);
    if (r != VK_SUCCESS) { fprintf(stderr, "vkCreateInstance: %d\n", (int)r); return 1; }
    PFN_vkEnumeratePhysicalDevices enumd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    uint32_t n = 0;
    enumd(inst, &n, NULL);
    VkPhysicalDevice dev[8];
    if (n > 8) n = 8;
    enumd(inst, &n, dev);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        props(dev[i], &p);
        printf("GPU %u: %s, Vulkan %u.%u\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
    }
    destroy(inst, NULL);
    return n > 0 ? 0 : 1;
}
