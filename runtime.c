/*
 * runtime.c
 * libclosure
 *
 * Copyright (c) 2008-2010 Apple Inc. All rights reserved.
 *
 * @APPLE_LLVM_LICENSE_HEADER@
 */


#include "Block_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#if TARGET_IPHONE_SIMULATOR
// workaround: 10682842
#define os_assumes(_x) (_x)
#define os_assert(_x) if (!(_x)) abort()
#else
#include <os/assumes.h>
#ifndef os_assumes
#define os_assumes(_x) os_assumes(_x)
#endif
#ifndef os_assert
#define os_assert(_x) os_assert(_x)
#endif
#endif

#if TARGET_OS_WIN32 // 如果是 win32，因为 win32 没有__sync_bool_compare_and_swap()函数，所以做了特殊处理
#define _CRT_SECURE_NO_WARNINGS 1
#include <windows.h>
static __inline bool OSAtomicCompareAndSwapLong(long oldl, long newl, long volatile *dst) 
{ 
    // fixme barrier is overkill -- see objc-os.h
    long original = InterlockedCompareExchange(dst, newl, oldl);
    return (original == oldl);
}

static __inline bool OSAtomicCompareAndSwapInt(int oldi, int newi, int volatile *dst) 
{ 
    // fixme barrier is overkill -- see objc-os.h
    int original = InterlockedCompareExchange(dst, newi, oldi);
    return (original == oldi);
}
#else
// __sync_bool_compare_and_swap 是 GCC 内建的原子操作函数， 执行CAS操作，也就是 比较 _Ptr 和 _Old 如果相等就将 _New 放到 _Ptr 中，并且返回true，否则返回false。
#define OSAtomicCompareAndSwapLong(_Old, _New, _Ptr) __sync_bool_compare_and_swap(_Ptr, _Old, _New)
#define OSAtomicCompareAndSwapInt(_Old, _New, _Ptr) __sync_bool_compare_and_swap(_Ptr, _Old, _New)
#endif


/***********************
Globals
************************/

// copy 后的 block 的类型，copy 都是 copy 到堆上，所以是 MallocBlock
static void *_Block_copy_class = _NSConcreteMallocBlock;
static void *_Block_copy_finalizing_class = _NSConcreteMallocBlock;

// Block_layout->flags 设为 BLOCK_NEEDS_FREE，即在堆上分配，需要手动释放
static int _Block_copy_flag = BLOCK_NEEDS_FREE;

// Block_byref->flags 的初始值，第一部分是设为需要释放，即在堆上分配；
// 第二部分是与 4 或运算，4 即 0b100，flag 中第 1~15 位是用来存引用计数的，所以引用计数初始化为 0b10，就是 2
// 引用计数是 2 的原因见 _Block_byref_assign_copy()
static int _Byref_flag_initial_value = BLOCK_BYREF_NEEDS_FREE | 4; // logical 2

static bool isGC = false; // 默认是不使用 GC

/*******************************************************************************
Internal Utilities 内部的工具函数
********************************************************************************/

// 引用计数加 1，最多不超过 BLOCK_REFCOUNT_MASK
// volatile的作用是：作为指令关键字，确保本条指令不会因编译器的优化而省略，且要求每次直接读值。简单地说就是防止编译器对代码进行优化
static int32_t latching_incr_int(volatile int32_t *where) {
    while (1) {
        int32_t old_value = *where;
        // 如果 old_value 在第 1~15 位都已经变为 1 了，即引用计数已经满了，就返回 BLOCK_REFCOUNT_MASK
        if ((old_value & BLOCK_REFCOUNT_MASK) == BLOCK_REFCOUNT_MASK) {
            return BLOCK_REFCOUNT_MASK;
        }
        // 比较 where 处的现在的值是否等于 old_value，如果等于，就将新值 oldValue + 2 放入 where
        // 否则继续下一轮循环
        // 这里加 2，是因为 flag 的第 0 位已经被占了，引用计数是第 1~15 位，所以加上 0b10，引用计数只是加 1
        if (OSAtomicCompareAndSwapInt(old_value, old_value+2, where)) {
            // 返回新的引用计数
            return old_value+2;
        }
    }
}

// 当 block 不是处于 dealloc 时，引用计数加 1
// 返回值是是否成功，只有在 block 处于 dealloc 时，才会失败
static bool latching_incr_int_not_deallocating(volatile int32_t *where) {
    while (1) {
        int32_t old_value = *where;
        if (old_value & BLOCK_DEALLOCATING) { // 如果 block 正在 dealloc，返回 false
            // if deallocating we can't do this
            return false;
        }
        // 引用计数最多不超过 BLOCK_REFCOUNT_MASK
        if ((old_value & BLOCK_REFCOUNT_MASK) == BLOCK_REFCOUNT_MASK) {
            // if latched, we're leaking this block, and we succeed
            return true;
        }
        // 引用计数加 1，这里 old_value+2 的原因和 latching_incr_int 一致
        // 如果失败，进行下一轮循环
        if (OSAtomicCompareAndSwapInt(old_value, old_value+2, where)) {
            // otherwise, we must store a new retained value without the deallocating bit set
            return true;
        }
    }
}


