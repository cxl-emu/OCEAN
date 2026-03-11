/*
 * CXL Type 2 Device - hetGPU Backend Integration
 * Provides CUDA compatibility layer for CXL Type 2 GPU accelerators
 *
 * This file implements stub functions for the hetGPU backend.
 * When the actual hetGPU library is available, it will be loaded dynamically.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/thread.h"
#include "hw/cxl/cxl_hetgpu.h"
#include <dlfcn.h>

/* Default device properties for simulation mode */
static const HetGPUDeviceProps default_props = {
    .name = "Virtual GPU (TMatmul)",
    .total_memory = 4ULL * 1024 * 1024 * 1024,  /* 4GB */
    .compute_capability_major = 8,
    .compute_capability_minor = 0,
    .max_threads_per_block = 1024,
    .max_block_dim = {1024, 1024, 64},
    .max_grid_dim = {65535, 65535, 65535},
    .warp_size = 32,
    .multiprocessor_count = 80,
    .clock_rate_khz = 1500000,
    .memory_clock_rate_khz = 5000000,
    .memory_bus_width = 256,
    .l2_cache_size = 6 * 1024 * 1024,  /* 6MB */
    .supports_managed_memory = true,
    .supports_coherent_memory = true,
    .backend_type = HETGPU_BACKEND_SIMULATION,
};

/* Function pointer types for dynamic loading */
typedef int (*cuInit_fn)(unsigned int);
typedef int (*cuDeviceGetCount_fn)(int *);
typedef int (*cuDeviceGet_fn)(int *, int);
typedef int (*cuDeviceGetName_fn)(char *, int, int);
typedef int (*cuDeviceTotalMem_fn)(size_t *, int);
typedef int (*cuDeviceGetAttribute_fn)(int *, int, int);
typedef int (*cuCtxCreate_fn)(void **, unsigned int, int);
typedef int (*cuCtxDestroy_fn)(void *);
typedef int (*cuCtxSynchronize_fn)(void);
typedef int (*cuMemAlloc_fn)(uint64_t *, size_t);
typedef int (*cuMemFree_fn)(uint64_t);
typedef int (*cuMemcpyHtoD_fn)(uint64_t, const void *, size_t);
typedef int (*cuMemcpyDtoH_fn)(void *, uint64_t, size_t);
typedef int (*cuMemcpyDtoD_fn)(uint64_t, uint64_t, size_t);
typedef int (*cuModuleLoadData_fn)(void **, const void *);
typedef int (*cuModuleGetFunction_fn)(void **, void *, const char *);
typedef int (*cuLaunchKernel_fn)(void *, unsigned int, unsigned int, unsigned int,
                                  unsigned int, unsigned int, unsigned int,
                                  unsigned int, void *, void **, void **);
typedef int (*cuCtxPushCurrent_fn)(void *);
typedef int (*cuCtxPopCurrent_fn)(void **);
typedef int (*cuCtxSetCurrent_fn)(void *);
typedef int (*cuCtxGetCurrent_fn)(void **);
typedef int (*cuGetErrorString_fn)(int, const char **);
typedef int (*cuGetErrorName_fn)(int, const char **);

/* CUDA device attribute constants */
#define CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK 1
#define CU_DEVICE_ATTRIBUTE_WARP_SIZE 10
#define CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT 16
#define CU_DEVICE_ATTRIBUTE_CLOCK_RATE 13
#define CU_DEVICE_ATTRIBUTE_MEMORY_CLOCK_RATE 36
#define CU_DEVICE_ATTRIBUTE_GLOBAL_MEMORY_BUS_WIDTH 37
#define CU_DEVICE_ATTRIBUTE_L2_CACHE_SIZE 38
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR 75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR 76

/* Loaded function pointers */
static struct {
    cuInit_fn cuInit;
    cuDeviceGetCount_fn cuDeviceGetCount;
    cuDeviceGet_fn cuDeviceGet;
    cuDeviceGetName_fn cuDeviceGetName;
    cuDeviceTotalMem_fn cuDeviceTotalMem;
    cuDeviceGetAttribute_fn cuDeviceGetAttribute;
    cuCtxCreate_fn cuCtxCreate;
    cuCtxDestroy_fn cuCtxDestroy;
    cuCtxSynchronize_fn cuCtxSynchronize;
    cuMemAlloc_fn cuMemAlloc;
    cuMemFree_fn cuMemFree;
    cuMemcpyHtoD_fn cuMemcpyHtoD;
    cuMemcpyDtoH_fn cuMemcpyDtoH;
    cuMemcpyDtoD_fn cuMemcpyDtoD;
    cuModuleLoadData_fn cuModuleLoadData;
    cuModuleGetFunction_fn cuModuleGetFunction;
    cuLaunchKernel_fn cuLaunchKernel;
    cuCtxPushCurrent_fn cuCtxPushCurrent;
    cuCtxPopCurrent_fn cuCtxPopCurrent;
    cuCtxSetCurrent_fn cuCtxSetCurrent;
    cuCtxGetCurrent_fn cuCtxGetCurrent;
    cuGetErrorString_fn cuGetErrorString;
    cuGetErrorName_fn cuGetErrorName;
} g_cuda_funcs = {0};

/* Global mutex for multi-device context switching safety */
static QemuMutex g_cuda_mutex;
static bool g_cuda_mutex_initialized = false;
static bool g_cuda_lib_initialized = false;
static void *g_cuda_lib_handle = NULL;

static void ensure_cuda_mutex_init(void)
{
    if (!g_cuda_mutex_initialized) {
        qemu_mutex_init(&g_cuda_mutex);
        g_cuda_mutex_initialized = true;
    }
}

