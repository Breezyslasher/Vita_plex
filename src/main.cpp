/**
 * VitaPlex - Plex Client for PlayStation Vita
 * 
 * Main entry point
 * Based on switchfin architecture (https://github.com/dragonflylee/switchfin)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Vita SDK headers
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>
#include <psp2/apputil.h>
#include <psp2/sysmodule.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/gxm.h>
#include <psp2/common_dialog.h>

// Networking
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/libssl.h>

// Graphics
#include <vita2d.h>

// Application headers
#include "app.h"
#include "utils/http_client.h"

// Increase heap size for video decoding and networking
// This is critical for preventing crashes
int _newlib_heap_size_user = 192 * 1024 * 1024;  // 192 MB heap

// Thread stack size for main thread (larger for video/network operations)
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024;  // 2 MB stack

// Memory allocations for networking
#define NET_MEMORY_SIZE (4 * 1024 * 1024)  // Increased to 4MB
#define SSL_MEMORY_SIZE (512 * 1024)       // Increased to 512KB
#define HTTP_MEMORY_SIZE (2 * 1024 * 1024) // 2MB for HTTP

// Use aligned memory for Vita
static char __attribute__((aligned(64))) netMemory[NET_MEMORY_SIZE];

/**
 * Load the shader compiler module (required for GXM graphics)
 * This must be extracted from a PSM game or downloaded separately
 */
static bool loadShaderCompiler() {
    // Try to load libshacccg.suprx - required for GXM shader compilation
    // This file should be at ur0:data/libshacccg.suprx or vs0:sys/external/libshacccg.suprx
    SceUID mod;
    
    // Try ur0 first (user installed)
    mod = sceKernelLoadStartModule("ur0:data/libshacccg.suprx", 0, NULL, 0, NULL, NULL);
    if (mod >= 0) {
        sceClibPrintf("Loaded libshacccg.suprx from ur0:data\n");
        return true;
    }
    
    // Try vs0 (system - only works on some firmwares/setups)
    mod = sceKernelLoadStartModule("vs0:sys/external/libshacccg.suprx", 0, NULL, 0, NULL, NULL);
    if (mod >= 0) {
        sceClibPrintf("Loaded libshacccg.suprx from vs0:sys/external\n");
        return true;
    }
    
    // Not fatal - vita2d has fallback shaders
    sceClibPrintf("Warning: Could not load libshacccg.suprx - using fallback shaders\n");
    sceClibPrintf("For better performance, install libshacccg.suprx to ur0:data/\n");
    return true;  // Continue anyway
}

/**
 * Initialize Vita system modules
 */
static bool initSystem() {
    int ret;
    
    sceClibPrintf("Initializing system modules...\n");
    
    // Initialize app utilities first
    SceAppUtilInitParam initParam;
    SceAppUtilBootParam bootParam;
    memset(&initParam, 0, sizeof(initParam));
    memset(&bootParam, 0, sizeof(bootParam));
    ret = sceAppUtilInit(&initParam, &bootParam);
    if (ret < 0) {
        sceClibPrintf("Warning: sceAppUtilInit failed: 0x%08X\n", ret);
    }
    
    // Set CPU clock to max for better performance
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    
    // Load shader compiler for GXM
    loadShaderCompiler();
    
    // Load required system modules via sceSysmoduleLoadModule
    // Order matters - load dependencies first
    
    // Core modules
    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (ret < 0 && ret != 0x80800003) {
        sceClibPrintf("Failed to load NET module: 0x%08X\n", ret);
    }
    
    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_SSL);
    if (ret < 0 && ret != 0x80800003) {
        sceClibPrintf("Failed to load SSL module: 0x%08X\n", ret);
    }
    
    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP);
    if (ret < 0 && ret != 0x80800003) {
        sceClibPrintf("Failed to load HTTP module: 0x%08X\n", ret);
    }
    
    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
    if (ret < 0 && ret != 0x80800003) {
        sceClibPrintf("Failed to load HTTPS module: 0x%08X\n", ret);
    }
    
    // Media modules - load before video playback
    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
    if (ret < 0 && ret != 0x80800003) {
        sceClibPrintf("Warning: Failed to load AVPLAYER module: 0x%08X\n", ret);
    }
    
    // Input and UI modules
    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    if (ret < 0 && ret != 0x80800003) {
        sceClibPrintf("Warning: Failed to load IME module: 0x%08X\n", ret);
    }
    
    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);
    if (ret < 0 && ret != 0x80800003) {
        sceClibPrintf("Warning: Failed to load PGF font module: 0x%08X\n", ret);
    }
    
    sceClibPrintf("System modules loaded\n");
    return true;
}

/**
 * Initialize networking subsystem
 */
