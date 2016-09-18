//
//  main.m
//  TestBlock
//
//  Created by Allen on 16/9/14.
//  Copyright © 2016年 Allen. All rights reserved.
//

#import <Foundation/Foundation.h>


typedef void(^MyBlock)(void); // 会被编译器处理成 typedef void(*MyBlock)(void); 看上去像函数指针，其实不是函数，
                              // 而是 impl 结构体实例的指针类型，中间为了迷惑我们，做了强制转换
                              // 比如 MyBlock block_1 = ((void (*)())&__main_block_impl_0((void *)__main_block_func_0, &__main_block_desc_0_DATA));

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        // 没有引入外部变量的block的类型是__NSGlobalBlock__
        MyBlock block_1 = ^{
            printf("没有引入外部变量");
        };
        NSLog(@"没有引入外部变量的block类型是：%@", [block_1 class]); // __NSGlobalBlock__
        
        // 引用了外部变量
        __block int outerVar = 1;
        MyBlock block_2 = ^{
            outerVar++; // 引用外部变量
            printf("引入了外部变量");
        };
        NSLog(@"引用了外部变量的block类型是：%@", [block_2 class]); // __NSMallocBlock__
        
        // 我们发现在ARC模式下，打印出来的结果并不是NSStackBlock这个类型，很多人以为在ARC模式下block的类型只有NSGlobalBlock和NSMallocBlock两种，其实这种观点是错误的。在ARC情况下，生成的block也是NSStackBlock，只是当赋值给strong对象时，系统会主动对其进行copy
        
        NSLog(@"没有赋给strong对象的block类型是：%@", [^{
            outerVar++;
            printf("没有赋给strong对象的block，引入了外部变量");
        } class]); // __NSStackBlock__
        
        void (^block)()=^{
            outerVar++;
            printf("赋给strong对象的block，引入了外部变量");
        };
        NSLog(@"赋给strong对象的block类型是：%@",[block class]); // __NSMallocBlock__
        
        // 如果NSStackBlock需要在其作用域外部使用的时候，在MRC的模式下需要手动将其copy到堆上，NSMallocBlock支持retain、release，会对其引用计数＋1或－1，copy不会生成新的对象，只是增加了一次引用，类似retain；而在ARC模式下会自动对其进行copy，不需要自己手动去管理，尽可能使用ARC
    }
    return 0;
}

