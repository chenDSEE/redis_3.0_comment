/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

// NOTE: 参考资料
// https://redis.io/topics/clients
// https://redis.io/topics/protocol

#include "redis.h"
#include <sys/uio.h>
#include <math.h>

static void setProtocolError(redisClient *c, int pos);

/* To evaluate the output buffer size of a client we need to get size of
 * allocated objects, however we can't used zmalloc_size() directly on sds
 * strings because of the trick they use to work (the header is before the
 * returned pointer), so we use this helper function. */
// 计算输出缓冲区的大小 
size_t zmalloc_size_sds(sds s) {
    return zmalloc_size(s-sizeof(struct sdshdr));
}

/* Return the amount of memory used by the sds string at object->ptr
 * for a string object. */
// 返回 object->ptr 所指向的字符串对象所使用的内存数量。
size_t getStringObjectSdsUsedMemory(robj *o) {
    redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
    switch(o->encoding) {
    case REDIS_ENCODING_RAW: return zmalloc_size_sds(o->ptr);
    case REDIS_ENCODING_EMBSTR: return sdslen(o->ptr);
    default: return 0; /* Just integer encoding for now. */
    }
}

/*
 * 回复内容复制函数
 */
void *dupClientReplyValue(void *o) {
    incrRefCount((robj*)o);
    return o;
}

/*
 * 订阅模式对比函数
 */
int listMatchObjects(void *a, void *b) {
    return equalStringObjects(a,b);
}

/*
 * 创建一个新客户端
 * 参数 fd 是进行监听的 listen-socket-fd accpet() client 连接之后，所创建处理的一个 socket
 */
redisClient *createClient(int fd) {

    // 分配空间
    redisClient *c = zmalloc(sizeof(redisClient));

    /* passing -1 as fd it is possible to create a non connected client.
     * This is useful since all the Redis commands needs to be executed
     * in the context of a client. When commands are executed in other
     * contexts (for instance a Lua script) we need a non connected client. */
    // TODO: 走一遍 lua 的过程，为什么要用伪终端？有什么好处？通过这种方式来接入相应的底层 api？产生一个干净的 context ？
    // 当 fd 不为 -1 时，创建带网络连接的客户端
    // 如果 fd 为 -1 ，那么创建无网络连接的伪客户端
    // 因为 Redis 的命令必须在客户端的上下文中使用，所以在执行 Lua 环境中的命令时
    // 需要用到这种伪终端
    if (fd != -1) {
        // 非阻塞
        anetNonBlock(NULL,fd);
        // 禁用 Nagle 算法
        anetEnableTcpNoDelay(NULL,fd);
        // 设置 keep alive
        if (server.tcpkeepalive)
            anetKeepAlive(NULL,fd,server.tcpkeepalive);
        // 绑定读事件到具体的事件 epoll-instance，也就是 server.el （开始接收命令请求）
        // 很显然，所谓的 event 根部不需要跟这一层函数打交道，真正的 event 仅仅是 epoll 配套的概念
        // 所以相应的 epoll-event 很自然的是让 aeCreateFileEvent() 这个函数直接内部完成创建、初始化（而不是在这里创建完成，并设置好相应 callback、初始化）
        if (aeCreateFileEvent(server.el,fd,AE_READABLE,
            readQueryFromClient, c) == AE_ERR)
        {
            close(fd);
            zfree(c);
            return NULL;
        }
    }

    // 初始化各个属性

    // 默认数据库
    selectDb(c,0);
    // 套接字
    c->fd = fd;
    // 名字
    c->name = NULL;
    // 回复缓冲区的偏移量
    c->bufpos = 0;

    // TODO:(DONE) 这几个缓冲区装的都是一些什么？为什么这个要初始化为 sdsempty()
    // 装的是从 socket 中 read() 出来的原始 RESP 协议内容，所以才会使用 sds 的方式
    // TODO: 既然发送过来的 RESP 内容长度总是未知的，为什么直接来一个 sdsempty() ? 
    //       创建出来的 sds-header 看起来毫无意义，sdsempty() 是不会预留字符串空间的，
    //       没有人能保证日后还会有内存空间放 RESP 协议的内容！
    // 查询缓冲区
    c->querybuf = sdsempty();
    // 查询缓冲区峰值
    c->querybuf_peak = 0;
    // 命令请求的类型
    c->reqtype = 0;
    // 命令参数数量
    c->argc = 0;
    // 命令参数
    c->argv = NULL;
    // 当前执行的命令和最近一次执行的命令
    c->cmd = c->lastcmd = NULL;
    // 查询缓冲区中未读入的命令内容数量
    c->multibulklen = 0;
    // 读入的参数的长度
    c->bulklen = -1;
    // 已发送字节数
    c->sentlen = 0;
    // 状态 FLAG
    c->flags = 0;
    // 创建时间和最后一次互动时间
    c->ctime = c->lastinteraction = server.unixtime;
    // 认证状态
    c->authenticated = 0;
    // 复制状态
    c->replstate = REDIS_REPL_NONE;
    // 复制偏移量
    c->reploff = 0;
    // 通过 ACK 命令接收到的偏移量
    c->repl_ack_off = 0;
    // 通过 ACK 命令接收到偏移量的时间
    c->repl_ack_time = 0;
    // 客户端为 slave 时使用，记录了 slave 所使用的端口号
    c->slave_listening_port = 0;
    // 回复链表
    c->reply = listCreate();
    // 回复链表的字节量
    c->reply_bytes = 0;
    // 回复缓冲区大小达到软限制的时间
    c->obuf_soft_limit_reached_time = 0;
    // 回复链表的释放和复制函数
    listSetFreeMethod(c->reply,decrRefCountVoid);   // 彻底释放拥有的整个 list-robj，而不是一直占着头节点不放
    listSetDupMethod(c->reply,dupClientReplyValue);
    // 阻塞类型
    c->btype = REDIS_BLOCKED_NONE;
    // 阻塞超时
    c->bpop.timeout = 0;
    // 造成客户端阻塞的列表键
    c->bpop.keys = dictCreate(&setDictType,NULL);
    // 在解除阻塞时将元素推入到 target 指定的键中
    // BRPOPLPUSH 命令时使用
    c->bpop.target = NULL;
    c->bpop.numreplicas = 0;
    c->bpop.reploffset = 0;
    c->woff = 0;
    // 进行事务时监视的键
    c->watched_keys = listCreate();
    // 订阅的频道和模式
    c->pubsub_channels = dictCreate(&setDictType,NULL);
    c->pubsub_patterns = listCreate();
    c->peerid = NULL;
    listSetFreeMethod(c->pubsub_patterns,decrRefCountVoid);
    listSetMatchMethod(c->pubsub_patterns,listMatchObjects);
    // 如果不是伪客户端，那么添加到服务器的客户端链表中
    if (fd != -1) listAddNodeTail(server.clients,c);
    // 初始化客户端的事务状态
    initClientMultiState(c);

    // 返回客户端
    return c;
}

/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * 这个函数在每次向客户端发送数据时都会被调用。函数的行为如下：
 *
 * If the client should receive new data (normal clients will), the function
 * returns REDIS_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * 当客户端可以接收新数据时（通常情况下都是这样），函数返回 REDIS_OK ，
 * 并将写处理器（write handler）安装到事件循环中，
 * 这样当套接字可写时，新数据就会被写入。
 *
 * If the client should not receive new data, because it is a fake client,
 * a master, a slave not yet online, or because the setup of the write handler
 * failed, the function returns REDIS_ERR.
 *
 * 对于那些不应该接收新数据的客户端，
 * 比如伪客户端、 master 以及 未 ONLINE 的 slave ，
 * 或者写处理器安装失败时，
 * 函数返回 REDIS_ERR 。
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns REDIS_ERR no
 * data should be appended to the output buffers. 
 *
 * 通常在每个回复被创建时调用，如果函数返回 REDIS_ERR ，
 * 那么没有数据会被追加到输出缓冲区。
 */
int prepareClientToWrite(redisClient *c) {

    // LUA 脚本环境所使用的伪客户端总是可写的
    if (c->flags & REDIS_LUA_CLIENT) return REDIS_OK;
    
    // 客户端是 master 并且不接受查询，
    // 那么它是不可写的，出错
    if ((c->flags & REDIS_MASTER) &&
        !(c->flags & REDIS_MASTER_FORCE_REPLY)) return REDIS_ERR;

    // 无连接的伪客户端总是不可写的
    if (c->fd <= 0) return REDIS_ERR; /* Fake client */

    // 一般情况，为客户端套接字安装写处理器到事件循环
    if (c->bufpos == 0 && listLength(c->reply) == 0 &&
        (c->replstate == REDIS_REPL_NONE ||
         c->replstate == REDIS_REPL_ONLINE) &&
        aeCreateFileEvent(server.el, c->fd, AE_WRITABLE,
        sendReplyToClient, c) == AE_ERR) return REDIS_ERR;
        // 仅仅是要求 epoll 看看这个 c->fd 是否可写，可以的话，那就进行 callback 回调
        //（避免直接调用 write() 因为此时此刻未必可以立即写，所以采用这种 write 就绪之后才写的异步方式，
        //  效率会更好，避免死等）

    return REDIS_OK;
}

/* Create a duplicate of the last object in the reply list when
 * it is not exclusively owned by the reply list. */
