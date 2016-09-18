/*
 * data.c
 * libclosure
 *
 * Copyright (c) 2008-2010 Apple Inc. All rights reserved.
 *
 * @APPLE_LLVM_LICENSE_HEADER@
 *
 */

/********************
NSBlock support

We allocate space and export a symbol to be used as the Class for the on-stack and malloc'ed copies until ObjC arrives on the scene.  These data areas are set up by Foundation to link in as real classes post facto.

We keep these in a separate file so that we can include the runtime code in test subprojects but not include the data so that compiled code that sees the data in libSystem doesn't get confused by a second copy.  Somehow these don't get unified in a common block.
 
 我们为栈上和堆上的 block 开辟了一块空间作为它们的 class，直到 Objc 到达现场（Objc到达现场，这句不好理解，看字面意思好像是 Objc4 库会做一些操作，但是我在 Objc4 库中并没有找到相关的代码，所以很让人疑惑）。这些数据区会被 Foundation 库 set up 后链接作为真正的类。即它们现在只被开辟内存，但是里面全都是0，没有数据。Foundation 库会填充数据，使之成为真正可以用的类。
**********************/

void * _NSConcreteStackBlock[32] = { 0 };
void * _NSConcreteMallocBlock[32] = { 0 };
void * _NSConcreteAutoBlock[32] = { 0 };
void * _NSConcreteFinalizingBlock[32] = { 0 };
void * _NSConcreteGlobalBlock[32] = { 0 };

// 在 _Block_byref_assign_copy() 中用到过，被 __weak 修饰的 byref 会被设置 isa 是 _NSConcreteWeakBlockVariable
void * _NSConcreteWeakBlockVariable[32] = { 0 };