HetGPUError hetgpu_init(HetGPUState *state, HetGPUBackendType backend,
                        int device_index, const char *hetgpu_lib_path)
{
    if (!state) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    ensure_cuda_mutex_init();

    memset(state, 0, sizeof(*state));
    state->backend = backend;
    state->device_index = device_index;

    /* Try to load hetGPU library (only once for all devices) */
    const char *lib_path = hetgpu_lib_path;
    if (!lib_path || lib_path[0] == '\0') {
        lib_path = "/usr/lib/x86_64-linux-gnu/libcuda.so";
    }

    fprintf(stderr, "CXL hetGPU: ========================================\n");
    fprintf(stderr, "CXL hetGPU: Initializing GPU backend for device %d\n", device_index);
    fprintf(stderr, "CXL hetGPU: Library path: %s\n", lib_path);
    fprintf(stderr, "CXL hetGPU: Library already loaded: %s\n",
            g_cuda_lib_initialized ? "yes" : "no");
    fprintf(stderr, "CXL hetGPU: ========================================\n");
    fflush(stderr);

    qemu_mutex_lock(&g_cuda_mutex);

    /* Only load library and call cuInit once across all devices */
    if (!g_cuda_lib_initialized) {
        g_cuda_lib_handle = dlopen(lib_path, RTLD_NOW | RTLD_GLOBAL);
        if (!g_cuda_lib_handle) {
            fprintf(stderr, "CXL hetGPU: FAILED to load library: %s\n", dlerror());
            fprintf(stderr, "CXL hetGPU: Trying alternate path /usr/lib64/libcuda.so\n");
            fflush(stderr);
            g_cuda_lib_handle = dlopen("/usr/lib64/libcuda.so", RTLD_NOW | RTLD_GLOBAL);
        }
        if (!g_cuda_lib_handle) {
            fprintf(stderr, "CXL hetGPU: FAILED to load library: %s\n", dlerror());
            fprintf(stderr, "CXL hetGPU: Trying alternate path libcuda.so.1\n");
            fflush(stderr);
            g_cuda_lib_handle = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
        }

        if (g_cuda_lib_handle) {
            fprintf(stderr, "CXL hetGPU: Successfully loaded CUDA library\n");
            fflush(stderr);

            /* Load function pointers */
            g_cuda_funcs.cuInit = dlsym(g_cuda_lib_handle, "cuInit");
            g_cuda_funcs.cuDeviceGetCount = dlsym(g_cuda_lib_handle, "cuDeviceGetCount");
            g_cuda_funcs.cuDeviceGet = dlsym(g_cuda_lib_handle, "cuDeviceGet");
            g_cuda_funcs.cuDeviceGetName = dlsym(g_cuda_lib_handle, "cuDeviceGetName");
            g_cuda_funcs.cuDeviceTotalMem = dlsym(g_cuda_lib_handle, "cuDeviceTotalMem_v2");
            g_cuda_funcs.cuDeviceGetAttribute = dlsym(g_cuda_lib_handle, "cuDeviceGetAttribute");
            g_cuda_funcs.cuCtxCreate = dlsym(g_cuda_lib_handle, "cuCtxCreate_v2");
            g_cuda_funcs.cuCtxDestroy = dlsym(g_cuda_lib_handle, "cuCtxDestroy_v2");
            g_cuda_funcs.cuCtxSynchronize = dlsym(g_cuda_lib_handle, "cuCtxSynchronize");
            g_cuda_funcs.cuMemAlloc = dlsym(g_cuda_lib_handle, "cuMemAlloc_v2");
            g_cuda_funcs.cuMemFree = dlsym(g_cuda_lib_handle, "cuMemFree_v2");
            g_cuda_funcs.cuMemcpyHtoD = dlsym(g_cuda_lib_handle, "cuMemcpyHtoD_v2");
            g_cuda_funcs.cuMemcpyDtoH = dlsym(g_cuda_lib_handle, "cuMemcpyDtoH_v2");
            g_cuda_funcs.cuMemcpyDtoD = dlsym(g_cuda_lib_handle, "cuMemcpyDtoD_v2");
            g_cuda_funcs.cuModuleLoadData = dlsym(g_cuda_lib_handle, "cuModuleLoadData");
            g_cuda_funcs.cuModuleGetFunction = dlsym(g_cuda_lib_handle, "cuModuleGetFunction");
            g_cuda_funcs.cuLaunchKernel = dlsym(g_cuda_lib_handle, "cuLaunchKernel");
            g_cuda_funcs.cuCtxPushCurrent = dlsym(g_cuda_lib_handle, "cuCtxPushCurrent_v2");
            g_cuda_funcs.cuCtxPopCurrent = dlsym(g_cuda_lib_handle, "cuCtxPopCurrent_v2");
            g_cuda_funcs.cuCtxSetCurrent = dlsym(g_cuda_lib_handle, "cuCtxSetCurrent");
            g_cuda_funcs.cuCtxGetCurrent = dlsym(g_cuda_lib_handle, "cuCtxGetCurrent");
            g_cuda_funcs.cuGetErrorString = dlsym(g_cuda_lib_handle, "cuGetErrorString");
            g_cuda_funcs.cuGetErrorName = dlsym(g_cuda_lib_handle, "cuGetErrorName");

            fprintf(stderr, "CXL hetGPU: Loaded CUDA functions - cuInit=%p, cuCtxSetCurrent=%p\n",
                     g_cuda_funcs.cuInit, g_cuda_funcs.cuCtxSetCurrent);
            fflush(stderr);

            if (g_cuda_funcs.cuInit) {
                int err = g_cuda_funcs.cuInit(0);
                fprintf(stderr, "CXL hetGPU: cuInit returned err=%d\n", err);
                fflush(stderr);
                if (err != 0) {
                    qemu_log("CXL hetGPU: cuInit failed with error %d\n", err);
                    qemu_mutex_unlock(&g_cuda_mutex);
                    goto simulation_fallback;
                }
            }
            g_cuda_lib_initialized = true;
        }
    }

    /* Store library handle reference (shared, don't dlclose individually) */
    state->hetgpu_lib = g_cuda_lib_handle;

    qemu_mutex_unlock(&g_cuda_mutex);

    if (state->hetgpu_lib && g_cuda_lib_initialized) {
        /* Library loaded and cuInit succeeded — create per-device context */
        int err = 0;
        int cuda_dev = 0;
        size_t total_mem = 0;
        int attr_val = 0;
        void *ctx = NULL;

        /* Get device handle — with MIG, different device_index = different MIG instance */
        if (g_cuda_funcs.cuDeviceGet) {
            err = g_cuda_funcs.cuDeviceGet(&cuda_dev, device_index);
            if (err != 0) {
                qemu_log("CXL hetGPU: cuDeviceGet(%d) failed: %d\n", device_index, err);
                qemu_log("CXL hetGPU: If using MIG, ensure MIG instances are configured\n");
                goto simulation_fallback;
            }
            qemu_log("CXL hetGPU: Got CUDA device %d for device_index %d\n",
                     cuda_dev, device_index);
        }

        /* Create per-device context */
        if (g_cuda_funcs.cuCtxCreate) {
            qemu_log("CXL hetGPU: Calling cuCtxCreate_v2 for device %d\n", cuda_dev);
            err = g_cuda_funcs.cuCtxCreate(&ctx, 0, cuda_dev);

            if (err != 0) {
                const char *err_name = "UNKNOWN";
                const char *err_str = "Unknown error";
                if (g_cuda_funcs.cuGetErrorName) {
                    g_cuda_funcs.cuGetErrorName(err, &err_name);
                }
                if (g_cuda_funcs.cuGetErrorString) {
                    g_cuda_funcs.cuGetErrorString(err, &err_str);
                }
                qemu_log("CXL hetGPU: cuCtxCreate FAILED: %s (%d) - %s\n",
                         err_name, err, err_str);
                goto simulation_fallback;
            } else if (ctx == NULL) {
                /* hetGPU returns NULL context - use hetGPU managed mode */
                qemu_log("CXL hetGPU: cuCtxCreate returned NULL context\n");
                qemu_log("CXL hetGPU: Using hetGPU managed mode (backend=%d)\n", backend);

                state->initialized = true;
                state->backend = (backend == HETGPU_BACKEND_AUTO) ?
                                 HETGPU_BACKEND_NVIDIA : backend;
                state->context = NULL;
                state->cuda_device = cuda_dev;

                state->props = default_props;
                if (g_cuda_funcs.cuDeviceGetName) {
                    g_cuda_funcs.cuDeviceGetName(state->props.name,
                                                 sizeof(state->props.name), cuda_dev);
                }
                if (g_cuda_funcs.cuDeviceTotalMem) {
                    size_t mem = 0;
                    if (g_cuda_funcs.cuDeviceTotalMem(&mem, cuda_dev) == 0) {
                        state->props.total_memory = mem;
                    }
                }
                state->props.backend_type = state->backend;

                qemu_log("CXL hetGPU: hetGPU mode initialized: %s, %lu MB, backend=%d\n",
                         state->props.name,
                         (unsigned long)(state->props.total_memory / (1024*1024)),
                         state->backend);
                return HETGPU_SUCCESS;
            }

            qemu_log("CXL hetGPU: Successfully created CUDA context %p for device %d\n",
                     ctx, device_index);

            /* Pop the context cuCtxCreate auto-pushed, we'll use cuCtxSetCurrent */
            if (g_cuda_funcs.cuCtxPopCurrent) {
                void *popped = NULL;
                g_cuda_funcs.cuCtxPopCurrent(&popped);
            }
        } else {
            qemu_log("CXL hetGPU: cuCtxCreate_v2 symbol not found\n");
            goto simulation_fallback;
        }

        state->initialized = true;
        state->backend = HETGPU_BACKEND_NVIDIA;
        state->cuda_device = cuda_dev;
        state->context = ctx;

        /* Query real GPU properties */
        state->props = default_props;

        if (g_cuda_funcs.cuDeviceGetName) {
            err = g_cuda_funcs.cuDeviceGetName(state->props.name,
                                         sizeof(state->props.name), cuda_dev);
        }
        if (g_cuda_funcs.cuDeviceTotalMem) {
            if (g_cuda_funcs.cuDeviceTotalMem(&total_mem, cuda_dev) == 0) {
                state->props.total_memory = total_mem;
                qemu_log("CXL hetGPU: Device %d total memory: %lu MB\n",
                         device_index, (unsigned long)(total_mem / (1024*1024)));
            }
        }
        if (g_cuda_funcs.cuDeviceGetAttribute) {
            if (g_cuda_funcs.cuDeviceGetAttribute(&attr_val,
                    CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, cuda_dev) == 0) {
                state->props.compute_capability_major = attr_val;
            }
            if (g_cuda_funcs.cuDeviceGetAttribute(&attr_val,
                    CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, cuda_dev) == 0) {
                state->props.compute_capability_minor = attr_val;
            }
            if (g_cuda_funcs.cuDeviceGetAttribute(&attr_val,
                    CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, cuda_dev) == 0) {
                state->props.multiprocessor_count = attr_val;
            }
            if (g_cuda_funcs.cuDeviceGetAttribute(&attr_val,
                    CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK, cuda_dev) == 0) {
                state->props.max_threads_per_block = attr_val;
            }
            if (g_cuda_funcs.cuDeviceGetAttribute(&attr_val,
                    CU_DEVICE_ATTRIBUTE_WARP_SIZE, cuda_dev) == 0) {
                state->props.warp_size = attr_val;
            }
        }

        state->props.backend_type = HETGPU_BACKEND_NVIDIA;

        qemu_log("CXL hetGPU: Device %d initialized: %s, %lu MB, CC %d.%d\n",
                 device_index, state->props.name,
                 (unsigned long)(state->props.total_memory / (1024*1024)),
                 state->props.compute_capability_major,
                 state->props.compute_capability_minor);
        return HETGPU_SUCCESS;
    }

    /* Fall through to error if library not loaded or cuInit not found */

simulation_fallback:
    /* Do NOT fall back to simulation - require real GPU */
    qemu_log("CXL hetGPU: ERROR - Real GPU initialization failed!\n");
    qemu_log("CXL hetGPU: To use the CXL Type 2 GPU device, ensure:\n");
    qemu_log("CXL hetGPU:   1. NVIDIA GPU is present and working (run nvidia-smi)\n");
    qemu_log("CXL hetGPU:   2. QEMU has GPU access (run as root or add user to video group)\n");
    qemu_log("CXL hetGPU:   3. libcuda.so is accessible at /usr/lib/x86_64-linux-gnu/libcuda.so\n");
    qemu_log("CXL hetGPU:   4. NVIDIA driver is loaded (lsmod | grep nvidia)\n");

    state->initialized = false;
    state->backend = HETGPU_BACKEND_SIMULATION;  /* Mark as simulation so errors are clear */
    state->context = NULL;

    return HETGPU_ERROR_NO_DEVICE;
}

