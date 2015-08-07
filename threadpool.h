/**//**
 * threadpool.h
 *
 * This file declares the functionality associated with
 * your implementation of a threadpool.
 * 线程池的实现
 */

#ifndef __threadpool_h__
#define __threadpool_h__

#ifdef __cplusplus
extern "C" {
#endif

// maximum number of threads allowed in a pool
//最大线程数在池中
#define MAXT_IN_POOL 200

// You must hide the internal details of the threadpool
// structure from callers, thus declare threadpool of type "void".
// In threadpool.c, you will use type conversion to coerce
// variables of type "threadpool" back and forth to a
// richer, internal type.  (See threadpool.c for details.)
//为了向使用者隐藏线程池的内部结构细节，将threadpool声明为void*
//在threadpool.c中可以用类型转换转换回来，细节请看threadpool.c

typedef void *threadpool;

// "dispatch_fn" declares a typed function pointer.  A
// variable of type "dispatch_fn" points to a function
// with the following signature:
// 
//     void dispatch_function(void *arg);
//dispatch_fn定义函数指针

typedef void (*dispatch_fn)(void *);

/**//**
 * create_threadpool creates a fixed-sized thread
 * pool.  If the function succeeds, it returns a (non-NULL)
 * "threadpool", else it returns NULL.
 */
//创建固定大小的线程池，如果创建成功，函数返回非空值，否则返回空值
threadpool create_threadpool(int num_threads_in_pool);


/**//**
 * dispatch sends a thread off to do some work.  If
 * all threads in the pool are busy, dispatch will
 * block until a thread becomes free and is dispatched.
 * 
 * Once a thread is dispatched, this function returns
 * immediately.
 * 
 * The dispatched thread calls into the function
 * "dispatch_to_here" with argument "arg".
 */
//分配一个线程完成请求，如果池中所有线程都非空闲，调度程序阻塞直到有线程空闲并马上调度
//一旦一个线程被调度，函数马上返回
//这个线程调用函数dispathch_to_here，arg作为函数参数
int dispatch_threadpool(threadpool from_me, dispatch_fn dispatch_to_here,
          void *arg);

/**//**
 * destroy_threadpool kills the threadpool, causing
 * all threads in it to commit suicide, and then
 * frees all the memory associated with the threadpool.
 */
//销毁线程池，使池中所有线程自杀，之后释放所有相关内存
void destroy_threadpool(threadpool destroyme);

#ifdef __cplusplus
}
#endif

#endif