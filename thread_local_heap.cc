/*
Copyright(c) 2020 Matthias Bühlmann, Mabulous GmbH. http://www.mabulous.com
All rights reserved.
*/

#include <windows.h>
#include <memory>
#include <mutex>
#include <new>

#if !defined(_WIN64) && __STDCPP_DEFAULT_NEW_ALIGNMENT__ > 8
// On 32-bit windows, HeapAlloc only guarantees 8-bit alignment whereas C++ requires
// new to align on __STDCPP_DEFAULT_NEW_ALIGNMENT__ boundaries. You should compile for
// x64 when using this library.
#error implementation cannot guarantee default new alignment.
#endif

namespace {

// The lifetime of PrivateHeap starts when the owning ThreadLocalHeap's lifetime starts. It ends,
// when the owning ThreadLocalHeap lifetime has ended AND the contained heap is empty.
struct PrivateHeap {
  HANDLE heap_handle;
  size_t allocation_count;
  bool marked_for_deletion;
  std::mutex mutex;
};

// Returns the number of bytes to allocate to store the pointer from which to free the memory.
constexpr size_t heap_ptr_ofst() {
  return sizeof(PrivateHeap*) > __STDCPP_DEFAULT_NEW_ALIGNMENT__
       ? sizeof(PrivateHeap*) : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
}

// Called when the associated thread has ended AND all memory from this heap has been freed.
void DestroyPrivateHeap(PrivateHeap* private_heap) {
  HeapDestroy(private_heap->heap_handle);
  std::free(private_heap);
}

// ThreadLocalHeap is allocated once for each thread and stores the pointer to the associated
// private heap.
struct ThreadLocalHeap {
  ~ThreadLocalHeap() {
    if(private_heap) {
      bool destroy_heap = false;
      {
        const std::lock_guard<std::mutex> lock(private_heap->mutex);
        private_heap->marked_for_deletion = private_heap->allocation_count == 0;
        destroy_heap = private_heap->marked_for_deletion;
      }
      if(destroy_heap) {
        DestroyPrivateHeap(private_heap);
      }
    }
  }
  PrivateHeap* private_heap;
};

// Each thread allocates a separate heap.
thread_local ThreadLocalHeap thread_local_heap_;

void* alloc_from_thread_local_heap(size_t count) {
  if(thread_local_heap_.private_heap == NULL) [[unlikely]] {
    thread_local_heap_.private_heap = static_cast<PrivateHeap*>(malloc(sizeof(PrivateHeap)));
    thread_local_heap_.private_heap->heap_handle = HeapCreate(0, 0, 0);
    thread_local_heap_.private_heap->allocation_count = 0;
    thread_local_heap_.private_heap->marked_for_deletion = false;
    thread_local_heap_.private_heap->mutex = std::mutex();
  }
  size_t overalloc = count + heap_ptr_ofst();
  void* ptr = HeapAlloc(thread_local_heap_.private_heap->heap_handle, 0, overalloc);
  if(ptr) [[likely]] {
    {
      const std::lock_guard<std::mutex> lock(thread_local_heap_.private_heap->mutex);
      thread_local_heap_.private_heap->allocation_count++;
    }
    *static_cast<PrivateHeap**>(ptr) = thread_local_heap_.private_heap;
    return static_cast<void*>(static_cast<char*>(ptr) + heap_ptr_ofst());
  }
  return ptr;
}

bool free_from_thread_local_heap(void* ptr) {
  void* actual_ptr = static_cast<void*>(static_cast<char*>(ptr) - heap_ptr_ofst());
  PrivateHeap* private_heap = *static_cast<PrivateHeap**>(actual_ptr);
  bool success = HeapFree(private_heap->heap_handle, 0, actual_ptr);
  if (success) [[likely]] {
    bool destroy_heap = false;
    {
      const std::lock_guard<std::mutex> lock(private_heap->mutex);
      size_t new_allocation_count = --private_heap->allocation_count;
      destroy_heap = new_allocation_count == 0 && private_heap->marked_for_deletion;
    }
    if(destroy_heap) [[unlikely]] {
      DestroyPrivateHeap(private_heap);
    }
  }
  return success;
}

// Implementation for operator new and operator new[]
inline void* new_impl(std::size_t bytes) {
  void* ptr = alloc_from_thread_local_heap(bytes);
  if (ptr) [[likely]] {
    return ptr;
  } else [[unlikely]] {
    throw std::bad_alloc{};
  }
}

// Implementation for operator delete and operator delete[]
inline void delete_impl(void* ptr) {
  if (!ptr)
    return;
  free_from_thread_local_heap(ptr);
}

}  // anonymous namespace


// ------------ global operator new, new[], delete and delete[] replacement ------------

void* operator new(std::size_t count) {
  return new_impl(count);
}

void operator delete(void* ptr) noexcept {
  delete_impl(ptr);
}

void* operator new(std::size_t count, std::align_val_t al) {
  std::size_t align = static_cast<std::size_t>(al);
  if (align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) [[unlikely]] return operator new(count);
  std::size_t actually_allocating = align + count;
  if (actually_allocating < count || (actually_allocating += sizeof(void*)) < (align + count)) [[unlikely]] {
    // overflow
    throw std::bad_alloc();
  }

  void* unaligned = operator new(actually_allocating);
  void* aligned = unaligned;
  std::align(align, 0, aligned, actually_allocating);
  // Store a pointer to the start of the aligned memory, to be retrieved by aligned delete.
  ::new (static_cast<void*>(static_cast<char*>(aligned) - sizeof(void*))) void*(unaligned);
  return aligned;
}

void operator delete(void* ptr, std::align_val_t al) noexcept {
  if (static_cast<std::size_t>(al) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) [[likely]] {
    ptr = *static_cast<void**>(static_cast<void*>(static_cast<char*>(ptr) - sizeof(void*)));
  }
  operator delete(ptr);
}