// return should_deallocate?
// 引用计数减 1，如果引用计数减到了 0，就将 block 置为 deallocating 状态
// 返回值是 block 是否需要被 dealloc
static bool latching_decr_int_should_deallocate(volatile int32_t *where) {
    while (1) {
        int32_t old_value = *where;
        // 如果引用计数还是满的，就不能 dealloc，#疑问：引用计数满了以后就不能减了么
        if ((old_value & BLOCK_REFCOUNT_MASK) == BLOCK_REFCOUNT_MASK) {
            return false; // latched high
        }
        // 如果引用计数为 0，按照正常的逻辑，它应该已经被置为 deallocating 状态，不需要再被 dealloc，所以返回 false
        if ((old_value & BLOCK_REFCOUNT_MASK) == 0) {
            return false;   // underflow, latch low
        }
        int32_t new_value = old_value - 2; // 引用计数减 1
        bool result = false;
        // 如果 old_value 在 0~15 位的值是 0b10，即引用计数是 1，且不是 deallocating 状态
        if ((old_value & (BLOCK_REFCOUNT_MASK|BLOCK_DEALLOCATING)) == 2) {
            new_value = old_value - 1; // old_value 减 1 后，新值 new_value 在 0~15 位值是 0b01，
                                       // 即引用计数变为0，且 BLOCK_DEALLOCATING 位变为 1
            result = true; // 需要被 dealloc
        }
        // 将新值 new_value 放入 where 中
        if (OSAtomicCompareAndSwapInt(old_value, new_value, where)) {
            return result;
        }
    }
}

// hit zero?
// 引用计数减 1，返回值是减 1 后引用计数是否等于 0，如果原来就是 0，也会返回 false
static bool latching_decr_int_now_zero(volatile int32_t *where) {
    while (1) {
        int32_t old_value = *where;
        if ((old_value & BLOCK_REFCOUNT_MASK) == BLOCK_REFCOUNT_MASK) {
            return false; // latched high
        }
        // 如果原来就是 0，直接返回 false
        if ((old_value & BLOCK_REFCOUNT_MASK) == 0) {
            return false;   // underflow, latch low
        }
        int32_t new_value = old_value - 2; // 引用计数减 1
        if (OSAtomicCompareAndSwapInt(old_value, new_value, where)) {
            // 引用计数当前是否是 0
            return (new_value & BLOCK_REFCOUNT_MASK) == 0;
        }
    }
}


/***********************
GC support stub routines
 
存根例程(stub routine)是被定义但是实际上不作任何事情的函数。意思有点像是函数的默认实现。
************************/
#if !TARGET_OS_WIN32
#pragma mark - GC Support Routines
#endif


static void *_Block_alloc_default(const unsigned long size, const bool initialCountIsOne __unused, const bool isObject __unused) {
    return malloc(size);
}

static void _Block_assign_default(void *value, void **destptr) {
    *destptr = value;
}

static void _Block_setHasRefcount_default(const void *ptr __unused, const bool hasRefcount __unused) {
}

static void _Block_do_nothing(const void *aBlock __unused) { }

static void _Block_retain_object_default(const void *ptr __unused) {
}

static void _Block_release_object_default(const void *ptr __unused) {
}

static void _Block_assign_weak_default(const void *ptr, void *dest) {
#if !TARGET_OS_WIN32
    *(long *)dest = (long)ptr;
#else
    *(void **)dest = (void *)ptr;
#endif
}

static void _Block_memmove_default(void *dst, void *src, unsigned long size) {
    memmove(dst, src, (size_t)size);
}

static void _Block_memmove_gc_broken(void *dest, void *src, unsigned long size) {
    void **destp = (void **)dest;
    void **srcp = (void **)src;
    while (size) {
        _Block_assign_default(*srcp, destp);
        destp++;
        srcp++;
        size -= sizeof(void *);
    }
}

static void _Block_destructInstance_default(const void *aBlock __unused) {}

/**************************************************************************
GC support callout functions - initially set to stub routines
 
 GC 的 callout(调出) 函数，被初始化为 stub routines（存根例程），即在非 GC 的情况下，它们不做或者只做很少的事情，完全与GC无关。
***************************************************************************/

static void *(*_Block_allocator)(const unsigned long, const bool isOne, const bool isObject) = _Block_alloc_default;
static void (*_Block_deallocator)(const void *) = (void (*)(const void *))free;
static void (*_Block_assign)(void *value, void **destptr) = _Block_assign_default;
static void (*_Block_setHasRefcount)(const void *ptr, const bool hasRefcount) = _Block_setHasRefcount_default;
static void (*_Block_retain_object)(const void *ptr) = _Block_retain_object_default;
static void (*_Block_release_object)(const void *ptr) = _Block_release_object_default;
static void (*_Block_assign_weak)(const void *dest, void *ptr) = _Block_assign_weak_default;
static void (*_Block_memmove)(void *dest, void *src, unsigned long size) = _Block_memmove_default;
static void (*_Block_destructInstance) (const void *aBlock) = _Block_destructInstance_default;


/**************************************************************************
GC support SPI functions - called from ObjC runtime and CoreFoundation
***************************************************************************/

// 指定用 GC
// Public SPI
// Called from objc-auto to turn on GC.
// version 3, 4 arg, but changed 1st arg
void _Block_use_GC( void *(*alloc)(const unsigned long, const bool isOne, const bool isObject),
                    void (*setHasRefcount)(const void *, const bool),
                    void (*gc_assign)(void *, void **),
                    void (*gc_assign_weak)(const void *, void *),
                    void (*gc_memmove)(void *, void *, unsigned long)) {
    // GC 下下面这些函数都被替换了
    isGC = true;
    _Block_allocator = alloc;
    _Block_deallocator = _Block_do_nothing;
    _Block_assign = gc_assign;
    _Block_copy_flag = BLOCK_IS_GC;
    _Block_copy_class = _NSConcreteAutoBlock;
    // blocks with ctors & dtors need to have the dtor run from a class with a finalizer
    _Block_copy_finalizing_class = _NSConcreteFinalizingBlock;
    _Block_setHasRefcount = setHasRefcount;
    _Byref_flag_initial_value = BLOCK_BYREF_IS_GC;   // no refcount
    _Block_retain_object = _Block_do_nothing;
    _Block_release_object = _Block_do_nothing;
    _Block_assign_weak = gc_assign_weak;
    _Block_memmove = gc_memmove;
}

