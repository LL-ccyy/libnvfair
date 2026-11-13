#ifndef NVFAIR_RM_DEFS_H
#define NVFAIR_RM_DEFS_H

#include <stdint.h>
#include <stddef.h>

// ============ 来源 3:基础类型 ============
// ogkm 570.211.01,src/common/sdk/nvidia/inc/nvtypes.h
typedef uint32_t NvV32;     // L158 /* "void": enumerated or multiple fields */
typedef uint32_t NvU32;     // L159 /* 0 to 4294967295 */
typedef int32_t  NvS32;     // L160
typedef int16_t  NvS16;     // L114
typedef uint64_t NvU64;     // L209;x86-64 下 OPTIONAL_ALIGN8_ATTR(L54-60 仅
                            //      __i386__ 激活)为空,不抄对齐属性,自然对齐即 8
typedef void*    NvP64;     // L306(NV_64_BITS 分支) /* 64 bit void pointer */
typedef NvU32    NvHandle;  // L268(NV_TYPESAFE_HANDLES 未定义 → 裸 U32 形态;
                            //      结构体形态 L260-263 不抄)
#define NV_ALIGN_BYTES(size) __attribute__((aligned(size)))

// ============ 来源 1:ioctl 顶层信封 ============
// ogkm 570.211.01,src/nvidia/arch/nvalloc/unix/include/nv-ioctl-numbers.h
#define NV_IOCTL_MAGIC  'F'   // 同上 L29
#define NV_IOCTL_BASE   200   // 同上 L30

// ============ 来源 2:RM 子命令信封 ============
// ogkm 570.211.01,src/nvidia/arch/nvalloc/unix/include/nv-ioctl-numbers.h L27-45
// 的 nv_escape.h 段。
// RM 类 escape 是独立编号空间,不落在 NV_IOCTL_BASE 上;用户态经
// NV_ESC_IOCTL_XFER_CMD(= NV_IOCTL_BASE + 11,同文件 L37)包装后,
// nr 段取以下值:
#define NV_ESC_RM_ALLOC_MEMORY                     0x27   // 同上 L27
#define NV_ESC_RM_ALLOC_OBJECT                     0x28   // 同上 L28
#define NV_ESC_RM_FREE                             0x29   // 同上 L29 → NVOS00_PARAMETERS(escape.c L424-426;NVOS54 是 CONTROL 的,勿混)
#define NV_ESC_RM_CONTROL                          0x2A   // 同上 L30 → NVOS54_PARAMETERS(escape.c L708-710;NVOS33 是 MAP_MEMORY 0x4E 的,勿混)
#define NV_ESC_RM_ALLOC                            0x2B   // 同上 L31 → NVOS64/21_PARAMETERS(PBI-003/004 核心)
#define NV_ESC_RM_VID_HEAP_CONTROL                 0x4A   // 同上 L40 → NVOS32_PARAMETERS

// NV_ESC_RM_ALLOC
typedef struct
{
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectNew;
    NvV32    hClass;
    NvP64    pAllocParms NV_ALIGN_BYTES(8);
    NvU32    paramsSize;
    NvV32    status;
} NVOS21_PARAMETERS;

typedef struct
{
    NvHandle hRoot;                               // [IN] client handle
    NvHandle hObjectParent;                       // [IN] parent handle of new object
    NvHandle hObjectNew;                          // [INOUT] new object handle, 0 to generate
    NvV32    hClass;                              // [in] class num of new object
    NvP64    pAllocParms NV_ALIGN_BYTES(8);       // [IN] class-specific alloc parameters
    NvP64    pRightsRequested NV_ALIGN_BYTES(8);  // [IN] RS_ACCESS_MASK to request rights, or NULL
    NvU32    paramsSize;                          // [IN] Size of alloc params
    NvU32    flags;                               // [IN] flags for FINN serialization
    NvV32    status;                              // [OUT] status
} NVOS64_PARAMETERS;

// NV_ESC_RM_FREE
typedef struct
{
  NvHandle  hRoot;
  NvHandle  hObjectParent;
  NvHandle  hObjectOld;
  NvV32     status;
} NVOS00_PARAMETERS;

// NV_ESC_RM_CONTROL
typedef struct
{
    NvHandle hClient;
    NvHandle hObject;
    NvV32    cmd;
    NvU32    flags;
    NvP64    params NV_ALIGN_BYTES(8);
    NvU32    paramsSize;
    NvV32    status;
} NVOS54_PARAMETERS;