/* Helper to find simulation allocation by device pointer */
static HetGPUSimAlloc *find_sim_alloc(HetGPUState *state, HetGPUDevicePtr dev_ptr)
{
    HetGPUSimAlloc *alloc = state->sim_allocs;
    while (alloc) {
        if (dev_ptr >= alloc->dev_ptr && dev_ptr < alloc->dev_ptr + alloc->size) {
            return alloc;
        }
        alloc = alloc->next;
    }
    return NULL;
}

void hetgpu_cleanup(HetGPUState *state)
{
    if (!state) {
        return;
    }

    /* Free simulation allocations */
    HetGPUSimAlloc *alloc = state->sim_allocs;
    while (alloc) {
        HetGPUSimAlloc *next = alloc->next;
        if (alloc->host_buffer) {
            g_free(alloc->host_buffer);
        }
        g_free(alloc);
        alloc = next;
    }
    state->sim_allocs = NULL;

    if (state->context && state->backend != HETGPU_BACKEND_SIMULATION &&
        g_cuda_funcs.cuCtxDestroy) {
        qemu_mutex_lock(&g_cuda_mutex);
        g_cuda_funcs.cuCtxDestroy(state->context);
        qemu_mutex_unlock(&g_cuda_mutex);
    }

    /* Don't dlclose — library handle is shared across all devices */

    qemu_log("CXL hetGPU: Stats - Kernel launches: %lu, Memory ops: %lu, Coherency ops: %lu\n",
             state->kernel_launches, state->memory_ops, state->coherency_ops);

    memset(state, 0, sizeof(*state));
}

