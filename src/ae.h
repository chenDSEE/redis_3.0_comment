/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __AE_H__
#define __AE_H__

/*
 * 事件执行状态
 */
// 成功
#define AE_OK 0
// 出错
#define AE_ERR -1

/*
 * 文件事件状态
 */
// 未设置
#define AE_NONE 0
// 可读
#define AE_READABLE 1
// 可写
#define AE_WRITABLE 2

/*
 * 时间处理器的执行 flags
 */
// 文件事件
#define AE_FILE_EVENTS 1
// 时间事件
#define AE_TIME_EVENTS 2
// 所有事件
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
// 不阻塞，也不进行等待（控制 epoll 系统调用是由阻塞等待）
#define AE_DONT_WAIT 4

/*
 * 决定时间事件是否要持续执行的 flag
 */
#define AE_NOMORE -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

/*
 * 事件处理器状态
 */
struct aeEventLoop;

/* Types and data structures 
 *
 * 事件接口
 */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure
 *
 * 文件事件结构
 * IO 复用器（如：epoll-instance），在 user-space 管理内部 fd 的结构
 * 将每一个 fd 的相应 callback、mask、data 都记录起来。这样在 callback 的时候，
 * 才能够根据体哦同调用返回的 fd 开展下一步的行动（根据返回的 mask + 记录的 mask + callback）
 */
typedef struct aeFileEvent {

    // 监听事件类型掩码，初始状态为 AE_NONE
    // 值可以是 AE_READABLE 或 AE_WRITABLE ，
    // 或者 AE_READABLE | AE_WRITABLE
    int mask; /* one of AE_(READABLE|WRITABLE) */

    // 读事件处理器（每一个已经完成链接的 client，所使用的 read-callback，例如：readQueryFromClient()）
    aeFileProc *rfileProc;

    // 写事件处理器
    aeFileProc *wfileProc;

    // 多路复用库的私有数据
    void *clientData;

} aeFileEvent;

/* Time event structure
 *
 * 时间事件结构
 */
typedef struct aeTimeEvent {

    // 时间事件的唯一标识符
    long long id; /* time event identifier. */

    // 事件的到达时间
    long when_sec; /* seconds */
    long when_ms; /* milliseconds */

    // 事件处理函数
    aeTimeProc *timeProc;

    // 事件释放函数
    aeEventFinalizerProc *finalizerProc;

    // 多路复用库的私有数据
    void *clientData;

    // 指向下个时间事件结构，形成链表
    struct aeTimeEvent *next;

} aeTimeEvent;

/* A fired event
 *
 * 已就绪事件，统一的放回方式（这样上层代码能够统一实现进一步分发的代码）
 */
typedef struct aeFiredEvent {

    // 已就绪文件描述符
    int fd;

    // 事件类型掩码，
    // 值可以是 AE_READABLE 或 AE_WRITABLE
    // 或者是两者的或
    int mask;

} aeFiredEvent;

/* State of an event based program 
 *
 * 事件处理器的状态
 * 这个 struct 里面的数据，都是每一个 epoll-loop 实例对象独自拥有的，并不是进程中全部 epoll-loop 实例对象都有的
 * 
 * 为了兼容不同的 IO 复用函数，实际上这个 aeEventLoop 就是一个中间层，兼容不同的底层 IO 复用函数
 */
typedef struct aeEventLoop {

    // 目前已注册（正在使用）的最大描述符（IO event，不包含 timer-event）
    // 用于 resize event-loop 能够管理的事件 pool 大小（每次 create_event 的时候，都会看看需不需要更新）
    int maxfd;   /* highest file descriptor currently registered */

    // 目前已追踪的最大描述符
    int setsize; /* max number of file descriptors tracked */

    // 用于生成时间事件 id（相当于记录了 timer-event 的 max_id）
    long long timeEventNextId;

    // 最后一次执行时间事件的时间
    time_t lastTime;     /* Used to detect system clock skew */

    // 已注册的文件事件（统计起来备查）
    // NOTE: 因为 fd 的具体数字是每一个进程独立的，都是从 0 开始的（0，1，2 默认用掉了），
    //       所以是可以采用 events[fd] 这样的方法，实现 O(1) 的随机访问速度
    // 用途：1) aeApiAddEvent() 的时候，避免重复操作，需要在这里面检查当前事件 fd 是否已经在 epoll 红黑树里面了
    aeFileEvent *events; /* Registered events */

    // 已就绪的文件事件
    // epoll_wait 返回的已就绪事件，从这个返回给上层调用（统一返回就绪事件的 fd + mask，表明那个 fd 的什么事件就绪了）
    aeFiredEvent *fired; /* Fired events */

    // 时间事件(挂载为单向链表，不会进行排序处理)
    // redis 对于 timer 处理的事件，并没有很严格的精确计时要求，所以并不会将 TIMEOUT_EVENT 注册进 epoll-instance 里面
    aeTimeEvent *timeEventHead;

    // 事件处理器的开关
    int stop;

    // 多路复用库的私有数据（每一个 epoll-loop 实例会有一个独立的 void *apidata）
    // epoll 情况下是：ae_epoll.c 的 struct aeApiState
    // 因为不同的 IO 复用，会需要不同的配套变量，所以采用这种方式来兼容不同的 IO 复用方案
    // 不同的 IO 复用方案文件里面都会定义自己的 aeApiState，然后这个 apidate 只能出现在相应的 .c 文件里面
    // 不对外公开（因为上层调用并不知道该采用什么类型的 struct 去解引用这个 void *apidata 通用指针）
    void *apidata; /* This is used for polling API specific data */

    // 在处理事件前要执行的函数
    aeBeforeSleepProc *beforesleep;

} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);

#endif