// transitional
void _Block_use_GC5( void *(*alloc)(const unsigned long, const bool isOne, const bool isObject),
                    void (*setHasRefcount)(const void *, const bool),
                    void (*gc_assign)(void *, void **),
                    void (*gc_assign_weak)(const void *, void *)) {
    // until objc calls _Block_use_GC it will call us; supply a broken internal memmove implementation until then
    _Block_use_GC(alloc, setHasRefcount, gc_assign, gc_assign_weak, _Block_memmove_gc_broken);
}

 
// Called from objc-auto to alternatively turn on retain/release.
// Prior to this the only "object" support we can provide is for those
// super special objects that live in libSystem, namely dispatch queues.
// Blocks and Block_byrefs have their own special entry points.
void _Block_use_RR( void (*retain)(const void *),
                    void (*release)(const void *)) {
    _Block_retain_object = retain;
    _Block_release_object = release;
    _Block_destructInstance = dlsym(RTLD_DEFAULT, "objc_destructInstance");
}

// Called from CF to indicate MRR. Newer version uses a versioned structure, so we can add more functions
// without defining a new entry point.
void _Block_use_RR2(const Block_callbacks_RR *callbacks) {
    _Block_retain_object = callbacks->retain;
    _Block_release_object = callbacks->release;
    _Block_destructInstance = callbacks->destructInstance;
}

/****************************************************************************
Accessors for block descriptor fields
*****************************************************************************/
#if 0  // if 0，岂不是永远不会编译，那放在这里有什么意义呢，真是费解。不过，还真没有地方用到过这个函数。
static struct Block_descriptor_1 * _Block_descriptor_1(struct Block_layout *aBlock)
{
    return aBlock->descriptor;
}
#endif

// 取得 block 中的 Block_descriptor_2，它藏在 descriptor 列表中
// 调用者：_Block_call_copy_helper() / _Block_call_dispose_helper
static struct Block_descriptor_2 * _Block_descriptor_2(struct Block_layout *aBlock)
{
    // Block_descriptor_2 中存的是 copy/dispose 方法，如果没有指定有 copy / dispose 方法，则返回 NULL
    if (! (aBlock->flags & BLOCK_HAS_COPY_DISPOSE)) return NULL;
    
    // 先取得 Block_descriptor_1 的地址
    uint8_t *desc = (uint8_t *)aBlock->descriptor;
    
    // 偏移 Block_descriptor_1 的大小，就是 Block_descriptor_2 的起始地址
    desc += sizeof(struct Block_descriptor_1);
    return (struct Block_descriptor_2 *)desc;
}

// 取得 block 中的 Block_descriptor_3，它藏在 descriptor 列表中
// 调用者：_Block_extended_layout() / _Block_layout() / _Block_signature()
static struct Block_descriptor_3 * _Block_descriptor_3(struct Block_layout *aBlock)
{
    // Block_descriptor_3 中存的是 block 的签名，如果没有指定有签名，则直接返回 NULL
    if (! (aBlock->flags & BLOCK_HAS_SIGNATURE)) return NULL;
    
    // 先取得 Block_descriptor_1 的地址
    uint8_t *desc = (uint8_t *)aBlock->descriptor;
    
    // 先偏移 Block_descriptor_1 的大小
    desc += sizeof(struct Block_descriptor_1);
    
    // 如果还有 Block_descriptor_2，就再偏移 Block_descriptor_2 的大小，得到的就是 Block_descriptor_3 的地址
    if (aBlock->flags & BLOCK_HAS_COPY_DISPOSE) {
        desc += sizeof(struct Block_descriptor_2);
    }
    return (struct Block_descriptor_3 *)desc;
}

// 判断 block 是否有扩展布局（extended layout），扩展布局存在 Block_descriptor_3 中
// 调用者：_Block_copy_internal()
static __inline bool _Block_has_layout(struct Block_layout *aBlock) {
    // 如果没有指定有签名，即没有 Block_descriptor_3，就返回 false
    if (! (aBlock->flags & BLOCK_HAS_SIGNATURE)) return false;
    
    // 先取得 Block_descriptor_1 的地址
    uint8_t *desc = (uint8_t *)aBlock->descriptor;
    
    // 先偏移 Block_descriptor_1 的大小
    desc += sizeof(struct Block_descriptor_1);
    
    // 如果还有 Block_descriptor_2，就再偏移 Block_descriptor_2 的大小，得到的就是 Block_descriptor_3 的地址
    if (aBlock->flags & BLOCK_HAS_COPY_DISPOSE) {
        desc += sizeof(struct Block_descriptor_2);
    }
    
    // 看 Block_descriptor_3 中的 layout 是否非空
    return ((struct Block_descriptor_3 *)desc)->layout != NULL;
}    

// 调用 block 的 copy helper 方法，即 Block_descriptor_2 中的 copy 方法
// 调用者：_Block_copy_internal()
static void _Block_call_copy_helper(void *result, struct Block_layout *aBlock)
{
    // 取得 block 中的 Block_descriptor_2
    struct Block_descriptor_2 *desc = _Block_descriptor_2(aBlock);
    if (!desc) return; // 如果没有 Block_descriptor_2，就直接返回

    // 调用 desc 中的 copy 方法，copy 方法中会调用 _Block_object_assign 函数
    (*desc->copy)(result, aBlock); // do fixup
}

// 调用 block 的 dispose helper 方法，即 Block_descriptor_2 中的 dispose 方法
// 调用者：_Block_release()
static void _Block_call_dispose_helper(struct Block_layout *aBlock)
{
    // 取得 block 中的 Block_descriptor_2
    struct Block_descriptor_2 *desc = _Block_descriptor_2(aBlock);
    if (!desc) return; // 如果没有 Block_descriptor_2，就直接返回

    // 调用 desc 中的 dispose 方法，dispose 方法中会调用 _Block_object_dispose 函数
    (*desc->dispose)(aBlock);
}