HetGPUError hetgpu_get_device_count(int *count)
{
    fprintf(stderr, "CXL hetGPU: hetgpu_get_device_count called\n");
    fflush(stderr);

    if (!count) {
        fprintf(stderr, "CXL hetGPU: hetgpu_get_device_count - NULL count pointer!\n");
        fflush(stderr);
        return HETGPU_ERROR_INVALID_VALUE;
    }

    fprintf(stderr, "CXL hetGPU: cuDeviceGetCount func ptr = %p\n", (void*)g_cuda_funcs.cuDeviceGetCount);
    fflush(stderr);

    if (g_cuda_funcs.cuDeviceGetCount) {
        *count = -999;  /* Sentinel value to detect if function updates it */
        int err = g_cuda_funcs.cuDeviceGetCount(count);
        fprintf(stderr, "CXL hetGPU: cuDeviceGetCount returned err=%d, count=%d\n", err, *count);
        fflush(stderr);
        if (err != 0) {
            fprintf(stderr, "CXL hetGPU: cuDeviceGetCount FAILED with error %d\n", err);
            fflush(stderr);
            return HETGPU_ERROR_NO_DEVICE;
        }
        if (*count <= 0) {
            fprintf(stderr, "CXL hetGPU: WARNING - cuDeviceGetCount returned count=%d\n", *count);
            fflush(stderr);
        }
        return HETGPU_SUCCESS;
    }

    fprintf(stderr, "CXL hetGPU: No cuDeviceGetCount function, using simulation mode\n");
    fflush(stderr);
    *count = 1;  /* Simulation mode: 1 virtual device */
    return HETGPU_SUCCESS;
}

HetGPUError hetgpu_get_device_props(HetGPUState *state, HetGPUDeviceProps *props)
{
    if (!state || !props) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    *props = state->props;
    return HETGPU_SUCCESS;
}

HetGPUError hetgpu_create_context(HetGPUState *state)
{
    if (!state || !state->initialized) {
        return HETGPU_ERROR_NOT_INITIALIZED;
    }

    /* Context was already created during initialization */
    if (state->context != NULL) {
        qemu_log("CXL hetGPU: Using existing context %p\n", state->context);
        return HETGPU_SUCCESS;
    }

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        state->context = (void *)0xDEADBEEF;  /* Dummy context */
        return HETGPU_SUCCESS;
    }

    /* hetGPU managed mode: NVIDIA/AMD/Intel backend with NULL context
     * means hetGPU library manages the context internally */
    if (state->backend == HETGPU_BACKEND_NVIDIA ||
        state->backend == HETGPU_BACKEND_AMD ||
        state->backend == HETGPU_BACKEND_INTEL ||
        state->backend == HETGPU_BACKEND_TENSTORRENT) {
        qemu_log("CXL hetGPU: Using hetGPU managed context (backend=%d)\n", state->backend);
        /* Context remains NULL - hetGPU manages it internally */
        return HETGPU_SUCCESS;
    }

    if (g_cuda_funcs.cuCtxCreate) {
        void *ctx = NULL;
        int err = g_cuda_funcs.cuCtxCreate(&ctx, 0, state->cuda_device);
        if (err == 0 && ctx != NULL) {
            state->context = ctx;
            qemu_log("CXL hetGPU: Created new CUDA context %p\n", ctx);
            return HETGPU_SUCCESS;
        }
        qemu_log("CXL hetGPU: cuCtxCreate failed with error %d\n", err);
        return HETGPU_ERROR_INVALID_CONTEXT;
    }

    return HETGPU_ERROR_NOT_INITIALIZED;
}

void hetgpu_destroy_context(HetGPUState *state)
{
    if (!state || !state->context) {
        return;
    }

    if (state->backend != HETGPU_BACKEND_SIMULATION && g_cuda_funcs.cuCtxDestroy) {
        g_cuda_funcs.cuCtxDestroy(state->context);
    }

    state->context = NULL;
}

/*
 * Multi-device context management:
 * Uses cuCtxSetCurrent (replaces current) instead of cuCtxPushCurrent (grows stack).
 * Each operation must be wrapped in cuda_lock/cuda_unlock to prevent races
 * when two CXL Type 2 devices share the same physical GPU (or MIG instances).
 */
