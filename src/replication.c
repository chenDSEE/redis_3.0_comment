/**
 * asynchronous replication(异步 replication)
 * synchronous replication(同步 replication，通过 WAIT command)
*/

/* Asynchronous replication implementation.
 *
 * 异步复制实现
 *
 * The replica will automatically reconnect to the master every time the link breaks, 
 * and will attempt to be an exact copy of it regardless of what happens to the master.
 * 
 * This system works using three main mechanisms:
 * 1. When a master and a replica instances are well-connected, the master keeps
 *    the replica updated by sending a stream of commands to the replica, in order to
 *    replicate the effects on the dataset happening in the master side due to:
 *    client writes, keys expired or evicted, any other action changing the master dataset.
 *    以复制主实例上发生的影响(effect)，这些影响包括：客户端写入、密钥过期或被驱逐、任何其他改变主数据集的行为。
 * 
 * 2. When the link between the master and the replica breaks, for network issues
 *    or because a timeout is sensed in the master or the replica, the replica
 *    reconnects and attempts to proceed with a partial resynchronization: 
 *    it means that it will try to just obtain the part of the stream of commands
 *    it missed during the disconnection.
 * 
 * 3. When a partial resynchronization is not possible, the replica will ask for
 *    a full resynchronization. This will involve a more complex process in which
 *    the master needs to create a snapshot of all its data, send it to the replica,
 *    and then continue sending the stream of commands as the dataset changes.
 * 
 * NOTE:
 * 1. However, Redis replicas asynchronously acknowledge the amount of data they
 *    received periodically with the master.
 * 
 * 2. Redis uses asynchronous replication, with asynchronous replica-to-master
 *    acknowledges of the amount of data processed.
 * 
 * 3. Replicas are able to accept connections from other replicas. Aside from
 *    connecting a number of replicas to the same master, replicas can also
 *    be connected to other replicas in a cascading-like structure.
 *    Since Redis 4.0, all the sub-replicas will receive exactly the same replication stream from the master.
 * 
 * 4. Redis replication is non-blocking on the master side. This means that the
 *    master will continue to handle queries when one or more replicas perform
 *    the initial synchronization or a partial resynchronization.
 * 
 * 5. Replication is also largely non-blocking on the replica side. 
 *    While the replica is performing the initial synchronization, it can handle
 *    queries using the old version of the dataset, assuming you configured Redis
 *    to do so in redis.conf. Otherwise, you can configure Redis replicas to
 *    return an error to clients if the replication stream is down. However,
 *    after the initial sync, the old dataset must be deleted and the new one must be loaded.
 *    The replica will block incoming connections during this brief window
 *    (that can be as long as many seconds for very large datasets). 
 *    Since Redis 4.0 it is possible to configure Redis so that the deletion of
 *    the old data set happens in a different thread, however loading the new
 *    initial dataset will still happen in the main thread and block the replica.
 * 
 *    replication 本身是非阻塞的，但是 loading the new initial dataset 是阻塞的
 * 
 * 6. It is possible to use replication to avoid the cost of having the master
 *    writing the full dataset to disk: a typical technique involves configuring
 *    your master redis.conf to avoid persisting to disk at all, then connect a
 *    replica configured to save from time to time, or with AOF enabled.
 *    However this setup must be handled with care, since a restarting master will
 *    start with an empty dataset: if the replica tries to synchronize with it,
 *    the replica will be emptied as well.
 * 
 *    DANGEROUS !!! you will get an empty redis-server cluster, and all data will be  deleted !!!
 *    1) When Redis Sentinel is used for high availability, also turning off
 *       persistence on the master, together with auto restart of the process, is dangerous. 
 *    2) masters node with persistence turned off and host computer configured to auto restart,
 *       this situation is dangerous
 * 
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

/**
 *  Replication ID explained:
 *  format:
 *  Replication ID, offset
 * 
 *  In the previous section we said that if two instances have the same replication ID 
 *  and replication offset, they have exactly the same data.
 * */

#include "redis.h"

#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>

void replicationDiscardCachedMaster(void);
void replicationResurrectCachedMaster(int newfd);
void replicationSendAck(void);

// TODO:（DONE） 总结一下: 同步阶段、命令传播阶段
/* 1. [SLAVE]:
 *    通过 SLAVEOF 5.5.75.110 6379，采用非阻塞的方式，异步 connect master
 *    然后把这个不知道可否成功 connect 的 socket_fd 注册进 epoll-instance
 * 2. [MASTER]:
 *    epoll-instance 检查到了新的连接，完成三次握手
 *    [SLVAE]:
 *    epoll 通知 connect-socket-fd 可以 handle，因为是异步的关系，需要检查这个 connect-socket-fd
 *    是否成功三次握手
 * 3. [SLAVE]:
 *    发送 psync CMD 尝试跟 master 同步数据库内容
 *    [MASTER]:
 *    1) 发现这个 slave 是一个生面孔，连 matser 的 runid 都不知道，绝对是 full sync，开始 full sync 的流程
 *    2) 同时开始暗搓搓的准备 PSYNC 会使用到的 backlog 资源
 * 
 *    ..... 省略 slave master 之间的握手确认细节 ........
 * 
 * -------------- full sync ------------------------
 * 4. [MASTER]:
 *    1) 执行 bgsave，让子进程自个儿生成 RDB
 *    2) 父进程继续处理新的 CMD，并且把新的 CMD 缓冲在 slave client 的 buf 里面（但不发送）
 *       以便待会追加数据
 *    3) 子进程完成了 RDB 文件，通知父进程，父进程开始将 RDB 文件发送给 slave
 *    [SLAVE]:
 *    1) 全程等待 master 准备好 RDB，并会回应 "\n" 的定期检查
 *    2) 接受来自 master 的 RDB 文件，并加载进自己的进程内存中
 * 5. master <---> slave 完成 RDB 文件的同步
 *    [MASTER]:
 *    1) 开始把 4.2 里面的 client->buf 数据，追加发送给 slave，这时候走的是正常的 addReply
 *       所以即使来了新的 CMD，也是加在 buf 后面的
 *    2) 剩下的就是，master 每次 CMD 都存一下 backlog，传播一下 repl 的数据内容给 salve 就好了
 *
 * ...... 发生了网络波动，master <---> slave 短暂失联 ......
 * -------------- PSYNC ------------------
 * 6. [SLAVE]\[MASTER]:
 *    断开相应的 socket-fd
 *    [SLAVE]:
 *    1) 不停尝试 connect master，成功后，再次开始三次握手的流程
 *    2) 握手成功，发送 PYSNC + master_runid + backlog_global_index 给 master
 *    [MASTER]:
 *    根据收到的 master_runid + backlog_global_index 来看看能不能跟这个 slave 进行 PSYNC
 * 7. [MASTER]:
 *    可以 PSYNC 的话，就把相应的数据，从 backlog 里面 addReply，等候 write 就绪在发送出去
 *    没错，不需要花里胡巧的特殊处理，放进 addReply 里面就好了
 * 8. 一如往常的正常 repl 传播
 */


// TODO:(DONE) slave 跟 master 直接的连接时短链接吗(长连接，GDB 太久了，心跳超时后，master、slave 都会各自主动断开)？
// TODO:（DONE） slave 跟 master 之间的心跳机制。定期收发 PING、PONG

/**
 * backlog 跟 client->buf 有什么区别？
 * backlog 是作为一个缓冲使用的，暂存所有 master --> slave 的 CMD
 * 当发生了 master <--> slave 短暂失联时，这个 master 就会利用这个 backlog 里面暂存的历史数据
 * 尝试跟 slave 进行 PSYNC，快速同步两者的数据库内容
 * 
 * client->buf 则是在完成同步之后，master 同步 CMD 给 slave 的方法；
 * 另一种使用情况是：master 在进行 bgsave --> 发送完 RDB 给 slave（full sync）期间依旧正常接受请求
 * 然后会把这期间的请求，保存至 client->buf 里面，然后等到发送完 RDB，client 的 write 解禁之后，
 * 就会开始向 slave 发送 client->buf 的数据，这样就能彻底同步了
 * （这个过程中，master 依旧会把 CMD 放进 backlog 里面，但是 backlog 不会在这个过程中被使用）
*/

/* ---------------------------------- MASTER -------------------------------- */

/* backlog 环形缓冲分类讨论：
 * 前提：这个环形缓冲是惰性丢弃数据的，哪怕 slave ack 了，也不删除旧的数据
 *      等到新的数据太多了，才会通过新数据覆盖旧数据的方式，被动删除旧数据（这点跟滑动窗口不一样）
 *
 * 情况一：没有溢出，也没有发生回旋
 * master_repl_offset 全部历史数据中最末尾的那一位，同时也是 当前有效数据 的最末尾那一位
 * repl_backlog_off   全部历史数据中最开头的那一位，同时也是 当前有效数据 的最开头那一位
 * 
 *   |<--------------------- repl_backlog_size --------------------->|
 *   |<----- histlen ---->|
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *   | 0| 1| 2| 3| 4| 5| 6|  |  |  |  |  |  |  |  |  |  |  |  |  |  |
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *                      ^                     
 *                      |
 *              repl_backlog_idx
 * 
 * 情况二：发生回旋，但没有溢出
 * master_repl_offset 全部历史数据中最末尾的那一位，同时也是 当前有效数据 的最末尾那一位
 * repl_backlog_off   全部历史数据中最开头的那一位，同时也是 当前有效数据 的最开头那一位
 * 
 *   |<--------- repl_backlog_size -------->|
 *   -- histlen_2 -->|          |<----- histlen_1 ----
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *   | 7| 8| 9|10|11|  |  |  |  | 0| 1| 2| 3| 4| 5| 6|
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *                 ^                     
 *                 |
 *         repl_backlog_idx（下一个写入的位置）
 * 
 * 情况三：发生回旋，也发生溢出
 * master_repl_offset 全部历史数据中最末尾的那一位，不是 当前有效数据 的最末尾那一位
 * repl_backlog_off   全部历史数据中最开头的那一位，不是 当前有效数据 的最开头那一位
 * 
 *   |<--------- ---- repl_backlog_size ------------>|
 *   -------- histlen_2 ------->|<----- histlen_1 ----
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *   |12|13|14|15|16|17|18|19|20| 5| 6| 7| 8| 9|10|11|
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *                             ^                     
 *                             |
 *                     repl_backlog_idx（下一个写入的位置，同时也是最后一位有效数据 跟 第一位有效数据的开头 的分界线）
 */

// 创建 backlog
void createReplicationBacklog(void) {

    redisAssert(server.repl_backlog == NULL);

    // backlog
    server.repl_backlog = zmalloc(server.repl_backlog_size);
    // 数据长度
    server.repl_backlog_histlen = 0;
    // 索引值，增加数据时使用
    server.repl_backlog_idx = 0;
    /* When a new backlog buffer is created, we increment the replication
     * offset by one to make sure we'll not be able to PSYNC with any
     * previous slave. This is needed because we avoid incrementing the
     * master_repl_offset if no backlog exists nor slaves are attached. */
    // 每次创建 backlog 时都将 master_repl_offset 增一
    // 这是为了防止之前使用过 backlog 的 slave 引发错误的 PSYNC 请求
    server.master_repl_offset++;

    /* We don't have any data inside our buffer, but virtually the first
     * byte we have is the next byte that will be generated for the
     * replication stream. */
    // 尽管没有任何数据，
    // 但 backlog 第一个字节的逻辑位置应该是 repl_offset 后的第一个字节
    server.repl_backlog_off = server.master_repl_offset+1;
}

/* This function is called when the user modifies the replication backlog
 * size at runtime. It is up to the function to both update the
 * server.repl_backlog_size and to resize the buffer and setup it so that
 * it contains the same data as the previous one (possibly less data, but
 * the most recent bytes, or the same data and more free space in case the
 * buffer is enlarged). */
// 动态调整 backlog 大小
// 当 backlog 是被扩大时，原有的数据会被保留，
// 因为分配空间使用的是 realloc
void resizeReplicationBacklog(long long newsize) {

    // 不能小于最小大小
    if (newsize < REDIS_REPL_BACKLOG_MIN_SIZE)
        newsize = REDIS_REPL_BACKLOG_MIN_SIZE;

    // 大小和目前大小相等
    if (server.repl_backlog_size == newsize) return;

    // 设置新大小
    server.repl_backlog_size = newsize;
    if (server.repl_backlog != NULL) {
        /* What we actually do is to flush the old buffer and realloc a new
         * empty one. It will refill with new data incrementally.
         * The reason is that copying a few gigabytes adds latency and even
         * worse often we need to alloc additional space before freeing the
         * old buffer. */
        // 释放 backlog
        zfree(server.repl_backlog);
        // 按新大小创建新 backlog
        server.repl_backlog = zmalloc(server.repl_backlog_size);
        server.repl_backlog_histlen = 0;
        server.repl_backlog_idx = 0;
        /* Next byte we have is... the next since the buffer is emtpy. */
        server.repl_backlog_off = server.master_repl_offset+1;
    }
}

// 释放 backlog
// After a master has no longer connected slaves for some time, the backlog will be freed. 
// 这个时间取决于配置项：repl-backlog-ttl，既然是时间触发，自然是在 replicationCron() 里面的
void freeReplicationBacklog(void) {
    redisAssert(listLength(server.slaves) == 0);
    zfree(server.repl_backlog);
    server.repl_backlog = NULL;
}

/* Add data to the replication backlog.
 * This function also increments the global replication offset stored at
 * server.master_repl_offset, because there is no case where we want to feed
 * the backlog without incrementing the buffer. 
 *
 * 添加数据到复制 backlog ，
 * 并且按照添加内容的长度更新 server.master_repl_offset 偏移量。
 */