static bool initNetwork() {
    int ret;
    
    sceClibPrintf("Initializing networking...\n");
    
    // Initialize network first (required before NetCtl)
    SceNetInitParam netInitParam;
    netInitParam.memory = netMemory;
    netInitParam.size = NET_MEMORY_SIZE;
    netInitParam.flags = 0;
    
    ret = sceNetInit(&netInitParam);
    if (ret < 0) {
        // 0x80410201 = SCE_NET_ERROR_EEXIST (already initialized) - this is OK
        if ((unsigned int)ret != 0x80410201) {
            sceClibPrintf("sceNetInit failed: 0x%08X\n", ret);
            return false;
        }
        sceClibPrintf("Network already initialized\n");
    }
    
    // Initialize network control
    ret = sceNetCtlInit();
    if (ret < 0) {
        // 0x80412102 = already initialized - this is OK
        if ((unsigned int)ret != 0x80412102) {
            sceClibPrintf("sceNetCtlInit failed: 0x%08X\n", ret);
            return false;
        }
        sceClibPrintf("NetCtl already initialized\n");
    }
    
    // Initialize SSL with larger memory pool
    ret = sceSslInit(SSL_MEMORY_SIZE);
    if (ret < 0) {
        // 0x80435001 = already initialized - this is OK
        if ((unsigned int)ret != 0x80435001) {
            sceClibPrintf("sceSslInit failed: 0x%08X\n", ret);
            return false;
        }
        sceClibPrintf("SSL already initialized\n");
    }
    
    // Initialize HTTP with larger memory pool
    ret = sceHttpInit(HTTP_MEMORY_SIZE);
    if (ret < 0) {
        // 0x80431002 = already initialized - this is OK  
        if ((unsigned int)ret != 0x80431002) {
            sceClibPrintf("sceHttpInit failed: 0x%08X\n", ret);
            return false;
        }
        sceClibPrintf("HTTP already initialized\n");
    }
    
    // Initialize curl
    vitaplex::HttpClient::globalInit();
    
    sceClibPrintf("Networking initialized successfully\n");
    return true;
}

/**
 * Initialize graphics (vita2d)
 */
static bool initGraphics() {
    sceClibPrintf("Initializing graphics...\n");
    
    // Initialize vita2d - this also initializes GXM
    vita2d_init();
    vita2d_set_clear_color(RGBA8(30, 30, 30, 255));
    
    // Wait for initialization to complete
    vita2d_wait_rendering_done();
    
    // Initialize common dialog AFTER vita2d - required for IME dialogs
    sceClibPrintf("Initializing common dialog...\n");
    SceCommonDialogConfigParam dialogConfig;
    sceCommonDialogConfigParamInit(&dialogConfig);
    // Use system defaults for language and button assignment
    // dialogConfig.language and dialogConfig.enterButtonAssign are set by init
    int ret = sceCommonDialogSetConfigParam(&dialogConfig);
    if (ret < 0) {
        sceClibPrintf("Warning: sceCommonDialogSetConfigParam failed: 0x%08X\n", ret);
    } else {
        sceClibPrintf("Common dialog initialized successfully\n");
    }
    
    sceClibPrintf("Graphics initialized successfully\n");
    return true;
}

/**
 * Cleanup networking
 */
static void cleanupNetwork() {
    sceClibPrintf("Cleaning up networking...\n");
    vitaplex::HttpClient::globalCleanup();
    sceHttpTerm();
    sceSslTerm();
    sceNetCtlTerm();
    sceNetTerm();
}

/**
 * Cleanup graphics
 */
static void cleanupGraphics() {
    sceClibPrintf("Cleaning up graphics...\n");
    // Wait for all rendering to complete before shutdown
    vita2d_wait_rendering_done();
    vita2d_fini();
}

/**
 * Main entry point
 */
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    sceClibPrintf("\n\n=================================\n");
    sceClibPrintf("VitaPlex starting...\n");
    sceClibPrintf("=================================\n\n");
    
    // Initialize system modules first
    if (!initSystem()) {
        sceClibPrintf("FATAL: Failed to initialize system modules\n");
        sceKernelDelayThread(3000000);  // Wait 3 seconds so user can see error
        sceKernelExitProcess(0);
        return 1;
    }
    
    // Initialize graphics before networking
    // vita2d init must happen before any GXM operations
    if (!initGraphics()) {
        sceClibPrintf("FATAL: Failed to initialize graphics\n");
        sceKernelDelayThread(3000000);
        sceKernelExitProcess(0);
        return 1;
    }
    
    // Initialize networking
    if (!initNetwork()) {
        sceClibPrintf("FATAL: Failed to initialize networking\n");
        cleanupGraphics();
        sceKernelDelayThread(3000000);
        sceKernelExitProcess(0);
        return 1;
    }
    
    sceClibPrintf("All systems initialized successfully\n\n");
    
    // Run application
    vitaplex::App& app = vitaplex::App::getInstance();
    if (app.init()) {
        app.run();
    } else {
        sceClibPrintf("FATAL: App initialization failed\n");
    }
    app.shutdown();
    
    // Cleanup in reverse order of initialization
    cleanupNetwork();
    cleanupGraphics();
    
    sceClibPrintf("\n=================================\n");
    sceClibPrintf("VitaPlex exiting cleanly\n");
    sceClibPrintf("=================================\n\n");
    
    sceKernelExitProcess(0);
    return 0;
}
