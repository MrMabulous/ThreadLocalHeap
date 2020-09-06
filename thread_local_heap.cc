/*
Copyright(c) 2020 Matthias Bühlmann, Mabulous GmbH. http://www.mabulous.com
All rights reserved.
*/

#include <windows.h>
#include <memory>
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
class PrivateHeap {
 public:
  // Allocates a new Windows heap. The heap does not specify HEAP_NO_SERIALIZE since memory
  // allocated by one thread might be freed by another.
  PrivateHeap(bool use_process_heap = false)
      : allocation_count_(0),
        heap_handle_(NULL),
        marked_for_destruction_(false),
        use_process_heap_(use_process_heap) {
    if(use_process_heap) {
      heap_handle_ = GetProcessHeap();
    } else {
      heap_handle_ = HeapCreate(0, 0, 0);
    }
  }

  // Destroys the allocated Windows heap.
  ~PrivateHeap() {
    bool success = HeapDestroy(heap_handle_);
  }

  // Allocate bytes from the wrapped Windows heap.
  void* alloc(size_t bytes) {
    void* ptr = HeapAlloc(heap_handle_, 0, bytes);
    if(ptr != NULL) [[likely]] {
      allocation_count_++;
    }
    return ptr;
  }

  // Free memory from the wrapped wWindows heap. Ptr must point to memory that has previously
  // allocated from the wrapped heap.
  bool free(void* ptr) {
    bool success = HeapFree(heap_handle_, 0, ptr);
    if (success) [[likely]] {
      allocation_count_--;
      if(marked_for_destruction_ && !use_process_heap_ && empty()) [[unlikely]] {
        delete this;
      }
    }
    return success;
  }

  // Returns whether there are currently no allocations in the heap.
  bool empty() { return allocation_count_ == 0; }

  // Destroys the heap or marks it to be destroyed as soon as all allocations have been freed.
  void mark_for_destruction() { 
    if(empty() && !use_process_heap_) {
      delete this;
    } else {
      marked_for_destruction_ = true;
    }
  }

 private:
  // Number of allocations not yet freed.
  size_t allocation_count_;

  // Handle to the wrapped Windows heap.
  HANDLE heap_handle_;

  // If true, the heap will destroyed as soon as all allocations have been freed.
  bool marked_for_destruction_;

  // Whether this PrivateHeap wraps the process heap, in which case it should never be destroyed.
  bool use_process_heap_;
};

// The PrivateHeap wrapping the shared process heap.
PrivateHeap process_private_heap_(true);

// Returns the number of bytes to allocate to store the pointer from which to free the memory.
constexpr size_t heap_ptr_ofst() {
  return sizeof(PrivateHeap*) > __STDCPP_DEFAULT_NEW_ALIGNMENT__
       ? sizeof(PrivateHeap*) : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
}

// ThreadLocalHeap is created for each new thread and is destroyed when this thread ends.
class ThreadLocalHeap {
 public:
  // Creates a corresponding PrivateHeap which wraps a Windows heap.
  ThreadLocalHeap() {
    private_heap_ = new PrivateHeap();
    ready_ = true;
  }

  // Marks the corresponding PrivateHeap for destruction, which will cause it to destroy
  // the wrapped Windows heap as soon as all allocations in it have been freed.
  ~ThreadLocalHeap() {
    private_heap_->mark_for_destruction();
  }

  // Allocates |count| bytes from the corresponding PrivateHeap.
  void* alloc(size_t count) {
    size_t overalloc = count + heap_ptr_ofst();
    PrivateHeap* private_heap;
    if(ready_) [[likely]] {
      private_heap = private_heap_;
    } else [[unlikely]] {
      private_heap = &process_private_heap_;
    }
    void* ptr = private_heap->alloc(overalloc);
    if(ptr) [[likely]] {
      *((PrivateHeap**)ptr) = private_heap;
      return (void*)(((char*)ptr) + heap_ptr_ofst());
    }
    return ptr;
  }

  // Frees memory allocated from ANY ThreadLocalHeap.
  static bool free(void* ptr) {
    void* actual_ptr = (void*)(((char*)ptr) - heap_ptr_ofst());
    PrivateHeap* heap = *((PrivateHeap**)actual_ptr);
    if(heap) [[likely]] {
      return heap->free(actual_ptr);
    } else [[unlikely]] {
      std::free(actual_ptr);
      return true;
    }
  }

 private:
  // The associated PrivateHeap.
  PrivateHeap* private_heap_;

  // Whether the associated PrivateHeap has been consructed.
  bool ready_ = false;
};

// Each thread allocates a separate instance of thread_local_heap_.
thread_local ThreadLocalHeap thread_local_heap_;

// Implementation for operator new and operator new[]
inline void* new_impl(std::size_t bytes) {
  void* ptr = thread_local_heap_.alloc(bytes);
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
  ThreadLocalHeap::free(ptr);
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