// 基本上是通过：1. repl-cron 来检查是否需要保存进 backlog 里面；
//              2. 在每次 server.db.dirty 之后，主动把 CMD 放进 backlog 里面
void feedReplicationBacklog(void *ptr, size_t len) {
    unsigned char *p = ptr;

    // 将长度累加到全局 offset 中
    server.master_repl_offset += len;

    /* This is a circular buffer, so write as much data we can at every
     * iteration and rewind the "idx" index if we reach the limit. */
    // 环形 buffer ，每次写尽可能多的数据，并在到达尾部时将 idx 重置到头部
    // 保存数据，并移动 repl_backlog_idx，增加 repl_backlog_histlen
    while(len) {
        // 从 idx 到 backlog 尾部的字节数，server.repl_backlog_size = REDIS_DEFAULT_REPL_BACKLOG_SIZE，1 MB
        // thislen = backlog 总长度 - backlog 目前用到哪 = 现在 backlog 还有多少空间可以用
        size_t thislen = server.repl_backlog_size - server.repl_backlog_idx;

        // 如果 idx 到 backlog 尾部这段空间足以容纳要写入的内容
        // 那么直接将写入数据长度设为 len
        // 在将这些 len 字节复制之后，这个 while 循环将跳出
        if (thislen > len) thislen = len;
        // 将 p 中的 thislen 字节内容复制到 backlog
        memcpy(server.repl_backlog+server.repl_backlog_idx,p,thislen);
        // 更新 idx ，指向新写入的数据之后
        server.repl_backlog_idx += thislen;
        // 如果写入达到尾部，那么将索引重置到头部
        if (server.repl_backlog_idx == server.repl_backlog_size)
            server.repl_backlog_idx = 0;
        // 减去已写入的字节数
        len -= thislen; // 决定了是否需要继续进行下一轮的 while-loop
        // 将指针移动到已被写入数据的后面，指向未被复制数据的开头
        p += thislen;   // 未写入数据的首地址
        // 增加环形队列中，未被 ack 的数据长度，最大值为 repl_backlog_size
        server.repl_backlog_histlen += thislen;
    }

    if (server.repl_backlog_histlen > server.repl_backlog_size)
        server.repl_backlog_histlen = server.repl_backlog_size;
    /* Set the offset of the first byte we have in the backlog. */
    // 记录程序可以依靠 backlog 来还原的数据的第一个字节的偏移量
    // 比如 master_repl_offset = 10086
    // repl_backlog_histlen = 30
    // 那么 backlog 所保存的数据的第一个字节的偏移量为
    // 10086 - 30 + 1 = 10056 + 1 = 10057
    // 这说明如果 slave 如果从 10057 至 10086 之间的任何时间断线
    // 那么 slave 都可以使用 PSYNC

    // 这个 repl_backlog_off 并不在这里使用，所以在这里看毫无意义
    // 应该配合 addReplyReplicationBacklog() 一起看
    // 当 repl_backlog_off 超过了 repl_backlog_size 之后，也是一直增长的
    /* 为什么要在这样算呢？
     * 1. 没有发生溢出，repl_backlog_off 基本不动，master_repl_offset 跟 repl_backlog_histlen 一开始就是固定的
     * 2. 发生了溢出，repl_backlog_off 永远都是 master_repl_offset 往回回溯 repl_backlog_histlen
     */
    server.repl_backlog_off = server.master_repl_offset -
                              server.repl_backlog_histlen + 1;
}

/* Wrapper for feedReplicationBacklog() that takes Redis string objects
 * as input. */
// 将 Redis 对象放进 replication backlog 里面
void feedReplicationBacklogWithObject(robj *o) {
    char llstr[REDIS_LONGSTR_SIZE];
    void *p;
    size_t len;

    if (o->encoding == REDIS_ENCODING_INT) {
        len = ll2string(llstr,sizeof(llstr),(long)o->ptr);
        p = llstr;
    } else {
        len = sdslen(o->ptr);
        p = o->ptr;
    }
    feedReplicationBacklog(p,len);
}

/* 完成 SYNC\PSYNC 之后，就会在这里开始完成日常的同步工作 */
// 在每次 CMD 运行完之后，master redis-server 都会检查一下 server.dirty
// 要是 dirty 的话，就启动 repl、AOF 将新的改动同步到其他的 slave 上面去
// TODO:(DONE) 谁来检查 server.dirty 的状态？谁来设置 flag ？（cron 会搞定的，发生了修改数据库的 CMD 之后，dirty 才会被 set）

// 将传入的参数发送给 slave 
// 操作分为三步：
// 1） 构建协议内容
// 2） 将协议内容备份到 backlog（用于日后可能发生的 PSYNC）
// 3） 将内容发送给各个 slave 
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j, len;
    char llstr[REDIS_LONGSTR_SIZE];

    /* If there aren't slaves, and there is no backlog buffer to populate,
     * we can return ASAP. */
    // backlog 为空，且没有 slave ，直接返回
    if (server.repl_backlog == NULL && listLength(slaves) == 0) return;

    /* We can't have slaves attached and no backlog. */
    redisAssert(!(listLength(slaves) != 0 && server.repl_backlog == NULL));

    /* Send SELECT command to every slave if needed. */
    // 如果有需要的话，发送 SELECT 命令，指定数据库
    if (server.slaveseldb != dictid) {
        robj *selectcmd;

        /* For a few DBs we have pre-computed SELECT command. */
        if (dictid >= 0 && dictid < REDIS_SHARED_SELECT_CMDS) {
            selectcmd = shared.select[dictid];
        } else {
            int dictid_len;

            dictid_len = ll2string(llstr,sizeof(llstr),dictid);
            selectcmd = createObject(REDIS_STRING,
                sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, llstr));
        }

        /* Add the SELECT command into the backlog. */
        // 将 SELECT 命令添加到 backlog
        if (server.repl_backlog) feedReplicationBacklogWithObject(selectcmd);

        /* Send it to slaves. */
        // 发送给所有 slave 
        listRewind(slaves,&li);
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;
            addReply(slave,selectcmd);
        }

        if (dictid < 0 || dictid >= REDIS_SHARED_SELECT_CMDS)
            decrRefCount(selectcmd);
    }

    server.slaveseldb = dictid;

    /* Write the command to the replication backlog if any. */
    // 将命令写入到backlog，备份，日后发生了短暂失联才会使用 backlog 里面的数据
    if (server.repl_backlog) {
        char aux[REDIS_LONGSTR_SIZE+3];

        /* Add the multi bulk reply length. */
        aux[0] = '*';
        len = ll2string(aux+1,sizeof(aux)-1,argc);
        aux[len+1] = '\r';
        aux[len+2] = '\n';
        feedReplicationBacklog(aux,len+3);

        for (j = 0; j < argc; j++) {
            long objlen = stringObjectLen(argv[j]);

            /* We need to feed the buffer with the object as a bulk reply
             * not just as a plain string, so create the $..CRLF payload len 
             * ad add the final CRLF */
            // 将参数从对象转换成协议格式
            aux[0] = '$';
            len = ll2string(aux+1,sizeof(aux)-1,objlen);
            aux[len+1] = '\r';
            aux[len+2] = '\n';
            feedReplicationBacklog(aux,len+3);
            feedReplicationBacklogWithObject(argv[j]);
            feedReplicationBacklog(aux+len+1,2);
        }
    }

    /* Write the command to every slave. */
    listRewind(slaves,&li);
    while((ln = listNext(&li))) {

        // 指向 slave 
        redisClient *slave = ln->value;

        /* Don't feed slaves that are still waiting for BGSAVE to start */
        // 不要给正在等待 BGSAVE 开始的 slave 发送命令
        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) continue;

        /* Feed slaves that are waiting for the initial SYNC (so these commands
         * are queued in the output buffer until the initial SYNC completes),
         * or are already in sync with the master. */
        // 向已经接收完和正在接收 RDB 文件的 slave 发送命令
        // 如果 slave 正在接收 master 发送的 RDB 文件，
        // 那么在初次 SYNC 完成之前， master 发送的内容会被放进一个缓冲区里面

        /* Add the multi bulk length. */
        addReplyMultiBulkLen(slave,argc);

        /* Finally any additional argument that was not stored inside the
         * static buffer if any (from j to argc). */
        for (j = 0; j < argc; j++)
            addReplyBulk(slave,argv[j]);
    }
}