// 当回复列表中的最后一个对象并非属于回复的一部分时
// 创建该对象的一个复制品
// IfNeeded 是指引用计数超过 1 的情况，因为本质上是想要利用 reply-list 作为临时使用的 buf，
// 所以要在一个 sds 里面尽可能多的装载数据（REDIS_REPLY_CHUNK_BYTES），所以有可能会将多个短的 sds-obj 拼接成一个 sds-obj
// 也不必担心这样拼接多个 sds-obj，会不会影响接受方的解析，不会，因为理论上一个 CMD 对应的协议内容向 sds-obj 转换是原子的，不可中断的
// 所以，即使发生 sds-obj 的拼接也不用担心 CMD 对应的协议内容发生问题
robj *dupLastObjectIfNeeded(list *reply) {
    robj *new, *cur;
    listNode *ln;
    redisAssert(listLength(reply) > 0);
    ln = listLast(reply);
    cur = listNodeValue(ln);
    if (cur->refcount > 1) {
        // refcount > 1 就是 if-needed 的情况，因为 reply-list 里面的 sds-obj 可能会进行多个 sds-obj 的拼接
        new = dupStringObject(cur);
        decrRefCount(cur);
        listNodeValue(ln) = new;
    }
    return listNodeValue(ln);
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */

/*
 * 尝试将回复添加到 c->buf 中
 */
int _addReplyToBuffer(redisClient *c, char *s, size_t len) {
    size_t available = sizeof(c->buf)-c->bufpos;

    // 正准备关闭客户端，无须再发送内容
    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return REDIS_OK;

    /* If there already are entries in the reply list, we cannot
     * add anything more to the static buffer. */
    // 回复链表里已经有内容，再添加内容到 c->buf 里面就是错误了
    // 因为到了要使用 list 的地步，就意味着 buf 已经满了
    if (listLength(c->reply) > 0) return REDIS_ERR;

    /* Check that the buffer has enough space available for this string. */
    // 空间必须满足
    if (len > available) return REDIS_ERR;

    // 复制内容到 c->buf 里面
    memcpy(c->buf+c->bufpos,s,len);
    c->bufpos+=len;

    return REDIS_OK;
}

/*
 * 将回复对象（一个 SDS ）添加到 c->reply 回复链表中
 */
void _addReplyObjectToList(redisClient *c, robj *o) {
    robj *tail;

    // 客户端即将被关闭，无须再发送回复
    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

    // 链表中无缓冲块，直接将对象追加到链表中
    if (listLength(c->reply) == 0) {
        incrRefCount(o);
        listAddNodeTail(c->reply,o);

        // 链表中已有缓冲块，尝试将回复添加到块内
        // 如果当前的块不能容纳回复的话，那么新建一个块
        c->reply_bytes += getStringObjectSdsUsedMemory(o);
    } else {

        // 取出表尾的 SDS
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        // 如果表尾 SDS 的已用空间加上对象的长度，小于 REDIS_REPLY_CHUNK_BYTES
        // 那么将新对象的内容拼接到表尾 SDS 的末尾
        if (tail->ptr != NULL &&
            tail->encoding == REDIS_ENCODING_RAW &&
            sdslen(tail->ptr)+sdslen(o->ptr) <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply); // 可能返回新的 sds-obj（专门用来进行拼接）
            // 拼接
            tail->ptr = sdscatlen(tail->ptr,o->ptr,sdslen(o->ptr));
            c->reply_bytes += zmalloc_size_sds(tail->ptr);

        // 直接将对象追加到末尾
        } else {
            incrRefCount(o);
            listAddNodeTail(c->reply,o);
            c->reply_bytes += getStringObjectSdsUsedMemory(o);
        }
    }

    // 检查回复缓冲区的大小，如果超过系统限制的话，那么关闭客户端
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* This method takes responsibility over the sds. When it is no longer
 * needed it will be free'd, otherwise it ends up in a robj. */
// 和 _addReplyObjectToList 类似，但会负责 SDS 的释放功能（如果需要的话）
void _addReplySdsToList(redisClient *c, sds s) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) {
        sdsfree(s);
        return;
    }

    if (listLength(c->reply) == 0) {
        listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
        c->reply_bytes += zmalloc_size_sds(s);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL && tail->encoding == REDIS_ENCODING_RAW &&
            sdslen(tail->ptr)+sdslen(s) <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,s,sdslen(s));
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
            sdsfree(s);
        } else {
            listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
            c->reply_bytes += zmalloc_size_sds(s);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

void _addReplyStringToList(redisClient *c, char *s, size_t len) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

    if (listLength(c->reply) == 0) {
        // 为字符串创建字符串对象并追加到回复链表末尾
        robj *o = createStringObject(s,len);

        listAddNodeTail(c->reply,o);
        c->reply_bytes += getStringObjectSdsUsedMemory(o);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL && tail->encoding == REDIS_ENCODING_RAW &&
            sdslen(tail->ptr)+len <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            // 将字符串拼接到一个 SDS 之后
            tail->ptr = sdscatlen(tail->ptr,s,len);
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
        } else {
            // 为字符串创建字符串对象并追加到回复链表末尾
            robj *o = createStringObject(s,len);

            listAddNodeTail(c->reply,o);
            c->reply_bytes += getStringObjectSdsUsedMemory(o);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * -------------------------------------------------------------------------- */

void addReply(redisClient *c, robj *obj) {

    // 为客户端安装写处理器到事件循环
    if (prepareClientToWrite(c) != REDIS_OK) return;

    /* This is an important place where we can avoid copy-on-write
     * when there is a saving child running, avoiding touching the
     * refcount field of the object if it's not needed.
     *
     * 如果在使用子进程，那么尽可能地避免修改对象的 refcount 域。（优先使用 _addReplyToBuffer()，不会改变 refcount field，避免 COW）
     * 实在不行了再使用 _addReplyObjectToList(), 会改变 refcount field
     *
     * If the encoding is RAW and there is room in the static buffer
     * we'll be able to send the object to the client without
     * messing with its page. 
     *
     * 如果对象的编码为 RAW ，并且静态缓冲区中有空间
     * 那么就可以在不弄乱内存页的情况下，将对象发送给客户端。
     */
    // TODO: (DONE)放进 reply_buffer 跟 reply_list 之间有什么区别？
    // 先用 buffer，再用 list; 用了 list 也就意味着 buffer 已经装不下了
    // buff 可以避免 refcount field 的改动
    if (sdsEncodedObject(obj)) {
        // 不管三七二十一，哪怕不是处于 copy-on-write 的状态下，也直接将要发送出去的 obj->ptr 数据深拷贝进 buffer 里面
        // 因为引用的方式放进 buffer 里面将会增加引用计数，也就以唯这内存的修改，也就会触发缺页错误，利用中断，重新分配内存，将整个 memory-page(4 kByte) 拷贝一次
        // 这样的话，将会比直接中断、拷贝 4kByte 来得更快（sds-obj data 部分超过 4 kByte 的概率实在是小）
        if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
            // 如果 c->buf 中的空间不够，就复制到 c->reply 链表中
            // 可能会引起内存分配
            _addReplyObjectToList(c,obj);
    } else if (obj->encoding == REDIS_ENCODING_INT) {
        /* Optimization: if there is room in the static buffer for 32 bytes
         * (more than the max chars a 64 bit integer can take as string) we
         * avoid decoding the object and go for the lower level approach. */
        // 优化，如果 c->buf 中有等于或多于 32 个字节的空间
        // 那么将整数直接以字符串的形式复制到 c->buf 中
        if (listLength(c->reply) == 0 && (sizeof(c->buf) - c->bufpos) >= 32) {
            char buf[32];
            int len;

            len = ll2string(buf,sizeof(buf),(long)obj->ptr);
            if (_addReplyToBuffer(c,buf,len) == REDIS_OK)   // 避免 touch decrRefCount() 引发引用计数的改变
                return;
            /* else... continue with the normal code path, but should never
             * happen actually since we verified there is room. */
        }
        // 执行到这里，代表对象是整数，并且长度大于 32 位
        // 将它转换为字符串
        obj = getDecodedObject(obj);
        // 保存到缓存中
        if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
            _addReplyObjectToList(c,obj);
        decrRefCount(obj);
    } else {
        redisPanic("Wrong obj->encoding in addReply()");
    }
}

/*
 * 将 SDS 中的内容复制到回复缓冲区
 */
void addReplySds(redisClient *c, sds s) {
    if (prepareClientToWrite(c) != REDIS_OK) {
        /* The caller expects the sds to be free'd. */
        sdsfree(s);
        return;
    }
    if (_addReplyToBuffer(c,s,sdslen(s)) == REDIS_OK) {
        sdsfree(s);
    } else {
        /* This method free's the sds when it is no longer needed. */
        _addReplySdsToList(c,s);
    }
}

/*
 * 将 C 字符串中的内容复制到回复缓冲区
 */
void addReplyString(redisClient *c, char *s, size_t len) {
    if (prepareClientToWrite(c) != REDIS_OK) return;
    if (_addReplyToBuffer(c,s,len) != REDIS_OK)
        _addReplyStringToList(c,s,len);
}

void addReplyErrorLength(redisClient *c, char *s, size_t len) {
    addReplyString(c,"-ERR ",5);
    addReplyString(c,s,len);
    addReplyString(c,"\r\n",2);
}

/*
 * 返回一个错误回复
 *
 * 例子 -ERR unknown command 'foobar'
 */
void addReplyError(redisClient *c, char *err) {
    addReplyErrorLength(c,err,strlen(err));
}

void addReplyErrorFormat(redisClient *c, const char *fmt, ...) {
    size_t l, j;
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted. */
    l = sdslen(s);
    for (j = 0; j < l; j++) {
        if (s[j] == '\r' || s[j] == '\n') s[j] = ' ';
    }
    addReplyErrorLength(c,s,sdslen(s));
    sdsfree(s);
}

/**
 * 就比如: 
 * telnet 127.0.0.1  6379
 * type global-key
 * +hash
*/
void addReplyStatusLength(redisClient *c, char *s, size_t len) {
    addReplyString(c,"+",1);
    addReplyString(c,s,len);
    addReplyString(c,"\r\n",2);
}

/*
 * 返回一个状态回复
 *
 * 例子 +OK\r\n
 */
void addReplyStatus(redisClient *c, char *status) {
    addReplyStatusLength(c,status,strlen(status));
}

void addReplyStatusFormat(redisClient *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    addReplyStatusLength(c,s,sdslen(s));
    sdsfree(s);
}

/* Adds an empty object to the reply list that will contain the multi bulk
 * length, which is not known when this function is called. */
// 当发送 Multi Bulk 回复时，先创建一个空的链表，之后再用实际的回复填充它
// 相当于预先准备 Arrays-reply 的空间
/**
 * EXAMPLE:
 *  ZRANGEBYSCORE zs-key 0 100
 *  *8
 *  $2
 *  m1
 *  $2
 *  m2
 *  $2
 *  m3
 *  $2
 *  m4
 *  $2
 *  m5
 *  $3
 *  am3
 *  $7
 *  memaber
 *  $6
 *  member
 */
void *addDeferredMultiBulkLength(redisClient *c) {
    /* Note that we install the write event here even if the object is not
     * ready to be sent, since we are sure that before returning to the
     * event loop setDeferredMultiBulkLength() will be called. */
    if (prepareClientToWrite(c) != REDIS_OK) return NULL;
    listAddNodeTail(c->reply,createObject(REDIS_STRING,NULL));  // 相当于手动禁止了 buf 的时候，转而使用 reply-list，同时这个 node 是预留给 "*15\r\n" 的
    return listLast(c->reply);  // 给 setDeferredMultiBulkLength() 使用
}

/* Populate the length object and try gluing it to the next chunk. */
// 设置 Multi Bulk 回复的长度，仅仅是调整最开头的 "*15" 这一个长度
void setDeferredMultiBulkLength(redisClient *c, void *node, long length) {
    listNode *ln = (listNode*)node; // 当前是指向 NULL 的 tail 节点
    robj *len, *next;

    /* Abort when *node is NULL (see addDeferredMultiBulkLength). */
    if (node == NULL) return;

    // ln 这个 node 是 addDeferredMultiBulkLength() 预留出来的节点，一定在整个 reply-list 的最开头部分
    len = listNodeValue(ln);
    len->ptr = sdscatprintf(sdsempty(),"*%ld\r\n",length);
    len->encoding = REDIS_ENCODING_RAW; /* in case it was an EMBSTR. */
    c->reply_bytes += zmalloc_size_sds(len->ptr);
    if (ln->next != NULL) {
        // 除了预留节点之外，还有别的节点
        next = listNodeValue(ln->next);

        /* Only glue when the value of next node is non-NULL (an sds in this case) */
        if (next->ptr != NULL) {
            c->reply_bytes -= zmalloc_size_sds(len->ptr);
            c->reply_bytes -= getStringObjectSdsUsedMemory(next);
            len->ptr = sdscatlen(len->ptr,next->ptr,sdslen(next->ptr));
            c->reply_bytes += zmalloc_size_sds(len->ptr);
            listDelNode(c->reply,ln->next);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* Add a double as a bulk reply */
/*
 * 以 bulk 回复的形式，返回一个双精度浮点数
 *
 * 例子 $4\r\n3.14\r\n
 */
void addReplyDouble(redisClient *c, double d) {
    char dbuf[128], sbuf[128];
    int dlen, slen;
    if (isinf(d)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        addReplyBulkCString(c, d > 0 ? "inf" : "-inf");
    } else {
        dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
        slen = snprintf(sbuf,sizeof(sbuf),"$%d\r\n%s\r\n",dlen,dbuf);
        addReplyString(c,sbuf,slen);
    }
}

/* Add a long long as integer reply or bulk len / multi bulk count.
 * 
 * 添加一个 long long 为整数回复，或者 bulk 或 multi bulk 的数目
 *
 * Basically this is used to output <prefix><long long><crlf>. 
 *
 * 输出格式为 <prefix><long long><crlf>
 *
 * 例子:
 *
 * *5\r\n10086\r\n
 *
 * $5\r\n10086\r\n
 */
void addReplyLongLongWithPrefix(redisClient *c, long long ll, char prefix) {
    char buf[128];
    int len;

    /* Things like $3\r\n or *2\r\n are emitted very often by the protocol
     * so we have a few shared objects to use if the integer is small
     * like it is most of the times. */
    if (prefix == '*' && ll < REDIS_SHARED_BULKHDR_LEN) {
        // 多条批量回复
        addReply(c,shared.mbulkhdr[ll]);
        return;
    } else if (prefix == '$' && ll < REDIS_SHARED_BULKHDR_LEN) {
        // 批量回复
        addReply(c,shared.bulkhdr[ll]);
        return;
    }

    buf[0] = prefix;
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addReplyString(c,buf,len+3);
}

/*
 * 返回一个整数回复
 * 
 * 格式为 :10086\r\n
 */
void addReplyLongLong(redisClient *c, long long ll) {
    if (ll == 0)
        addReply(c,shared.czero);
    else if (ll == 1)
        addReply(c,shared.cone);
    else
        addReplyLongLongWithPrefix(c,ll,':');
}

/**
 * 1. 批量回复（bulk reply）的第一个字节是 "$"; For Bulk Strings the first byte of the reply is "$"
 * Bulk Strings are used in order to represent a single binary safe string up to 512 MB in length.
 * Bulk Strings are encoded in the following way:
 *  1) A "$" byte followed by the number of bytes composing the string (a prefixed length), terminated by CRLF.
 *  2) The actual string data.
 *  3) A final CRLF.
 * 
 * EXAMPLE:
 *  1) Null Bulk String   : "$-1\r\n"
 *  2) an empty string    : "$0\r\n\r\n"
 *  3) the string "foobar": "$6\r\nfoobar\r\n"
 * 
 * 2. 多条批量回复（multi bulk reply）的第一个字节是 "*"; For Arrays the first byte of the reply is "*"(后续版本中，multi-bulk 被替换为 Arrays, 但本质内容不变)
 * 比如 LRANGE 这样的 CMD，就会采用这种方式来进行 reply，因为要 reply 很多个 element
 * 
 * Arrays can contain mixed types, it's not necessary for the elements to be of the same type.
 * 同一个 Array\multi-bulk 里面可以装多种不同类型的数据
 * 
 * RESP Arrays are sent using the following format:
 *  1) A * character as the first byte, followed by the number of elements in the array as a decimal number, followed by CRLF.
 *  2) An additional RESP type for every element of the Array.
 * 
 * EXAMPLE:
 *  1) Null Array: "*-1\r\n"   比如当 BLPOP 命令的阻塞时间超过最大时限时，它就返回一个无内容的多条批量回复，就是这个
 *  2) an empty Array: "*0\r\n"
 *  3) two RESP Bulk Strings "foo" and "bar": "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
 *  4) contain mixed types: 
 *     *5\r\n
 *     :1\r\n
 *     :2\r\n
 *     :3\r\n
 *     :4\r\n
 *     $6\r\n
 *     foobar\r\n
 *  5) Arrays of arrays:
 *     *2\r\n
 *       *3\r\n
 *         :1\r\n
 *         :2\r\n
 *         :3\r\n
 *       *2\r\n
 *         +Foo\r\n
 *         -Bar\r\n
 *  6) Null elements in Arrays: ["foo",nil,"bar"]
 *     *3\r\n
 *     $3\r\n
 *     foo\r\n
 *     $-1\r\n
 *     $3\r\n
 *     bar\r\n
*/

void addReplyMultiBulkLen(redisClient *c, long length) {
    if (length < REDIS_SHARED_BULKHDR_LEN)
        addReply(c,shared.mbulkhdr[length]);
    else
        addReplyLongLongWithPrefix(c,length,'*');
}

/* Create the length prefix of a bulk reply, example: $2234 */
// 测量并自动填充 obj 的 len
void addReplyBulkLen(redisClient *c, robj *obj) {
    size_t len;

    if (sdsEncodedObject(obj)) {
        len = sdslen(obj->ptr);
    } else {
        long n = (long)obj->ptr;

        /* Compute how many bytes will take this integer as a radix 10 string */
        len = 1;
        if (n < 0) {
            len++;
            n = -n;
        }
        while((n = n/10) != 0) {
            len++;
        }
    }

    if (len < REDIS_SHARED_BULKHDR_LEN)
        addReply(c,shared.bulkhdr[len]);
    else
        addReplyLongLongWithPrefix(c,len,'$');
}

/* Add a Redis Object as a bulk reply 
 *
 * 返回一个 Redis 对象作为回复
 */
void addReplyBulk(redisClient *c, robj *obj) {
    addReplyBulkLen(c,obj);
    addReply(c,obj);
    addReply(c,shared.crlf);
}

/* Add a C buffer as bulk reply 
 *
 * 返回一个 C 缓冲区作为回复
 */
void addReplyBulkCBuffer(redisClient *c, void *p, size_t len) {
    addReplyLongLongWithPrefix(c,len,'$');
    addReplyString(c,p,len);
    addReply(c,shared.crlf);
}

/* Add a C nul term string as bulk reply 
 *
 * 返回一个 C 字符串作为回复
 */
void addReplyBulkCString(redisClient *c, char *s) {
    if (s == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        addReplyBulkCBuffer(c,s,strlen(s));
    }
}

/* Add a long long as a bulk reply 
 *
 * 返回一个 long long 值作为回复
 */
void addReplyBulkLongLong(redisClient *c, long long ll) {
    char buf[64];
    int len;

    len = ll2string(buf,64,ll);
    addReplyBulkCBuffer(c,buf,len);
}

/* Copy 'src' client output buffers into 'dst' client output buffers.
 * The function takes care of freeing the old output buffers of the
 * destination client. */
// 释放 dst 客户端原有的输出内容，并将 src 客户端的输出内容复制给 dst
// repl 进行 full sync 的时候，会通过这个函数，复用 RDB 跟相应的追加数据
void copyClientOutputBuffer(redisClient *dst, redisClient *src) {

    // 释放 dst 原有的回复链表
    // 无所谓，反正在做 full sync，扔了就扔了
    listRelease(dst->reply);
    // 复制新链表到 dst
    dst->reply = listDup(src->reply);

    // 复制内容到回复 buf
    memcpy(dst->buf,src->buf,src->bufpos);

    // 同步偏移量和字节数
    dst->bufpos = src->bufpos;
    dst->reply_bytes = src->reply_bytes;
}

/*
 * TCP 连接 accept 处理器
 */
#define MAX_ACCEPTS_PER_CALL 1000
static void acceptCommonHandler(int fd, int flags) {

    // 创建客户端
    redisClient *c;
    if ((c = createClient(fd)) == NULL) {
        redisLog(REDIS_WARNING,
            "Error registering fd event for the new client: %s (fd=%d)",
            strerror(errno),fd);
        close(fd); /* May be already closed, just ignore errors */
        return;
    }

    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in non-blocking
     * mode and we can send an error for free using the Kernel I/O */
    // 如果新添加的客户端令服务器的最大客户端数量达到了
    // 那么向新客户端写入错误信息，并关闭新客户端
    // 先创建客户端，再进行数量检查是为了方便地进行错误信息写入
    if (listLength(server.clients) > server.maxclients) {   // 已经预留了部分 fd 来进行拒绝链接
        char *err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        if (write(c->fd,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        // 更新拒绝连接数
        server.stat_rejected_conn++;
        freeClient(c);
        return;
    }

    // 更新连接次数
    server.stat_numconnections++;

    // 设置 FLAG
    c->flags |= flags;
}

/* 
 * 创建一个 TCP 连接处理器
 * 在向 epoll-instance 注册 listen-socket 的时候，一同注册进去的回调函数
 */
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL; // 避免长时间阻塞，甚至是 syn 攻击；反正 epoll-instance 也是 level-trigger
    char cip[REDIS_IP_STR_LEN];
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    while(max--) {
        // accept 客户端连接
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                redisLog(REDIS_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        redisLog(REDIS_VERBOSE,"Accepted %s:%d", cip, cport);
        // 为客户端创建客户端状态（redisClient）
        acceptCommonHandler(cfd,0);
    }
}

/*
 * 创建一个本地连接处理器
 */
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd, max = MAX_ACCEPTS_PER_CALL;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    while(max--) {
        // accept 本地客户端连接
        cfd = anetUnixAccept(server.neterr, fd);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                redisLog(REDIS_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        redisLog(REDIS_VERBOSE,"Accepted connection to %s", server.unixsocket);
        // 为本地客户端创建客户端状态
        acceptCommonHandler(cfd,REDIS_UNIX_SOCKET);
    }
}

/*
 * 清空所有命令参数
 */
static void freeClientArgv(redisClient *c) {
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
    c->cmd = NULL;
}

/* Close all the slaves connections. This is useful in chained replication
 * when we resync with our own master and want to force all our slaves to
 * resync with us as well. */
// 断开所有 slave 的连接，强制所有 slave 执行重同步，TODO: 为什么能做到这种效果？
// TODO: 要是 slave_1 的 slave_2 先跟 slave_1 连接上，然后 sync，这之后 slave_1 才跟 master 接上，会怎样？有没有 handle 这个 case？
void disconnectSlaves(void) {
    while (listLength(server.slaves)) {
        listNode *ln = listFirst(server.slaves);
        freeClient((redisClient*)ln->value);
    }
}

/* This function is called when the slave lose the connection with the
 * master into an unexpected way. */
// 这个函数在 slave 以外地和 master 失去联系时调用
// TODO:（DONE） 多机功能看完之后，再看看这个，这个跟 PSYNC 过程有关
void replicationHandleMasterDisconnection(void) {
    server.master = NULL;
    server.repl_state = REDIS_REPL_CONNECT; // 尝试 connect 的状态
    server.repl_down_since = server.unixtime;
    /* We(we are a slave) lost connection with our master, force our slaves to resync
     * with us as well to load the new data set.
     *
     * 和 master 失联，强制所有这个服务器的 slave  resync ，
     * 等待载入新数据。
     *
     * If server.masterhost is NULL the user called SLAVEOF NO ONE so
     * slave resync is not needed. 
     *
     * TODO:（DONE）如果 masterhost 不存在（怎么会这样呢？）有检查的，syncCommand() 最开头就检查了
     * 那么调用 SLAVEOF NO ONE ，避免 slave resync
     */
    if (server.masterhost != NULL) disconnectSlaves();
}

/*
 * 释放客户端
 */
void freeClient(redisClient *c) {
    listNode *ln;

    /* If this is marked as current client unset it */
    if (server.current_client == c) server.current_client = NULL;

    /* If it is our master that's beging disconnected we should make sure
     * to cache the state to try a partial resynchronization later.
     *
     * Note that before doing this we make sure that the client is not in
     * some unexpected state, by checking its flags. */
    if (server.master && c->flags & REDIS_MASTER) {
        // slave 才会进来这个分支
        redisLog(REDIS_WARNING,"Connection with master lost.");
        if (!(c->flags & (REDIS_CLOSE_AFTER_REPLY|
                          REDIS_CLOSE_ASAP|
                          REDIS_BLOCKED|
                          REDIS_UNBLOCKED)))
        {
            // 这是为了避免 slave 跟 master 短暂失联后，必须 full sync 的局面
            // 短暂失联，是可以采用 p sync 的
            replicationCacheMaster(c);
            return;
        }
    }

    /* Log link disconnection with slave */
    if ((c->flags & REDIS_SLAVE) && !(c->flags & REDIS_MONITOR)) {
        char ip[REDIS_IP_STR_LEN];

        if (anetPeerToString(c->fd,ip,sizeof(ip),NULL) != -1) {
            redisLog(REDIS_WARNING,"Connection with slave %s:%d lost.",
                ip, c->slave_listening_port);
        }
    }

    /* Free the query buffer */
    sdsfree(c->querybuf);
    c->querybuf = NULL;

    /* Deallocate structures used to block on blocking ops. */
    if (c->flags & REDIS_BLOCKED) unblockClient(c);
    dictRelease(c->bpop.keys);

    /* UNWATCH all the keys */
    // 清空 WATCH 信息
    unwatchAllKeys(c);
    listRelease(c->watched_keys);

    /* Unsubscribe from all the pubsub channels */
    // 退订所有频道和模式
    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeAllPatterns(c,0);
    dictRelease(c->pubsub_channels);
    listRelease(c->pubsub_patterns);

    /* Close socket, unregister events, and remove list of replies and
     * accumulated arguments. */
    // 关闭套接字，并从事件处理器中删除该套接字的事件
    if (c->fd != -1) {
        aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
        close(c->fd);
    }

    // 清空回复缓冲区
    listRelease(c->reply);

    // 清空命令参数
    freeClientArgv(c);

    /* Remove from the list of clients */
    //  slave 的客户端链表中删除自身
    if (c->fd != -1) {
        ln = listSearchKey(server.clients,c);
        redisAssert(ln != NULL);
        listDelNode(server.clients,ln);
    }

    /* When client was just unblocked because of a blocking operation,
     * remove it from the list of unblocked clients. */
    // 删除客户端的阻塞信息
    if (c->flags & REDIS_UNBLOCKED) {
        ln = listSearchKey(server.unblocked_clients,c);
        redisAssert(ln != NULL);
        listDelNode(server.unblocked_clients,ln);
    }

    /* Master/slave cleanup Case 1:
     * we lost the connection with a slave. */
    if (c->flags & REDIS_SLAVE) {
        if (c->replstate == REDIS_REPL_SEND_BULK) {
            if (c->repldbfd != -1) close(c->repldbfd);
            if (c->replpreamble) sdsfree(c->replpreamble);
        }
        list *l = (c->flags & REDIS_MONITOR) ? server.monitors : server.slaves;
        ln = listSearchKey(l,c);
        redisAssert(ln != NULL);
        listDelNode(l,ln);
        /* We need to remember the time when we started to have zero
         * attached slaves, as after some time we'll free the replication
         * backlog. */
        if (c->flags & REDIS_SLAVE && listLength(server.slaves) == 0)
            server.repl_no_slaves_since = server.unixtime;
        refreshGoodSlavesCount();
    }

    /* Master/slave cleanup Case 2:
     * we lost the connection with the master. */
    if (c->flags & REDIS_MASTER) replicationHandleMasterDisconnection();

    /* If this client was scheduled for async freeing we need to remove it
     * from the queue. */
    if (c->flags & REDIS_CLOSE_ASAP) {
        ln = listSearchKey(server.clients_to_close,c);
        redisAssert(ln != NULL);
        listDelNode(server.clients_to_close,ln);
    }

    /* Release other dynamically allocated client structure fields,
     * and finally release the client structure itself. */
    if (c->name) decrRefCount(c->name);
    // 清除参数空间
    zfree(c->argv);
    // 清除事务状态信息
    freeClientMultiState(c);
    sdsfree(c->peerid);
    // 释放客户端 redisClient 结构本身
    zfree(c);
}

/* Schedule a client to free it at a safe time in the serverCron() function.
 * This function is useful when we need to terminate a client but we are in
 * a context where calling freeClient() is not possible, because the client
 * should be valid for the continuation of the flow of the program. */
// 异步地释放给定的客户端
void freeClientAsync(redisClient *c) {
    if (c->flags & REDIS_CLOSE_ASAP) return;
    c->flags |= REDIS_CLOSE_ASAP;
    listAddNodeTail(server.clients_to_close,c);
}

// 关闭需要异步关闭的客户端(Queue: server.clients_to_close)
void freeClientsInAsyncFreeQueue(void) {
    
    // 遍历所有要关闭的客户端
    while (listLength(server.clients_to_close)) {
        listNode *ln = listFirst(server.clients_to_close);
        redisClient *c = listNodeValue(ln);

        c->flags &= ~REDIS_CLOSE_ASAP;
        // 关闭客户端
        freeClient(c);
        // 从客户端链表中删除被关闭的客户端
        listDelNode(server.clients_to_close,ln);
    }
}

/*
 * 负责传送命令回复的写处理器，将有 epoll-instance 进行监听调度，在就绪的情况下调用本函数，完成相应的 send 任务
 * 
 * 会尽可能的将 c->buf 里面需要发送，却被堆积的内容，全部发送给对应的 client（因为 write 不是立马就可以就绪的，
 * 所以一般会采用这样的异步 write 方案，等到 fd 的 write 就绪之后，再把数据从 buf 里面，真正的 write 出去）
 */
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = privdata;
    int nwritten = 0, totwritten = 0, objlen;
    size_t objmem;
    robj *o;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    // 一直循环，直到回复缓冲区为空（包含 buf 里面的内容跟 reply 这个 list 里面的内容清空）
    // 或者指定条件满足为止
    while(c->bufpos > 0 || listLength(c->reply)) {

        if (c->bufpos > 0) {

            // 是优先将 buf 里面的内容发送出去的
            // c->bufpos > 0, 说明 client->buf 里面是有内容的

            // 写入内容到套接字
            // c->sentlen 是用来处理 short write 的
            // 当出现 short write ，导致写入未能一次完成时，
            // c->buf+c->sentlen 就会偏移到正确（未写入）内容的位置上。
            nwritten = write(fd,c->buf+c->sentlen,c->bufpos-c->sentlen);
            // 出错则跳出
            if (nwritten <= 0) break;   // EAGAIN
            // 成功写入则更新写入计数器变量
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
             * the remainder of the reply. */
            // 如果缓冲区中的内容已经全部写入完毕
            // 那么清空客户端的两个计数器变量
            if (c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }
        } else {

            // listLength(c->reply) != 0

            // 取出位于链表最前面的对象，objlen 跟 objmem 实际上差别不大
            // TODO:(DONE) 为什么要区别对待 objlen\objmem ?
            // 本质上没有啥区别，只不过，objmem 支持 REDIS_ENCODING_RAW，REDIS_ENCODING_EMBSTR 两种格式罢了
            o = listNodeValue(listFirst(c->reply));
            objlen = sdslen(o->ptr);
            objmem = getStringObjectSdsUsedMemory(o);

            // 略过空对象
            if (objlen == 0) {
                listDelNode(c->reply,listFirst(c->reply));
                c->reply_bytes -= objmem;
                continue;
            }

            // 写入内容到套接字
            // c->sentlen 是用来处理 short write 的
            // 当出现 short write ，导致写入未能一次完成时，
            // c->buf+c->sentlen 就会偏移到正确（未写入）内容的位置上。
            nwritten = write(fd, ((char*)o->ptr)+c->sentlen,objlen-c->sentlen);
            // 写入出错则跳出
            if (nwritten <= 0) break;   // EAGAIN
            // 成功写入则更新写入计数器变量
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If we fully sent the object on head go to the next one */
            // 如果缓冲区内容全部写入完毕，那么删除已写入完毕的节点
            if (c->sentlen == objlen) {
                listDelNode(c->reply,listFirst(c->reply));
                c->sentlen = 0;
                c->reply_bytes -= objmem;
            }
        }
        /* Note that we avoid to send more than REDIS_MAX_WRITE_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * 为了避免一个非常大的回复独占服务器，
         * 当写入的总数量大于 REDIS_MAX_WRITE_PER_EVENT ，
         * 临时中断写入，将处理时间让给其他客户端，
         * 剩余的内容等下次写入就绪再继续写入
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver. 
         *
         * 不过，如果服务器的内存占用已经超过了限制，
         * 那么为了将回复缓冲区中的内容尽快写入给客户端，
         * 然后释放回复缓冲区的空间来回收内存，
         * 这时即使写入量超过了 REDIS_MAX_WRITE_PER_EVENT ，
         * 程序也继续进行写入
         */
        // 先复制，在决定是否终止的话，能够确保每一个 obj 的 data 都是完整被发送的，不会将一个 obj 的 data 割裂开来
        // 但是当 write() 的缓冲区满了之后，就不可避免地发生了对象割裂地问题，只能够下次 write() 就绪的时候，再把剩下的部分发送出去了
        if (totwritten > REDIS_MAX_WRITE_PER_EVENT &&
            (server.maxmemory == 0 ||
             zmalloc_used_memory() < server.maxmemory)) break;
    } // end of while

    // 写入出错检查（对应上面的 break 跳出）
    if (nwritten == -1) {
        if (errno == EAGAIN) {  // 常见，缓冲区满了
            nwritten = 0;
        } else {
            redisLog(REDIS_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    }

    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        if (!(c->flags & REDIS_MASTER)) c->lastinteraction = server.unixtime;
    }
    if (c->bufpos == 0 && listLength(c->reply) == 0) {
        c->sentlen = 0;

        // 删除 write handler（注意，redis 里面的 event 是采用 level-trigger 的，所以没有写完的时候，不需要再次向 epoll 注册 event）
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);

        /* Close connection after entire reply has been sent. */
        // 如果指定了写入之后关闭客户端 FLAG ，那么关闭客户端
        if (c->flags & REDIS_CLOSE_AFTER_REPLY) freeClient(c);
    }
}

/* resetClient prepare the client to process the next command */
// 在客户端执行完命令之后执行：重置客户端以准备执行下个命令
void resetClient(redisClient *c) {
    redisCommandProc *prevcmd = c->cmd ? c->cmd->proc : NULL;

    freeClientArgv(c);
    c->reqtype = 0;
    c->multibulklen = 0;
    c->bulklen = -1;
    /* We clear the ASKING flag as well if we are not inside a MULTI, and
     * if what we just executed is not the ASKING command itself. */
    if (!(c->flags & REDIS_MULTI) && prevcmd != askingCommand)
        c->flags &= (~REDIS_ASKING);
}

/*
 * 处理内联命令，并创建参数对象
 *
 * 内联命令的各个参数以空格分开，并以 \r\n 结尾
 * 例子：
 *
 * <arg0> <arg1> <arg...> <argN>\r\n
 *
 * 这些内容会被用于创建参数对象，
 * 比如
 *
 * argv[0] = arg0
 * argv[1] = arg1
 * argv[2] = arg2
 */
int processInlineBuffer(redisClient *c) {
    char *newline;
    int argc, j;
    sds *argv, aux;
    size_t querylen;

    /* Search for end of line */
    newline = strchr(c->querybuf,'\n');

    /* Nothing to do without a \r\n */
    // 收到的查询内容不符合协议格式，出错，可能是因为 stream 式协议的锅
    if (newline == NULL) {
        if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
            addReplyError(c,"Protocol error: too big inline request");
            setProtocolError(c,0);
        }
        return REDIS_ERR;
    }

    /* Handle the \r\n case. */
    if (newline && newline != c->querybuf && *(newline-1) == '\r')
        newline--;

    /* Split the input buffer up to the \r\n */
    // 根据空格，分割命令的参数
    // 比如说 SET msg hello \r\n 将分割为
    // argv[0] = SET
    // argv[1] = msg
    // argv[2] = hello
    // argc = 3
    querylen = newline-(c->querybuf);
    aux = sdsnewlen(c->querybuf,querylen);
    argv = sdssplitargs(aux,&argc);
    sdsfree(aux);
    if (argv == NULL) {
        addReplyError(c,"Protocol error: unbalanced quotes in request");
        setProtocolError(c,0);
        return REDIS_ERR;
    }

    /* Newline from slaves can be used to refresh the last ACK time.
     * This is useful for a slave to ping back while loading a big
     * RDB file. */
    if (querylen == 0 && c->flags & REDIS_SLAVE)
        c->repl_ack_time = server.unixtime;

    /* Leave data after the first line of the query in the buffer */

    // 从缓冲区中删除已 argv 已读取的内容
    // 剩余的内容是未读取的
    sdsrange(c->querybuf,querylen+2,-1);

    /* Setup argv array on client structure */
    // 为客户端的参数分配空间
    if (c->argv) zfree(c->argv);
    c->argv = zmalloc(sizeof(robj*)*argc);

    /* Create redis objects for all arguments. */
    // 为每个参数创建一个字符串对象
    for (c->argc = 0, j = 0; j < argc; j++) {
        if (sdslen(argv[j])) {
            // argv[j] 已经是 SDS 了
            // 所以创建的字符串对象直接指向该 SDS
            c->argv[c->argc] = createObject(REDIS_STRING,argv[j]);
            c->argc++;
        } else {
            sdsfree(argv[j]);
        }
    }

    zfree(argv);

    return REDIS_OK;
}

/* Helper function. Trims query buffer to make the function that processes
 * multi bulk requests idempotent. */
// 如果在读入协议内容时，发现内容不符合协议，那么异步地关闭这个客户端。
static void setProtocolError(redisClient *c, int pos) {
    if (server.verbosity >= REDIS_VERBOSE) {
        sds client = catClientInfoString(sdsempty(),c);
        redisLog(REDIS_VERBOSE,
            "Protocol error from client: %s", client);
        sdsfree(client);
    }
    c->flags |= REDIS_CLOSE_AFTER_REPLY;
    sdsrange(c->querybuf,pos,-1);
}

/*
 * 将 c->querybuf 中的协议内容转换成 c->argv 中的参数对象
 * 
 * 比如 *3\r\n$3\r\nSET\r\n$3\r\nMSG\r\n$5\r\nHELLO\r\n
 * 将被转换为：
 * argv[0] = SET
 * argv[1] = MSG
 * argv[2] = HELLO
 */
/**
 * test case: zadd key-string 6 member-6 7 member-7 8 member-8 9 member-9 10 member-10
 * RESP info: "*12\r\n
 *             $4\r\nzadd\r\n
 *             $10\r\nkey-string\r\n
 *             $1\r\n6\r\n
 *             $8\r\nmember-6\r\n
 *             $1\r\n7\r\n
 *             $8\r\nmember-7\r\n
 *             $1\r\n8\r\n
 *             $8\r\nmember-8\r\n
 *             $1\r\n9\r\n
 *             $8\r\nmember-9\r\n
 *             $2\r\n10\r\n
 *             $9\r\nmember-10\r\n"
 */
// 根据 RESP 协议，将 c->querybuf 中的字符串，解析为 argv[argc]
// TODO:(DONE, 详见 processInputBuffer() 中的 while-loop) 
//       面对被 block 住的 client 会发生什么？
//       这个 block 住的 client 是不是没有办法发送任何命令过来？(看起来是这样的，server 不会，client 没办法输入新的 CMD)
//       所以这个 block 的 client 也不会发生 querybuf 多个完整 CMD 堆积的现象？
//       这个 querybuf 究竟会不会发生多个 CMD 堆积的问题？
int processMultibulkBuffer(redisClient *c) {
    char *newline = NULL;
    int pos = 0, ok;
    long long ll;

    /* 从 c->quertbuf 中解析、并设置 multibulklen */
    // 比如 *3\r\n$3\r\nSET\r\n... 将令 c->multibulklen = 3
    if (c->multibulklen == 0) {
        // c->multibulklen == 0 说明上一次的 read 没有发生不完整的读取 RESP 协议的情况
        // 一旦没有完整读取 RESP 协议数据，这个 multibulklen 将不会是 0
        /* The client should have been reset */
        redisAssertWithInfo(c,NULL,c->argc == 0);

        /* Multi bulk length cannot be read without a \r\n */
        newline = strchr(c->querybuf,'\r');
        if (newline == NULL) {
            if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
                addReplyError(c,"Protocol error: too big mbulk count string");
                setProtocolError(c,0);
            }
            // 可能是因为 redis-cli 发过来的第一个 string 太长了，长到超过了可以接收的范围
            // 也可能是因为压根没有发送 "\r\n" 的分隔符过来（格式错误）
            return REDIS_ERR;
        }
        /* Buffer should also contain */
        if (newline - (c->querybuf) > ((signed)sdslen(c->querybuf)-2))
            return REDIS_ERR;   // c->querybuf 里面压根没有多余的内存放后面的 \n

        /* We know for sure there is a whole line since newline != NULL,
         * so go ahead and find out the multi bulk length. */
        // 协议的第一个字符必须是 '*'，其实，跨网络传输的话，还真有可能不是 "*"（传输的时候，出错）
        // 感觉直接把这个 req drop 掉就好（error 降级），而不是直接退出 redis-server
        // 后续的 redis 6.0 version 也确实移除了下面的 assert
        redisAssertWithInfo(c,NULL,c->querybuf[0] == '*');

        // 将参数个数，也即是 * 之后， \r\n 之前的数字取出并保存到 ll 中
        // 比如对于 *3\r\n ，那么 ll 将等于 3（newline 标记了数字的末尾）
        ok = string2ll(c->querybuf+1,newline-(c->querybuf+1),&ll);
        // 参数的数量超出限制
        if (!ok || ll > 1024*1024) {
            addReplyError(c,"Protocol error: invalid multibulk length");
            setProtocolError(c,pos);
            return REDIS_ERR;
        }

        // 参数数量之后的位置
        // 比如对于 *3\r\n$3\r\n$SET\r\n... 来说，
        // pos 指向 *3\r\n$3\r\n$SET\r\n...
        //            ^   ^
        //            |   |
        //       newline  pos
        pos = (newline - c->querybuf) + 2;  // 跳过 *3\r\n, pos 是采用 index 的方式
        // 如果 ll <= 0 ，那么这个命令是一个空白命令
        // 那么将这段内容从查询缓冲区中删除，只保留未阅读的那部分内容
        // 为什么参数可以是空的呢？
        // processInputBuffer 中有注释到 "Multibulk processing could see a <= 0 length"
        // 但并没有详细说明原因
        if (ll <= 0) {
            sdsrange(c->querybuf,pos,-1);
            return REDIS_OK;
        }

        // 设置参数数量
        c->multibulklen = ll;

        /* Setup argv array on client structure */
        // 根据参数数量，为各个参数对象分配空间
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*c->multibulklen);
    }   // end of (c->multibulklen == 0), handle 已知的 multi-bulk 小节数量

    redisAssertWithInfo(c,NULL,c->multibulklen > 0);

    // 从 c->querybuf 中读入参数，并创建各个参数对象到 c->argv
    // TODO:(DONE, 详见 processInputBuffer() 中的 while-loop)  
    //      这样岂不是一次只读一个 CMD？那样 block 导致堆积的 CMD 在什么情况下会被消耗掉？
    //      确实可以 handle 异步的 redis-cli；但是，谁来再次触发这个函数呢？尤其是一次性读入了两个 CMD
    while(c->multibulklen) {

        /* Read bulk's length if unknown */
        // 读入单个参数长度(相当于预处理，还没有真正处理 CMD 里面的 argv)
        if (c->bulklen == -1) { // reset 之后都是 -1 的

            // 确保 "\r\n" 存在
            newline = strchr(c->querybuf+pos,'\r');
            if (newline == NULL) {
                if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
                    addReplyError(c,
                        "Protocol error: too big bulk count string");
                    setProtocolError(c,0);
                    return REDIS_ERR;
                }
                break;
            }
            /* Buffer should also contain \n */
            if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
                break;

            // 确保协议符合参数格式，检查其中的 $...
            // 比如 $3\r\nSET\r\n
            if (c->querybuf[pos] != '$') {
                addReplyErrorFormat(c,
                    "Protocol error: expected '$', got '%c'",
                    c->querybuf[pos]);
                setProtocolError(c,pos);
                return REDIS_ERR;
            }

            // 读取长度，pos 是本小节的开头 offset
            // 比如 $3\r\nSET\r\n 将会让 ll 的值设置 3
            ok = string2ll(c->querybuf+pos+1,newline-(c->querybuf+pos+1),&ll);
            if (!ok || ll < 0 || ll > 512*1024*1024) {
                addReplyError(c,"Protocol error: invalid bulk length");
                setProtocolError(c,pos);
                return REDIS_ERR;
            }

            // 定位到参数的开头
            // 比如 
            // $3\r\nSET\r\n...
            //       ^
            //       |
            //      pos
            pos += newline - (c->querybuf + pos) + 2;   // 基于 pos 的情况下，将 $3\r\n skip 掉
            // 如果参数非常长（因为 $100, 意味着接下来的参数拥有 100 个字符），
            // 那么做一些预备措施来优化接下来的参数复制操作：
            //     
            if (ll >= REDIS_MBULK_BIG_ARG) {    // 面对 REDIS_MBULK_BIG_ARG 的优化操作
                size_t qblen;

                /* If we are going to read a large object from network
                 * try to make it likely that it will start at c->querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data. */
                // 这个优化要跟 readQueryFromClient() 的开头 配合起作用
                sdsrange(c->querybuf,pos,-1);   // 裁剪掉多余的数据
                pos = 0;
                qblen = sdslen(c->querybuf);
                /* Hint the sds library about the amount of bytes this string is
                 * going to contain. */
                if (qblen < ll+2)   // TODO:(DONE, 看下面的注释，跟 readQueryFromClient()) 为什么是增加作为 buf 的 querybuf 长度来当作优化手段？
                    c->querybuf = sdsMakeRoomFor(c->querybuf,ll+2-qblen);
                    // 因为一个 while-loop 只会处理一个 arg 的缘故，
                    // 一般进了这个分支之后，都是发生了 RESP 内容滞留，需要等待下一次 read 的
                    // 这一次，扩展长度，只是为了下几次 read 的高效率
            }
            // 参数的长度
            c->bulklen = ll;
        }

        /* Read bulk argument（将 c->querybuf 中的 RESP 内容，读取并解析到 argv[argc] 中） */
        if (sdslen(c->querybuf)-pos < (unsigned)(c->bulklen+2)) {
            // 确保内容符合协议格式（可能因为部分数据仍然滞留在 redis-cli，所以直接下一次再处理）
            /* Not enough data (+2 == trailing \r\n) */
            break;
        } else {
            // 说明 $100, 接下来的 100 长度的 char 都已经再 querybuf 里面了，本次 read 可以进行处理
            // 为参数创建字符串对象  
            /* Optimization: if the buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (pos == 0 &&
                c->bulklen >= REDIS_MBULK_BIG_ARG &&    // 大对象，而且现在这个 c->querybuf 
                (signed) sdslen(c->querybuf) == c->bulklen+2)
            {
                c->argv[c->argc++] = createObject(REDIS_STRING,c->querybuf);
                sdsIncrLen(c->querybuf,-2); /* remove CRLF */
                c->querybuf = sdsempty();
                /* Assume that if we saw a fat argument we'll see another one
                 * likely... */
                c->querybuf = sdsMakeRoomFor(c->querybuf,c->bulklen+2);
                pos = 0;
            } else {
                // 小对象，直接拷贝得了
                c->argv[c->argc++] =
                    createStringObject(c->querybuf+pos,c->bulklen);
                pos += c->bulklen+2;
            }

            // 清空参数长度
            c->bulklen = -1;

            // 减少还需读入的参数个数
            c->multibulklen--;
        }
    }

    /* Trim to pos */
    // 从 querybuf 中删除已被读取的内容
    if (pos) sdsrange(c->querybuf,pos,-1);

    /* We're done when c->multibulk == 0 */
    // 如果本条命令的所有参数都已读取完，那么返回
    if (c->multibulklen == 0) return REDIS_OK;

    /* Still not read to process the command */
    // 部分数据可能是滞留在了 redis-cli，等待下一个 read 的时候，再进行处理
    return REDIS_ERR;
}

// 处理客户端输入的命令内容，当因为粘包问题导致的 RESP element 读取不完整，也会退出这个函数，等到下一个完整了之后再来读取的
void processInputBuffer(redisClient *c) {

    /* Keep processing while there is something in the input buffer */
    // 尽可能地处理查询缓冲区中的内容
    // 如果读取出现 short read ，那么可能会有内容滞留在读取缓冲区里面
    // 这些滞留内容也许不能完整构成一个符合协议的命令，
    // 需要等待下次读事件的就绪
    // NOTE: 这个 while-loop 就十分灵魂了，当 querybuf 里面有多个 *num 开头的命令，全靠这个 while-loop
    //       将积压在 querybuf 里面的全部 CMD 都处理掉，而不是通过 cron 的方式清空 querybuf
    //       这算是一个处理 buf 中多个 message 的方法之一；
    //       也正是这个 while-loop，是的 redis-server 能够兼容异步的 redis-cli 客户端
    while(sdslen(c->querybuf)) {

        /* Return if clients are paused. */
        // 如果客户端正处于暂停状态，那么直接返回（会导致 querybuf 的内容堆积）
        if (!(c->flags & REDIS_SLAVE) && clientsArePaused()) return;

        /* Immediately abort if the client is in the middle of something. */
        // REDIS_BLOCKED 状态表示客户端正在被阻塞（会导致 querybuf 的内容堆积）
        if (c->flags & REDIS_BLOCKED) return;

        /* REDIS_CLOSE_AFTER_REPLY closes the connection once the reply is
         * written to the client. Make sure to not let the reply grow after
         * this flag has been set (i.e. don't process more commands). */
        // 客户端已经设置了关闭 FLAG ，没有必要处理命令了
        if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

        /* Determine request type when unknown. */
        // 判断 client 所采用的 协议类型 的类型：REDIS_REQ_MULTIBULK、REDIS_REQ_INLINE
        // 两种类型的区别可以在 Redis 的通讯协议上查到：
        // https://redis.io/topics/protocol
        // 简单来说，多条查询是一般客户端发送来的，
        // 而内联查询则是 TELNET 发送来的
        // 而且因为 client-socket 都是绑定 port 的，所以基本上只需要设置一次就好，后续的 read 都是不需要设置的
        if (!c->reqtype) {
            if (c->querybuf[0] == '*') {
                // 多条查询
                c->reqtype = REDIS_REQ_MULTIBULK;
            } else {
                // Sometimes you have only telnet to hand and you need to send a command to the Redis server.
                // 一般只会用于来自 telnet 的连接，避免写太复杂的协议格式
                c->reqtype = REDIS_REQ_INLINE;
            }
        }

        // 将缓冲区中的内容转换成命令，以及 argv
        if (c->reqtype == REDIS_REQ_INLINE) {
            if (processInlineBuffer(c) != REDIS_OK) break;
        } else if (c->reqtype == REDIS_REQ_MULTIBULK) {
            if (processMultibulkBuffer(c) != REDIS_OK) break;
        } else {
            redisPanic("Unknown request type");
        }

        /* Multibulk processing could see a <= 0 length. */
        if (c->argc == 0) {
            resetClient(c);
        } else {
            /* Only reset the client when the command was executed. */
            // 执行命令，并重置客户端，重置为单个命令而生的变量
            if (processCommand(c) == REDIS_OK)
                resetClient(c);
        }
    }   // end of while
}

/*
 * 是 client-socket 发生 read-event 的时候，所采用的 callback
 * 1. 在 client-socket 第一次连接的时候，被 listen-socket accept 之后，
 *   调用 createClient()，并将这个 fd 注册进 epoll-instance 里面，
 *   并登记这个函数为 fd-read-event 对应的 read-event-callback
 * 2. 这个函数将会在 ae.c:aeProcessEvents() 中被调用，通过上面等级的 fd->read-callback 来触发
 * 
 * 读取客户端的发送过来的 RESP (REdis Serialization Protocol)，并保存到 c->querybuf 中
 * 然后在本函数中，通过 processInputBuffer() 来将 RESP 转换成 client 中的 argc 跟 argv
 */
// test case: zadd key-string 1 member-1 2 member-2 3 member-3 4 member-4 5 member-5
/**
 * RESP info: *12\r\n
 *            $4\r\nzadd\r\n
 *            $10\r\nkey-string\r\n
 *            $1\r\n1\r\n
 *            $8\r\nmember-1\r\n
 *            $1\r\n2\r\n
 *            $8\r\nmember-2\r\n
 *            $1\r\n3\r\n
 *            $8\r\nmember-3\r\n
 *            $1\r\n4\r\n
 *            $8\r\nmember-4\r\n
 *            $1\r\n5\r\n
 *            $8\r\nmember-5\r\n
*/
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = (redisClient*) privdata;
    int nread, readlen;
    size_t qblen;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    // 设置服务器的当前客户端
    server.current_client = c;
    
    // 读入长度（默认为 16 MB）
    // 那也就是意味着：当这个 CMD 的任何一部分、总长度超过了 REDIS_IOBUF_LEN，
    // 都将会再次从 epoll-instance 那里，再次触发本函数，因为一次 read，并不能顺利的将完整的 RESP element 读取出来
    // 必须执行两次，这也就有了下面的那个常常的 if 判断
    readlen = REDIS_IOBUF_LEN;

    /* If this is a multi bulk request（在解析内容之前就已经知道 req-type ？那是因为上一次 read 的时候，就已经设置好了）,
     * and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument. */
    // TODO:(DONE, 看下面 if 中的注释) 这玩意想要干嘛？为什么一个刚过来的 client，会有 REDIS_REQ_MULTIBULK ？
    if (c->reqtype == REDIS_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1
        && c->bulklen >= REDIS_MBULK_BIG_ARG)
    {
        // 发生了这个 if 判断，必然是因为：其中一个小命令，实在是太长了，长到要 read 多次，才能完整读取
        // 因为这个超长的小命令具体多长，不解析 RESP 是没办法知道的；
        // 你要是每次 read 都拓展一下 querybuf，你只会拥有巨大的 data-copy
        // 而且每次 copy，前面的那些数据都是一样的，会发生大量的无意义重复拷贝
        // 为了解决这个无意义重复拷贝的问题：
        // 在 processMultibulkBuffer() 函数里面，面对 REDIS_MBULK_BIG_ARG 的情况
        // 直接将这一小节的命令长度读取出来（得知这个命令的总长度），然后直接将这个 sds 拓展为 RESP 中 $ 引领的长度
        // 这样就可以避免因为不知道可能会有多长，进而导致的多次拓展 sds，引发的大量 copy
        int remaining = (unsigned)(c->bulklen+2)-sdslen(c->querybuf);

        if (remaining < readlen) readlen = remaining;
    }

    // 获取查询缓冲区当前内容的长度
    // 如果读取出现 short read ，那么可能会有内容滞留在读取缓冲区里面
    // 这些滞留内容也许不能完整构成一个符合协议的命令，
    qblen = sdslen(c->querybuf);
    // 也是 lazy 的方式更新 buff 的峰值，每次从 socket read 之前，检查一次，看看要不要更新
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;
    // 为 querybuf 分配空间，每次默认读取 16 MB 的 data；REDIS_IOBUF_LEN
    c->querybuf = sdsMakeRoomFor(c->querybuf, readlen); // 要是 readlen 小于原本的 sdslen(c->querybuf)，sdsMakeRoomFor() 将不会生效

    // 读入内容，并存放在 querybuf 中，最多读取 REDIS_IOBUF_LEN（遇上了 REDIS_MBULK_BIG_ARG 可能除外） data
    // 一定要在 c->querybuf+qblen，这样才能避免覆盖还没有来得及处理的 RESP 内容
    nread = read(fd, c->querybuf+qblen, readlen);

    if (nread == -1) {
        // 读入出错
        if (errno == EAGAIN) {
            nread = 0;  // TODO: 在 redis 的场景下，这个 EAGAIN 究竟意味着什么，数据读出来了没有？
            // 说明 read 没有读到任何数据，而且是非阻塞的关系，所以立马返回，并设置 EAGAIN
        } else {
            redisLog(REDIS_VERBOSE, "Reading from client: %s",strerror(errno));
            freeClient(c);
            return;
        }
    } else if (nread == 0) {
        // 遇到 EOF，client 关闭，会作为一个单独的 read 事件，由 epoll-instance 触发
        // 然后 close(client_socket_fd)
        redisLog(REDIS_VERBOSE, "Client closed connection");
        freeClient(c);
        return;
    }

    if (nread) {
        // 根据内容，更新查询缓冲区（SDS） free 和 len 属性
        // 并将 '\0' 正确地放到内容的最后
        sdsIncrLen(c->querybuf,nread);
        // 记录服务器和客户端最后一次互动的时间
        c->lastinteraction = server.unixtime;
        // 如果客户端是 master 的话(slave --> master 时用的 client)，更新它的复制偏移量
        if (c->flags & REDIS_MASTER) c->reploff += nread;
    } else {
        // 在 nread == -1 且 errno == EAGAIN 时运行
        // 还不至于要走到崩溃报告的程度，所以设置为 NULL
        server.current_client = NULL;
        return;
    }

    // querybuf 长度超出服务器所允许的最大缓冲区长度
    // 清空缓冲区并释放客户端
    // REDIS_MAX_QUERYBUF_LEN = 1 GB；之所以会不停堆积，是因为 redis-server 没有消费掉发送过来的 RESP 内容
    if (sdslen(c->querybuf) > server.client_max_querybuf_len) {
        sds ci = catClientInfoString(sdsempty(),c), bytes = sdsempty();

        bytes = sdscatrepr(bytes,c->querybuf,64);
        redisLog(REDIS_WARNING,"Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)", ci, bytes);
        sdsfree(ci);
        sdsfree(bytes);
        freeClient(c);
        return;
    }

    // 函数会 wile-loop 的死循环执行，直到 querybuf 中的所有内容都被处理完为止
    // 特殊情况的会中断对 querybuf 的处理：
    // client 仍在 blocked 状态、RESP 内容处理出错（这个通常是因为 stream 式的 TCP 协议，导致粘包，read 没有读取到完整的 RESP element）
    // 到这里为止，RESP 的内容已经成功\部分成功从 socket-buf 里面，读取到了 c->querybuf 中
    // c->querybuf == "*12\r\n$4\r\nzadd\r\n$10\r\nkey-string\r\n$1\r\n1\r\n$8\r\nmember-1\r\n
    //                 $1\r\n2\r\n$8\r\nmember-2\r\n$1\r\n3\r\n$8\r\nmember-3\r\n$1\r\n4\r\n
    //                 $8\r\nmember-4\r\n$1\r\n5\r\n$8\r\nmember-5\r\n"
    processInputBuffer(c);

    server.current_client = NULL;
}

