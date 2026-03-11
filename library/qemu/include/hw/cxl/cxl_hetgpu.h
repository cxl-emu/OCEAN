/*
 * CXL Type 2 Device - hetGPU Backend Integration
 * Provides CUDA compatibility layer for CXL Type 2 GPU accelerators
 *
 * This header defines the interface between QEMU's CXL Type 2 device
 * and the hetGPU backend, enabling PTX execution on any GPU through
 * the CXL coherency protocol.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CXL_HETGPU_H
#define CXL_HETGPU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hetGPU Backend Types */
typedef enum {
    HETGPU_BACKEND_AUTO = 0,     /* Auto-detect best backend */
    HETGPU_BACKEND_INTEL = 1,    /* Intel Level Zero backend */
    HETGPU_BACKEND_AMD = 2,      /* AMD HIP/ROCm backend */
    HETGPU_BACKEND_NVIDIA = 3,   /* Native NVIDIA (for testing) */
    HETGPU_BACKEND_TENSTORRENT = 4, /* Tenstorrent backend */
    HETGPU_BACKEND_SIMULATION = 5,  /* Software simulation */
} HetGPUBackendType;

/* hetGPU Error Codes */
typedef enum {
    HETGPU_SUCCESS = 0,
    HETGPU_ERROR_NOT_INITIALIZED = 1,
    HETGPU_ERROR_NO_DEVICE = 2,
    HETGPU_ERROR_INVALID_DEVICE = 3,
    HETGPU_ERROR_INVALID_CONTEXT = 4,
    HETGPU_ERROR_OUT_OF_MEMORY = 5,
    HETGPU_ERROR_INVALID_PTX = 6,
    HETGPU_ERROR_LAUNCH_FAILED = 7,
    HETGPU_ERROR_INVALID_VALUE = 8,
    HETGPU_ERROR_NOT_SUPPORTED = 9,
    HETGPU_ERROR_UNKNOWN = 255,
} HetGPUError;

/* Memory allocation flags */
typedef enum {
    HETGPU_MEM_DEFAULT = 0,
    HETGPU_MEM_UNIFIED = 1,       /* Unified (managed) memory */
    HETGPU_MEM_HOST_MAPPED = 2,   /* Host-mapped memory for coherent access */
    HETGPU_MEM_DEVICE_ONLY = 4,   /* Device-only memory */
} HetGPUMemFlags;

/* Device properties */
typedef struct HetGPUDeviceProps {
    char name[256];
    uint64_t total_memory;
    uint32_t compute_capability_major;
    uint32_t compute_capability_minor;
    uint32_t max_threads_per_block;
    uint32_t max_block_dim[3];
    uint32_t max_grid_dim[3];
    uint32_t warp_size;
    uint32_t multiprocessor_count;
    uint32_t clock_rate_khz;
    uint32_t memory_clock_rate_khz;
    uint32_t memory_bus_width;
    uint64_t l2_cache_size;
    bool supports_managed_memory;
    bool supports_coherent_memory;
    HetGPUBackendType backend_type;
} HetGPUDeviceProps;

/* Opaque handles */
typedef void* HetGPUContext;
typedef void* HetGPUModule;
typedef void* HetGPUFunction;
typedef void* HetGPUStream;
typedef void* HetGPUEvent;
typedef uint64_t HetGPUDevicePtr;

/* Memory region for CXL coherent access */
typedef struct HetGPUCoherentRegion {
    void *host_ptr;              /* Host-visible pointer */
    HetGPUDevicePtr device_ptr;  /* Device-side address */
    uint64_t size;               /* Region size */
    uint32_t flags;              /* HetGPUMemFlags */
    bool is_coherent;            /* True if CXL.cache coherent */
} HetGPUCoherentRegion;

/* PTX module info */
typedef struct HetGPUModuleInfo {
    const char *ptx_source;      /* PTX source code */
    size_t ptx_size;             /* PTX size in bytes */
    const char *cubin_data;      /* Optional pre-compiled cubin */
    size_t cubin_size;           /* Cubin size */
    uint32_t num_functions;      /* Number of functions in module */
    char **function_names;       /* Array of function names */
} HetGPUModuleInfo;

/* Kernel launch configuration */
typedef struct HetGPULaunchConfig {
    uint32_t grid_dim[3];        /* Grid dimensions */
    uint32_t block_dim[3];       /* Block dimensions */
    uint32_t shared_mem_bytes;   /* Dynamic shared memory size */
    HetGPUStream stream;         /* Stream for async execution */
} HetGPULaunchConfig;

/* CXL Coherency callback for cache operations */
typedef void (*HetGPUCoherencyCallback)(
    void *opaque,
    uint64_t addr,
    uint64_t size,
    bool invalidate
);