// 将协议发给 Monitor
// TODO: 看了 Monitor 功能后再来看
void replicationFeedMonitors(redisClient *c, list *monitors, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j;
    sds cmdrepr = sdsnew("+");
    robj *cmdobj;
    struct timeval tv;

    // 获取时间戳
    gettimeofday(&tv,NULL);
    cmdrepr = sdscatprintf(cmdrepr,"%ld.%06ld ",(long)tv.tv_sec,(long)tv.tv_usec);
    if (c->flags & REDIS_LUA_CLIENT) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d lua] ",dictid);
    } else if (c->flags & REDIS_UNIX_SOCKET) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d unix:%s] ",dictid,server.unixsocket);
    } else {
        cmdrepr = sdscatprintf(cmdrepr,"[%d %s] ",dictid,getClientPeerId(c));
    }

    // 获取命令和参数
    for (j = 0; j < argc; j++) {
        if (argv[j]->encoding == REDIS_ENCODING_INT) {
            cmdrepr = sdscatprintf(cmdrepr, "\"%ld\"", (long)argv[j]->ptr);
        } else {
            cmdrepr = sdscatrepr(cmdrepr,(char*)argv[j]->ptr,
                        sdslen(argv[j]->ptr));
        }
        if (j != argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    cmdrepr = sdscatlen(cmdrepr,"\r\n",2);
    cmdobj = createObject(REDIS_STRING,cmdrepr);

    // 将内容发送给所有 MONITOR 
    listRewind(monitors,&li);
    while((ln = listNext(&li))) {
        redisClient *monitor = ln->value;
        addReply(monitor,cmdobj);
    }
    decrRefCount(cmdobj);
}

/* Feed the slave 'c' with the replication backlog starting from the
 * specified 'offset' up to the end of the backlog. */
// 向 slave  c 发送 backlog 中从 offset 到 backlog 尾部之间的数据，完成 PSYNC 过程
long long addReplyReplicationBacklog(redisClient *c, long long offset) {
    long long j, skip, len;

    redisLog(REDIS_DEBUG, "[PSYNC] Slave request offset: %lld", offset);

    if (server.repl_backlog_histlen == 0) {
        redisLog(REDIS_DEBUG, "[PSYNC] Backlog history len is zero");
        return 0;
    }

    redisLog(REDIS_DEBUG, "[PSYNC] Backlog size: %lld",
             server.repl_backlog_size);
    redisLog(REDIS_DEBUG, "[PSYNC] First byte: %lld",
             server.repl_backlog_off);
    redisLog(REDIS_DEBUG, "[PSYNC] History len: %lld",
             server.repl_backlog_histlen);
    redisLog(REDIS_DEBUG, "[PSYNC] Current index: %lld",
             server.repl_backlog_idx);

    /* Compute the amount of bytes we need to discard. */
    skip = offset - server.repl_backlog_off;    // 跳过不需要的版本号
    redisLog(REDIS_DEBUG, "[PSYNC] Skipping: %lld", skip);

    /* Point j to the oldest byte, that is actaully our
     * server.repl_backlog_off byte. */
    /* 1. 溢出后，(server.repl_backlog_size - server.repl_backlog_histlen) = 0
     *    当 backlog 中有效数据充满了整个 backlog 时，即 backlog 被完全利用，计算退化成
     *    j = server.repl_backlog_idx % server.repl_backlog_size，
     *    由于 repl_backlog_idx 不可能大于server.repl_backlog_size，
     *    所以计算结果就等于 server.repl_backlog_idx（这时候，idx 下一个 byte 就是最旧的那个 byte ），
     *    它是读写数据的分割点
     * 2. 当 backlog 中尚有未使用的空间时，repl_backlog_idx 等于 server.repl_backlog_histlen，
     *    计算退化成 server.repl_backlog_size % server.repl_backlog_size = 0
     *    全靠 j = (j + skip) % server.repl_backlog_size; 来计算
     */
    j = (server.repl_backlog_idx + (server.repl_backlog_size - server.repl_backlog_histlen)) // 
        % server.repl_backlog_size; // 取出多余的部分，回归 index
    redisLog(REDIS_DEBUG, "[PSYNC] Index of first byte: %lld", j);

    /* Discard the amount of data to seek to the specified 'offset'. */
    j = (j + skip) % server.repl_backlog_size;

    /* Feed slave with data. Since it is a circular buffer we have to
     * split the reply in two parts if we are cross-boundary. */
    len = server.repl_backlog_histlen - skip;
    redisLog(REDIS_DEBUG, "[PSYNC] Reply total length: %lld", len);
    while(len) {    // 因为环形 buf 可能要从头再来，所以要检查 len
        long long thislen =
            ((server.repl_backlog_size - j) < len) ?
            (server.repl_backlog_size - j) : len;

        redisLog(REDIS_DEBUG, "[PSYNC] addReply() length: %lld", thislen);

        // 因为 write 未必已经就绪，所以采用 epoll 监听到 write 事件就绪后再刷写相应的 buf
        // 而 addReply 里面的机制，已经能够确保这些数据最终能够被发送出去，所以也就可以移动相应的 offset
        addReplySds(c,sdsnewlen(server.repl_backlog + j, thislen)); // 需要发送出去的内容已经挂在了 c->buf 里面
        len -= thislen;
        j = 0;
    }
    return server.repl_backlog_histlen - skip;
}

/* This function handles the PSYNC command from the point of view of a
 * master receiving a request for partial resynchronization.
 *
 * On success return REDIS_OK, otherwise REDIS_ERR is returned and we proceed
 * with the usual full resync. */
// 尝试进行部分 resync ，成功返回 REDIS_OK ，失败返回 REDIS_ERR 。
// 在 slave 短时断开重连后，上报master runid 及复制偏移量。如果 runid 与 master 一致，且偏移量仍然在 master 的复制缓冲积压中，则 master 进行增量同步。
// 但如果 slave 重启后，master runid 会丢失，或者切换 master 后，runid 会变化，仍然需要全量同步。
int masterTryPartialResynchronization(redisClient *c) {
    long long psync_offset, psync_len;
    char *master_runid = c->argv[1]->ptr;
    char buf[128];
    int buflen;

    /* Is the runid of this master the same advertised by the wannabe slave
     * via PSYNC? If runid changed this master is a different instance(slave 以前并不是跟随这个 master 的) and
     * there is no way to continue perform a PSYNC. */
    // 检查 master id 是否和 runid 一致，只有一致的情况下才有 PSYNC 的可能 
    /* TODO:(DEON) 为什么？一致跟不一致，分别意味着什么？
     * sync 的核心目的就是：slave 的数据永远跟 master 一致；
     * slave 的核心目标是：成为 master 的精确复制品，是 master 的影子
     * 而 run-id 的改变，也就意味着：这个 redis-server 重启过，或者 master 发生过切换。
     * 这两种场景都意味着：slave 里面保存的 master run-id 跟现在的 master 是不一样的了，
     * 不能采用部分同步，而是采用完全同步才更稳妥
     * 
     * TODO:(DONE) runid 什么时候会变化？
     * redis-server 启动的时候才会变化，而且基本上随机序列串
     * 
     * TODO:(DONE) runid 有什么用？为什么同步的时候要带上这个 runid（让 slave 能够察觉自己的 master 是不是变了）
     * 因为 master 的 runid 会被 slave 保存起来，所以每次同步之前，slave 发送的 sync 请求都会带上自己保存的 master run-id
     * 这样一来，就可以让现在 master 来决策：这个 slave 以前的 master 是不是我，是的话进行完全同步，否则进行部分同步
     */
    if (strcasecmp(master_runid, server.runid)) {   // run-id 没有；或者是，slave 保存的 master run-id 跟当前的 master run-id 不一致
        /* Run id "?" is used by slaves that want to force a full resync. */
        if (master_runid[0] != '?') {
            // slave 提供的 run id 和服务器的 run id 不一致
            // 说明以前这个 slave 跟随的 master 并不是自己，所以采取完全同步的方案，避免 slave 的数据有问题
            redisLog(REDIS_NOTICE,"Partial resynchronization not accepted: "
                "Runid mismatch (Client asked for runid '%s', my runid is '%s')",
                master_runid, server.runid);
        } else {
            // slave 提供的 run id 为 '?' ，表示强制 FULL RESYNC
            // 毕竟是第一次进行同步嘛
            redisLog(REDIS_NOTICE,"Full resync requested by slave.");
        }
        // 需要 full resync
        goto need_full_resync;
    }

    /* We still have the data our slave is asking for? */
    // 取出 psync_offset 参数
    if (getLongLongFromObjectOrReply(c,c->argv[2],&psync_offset,NULL) !=
       REDIS_OK) goto need_full_resync;

    // 判断是否能够进行 PSYNC
    // 1. 要有 backlog
    // 2. master 要能够恢复 slave 需要的所有数据
    if (!server.repl_backlog ||
        // 或者 psync_offset 小于 server.repl_backlog_off
        // （想要恢复的那部分数据已经被覆盖）
        psync_offset < server.repl_backlog_off ||   // 根据目前记录的 repl_backlog_off 缓冲区最开头的版本号，来看看 slave 要的还有没保留在缓冲区里面
        // psync offset 大于 backlog 所保存的数据的偏移量
        // repl_backlog_histlen 只是用来兼容 backlog 溢出跟没有溢出两种情况罢了
        // 实际上，这个可以直接检查 psync_offset > master_repl_offset - 1
        psync_offset > (server.repl_backlog_off + server.repl_backlog_histlen))
    {
        // 执行 FULL RESYNC
        redisLog(REDIS_NOTICE,
            "Unable to partial resync with the slave for lack of backlog (Slave request was: %lld).", psync_offset);
        if (psync_offset > server.master_repl_offset) {
            redisLog(REDIS_WARNING,
                "Warning: slave tried to PSYNC with an offset that is greater than the master replication offset.");
        }
        goto need_full_resync;
    }

    /* If we reached this point, we are able to perform a partial resync:
     * 程序运行到这里，说明可以执行 partial resync
     *
     * 1) Set client state to make it a slave.
     *    将客户端状态设为 salve  
     *
     * 2) Inform the client we can continue with +CONTINUE
     *    向 slave 发送 +CONTINUE ，表示 partial resync 的请求被接受
     *
     * 3) Send the backlog data (from the offset to the end) to the slave. 
     *    发送 backlog 中，客户端所需要的数据
     */
    c->flags |= REDIS_SLAVE;
    c->replstate = REDIS_REPL_ONLINE;
    c->repl_ack_time = server.unixtime;
    listAddNodeTail(server.slaves,c);
    /* We can't use the connection buffers since they are used to accumulate
     * new commands at this stage. But we are sure the socket send buffer is
     * emtpy so this write will never fail actually. */
    // 向 slave 发送一个同步 +CONTINUE ，表示 PSYNC 可以执行
    buflen = snprintf(buf,sizeof(buf),"+CONTINUE\r\n");
    if (write(c->fd,buf,buflen) != buflen) {
        freeClientAsync(c);
        return REDIS_OK;
    }
    // 发送 backlog 中的内容（也即是 slave 缺失的那些内容）到 slave 
    // 为什么可以直接发呢？因为做不做 PSYNC 是取决于 master 上面有没有给 slave 做 PSYNC 的条件
    // 既然 master 都已经通知 slave "+CONTINUE" 没问题了
    // slave 就会开始准备接受数据（slave 把相应的 socket-fd 注册进 epoll 里面）
    // 自然，master 只管发送数据就好了
    psync_len = addReplyReplicationBacklog(c,psync_offset);
    redisLog(REDIS_NOTICE,
        "Partial resynchronization request accepted. Sending %lld bytes of backlog starting from offset %lld.", psync_len, psync_offset);
    /* Note that we don't need to set the selected DB at server.slaveseldb
     * to -1 to force the master to emit SELECT, since the slave already
     * has this state from the previous connection with the master. */

    // 刷新低延迟 slave 的数量
    refreshGoodSlavesCount();
    return REDIS_OK; /* The caller can return, no full resync needed. */

need_full_resync:
    /* Although slave wants to perform a PSYNC, we need a full resync for some reason... notify the client. */
    // 刷新 psync_offset
    psync_offset = server.master_repl_offset;
    /* Add 1 to psync_offset if it the replication backlog does not exists
     * as when it will be created later we'll increment the offset by one. TODO:（DONE） why ? 没啥的，约定罢了，你看看 backlog 那里就好 */
    // 刷新 psync_offset
    if (server.repl_backlog == NULL) psync_offset++;
    /* Again, we can't use the connection buffers (see above).TODO:（DONE） why ? master 的 backlog 增长太快了，PSYNC 的数据被刷走了 */
    // 发送 +FULLRESYNC ，表示需要完整重同步
    // buf = "+FULLRESYNC 21a931865c82d28ec842c7c60bdc0ae6ceeaa12a 1\r\n\000"
    // master 第一次启动的话：server.master_repl_offset = 0; server.repl_backlog = NULL
    buflen = snprintf(buf,sizeof(buf),"+FULLRESYNC %s %lld\r\n",
                      server.runid,psync_offset);
    if (write(c->fd,buf,buflen) != buflen) {
        freeClientAsync(c);
        return REDIS_OK;
    }
    /*
     * 实际上，这里 master 就已经把 "+FULLRESYNC 21a931865c82d28ec842c7c60bdc0ae6ceeaa12a 1\r\n\000" 发送了给 slave
     * 而 slave 发出 SYNC 请求之后，是会进行同步阻塞等待的
     * （slaveTryPartialResynchronization() 的 sendSynchronousCommand() 里面）
     * 而 slave 则是会在这里收到 master 发送的内容
     */
    return REDIS_ERR;
}

/* SYNC ad PSYNC command implemenation. */
/* The SYNC command: is called by Redis replicas for initiating a replication stream from the master.
 * It has been replaced in newer versions of Redis by PSYNC.
 *
 * The PSYNC command is called by Redis replicas for initiating a replication stream from the master.
 * 
 * 理论上这两个 command 都是 slave 调用的
 */
/**
 * (gdb) p c->argc
 * $2 = 3
 * (gdb) p (char*)c->argv[0]->ptr
 * $3 = 0x7f32da486258 "PSYNC"
 * (gdb) p (char*)c->argv[1]->ptr
 * $4 = 0x7f32da486238 "?"
 * (gdb) p (char*)c->argv[2]->ptr
 * $5 = 0x7f32da486228 "-1"
 * 
 * slave 第一次连进这个 master 的话，将会发送 "PSYNC ? -1" 过来给 master
 * 
 * 同步机制：
 * 1. 从库尝试发送 psync 命令到主库，而不是直接使用 sync 命令进行全量同步
 * 2. 主库判断是否满足 psync 条件, 满足就返回+CONTINUE进行增量同步, 否则返回+FULLRESYNC runid offfset
 *    是否允许 psync 有两个条件:
 *    1) 条件一: psync 命令携带的 runid 需要和主库的 runid 一致才可以进行增量同步，否则需要全量同步。
 *    2) 条件二: psync 命令携带的 offset 是否超过缓冲区。如果超过则需要全量同步，否则就进行增量同步。
 * 
 * 
 * FIXME: in 4.0 version
 * 虽然 2.8 引入的 psync 可以解决短时间主从同步断掉重连问题，但以下几个场景仍然是需要全量同步:
 *   1. 主库/从库有重启过。因为 runnid 重启后就会丢失，所以当前机制无法做增量同步。
 *   2. 从库提升为主库。其他从库切到新主库全部要全量不同数据，因为新主库的 runnid 跟老的主库是不一样的。
 * 这两个应该是我们比较常见的场景。主库切换或者重启都需要全量同步数据在从库实例比较大或者多的场景下，
 * 那内网网络带宽和服务都会有很大的影响。所以 redis 4.0 对 psync 优化之后可以一定程度上规避这些问题。
*/
// 因为 sync 命令，是 slave 主动发送给 master，希望能够拉取 master 上面最新的数据
// 所以这个 syncCommand() 理应是 master 调用，来处理 slave 发过来的 psync CMD
void syncCommand(redisClient *c) {

    /* ignore SYNC if already slave or in monitor mode */
    // 已经是 SLAVE ，或者处于 MONITOR 模式，返回
    if (c->flags & REDIS_SLAVE) return;

    /* Refuse SYNC requests if we are a slave but the link with our master
     * is not ok... */
    // 如果这是一个 slave ，但与 master 的连接仍未就绪，那么拒绝 SYNC
    // 这就很奇怪了，为什么 slave 会调用 master 的函数呢？
    // redis 的主从，是支持级联的！所以 slave 可以作为其他 redis 的 master，这时候，作为 master 的 slave 就会用这个函数
    // 这个作为 master 的 slave 可以处理 sync 请求的前提是：自己已经是一个 online，且没有跟自己的 master 失联
    if (server.masterhost && server.repl_state != REDIS_REPL_CONNECTED) {
        addReplyError(c,"Can't SYNC while not connected with my master");
        return;
    }

    /* SYNC can't be issued when the server has pending data to send to
     * the client(this client is a slave node) about already issued commands. We need a fresh reply
     * buffer registering the differences between the BGSAVE and the current
     * dataset, so that we can copy to other slaves if needed. */
    // 在 master 上： slave 连接的 redisClient 仍有输出数据等待输出，不能 SYNC
    // 即使是 slave 与 master 相互连接，彼此也是将对方视为一个 redisClient 的
    // TODO:(DONE) 为什么 reply\buf 还有数据，就不能进行 SYNC， We need a fresh reply buffer 是用来干嘛？
    // 因为开始 bgsave 到 RDB 传递完毕之间，master 依旧会处理新的数据，
    // 这部分数据要在 slave 加载完 RDB 之后 master 发送给 slave
    // 所以这时候 reply 最好是干净的（开始 SYNC 之后，master --> slave 的 socket-fd 的 write 会被锁定，应为 REDIS_REPL_WAIT_BGSAVE_END 状态的关系）
    if (listLength(c->reply) != 0 || c->bufpos != 0) {
        addReplyError(c,"SYNC and PSYNC are invalid with pending output");
        return;
    }

    redisLog(REDIS_NOTICE,"Slave asks for synchronization");

    /* Try a partial resynchronization if this is a PSYNC command.
     * 如果这是一个 PSYNC 命令，那么尝试 partial resynchronization 。
     *
     * If it fails, we continue with usual full resynchronization, however
     * when this happens masterTryPartialResynchronization() already
     * replied with:
     *
     * 如果失败，那么使用 full resynchronization ，
     * 在这种情况下， masterTryPartialResynchronization() 返回以下内容：
     *
     * +FULLRESYNC <runid> <offset>
     *
     * So the slave knows the new runid and offset to try a PSYNC later
     * if the connection with the master is lost. 
     *
     * 这样的话，之后如果 master 断开，那么 slave 就可以尝试 PSYNC 了。
     */
    if (!strcasecmp(c->argv[0]->ptr,"psync")) {
        // 尝试进行 PSYNC
        if (masterTryPartialResynchronization(c) == REDIS_OK) {
            // 可执行 PSYNC
            // TODO:（DONE） 看部分同步的过程，为什么直接这里就 return 了
            // 因为 masterTryPartialResynchronization() 判断可以进行 PSYNC 之后，就把数据 addReply 了，
            // 然后 master 进 epoll 等 write 就绪，调度就好了
            server.stat_sync_partial_ok++;
            return; /* No full resync needed, return. */
        } else {
            // 不可执行 PSYNC
            // 这时候一般会将 master 的 run-id 更新给 slave
            char *master_runid = c->argv[1]->ptr;
            
            /* Increment stats for failed PSYNCs, but only if the
             * runid is not "?", as this is used by slaves to force a full
             * resync on purpose when they are not albe to partially
             * resync.（就比如 slave 第一次进行 SLAVEOF 命令） */
            if (master_runid[0] != '?') server.stat_sync_partial_err++; // "?" 不算 PSYNC 失败，这是 slave 用来强制 master 进行完全同步的方式
        }
    } else {
        /* If a slave uses SYNC, we are dealing with an old implementation
         * of the replication protocol (like redis-cli --slave). Flag the client
         * so that we don't expect to receive REPLCONF ACK feedbacks. */
        // 旧版实现，设置标识，避免接收 REPLCONF ACK 
        c->flags |= REDIS_PRE_PSYNC;
    }

    /* Full resynchronization. */
    server.stat_sync_full++;

    /* Here we need to check if there is a background saving operation
     * in progress, or if it is required to start one */
    // TODO:（DONE） 假设有会怎样？没有又怎样？为什么？
    // 有就等待那个 RDB 完成，然后复用
    if (server.rdb_child_pid != -1) {
        /* Ok a background save is in progress. Let's check if it is a good
         * one for replication, i.e. if there is another slave that is
         * registering differences since the server forked to save */
        redisClient *slave;
        listNode *ln;
        listIter li;

        // 如果有至少一个 slave 在等待这个 BGSAVE 完成
        // 那么说明正在进行的 BGSAVE 所产生的 RDB 也可以为其他 slave 所用
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) break;
        }

        if (ln) {
            /* Perfect, the server is already registering differences for
             * another slave. Set the right state, and copy the buffer. */
            // 幸运的情况，可以使用目前 BGSAVE 所生成的 RDB
            // 既然是用同一个 RDB，那自然也是要用同样的追加内容
            copyClientOutputBuffer(c,slave);
            c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
            redisLog(REDIS_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            /* No way, we need to wait for the next BGSAVE in order to
             * register differences */
            // 不好运的情况，必须等待下个 BGSAVE
            c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
            redisLog(REDIS_NOTICE,"Waiting for next BGSAVE for SYNC");
        }
    } else {
        /* Ok we don't have a BGSAVE in progress, let's start one */
        // 没有 BGSAVE 在进行，开始一个新的 BGSAVE
        redisLog(REDIS_NOTICE,"Starting BGSAVE for SYNC");
        if (rdbSaveBackground(server.rdb_filename) != REDIS_OK) {
            redisLog(REDIS_NOTICE,"Replication failed, can't BGSAVE");
            addReplyError(c,"Unable to perform background save");
            return;
        }
        // 设置状态，TODO:(DONE) 为什么一开始就设置为 REDIS_REPL_WAIT_BGSAVE_END ？
        // 没办法，看英语注释更好，人类迷惑行为一般的命名
        /* Waiting RDB file creation to finish. */
        c->replstate = REDIS_REPL_WAIT_BGSAVE_END;

        /* Flush the script cache for the new slave. */
        // 因为新 slave 进入，刷新复制脚本缓存，TODO: why ?
        replicationScriptCacheFlush();

        // 成功执行 BGSAVE 之后的 master，就会在 ServerCron 里面检查子进程是否完成了 RDB 的任务
        // 完成之后会在 backgroundSaveDoneHandler() 里面调用 updateSlavesWaitingBgsave() 完成 repl full sync 剩下的部分
    }

    // 启用了 Nagle 算法，避免大量拥堵网络  
    // TODO:(DONE) 为什么 repl 这里要单独启用呢？接下来是收发体积较大的 RDB 文件，除非 slave 收到一点就返回 OK，不然延时可能很恐怖
    // 这取决于你当前 slave 跟 master 的网络环境允不允许，有没有多余的带宽给 ACK 跟 TCP 头部
    /**
     * redis.conf:
     * 
     * Disable TCP_NODELAY **on the slave socket after SYNC?**
     *
     * If you select "yes" Redis will use a smaller number of TCP packets and
     * less bandwidth to send data to slaves. But this can add a delay for
     * the data to appear on the slave side, up to 40 milliseconds with
     * Linux kernels using a default configuration.
     *
     * If you select "no" the delay for data to appear on the slave side will
     * be reduced but more bandwidth will be used for replication.
     *
     * By default we optimize for low latency, but in very high traffic conditions
     * or when the master and slaves are many hops away, turning this to "yes" may
     * be a good idea.
    */
    if (server.repl_disable_tcp_nodelay)
        anetDisableTcpNoDelay(NULL, c->fd); /* Non critical if it fails. */

    c->repldbfd = -1;

    c->flags |= REDIS_SLAVE;

    server.slaveseldb = -1; /* Force to re-emit the SELECT command. */

    // 添加到 slave 列表中，这个无论是完全同步还是部分同步，哪怕是不需要进行同步，
    // 都是需要将 slave 的 redisClient 加入 master 的 server.slave 里面的
    listAddNodeTail(server.slaves,c);
    // 如果是第一个 slave ，那么初始化 backlog
    if (listLength(server.slaves) == 1 && server.repl_backlog == NULL)
        createReplicationBacklog();
    return;
}