// NVOS32_PARAMETERS
typedef struct
{
  NvHandle  hRoot;                      // [IN]  - root object handle
  NvHandle  hObjectParent;              // [IN]  - device handle
  NvU32     function;                   // [IN]  - heap function, see below FUNCTION* defines
  NvHandle  hVASpace;                   // [IN]  - VASpace handle
  NvS16     ivcHeapNumber;              // [IN] - When IVC heaps are present: either 1) number of the IVC heap
                                        //        shared between two VMs or 2) number indicating that allocation
                                        //        should not be done on an IVC heap. Values greater than constant
                                        //        'NVOS32_IVC_HEAP_NUMBER_DONT_ALLOCATE_ON_IVC_HEAP' define set 1)
                                        //        and values less or equal to that constant define set 2).
                                        //        When IVC heaps are present, correct IVC-heap number must be specified.
                                        //        When IVC heaps are absent, IVC-heap number is diregarded.
                                        //        RM provides for each VM a bitmask of heaps with each bit
                                        //        specifying the other peer that can use the partition.
                                        //        Each bit set to one can be enumerated, such that the bit
                                        //        with lowest significance is enumerated with one.
                                        //        'ivcHeapNumber' parameter specifies this enumeration value.
                                        //        This value is used to uniquely identify a heap shared between
                                        //        two particular VMs.
                                        //        Illustration:
                                        //                                bitmask: 1  1  0  1  0 = 0x1A
                                        //        possible 'ivcHeapNumber' values: 3, 2,    1
  NvV32     status;                     // [OUT] - returned NVOS32* status code, see below STATUS* defines
  NvU64     total NV_ALIGN_BYTES(8);    // [OUT] - returned total size of heap
  NvU64     free  NV_ALIGN_BYTES(8);    // [OUT] - returned free space available in heap

  union
  {
      // NVOS32_FUNCTION_ALLOC_SIZE
      struct
      {
          NvU32     owner;              // [IN]  - memory owner ID
          NvHandle  hMemory;            // [IN/OUT]  - unique memory handle - IN only if MEMORY_HANDLE_PROVIDED is set (otherwise generated)
          NvU32     type;               // [IN]  - surface type, see below TYPE* defines
          NvU32     flags;              // [IN]  - allocation modifier flags, see below ALLOC_FLAGS* defines
          NvU32     attr;               // [IN/OUT] - surface attributes requested, and surface attributes allocated
          NvU32     format;             // [IN/OUT] - format requested, and format allocated
          NvU32     comprCovg;          // [IN/OUT] - compr covg requested, and allocated
          NvU32     zcullCovg;          // [OUT] - zcull covg allocated
          NvU32     partitionStride;    // [IN/OUT] - 0 means "RM" chooses
          NvU32     width;              // [IN] - width "hint" used for zcull region allocations
          NvU32     height;             // [IN] - height "hint" used for zcull region allocations
          NvU64     size      NV_ALIGN_BYTES(8); // [IN/OUT]  - size of allocation - also returns the actual size allocated
          NvU64     alignment NV_ALIGN_BYTES(8); // [IN]  - requested alignment - NVOS32_ALLOC_FLAGS_ALIGNMENT* must be on
          NvU64     offset    NV_ALIGN_BYTES(8); // [IN/OUT]  - desired offset if NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE is on AND returned offset
          NvU64     limit     NV_ALIGN_BYTES(8); // [OUT] - returned surface limit
          NvP64     address NV_ALIGN_BYTES(8);// [OUT] - returned address
          NvU64     rangeBegin NV_ALIGN_BYTES(8); // [IN]  - allocated memory will be limited to the range
          NvU64     rangeEnd   NV_ALIGN_BYTES(8); // [IN]  - from rangeBegin to rangeEnd, inclusive.
          NvU32     attr2;              // [IN/OUT] - surface attributes requested, and surface attributes allocated
          NvU32     ctagOffset;         // [IN] - comptag offset for this surface (see NVOS32_ALLOC_COMPTAG_OFFSET)
          NvS32     numaNode;           // [IN] - NUMA node from which memory should be allocated
      } AllocSize;

      // NVOS32_FUNCTION_ALLOC_TILED_PITCH_HEIGHT
      struct
      {
          NvU32     owner;              // [IN]  - memory owner ID
          NvHandle  hMemory;            // [IN/OUT]  - unique memory handle - IN only if MEMORY_HANDLE_PROVIDED is set (otherwise generated)
          NvU32     type;               // [IN]  - surface type, see below TYPE* defines
          NvU32     flags;              // [IN]  - allocation modifier flags, see below ALLOC_FLAGS* defines
          NvU32     height;             // [IN]  - height of surface in pixels
          NvS32     pitch;              // [IN/OUT] - desired pitch AND returned actual pitch allocated
          NvU32     attr;               // [IN/OUT] - surface attributes requested, and surface attributes allocated
          NvU32     width;               //[IN] - width of surface in pixels
          NvU32     format;             // [IN/OUT] - format requested, and format allocated
          NvU32     comprCovg;          // [IN/OUT] - compr covg requested, and allocated
          NvU32     zcullCovg;          // [OUT] - zcull covg allocated
          NvU32     partitionStride;    // [IN/OUT] - 0 means "RM" chooses
          NvU64     size      NV_ALIGN_BYTES(8); // [IN/OUT]  - size of allocation - also returns the actual size allocated
          NvU64     alignment NV_ALIGN_BYTES(8); // [IN]  - requested alignment - NVOS32_ALLOC_FLAGS_ALIGNMENT* must be on
          NvU64     offset    NV_ALIGN_BYTES(8); // [IN/OUT]  - desired offset if NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE is on AND returned offset
          NvU64     limit     NV_ALIGN_BYTES(8); // [OUT] - returned surface limit
          NvP64     address NV_ALIGN_BYTES(8);// [OUT] - returned address
          NvU64     rangeBegin NV_ALIGN_BYTES(8); // [IN]  - allocated memory will be limited to the range
          NvU64     rangeEnd   NV_ALIGN_BYTES(8); // [IN]  - from rangeBegin to rangeEnd, inclusive.
          NvU32     attr2;              // [IN/OUT] - surface attributes requested, and surface attributes allocated
          NvU32     ctagOffset;         // [IN] - comptag offset for this surface (see NVOS32_ALLOC_COMPTAG_OFFSET)
          NvS32     numaNode;           // [IN] - NUMA node from which memory should be allocated
      } AllocTiledPitchHeight;

      // NVOS32_FUNCTION_FREE
      struct
      {
          NvU32     owner;              // [IN]  - memory owner ID
          NvHandle  hMemory;            // [IN]  - unique memory handle
          NvU32     flags;              // [IN]  - heap free flags (must be NVOS32_FREE_FLAGS_MEMORY_HANDLE_PROVIDED)
      } Free;

      // NVOS32_FUNCTION_RELEASE_COMPR
      struct
      {
          NvU32     owner;           // [IN]  - memory owner ID
          NvU32     flags;           // [IN]  - must be NVOS32_RELEASE_COMPR_FLAGS_MEMORY_HANDLE_PROVIDED
          NvHandle  hMemory;         // [IN]  - unique memory handle (valid if _RELEASE_COMPR_FLAGS_MEMORY_HANDLE_PROVIDED
      } ReleaseCompr;

      // NVOS32_FUNCTION_REACQUIRE_COMPR
      struct
      {
          NvU32     owner;           // [IN]  - memory owner ID
          NvU32     flags;           // [IN]  - must be NVOS32_REACQUIRE_COMPR_FLAGS_MEMORY_HANDLE_PROVIDED
          NvHandle  hMemory;         // [IN]  - unique memory handle (valid if _REACQUIRE_COMPR_FLAGS_MEMORY_HANDLE_PROVIDED
      } ReacquireCompr;

      // NVOS32_FUNCTION_INFO
      struct
      {
          NvU32 attr;               // [IN] - memory heap attributes requested
          NvU64 offset NV_ALIGN_BYTES(8); // [OUT] - base of largest free block
          NvU64 size   NV_ALIGN_BYTES(8); // [OUT] - size of largest free block
          NvU64 base   NV_ALIGN_BYTES(8);   // [OUT] - returned heap phys base
      } Info;

      // NVOS32_FUNCTION_DUMP
      struct
      {
          NvU32 flags;              // [IN] - see _DUMP_FLAGS
          // [IN]  - if NULL, numBlocks is the returned number of blocks in
          //         heap, else returns all blocks in eHeap
          //         if non-NULL points to a buffer that is at least numBlocks
          //         * sizeof(NVOS32_HEAP_DUMP_BLOCK) bytes.
          NvP64 pBuffer NV_ALIGN_BYTES(8);
          // [IN/OUT] - if pBuffer is NULL, will number of blocks in heap
          //            if pBuffer is non-NULL, is input containing the size of
          //            pBuffer in units of NVOS32_HEAP_DUMP_BLOCK.  This must
          //            be greater than or equal to the number of blocks in the
          //            heap.
          NvU32 numBlocks;
      } Dump;

      // NVOS32_FUNCTION_DESTROY - no extra parameters needed

      // NVOS32_FUNCTION_ALLOC_SIZE_RANGE
      struct
      {
          NvU32     owner;              // [IN]  - memory owner ID
          NvHandle  hMemory;            // [IN]  - unique memory handle
          NvU32     type;               // [IN]  - surface type, see below TYPE* defines
          NvU32     flags;              // [IN]  - allocation modifier flags, see below ALLOC_FLAGS* defines
          NvU32     attr;               // [IN/OUT] - surface attributes requested, and surface attributes allocated
          NvU32     format;             // [IN/OUT] - format requested, and format allocated
          NvU32     comprCovg;          // [IN/OUT] - compr covg requested, and allocated
          NvU32     zcullCovg;          // [OUT] - zcull covg allocated
          NvU32     partitionStride;    // [IN/OUT] - 0 means "RM" chooses
          NvU64     size      NV_ALIGN_BYTES(8);  // [IN/OUT]  - size of allocation - also returns the actual size allocated
          NvU64     alignment NV_ALIGN_BYTES(8);  // [IN]  - requested alignment - NVOS32_ALLOC_FLAGS_ALIGNMENT* must be on
          NvU64     offset    NV_ALIGN_BYTES(8);  // [IN/OUT]  - desired offset if NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE is on AND returned offset
          NvU64     limit     NV_ALIGN_BYTES(8);  // [OUT] - returned surface limit
          NvU64     rangeBegin NV_ALIGN_BYTES(8); // [IN]  - allocated memory will be limited to the range
          NvU64     rangeEnd   NV_ALIGN_BYTES(8); // [IN]  - from rangeBegin to rangeEnd, inclusive.
          NvP64     address NV_ALIGN_BYTES(8);// [OUT] - returned address
          NvU32     attr2;              // [IN/OUT] - surface attributes requested, and surface attributes allocated
          NvU32     ctagOffset;         // [IN] - comptag offset for this surface (see NVOS32_ALLOC_COMPTAG_OFFSET)
          NvS32     numaNode;           // [IN] - NUMA node from which memory should be allocated
      } AllocSizeRange;

      // additions for Longhorn
#define NVAL_MAX_BANKS (4)
#define NVAL_MAP_DIRECTION             0:0
#define NVAL_MAP_DIRECTION_DOWN 0x00000000
#define NVAL_MAP_DIRECTION_UP   0x00000001

      // NVOS32_FUNCTION_GET_MEM_ALIGNMENT
      struct
      {
          NvU32 alignType;                                 // Input
          NvU32 alignAttr;
          NvU32 alignInputFlags;
          NvU64 alignSize NV_ALIGN_BYTES(8);
          NvU32 alignHeight;
          NvU32 alignWidth;
          NvU32 alignPitch;
          NvU32 alignPad;
          NvU32 alignMask;
          NvU32 alignOutputFlags[NVAL_MAX_BANKS];           // We could compress this information but it is probably not that big of a deal
          NvU32 alignBank[NVAL_MAX_BANKS];
          NvU32 alignKind;
          NvU32 alignAdjust;                                // Output -- If non-zero the amount we need to adjust the offset
          NvU32 alignAttr2;
      } AllocHintAlignment;

      struct
      {
          NvU32     allocOwner;              // [IN]  - memory owner ID
          NvHandle  allochMemory;            // [IN/OUT] - unique memory handle - IN only if MEMORY_HANDLE_PROVIDED is set (otherwise generated)
          NvU32     flags;
          NvU32     allocType;               // Input
          NvU32     allocAttr;
          NvU32     allocInputFlags;
          NvU64     allocSize       NV_ALIGN_BYTES(8);
          NvU32     allocHeight;
          NvU32     allocWidth;
          NvU32     allocPitch;
          NvU32     allocMask;
          NvU32     allocComprCovg;
          NvU32     allocZcullCovg;
          NvP64     bindResultFunc  NV_ALIGN_BYTES(8);         // BindResultFunc
          NvP64     pHandle         NV_ALIGN_BYTES(8);
          NvHandle  hResourceHandle;                          // Handle to RM container
          NvU32     retAttr;                                  // Output Indicates the resources that we allocated
          NvU32     kind;
          NvU64     osDeviceHandle  NV_ALIGN_BYTES(8);
          NvU32     allocAttr2;
          NvU32     retAttr2;                                 // Output Indicates the resources that we allocated
          NvU64     allocAddr       NV_ALIGN_BYTES(8);
          // [out] from GMMU_COMPR_INFO in drivers/common/shared/inc/mmu/gmmu_fmt.h
          struct
          {
              NvU32 compPageShift;
              NvU32 compressedKind;
              NvU32 compTagLineMin;
              NvU32 compPageIndexLo;
              NvU32 compPageIndexHi;
              NvU32 compTagLineMultiplier;
          } comprInfo;
          // [out] fallback uncompressed kind.
          NvU32  uncompressedKind;
      } HwAlloc;

      // NVOS32_FUNCTION_HW_FREE
      struct
      {
          NvHandle  hResourceHandle;                          // Handle to RM Resource Info
          NvU32     flags;                                    // Indicate if HW Resources and/or Memory
      } HwFree;
// Updated interface check.
#define NV_RM_OS32_ALLOC_OS_DESCRIPTOR_WITH_OS32_ATTR   1

      // NVOS32_FUNCTION_ALLOC_OS_DESCRIPTOR
      struct
      {
          NvHandle  hMemory;                      // [IN/OUT] - unique memory handle - IN only if MEMORY_HANDLE_PROVIDED is set (otherwise generated)
          NvU32     type;                         // [IN]  - surface type, see below TYPE* defines
          NvU32     flags;                        // [IN]  - allocation modifier flags, see below ALLOC_FLAGS* defines
          NvU32     attr;                         // [IN]  - attributes for memory placement/properties, see below
          NvU32     attr2;                        // [IN]  - attributes GPU_CACHEABLE
          NvP64     descriptor NV_ALIGN_BYTES(8); // [IN]  - descriptor address
          NvU64     limit      NV_ALIGN_BYTES(8); // [IN]  - allocated size -1
          NvU32     descriptorType;               // [IN]  - descriptor type(Virtual | nvmap Handle)
      } AllocOsDesc;

  } data;
} NVOS32_PARAMETERS;