// 获取所有客户端中，目前最大的一块输入 querybuf 的大小、等待输出 reply 的长度
// 可以一定程度上衡量 redis-server 跟这个 redis-cli client 之间的消息交互负载情况（拥堵程度、被调度时延）
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer) {
    redisClient *c;
    listNode *ln;
    listIter li;
    unsigned long lol = 0, bib = 0;

    listRewind(server.clients,&li); // 设置 iter，准备迭代
    while ((ln = listNext(&li)) != NULL) {
        c = listNodeValue(ln);

        if (listLength(c->reply) > lol) lol = listLength(c->reply); // client_longest_output_list
        if (sdslen(c->querybuf) > bib) bib = sdslen(c->querybuf);   // client_biggest_input_buf
    }
    *longest_output_list = lol;
    *biggest_input_buffer = bib;
}

/* This is a helper function for genClientPeerId().
 * It writes the specified ip/port to "peerid" as a null termiated string
 * in the form ip:port if ip does not contain ":" itself, otherwise
 * [ip]:port format is used (for IPv6 addresses basically). */
void formatPeerId(char *peerid, size_t peerid_len, char *ip, int port) {
    if (strchr(ip,':'))
        snprintf(peerid,peerid_len,"[%s]:%d",ip,port);  // ipv6
    else
        snprintf(peerid,peerid_len,"%s:%d",ip,port);    // ipv4
}