// 后期删掉了的样子，不然就是一个内部使用的 CMD
/* REPLCONF <option> <value> <option> <value> ...
 * This command is used by a slave in order to configure the replication
 * process before starting it with the SYNC command.
 *
 * 由 slave 使用，在 SYNC 之前配置复制进程（process）
 *
 * Currently the only use of this command is to communicate to the master
 * what is the listening port of the Slave redis instance, so that the
 * master can accurately list slaves and their listening ports in
 * the INFO output.
 *
 * 目前这个函数的唯一作用就是，让 slave 告诉 master 它正在监听的端口号
 * 然后 master 就可以在 INFO 命令的输出中打印这个号码了。
 *
 * In the future the same command can be used in order to configure
 * the replication to initiate an incremental replication instead of a
 * full resync. 
 *
 * 将来可能会用这个命令来实现增量式复制，取代 full resync 。
 */
// NOTE: REPLCONF <option> <value> <option> <value> 这个 CMD 是由 slave 发送给 master 的
//       但是 replconfCommand() 这个函数却是由 master 调用的，不要搞混了
void replconfCommand(redisClient *c) {
    int j;

    if ((c->argc % 2) == 0) {
        /* Number of arguments must be odd to make sure that every
         * option has a corresponding value. */
        addReply(c,shared.syntaxerr);
        return;
    }

    /* Process every option-value pair. */
    for (j = 1; j < c->argc; j+=2) {

        //  slave 发来 REPLCONF listening-port <port> 命令
        //  master 将 slave 监听的端口号记录下来
        // 也即是 INFO replication 中的 slaveN ..., port = xxx 这一项
        if (!strcasecmp(c->argv[j]->ptr,"listening-port")) {
            long port;

            if ((getLongFromObjectOrReply(c,c->argv[j+1],
                    &port,NULL) != REDIS_OK))
                return;
            c->slave_listening_port = port;

        //  slave 发来 REPLCONF ACK <offset> 命令
        // 告知 master ， slave 已处理的复制流的偏移量
        } else if (!strcasecmp(c->argv[j]->ptr,"ack")) {
            /* REPLCONF ACK is used by slave to inform the master the amount
             * of replication stream that it processed so far. It is an
             * internal only command that normal clients should never use. */
            //  slave 使用 REPLCONF ACK 告知 master ，
            //  slave 目前已处理的复制流的偏移量
            //  master 更新它的记录值
            // 也即是 INFO replication 中的  slaveN ..., offset = xxx 这一项
            long long offset;

            if (!(c->flags & REDIS_SLAVE)) return;
            if ((getLongLongFromObject(c->argv[j+1], &offset) != REDIS_OK))
                return;
            // 如果 offset 已改变，那么更新
            if (offset > c->repl_ack_off)
                c->repl_ack_off = offset;
            // 更新最后一次发送 ack 的时间
            c->repl_ack_time = server.unixtime;
            /* Note: this command does not reply anything! */
            return;
        } else if (!strcasecmp(c->argv[j]->ptr,"getack")) {
            /* REPLCONF GETACK is used in order to request an ACK ASAP
             * to the slave. */
            if (server.masterhost && server.master) replicationSendAck();
            /* Note: this command does not reply anything! */
        } else {
            addReplyErrorFormat(c,"Unrecognized REPLCONF option: %s",
                (char*)c->argv[j]->ptr);
            return;
        }
    }
    addReply(c,shared.ok);
}

// master 将 RDB 文件发送给 slave 的写事件处理器
// 此时此刻，slave redisClient 的 write-callback 将会替换为这个函数
void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *slave = privdata;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    char buf[REDIS_IOBUF_LEN];
    ssize_t nwritten, buflen;

    /* Before sending the RDB file, we send the preamble as configured by the
     * replication process. Currently the preamble is just the bulk count of
     * the file in the form "$<length>\r\n". */
    if (slave->replpreamble) {
        nwritten = write(fd,slave->replpreamble,sdslen(slave->replpreamble));
        if (nwritten == -1) {
            redisLog(REDIS_VERBOSE,"Write error sending RDB preamble to slave: %s",
                strerror(errno));
            freeClient(slave);
            return;
        }
        sdsrange(slave->replpreamble,nwritten,-1);
        if (sdslen(slave->replpreamble) == 0) {
            sdsfree(slave->replpreamble);
            slave->replpreamble = NULL;
            /* fall through sending data. */
        } else {
            // 等待下一次 write-able，然后进行传递
            return;
        }
    }

    /* If the preamble was already transfered, send the RDB bulk data. */
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);
    // 读取 RDB 数据
    buflen = read(slave->repldbfd,buf,REDIS_IOBUF_LEN);
    if (buflen <= 0) {
        redisLog(REDIS_WARNING,"Read error sending DB to slave: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    // 写入数据到 slave
    if ((nwritten = write(fd,buf,buflen)) == -1) {
        if (errno != EAGAIN) {
            // 出了 EAGAIN 之外的其他错误都不能够接受
            redisLog(REDIS_WARNING,"Write error sending DB to slave: %s",
                strerror(errno));
            freeClient(slave);
        }
        return;
    }

    // 如果写入成功，那么更新写入字节数到 repldboff ，等待下次继续写入
    slave->repldboff += nwritten;

    // 如果写入已经全部完成
    if (slave->repldboff == slave->repldbsize) {
        // 关闭 RDB 文件描述符
        close(slave->repldbfd);
        slave->repldbfd = -1;
        // 删除之前绑定的写事件处理器
        aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
        // 将状态更新为 REDIS_REPL_ONLINE
        slave->replstate = REDIS_REPL_ONLINE;
        // 更新响应时间（因为开启了 TCP_NODELAY）
        slave->repl_ack_time = server.unixtime;
        // 创建向 slave 发送命令的写事件处理器
        // 将保存并发送 RDB 期间的回复全部发送给 slave 
        // TODO:（DONE） 要是利用重新注册 sendReplyToClient() 的方法来补充 CMD，那样就跟 backlog 没有任何关系了啦？
        // TODO: （DONE）这个跟 backlog 有什么区别？
        // backlog 是用来让 slave 在短暂失联的情况下，能够通过 PSYNC 的方式快速同步
        // sendReplyToClient() 则是在 full SYNC 之后，将创建、传送 RDB 过程中，master 接收到的数据，补发给 slave
        if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE,
            sendReplyToClient, slave) == AE_ERR) {
            redisLog(REDIS_WARNING,"Unable to register writable event for slave bulk transfer: %s", strerror(errno));
            freeClient(slave);
            return;
        }
        // 刷新低延迟 slave 数量
        refreshGoodSlavesCount();
        redisLog(REDIS_NOTICE,"Synchronization with slave succeeded");
    }
}

/* This function is called at the end of every background saving.
 * 在每次 BGSAVE 执行完毕之后使用
 *
 * The argument bgsaveerr（bgsave-err） is REDIS_OK if the background saving succeeded
 * otherwise REDIS_ERR is passed to the function.
 * bgsaveerr 可能是 REDIS_OK 或者 REDIS_ERR ，显示 BGSAVE 的执行结果
 *
 * The goal of this function is to handle slaves waiting for a successful
 * background saving in order to perform non-blocking synchronization.
 * （这样一来，就不用阻塞在那里，等待 RDB 生成，然后再进行 sync，现在这样异步操作，就可以再 RDB 完成之后，再回过头来进行 sync 剩下的工作了） 
 * 
 * 这个函数是在 BGSAVE 完成之后的异步回调函数，
 * 它指导该怎么执行和 slave 相关的 RDB 下一步工作。
 */
// TODO: 要是多个 slave 过来要求同步，而且每个 slave 都发现当前的 RDB 文件不能公用，会发生什么？
void updateSlavesWaitingBgsave(int bgsaveerr) {
    listNode *ln;
    int startbgsave = 0;
    listIter li;

    // 遍历所有 slave
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        // REDIS_REPL_WAIT_BGSAVE_START 跟 REDIS_REPL_WAIT_BGSAVE_END 并不会影响其他不需要进行 repl 的 slave
        // 之后正在走 sync 的 slave 才会被设置这两个状态情况
        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
            // TODO: 进来这里的话，意味着是一个怎样的 case ？
            // 之前的 RDB 文件不能被 slave 使用，
            // 开始新的 BGSAVE
            startbgsave = 1;
            slave->replstate = REDIS_REPL_WAIT_BGSAVE_END;

        } else if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) {

            // 执行到这里，说明有 slave 在等待 BGSAVE 完成

            struct redis_stat buf;  // 配合 redis_fstat 用的，依赖于操作系统

            // 但是 BGSAVE 执行错误
            if (bgsaveerr != REDIS_OK) {
                // 释放 slave
                // 因为能够作为 slave 的 redisClient，都是 slave 通过 SLAVEOF 命令而发起、进而让 master 创建的
                // 当 RDB 失败了，也就意味着无法同步，与 slave 断开连接也是十分合理的
                freeClient(slave);
                redisLog(REDIS_WARNING,"SYNC failed. BGSAVE child returned an error");
                continue;
            }

            // 打开 RDB 文件
            // 这个 open 就很灵性，即使 rdb_filename 被删除了也没有关系，因为最终的删除是要等 rdb_filename 这个没有没有任何引用之后才正式废除的
            // 所以完全不担心在 sync 中途这个文件被删除，不可能的
            if ((slave->repldbfd = open(server.rdb_filename,O_RDONLY)) == -1 ||
                redis_fstat(slave->repldbfd,&buf) == -1) {
                freeClient(slave);
                redisLog(REDIS_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                continue;
            }

            // 设置偏移量，各种值
            slave->repldboff = 0;
            slave->repldbsize = buf.st_size;
            // 更新状态
            slave->replstate = REDIS_REPL_SEND_BULK;

            slave->replpreamble = sdscatprintf(sdsempty(),"$%lld\r\n",
                (unsigned long long) slave->repldbsize);

            // 清空之前的写事件处理器（避免破坏正在传递的 RDB 文件）
            // 并且更换为 RDB 文件传递的专用函数 sendBulkToSlave()
            aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
            // 将 sendBulkToSlave 安装为 slave 的写事件处理器
            // 它用于将 RDB 文件发送给 slave
            if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE, sendBulkToSlave, slave) == AE_ERR) {
                freeClient(slave);
                continue;
            }
        }
    }

    // 需要执行新的 BGSAVE
    if (startbgsave) {
        /* Since we are starting a new background save for one or more slaves,
         * we flush the Replication Script Cache to use EVAL to propagate every
         * new EVALSHA for the first time, since all the new slaves don't know
         * about previous scripts. */
        // 开始行的 BGSAVE ，并清空脚本缓存
        replicationScriptCacheFlush();
        if (rdbSaveBackground(server.rdb_filename) != REDIS_OK) {
            listIter li;

            listRewind(server.slaves,&li);
            redisLog(REDIS_WARNING,"SYNC failed. BGSAVE failed");
            while((ln = listNext(&li))) {
                redisClient *slave = ln->value;

                if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START)
                    freeClient(slave);
            }
        }
    }
}

/* ----------------------------------- SLAVE -------------------------------- */

/* Abort the async download of the bulk dataset while SYNC-ing with master */
// 停止下载 RDB 文件，slave 的状态机回退，释放 fd 资源（相关的计数器会在状态转换的过程中被 reset）
// 释放进程中的 file description 资源，同时通过 unlink 删除在文件系统中的文件
void replicationAbortSyncTransfer(void) {
    redisAssert(server.repl_state == REDIS_REPL_TRANSFER);

    aeDeleteFileEvent(server.el,server.repl_transfer_s,AE_READABLE);
    close(server.repl_transfer_s);

    // close VS unlink
    // unlink 之后，会导致引用计数减少，这时候在 linux 看来，才是真正的删除了这个文件
    // close 并不会影响引用计数，所以并不会删除文件
    close(server.repl_transfer_fd); // 文件被 close 了（文件并没有被删除）
    unlink(server.repl_transfer_tmpfile);   // unlink 了，确保文件会被删除；这样一来，才不会在下次使用这个文件的时候，残留有这次的数据，确保文件时干净的，没有被污染
    zfree(server.repl_transfer_tmpfile);
    server.repl_state = REDIS_REPL_CONNECT;
}

/* Avoid the master to detect the slave is timing out while loading the
 * RDB file in initial synchronization. We send a single newline character
 * that is valid protocol but is guaranteed to either be sent entierly or
 * not, since the byte is indivisible.
 *
 * The function is called in two contexts: while we flush the current
 * data with emptyDb(), and while we load the new data received as an
 * RDB file from the master. */
void replicationSendNewlineToMaster(void) {
    static time_t newline_sent;
    if (time(NULL) != newline_sent) {
        newline_sent = time(NULL);
        if (write(server.repl_transfer_s,"\n",1) == -1) {
            /* Pinging back in this stage is best-effort. */
        }
    }
}

/* Callback used by emptyDb() while flushing away old data to load
 * the new dataset received by the master. */
void replicationEmptyDbCallback(void *privdata) {
    REDIS_NOTUSED(privdata);
    replicationSendNewlineToMaster();
}

