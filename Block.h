/*
 *  Block.h
 *
 * Copyright (c) 2008-2010 Apple Inc. All rights reserved.
 *
 * @APPLE_LLVM_LICENSE_HEADER@
 *
 */

#ifndef _Block_H_
#define _Block_H_

#if !defined(BLOCK_EXPORT)
#   if defined(__cplusplus)
#       define BLOCK_EXPORT extern "C" 
#   else
#       define BLOCK_EXPORT extern
#   endif
#endif

#include <Availability.h>
#include <TargetConditionals.h>

#if __cplusplus
extern "C" { // 如果是 C++，就将下面几个函数编译成 C 的形式，即不加那些重整字符
#endif

// Create a heap based copy of a Block or simply add a reference to an existing one.
// This must be paired with Block_release to recover memory, even when running
// under Objective-C Garbage Collection.
// 拷贝 block，
// 如果原来就在堆上，就将引用计数加 1;
// 如果原来在栈上，会拷贝到堆上，引用计数初始化为 1，并且会调用 copy helper 方法（如果存在的话）；
// 如果 block 在全局区，不用加引用计数，也不用拷贝，直接返回 block 本身
// 返回值是拷贝后的 block 的地址
BLOCK_EXPORT void *_Block_copy(const void *aBlock)
    __OSX_AVAILABLE_STARTING(__MAC_10_6, __IPHONE_3_2);

// Lose the reference, and if heap based and last reference, recover the memory
// 对 block 做 release 操作。
// block 在堆上，才需要 release，在全局区和栈区都不需要 release.
// 先将引用计数减 1，如果引用计数减到了 0，就将 block 销毁
BLOCK_EXPORT void _Block_release(const void *aBlock)
    __OSX_AVAILABLE_STARTING(__MAC_10_6, __IPHONE_3_2);


// Used by the compiler. Do not call this function yourself.
// runtime 入口。
// 当 block 和 byref 要持有对象时，它们的 copy helper 函数会调用这个函数来完成 assignment
BLOCK_EXPORT void _Block_object_assign(void *, const void *, const int)
    __OSX_AVAILABLE_STARTING(__MAC_10_6, __IPHONE_3_2);

// Used by the compiler. Do not call this function yourself.
// runtime 入口。在 dispose helper 程序中 dispose 对象时，被编译器调用。
BLOCK_EXPORT void _Block_object_dispose(const void *, const int)
    __OSX_AVAILABLE_STARTING(__MAC_10_6, __IPHONE_3_2);

// Used by the compiler. Do not use these variables yourself.
BLOCK_EXPORT void * _NSConcreteGlobalBlock[32]
    __OSX_AVAILABLE_STARTING(__MAC_10_6, __IPHONE_3_2);
    
// 在 TestBlock 中，block 被赋予的最初类型，即它们的 isa 都是 _NSConcreteStackBlock
// 无论是否引用了外部变量
BLOCK_EXPORT void * _NSConcreteStackBlock[32]
    __OSX_AVAILABLE_STARTING(__MAC_10_6, __IPHONE_3_2);

// 另有几种 block 类型声明在 Block_pivate.h 中
    
#if __cplusplus
}
#endif

// Type correct macros

#define Block_copy(...) ((__typeof(__VA_ARGS__))_Block_copy((const void *)(__VA_ARGS__)))
#define Block_release(...) _Block_release((const void *)(__VA_ARGS__))


#endif