/*******************************************************************************
Internal Support routines for copying
********************************************************************************/

#if !TARGET_OS_WIN32
#pragma mark - Copy/Release support
#endif

// Copy, or bump refcount, of a block.  If really copying, call the copy helper if present.
// 拷贝 block，
// 如果原来就在堆上，就将引用计数加 1;
// 如果原来在栈上，会拷贝到堆上，引用计数初始化为 1，并且会调用 copy helper 方法（如果存在的话）；
// 如果 block 在全局区，不用加引用计数，也不用拷贝，直接返回 block 本身
// 参数 arg 就是 Block_layout 对象，
// 参数 wantsOne 用于 GC，不必深究
// 返回值是拷贝后的 block 的地址
// 调用者：_Block_copy() / _Block_copy_collectable() / _Block_object_assign()
static void *_Block_copy_internal(const void *arg, const bool wantsOne) {
    struct Block_layout *aBlock;

    if (!arg) return NULL; // 如果 arg 为 NULL，直接返回 NULL
    
    
    // The following would be better done as a switch statement
    aBlock = (struct Block_layout *)arg; // 强转为 Block_layout 类型
    
    if (aBlock->flags & BLOCK_NEEDS_FREE) { // 如果现在已经在堆上
        // latches on high
        latching_incr_int(&aBlock->flags); // 就只将引用计数加 1
        return aBlock;
    }
    else if (aBlock->flags & BLOCK_IS_GC) { // 如果是 GC，不用管
        // GC refcounting is expensive so do most refcounting here.
        if (wantsOne && ((latching_incr_int(&aBlock->flags) & BLOCK_REFCOUNT_MASK) == 2)) {
            // Tell collector to hang on this - it will bump the GC refcount version
            _Block_setHasRefcount(aBlock, true);
        }
        return aBlock;
    }
    else if (aBlock->flags & BLOCK_IS_GLOBAL) { // 如果 block 在全局区，不用加引用计数，也不用拷贝，直接返回 block 本身
        return aBlock;
    }

    
    // Its a stack block.  Make a copy.
    // block 现在在栈上，现在需要将其拷贝到堆上
    
    if (!isGC) { // 如果不是 GC，我们只关心不是 GC 的情况
        // 在堆上重新开辟一块和 aBlock 相同大小的内存
        struct Block_layout *result = malloc(aBlock->descriptor->size);
        if (!result) return NULL; // 开辟失败，返回 NULL
        
        // 将 aBlock 内存上的数据全部移到新开辟的 result 上
        memmove(result, aBlock, aBlock->descriptor->size); // bitcopy first
        
        // reset refcount
        // 将 flags 中的 BLOCK_REFCOUNT_MASK 和 BLOCK_DEALLOCATING 部分的位全部清为 0
        result->flags &= ~(BLOCK_REFCOUNT_MASK|BLOCK_DEALLOCATING);    // XXX not needed
        
        // 将 result 标记位在堆上，需要手动释放；并且引用计数初始化为 1
        result->flags |= BLOCK_NEEDS_FREE | 2;  // logical refcount 1
        
        // isa 变为 _NSConcreteMallocBlock
        result->isa = _NSConcreteMallocBlock;
        
        // 调用 copy helper，即 Block_descriptor_2 中的 copy 方法
        // copy 方法中会调用做拷贝成员变量的工作
        _Block_call_copy_helper(result, aBlock);
        return result;
    }
    
    else { // GC 下，不用管
        // Under GC want allocation with refcount 1 so we ask for "true" if wantsOne
        // This allows the copy helper routines to make non-refcounted block copies under GC
        int32_t flags = aBlock->flags;
        bool hasCTOR = (flags & BLOCK_HAS_CTOR) != 0;
        struct Block_layout *result = _Block_allocator(aBlock->descriptor->size, wantsOne, hasCTOR || _Block_has_layout(aBlock));
        if (!result) return NULL;
        memmove(result, aBlock, aBlock->descriptor->size); // bitcopy first
        // reset refcount
        // if we copy a malloc block to a GC block then we need to clear NEEDS_FREE.
        flags &= ~(BLOCK_NEEDS_FREE|BLOCK_REFCOUNT_MASK|BLOCK_DEALLOCATING);   // XXX not needed
        if (wantsOne)
            flags |= BLOCK_IS_GC | 2;
        else
            flags |= BLOCK_IS_GC;
        result->flags = flags;
        _Block_call_copy_helper(result, aBlock);
        if (hasCTOR) {
            result->isa = _NSConcreteFinalizingBlock;
        }
        else {
            result->isa = _NSConcreteAutoBlock;
        }
        return result;
    }
}





// Runtime entry points for maintaining the sharing knowledge of byref data blocks.

// A closure has been copied and its fixup routine is asking us to fix up the reference to the shared byref data
// Closures that aren't copied must still work, so everyone always accesses variables after dereferencing the forwarding ptr.
// We ask if the byref pointer that we know about has already been copied to the heap, and if so, increment it.
// Otherwise we need to copy it and update the stack forwarding pointer