/* Asynchronously read the SYNC payload we receive from a master */
// 将这个函数注册进 epoll-instance 里面去，作为 slave 连接 master 的 read-callback
// slave 就是通过这个函数，将 master 发送过来的 RDB 文件，加载进 slave redis-server 里面
#define REPL_MAX_WRITTEN_BEFORE_FSYNC (1024*1024*8) /* 8 MB */
void readSyncBulkPayload(aeEventLoop *el, int fd, void *privdata, int mask) {
    char buf[4096];
    ssize_t nread, readlen;
    off_t left;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(privdata);
    REDIS_NOTUSED(mask);

    /* If repl_transfer_size == -1 we still have to read the bulk length
     * from the master reply. */
    // 读取 RDB 文件的大小
    if (server.repl_transfer_size == -1) {

        // 调用读函数
        if (syncReadLine(fd,buf,1024,server.repl_syncio_timeout*1000) == -1) {
            redisLog(REDIS_WARNING,
                "I/O error reading bulk count from MASTER: %s",
                strerror(errno));
            goto error; // 不能成功的 read 到 repl_transfer_size，那就直接中断 repl 过程 replicationAbortSyncTransfer()
        }

        // 出错？
        if (buf[0] == '-') {
            redisLog(REDIS_WARNING,
                "MASTER aborted replication with an error: %s",
                buf+1);
            goto error;
        } else if (buf[0] == '\0') {
            /* At this stage just a newline works as a PING in order to take
             * the connection live. So we refresh our last interaction
             * timestamp. */
            // 只接到了一个作用和 PING 一样的 '\0'
            // 更新最后互动时间
            server.repl_transfer_lastio = server.unixtime;
            return;
        } else if (buf[0] != '$') {
            // 读入的内容出错，和协议格式不符
            redisLog(REDIS_WARNING,"Bad protocol from MASTER, the first byte is not '$' (we received '%s'), are you sure the host and port are right?", buf);
            goto error;
        }

        // 分析 RDB 文件大小，buf = "$40\r\n"
        server.repl_transfer_size = strtol(buf+1,NULL,10);

        redisLog(REDIS_NOTICE,
            "MASTER <-> SLAVE sync: receiving %lld bytes from master",
            (long long) server.repl_transfer_size);
        return; // 个人估计是不想 syncReadLine() 那里长时间阻塞，而且 master 那边，长度跟 RDB 文件也是分开两次 write 发过来的
    }

    /* Read bulk data */
    // 读数据
    // 还有多少字节要读？server.repl_transfer_read 在 REDIS_REPL_RECEIVE_PONG 的时候就被 reset 0 了
    left = server.repl_transfer_size - server.repl_transfer_read;
    readlen = (left < (signed)sizeof(buf)) ? left : (signed)sizeof(buf);    // 限制一次性最多读取 4 KB 数据
    // 读取
    nread = read(fd,buf,readlen);   // 一次最多 read 4k 内存
    if (nread <= 0) {
        redisLog(REDIS_WARNING,"I/O error trying to sync with MASTER: %s",
            (nread == -1) ? strerror(errno) : "connection lost");
        
        // nread = 0, 对应的情况就是对端执行了 close(socket_fd)，也就是 connection lost
        // 这种情况下，对端已经不会在发送数据过来了，自然是中断 repl
        replicationAbortSyncTransfer();
        return;
    }
    // 更新最后 RDB 产生的 IO 时间
    server.repl_transfer_lastio = server.unixtime;
    if (write(server.repl_transfer_fd,buf,nread) != nread) {    // 将 buf 里面接收到的数据落盘，形成本地的 RDB
        redisLog(REDIS_WARNING,"Write error or short write writing to the DB dump file needed for MASTER <-> SLAVE synchronization: %s", strerror(errno));
        goto error;
    }
    // 加上刚读取好的字节数
    server.repl_transfer_read += nread;

    /* Sync data on disk from time to time, otherwise at the end of the transfer
     * we may suffer a big delay as the memory buffers are copied into the
     * actual disk. */
    // 定期将读入的文件 fsync 到磁盘，到最后时因为 buffer 太多，一下子写入时撑爆 IO，导致的阻塞
    // 这样就不用担心 close 的时候，数据没有真正落盘了
    if (server.repl_transfer_read >=
        server.repl_transfer_last_fsync_off + REPL_MAX_WRITTEN_BEFORE_FSYNC)
    {
        off_t sync_size = server.repl_transfer_read -
                          server.repl_transfer_last_fsync_off;
        rdb_fsync_range(server.repl_transfer_fd,
            server.repl_transfer_last_fsync_off, sync_size);
        server.repl_transfer_last_fsync_off += sync_size;
    }

    /* Check if the transfer is now complete */
    // 检查 RDB 是否已经传送完毕
    if (server.repl_transfer_read == server.repl_transfer_size) {

        // 完毕，将临时文件改名为 dump.rdb
        if (rename(server.repl_transfer_tmpfile,server.rdb_filename) == -1) {
            redisLog(REDIS_WARNING,"Failed trying to rename the temp DB into dump.rdb in MASTER <-> SLAVE synchronization: %s", strerror(errno));
            replicationAbortSyncTransfer();
            return;
        }

        // 先清空旧数据库
        redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync: Flushing old data");
        signalFlushedDb(-1);
        emptyDb(replicationEmptyDbCallback);
        /* Before loading the DB into memory we need to delete the readable
         * handler, otherwise it will get called recursively since
         * rdbLoad() will call the event loop to process events from time to
         * time for non blocking loading. */
        // 先删除 master 的读事件监听，因为 rdbLoad() 函数也会监听读事件
        // TODO: repl_transfer_s 的这个读事件为何冲突呢？
        aeDeleteFileEvent(server.el,server.repl_transfer_s,AE_READABLE);

        // 载入 RDB
        if (rdbLoad(server.rdb_filename) != REDIS_OK) {
            redisLog(REDIS_WARNING,"Failed trying to load the MASTER synchronization DB from disk");
            replicationAbortSyncTransfer();
            return;
        }

        /* Final setup of the connected slave <- master link */
        // 关闭临时文件
        zfree(server.repl_transfer_tmpfile);
        close(server.repl_transfer_fd);

        // 将 master 设置成一个 redis client
        // 注意 createClient 会为 master 绑定事件，为接下来接收命令做好准备
        server.master = createClient(server.repl_transfer_s);
        // 标记这个客户端为 master 
        server.master->flags |= REDIS_MASTER;
        // 标记它为已验证身份
        server.master->authenticated = 1;
        // 更新复制状态
        server.repl_state = REDIS_REPL_CONNECTED;
        // 设置 master 的复制偏移量
        server.master->reploff = server.repl_master_initial_offset;
        // 保存 master 的 RUN ID
        memcpy(server.master->replrunid, server.repl_master_runid,
            sizeof(server.repl_master_runid));

        /* If master offset is set to -1, this master is old and is not
         * PSYNC capable, so we flag it accordingly. */
        // 如果 offset 被设置为 -1 ，那么表示 master 的版本低于 2.8 
        // 无法使用 PSYNC ，所以需要设置相应的标识值
        if (server.master->reploff == -1)
            server.master->flags |= REDIS_PRE_PSYNC;
        redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync: Finished with success");

        /* Restart the AOF subsystem now that we finished the sync. This
         * will trigger an AOF rewrite, and when done will start appending
         * to the new file. */
        // 如果有开启 AOF 持久化，那么重启 AOF 功能，并强制生成新数据库的 AOF 文件
        if (server.aof_state != REDIS_AOF_OFF) {
            int retry = 10;

            // 关闭
            stopAppendOnly();
            // 再重启
            while (retry-- && startAppendOnly() == REDIS_ERR) {
                redisLog(REDIS_WARNING,"Failed enabling the AOF after successful master synchronization! Trying it again in one second.");
                sleep(1);
            }
            if (!retry) {
                redisLog(REDIS_WARNING,"FATAL: this slave instance finished the synchronization with its master, but the AOF can't be turned on. Exiting now.");
                exit(1);
            }
        }
    }

    return;

error:
    replicationAbortSyncTransfer();
    return;
}

/* Send a synchronous command to the master. Used to send AUTH and
 * REPLCONF commands before starting the replication with SYNC.
 *
 * The command returns an sds string representing the result of the
 * operation. On error the first byte is a "-".
 */
// Redis 通常情况下是将命令的发送和回复用不同的事件处理器来异步处理的
// 但这里是同步地发送然后读取对端的回复
char *sendSynchronousCommand(int fd, ...) {
    va_list ap;
    sds cmd = sdsempty();
    char *arg, buf[256];

    /* Create the command to send to the master, we use simple inline
     * protocol for simplicity as currently we only send simple strings. */
    va_start(ap,fd);
    while(1) {
        arg = va_arg(ap, char*);
        if (arg == NULL) break;

        if (sdslen(cmd) != 0) cmd = sdscatlen(cmd," ",1);
        cmd = sdscat(cmd,arg);
    }
    cmd = sdscatlen(cmd,"\r\n",2);

    /* Transfer command to the server. */
    // 发送命令到 master 
    if (syncWrite(fd,cmd,sdslen(cmd),server.repl_syncio_timeout*1000) == -1) {
        sdsfree(cmd);
        return sdscatprintf(sdsempty(),"-Writing to master: %s",
                strerror(errno));
    }
    sdsfree(cmd);

    /* Read the reply from the server. */
    // 从 master 中读取回复
    if (syncReadLine(fd,buf,sizeof(buf),server.repl_syncio_timeout*1000) == -1)
    {
        return sdscatprintf(sdsempty(),"-Reading from master: %s",
                strerror(errno));
    }
    return sdsnew(buf);
}

/* Try a partial resynchronization with the master if we are about to reconnect.
 *
 * 在重连接之后，尝试进行部分重同步。
 *
 * If there is no cached master structure, at least try to issue a
 * "PSYNC ? -1" command in order to trigger a full resync using the PSYNC
 * command in order to obtain the master run id and the master replication
 * global offset.
 *
 * 如果 master 缓存为空，那么通过 "PSYNC ? -1" 命令来触发一次 full resync ，
 * 让 master 的 run id 和复制偏移量可以传到附属节点里面。
 *
 * This function is designed to be called from syncWithMaster(), so the
 * following assumptions are made:
 *
 * 这个函数由 syncWithMaster() 函数调用，它做了以下假设：
 *
 * 1) We pass the function an already connected socket "fd".
 *    一个已连接套接字 fd 会被传入函数
 * 2) This function does not close the file descriptor "fd". However in case
 *    of successful partial resynchronization, the function will reuse
 *    'fd' as file descriptor of the server.master client structure.
 *    函数不会关闭 fd 。
 *    当部分同步成功时，函数会将 fd 用作 server->master; 中的
 *    文件描述符。
 *
 * The function returns:
 * 以下是函数的返回值：
 *
 * PSYNC_CONTINUE: If the PSYNC command succeded and we can continue.
 *                 PSYNC 命令成功，可以继续。
 * PSYNC_FULLRESYNC: If PSYNC is supported but a full resync is needed.
 *                   In this case the master run_id and global replication
 *                   offset is saved.
 *                   master 支持 PSYNC 功能，但目前情况需要执行 full resync 。
 *                   在这种情况下， run_id 和全局复制偏移量会被保存。
 * PSYNC_NOT_SUPPORTED: If the server does not understand PSYNC at all and
 *                      the caller should fall back to SYNC.
 *                      master 不支持 PSYNC ，调用者应该下降到 SYNC 命令。
 */

#define PSYNC_CONTINUE 0
#define PSYNC_FULLRESYNC 1
#define PSYNC_NOT_SUPPORTED 2
int slaveTryPartialResynchronization(int fd) {
    char *psync_runid;
    char psync_offset[32];
    sds reply;

    /* Initially set repl_master_initial_offset to -1 to mark the current
     * master run_id and offset as not valid. Later if we'll be able to do
     * a FULL resync using the PSYNC command we'll set the offset at the
     * right value, so that this information will be propagated to the
     * client structure representing the master into server.master. */
    server.repl_master_initial_offset = -1;

    if (server.cached_master) {
        // 缓存存在，尝试部分重同步
        // 命令为 "PSYNC <master_run_id> <repl_offset>"
        psync_runid = server.cached_master->replrunid;
        snprintf(psync_offset,sizeof(psync_offset),"%lld", server.cached_master->reploff+1);
        redisLog(REDIS_NOTICE,"Trying a partial resynchronization (request %s:%s).", psync_runid, psync_offset);
    } else {
        // 缓存不存在
        // 发送 "PSYNC ? -1" ，要求完整重同步
        redisLog(REDIS_NOTICE,"Partial resynchronization not possible (no cached master)");
        psync_runid = "?";
        memcpy(psync_offset,"-1",3);    // 3 = '-' + '1' + '\0'
    }

    /* Issue the PSYNC command */
    // 向 master 发送 PSYNC 命令
    // 1. 第一次的话，将会发送 "PSYNC ? -1"
    //    同时 slave 会同步 block 在这里等待 master 回复（超时会结束 block 等待）
    // 2. 要是 PSYNC 的话，发送 "PSYNC ec8a50f43887ce9d69aaf49811fb7a885ac1a382 5406\r\n"
    //    要是 master 觉得没问题，那就会直接回复：可以进行 PYSNC，你就绪了之后就发起这个流程吧
    reply = sendSynchronousCommand(fd,"PSYNC",psync_runid,psync_offset,NULL);

    // 接收到 FULLRESYNC ，进行 full-resync，同时更新 repl_master_initial_offset（为了日后可以进行 PSYNC）。
    // 例如：下面的 1
    // "+FULLRESYNC 21a931865c82d28ec842c7c60bdc0ae6ceeaa12a 1\r\n\000"
    if (!strncmp(reply,"+FULLRESYNC",11)) {
        char *runid = NULL, *offset = NULL;

        /* FULL RESYNC, parse the reply in order to extract the run id
         * and the replication offset. */
        // 分析并记录 master 的 run id
        runid = strchr(reply,' ');
        if (runid) {
            runid++;
            offset = strchr(runid,' ');
            if (offset) offset++;
        }
        // 检查 run id 的合法性
        if (!runid || !offset || (offset-runid-1) != REDIS_RUN_ID_SIZE) {
            redisLog(REDIS_WARNING,
                "Master replied with wrong +FULLRESYNC syntax.");
            /* This is an unexpected condition, actually the +FULLRESYNC
             * reply means that the master supports PSYNC, but the reply
             * format seems wrong. To stay safe we blank the master
             * runid to make sure next PSYNCs will fail. */
            //  master 支持 PSYNC ，但是却发来了异常的 run id
            // 只好将 run id 设为 0 ，让下次 PSYNC 时失败
            memset(server.repl_master_runid,0,REDIS_RUN_ID_SIZE+1);
        } else {
            /* 之所以要做这么多东西，要保留这么多东西，基本都是为了 PSYNC 的
             * 而且在不需要重新 FULL SYNC 的情况下，这些数据都是能够被更新、被沿用的
             */
            // 保存 run id
            memcpy(server.repl_master_runid, runid, offset-runid-1);
            server.repl_master_runid[REDIS_RUN_ID_SIZE] = '\0';
            // 以及 initial offset
            server.repl_master_initial_offset = strtoll(offset,NULL,10);
            // 打印日志，这是一个 FULL resync
            redisLog(REDIS_NOTICE,"Full resync from master: %s:%lld",
                server.repl_master_runid,
                server.repl_master_initial_offset);
        }
        /* We are going to full resync, discard the cached master structure. */
        // 要开始完整重同步，缓存中的 master 已经没用了，清除它
        replicationDiscardCachedMaster();
        sdsfree(reply);
        
        // 返回状态
        return PSYNC_FULLRESYNC;
    }

    // 接收到 CONTINUE ，进行 partial resync
    // slave 需要做的准备有两个：
    // 1. 重新把 master client 的 socket fd 放进监听 epoll 监听里面
    // 2. return 结果
    if (!strncmp(reply,"+CONTINUE",9)) {
        /* Partial resync was accepted, set the replication state accordingly */
        redisLog(REDIS_NOTICE,
            "Successful partial resynchronization with master.");
        sdsfree(reply);
        // 将缓存中的 master 设为当前 master
        replicationResurrectCachedMaster(fd);

        // 返回状态
        return PSYNC_CONTINUE;
    }

    /* If we reach this point we receied either an error since the master does
     * not understand PSYNC, or an unexpected reply from the master.
     * Return PSYNC_NOT_SUPPORTED to the caller in both cases. */
    // TODO: timeout case 的处理在哪里？
    // 接收到错误？
    if (strncmp(reply,"-ERR",4)) {
        /* If it's not an error, log the unexpected event. */
        redisLog(REDIS_WARNING,
            "Unexpected reply to PSYNC from master: %s", reply);
    } else {
        redisLog(REDIS_NOTICE,
            "Master does not support PSYNC or is in "
            "error state (reply: %s)", reply);
    }
    sdsfree(reply);
    replicationDiscardCachedMaster();

    //  master 不支持 PSYNC
    return PSYNC_NOT_SUPPORTED;
}