/* A Redis "Peer ID" is a colon separated ip:port pair.
 * For IPv4 it's in the form x.y.z.k:pork, example: "127.0.0.1:1234".
 * For IPv6 addresses we use [] around the IP part, like in "[::1]:1234".
 * For Unix socekts we use path:0, like in "/tmp/redis:0".
 *
 * A Peer ID always fits inside a buffer of REDIS_PEER_ID_LEN bytes, including
 * the null term.
 *
 * The function returns REDIS_OK on succcess, and REDIS_ERR on failure.
 *
 * On failure the function still populates 'peerid' with the "?:0" string
 * in case you want to relax error checking or need to display something
 * anyway (see anetPeerToString implementation for more info). */
// example: "127.0.0.1:57702"
int genClientPeerId(redisClient *client, char *peerid, size_t peerid_len) {
    char ip[REDIS_IP_STR_LEN];
    int port;

    if (client->flags & REDIS_UNIX_SOCKET) {
        /* Unix socket client. */
        snprintf(peerid,peerid_len,"%s:0",server.unixsocket);
        return REDIS_OK;
    } else {
        /* TCP client. */
        // 显示的是：这个 client-socket 在 redis-server 里面的记录信息
        int retval = anetPeerToString(client->fd,ip,sizeof(ip),&port);
        formatPeerId(peerid,peerid_len,ip,port);
        return (retval == -1) ? REDIS_ERR : REDIS_OK;
    }
}