/* Simulation memory allocation tracking */
typedef struct HetGPUSimAlloc {
    HetGPUDevicePtr dev_ptr;
    void *host_buffer;
    size_t size;
    struct HetGPUSimAlloc *next;
} HetGPUSimAlloc;

/* hetGPU State for CXL Type 2 integration */
typedef struct HetGPUState {
    bool initialized;
    HetGPUBackendType backend;
    HetGPUContext context;
    int device_index;
    int cuda_device;        /* CUDA device handle from cuDeviceGet */
    HetGPUDeviceProps props;

    /* CXL coherency integration */
    void *cxl_opaque;
    HetGPUCoherencyCallback coherency_callback;

    /* Memory management */
    uint64_t allocated_memory;
    uint64_t max_memory;

    /* Simulation memory tracking */
    HetGPUSimAlloc *sim_allocs;
    uint64_t sim_next_ptr;

    /* Loaded modules */
    HetGPUModule *modules;
    size_t num_modules;
    size_t modules_capacity;

    /* Library handle for dynamic loading */
    void *hetgpu_lib;

    /* Statistics */
    uint64_t kernel_launches;
    uint64_t memory_ops;
    uint64_t coherency_ops;
} HetGPUState;

/* ========================================================================
 * Initialization and Cleanup
 * ======================================================================== */

/**
 * hetgpu_init - Initialize hetGPU backend
 * @state: Pointer to HetGPUState structure to initialize
 * @backend: Preferred backend type (use HETGPU_BACKEND_AUTO for auto-detect)
 * @device_index: GPU device index to use
 * @hetgpu_lib_path: Path to hetGPU library (libnvcuda.so)
 *
 * Returns: HETGPU_SUCCESS on success, error code otherwise
 */
HetGPUError hetgpu_init(HetGPUState *state, HetGPUBackendType backend,
                        int device_index, const char *hetgpu_lib_path);

/**
 * hetgpu_cleanup - Clean up hetGPU state
 * @state: Pointer to initialized HetGPUState
 */
void hetgpu_cleanup(HetGPUState *state);

/**
 * hetgpu_get_device_count - Get number of available GPU devices
 * @count: Output parameter for device count
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_get_device_count(int *count);

/**
 * hetgpu_get_device_props - Get device properties
 * @state: Initialized HetGPUState
 * @props: Output parameter for device properties
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_get_device_props(HetGPUState *state, HetGPUDeviceProps *props);

/* ========================================================================
 * Context Management
 * ======================================================================== */

/**
 * hetgpu_create_context - Create execution context
 * @state: Initialized HetGPUState
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_create_context(HetGPUState *state);

/**
 * hetgpu_destroy_context - Destroy execution context
 * @state: Initialized HetGPUState with context
 */
void hetgpu_destroy_context(HetGPUState *state);

/**
 * hetgpu_synchronize - Synchronize all GPU operations
 * @state: Initialized HetGPUState
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_synchronize(HetGPUState *state);

/* ========================================================================
 * Memory Management
 * ======================================================================== */

/**
 * hetgpu_malloc - Allocate device memory
 * @state: Initialized HetGPUState
 * @size: Size in bytes
 * @flags: Memory allocation flags
 * @dev_ptr: Output device pointer
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_malloc(HetGPUState *state, size_t size,
                          HetGPUMemFlags flags, HetGPUDevicePtr *dev_ptr);

/**
 * hetgpu_free - Free device memory
 * @state: Initialized HetGPUState
 * @dev_ptr: Device pointer to free
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_free(HetGPUState *state, HetGPUDevicePtr dev_ptr);

/**
 * hetgpu_memcpy_htod - Copy memory from host to device
 * @state: Initialized HetGPUState
 * @dst: Destination device pointer
 * @src: Source host pointer
 * @size: Size in bytes
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_memcpy_htod(HetGPUState *state, HetGPUDevicePtr dst,
                               const void *src, size_t size);

/**
 * hetgpu_memcpy_dtoh - Copy memory from device to host
 * @state: Initialized HetGPUState
 * @dst: Destination host pointer
 * @src: Source device pointer
 * @size: Size in bytes
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_memcpy_dtoh(HetGPUState *state, void *dst,
                               HetGPUDevicePtr src, size_t size);

/**
 * hetgpu_memcpy_dtod - Copy memory from device to device
 * @state: Initialized HetGPUState
 * @dst: Destination device pointer
 * @src: Source device pointer
 * @size: Size in bytes
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_memcpy_dtod(HetGPUState *state, HetGPUDevicePtr dst,
                               HetGPUDevicePtr src, size_t size);

/**
 * hetgpu_memset - Set device memory
 * @state: Initialized HetGPUState
 * @dev_ptr: Device pointer
 * @value: Value to set (single byte)
 * @size: Size in bytes
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_memset(HetGPUState *state, HetGPUDevicePtr dev_ptr,
                          int value, size_t size);

/* ========================================================================
 * CXL Coherent Memory Operations
 * ======================================================================== */

