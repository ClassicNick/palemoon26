/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef WEBRTC_SYSTEM_WRAPPERS_INTERFACE_ALIGNED_MALLOC_H_
#define WEBRTC_SYSTEM_WRAPPERS_INTERFACE_ALIGNED_MALLOC_H_

// The functions declared here
// 1) Allocates block of aligned memory.
// 2) Re-calculates a pointer such that it is aligned to a higher or equal
//    address.
// Note: alignment must be a power of two. The alignment is in bytes.

#include <stddef.h>

#include "system_wrappers/interface/scoped_ptr.h"

/* Portable implementation From MSPS */

typedef struct _MSPS_SLIST_ENTRY {
    // Want a volatile pointer to non-volatile data so place after all type info
    struct _MSPS_SLIST_ENTRY * volatile Next;
} MSPS_SLIST_ENTRY, *MSPS_PSLIST_ENTRY;

typedef union _MSPS_SLIST_HEADER {
    // Provides 8 byte alignment for 32-bit builds.  Technically, not needed for
    // current implementation below but leaving for future use.
    ULONGLONG Alignment;
    struct {
        // Want a volatile pointer to non-volatile data so place after all type info
        MSPS_PSLIST_ENTRY volatile Head;
        volatile LONG Depth;
        volatile LONG Mutex;
    } List;
} MSPS_SLIST_HEADER, *MSPS_PSLIST_HEADER;

/* Portable implementation From MSPS */

namespace webrtc {

// Returns a pointer to the first boundry of |alignment| bytes following the
// address of |ptr|.
// Note that there is no guarantee that the memory in question is available.
// |ptr| has no requirements other than it can't be NULL.
void* GetRightAlign(const void* ptr, size_t alignment);

// Allocates memory of |size| bytes aligned on an |alignment| boundry.
// The return value is a pointer to the memory. Note that the memory must
// be de-allocated using AlignedFree.
void* AlignedMalloc(size_t size, size_t alignment);
// De-allocates memory created using the AlignedMalloc() API.
void AlignedFree(void* mem_block);

void InitializeSListHead_kex(MSPS_PSLIST_HEADER ListHeader);
MSPS_PSLIST_ENTRY InterlockedPopEntrySList_kex(MSPS_PSLIST_HEADER ListHeader);
MSPS_PSLIST_ENTRY InterlockedPushEntrySList_kex(MSPS_PSLIST_HEADER ListHeader, MSPS_PSLIST_ENTRY ListEntry);
MSPS_PSLIST_ENTRY InterlockedFlushSList_kex(MSPS_PSLIST_HEADER ListHeader);


// Templated versions to facilitate usage of aligned malloc without casting
// to and from void*.
template<typename T>
T* GetRightAlign(const T* ptr, size_t alignment) {
  return reinterpret_cast<T*>(GetRightAlign(reinterpret_cast<const void*>(ptr),
                                            alignment));
}
template<typename T>
T* AlignedMalloc(size_t size, size_t alignment) {
  return reinterpret_cast<T*>(AlignedMalloc(size, alignment));
}

// Scoped pointer to AlignedMalloc-memory.
template<typename T>
struct Allocator {
  typedef scoped_ptr_malloc<T, AlignedFree> scoped_ptr_aligned;
};

}  // namespace webrtc

#endif // WEBRTC_SYSTEM_WRAPPERS_INTERFACE_ALIGNED_MALLOC_H_