/* This function returns the client peer id, by creating and caching it
 * if client->perrid is NULL, otherwise returning the cached value.
 * The Peer ID never changes during the life of the client, however it
 * is expensive to compute. */
char *getClientPeerId(redisClient *c) {
    char peerid[REDIS_PEER_ID_LEN];

    if (c->peerid == NULL) {
        genClientPeerId(c,peerid,sizeof(peerid));
        c->peerid = sdsnew(peerid);
    }
    return c->peerid;
}

/* Concatenate a string representing the state of a client in an human
 * readable format, into the sds string 's'. */
// 获取客户端的各项信息，将它们储存到 sds 值 s 里面，并返回。
sds catClientInfoString(sds s, redisClient *client) {
    char flags[16], events[3], *p;
    int emask;

    p = flags;
    if (client->flags & REDIS_SLAVE) {
        if (client->flags & REDIS_MONITOR)
            *p++ = 'O';
        else
            *p++ = 'S';
    }
    if (client->flags & REDIS_MASTER) *p++ = 'M';
    if (client->flags & REDIS_MULTI) *p++ = 'x';
    if (client->flags & REDIS_BLOCKED) *p++ = 'b';
    if (client->flags & REDIS_DIRTY_CAS) *p++ = 'd';
    if (client->flags & REDIS_CLOSE_AFTER_REPLY) *p++ = 'c';
    if (client->flags & REDIS_UNBLOCKED) *p++ = 'u';
    if (client->flags & REDIS_CLOSE_ASAP) *p++ = 'A';
    if (client->flags & REDIS_UNIX_SOCKET) *p++ = 'U';
    if (client->flags & REDIS_READONLY) *p++ = 'r';
    if (p == flags) *p++ = 'N';
    *p++ = '\0';

    emask = client->fd == -1 ? 0 : aeGetFileEvents(server.el,client->fd);
    p = events;
    if (emask & AE_READABLE) *p++ = 'r';
    if (emask & AE_WRITABLE) *p++ = 'w';
    *p = '\0';
    return sdscatfmt(s,
        "addr=%s fd=%i name=%s age=%I idle=%I flags=%s db=%i sub=%i psub=%i multi=%i qbuf=%U qbuf-free=%U obl=%U oll=%U omem=%U events=%s cmd=%s",
        getClientPeerId(client),
        client->fd,
        client->name ? (char*)client->name->ptr : "",
        (long long)(server.unixtime - client->ctime),
        (long long)(server.unixtime - client->lastinteraction),
        flags,
        client->db->id,
        (int) dictSize(client->pubsub_channels),
        (int) listLength(client->pubsub_patterns),
        (client->flags & REDIS_MULTI) ? client->mstate.count : -1,
        (unsigned long long) sdslen(client->querybuf),
        (unsigned long long) sdsavail(client->querybuf),
        (unsigned long long) client->bufpos,
        (unsigned long long) listLength(client->reply),
        (unsigned long long) getClientOutputBufferMemoryUsage(client),
        events,
        client->lastcmd ? client->lastcmd->name : "NULL");
}