// assign byref
// 1. 如果 byref 原来在堆上，就将其拷贝到堆上，拷贝的包括 Block_byref、Block_byref_2、Block_byref_3，
//    被 __weak 修饰的 byref 会被修改 isa 为 _NSConcreteWeakBlockVariable，
//    原来 byref 的 forwarding 也会指向堆上的 byref;
// 2. 如果 byref 已经在堆上，就只增加一个引用计数。
// 参数 dest是一个二级指针，指向了目标指针，最终，目标指针会指向堆上的 byref
// 调用者：_Block_object_assign()
static void _Block_byref_assign_copy(void *dest, // 指向目标地址的二级指针
                                     const void *arg, // byref对象
                                     const int flags) // 标识了 arg 是什么类型的
{
    struct Block_byref **destp = (struct Block_byref **)dest; // 原本就是一个二级指针，指向目标地址
    struct Block_byref *src = (struct Block_byref *)arg; // arg 强转为 Block_byref * 类型
    
    // 如果是 GC，就什么都不用干
    if (src->forwarding->flags & BLOCK_BYREF_IS_GC) {
        ;   // don't need to do any more work
    }
    
    // 如果不是 GC，且引用计数等于 0
    else if ((src->forwarding->flags & BLOCK_REFCOUNT_MASK) == 0) {
        // src points to stack
        // 如果是弱引用，即被 __weak 修饰过的 byref
        bool isWeak = ((flags & (BLOCK_FIELD_IS_BYREF|BLOCK_FIELD_IS_WEAK)) == (BLOCK_FIELD_IS_BYREF|BLOCK_FIELD_IS_WEAK));
        
        // if its weak ask for an object (only matters under GC)
        // 为新的 byref 在堆中分配内存，isWeak 只对 GC 下有用，其他情况只是单纯得调用 _Block_alloc_default，里面只是 malloc(size)
        struct Block_byref *copy = (struct Block_byref *)_Block_allocator(src->size, false, isWeak);
        
        // _Byref_flag_initial_value = BLOCK_BYREF_NEEDS_FREE | 4，即新 byref 的 flags 中标记了它是在堆上，且引用计数为 2。
        // 为什么是 2 呢？注释说的是 non-GC one for caller, one for stack
        // one for caller 很好理解，那 one for stack 是为什么呢？
        // 看下面的代码中有一行 src->forwarding = copy。src 的 forwarding 也指向了 copy，相当于引用了 copy。
        copy->flags = src->flags | _Byref_flag_initial_value; // non-GC one for caller, one for stack
        
        // 堆上 byref 的 forwarding 指向自己
        copy->forwarding = copy; // patch heap copy to point to itself (skip write-barrier)
        
        // 原来栈上的 byref 的 forwarding 现在也指向堆上的 byref
        src->forwarding = copy;  // patch stack to point to heap copy
        copy->size = src->size; // 拷贝 size
        
        // 被 __weak 修饰的 byref 会被设置 isa 是 _NSConcreteWeakBlockVariable
        // 它在后面会接受 weak scanning
        if (isWeak) {
            copy->isa = &_NSConcreteWeakBlockVariable;  // mark isa field so it gets weak scanning
        }
        
        // 如果 src 有 copy/dispose helper
        if (src->flags & BLOCK_BYREF_HAS_COPY_DISPOSE) {
            // Trust copy helper to copy everything of interest
            // If more than one field shows up in a byref block this is wrong XXX
            
            // 取得 src 和 copy 的 Block_byref_2
            struct Block_byref_2 *src2 = (struct Block_byref_2 *)(src+1);
            struct Block_byref_2 *copy2 = (struct Block_byref_2 *)(copy+1);
            
            // copy 的 copy/dispose helper 也与 src 保持一致
            // 因为是函数指针，估计也不是在栈上，所以不用担心被销毁
            copy2->byref_keep = src2->byref_keep;
            copy2->byref_destroy = src2->byref_destroy;

            // 如果 src 有扩展布局，也拷贝扩展布局
            if (src->flags & BLOCK_BYREF_LAYOUT_EXTENDED) {
                struct Block_byref_3 *src3 = (struct Block_byref_3 *)(src2+1);
                struct Block_byref_3 *copy3 = (struct Block_byref_3*)(copy2+1);
                copy3->layout = src3->layout; // 没有将 layout 字符串拷贝到堆上，是因为它是 const 常量，不在栈上
            }

            // 调用 copy helper，因为 src 和 copy 的 copy helper 是一样的，所以用谁的都行，调用的都是同一个函数
            (*src2->byref_keep)(copy, src);
        }
        else { // 如果 src 没有 copy/dispose helper
            // just bits.  Blast 'em using _Block_memmove in case they're __strong
            // This copy includes Block_byref_3, if any.
            // _Block_memmove 在非 GC 下的默认实现只是 memmove
            // 将 Block_byref 后面的数据都拷贝到 copy 中，一定包括 Block_byref_3
            _Block_memmove(copy+1, src+1,
                           src->size - sizeof(struct Block_byref));
        }
    }
    
    // already copied to heap
    // 如果不是 GC 且 src 已经在堆上，就只将引用计数加 1
    else if ((src->forwarding->flags & BLOCK_BYREF_NEEDS_FREE) == BLOCK_BYREF_NEEDS_FREE) {
        latching_incr_int(&src->forwarding->flags);
    }
    
    // assign byref data block pointer into new Block
    // _Block_assign 在非 GC 下的默认实现只是 *destp = src->forwarding
    // 即 destp 指向的指针，指向堆上的 byref 对象（src->forwarding 在上面已经指向了堆上的 byref）
    _Block_assign(src->forwarding, (void **)destp);
}