// 本函数在 connectWithMaster() 的时候，注册进 epoll 里面，等候触发（也就是等候 master 发送数据过来）
//  slave 异步处理 master 的数据
// 没办法，因为 slaveof 是一个异步的命令，不然同步等待 master listen slave 的连接请求，可能会很久
// master 可能有很繁重的任务，而且，slave 是要等到 slave 上面的 fd 可读可写之后，才进行 read\write 操作的
// 这样一来可能会等更久，所以，异步才是比较好的选择
void syncWithMaster(aeEventLoop *el, int fd, void *privdata, int mask) {
    char tmpfile[256], *err;
    int dfd, maxtries = 5;
    int sockerr = 0, psync_result;
    socklen_t errlen = sizeof(sockerr);
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(privdata);    // 因为这个 slave 调用的，所以对端自然就是 master 了，fd 就够用了
    REDIS_NOTUSED(mask);

    /* If this event fired after the user turned the instance into a master
     * with SLAVEOF NO ONE we must just return ASAP. */
    // 如果当前的 redis-server 处于 REDIS_REPL_NONE 时（可能是在 connect 成功之前，slaveof no one CMD 来了），
    // 那么得尽快关闭与 master 连接的 fd，因为走接下来的流程可能会很耗费资源
    if (server.repl_state == REDIS_REPL_NONE) {
        close(fd);
        return;
    }

    /* Check for errors in the socket. */
    /**
     * 前提：这个 socket 是进行了异步 connect 的，而这时候，怎么看最后究竟是不是 connect 成功了呢？
     * 因为这个异步非阻塞的 connect 是调用完就走的；现在这个 connect 出来的 socket 有 read event 被触发了
     * 你必然是要先检查一下：上次的 connect 是不是成功，然后才能够使用这个 connect 出来的 socket 的
     * 
     * man connect:
     * EINPROGRESS
     * The socket is **nonblocking and the connection** cannot be completed immediately.
     * It is possible to select(2) or poll(2) for completion by selecting the socket for writing.
     * After select(2) indicates writability, use getsockopt(2) to read the SO_ERROR option
     * at level SOL_SOCKET to determine whether connect() completed successfully (SO_ERROR
     * is zero) or unsuccessfully (SO_ERROR is one of the usual error codes listed here,
     * explaining the reason for the failure).
    */
    // 检查套接字错误(精彩绝伦，getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen))
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    if (sockerr) {
        aeDeleteFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE);
        redisLog(REDIS_WARNING,"Error condition on socket for SYNC: %s",
            strerror(sockerr));
        goto error;
    }

    /* If we were connecting, it's time to send a non blocking PING, we want to
     * make sure the master is able to reply before going into the actual
     * replication process where we have long timeouts in the order of
     * seconds (in the meantime the slave would block). */
    // 如果状态为 CONNECTING ，那么在进行初次同步之前，
    // 向 master 发送一个非阻塞的 PONG 
    // 因为接下来的 RDB 文件发送非常耗时，所以我们想确认 master 真的能访问
    if (server.repl_state == REDIS_REPL_CONNECTING) {
        redisLog(REDIS_NOTICE,"Non blocking connect for SYNC fired the event.");
        /* Delete the writable event so that the readable event remains
         * registered and we can wait for the PONG reply. */
        // 手动发送同步 PING ，暂时取消监听写事件
        aeDeleteFileEvent(server.el,fd,AE_WRITABLE);
        // 更新状态
        server.repl_state = REDIS_REPL_RECEIVE_PONG;
        /* Send the PING, don't check for errors at all, we have the timeout
         * that will take care about this. */
        // 同步发送 PING
        syncWrite(fd,"PING\r\n",6,100); // 设置了 100 ms 的 write 超时时间

        // 返回，等待 PONG 到达
        // TODO: 要是 write 失败的话，slave 会等待什么？master 还会回吗？
        return;
    }

    /* Receive the PONG command. */
    // 接收 PONG 命令，因为 slave 的 read 事件注册的也是 syncWithMaster() 这个函数呀
    if (server.repl_state == REDIS_REPL_RECEIVE_PONG) {
        /* 你会发现：自从 slave 收到 pong 之后，slave 执行的是一堆带超时的同步阻塞操作
         * 因为这个状态下的 redis-server，即将要成为 slave，所有条件都成熟了，而且这时候的准 slave redis-server
         * 里面的数据，并不一定跟 master 是一致的，那么这时候是不应该响应任何的读写请求的，
         * 不然可能会返回一个错误的 resp，所以就直接采用同步阻塞操作了
         */
        char buf[1024];

        /* Delete the readable event, we no longer need it now that there is
         * the PING reply to read. */
        // 手动同步接收 PONG ，暂时取消监听读事件
        aeDeleteFileEvent(server.el,fd,AE_READABLE);

        /* Read the reply with explicit timeout. */
        // 尝试在指定时间限制内读取 PONG
        buf[0] = '\0';
        // 同步接收 PONG
        if (syncReadLine(fd,buf,sizeof(buf),
            server.repl_syncio_timeout*1000) == -1)
        {
            redisLog(REDIS_WARNING,
                "I/O error reading PING reply from master: %s",
                strerror(errno));
            goto error;
        }

        /* We accept only two replies as valid, a positive +PONG reply
         * (we just check for "+") or an authentication error.
         * Note that older versions of Redis replied with "operation not
         * permitted" instead of using a proper error code, so we test
         * both. */
        // 接收到的数据只有两种可能：
        // 第一种是 +PONG ，第二种是因为未验证而出现的 -NOAUTH 错误
        if (buf[0] != '+' &&
            strncmp(buf,"-NOAUTH",7) != 0 &&
            strncmp(buf,"-ERR operation not permitted",28) != 0)
        {
            // 接收到未知的 reply（既不是 -NOAUTH，也不是 -ERR）
            redisLog(REDIS_WARNING,"Error reply to PING from master: '%s'",buf);
            goto error;
        } else {
            // 接收到 PONG、或者是 -NOAUTH、-ERR operation not permitted
            redisLog(REDIS_NOTICE,
                "Master replied to PING, replication can continue...");
        }
    }

    /* AUTH with the master if required. */
    // 进行身份验证
    if(server.masterauth) {
        err = sendSynchronousCommand(fd,"AUTH",server.masterauth,NULL);
        if (err[0] == '-') {    // 当 error 是 '-' 开头，也就意味着同步操作出了问题
            redisLog(REDIS_WARNING,"Unable to AUTH to MASTER: %s",err);
            sdsfree(err);
            goto error;
        }
        sdsfree(err);
    }

    /* Set the slave port, so that Master's INFO command can list the
     * slave listening port correctly. */
    // 将 slave 的端口同步发送给 master ，
    // 使得 master 的 INFO 命令可以显示 slave 正在监听的端口，在哪个端口提供 service
    {
        sds port = sdsfromlonglong(server.port);
        err = sendSynchronousCommand(fd,"REPLCONF","listening-port",port,
                                         NULL);
        sdsfree(port);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF listening-port. */
        if (err[0] == '-') {
            redisLog(REDIS_NOTICE,"(Non critical) Master does not understand REPLCONF listening-port: %s", err);
        }
        sdsfree(err);
    }

    /* Try a partial resynchonization. If we don't have a cached master
     * slaveTryPartialResynchronization() will at least try to use PSYNC
     * to start a full resynchronization so that we get the master run id
     * and the global offset, to try a partial resync at the next
     * reconnection attempt. */
    // 根据返回的结果决定是执行部分 resync ，还是 full-resync
    psync_result = slaveTryPartialResynchronization(fd);

    // 可以执行部分 resync
    if (psync_result == PSYNC_CONTINUE) {
        redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync: Master accepted a Partial Resynchronization.");
        // 返回
        // TODO: (DONE) 为什么 psync 是直接 return ？
        // 因为只需要做三个准备：
        // 1. 跟 master 协商是否能够进行 PSYNC（同步的）
        // 2. 恢复 slave 的 server.master，并且把新创建的 socket 注册进 epoll 里面
        // 3. 等待 master 将 PSYNC 的数据发过来就好了（这不就是要去 epoll 了吗，所以就 return 了）
        return;
    }

    /* Fall back to SYNC if needed. Otherwise psync_result == PSYNC_FULLRESYNC
     * and the server.repl_master_runid and repl_master_initial_offset are
     * already populated. */
    //  master 不支持 PSYNC ，发送 SYNC
    if (psync_result == PSYNC_NOT_SUPPORTED) {
        redisLog(REDIS_NOTICE,"Retrying with SYNC...");
        // 向 master 发送 SYNC 命令
        if (syncWrite(fd,"SYNC\r\n",6,server.repl_syncio_timeout*1000) == -1) {
            redisLog(REDIS_WARNING,"I/O error writing to MASTER: %s",
                strerror(errno));
            goto error;
        }
    }

    // 如果执行到这里，
    // 那么 psync_result == PSYNC_FULLRESYNC 或 PSYNC_NOT_SUPPORTED

    /* Prepare a suitable temp file for bulk transfer */
    // 打开一个临时文件，用于写入和保存接下来从 master 传来的 RDB 文件数据
    while(maxtries--) {
        snprintf(tmpfile,256,
            "temp-%d.%ld.rdb",(int)server.unixtime,(long int)getpid());
        // O_EXCL | O_CREAT 的配合使用，确保了这时候，只有当前进程在操作这一个文件，这是 O_EXCL 的一个特性
        dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);   // 上一次可能会发生中途取消同步，所以这里并没有添加 O_APPEND 的 flag，而是采用从头开始的覆盖式写入

        if (dfd != -1) break;
        sleep(1);
    }
    if (dfd == -1) {
        redisLog(REDIS_WARNING,"Opening the temp file needed for MASTER <-> SLAVE synchronization: %s",strerror(errno));
        goto error;
    }

    /* Setup the non blocking download of the bulk file. */
    // 为 slave 连接向 master 的 socket 设置一个读事件处理器，来读取 master 传过来的 RDB 文件
    if (aeCreateFileEvent(server.el,fd, AE_READABLE,readSyncBulkPayload,NULL)
            == AE_ERR)
    {
        redisLog(REDIS_WARNING,
            "Can't create readable event for SYNC: %s (fd=%d)",
            strerror(errno),fd);
        goto error;
    }

    // 设置状态
    server.repl_state = REDIS_REPL_TRANSFER;

    // 更新统计信息
    server.repl_transfer_size = -1;
    server.repl_transfer_read = 0;
    server.repl_transfer_last_fsync_off = 0;
    server.repl_transfer_fd = dfd;
    server.repl_transfer_lastio = server.unixtime;
    server.repl_transfer_tmpfile = zstrdup(tmpfile);

    return;

error:
    close(fd);
    server.repl_transfer_s = -1;
    server.repl_state = REDIS_REPL_CONNECT; // 退回上一次的状态，尝试重新连接
    return;
}

// slave 以非阻塞方式（这里采用的是异步方案）连接 master
int connectWithMaster(void) {
    int fd;

    // 连接 master 
    fd = anetTcpNonBlockConnect(NULL,server.masterhost,server.masterport);
    if (fd == -1) {
        redisLog(REDIS_WARNING,"Unable to connect to MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }

    // 将与 master 相连接的 socket 注册进 epoll-instance 里面，并监听等待 read + write 事件
    if (aeCreateFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE,syncWithMaster,NULL) ==
            AE_ERR)
    {
        close(fd);
        redisLog(REDIS_WARNING,"Can't create readable event for SYNC");
        return REDIS_ERR;
    }

    // 初始化统计变量
    server.repl_transfer_lastio = server.unixtime;
    server.repl_transfer_s = fd;

    // 将状态改为已连接
    server.repl_state = REDIS_REPL_CONNECTING;

    return REDIS_OK;
}

/* This function can be called when a non blocking connection is currently
 * in progress to undo it. */
// 取消正在进行的连接
void undoConnectWithMaster(void) {
    int fd = server.repl_transfer_s;

    // 连接必须处于正在连接状态
    redisAssert(server.repl_state == REDIS_REPL_CONNECTING ||
                server.repl_state == REDIS_REPL_RECEIVE_PONG);
    aeDeleteFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE);
    close(fd);
    server.repl_transfer_s = -1;
    // 回到 CONNECT 状态
    server.repl_state = REDIS_REPL_CONNECT;
}

/* This function aborts a non blocking replication attempt if there is one
 * in progress, by canceling the non-blocking connect attempt or
 * the initial bulk transfer.
 *
 * 如果有正在进行的非阻塞复制在进行，那么取消它。
 *
 * If there was a replication handshake in progress 1 is returned and
 * the replication state (server.repl_state) set to REDIS_REPL_CONNECT.
 *
 * 如果复制在握手阶段被取消，那么返回 1 ，
 * 并且 server.repl_state 被设置为 REDIS_REPL_CONNECT 。
 *
 * Otherwise zero is returned and no operation is perforemd at all. 
 *
 * 否则返回 0 ，并且不执行任何操作。
 */
int cancelReplicationHandshake(void) {
    if (server.repl_state == REDIS_REPL_TRANSFER) {
        replicationAbortSyncTransfer();
    } else if (server.repl_state == REDIS_REPL_CONNECTING ||
             server.repl_state == REDIS_REPL_RECEIVE_PONG)
    {
        undoConnectWithMaster();
    } else {
        return 0;
    }
    return 1;
}

/* Set replication to the specified master address and port. */
// 将服务器设为指定地址的 slave 
void replicationSetMaster(char *ip, int port) {

    // 清除原有的 master 信息（如果有的话）
    sdsfree(server.masterhost);

    // IP
    server.masterhost = sdsnew(ip);

    // 端口
    server.masterport = port;

    // 如果之前有 master 地址，那么释放相关资源
    if (server.master) freeClient(server.master);
    // 断开所有 slave 的连接，强制所有 slave 执行重同步
    disconnectSlaves(); /* Force our slaves to resync with us as well. */
    // 清空可能有的 master 缓存，因为已经不会执行 PSYNC 了
    replicationDiscardCachedMaster(); /* Don't try a PSYNC. */
    // 释放 backlog ，同理， PSYNC 目前已经不会执行了
    freeReplicationBacklog(); /* Don't allow our chained slaves to PSYNC. */
    // 取消之前的复制进程（如果有的话）
    cancelReplicationHandshake();

    // 进入连接状态（重点）
    // TODO:(DONE) 既然 redis 代码中，repl 的状态机切换是分散在各个文件里面的，那为何不再这里直接异步 connect 呢？
    // 为了能够重试！当某一次 connect 中途出了问题，那么我可以把 REDIS_REPL_CONNECTING 状态回退到 REDIS_REPL_CONNECT 状态
    // 然后在 replicationCron() 里面再次尝试
    server.repl_state = REDIS_REPL_CONNECT; // 实际上是进行异步 connect 的
    server.master_repl_offset = 0;
    server.repl_down_since = 0;
}