/*
 * 打印出所有连接到服务器的客户端的信息
 */
sds getAllClientsInfoString(void) {
    listNode *ln;
    listIter li;
    redisClient *client;
    sds o = sdsempty();

    o = sdsMakeRoomFor(o,200*listLength(server.clients));
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client = listNodeValue(ln);
        o = catClientInfoString(o,client);
        o = sdscatlen(o,"\n",1);
    }
    return o;
}

/*
 * CLIENT 命令的实现
 */
void clientCommand(redisClient *c) {
    listNode *ln;
    listIter li;
    redisClient *client;

    // CLIENT list
    if (!strcasecmp(c->argv[1]->ptr,"list") && c->argc == 2) {
        sds o = getAllClientsInfoString();
        addReplyBulkCBuffer(c,o,sdslen(o));
        sdsfree(o);

    // CLIENT kill
    } else if (!strcasecmp(c->argv[1]->ptr,"kill") && c->argc == 3) {

        // 遍历客户端链表，并杀死指定地址的客户端
        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            char *peerid;

            client = listNodeValue(ln);
            peerid = getClientPeerId(client);
            if (strcmp(peerid,c->argv[2]->ptr) == 0) {
                addReply(c,shared.ok);
                if (c == client) {
                    client->flags |= REDIS_CLOSE_AFTER_REPLY;
                } else {
                    freeClient(client);
                }
                return;
            }
        }
        addReplyError(c,"No such client");

    // CLIENT setname 设置客户端名字
    } else if (!strcasecmp(c->argv[1]->ptr,"setname") && c->argc == 3) {
        int j, len = sdslen(c->argv[2]->ptr);
        char *p = c->argv[2]->ptr;

        /* Setting the client name to an empty string actually removes
         * the current name. */
        // 名字为空时，清空客户端的名字
        if (len == 0) {
            if (c->name) decrRefCount(c->name);
            c->name = NULL;
            addReply(c,shared.ok);
            return;
        }

        /* Otherwise check if the charset is ok. We need to do this otherwise
         * CLIENT LIST format will break. You should always be able to
         * split by space to get the different fields. */
        for (j = 0; j < len; j++) {
            if (p[j] < '!' || p[j] > '~') { /* ASCII is assumed. */
                addReplyError(c,
                    "Client names cannot contain spaces, "
                    "newlines or special characters.");
                return;
            }
        }
        if (c->name) decrRefCount(c->name);
        c->name = c->argv[2];
        incrRefCount(c->name);
        addReply(c,shared.ok);

    // CLIENT getname 获取客户端的名字
    } else if (!strcasecmp(c->argv[1]->ptr,"getname") && c->argc == 2) {
        if (c->name)
            addReplyBulk(c,c->name);
        else
            addReply(c,shared.nullbulk);
    } else if (!strcasecmp(c->argv[1]->ptr,"pause") && c->argc == 3) {
        long long duration;

        if (getTimeoutFromObjectOrReply(c,c->argv[2],&duration,UNIT_MILLISECONDS)
                                        != REDIS_OK) return;
        pauseClients(duration);
        addReply(c,shared.ok);
    } else {
        addReplyError(c, "Syntax error, try CLIENT (LIST | KILL ip:port | GETNAME | SETNAME connection-name)");
    }
}

