# ThreadLocalHeap
Including this library in a Windows c++ project causes each thread to receive its own heap for memory allocations, which can greatly increase performance.

## Usage

Just add thread_local_heap.cc to the compiled sources of your project and make sure it is compiled with /std:c++17. Nothing else is required - all threads will now allocate memory from independent heaps when using versions of new or new[] (note that C-style malloc allocation still will be allocated from the shared default heap of the process).

## When to use
If you see that your mutithreaded application does not fully use your CPU because heap allocations and heap freeing is very slow, it's likely that thread synchronization is an issue. In this case, using this library can greatly improve the performance of your application.