/* Cancel replication, setting the instance as a master itself. */
// 取消复制，将服务器设置为 master 
void replicationUnsetMaster(void) {

    if (server.masterhost == NULL) return; /* Nothing to do. */

    sdsfree(server.masterhost);
    server.masterhost = NULL;

    if (server.master) {
        if (listLength(server.slaves) == 0) {
            /* If this instance is turned into a master and there are no
             * slaves, it inherits the replication offset from the master.
             * Under certain conditions this makes replicas comparable by
             * replication offset to understand what is the most updated. */
            server.master_repl_offset = server.master->reploff;
            freeReplicationBacklog();
        }
        freeClient(server.master);
    }

    replicationDiscardCachedMaster();

    cancelReplicationHandshake();

    server.repl_state = REDIS_REPL_NONE;
}

// 这里的 client 是即将成为 slave 的那个 redis-server 相连通的 redis-cli
void slaveofCommand(redisClient *c) {
    /* SLAVEOF is not allowed in cluster mode as replication is automatically
     * configured using the current address of the master node. */
    // 不允许在集群模式中使用
    if (server.cluster_enabled) {
        addReplyError(c,"SLAVEOF not allowed in cluster mode.");
        return;
    }

    /* The special host/port combination "NO" "ONE" turns the instance
     * into a master. Otherwise the new master address is set. */
    // SLAVEOF NO ONE 让 slave 转为 master 
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        if (server.masterhost) {
            // 让服务器取消复制，成为 master 
            replicationUnsetMaster();
            redisLog(REDIS_NOTICE,"MASTER MODE enabled (user request)");
        }
    } else {
        long port;

        // 获取端口参数
        // (gdb) p (char*)c->argv[1]->ptr = "127.0.0.1"; 
        // (gdb) p (char*)c->argv[2]->ptr = "1000";
        if ((getLongFromObjectOrReply(c, c->argv[2], &port, NULL) != REDIS_OK))
            return;

        /* Check if we are already attached to the specified slave */
        // 检查输入的 host 和 port 是否服务器目前的 master 
        // 如果是的话，向客户端返回 +OK ，不做其他动作
        if (server.masterhost && !strcasecmp(server.masterhost,c->argv[1]->ptr)
            && server.masterport == port) {
            redisLog(REDIS_NOTICE,"SLAVE OF would result into synchronization with the master we are already connected with. No operation performed.");
            addReplySds(c,sdsnew("+OK Already connected to specified master\r\n"));
            return;
        }

        /* There was no previous master or the user specified a different one,
         * we can continue. */
        // 没有前任 master ，或者客户端指定了新的 master 
        // 开始进行连接操作，连接完毕之后，等 master 准备好了 RDB 文件才会开始 full sync
        replicationSetMaster(c->argv[1]->ptr, port);    // 
        redisLog(REDIS_NOTICE,"SLAVE OF %s:%d enabled (user request)",
            server.masterhost, server.masterport);
    }
    addReply(c,shared.ok);
}

/* Send a REPLCONF ACK command to the master to inform it about the current
 * processed offset. If we are not connected with a master, the command has
 * no effects. */
// 向 master 发送 REPLCONF AKC ，告知当前处理的偏移量
// 如果未连接上 master ，那么这个函数没有实际效果
void replicationSendAck(void) {
    redisClient *c = server.master;

    if (c != NULL) {
        c->flags |= REDIS_MASTER_FORCE_REPLY;
        addReplyMultiBulkLen(c,3);
        addReplyBulkCString(c,"REPLCONF");
        addReplyBulkCString(c,"ACK");
        // 发送偏移量
        addReplyBulkLongLong(c,c->reploff);
        c->flags &= ~REDIS_MASTER_FORCE_REPLY;
    }
}

/* ---------------------- MASTER CACHING FOR PSYNC（slave use those function） -------------------------- */
// 处理的是 master <----> slave 之间短暂的断开连接的情况
/* In order to implement partial synchronization we need to be able to cache
 * our master's client structure after a transient disconnection.
 * （是作为 replication 的 slave 要把失联的 slave --> master connection client 存起来）
 *
 * 为了实现 partial synchronization ，
 * slave 需要一个 cache 来在 master 断线时将 master 保存到 cache 上。
 *
 * It is cached into server.cached_master and flushed away using the following
 * functions. 
 *
 * 以下是该 cache 的设置和清除函数。
 */

/* This function is called by freeClient() in order to cache the master
 * client structure instead of destryoing it（彻底毁掉不能通过 psync 恢复了）. freeClient() will return
 * ASAP after this function returns, so every action needed to avoid problems
 * with a client that is really "suspended" has to be done by this function.
 *
 * 这个函数由 freeClient() 函数调用，它将当前的 master 记录到 master cache 里面，
 * 然后返回。
 *
 * The other functions that will deal with the cached master are:
 *
 * 其他和 master cahce 有关的函数是：
 *
 * replicationDiscardCachedMaster() that will make sure to kill the client
 * as for some reason we don't want to use it in the future.
 *
 * replicationDiscardCachedMaster() 确认清空整个 master ，不对它进行缓存。
 *
 * replicationResurrectCachedMaster() that is used after a successful PSYNC
 * handshake in order to reactivate the cached master.
 *
 * replicationResurrectCachedMaster() 在 PSYNC 成功时将缓存中的 master 提取出来，
 * 重新成为新的 master 。
 */
// 这一行为，将会由 replicationCron() 进行定期检查，一旦发现 master 在 server.repl_timeout 这么长时间都没有 reply 过
// 则认为 master 断开了连接，开始进入：可以进行 psync 的等待重新连接阶段
void replicationCacheMaster(redisClient *c) {
    listNode *ln;

    redisAssert(server.master != NULL && server.cached_master == NULL);
    redisLog(REDIS_NOTICE,"Caching the disconnected master state.");

    /* Remove from the list of clients, we don't want this client to be
     * listed by CLIENT LIST or processed in any way by batch operations. */
    // 从客户端链表中移除 master 
    ln = listSearchKey(server.clients,c);
    redisAssert(ln != NULL);
    listDelNode(server.clients,ln);

    /* Save the master. Server.master will be set to null later by
     * replicationHandleMasterDisconnection(). */
    // 缓存 master
    server.cached_master = server.master;

    /* Remove the event handlers and close the socket. We'll later reuse
     * the socket of the new connection with the master during PSYNC. */
    // 删除事件监视，关闭 socket
    aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
    aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
    close(c->fd);

    /* Set fd to -1 so that we can safely call freeClient(c) later. */
    c->fd = -1;

    /* Invalidate the Peer ID cache. */
    if (c->peerid) {
        sdsfree(c->peerid);
        c->peerid = NULL;
    }

    /* Caching the master happens instead of the actual freeClient() call,
     * so make sure to adjust the replication state. This function will
     * also set server.master to NULL. */
    // 重置复制状态，并将 server.master 设为 NULL
    // 并强制断开这个服务器的所有 slave ，让它们执行 resync TODO:(DONE) 为什么要强制断开 slave 的 slave？
    // 相当于强制让 slave_1 的 slave_2，再次 跟 slave_1 PSYNC 一次，确保数据库内容的完备
    // 因为能调用 replicationCacheMaster() 这个函数，必然意味着：
    // 1. 这个 slave_1 曾经跟 master 短暂失联(所以 slave_1 必然会有比较多的数据新增)
    // 2. 这个 slave_1 能够进行 PSYNC

    // TODO:（DONE） slave_2 万一比 slave_1 更早发起 sync 呢？
    // 不慌，slave_1 会拒绝掉的。syncCommand() 最开头就做出了检查
    replicationHandleMasterDisconnection();
}

/* Free a cached master, called when there are no longer the conditions for
 * a partial resync on reconnection. 
 *
 * 清空 master 缓存，在条件已经不可能执行 partial resync 时执行
 */
void replicationDiscardCachedMaster(void) {

    if (server.cached_master == NULL) return;

    redisLog(REDIS_NOTICE,"Discarding previously cached master state.");
    server.cached_master->flags &= ~REDIS_MASTER;
    freeClient(server.cached_master);
    server.cached_master = NULL;
}

/* Turn the cached master into the current master, using the file descriptor
 * passed as argument as the socket for the new master.
 *
 * 将缓存中的 master 设置为服务器的当前 master 。
 *
 * This funciton is called when successfully setup a partial resynchronization
 * so the stream of data that we'll receive will start from were this
 * master left. 
 *
 * 当部分重同步准备就绪之后，调用这个函数。
 * master 断开之前遗留下来的数据可以继续使用。
 */
// 当 slave 心跳断开了，就会直接 close 掉 fd，但是保留 slave --> master 的 client context 信息罢了
void replicationResurrectCachedMaster(int newfd) {
    
    // 设置 master
    server.master = server.cached_master;
    server.cached_master = NULL;

    server.master->fd = newfd;  // 采用 slave 新 connect 去 master 的 socket-fd

    server.master->flags &= ~(REDIS_CLOSE_AFTER_REPLY|REDIS_CLOSE_ASAP);

    server.master->authenticated = 1;
    server.master->lastinteraction = server.unixtime;

    // 回到已连接状态
    server.repl_state = REDIS_REPL_CONNECTED;

    /* Re-add to the list of clients. */
    // 将 master 重新加入到客户端列表中
    listAddNodeTail(server.clients,server.master);
    // 监听 master 的读事件
    // 心跳断开之后，master、slave 都会各自 close(socket_fd)，所以重新 connect 上之后，是要把新的 socket_fd 主从进去 epoll-instance 的
    if (aeCreateFileEvent(server.el, newfd, AE_READABLE,
                          readQueryFromClient, server.master)) {
        redisLog(REDIS_WARNING,"Error resurrecting the cached master, impossible to add the readable handler: %s", strerror(errno));
        freeClientAsync(server.master); /* Close ASAP. */
    }

    /* We may also need to install the write handler as well if there is
     * pending data in the write buffers. */
    if (server.master->bufpos || listLength(server.master->reply)) {
        if (aeCreateFileEvent(server.el, newfd, AE_WRITABLE,
                          sendReplyToClient, server.master)) {
            redisLog(REDIS_WARNING,"Error resurrecting the cached master, impossible to add the writable handler: %s", strerror(errno));
            freeClientAsync(server.master); /* Close ASAP. */
        }
    }
}

/* ------------------------- MIN-SLAVES-TO-WRITE  --------------------------- */

/* This function counts the number of slaves with lag <= min-slaves-max-lag.
 * 
 * 计算那些延迟值少于等于 min-slaves-max-lag 的 slave 数量。
 *
 * If the option is active, the server will prevent writes if there are not
 * enough connected slaves with the specified lag (or less). 
 *
 * 如果服务器开启了 min-slaves-max-lag 选项，
 * 那么在这个选项所指定的条件达不到时，服务器将阻止写操作执行。
 */
void refreshGoodSlavesCount(void) {
    listIter li;
    listNode *ln;
    int good = 0;

    if (!server.repl_min_slaves_to_write ||
        !server.repl_min_slaves_max_lag) return;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        // 计算延迟值
        time_t lag = server.unixtime - slave->repl_ack_time;

        // 计入 GOOD
        if (slave->replstate == REDIS_REPL_ONLINE &&
            lag <= server.repl_min_slaves_max_lag) good++;
    }

    // 更新状态良好的 slave 数量
    server.repl_good_slaves_count = good;
}

/* ----------------------- REPLICATION SCRIPT CACHE --------------------------
 * The goal of this code is to keep track of scripts already sent to every
 * connected slave, in order to be able to replicate EVALSHA as it is without
 * translating it to EVAL every time it is possible.
 *
 * 这部分代码的目的是，
 * 将那些已经发送给所有已连接 slave 的脚本保存到缓存里面，
 * 这样在执行过一次 EVAL 之后，其他时候都可以直接发送 EVALSHA 了。
 *
 * We use a capped collection implemented by a hash table for fast lookup
 * of scripts we can send as EVALSHA, plus a linked list that is used for
 * eviction of the oldest entry when the max number of items is reached.
 *
 * 程序构建了一个固定大小的集合（capped collection），
 * 该集合由哈希结构和一个链表组成，
 * 哈希负责快速查找，而链表则负责形成一个 FIFO 队列，
 * 在脚本的数量超过最大值时，最先保存的脚本将被删除。
 *
 * We don't care about taking a different cache for every different slave
 * since to fill the cache again is not very costly, the goal of this code
 * is to avoid that the same big script is trasmitted a big number of times
 * per second wasting bandwidth and processor speed, but it is not a problem
 * if we need to rebuild the cache from scratch from time to time, every used
 * script will need to be transmitted a single time to reappear in the cache.
 *
 * Redis 我们不是为每个 slave 保存独立的脚本缓存，
 * 而是让所有 slave 都共用一个全局缓存。
 * 这是因为重新填充脚本到缓存中的操作并不昂贵，
 * 这个程序的目的是避免在短时间内发送同一个大脚本多次，
 * 造成带宽和 CPU 浪费，
 * 但时不时重新建立一次缓存的代码并不高昂，
 * 每次将一个脚本添加到缓存中时，都需要发送这个脚本一次。
 *
 * This is how the system works:
 *
 * 以下是这个系统的工作方式：
 *
 * 1) Every time a new slave connects, we flush the whole script cache.
 *    每次有新的 slave 连接时，清空所有脚本缓存。
 *
 * 2) We only send as EVALSHA what was sent to the master as EVALSHA, without
 *    trying to convert EVAL into EVALSHA specifically for slaves.
 *    程序只在 master 接到 EVALSHA 时才向 slave 发送 EVALSHA ，
 *    它不会主动尝试将 EVAL 转换成 EVALSHA 。
 *
 * 3) Every time we trasmit a script as EVAL to the slaves, we also add the
 *    corresponding SHA1 of the script into the cache as we are sure every
 *    slave knows about the script starting from now.
 *    每次将脚本通过 EVAL 命令发送给所有 slave 时，
 *    将脚本的 SHA1 键保存到脚本字典中，字典的键为 SHA1 ，值为 NULL ，
 *    这样我们就知道，只要脚本的 SHA1 在字典中，
 *    那么这个脚本就存在于所有 slave 中。
 *
 * 4) On SCRIPT FLUSH command, we replicate the command to all the slaves
 *    and at the same time flush the script cache.
 *    当客户端执行 SCRIPT FLUSH 的时候，服务器将该命令复制给所有 slave ，
 *    让它们也刷新自己的脚本缓存。
 *
 * 5) When the last slave disconnects, flush the cache.
 *    当所有 slave 都断开时，清空脚本。
 *
 * 6) We handle SCRIPT LOAD as well since that's how scripts are loaded
 *    in the master sometimes.
 *    SCRIPT LOAD 命令对这个脚本缓存的作用和 EVAL 一样。
 */