/* Rewrite the command vector of the client. All the new objects ref count
 * is incremented. The old command vector is freed, and the old objects
 * ref count is decremented. */
// 修改客户端的参数数组
// TODO: 看完 传播 相关的内容再回来看
void rewriteClientCommandVector(redisClient *c, int argc, ...) {
    va_list ap;
    int j;
    robj **argv; /* The new argument vector */

    // 创建新参数
    argv = zmalloc(sizeof(robj*)*argc);
    va_start(ap,argc);
    for (j = 0; j < argc; j++) {
        robj *a;
        
        a = va_arg(ap, robj*);
        argv[j] = a;
        incrRefCount(a);
    }
    /* We free the objects in the original vector at the end, so we are
     * sure that if the same objects are reused in the new vector the
     * refcount gets incremented before it gets decremented. */
    // 释放旧参数
    for (j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
    zfree(c->argv);

    /* Replace argv and argc with our new versions. */
    // 用新参数替换
    c->argv = argv;
    c->argc = argc;
    c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
    redisAssertWithInfo(c,NULL,c->cmd != NULL);
    va_end(ap);
}

/* Rewrite a single item in the command vector.
 * The new val ref count is incremented, and the old decremented. */
// 修改单个参数
void rewriteClientCommandArgument(redisClient *c, int i, robj *newval) {
    robj *oldval;
   
    redisAssertWithInfo(c,NULL,i < c->argc);
    oldval = c->argv[i];
    c->argv[i] = newval;
    incrRefCount(newval);
    decrRefCount(oldval);

    /* If this is the command name make sure to fix c->cmd. */
    if (i == 0) {
        c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
        redisAssertWithInfo(c,NULL,c->cmd != NULL);
    }
}

/* This function returns the number of bytes that Redis is virtually
 * using to store the reply still not read by the client.
 * It is "virtual" since the reply output list may contain objects that
 * are shared and are not really using additional memory.
 *
 * 函数返回客用于保存目前仍未返回给客户端的回复的虚拟大小（以字节为单位）。
 * 之所以说是虚拟大小，因为回复列表中可能有包含共享的对象。
 *
 * The function returns the total sum of the length of all the objects
 * stored in the output list, plus the memory used to allocate every
 * list node. The static reply buffer is not taken into account since it
 * is allocated anyway.
 *
 * 函数返回回复列表中所包含的全部对象的体积总和，
 * 加上列表节点所分配的空间。
 * 静态回复缓冲区不会被计算在内，因为它总是会被分配的。
 *
 * Note: this function is very fast so can be called as many time as
 * the caller wishes. The main usage of this function currently is
 * enforcing the client output length limits. 
 *
 * 注意：这个函数的速度很快，所以它可以被随意地调用多次。
 * 这个函数目前的主要作用就是用来强制客户端输出长度限制。
 */
// 是一个不太精确的统计（针对 reply-list 缓冲区，但是没有考虑共享的内存）
unsigned long getClientOutputBufferMemoryUsage(redisClient *c) {
    unsigned long list_item_size = sizeof(listNode)+sizeof(robj);

    return c->reply_bytes + (list_item_size*listLength(c->reply));
}

/* Get the class of a client, used in order to enforce limits to different
 * classes of clients.
 *
 * 获取客户端的类型，用于对不同类型的客户端应用不同的限制。
 *
 * The function will return one of the following:
 * 
 * 函数将返回以下三个值的其中一个：
 *
 * REDIS_CLIENT_LIMIT_CLASS_NORMAL -> Normal client
 *                                    普通客户端
 *
 * REDIS_CLIENT_LIMIT_CLASS_SLAVE  -> Slave or client executing MONITOR command
 *                                     slave ，或者正在执行 MONITOR 命令的客户端
 *
 * REDIS_CLIENT_LIMIT_CLASS_PUBSUB -> Client subscribed to Pub/Sub channels
 *                                    正在进行订阅操作（SUBSCRIBE/PSUBSCRIBE）的客户端
 */
int getClientLimitClass(redisClient *c) {
    if (c->flags & REDIS_SLAVE) return REDIS_CLIENT_LIMIT_CLASS_SLAVE;
    if (dictSize(c->pubsub_channels) || listLength(c->pubsub_patterns))
        return REDIS_CLIENT_LIMIT_CLASS_PUBSUB;
    return REDIS_CLIENT_LIMIT_CLASS_NORMAL;
}

// 根据名字，获取客户端的类型常量
int getClientLimitClassByName(char *name) {
    if (!strcasecmp(name,"normal")) return REDIS_CLIENT_LIMIT_CLASS_NORMAL;
    else if (!strcasecmp(name,"slave")) return REDIS_CLIENT_LIMIT_CLASS_SLAVE;
    else if (!strcasecmp(name,"pubsub")) return REDIS_CLIENT_LIMIT_CLASS_PUBSUB;
    else return -1;
}

// 根据客户端的类型，获取名字
char *getClientLimitClassName(int class) {
    switch(class) {
    case REDIS_CLIENT_LIMIT_CLASS_NORMAL:   return "normal";
    case REDIS_CLIENT_LIMIT_CLASS_SLAVE:    return "slave";
    case REDIS_CLIENT_LIMIT_CLASS_PUBSUB:   return "pubsub";
    default:                                return NULL;
    }
}

/* The function checks if the client reached output buffer soft or hard
 * limit, and also update the state needed to check the soft limit as
 * a side effect.
 *
 * 这个函数检查客户端是否达到了输出缓冲区的软性（soft）限制或者硬性（hard）限制，
 * 并在到达软限制时，对客户端进行标记。
 *
 * Return value: non-zero if the client reached the soft or the hard limit.
 *               Otherwise zero is returned. 
 *
 * 返回值：到达软性限制或者硬性限制时，返回非 0 值。
 *         否则返回 0 。
 */
/** TODO:(DONE) output buffer soft\hard limit 分别是要干什么的？这两个 limit 将会有什么效果？
 * 
 * There are two kind of limits Redis uses:
 * 1. The hard limit is a fixed limit that when reached will make Redis closing
 *    the client connection as soon as possible.
 * 2. The soft limit instead is a limit that depends on the time, for instance
 *    a soft limit of 32 megabytes per 10 seconds means that if the client has
 *    an output buffer bigger than 32 megabytes for, continuously, 10 seconds,
 *    the connection gets closed.
 * 
 * 因为 redisClient 的 outbuf 不作出限制的话，将会无限制增长，无上限的占用内存
 * When the limit is reached the client connection is closed and the event logged in the Redis log file.
 */
int checkClientOutputBufferLimits(redisClient *c) {
    int soft = 0, hard = 0, class;

    // 获取客户端回复缓冲区的大小
    unsigned long used_mem = getClientOutputBufferMemoryUsage(c);

    // 获取客户端的种类，从而得知相应的 buf 限制大小
    class = getClientLimitClass(c);

    /* 先检查，后操作，代码的逻辑进行分层处理，这样能够避免代码的嵌套过深，相当于增加了一层控制层，虽然引入了中间变量 */
    // 检查硬性限制
    if (server.client_obuf_limits[class].hard_limit_bytes &&    // 配置了 class 类型的 client hard_limit_bytes
        used_mem >= server.client_obuf_limits[class].hard_limit_bytes)  // 触发了 class 类型的 client hard_limit_bytes
        hard = 1;

    // 检查软性限制
    if (server.client_obuf_limits[class].soft_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].soft_limit_bytes)
        soft = 1;

    /* We need to check if the soft limit is reached continuously for the
     * specified amount of seconds. */
    // 达到软性限制（在一定时间内，不能够写入超过 xxx MB 的数据）
    if (soft) {

        // 第一次达到软性限制
        if (c->obuf_soft_limit_reached_time == 0) {
            // 记录时间
            c->obuf_soft_limit_reached_time = server.unixtime;
            // 关闭软性限制 flag
            soft = 0; /* First time we see the soft limit reached */

        // 再次达到软性限制
        } else {
            // 软性限制的连续时长
            time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;

            if (elapsed <= server.client_obuf_limits[class].soft_limit_seconds) {
                // 如果没有超过最大连续时长的话，那么关闭软性限制 flag
                soft = 0; /* The client still did not reached the max number of
                             seconds for the soft limit to be considered
                             reached. */
            }
            // else { 
            //     如果超过了最大连续时长的话，软性限制 flag 就会被保留
            //     实际上，这时候之所以要 soft = 1 保持不变，是因为：
            //     都已经过了 soft_limit_seconds 这么久了，你的 buf 里面还有这么多数据
            //     这不很显然这个 client 有问题（网络、程序），继续向这个 client 写入数据，只会占用更多的内存
            //     所以暂时不处理这个 client 了，处理了也是白费力气，对方压根就接收不到数据
            //
            //     而作为检测依据的 used_mem，则会随着 sendReplyToClient() 函数的调用，慢慢减小
            //     sendReplyToClient() 的成功调用，那就看 socket-send-buf 能不能空出来，顺利的容纳数据了
            // }
        }
    } else {
        // 未达到软性限制，或者已脱离软性限制，那么清空软性限制的进入时间
        c->obuf_soft_limit_reached_time = 0;
    }

    // 一旦需要 soft || hard == 1, 将意味着这个 client 需要进行 异步关闭
    return soft || hard;
}