// Old compiler SPI
// 对 byref 对象做 release 操作，
// 堆上的 byref 需要 release，栈上的不需要 release，
// release 就是引用计数减 1，如果引用计数减到了 0，就将 byref 对象销毁
// 调用者：_Block_object_dispose()
static void _Block_byref_release(const void *arg) {
    struct Block_byref *byref = (struct Block_byref *)arg;
    int32_t refcount; // 用来记录引用计数

    // dereference the forwarding pointer since the compiler isn't doing this anymore (ever?)
    // 取得真正指向的 byref，如果 byref 已经被堆拷贝，则取得是堆上的 byref，否则是栈上的，栈上的不需要 release，也没有引用计数
    byref = byref->forwarding;
    
    // To support C++ destructors under GC we arrange for there to be a finalizer for this
    // by using an isa that directs the code to a finalizer that calls the byref_destroy method.
    
    // 如果 byref 还没有被拷贝到堆上，则不需要 release，直接返回
    if ((byref->flags & BLOCK_BYREF_NEEDS_FREE) == 0) {
        return; // stack or GC or global
    }
    
    // 取得引用计数
    refcount = byref->flags & BLOCK_REFCOUNT_MASK;
	
    os_assert(refcount); // 断言，但是不知道干嘛的，可能是防止引用计数为 0，
                        // 正常情况下，如果上一次 release 使得引用计数为 0，那么 byref 就应该已经被销毁了，不会走到这里的
    
    // 引用计数减 1，如果引用计数减到了 0，会返回 true，表示 byref 需要被销毁
    if (latching_decr_int_should_deallocate(&byref->flags)) {
        
        // 如果 byref 有 dispose helper，就先调用它的 dispose helper
        if (byref->flags & BLOCK_BYREF_HAS_COPY_DISPOSE) {
            // dispose helper 藏在 Block_byref_2 里
            struct Block_byref_2 *byref2 = (struct Block_byref_2 *)(byref+1);
            (*byref2->byref_destroy)(byref);
        }
        
        // 非 GC 下的 _Block_deallocator 的默认实现就是 free
        _Block_deallocator((struct Block_layout *)byref);
    }
}


/************************************************************
 *
 * API supporting SPI
 * _Block_copy, _Block_release, and (old) _Block_destroy
 *
 
 SPI - Service Provider Interface
 API - Application Programming Interface 
 API 和 SPI 都是相对的概念，他们的差别只在语义上，API 直接被应用开发人员使用，SPI 被框架扩张人员使用
 ***********************************************************/

#if !TARGET_OS_WIN32
#pragma mark - SPI/API  下面都是 SPI，即给其他库提供的接口，在 libclosure 中并没有使用到
#endif

// 拷贝 block，详情见 _Block_copy_internal
void *_Block_copy(const void *arg) {
    return _Block_copy_internal(arg, true);
}


// API entry point to release a copied Block
// 对 block 做 release 操作。
// block 在堆上，才需要 release，在全局区和栈区都不需要 release.
// 先将引用计数减 1，如果引用计数减到了 0，就将 block 销毁
void _Block_release(const void *arg) {
    struct Block_layout *aBlock = (struct Block_layout *)arg;
    if (!aBlock // 如果 block == nil
        || (aBlock->flags & BLOCK_IS_GLOBAL) // 如果 block 在全局区
        || ((aBlock->flags & (BLOCK_IS_GC|BLOCK_NEEDS_FREE)) == 0)) // 如果不是 GC，且 block 不在堆上
    {
        return; // 直接返回
    }
    
    // 如果是 GC，不用管
    if (aBlock->flags & BLOCK_IS_GC) {
        if (latching_decr_int_now_zero(&aBlock->flags)) {
            // Tell GC we no longer have our own refcounts.  GC will decr its refcount
            // and unless someone has done a CFRetain or marked it uncollectable it will
            // now be subject to GC reclamation.
            _Block_setHasRefcount(aBlock, false);
        }
    }
    
    // 如果不是 GC，且 block 在堆上
    else if (aBlock->flags & BLOCK_NEEDS_FREE) {
        
        // 引用计数减 1，如果引用计数减到了 0，会返回 true，表示 block 需要被销毁
        if (latching_decr_int_should_deallocate(&aBlock->flags)) {
            
            // 调用 block 的 dispose helper，dispose helper 方法中会做诸如销毁 byref 等操作
            _Block_call_dispose_helper(aBlock);
            
            // 非 GC 下的 _Block_destructInstance 啥也不干，函数体是空的
            _Block_destructInstance(aBlock);
            
            // 非 GC 下的 _Block_deallocator 的默认实现就是 free
            _Block_deallocator(aBlock);
        }
    }
}

// 尝试 retain block。当 block 不是处于 dealloc 时，引用计数加 1
// 返回值是是否成功，只有在 block 处于 deallocating 时，才会失败
bool _Block_tryRetain(const void *arg) {
    struct Block_layout *aBlock = (struct Block_layout *)arg;
    return latching_incr_int_not_deallocating(&aBlock->flags);
}

// 判断 block 是否处于 deallocating 状态
bool _Block_isDeallocating(const void *arg) {
    struct Block_layout *aBlock = (struct Block_layout *)arg;
    return (aBlock->flags & BLOCK_DEALLOCATING) != 0;
}

// Old Compiler SPI point to release a copied Block used by the compiler in dispose helpers
// 虽然名字是 destroy，但只是做 release 操作，主要逻辑都在 _Block_release 中
static void _Block_destroy(const void *arg) {
    struct Block_layout *aBlock;
    if (!arg) return;
    aBlock = (struct Block_layout *)arg;
    if (aBlock->flags & BLOCK_IS_GC) { // GC 下什么都不干
        // assert(aBlock->Block_flags & BLOCK_HAS_CTOR);
        return; // ignore, we are being called because of a DTOR
    }
    
    // 对 block 做 release 操作
    _Block_release(aBlock);
}



/************************************************************
 *
 * SPI used by other layers
 *
 ***********************************************************/

// SPI, also internal.  Called from NSAutoBlock only under GC
// 只在 GC 下有用，不用管
void *_Block_copy_collectable(const void *aBlock) {
    return _Block_copy_internal(aBlock, false);
}


// SPI
// 取得 block 的完整大小
size_t Block_size(void *aBlock) {
    return ((struct Block_layout *)aBlock)->descriptor->size;
}

// 如果 block 的返回值在栈上，则返回 TRUE，反之返回 FALSE
bool _Block_use_stret(void *aBlock) {
    struct Block_layout *layout = (struct Block_layout *)aBlock;
 
    // block 的 flag 有 BLOCK_HAS_SIGNATURE 和 BLOCK_USE_STRET，才会返回 TRUE
    int requiredFlags = BLOCK_HAS_SIGNATURE | BLOCK_USE_STRET;
    return (layout->flags & requiredFlags) == requiredFlags;
}