static void cuda_lock(HetGPUState *state)
{
    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        return;
    }
    qemu_mutex_lock(&g_cuda_mutex);

    if (state->context) {
        /* Use cuCtxSetCurrent — does NOT grow the context stack */
        if (g_cuda_funcs.cuCtxSetCurrent) {
            int err = g_cuda_funcs.cuCtxSetCurrent(state->context);
            if (err != 0) {
                qemu_log("CXL hetGPU: cuCtxSetCurrent(%p) failed (%d), "
                         "attempting context re-creation\n", state->context, err);
                if (g_cuda_funcs.cuCtxCreate) {
                    void *ctx = NULL;
                    err = g_cuda_funcs.cuCtxCreate(&ctx, 0, state->cuda_device);
                    if (err == 0 && ctx != NULL) {
                        /* Pop the auto-pushed context from cuCtxCreate */
                        if (g_cuda_funcs.cuCtxPopCurrent) {
                            void *popped = NULL;
                            g_cuda_funcs.cuCtxPopCurrent(&popped);
                        }
                        state->context = ctx;
                        g_cuda_funcs.cuCtxSetCurrent(ctx);
                        qemu_log("CXL hetGPU: Re-created CUDA context %p\n", ctx);
                    }
                }
            }
        } else if (g_cuda_funcs.cuCtxPushCurrent) {
            /* Fallback to push if SetCurrent not available */
            g_cuda_funcs.cuCtxPushCurrent(state->context);
        }
    } else if (!state->context && g_cuda_funcs.cuCtxCreate) {
        /* No context stored (hetGPU managed mode) - create one */
        void *ctx = NULL;
        int err = g_cuda_funcs.cuCtxCreate(&ctx, 0, state->cuda_device);
        if (err == 0 && ctx != NULL) {
            /* Pop the auto-pushed context, use SetCurrent instead */
            if (g_cuda_funcs.cuCtxPopCurrent) {
                void *popped = NULL;
                g_cuda_funcs.cuCtxPopCurrent(&popped);
            }
            state->context = ctx;
            if (g_cuda_funcs.cuCtxSetCurrent) {
                g_cuda_funcs.cuCtxSetCurrent(ctx);
            }
            qemu_log("CXL hetGPU: Created CUDA context on demand: %p\n", ctx);
        }
    }
}

static void cuda_unlock(HetGPUState *state)
{
    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        return;
    }
    qemu_mutex_unlock(&g_cuda_mutex);
}


HetGPUError hetgpu_synchronize(HetGPUState *state)
{
    if (!state || !state->initialized) {
        return HETGPU_ERROR_NOT_INITIALIZED;
    }

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        return HETGPU_SUCCESS;
    }

    /* For hetGPU managed mode (NULL context), still call synchronize */
    cuda_lock(state);

    if (g_cuda_funcs.cuCtxSynchronize) {
        int err = g_cuda_funcs.cuCtxSynchronize();
        cuda_unlock(state);
        if (err != 0) {
            const char *err_name = "UNKNOWN";
            if (g_cuda_funcs.cuGetErrorName) {
                g_cuda_funcs.cuGetErrorName(err, &err_name);
            }
            qemu_log("CXL hetGPU: cuCtxSynchronize failed: %s (%d)\n", err_name, err);
            return HETGPU_ERROR_UNKNOWN;
        }
        return HETGPU_SUCCESS;
    }

    cuda_unlock(state);
    return HETGPU_SUCCESS;
}

HetGPUError hetgpu_malloc(HetGPUState *state, size_t size,
                          HetGPUMemFlags flags, HetGPUDevicePtr *dev_ptr)
{
    (void)flags;

    if (!state || !state->initialized || !dev_ptr) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    state->memory_ops++;

    /* For hetGPU managed mode (NULL context), still use real GPU through hetGPU library */
    if (g_cuda_funcs.cuMemAlloc && state->backend != HETGPU_BACKEND_SIMULATION) {
        cuda_lock(state);

        uint64_t ptr = 0;
        int err = g_cuda_funcs.cuMemAlloc(&ptr, size);
        cuda_unlock(state);

        if (err == 0) {
            *dev_ptr = ptr;
            state->allocated_memory += size;
            qemu_log("CXL hetGPU: [dev%d] Allocated %zu bytes at device ptr 0x%lx\n",
                     state->device_index, size, (unsigned long)ptr);
            return HETGPU_SUCCESS;
        }
        const char *err_name = "UNKNOWN";
        if (g_cuda_funcs.cuGetErrorName) {
            g_cuda_funcs.cuGetErrorName(err, &err_name);
        }
        qemu_log("CXL hetGPU: [dev%d] cuMemAlloc(%zu) failed: %s (%d)\n",
                 state->device_index, size, err_name, err);
        return HETGPU_ERROR_OUT_OF_MEMORY;
    }

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        /* Allocate real memory for simulation */
        HetGPUSimAlloc *alloc = g_new0(HetGPUSimAlloc, 1);
        if (!alloc) {
            return HETGPU_ERROR_OUT_OF_MEMORY;
        }
        alloc->host_buffer = g_malloc0(size);
        if (!alloc->host_buffer) {
            g_free(alloc);
            return HETGPU_ERROR_OUT_OF_MEMORY;
        }
        alloc->dev_ptr = state->sim_next_ptr;
        alloc->size = size;
        alloc->next = state->sim_allocs;
        state->sim_allocs = alloc;

        *dev_ptr = state->sim_next_ptr;
        state->sim_next_ptr += (size + 0xFFF) & ~0xFFF;  /* Page align */
        state->allocated_memory += size;

        qemu_log("CXL hetGPU: SIM allocated %zu bytes at 0x%lx -> host %p\n",
                 size, (unsigned long)*dev_ptr, alloc->host_buffer);
        return HETGPU_SUCCESS;
    }

    return HETGPU_ERROR_NOT_INITIALIZED;
}