/* Asynchronously close a client if soft or hard limit is reached on the
 * output buffer size. The caller can check if the client will be closed
 * checking if the client REDIS_CLOSE_ASAP flag is set.
 *
 * 如果客户端达到缓冲区大小的软性或者硬性限制，那么打开客户端的 ``REDIS_CLOSE_ASAP`` 状态，
 * 让服务器异步地关闭客户端。
 *
 * Note: we need to close the client asynchronously because this function is
 * called from contexts where the client can't be freed safely, i.e. from the
 * lower level functions pushing data inside the client output buffers. 
 *
 * 注意：
 * 我们不能直接关闭客户端，而要异步关闭的原因是客户端正处于一个不能被安全地关闭的上下文中。
 * 比如说，可能有底层函数正在推入数据到客户端的输出缓冲区里面。      
 */
void asyncCloseClientOnOutputBufferLimitReached(redisClient *c) {
    redisAssert(c->reply_bytes < ULONG_MAX-(1024*64));

    // 已经被标记了
    if (c->reply_bytes == 0 || c->flags & REDIS_CLOSE_ASAP) return;

    // 检查限制
    if (checkClientOutputBufferLimits(c)) {
        sds client = catClientInfoString(sdsempty(),c); // 取出这个 client 的绝大部分状态信息，然后填装到 sds 里面，准备输出日志信息

        // 异步关闭
        // 1. 设置 REDIS_CLOSE_ASAP；
        // 2. 把这个 client 加入 server.clients_to_close 这个 list 里面，进行异步关闭；
        // 3. 等待周期执行 serverCron()，调用 freeClientsInAsyncFreeQueue() 完成异步的关闭
        freeClientAsync(c);
        redisLog(REDIS_WARNING,"Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        sdsfree(client);
    }
}

/* Helper function used by freeMemoryIfNeeded() in order to flush slaves
 * output buffers without returning control to the event loop. */
// freeMemoryIfNeeded() 函数的辅助函数，
// 用于在不进入事件循环的情况下，冲洗所有 slave 的输出缓冲区。
// TODO: 看完主从之后看这个
void flushSlavesOutputBuffers(void) {
    listIter li;
    listNode *ln;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = listNodeValue(ln);
        int events;

        events = aeGetFileEvents(server.el,slave->fd);
        if (events & AE_WRITABLE &&
            slave->replstate == REDIS_REPL_ONLINE &&
            listLength(slave->reply))
        {
            sendReplyToClient(server.el,slave->fd,slave,0);
        }
    }
}

/* Pause clients up to the specified unixtime (in ms). While clients
 * are paused no command is processed from clients, so the data set can't
 * change during that time.
 *
 * However while this function pauses normal and Pub/Sub clients, slaves are
 * still served, so this function can be used on server upgrades where it is
 * required that slaves process the latest bytes from the replication stream
 * before being turned to masters.
 *
 * This function is also internally used by Redis Cluster for the manual
 * failover procedure implemented by CLUSTER FAILOVER.
 *
 * The function always succeed, even if there is already a pause in progress.
 * In such a case, the pause is extended if the duration is more than the
 * time left for the previous duration. However if the duration is smaller
 * than the time left for the previous pause, no change is made to the
 * left duration. */
// 暂停客户端，让服务器在指定的时间内不再接受被暂停客户端发来的命令
// 可以用于系统更新，并在内部由 CLUSTER FAILOVER 命令使用。
void pauseClients(mstime_t end) {

    // 设置暂停时间
    if (!server.clients_paused || end > server.clients_pause_end_time)
        server.clients_pause_end_time = end;

    // 打开客户端的“已被暂停”标志
    server.clients_paused = 1;
}

/* Return non-zero if clients are currently paused. As a side effect the
 * function checks if the pause time was reached and clear it. */
 // 判断服务器目前被暂停客户端的数量，没有任何客户端被暂停时，返回 0 。
// TODO: 看完切片部署，再回来看这个函数，看看 clients_paused 跟 unblocked_clients 的关系
int clientsArePaused(void) {
    if (server.clients_paused && server.clients_pause_end_time < server.mstime) {
        listNode *ln;
        listIter li;
        redisClient *c;

        server.clients_paused = 0;

        /* Put all the clients in the unblocked clients queue in order to
         * force the re-processing of the input buffer if any. */
        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            c = listNodeValue(ln);

            if (c->flags & REDIS_SLAVE) continue;
            listAddNodeTail(server.unblocked_clients,c);
        }
    }
    return server.clients_paused;
}

/* This function is called by Redis in order to process a few events from
 * time to time while blocked into some not interruptible operation.
 * This allows to reply to clients with the -LOADING error while loading the
 * data set at startup or after a full resynchronization with the master
 * and so forth.
 *
 * It calls the event loop in order to process a few events. Specifically we
 * try to call the event loop for times as long as we receive acknowledge that
 * some event was processed, in order to go forward with the accept, read,
 * write, close sequence needed to serve a client.
 *
 * The function returns the total number of events processed. */
// 让服务器在被阻塞的情况下，仍然处理某些事件。例如重新载入 AOF
// TODO: 看完主从之后看这个
int processEventsWhileBlocked(void) {
    int iterations = 4; /* See the function top-comment. */
    int count = 0;
    while (iterations--) {
        int events = aeProcessEvents(server.el, AE_FILE_EVENTS|AE_DONT_WAIT);
        if (!events) break;
        count += events;
    }
    return count;
}