/* Initialize the script cache, only called at startup. */
// 初始化缓存，只在服务器启动时调用
void replicationScriptCacheInit(void) {
    // 最大缓存脚本数
    server.repl_scriptcache_size = 10000;
    // 字典
    server.repl_scriptcache_dict = dictCreate(&replScriptCacheDictType,NULL);
    // FIFO 队列
    server.repl_scriptcache_fifo = listCreate();
}

/* Empty the script cache. Should be called every time we are no longer sure
 * that every slave knows about all the scripts in our set, or when the
 * current AOF "context" is no longer aware of the script. In general we
 * should flush the cache:
 *
 * 清空脚本缓存。
 *
 * 在以下情况下执行：
 *
 * 1) Every time a new slave reconnects to this master and performs a
 *    full SYNC (PSYNC does not require flushing).
 *    有新 slave 连入，并且执行了一次 full SYNC ， PSYNC 无须清空缓存
 * 2) Every time an AOF rewrite is performed.
 *    每次执行 AOF 重写时
 * 3) Every time we are left without slaves at all, and AOF is off, in order
 *    to reclaim otherwise unused memory.
 *    在没有任何 slave ，AOF 关闭的时候，为节约内存而执行清空。
 */
void replicationScriptCacheFlush(void) {
    dictEmpty(server.repl_scriptcache_dict,NULL);
    listRelease(server.repl_scriptcache_fifo);
    server.repl_scriptcache_fifo = listCreate();
}

/* Add an entry into the script cache, if we reach max number of entries the
 * oldest is removed from the list. 
 *
 * 将脚本的 SHA1 添加到缓存中，
 * 如果缓存的数量已达到最大值，那么删除最旧的那个脚本（FIFO）
 */
void replicationScriptCacheAdd(sds sha1) {
    int retval;
    sds key = sdsdup(sha1);

    /* Evict oldest. */
    // 如果大小超过数量限制，那么删除最旧
    if (listLength(server.repl_scriptcache_fifo) == server.repl_scriptcache_size)
    {
        listNode *ln = listLast(server.repl_scriptcache_fifo);
        sds oldest = listNodeValue(ln);

        retval = dictDelete(server.repl_scriptcache_dict,oldest);
        redisAssert(retval == DICT_OK);
        listDelNode(server.repl_scriptcache_fifo,ln);
    }

    /* Add current. */
    // 添加 SHA1
    retval = dictAdd(server.repl_scriptcache_dict,key,NULL);
    listAddNodeHead(server.repl_scriptcache_fifo,key);
    redisAssert(retval == DICT_OK);
}

/* Returns non-zero if the specified entry exists inside the cache, that is,
 * if all the slaves are aware of this script SHA1. */
// 如果脚本存在于脚本，那么返回 1 ；否则，返回 0 。
int replicationScriptCacheExists(sds sha1) {
    return dictFind(server.repl_scriptcache_dict,sha1) != NULL;
}

// TODO: 3.0 之后再看吧，WAIT numreplicas timeout, 3.0 才正式作为 CMD 支持发布
/* ----------------------- SYNCHRONOUS REPLICATION --------------------------
 * Redis synchronous replication design can be summarized in points:
 *
 * - Redis masters have a global replication offset, used by PSYNC.
 * - Master increment the offset every time new commands are sent to slaves.
 * - Slaves ping back masters with the offset processed so far.
 *
 * So synchronous replication adds a new WAIT command in the form:
 *
 *   WAIT <num_replicas> <milliseconds_timeout>
 *
 * That returns the number of replicas that processed the query when
 * we finally have at least num_replicas, or when the timeout was
 * reached.
 *
 * The command is implemented in this way:
 *
 * - Every time a client processes a command, we remember the replication
 *   offset after sending that command to the slaves.
 * - When WAIT is called, we ask slaves to send an acknowledgement ASAP.
 *   The client is blocked at the same time (see blocked.c).
 * - Once we receive enough ACKs for a given offset or when the timeout
 *   is reached, the WAIT command is unblocked and the reply sent to the
 *   client.
 */

/* This just set a flag so that we broadcast a REPLCONF GETACK command
 * to all the slaves in the beforeSleep() function. Note that this way
 * we "group" all the clients that want to wait for synchronouns replication
 * in a given event loop iteration, and send a single GETACK for them all. */
void replicationRequestAckFromSlaves(void) {
    server.get_ack_from_slaves = 1;
}

/* Return the number of slaves that already acknowledged the specified
 * replication offset. */
int replicationCountAcksByOffset(long long offset) {
    listIter li;
    listNode *ln;
    int count = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        if (slave->replstate != REDIS_REPL_ONLINE) continue;
        if (slave->repl_ack_off >= offset) count++;
    }
    return count;
}

/* WAIT for N replicas to acknowledge the processing of our latest
 * write command (and all the previous commands). */
void waitCommand(redisClient *c) {
    mstime_t timeout;
    long numreplicas, ackreplicas;
    long long offset = c->woff;

    /* Argument parsing. */
    if (getLongFromObjectOrReply(c,c->argv[1],&numreplicas,NULL) != REDIS_OK)
        return;
    if (getTimeoutFromObjectOrReply(c,c->argv[2],&timeout,UNIT_MILLISECONDS)
        != REDIS_OK) return;

    /* First try without blocking at all. */
    ackreplicas = replicationCountAcksByOffset(c->woff);
    if (ackreplicas >= numreplicas || c->flags & REDIS_MULTI) {
        addReplyLongLong(c,ackreplicas);
        return;
    }

    /* Otherwise block the client and put it into our list of clients
     * waiting for ack from slaves. */
    c->bpop.timeout = timeout;
    c->bpop.reploffset = offset;
    c->bpop.numreplicas = numreplicas;
    listAddNodeTail(server.clients_waiting_acks,c);
    blockClient(c,REDIS_BLOCKED_WAIT);

    /* Make sure that the server will send an ACK request to all the slaves
     * before returning to the event loop. */
    replicationRequestAckFromSlaves();
}

/* This is called by unblockClient() to perform the blocking op type
 * specific cleanup. We just remove the client from the list of clients
 * waiting for replica acks. Never call it directly, call unblockClient()
 * instead. */
void unblockClientWaitingReplicas(redisClient *c) {
    listNode *ln = listSearchKey(server.clients_waiting_acks,c);
    redisAssert(ln != NULL);
    listDelNode(server.clients_waiting_acks,ln);
}

/* Check if there are clients blocked in WAIT that can be unblocked since
 * we received enough ACKs from slaves. */
void processClientsWaitingReplicas(void) {
    long long last_offset = 0;
    int last_numreplicas = 0;

    listIter li;
    listNode *ln;

    listRewind(server.clients_waiting_acks,&li);
    while((ln = listNext(&li))) {
        redisClient *c = ln->value;

        /* Every time we find a client that is satisfied for a given
         * offset and number of replicas, we remember it so the next client
         * may be unblocked without calling replicationCountAcksByOffset()
         * if the requested offset / replicas were equal or less. */
        if (last_offset && last_offset > c->bpop.reploffset &&
                           last_numreplicas > c->bpop.numreplicas)
        {
            unblockClient(c);
            addReplyLongLong(c,last_numreplicas);
        } else {
            int numreplicas = replicationCountAcksByOffset(c->bpop.reploffset);

            if (numreplicas >= c->bpop.numreplicas) {
                last_offset = c->bpop.reploffset;
                last_numreplicas = numreplicas;
                unblockClient(c);
                addReplyLongLong(c,numreplicas);
            }
        }
    }
}

/* Return the slave replication offset for this instance, that is
 * the offset for which we already processed the master replication stream. */
long long replicationGetSlaveOffset(void) {
    long long offset = 0;

    if (server.masterhost != NULL) {
        if (server.master) {
            offset = server.master->reploff;
        } else if (server.cached_master) {
            offset = server.cached_master->reploff;
        }
    }
    /* offset may be -1 when the master does not support it at all, however
     * this function is designed to return an offset that can express the
     * amount of data processed by the master, so we return a positive
     * integer. */
    if (offset < 0) offset = 0;
    return offset;
}

/* --------------------------- REPLICATION CRON  ---------------------------- */

/* Replication cron funciton, called 1 time per second. */
// 复制 cron 函数，每秒调用一次，由 serverCron() 进行周期性调度
// 最开头是状态机转换
void replicationCron(void) {

    /* 状态机处理 */
    // 状态机的转换处理之所以放在这里，不仅仅是因为这里的状态转换不跟具体的 redisClient 耦合
    // 更是为了能够自动进行重试！因为这可是一个 cron 函数！但某一次 handle 失败了之后，只要回退状态就好了，
    // 不用手动进行重试，晚一点就可以在这里根据状态进行重试！
    // 而且能够放在这里的状态，理应是临时状态，而不应该是能够长时间保持稳定的状态
    /* Non blocking connection timeout? */
    // 尝试连接到 master ，但超时
    if (server.masterhost &&
        (server.repl_state == REDIS_REPL_CONNECTING ||
         server.repl_state == REDIS_REPL_RECEIVE_PONG) &&
        (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        redisLog(REDIS_WARNING,"Timeout connecting to the MASTER...");
        // 取消连接
        // TODO:(DONE) 不重试吗？那怎么异步通知使用者呢？
        // 会不停重试的，就在下面 if (server.repl_state == REDIS_REPL_CONNECT)
        undoConnectWithMaster();
    }

    /* Bulk transfer I/O timeout? */
    // RDB 文件的传送已超时？
    if (server.masterhost && server.repl_state == REDIS_REPL_TRANSFER &&
        (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        redisLog(REDIS_WARNING,"Timeout receiving bulk data from MASTER... If the problem persists try to set the 'repl-timeout' parameter in redis.conf to a larger value.");
        // 停止传送，并删除临时文件
        replicationAbortSyncTransfer();
    }

    /* Timed out master when we are an already connected slave? */
    //  slave 发现：曾经连接上的 master ，现在超时不回应了（master 短暂失联了）
    if (server.masterhost && server.repl_state == REDIS_REPL_CONNECTED &&
        (time(NULL)-server.master->lastinteraction) > server.repl_timeout)
    {
        redisLog(REDIS_WARNING,"MASTER timeout: no data nor PING received...");
        // 释放 master 
        freeClient(server.master);
    }

    /* Check if we should connect to a MASTER */
    // （这可是定期重试）尝试（异步）连接 master 
    // 两种情况：1. 调用 slaveof 之后，slave 主动设置完相应的状态，然后开始连接 master
    //           2. slave 跟 master 失联了，slave 把原本的 master cache 起来之后，再次尝试连接 master
    if (server.repl_state == REDIS_REPL_CONNECT) {
        redisLog(REDIS_NOTICE,"Connecting to MASTER %s:%d",
            server.masterhost, server.masterport);
        if (connectWithMaster() == REDIS_OK) {
            redisLog(REDIS_NOTICE,"MASTER <-> SLAVE sync started");
        }
    }

    /* Send ACK to master from time to time.
     * Note that we do not send periodic acks to masters that don't
     * support PSYNC and replication offsets. */
    // 定期向 master 发送 ACK 命令
    // 不过如果 master 带有 REDIS_PRE_PSYNC 的话就不发送
    // 因为带有该标识的版本为 < 2.8 的版本，这些版本不支持 ACK 命令
    // server.masterhost 说明当前 redis-server 是 slave；
    // server.master 是 redisClient，不为 NULL 则说明已经成功连接了
    if (server.masterhost && server.master &&
        !(server.master->flags & REDIS_PRE_PSYNC))
        replicationSendAck();
    
    /* master <---> slave 的心跳 PING\PONG */
    /* If we have attached slaves, PING them from time to time.
     *
     * 如果服务器有 slave ，定时向它们发送 PING 。
     *
     * So slaves can implement an explicit timeout to masters, and will
     * be able to detect a link disconnection even if the TCP connection
     * will not actually go down. 
     *
     * 这样 slave 就可以实现显式的 master 超时判断机制，
     * 即使 TCP 连接未断开也是如此。
     */
    if (!(server.cronloops % (server.repl_ping_slave_period * server.hz))) {
        listIter li;
        listNode *ln;
        robj *ping_argv[1];

        /* First, send PING */
        // 向所有已连接 slave （状态为 ONLINE）发送 PING
        ping_argv[0] = createStringObject("PING",4);
        replicationFeedSlaves(server.slaves, server.slaveseldb, ping_argv, 1);
        decrRefCount(ping_argv[0]);

        /* Second, send a newline to all the slaves in pre-synchronization
         * stage, that is, slaves waiting for the master to create the RDB file.
         *
         * 向那些正在等待 RDB 文件的 slave （状态为 BGSAVE_START 或 BGSAVE_END）
         * 发送 "\n"
         *
         * The newline will be ignored by the slave but will refresh the
         * last-io timer preventing a timeout. 
         *
         * 这个 "\n" 会被 slave 忽略，
         * 它的作用就是用来防止 master 因为长期不发送信息而被 slave 误判为超时
         */
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;

            if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START ||
                slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) {
                if (write(slave->fd, "\n", 1) == -1) {
                    /* Don't worry, it's just a ping. */
                }
            }
        }
    }

    /* Disconnect timedout slaves. */
    // 断开超时 slave 
    if (listLength(server.slaves)) {
        listIter li;
        listNode *ln;

        // 遍历所有 slave 
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;

            // 略过未 ONLINE 的 slave 
            if (slave->replstate != REDIS_REPL_ONLINE) continue;

            // 不检查旧版的 slave 
            if (slave->flags & REDIS_PRE_PSYNC) continue;

            // 释放超时 slave 
            if ((server.unixtime - slave->repl_ack_time) > server.repl_timeout)
            {
                char ip[REDIS_IP_STR_LEN];
                int port;

                if (anetPeerToString(slave->fd,ip,sizeof(ip),&port) != -1) {
                    redisLog(REDIS_WARNING,
                        "Disconnecting timedout slave: %s:%d",
                        ip, slave->slave_listening_port);
                }
                
                // 释放
                freeClient(slave);
            }
        }
    }

    /* If we have no attached slaves and there is a replication backlog
     * using memory, free it after some (configured) time. */
    // 在没有任何 slave 的 N 秒之后，释放 backlog
    if (listLength(server.slaves) == 0 && server.repl_backlog_time_limit &&
        server.repl_backlog)
    {
        time_t idle = server.unixtime - server.repl_no_slaves_since;

        if (idle > server.repl_backlog_time_limit) {
            // 释放
            freeReplicationBacklog();
            redisLog(REDIS_NOTICE,
                "Replication backlog freed after %d seconds "
                "without connected slaves.",
                (int) server.repl_backlog_time_limit);
        }
    }

    /* If AOF is disabled and we no longer have attached slaves, we can
     * free our Replication Script Cache as there is no need to propagate
     * EVALSHA at all. */
    // 在没有任何 slave ，AOF 关闭的情况下，清空 script 缓存
    // 因为已经没有传播 EVALSHA 的必要了
    if (listLength(server.slaves) == 0 &&
        server.aof_state == REDIS_AOF_OFF &&
        listLength(server.repl_scriptcache_fifo) != 0)
    {
        replicationScriptCacheFlush();
    }

    /* Refresh the number of slaves with lag <= min-slaves-max-lag. */
    // 更新符合给定延迟值的 slave 的数量
    refreshGoodSlavesCount();
}