HetGPUError hetgpu_free(HetGPUState *state, HetGPUDevicePtr dev_ptr)
{
    if (!state || !state->initialized) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    state->memory_ops++;

    /* For hetGPU managed mode, use real GPU through hetGPU library */
    if (g_cuda_funcs.cuMemFree && state->backend != HETGPU_BACKEND_SIMULATION) {
        cuda_lock(state);
        int err = g_cuda_funcs.cuMemFree(dev_ptr);
        cuda_unlock(state);

        if (err != 0) {
            const char *err_name = "UNKNOWN";
            if (g_cuda_funcs.cuGetErrorName) {
                g_cuda_funcs.cuGetErrorName(err, &err_name);
            }
            qemu_log("CXL hetGPU: [dev%d] cuMemFree failed: %s (%d)\n",
                     state->device_index, err_name, err);
            return HETGPU_ERROR_INVALID_VALUE;
        }
        return HETGPU_SUCCESS;
    }

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        /* Find and free the simulation allocation */
        HetGPUSimAlloc *prev = NULL;
        HetGPUSimAlloc *alloc = state->sim_allocs;
        while (alloc) {
            if (alloc->dev_ptr == dev_ptr) {
                if (prev) {
                    prev->next = alloc->next;
                } else {
                    state->sim_allocs = alloc->next;
                }
                qemu_log("CXL hetGPU: SIM freed 0x%lx\n", (unsigned long)dev_ptr);
                g_free(alloc->host_buffer);
                g_free(alloc);
                return HETGPU_SUCCESS;
            }
            prev = alloc;
            alloc = alloc->next;
        }
        return HETGPU_SUCCESS;  /* Not found, ignore */
    }

    return HETGPU_SUCCESS;
}

HetGPUError hetgpu_memcpy_htod(HetGPUState *state, HetGPUDevicePtr dst,
                               const void *src, size_t size)
{
    if (!state || !state->initialized || !src) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    state->memory_ops++;

    /* For hetGPU managed mode, use real GPU through hetGPU library */
    if (g_cuda_funcs.cuMemcpyHtoD && state->backend != HETGPU_BACKEND_SIMULATION) {
        cuda_lock(state);
        int err = g_cuda_funcs.cuMemcpyHtoD(dst, src, size);
        cuda_unlock(state);

        if (err != 0) {
            const char *err_name = "UNKNOWN";
            if (g_cuda_funcs.cuGetErrorName) {
                g_cuda_funcs.cuGetErrorName(err, &err_name);
            }
            qemu_log("CXL hetGPU: [dev%d] cuMemcpyHtoD failed: %s (%d)\n",
                     state->device_index, err_name, err);
            return HETGPU_ERROR_INVALID_VALUE;
        }
        return HETGPU_SUCCESS;
    }

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        /* Find the allocation and copy data */
        HetGPUSimAlloc *alloc = find_sim_alloc(state, dst);
        if (alloc) {
            size_t offset = dst - alloc->dev_ptr;
            if (offset + size <= alloc->size) {
                memcpy((uint8_t *)alloc->host_buffer + offset, src, size);
                qemu_log("CXL hetGPU: SIM memcpy HtoD 0x%lx <- %zu bytes\n",
                         (unsigned long)dst, size);
                return HETGPU_SUCCESS;
            }
        }
        qemu_log("CXL hetGPU: SIM memcpy HtoD failed - allocation not found for 0x%lx\n",
                 (unsigned long)dst);
        return HETGPU_ERROR_INVALID_VALUE;
    }

    return HETGPU_SUCCESS;
}

HetGPUError hetgpu_memcpy_dtoh(HetGPUState *state, void *dst,
                               HetGPUDevicePtr src, size_t size)
{
    if (!state || !state->initialized || !dst) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    state->memory_ops++;

    /* For hetGPU managed mode, use real GPU through hetGPU library */
    if (g_cuda_funcs.cuMemcpyDtoH && state->backend != HETGPU_BACKEND_SIMULATION) {
        cuda_lock(state);
        int err = g_cuda_funcs.cuMemcpyDtoH(dst, src, size);
        cuda_unlock(state);

        if (err != 0) {
            const char *err_name = "UNKNOWN";
            if (g_cuda_funcs.cuGetErrorName) {
                g_cuda_funcs.cuGetErrorName(err, &err_name);
            }
            qemu_log("CXL hetGPU: [dev%d] cuMemcpyDtoH failed: %s (%d)\n",
                     state->device_index, err_name, err);
            return HETGPU_ERROR_INVALID_VALUE;
        }
        return HETGPU_SUCCESS;
    }

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        /* Find the allocation and copy data */
        HetGPUSimAlloc *alloc = find_sim_alloc(state, src);
        if (alloc) {
            size_t offset = src - alloc->dev_ptr;
            if (offset + size <= alloc->size) {
                memcpy(dst, (uint8_t *)alloc->host_buffer + offset, size);
                qemu_log("CXL hetGPU: SIM memcpy DtoH 0x%lx -> %zu bytes\n",
                         (unsigned long)src, size);
                return HETGPU_SUCCESS;
            }
        }
        qemu_log("CXL hetGPU: SIM memcpy DtoH failed - allocation not found for 0x%lx\n",
                 (unsigned long)src);
        memset(dst, 0, size);  /* Return zeros on error */
        return HETGPU_ERROR_INVALID_VALUE;
    }

    return HETGPU_SUCCESS;
}

HetGPUError hetgpu_memcpy_dtod(HetGPUState *state, HetGPUDevicePtr dst,
                               HetGPUDevicePtr src, size_t size)
{
    if (!state || !state->initialized) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    state->memory_ops++;

    /* For hetGPU managed mode, use real GPU through hetGPU library */
    if (g_cuda_funcs.cuMemcpyDtoD && state->backend != HETGPU_BACKEND_SIMULATION) {
        cuda_lock(state);
        int err = g_cuda_funcs.cuMemcpyDtoD(dst, src, size);
        cuda_unlock(state);

        if (err != 0) {
            const char *err_name = "UNKNOWN";
            if (g_cuda_funcs.cuGetErrorName) {
                g_cuda_funcs.cuGetErrorName(err, &err_name);
            }
            qemu_log("CXL hetGPU: [dev%d] cuMemcpyDtoD failed: %s (%d)\n",
                     state->device_index, err_name, err);
            return HETGPU_ERROR_INVALID_VALUE;
        }
        return HETGPU_SUCCESS;
    }

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        /* Find both allocations and copy between them */
        HetGPUSimAlloc *src_alloc = find_sim_alloc(state, src);
        HetGPUSimAlloc *dst_alloc = find_sim_alloc(state, dst);
        if (src_alloc && dst_alloc) {
            size_t src_offset = src - src_alloc->dev_ptr;
            size_t dst_offset = dst - dst_alloc->dev_ptr;
            if (src_offset + size <= src_alloc->size &&
                dst_offset + size <= dst_alloc->size) {
                memcpy((uint8_t *)dst_alloc->host_buffer + dst_offset,
                       (uint8_t *)src_alloc->host_buffer + src_offset, size);
                qemu_log("CXL hetGPU: SIM memcpy DtoD 0x%lx -> 0x%lx (%zu bytes)\n",
                         (unsigned long)src, (unsigned long)dst, size);
                return HETGPU_SUCCESS;
            }
        }
        qemu_log("CXL hetGPU: SIM memcpy DtoD failed - allocation not found\n");
        return HETGPU_ERROR_INVALID_VALUE;
    }

    return HETGPU_SUCCESS;
}