// ============ 布局锚定 ============
// 数字来源:双探针互证 —— 同一探针程序分别编 ogkm 570.211.01 真头与本文档,
// 输出逐项一致。ogkm 升级后本区任何一条断言失败即布局漂移,必须人工核对
// escape.c 与 nvos.h 后更新本文件及相关偏移使用方。
static_assert(sizeof(NVOS00_PARAMETERS) == 16);
static_assert(offsetof(NVOS00_PARAMETERS, status) == 12);

static_assert(sizeof(NVOS21_PARAMETERS) == 32);
static_assert(offsetof(NVOS21_PARAMETERS, status) == 28);
static_assert(offsetof(NVOS21_PARAMETERS, pAllocParms) == 16);
static_assert(offsetof(NVOS21_PARAMETERS, paramsSize) == 24);

static_assert(sizeof(NVOS64_PARAMETERS) == 48);
static_assert(offsetof(NVOS64_PARAMETERS, status) == 40);
static_assert(offsetof(NVOS64_PARAMETERS, pAllocParms) == 16);
static_assert(offsetof(NVOS64_PARAMETERS, paramsSize) == 32);
static_assert(offsetof(NVOS64_PARAMETERS, flags) == 36);

static_assert(sizeof(NVOS54_PARAMETERS) == 32);
static_assert(offsetof(NVOS54_PARAMETERS, status) == 28);
static_assert(offsetof(NVOS54_PARAMETERS, params) == 16);