// Checks for a valid signature, not merely the BLOCK_HAS_SIGNATURE bit.
// 判断 block 是否有签名，不判断 BLOCK_HAS_SIGNATURE，而是通过直接取签名字符串来确定存在与否
bool _Block_has_signature(void *aBlock) {
    return _Block_signature(aBlock) ? true : false;
}

// 取得 block 的签名字符串，可能是 NULL
const char * _Block_signature(void *aBlock)
{
    // 取得 Block_descriptor_3，签名在其中
    struct Block_descriptor_3 *desc3 = _Block_descriptor_3(aBlock);
    if (!desc3) return NULL; // 如果没有 desc3，则一定没有签名，返回 NULL

    return desc3->signature; // 返回 desc3 中的签名字符串
}

// 取得 block 的 GC layout
const char * _Block_layout(void *aBlock)
{
    // Don't return extended layout to callers expecting GC layout
    // 如果有扩展布局，就返回 NULL，要的是 GC layout 而不是扩展布局
    struct Block_layout *layout = (struct Block_layout *)aBlock;
    if (layout->flags & BLOCK_HAS_EXTENDED_LAYOUT) return NULL;

    // 如果没有 Block_descriptor_3，也返回 NULL
    struct Block_descriptor_3 *desc3 = _Block_descriptor_3(aBlock);
    if (!desc3) return NULL;

    return desc3->layout;
}

// 返回 block 的扩展布局字符串
// 如果没有扩展布局，则返回 NULL，如果指定有扩展布局，但扩展布局为 NULL，则返回的是 "" 空字符串
const char * _Block_extended_layout(void *aBlock)
{
    // Don't return GC layout to callers expecting extended layout
    // 如果没有扩展布局，就返回 NULL，不要返回 GC layout
    struct Block_layout *layout = (struct Block_layout *)aBlock;
    if (! (layout->flags & BLOCK_HAS_EXTENDED_LAYOUT)) return NULL;

    // desc3 为 NULL，则返回 NULL，因为一定没有扩展布局
    struct Block_descriptor_3 *desc3 = _Block_descriptor_3(aBlock);
    if (!desc3) return NULL;

    // Return empty string (all non-object bytes) instead of NULL 
    // so callers can distinguish "empty layout" from "no layout".
    if (!desc3->layout) {
        return ""; // 如果扩展布局为 NULL，则返回空字符串
    } else {
        return desc3->layout; // 否则返回扩展布局
    }
}

#if !TARGET_OS_WIN32
#pragma mark - Compiler SPI entry points
#endif

    
/*******************************************************

Entry points used by the compiler - the real API!


A Block can reference four different kinds of things that require help when the Block is copied to the heap.
1) C++ stack based objects
2) References to Objective-C objects
3) Other Blocks
4) __block variables

In these cases helper functions are synthesized by the compiler for use in Block_copy and Block_release, called the copy and dispose helpers.  The copy helper emits a call to the C++ const copy constructor for C++ stack based objects and for the rest calls into the runtime support function _Block_object_assign.  The dispose helper has a call to the C++ destructor for case 1 and a call into _Block_object_dispose for the rest.

The flags parameter of _Block_object_assign and _Block_object_dispose is set to
	* BLOCK_FIELD_IS_OBJECT (3), for the case of an Objective-C Object,
	* BLOCK_FIELD_IS_BLOCK (7), for the case of another Block, and
	* BLOCK_FIELD_IS_BYREF (8), for the case of a __block variable.
If the __block variable is marked weak the compiler also or's in BLOCK_FIELD_IS_WEAK (16)

So the Block copy/dispose helpers should only ever generate the four flag values of 3, 7, 8, and 24.

When  a __block variable is either a C++ object, an Objective-C object, or another Block then the compiler also generates copy/dispose helper functions.  Similarly to the Block copy helper, the "__block" copy helper (formerly and still a.k.a. "byref" copy helper) will do a C++ copy constructor (not a const one though!) and the dispose helper will do the destructor.  And similarly the helpers will call into the same two support functions with the same values for objects and Blocks with the additional BLOCK_BYREF_CALLER (128) bit of information supplied.

So the __block copy/dispose helpers will generate flag values of 3 or 7 for objects and Blocks respectively, with BLOCK_FIELD_IS_WEAK (16) or'ed as appropriate and always 128 or'd in, for the following set of possibilities:
	__block id                   128+3       (0x83)
	__block (^Block)             128+7       (0x87)
    __weak __block id            128+3+16    (0x93)
	__weak __block (^Block)      128+7+16    (0x97)
        

 有点长，翻译解释一下。
 
 这是给编译器提供的 API，在 TestBlock 中可以看到，编译器确实使用了这两个函数。
 
 block 可以引用 4 种不同的类型的对象，当 block 被拷贝到堆上时，需要 help，即帮助拷贝一些东西。
 1）基于 C++ 栈的对象
 2）Objective-C 对象
 3）其他 Block
 4）被 __block 修饰的变量
 
 block 的 helper 函数是编译器合成的（比如 TestBlock 中编译器写的 __main_block_copy_1() 函数），它们被用在 _Block_copy() 函数和 _Block_release() 函数中。copy helper 对基于 C++ 栈的对象调用调用 C++ 常拷贝构造函数，对其他三种对象调用 _Block_object_assign 函数。 dispose helper 对基于 C++ 栈的对象调用析构函数，对其他的三种调用 _Block_object_dispose 函数。
 
 _Block_object_assign 和 _Block_object_dispose 函数的第三个参数 flags 有可能是：
 1）BLOCK_FIELD_IS_OBJECT(3) 表示是一个对象
 2）BLOCK_FIELD_IS_BLOCK(7) 表示是一个 block
 3）BLOCK_FIELD_IS_BYREF(8) 表示是一个 byref，一个被 __block 修饰的变量；如果 __block 变量还被 __weak 修饰，则还会加上 BLOCK_FIELD_IS_WEAK(16)
 
 所以 block 的 copy/dispose helper 只会传入四种值：3，7，8，24
 
 上述的4种类型的对象都会由编译器合成 copy/dispose helper 函数，和 block 的 helper 函数类似，byref 的 copy helper 将会调用 C++ 的拷贝构造函数（不是常拷贝构造），dispose helper 则会调用析构函数。还一样的是，helpers 将会一样调用进两个支持函数中，对于对象和 block，参数值是一样的，都另外附带上 BLOCK_BYREF_CALLER (128) bit 的信息。#疑问：调用的这两个函数是啥？BLOCK_BYREF_CALLER 里究竟存的是什么？？
 
 所以 __block copy/dispose helper 函数生成 flag 的值为：对象是 3，block 是 7，带 __weak 的是 16，并且一直有 128，有下面这么几种组合：
    __block id                   128+3       (0x83)
	__block (^Block)             128+7       (0x87)
    __weak __block id            128+3+16    (0x93)
	__weak __block (^Block)      128+7+16    (0x97)
 
********************************************************/