HetGPUError hetgpu_memset(HetGPUState *state, HetGPUDevicePtr dev_ptr,
                          int value, size_t size)
{
    if (!state || !state->initialized) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    state->memory_ops++;

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        /* Find the allocation and set memory */
        HetGPUSimAlloc *alloc = find_sim_alloc(state, dev_ptr);
        if (alloc) {
            size_t offset = dev_ptr - alloc->dev_ptr;
            if (offset + size <= alloc->size) {
                memset((uint8_t *)alloc->host_buffer + offset, value, size);
                return HETGPU_SUCCESS;
            }
        }
        return HETGPU_ERROR_INVALID_VALUE;
    }

    return HETGPU_SUCCESS;
}

HetGPUError hetgpu_create_coherent_region(HetGPUState *state, size_t size,
                                          HetGPUCoherentRegion *region)
{
    if (!state || !state->initialized || !region) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    state->coherency_ops++;

    /* Allocate host-mapped coherent memory */
    region->host_ptr = g_malloc0(size);
    if (!region->host_ptr) {
        return HETGPU_ERROR_OUT_OF_MEMORY;
    }

    region->size = size;
    region->flags = HETGPU_MEM_HOST_MAPPED;
    region->is_coherent = true;

    /* Get device pointer */
    HetGPUError err = hetgpu_malloc(state, size, HETGPU_MEM_HOST_MAPPED,
                                    &region->device_ptr);
    if (err != HETGPU_SUCCESS) {
        g_free(region->host_ptr);
        return err;
    }

    return HETGPU_SUCCESS;
}

void hetgpu_destroy_coherent_region(HetGPUState *state,
                                    HetGPUCoherentRegion *region)
{
    if (!state || !region) {
        return;
    }

    if (region->device_ptr) {
        hetgpu_free(state, region->device_ptr);
    }
    if (region->host_ptr) {
        g_free(region->host_ptr);
    }

    memset(region, 0, sizeof(*region));
}

void hetgpu_set_coherency_callback(HetGPUState *state,
                                   HetGPUCoherencyCallback callback,
                                   void *opaque)
{
    if (!state) {
        return;
    }

    state->coherency_callback = callback;
    state->cxl_opaque = opaque;
}

HetGPUError hetgpu_flush_cache(HetGPUState *state, HetGPUDevicePtr dev_ptr,
                               size_t size)
{
    (void)dev_ptr;
    (void)size;

    if (!state || !state->initialized) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    state->coherency_ops++;
    return HETGPU_SUCCESS;
}

HetGPUError hetgpu_invalidate_cache(HetGPUState *state, HetGPUDevicePtr dev_ptr,
                                    size_t size)
{
    (void)dev_ptr;
    (void)size;

    if (!state || !state->initialized) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    state->coherency_ops++;
    return HETGPU_SUCCESS;
}

HetGPUError hetgpu_load_ptx(HetGPUState *state, const char *ptx_source,
                            HetGPUModule *module)
{
    if (!state || !state->initialized || !ptx_source || !module) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    /* For hetGPU managed mode, use real GPU through hetGPU library */
    if (g_cuda_funcs.cuModuleLoadData && state->backend != HETGPU_BACKEND_SIMULATION) {
        cuda_lock(state);
        void *mod = NULL;
        int err = g_cuda_funcs.cuModuleLoadData(&mod, ptx_source);
        cuda_unlock(state);

        if (err == 0) {
            *module = mod;
            qemu_log("CXL hetGPU: [dev%d] Loaded PTX module %p\n",
                     state->device_index, mod);
            return HETGPU_SUCCESS;
        }
        const char *err_name = "UNKNOWN";
        if (g_cuda_funcs.cuGetErrorName) {
            g_cuda_funcs.cuGetErrorName(err, &err_name);
        }
        qemu_log("CXL hetGPU: [dev%d] cuModuleLoadData failed: %s (%d)\n",
                 state->device_index, err_name, err);
        return HETGPU_ERROR_INVALID_PTX;
    }

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        *module = (void *)0x12345678;  /* Dummy module handle */
        return HETGPU_SUCCESS;
    }

    return HETGPU_ERROR_NOT_INITIALIZED;
}

HetGPUError hetgpu_load_cubin(HetGPUState *state, const void *cubin_data,
                              size_t cubin_size, HetGPUModule *module)
{
    (void)cubin_size;

    if (!state || !state->initialized || !cubin_data || !module) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    /* For hetGPU managed mode, use real GPU through hetGPU library */
    if (g_cuda_funcs.cuModuleLoadData && state->backend != HETGPU_BACKEND_SIMULATION) {
        cuda_lock(state);
        void *mod = NULL;
        int err = g_cuda_funcs.cuModuleLoadData(&mod, cubin_data);
        cuda_unlock(state);

        if (err == 0) {
            *module = mod;
            qemu_log("CXL hetGPU: [dev%d] Loaded CUBIN module %p\n",
                     state->device_index, mod);
            return HETGPU_SUCCESS;
        }
        const char *err_name = "UNKNOWN";
        if (g_cuda_funcs.cuGetErrorName) {
            g_cuda_funcs.cuGetErrorName(err, &err_name);
        }
        qemu_log("CXL hetGPU: [dev%d] cuModuleLoadData (cubin) failed: %s (%d)\n",
                 state->device_index, err_name, err);
        return HETGPU_ERROR_INVALID_PTX;
    }

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        *module = (void *)0x12345678;
        return HETGPU_SUCCESS;
    }

    return HETGPU_ERROR_NOT_INITIALIZED;
}