static_assert(sizeof(NVOS32_PARAMETERS) == 184);
static_assert(offsetof(NVOS32_PARAMETERS, status) == 20);
static_assert(offsetof(NVOS32_PARAMETERS, total) == 24);
static_assert(offsetof(NVOS32_PARAMETERS, free) == 32);
static_assert(offsetof(NVOS32_PARAMETERS, data) == 40);
static_assert(offsetof(NVOS32_PARAMETERS, data.Info.size) == 56);
static_assert(offsetof(NVOS32_PARAMETERS, data.AllocSize.size) == 88);

// ============ AC3:NVOS64 status 偏移选择 ============
// 同一编号 0x2B 有两种信纸(NVOS64=48B / NVOS21=32B),判据同内核
// escape.c:只看调用方传入的 dataSize,不猜驱动版本。返回 status 的
// 字节偏移;识别不了返回 -1。
static inline long rm_alloc_status_offset(NvU32 dataSize)
{
    if (dataSize == sizeof(NVOS64_PARAMETERS)) {
        return (long)offsetof(NVOS64_PARAMETERS, status);
    }
    if (dataSize == sizeof(NVOS21_PARAMETERS)) {
        return (long)offsetof(NVOS21_PARAMETERS, status);
    }
    return -1;
}
#endif  // NVFAIR_RM_DEFS_H