//
// When Blocks or Block_byrefs hold objects then their copy routine helpers use this entry point
// to do the assignment.
// 当 block 和 byref 要持有对象时，它们的 copy helper 函数会调用这个函数来完成 assignment，
// 参数 destAddr 其实是一个二级指针，指向真正的目标指针
void _Block_object_assign(void *destAddr, const void *object, const int flags) {
    
    switch (os_assumes(flags & BLOCK_ALL_COPY_DISPOSE_FLAGS)) {
      case BLOCK_FIELD_IS_OBJECT: // 如果是对象
        /*******
        id object = ...;
        [^{ object; } copy];
        ********/
        // 非 GC 下的 _Block_retain_object 默认什么都不干，但在 _Block_use_RR() 中会被 Objc runtime 或者 CoreFoundation 设置 retain 函数，
        // 其中，可能会与 runtime 建立联系，操作对象的引用计数什么的
        _Block_retain_object(object);
            
        // 非 GC 下的 _Block_assign 只是使 destAddr 指向的目标指针指向 object
        _Block_assign((void *)object, destAddr);
        break;

      case BLOCK_FIELD_IS_BLOCK: // 如果是 block
        /*******
        void (^object)(void) = ...;
        [^{ object; } copy];
        ********/
        
        // 先用 _Block_copy_internal 将 block 拷贝到堆上，如果原来就在堆上，则引用计数加 1。再进行 assign。
        _Block_assign(_Block_copy_internal(object, false), destAddr);
        break;
    
      case BLOCK_FIELD_IS_BYREF | BLOCK_FIELD_IS_WEAK: // 如果是 byref
      case BLOCK_FIELD_IS_BYREF:
        /*******
         // copy the onstack __block container to the heap
         __block ... x;
         __weak __block ... x;
         [^{ x; } copy];
         ********/
        
        // 先将 byref 拷贝到堆上，如果 byref 原来就在堆上，则引用计数加 1。再 assign，使 destAddr 间接指向堆上的 byref
        _Block_byref_assign_copy(destAddr, object, flags);
        break;
        
      case BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_OBJECT:
      case BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_BLOCK: // 如果是被 __block 修饰的对象和 block
        /*******
         // copy the actual field held in the __block container
         __block id object;
         __block void (^object)(void);
         [^{ object; } copy];
         ********/

        // under manual retain release __block object/block variables are dangling(悬挂的)
        // 这里没有进行堆拷贝，直接进行 assign 了
        _Block_assign((void *)object, destAddr);
        break;

      case BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_OBJECT | BLOCK_FIELD_IS_WEAK: // 如果是被 __weak 修饰的 __block 对象和block
      case BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_BLOCK  | BLOCK_FIELD_IS_WEAK:
        /*******
         // copy the actual field held in the __block container
         __weak __block id object;
         __weak __block void (^object)(void);
         [^{ object; } copy];
         ********/

        // 在非 GC 下，_Block_assign_weak 和 _Block_assign 好像没什么区别
        _Block_assign_weak(object, destAddr);
        break;

      default:
        break;
    }
}

// When Blocks or Block_byrefs hold objects their destroy helper routines call this entry point
// to help dispose of the contents
// Used initially only for __attribute__((NSObject)) marked pointers.
// 当 block 和 byref 要 dispose 对象时，它们的 dispose helper 会调用这个函数
void _Block_object_dispose(const void *object, const int flags) {
    
    switch (os_assumes(flags & BLOCK_ALL_COPY_DISPOSE_FLAGS)) {
      case BLOCK_FIELD_IS_BYREF | BLOCK_FIELD_IS_WEAK: // 如果是 byref
      case BLOCK_FIELD_IS_BYREF:
        // get rid of the __block data structure held in a Block
        _Block_byref_release(object); // 对 byref 对象做 release 操作
        break;
            
      case BLOCK_FIELD_IS_BLOCK: // 如果是 block
        _Block_destroy(object); // 对 block 做 release 操作
        break;
            
      case BLOCK_FIELD_IS_OBJECT: // 如果是对象
        _Block_release_object(object); // 默认啥也不干，但在 _Block_use_RR() 中可能会被 Objc runtime 或者 CoreFoundation 设置一个 release 函数，里面可能会涉及到 runtime 的引用计数
        break;
            
      // 下面的这些，都有 BLOCK_BYREF_CALLER，则什么也不干
      case BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_OBJECT:
      case BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_BLOCK:
      case BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_OBJECT | BLOCK_FIELD_IS_WEAK:
      case BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_BLOCK  | BLOCK_FIELD_IS_WEAK:
        break;
      default:
        break;
    }
}