void hetgpu_unload_module(HetGPUState *state, HetGPUModule module)
{
    (void)state;
    (void)module;
    /* Module unloading is handled by context destruction */
}

HetGPUError hetgpu_get_function(HetGPUState *state, HetGPUModule module,
                                const char *name, HetGPUFunction *function)
{
    if (!state || !state->initialized || !module || !name || !function) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    /* For hetGPU managed mode, use real GPU through hetGPU library */
    if (g_cuda_funcs.cuModuleGetFunction && state->backend != HETGPU_BACKEND_SIMULATION) {
        cuda_lock(state);
        void *func = NULL;
        int err = g_cuda_funcs.cuModuleGetFunction(&func, module, name);
        cuda_unlock(state);

        if (err == 0) {
            *function = func;
            qemu_log("CXL hetGPU: [dev%d] Got function '%s' -> %p\n",
                     state->device_index, name, func);
            return HETGPU_SUCCESS;
        }
        const char *err_name = "UNKNOWN";
        if (g_cuda_funcs.cuGetErrorName) {
            g_cuda_funcs.cuGetErrorName(err, &err_name);
        }
        qemu_log("CXL hetGPU: [dev%d] cuModuleGetFunction('%s') failed: %s (%d)\n",
                 state->device_index, name, err_name, err);
        return HETGPU_ERROR_UNKNOWN;
    }

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        *function = (void *)0x87654321;  /* Dummy function handle */
        return HETGPU_SUCCESS;
    }

    return HETGPU_ERROR_NOT_INITIALIZED;
}

HetGPUError hetgpu_launch_kernel(HetGPUState *state, HetGPUFunction function,
                                 const HetGPULaunchConfig *config,
                                 void **args, size_t num_args)
{
    (void)num_args;

    if (!state || !state->initialized || !function || !config) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    state->kernel_launches++;

    /* For hetGPU managed mode, use real GPU through hetGPU library */
    if (g_cuda_funcs.cuLaunchKernel && state->backend != HETGPU_BACKEND_SIMULATION) {
        cuda_lock(state);

        qemu_log("CXL hetGPU: [dev%d] Launching kernel grid=(%u,%u,%u) block=(%u,%u,%u) shared=%u\n",
                 state->device_index,
                 config->grid_dim[0], config->grid_dim[1], config->grid_dim[2],
                 config->block_dim[0], config->block_dim[1], config->block_dim[2],
                 config->shared_mem_bytes);

        int err = g_cuda_funcs.cuLaunchKernel(
            function,
            config->grid_dim[0], config->grid_dim[1], config->grid_dim[2],
            config->block_dim[0], config->block_dim[1], config->block_dim[2],
            config->shared_mem_bytes, config->stream,
            args, NULL);
        cuda_unlock(state);

        if (err == 0) {
            return HETGPU_SUCCESS;
        }
        const char *err_name = "UNKNOWN";
        if (g_cuda_funcs.cuGetErrorName) {
            g_cuda_funcs.cuGetErrorName(err, &err_name);
        }
        qemu_log("CXL hetGPU: [dev%d] cuLaunchKernel failed: %s (%d)\n",
                 state->device_index, err_name, err);
        return HETGPU_ERROR_LAUNCH_FAILED;
    }

    if (state->backend == HETGPU_BACKEND_SIMULATION) {
        qemu_log("CXL hetGPU: Simulated kernel launch grid=(%u,%u,%u) block=(%u,%u,%u)\n",
                 config->grid_dim[0], config->grid_dim[1], config->grid_dim[2],
                 config->block_dim[0], config->block_dim[1], config->block_dim[2]);
        return HETGPU_SUCCESS;
    }

    return HETGPU_SUCCESS;
}

HetGPUError hetgpu_create_stream(HetGPUState *state, HetGPUStream *stream)
{
    if (!state || !state->initialized || !stream) {
        return HETGPU_ERROR_INVALID_VALUE;
    }

    *stream = (void *)1;  /* Dummy stream for now */
    return HETGPU_SUCCESS;
}

void hetgpu_destroy_stream(HetGPUState *state, HetGPUStream stream)
{
    (void)state;
    (void)stream;
}

HetGPUError hetgpu_stream_synchronize(HetGPUState *state, HetGPUStream stream)
{
    (void)stream;
    return hetgpu_synchronize(state);
}

const char *hetgpu_get_error_string(HetGPUError error)
{
    switch (error) {
    case HETGPU_SUCCESS:
        return "Success";
    case HETGPU_ERROR_NOT_INITIALIZED:
        return "Not initialized";
    case HETGPU_ERROR_NO_DEVICE:
        return "No device";
    case HETGPU_ERROR_INVALID_DEVICE:
        return "Invalid device";
    case HETGPU_ERROR_INVALID_CONTEXT:
        return "Invalid context";
    case HETGPU_ERROR_OUT_OF_MEMORY:
        return "Out of memory";
    case HETGPU_ERROR_INVALID_PTX:
        return "Invalid PTX";
    case HETGPU_ERROR_LAUNCH_FAILED:
        return "Kernel launch failed";
    case HETGPU_ERROR_INVALID_VALUE:
        return "Invalid value";
    case HETGPU_ERROR_NOT_SUPPORTED:
        return "Not supported";
    default:
        return "Unknown error";
    }
}

const char *hetgpu_get_backend_name(HetGPUBackendType backend)
{
    switch (backend) {
    case HETGPU_BACKEND_AUTO:
        return "Auto";
    case HETGPU_BACKEND_INTEL:
        return "Intel Level Zero";
    case HETGPU_BACKEND_AMD:
        return "AMD HIP/ROCm";
    case HETGPU_BACKEND_NVIDIA:
        return "NVIDIA CUDA";
    case HETGPU_BACKEND_TENSTORRENT:
        return "Tenstorrent";
    case HETGPU_BACKEND_SIMULATION:
        return "Simulation";
    default:
        return "Unknown";
    }
}