/**
 * hetgpu_create_coherent_region - Create CXL coherent memory region
 * @state: Initialized HetGPUState
 * @size: Size in bytes
 * @region: Output coherent region info
 *
 * This creates a memory region that is coherent between CPU and GPU
 * through the CXL.cache protocol.
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_create_coherent_region(HetGPUState *state, size_t size,
                                          HetGPUCoherentRegion *region);

/**
 * hetgpu_destroy_coherent_region - Destroy coherent memory region
 * @state: Initialized HetGPUState
 * @region: Coherent region to destroy
 */
void hetgpu_destroy_coherent_region(HetGPUState *state,
                                    HetGPUCoherentRegion *region);

/**
 * hetgpu_set_coherency_callback - Set CXL coherency callback
 * @state: Initialized HetGPUState
 * @callback: Callback function for coherency operations
 * @opaque: Opaque pointer passed to callback
 */
void hetgpu_set_coherency_callback(HetGPUState *state,
                                   HetGPUCoherencyCallback callback,
                                   void *opaque);

/**
 * hetgpu_flush_cache - Flush GPU cache for coherency
 * @state: Initialized HetGPUState
 * @dev_ptr: Device pointer (0 for all)
 * @size: Size to flush (0 for all)
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_flush_cache(HetGPUState *state, HetGPUDevicePtr dev_ptr,
                               size_t size);

/**
 * hetgpu_invalidate_cache - Invalidate GPU cache for coherency
 * @state: Initialized HetGPUState
 * @dev_ptr: Device pointer (0 for all)
 * @size: Size to invalidate (0 for all)
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_invalidate_cache(HetGPUState *state, HetGPUDevicePtr dev_ptr,
                                    size_t size);

/* ========================================================================
 * PTX Module and Kernel Management
 * ======================================================================== */

/**
 * hetgpu_load_ptx - Load PTX module
 * @state: Initialized HetGPUState
 * @ptx_source: PTX source code (null-terminated)
 * @module: Output module handle
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_load_ptx(HetGPUState *state, const char *ptx_source,
                            HetGPUModule *module);

/**
 * hetgpu_load_cubin - Load pre-compiled CUBIN
 * @state: Initialized HetGPUState
 * @cubin_data: CUBIN binary data
 * @cubin_size: Size of CUBIN data
 * @module: Output module handle
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_load_cubin(HetGPUState *state, const void *cubin_data,
                              size_t cubin_size, HetGPUModule *module);

/**
 * hetgpu_unload_module - Unload module
 * @state: Initialized HetGPUState
 * @module: Module to unload
 */
void hetgpu_unload_module(HetGPUState *state, HetGPUModule module);

/**
 * hetgpu_get_function - Get function handle from module
 * @state: Initialized HetGPUState
 * @module: Loaded module
 * @name: Function name
 * @function: Output function handle
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_get_function(HetGPUState *state, HetGPUModule module,
                                const char *name, HetGPUFunction *function);

/**
 * hetgpu_launch_kernel - Launch GPU kernel
 * @state: Initialized HetGPUState
 * @function: Function to launch
 * @config: Launch configuration
 * @args: Array of kernel argument pointers
 * @num_args: Number of arguments
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_launch_kernel(HetGPUState *state, HetGPUFunction function,
                                 const HetGPULaunchConfig *config,
                                 void **args, size_t num_args);

/* ========================================================================
 * Stream Management
 * ======================================================================== */

/**
 * hetgpu_create_stream - Create execution stream
 * @state: Initialized HetGPUState
 * @stream: Output stream handle
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_create_stream(HetGPUState *state, HetGPUStream *stream);

/**
 * hetgpu_destroy_stream - Destroy execution stream
 * @state: Initialized HetGPUState
 * @stream: Stream to destroy
 */
void hetgpu_destroy_stream(HetGPUState *state, HetGPUStream stream);

/**
 * hetgpu_stream_synchronize - Wait for stream completion
 * @state: Initialized HetGPUState
 * @stream: Stream to synchronize
 *
 * Returns: HETGPU_SUCCESS on success
 */
HetGPUError hetgpu_stream_synchronize(HetGPUState *state, HetGPUStream stream);

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

/**
 * hetgpu_get_error_string - Get error description
 * @error: Error code
 *
 * Returns: Static string describing the error
 */
const char *hetgpu_get_error_string(HetGPUError error);

/**
 * hetgpu_get_backend_name - Get backend name string
 * @backend: Backend type
 *
 * Returns: Static string with backend name
 */
const char *hetgpu_get_backend_name(HetGPUBackendType backend);

#ifdef __cplusplus
}
#endif

#endif /* CXL_HETGPU_H */
