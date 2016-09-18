/*
 * Block_private.h
 *
 * SPI for Blocks
 *
 * Copyright (c) 2008-2010 Apple Inc. All rights reserved.
 *
 * @APPLE_LLVM_LICENSE_HEADER@
 *
 */

#ifndef _BLOCK_PRIVATE_H_
#define _BLOCK_PRIVATE_H_

#include <Availability.h>
#include <AvailabilityMacros.h>
#include <TargetConditionals.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <Block.h>

#if __cplusplus
extern "C" { // 如果是 C++，就指定下面的数据结构和函数按照 C 的形式编译
#endif


// Values for Block_layout->flags to describe block objects
enum {
    BLOCK_DEALLOCATING =      (0x0001),  // runtime  正在 dealloc
    BLOCK_REFCOUNT_MASK =     (0xfffe),  // runtime  引用计数掩码，即从第 1 ~ 15 位是用来存引用计数的，第 0 位上面已经被用了
    BLOCK_NEEDS_FREE =        (1 << 24), // runtime  需要释放，即它现在在堆上
    BLOCK_HAS_COPY_DISPOSE =  (1 << 25), // compiler 是否有 copy / dispose 函数，copy 和 dispose 在 desc 中
    BLOCK_HAS_CTOR =          (1 << 26), // compiler: helpers have C++ code block 有 C++ 的构造器
    BLOCK_IS_GC =             (1 << 27), // runtime  用了 GC，这个不用管，GC 已经被淘汰
    BLOCK_IS_GLOBAL =         (1 << 28), // compiler 是否处于全局区
    BLOCK_USE_STRET =         (1 << 29), // compiler: undefined if !BLOCK_HAS_SIGNATURE
                                         //    返回值是否在栈上，如果没有签名，则它一定是 0
    BLOCK_HAS_SIGNATURE  =    (1 << 30), // compiler 是否有签名，签名是描述 block 的参数和返回值的一个字符串
    BLOCK_HAS_EXTENDED_LAYOUT=(1 << 31)  // compiler 是否有扩展布局
};

#define BLOCK_DESCRIPTOR_1 1
struct Block_descriptor_1 { // 和 TestBlock 中 desc 的结构是一样的
    uintptr_t reserved;
    uintptr_t size; // block 的大小，这个大小应该是包括 Block_layout、Block_descriptor_1、Block_descriptor_2、Block_descriptor_3 的总大小
};

#define BLOCK_DESCRIPTOR_2 1
struct Block_descriptor_2 {
    // requires BLOCK_HAS_COPY_DISPOSE 必须有 BLOCK_HAS_COPY_DISPOSE
    // copy helper 和 dispose helper
    // copy helper 用于帮助拷贝成员变量；dispose helper 用于帮助释放成员变量
    void (*copy)(void *dst, const void *src);
    void (*dispose)(const void *);
};

#define BLOCK_DESCRIPTOR_3 1
struct Block_descriptor_3 {
    // requires BLOCK_HAS_SIGNATURE
    const char *signature;  // 签名，是个 char * 字符串，描述 block 的参数和返回值
    const char *layout;     // 扩展布局。contents depend on BLOCK_HAS_EXTENDED_LAYOUT
                            // #疑问：这个变量很让人疑惑，按 _Block_layout() 函数的意思，它有可能存的是 GC layout，
                            //       扩展布局和 GC layout 是什么关系呢？？
};


// block 的 layout ，数据结构和 TestBlock 中每个 block 对应的结构体（例如 __main_block_desc_1）是一样的
// isa/flags/reserved/invoke 对应 __block_impl 结构体中的变量
// descriptor 其实是一个列表，第一个元素是 Block_descriptor_1，后面还有 Block_descriptor_2 、Block_descriptor_3
// Block_descriptor_1 中的 reserved 和 size 对应 __main_block_desc_1 中 reserved 和 Block_size 变量，
// Block_descriptor_2 中 copy 和 dispose 方法对应 __main_block_desc_1 中的 copy 和 dispose 方法
struct Block_layout {
    void *isa;
    volatile int32_t flags; // contains ref count 包括引用计数在内的 flag，详情见本文件顶部
    int32_t reserved; // 保留，不知道有没有大用
    void (*invoke)(void *, ...); // block 对应的函数指针
    struct Block_descriptor_1 *descriptor; // desc 数组，第一个元素是 Block_descriptor_1，
                                           // 后面还有 Block_descriptor_2 、Block_descriptor_3
    
    // imported variables
    // 后面跟被引入的外部变量（这和 __main_block_impl_1 也是一样的）
};


// Values for Block_byref->flags to describe __block variables
// Block_byref->flags 的值，用来描述 __block 变量
enum {
    // Byref refcount must use the same bits as Block_layout's refcount.
    // Byref 的引用计数必须和 Block_layout's 的引用计数用同样的位
    
    // BLOCK_DEALLOCATING =      (0x0001),  // runtime
    // BLOCK_REFCOUNT_MASK =     (0xfffe),  // runtime

    BLOCK_BYREF_LAYOUT_MASK =       (0xf << 28), // compiler layout 的掩码，即下面这几个 layout 在第 28~31 位
    BLOCK_BYREF_LAYOUT_EXTENDED =   (  1 << 28), // compiler 扩展布局 28~31位：0b0001
    BLOCK_BYREF_LAYOUT_NON_OBJECT = (  2 << 28), // compiler 非对象 28~31位：0b0010
    BLOCK_BYREF_LAYOUT_STRONG =     (  3 << 28), // compiler 强应用 28~31位：0b0011
    BLOCK_BYREF_LAYOUT_WEAK =       (  4 << 28), // compiler 弱引用 28~31位：0b0100
    BLOCK_BYREF_LAYOUT_UNRETAINED = (  5 << 28), // compiler 未引用 28~31位：0b0101

    BLOCK_BYREF_IS_GC =             (  1 << 27), // runtime  是否用了 GC，不用关心，GC 已经被淘汰

    BLOCK_BYREF_HAS_COPY_DISPOSE =  (  1 << 25), // compiler 是否有 copy / dispose 函数
    BLOCK_BYREF_NEEDS_FREE =        (  1 << 24), // runtime  是否需要被 free
};

struct Block_byref { // 包装被引用的外部变量的结构体，结构与 __Block_byref_outerVar_0 一致
    void *isa;
    struct Block_byref *forwarding; // 堆中的 byref 的 forwarding 指向的是自己，
                                    // 栈中的 byref 的 forwarding 一开始指向的是自己，拷贝到堆上后，指向的是堆上的 byref
                                    // forwarding 有转发的意思
    volatile int32_t flags; // contains ref count 包括引用计数在内的一些 flag
    uint32_t size; // byref 的大小，不止是 Block_byref 本身，还包括跟在后面的 Block_byref_2 和 Block_byref_3
};

struct Block_byref_2 { // 在内存中 Block_byref_2 对象是紧跟在 Block_byref 对象后面的
    // requires BLOCK_BYREF_HAS_COPY_DISPOSE
    // byref 的 copy helper
    void (*byref_keep)(struct Block_byref *dst, struct Block_byref *src);
    // byref 的 dispose helper
    void (*byref_destroy)(struct Block_byref *);
};

struct Block_byref_3 { // 在内存中 Block_byref_3 对象是紧跟在 Block_byref_2 对象后面的
    // requires BLOCK_BYREF_LAYOUT_EXTENDED
    const char *layout; // 扩展布局
};


// Extended layout encoding.

// Values for Block_descriptor_3->layout with BLOCK_HAS_EXTENDED_LAYOUT
// and for Block_byref_3->layout with BLOCK_BYREF_LAYOUT_EXTENDED

// If the layout field is less than 0x1000, then it is a compact encoding 
// of the form 0xXYZ: X strong pointers, then Y byref pointers, 
// then Z weak pointers.

// If the layout field is 0x1000 or greater, it points to a 
// string of layout bytes. Each byte is of the form 0xPN.
// Operator P is from the list below. Value N is a parameter for the operator.
// Byte 0x00 terminates the layout; remaining block data is non-pointer bytes.

// 扩展布局编码
// 是 Block_descriptor_3->layout（需要指定BLOCK_HAS_EXTENDED_LAYOUT） 和 Block_byref_3->layout（需要指定BLOCK_BYREF_LAYOUT_EXTENDED） 的值

// 如果布局域小于 0x1000，则它是一个紧凑的编码，格式是 0xXYZ，X 代表强指针，Y 代表 byref（被引用） 指针，Z 代表弱指针

// 如果布局域大于等于 0x1000，则它指向一个布局字节串，每个字节都是 0xPN 的格式
// 操作符 P 来自下面的列表，值 N 是操作符的一个参数
// 字节 0x00 是布局是终止符；剩下的 block 的数据是 non-pointer 字节
    
enum {
    BLOCK_LAYOUT_ESCAPE = 0, // N=0 halt, rest is non-pointer. N!=0 reserved. N=0 停止，剩余部分是 non-pointer，N!=0 保留
    BLOCK_LAYOUT_NON_OBJECT_BYTES = 1,    // N bytes non-objects 非对象的字节
    BLOCK_LAYOUT_NON_OBJECT_WORDS = 2,    // N words non-objects 非对象的words
    BLOCK_LAYOUT_STRONG           = 3,    // N words strong pointers 强指针
    BLOCK_LAYOUT_BYREF            = 4,    // N words byref pointers byref 指针
    BLOCK_LAYOUT_WEAK             = 5,    // N words weak pointers 弱指针
    BLOCK_LAYOUT_UNRETAINED       = 6,    // N words unretained pointers unretained 指针
                                          //  （与 weak 的区别应该是不会自动置为nil）
    BLOCK_LAYOUT_UNKNOWN_WORDS_7  = 7,    // N words, reserved 保留
    BLOCK_LAYOUT_UNKNOWN_WORDS_8  = 8,    // N words, reserved 保留
    BLOCK_LAYOUT_UNKNOWN_WORDS_9  = 9,    // N words, reserved 保留
    BLOCK_LAYOUT_UNKNOWN_WORDS_A  = 0xA,  // N words, reserved 保留
    BLOCK_LAYOUT_UNUSED_B         = 0xB,  // unspecified, reserved 保留
    BLOCK_LAYOUT_UNUSED_C         = 0xC,  // unspecified, reserved 保留
    BLOCK_LAYOUT_UNUSED_D         = 0xD,  // unspecified, reserved 保留
    BLOCK_LAYOUT_UNUSED_E         = 0xE,  // unspecified, reserved 保留
    BLOCK_LAYOUT_UNUSED_F         = 0xF,  // unspecified, reserved 保留
};


// Runtime support functions used by compiler when generating copy/dispose helpers

// Values for _Block_object_assign() and _Block_object_dispose() parameters
// _Block_object_assign() 和 _Block_object_dispose() 函数中最后一个参数 flags 的值，
// 应该是标识了 src 是什么类型的，对象 / block / byref / 弱引用 ...
enum {
    // see function implementation for a more complete description of these fields and combinations
    BLOCK_FIELD_IS_OBJECT   =  3,  // id, NSObject, __attribute__((NSObject)), block, ... 是对象
    
    BLOCK_FIELD_IS_BLOCK    =  7,  // a block variable 是一个 block 变量
    
    BLOCK_FIELD_IS_BYREF    =  8,  // the on stack structure holding the __block variable
                                   // 栈上的数据结构持有 __block 修饰的变量
    
    BLOCK_FIELD_IS_WEAK     = 16,  // declared __weak, only used in byref copy helpers
                                   // 被 __weak 修饰过的指针，只在 byref 的 copy helper 中被用到
    
    BLOCK_BYREF_CALLER      = 128, // called from __block (byref) copy/dispose support routines.
                                   // 被 __block (byref) copy/dispose support routines 调用
                                   // 详情见 _Block_object_assign()，这个东西有点让人摸不着头脑，琢磨不透干嘛用的
                                   // 反正被 __block 修饰的对象或 block 的 flag 中就会带有 BLOCK_BYREF_CALLER
};

enum {
    // 包括了上面所有的值
    BLOCK_ALL_COPY_DISPOSE_FLAGS = 
        BLOCK_FIELD_IS_OBJECT | BLOCK_FIELD_IS_BLOCK | BLOCK_FIELD_IS_BYREF |
        BLOCK_FIELD_IS_WEAK | BLOCK_BYREF_CALLER
};

// Runtime entry point called by compiler when assigning objects inside copy helper routines
// runtime 入口。在 copy helper 程序中 assign 对象时，被编译器调用。见 TestBlock 中的 __main_block_copy_1() 函数，应该是这么个意思。
// BLOCK_FIELD_IS_BYREF 只在 block copy helpers 函数中被用到（作为 flags）。估计就是 __main_block_copy_1() 这样的函数
BLOCK_EXPORT void _Block_object_assign(void *destAddr, const void *object, const int flags);
    // BLOCK_FIELD_IS_BYREF is only used from within block copy helpers


// runtime entry point called by the compiler when disposing of objects inside dispose helper routine
// runtime 入口。在 dispose helper 程序中 dispose 对象时，被编译器调用。见 TestBlock 中的 __main_block_dispose_1() 函数
BLOCK_EXPORT void _Block_object_dispose(const void *object, const int flags);


// Other support functions

// runtime entry to get total size of a closure
// 取得 block 的完整大小
BLOCK_EXPORT size_t Block_size(void *aBlock);

// indicates whether block was compiled with compiler that sets the ABI related metadata bits
// 指定 block 是否被编译器设置了 ABI 关联元数据 bits
BLOCK_EXPORT bool _Block_has_signature(void *aBlock)
    __OSX_AVAILABLE_STARTING(__MAC_10_7, __IPHONE_4_3);

// returns TRUE if return value of block is on the stack, FALSE otherwise
// 如果 block 的返回值在栈上，则返回 TRUE，反之返回 FALSE
BLOCK_EXPORT bool _Block_use_stret(void *aBlock)
    __OSX_AVAILABLE_STARTING(__MAC_10_7, __IPHONE_4_3);

// Returns a string describing the block's parameter and return types.
// The encoding scheme is the same as Objective-C @encode.
// Returns NULL for blocks compiled with some compilers.
// 取得 block 的签名字符串，签名字符串描述了 block 的参数和返回值
// 编码规则和 objective-c 中的 @encode 是一样的
BLOCK_EXPORT const char * _Block_signature(void *aBlock)
    __OSX_AVAILABLE_STARTING(__MAC_10_7, __IPHONE_4_3);

// Returns a string describing the block's GC layout.
// This uses the GC skip/scan encoding.
// May return NULL.
// 返回一个描述 block 的 GC layout 的字符串。它使用了 GC 的 skip/scan 编码。
// 可能返回 NULL
BLOCK_EXPORT const char * _Block_layout(void *aBlock)
    __OSX_AVAILABLE_STARTING(__MAC_10_7, __IPHONE_4_3);

// Returns a string describing the block's layout.
// This uses the "extended layout" form described above.
// May return NULL.
// 返回 block 的扩展布局字符串
// 如果没有扩展布局，则返回 NULL，如果指定有扩展布局，但扩展布局为 NULL，则返回的是 "" 空字符串
BLOCK_EXPORT const char * _Block_extended_layout(void *aBlock)
    __OSX_AVAILABLE_STARTING(__MAC_10_8, __IPHONE_7_0);

// Callable only from the ARR weak subsystem while in exclusion zone（禁区）
// 尝试 retain
BLOCK_EXPORT bool _Block_tryRetain(const void *aBlock)
    __OSX_AVAILABLE_STARTING(__MAC_10_7, __IPHONE_4_3);

// Callable only from the ARR weak subsystem while in exclusion zone
// block 是否正在 dealloc
BLOCK_EXPORT bool _Block_isDeallocating(const void *aBlock)
    __OSX_AVAILABLE_STARTING(__MAC_10_7, __IPHONE_4_3);


// the raw data space for runtime classes for blocks
// class+meta used for stack, malloc, and collectable based blocks
BLOCK_EXPORT void * _NSConcreteMallocBlock[32]
    __OSX_AVAILABLE_STARTING(__MAC_10_6, __IPHONE_3_2);
BLOCK_EXPORT void * _NSConcreteAutoBlock[32]
    __OSX_AVAILABLE_STARTING(__MAC_10_6, __IPHONE_3_2);
BLOCK_EXPORT void * _NSConcreteFinalizingBlock[32]
    __OSX_AVAILABLE_STARTING(__MAC_10_6, __IPHONE_3_2);
BLOCK_EXPORT void * _NSConcreteWeakBlockVariable[32]
    __OSX_AVAILABLE_STARTING(__MAC_10_6, __IPHONE_3_2);
// declared in Block.h
// BLOCK_EXPORT void * _NSConcreteGlobalBlock[32];
// BLOCK_EXPORT void * _NSConcreteStackBlock[32];


    
    
// -----------  下面是 GC 中才用到的函数，不必深究  ---------------------------------------
    

// the intercept routines that must be used under GC
BLOCK_EXPORT void _Block_use_GC( void *(*alloc)(const unsigned long, const bool isOne, const bool isObject),
                                  void (*setHasRefcount)(const void *, const bool),
                                  void (*gc_assign_strong)(void *, void **),
                                  void (*gc_assign_weak)(const void *, void *),
                                  void (*gc_memmove)(void *, void *, unsigned long));

// earlier version, now simply transitional
BLOCK_EXPORT void _Block_use_GC5( void *(*alloc)(const unsigned long, const bool isOne, const bool isObject),
                                  void (*setHasRefcount)(const void *, const bool),
                                  void (*gc_assign_strong)(void *, void **),
                                  void (*gc_assign_weak)(const void *, void *));

BLOCK_EXPORT void _Block_use_RR( void (*retain)(const void *),
                                 void (*release)(const void *));

struct Block_callbacks_RR {
    size_t  size;                   // size == sizeof(struct Block_callbacks_RR)
    void  (*retain)(const void *);
    void  (*release)(const void *);
    void  (*destructInstance)(const void *);
};
typedef struct Block_callbacks_RR Block_callbacks_RR;

BLOCK_EXPORT void _Block_use_RR2(const Block_callbacks_RR *callbacks);

// make a collectable GC heap based Block.  Not useful under non-GC.
BLOCK_EXPORT void *_Block_copy_collectable(const void *aBlock);

// thread-unsafe diagnostic
BLOCK_EXPORT const char *_Block_dump(const void *block);


// Obsolete  废弃的

// first layout
struct Block_basic {
    void *isa;
    int Block_flags;  // int32_t
    int Block_size; // XXX should be packed into Block_flags
    void (*Block_invoke)(void *);
    void (*Block_copy)(void *dst, void *src);  // iff BLOCK_HAS_COPY_DISPOSE
    void (*Block_dispose)(void *);             // iff BLOCK_HAS_COPY_DISPOSE
    //long params[0];  // where const imports, __block storage references, etc. get laid down
} __attribute__((deprecated));


#if __cplusplus
}
#endif


#endif
