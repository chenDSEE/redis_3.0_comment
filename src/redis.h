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

#ifndef __REDIS_H
#define __REDIS_H

#include "fmacros.h"
#include "config.h"

#if defined(__sun)
#include "solarisfixes.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <syslog.h>
#include <netinet/in.h>
#include <lua.h>
#include <signal.h>

#include "ae.h"      /* Event driven programming library */
#include "sds.h"     /* Dynamic safe strings */
#include "dict.h"    /* Hash tables */
#include "adlist.h"  /* Linked lists */
#include "zmalloc.h" /* total memory usage aware version of malloc/free */
#include "anet.h"    /* Networking the easy way */
#include "ziplist.h" /* Compact list data structure */
#include "intset.h"  /* Compact integer set structure */
#include "version.h" /* Version macro */
#include "util.h"    /* Misc functions useful in many places */

/* Error codes */
#define REDIS_OK                0
#define REDIS_ERR               -1

/* Static server configuration */
/* 默认的服务器配置值 */
#define REDIS_DEFAULT_HZ        10      /* Time interrupt calls/sec. */
#define REDIS_MIN_HZ            1
#define REDIS_MAX_HZ            500 
#define REDIS_SERVERPORT        6379    /* TCP port */
#define REDIS_TCP_BACKLOG       511     /* TCP listen backlog */
#define REDIS_MAXIDLETIME       0       /* default client timeout: infinite */
#define REDIS_DEFAULT_DBNUM     16
#define REDIS_CONFIGLINE_MAX    1024
#define REDIS_DBCRON_DBS_PER_CALL 16
#define REDIS_MAX_WRITE_PER_EVENT (1024*64)
#define REDIS_SHARED_SELECT_CMDS 10
#define REDIS_SHARED_INTEGERS 10000
#define REDIS_SHARED_BULKHDR_LEN 32
#define REDIS_MAX_LOGMSG_LEN    1024 /* Default maximum length of syslog messages */
#define REDIS_AOF_REWRITE_PERC  100
#define REDIS_AOF_REWRITE_MIN_SIZE (64*1024*1024)
#define REDIS_AOF_REWRITE_ITEMS_PER_CMD 64
#define REDIS_SLOWLOG_LOG_SLOWER_THAN 10000
#define REDIS_SLOWLOG_MAX_LEN 128
#define REDIS_MAX_CLIENTS 10000
#define REDIS_AUTHPASS_MAX_LEN 512
#define REDIS_DEFAULT_SLAVE_PRIORITY 100
#define REDIS_REPL_TIMEOUT 60
#define REDIS_REPL_PING_SLAVE_PERIOD 10
#define REDIS_RUN_ID_SIZE 40
#define REDIS_OPS_SEC_SAMPLES 16
#define REDIS_DEFAULT_REPL_BACKLOG_SIZE (1024*1024)    /* 1mb */
#define REDIS_DEFAULT_REPL_BACKLOG_TIME_LIMIT (60*60)  /* 1 hour */
#define REDIS_REPL_BACKLOG_MIN_SIZE (1024*16)          /* 16k */
#define REDIS_BGSAVE_RETRY_DELAY 5 /* Wait a few secs before trying again. */
#define REDIS_DEFAULT_PID_FILE "/var/run/redis.pid"
#define REDIS_DEFAULT_SYSLOG_IDENT "redis"
#define REDIS_DEFAULT_CLUSTER_CONFIG_FILE "nodes.conf"
#define REDIS_DEFAULT_DAEMONIZE 0
#define REDIS_DEFAULT_UNIX_SOCKET_PERM 0
#define REDIS_DEFAULT_TCP_KEEPALIVE 0
#define REDIS_DEFAULT_LOGFILE ""
#define REDIS_DEFAULT_SYSLOG_ENABLED 0
#define REDIS_DEFAULT_STOP_WRITES_ON_BGSAVE_ERROR 1
#define REDIS_DEFAULT_RDB_COMPRESSION 1
#define REDIS_DEFAULT_RDB_CHECKSUM 1
#define REDIS_DEFAULT_RDB_FILENAME "dump.rdb"
#define REDIS_DEFAULT_SLAVE_SERVE_STALE_DATA 1
#define REDIS_DEFAULT_SLAVE_READ_ONLY 1
#define REDIS_DEFAULT_REPL_DISABLE_TCP_NODELAY 0
#define REDIS_DEFAULT_MAXMEMORY 0
#define REDIS_DEFAULT_MAXMEMORY_SAMPLES 5
#define REDIS_DEFAULT_AOF_FILENAME "appendonly.aof"
#define REDIS_DEFAULT_AOF_NO_FSYNC_ON_REWRITE 0
#define REDIS_DEFAULT_ACTIVE_REHASHING 1
#define REDIS_DEFAULT_AOF_REWRITE_INCREMENTAL_FSYNC 1
#define REDIS_DEFAULT_MIN_SLAVES_TO_WRITE 0
#define REDIS_DEFAULT_MIN_SLAVES_MAX_LAG 10
#define REDIS_IP_STR_LEN INET6_ADDRSTRLEN
#define REDIS_PEER_ID_LEN (REDIS_IP_STR_LEN+32) /* Must be enough for ip:port */
#define REDIS_BINDADDR_MAX 16
// It also reserves a number of file. why 32 ? That is the number of file descriptors Redis reserves for internal uses
/* REDIS_MIN_RESERVED_FDS for extra operations of persistence, listening sockets, log files and so forth. */
#define REDIS_MIN_RESERVED_FDS 32

#define ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP 20 /* Loopkups per loop. */
#define ACTIVE_EXPIRE_CYCLE_FAST_DURATION 1000 /* Microseconds */
#define ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 25 /* CPU max % for keys collection */
#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1

/* Protocol and I/O related defines */
#define REDIS_MAX_QUERYBUF_LEN  (1024*1024*1024) /* 1GB max query buffer. */
#define REDIS_IOBUF_LEN         (1024*16)  /* Generic I/O buffer size */
#define REDIS_REPLY_CHUNK_BYTES (16*1024) /* 16k output buffer */
#define REDIS_INLINE_MAX_SIZE   (1024*64) /* Max size of inline reads */
#define REDIS_MBULK_BIG_ARG     (1024*32)
#define REDIS_LONGSTR_SIZE      21          /* Bytes needed for long -> str */
// 指示 AOF 程序每累积这个量的写入数据
// 就执行一次显式的 fsync
#define REDIS_AOF_AUTOSYNC_BYTES (1024*1024*32) /* fdatasync every 32MB */
/* When configuring the Redis eventloop, we setup it so that the total number
 * of file descriptors we can handle are server.maxclients + RESERVED_FDS + FDSET_INCR
 * that is our safety margin. */
#define REDIS_EVENTLOOP_FDSET_INCR (REDIS_MIN_RESERVED_FDS+96)

/* Hash table parameters */
#define REDIS_HT_MINFILL        10      /* Minimal hash table fill 10% */

/* Command flags. Please check the command table defined in the redis.c file
 * for more information about the meaning of every flag. */
// 命令标志
#define REDIS_CMD_WRITE 1                   /* "w" flag */
#define REDIS_CMD_READONLY 2                /* "r" flag */
#define REDIS_CMD_DENYOOM 4                 /* "m" flag */
#define REDIS_CMD_NOT_USED_1 8              /* no longer used flag */
#define REDIS_CMD_ADMIN 16                  /* "a" flag */
#define REDIS_CMD_PUBSUB 32                 /* "p" flag */
#define REDIS_CMD_NOSCRIPT  64              /* "s" flag */
#define REDIS_CMD_RANDOM 128                /* "R" flag */
#define REDIS_CMD_SORT_FOR_SCRIPT 256       /* "S" flag */
#define REDIS_CMD_LOADING 512               /* "l" flag */
#define REDIS_CMD_STALE 1024                /* "t" flag */
#define REDIS_CMD_SKIP_MONITOR 2048         /* "M" flag */
#define REDIS_CMD_ASKING 4096               /* "k" flag */

/**
 * 编码层(REDIS_ENCODING_EMBSTR...) VS 对象抽象层(REDIS_STRING...)
 * 1) 引用计数问题：
 * 通过 grep -rn RefCount，你会发现：引用计数的加减，只会发生在 对象抽象层，也就是 t_xxx.c\db.c\object.c 之类的文件里面；
 * 而不会在 dict.c\ziplist.c 之类的 编码层 文件里面；
 * 这是因为编码层并不知晓 obj 这个概念，也就没有不存在什么引用计数了；
 * 编码本身是可以通过引用的方式进行共用的，但是编码层并没有必要知晓自己是否正在被共用，
 * 哪怕是 struct dict 这样的管理结构也是没有任何必要知道的，因为这一层仍然底层数据管理的职责中；
 * 而引用计数时更高的层面，就比如：引用同一个 ziplist、embedded-string，引用计数这一点本身就是大家共用的能力，也没有必要再每一个不同的编码方式中实现
 * 这也就是为什么，在 HSET CMD 里面，发生 value replace 的时候，并不会在 dict.c 里面直接 decsRefCount，因为 dict.c 这一层并不知晓 robj 的存在，
 * 仅仅知道怎么编码，怎么解码，怎么管理、组织 dict 里面的每一个 node
*/

/* Object types */
// 对象类型
// zset 是一种很特别的类型，因为 zset 同时使用了两种底层数据结构进行构建（skip-list + ）
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_ZSET 3
#define REDIS_HASH 4

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
// 对象编码
// 整体压缩编码方式：REDIS_ENCODING_ZIPLIST，REDIS_ENCODING_INTSET，采用独立的编解码函数 zipTryEncoding(), _intsetSet()
// 可进行独立压缩编码的 obj：REDIS_ENCODING_RAW，REDIS_ENCODING_INT，REDIS_ENCODING_EMBSTR，采用统一的 tryObjectEncoding() 进行转发
#define REDIS_ENCODING_RAW 0        /* Raw representation, 初始化默认类型，ptr 有可能指向的是 sds（可能是 string，也可能是一个很长的数值转成的 string） */
#define REDIS_ENCODING_INT 1        /* Encoded as integer, data 部分直接占用 ptr 的内存（8 byte） */
#define REDIS_ENCODING_HT 2         /* Encoded as hash table */
#define REDIS_ENCODING_ZIPMAP 3     /* Encoded as zipmap */
#define REDIS_ENCODING_LINKEDLIST 4 /* Encoded as regular linked list */

// 采用 REDIS_ENCODING_ZIPLIST 方式编码的 REDIS_HT，REDIS_ZSET，会将 field\score、value\member 顺序的 push-tail 进 ziplist 中
#define REDIS_ENCODING_ZIPLIST 5    /* Encoded as ziplist */
#define REDIS_ENCODING_INTSET 6     /* Encoded as intset */
#define REDIS_ENCODING_SKIPLIST 7   /* Encoded as skiplist */

// REDIS_ENCODING_EMBSTR: in the same chunk of memory to save space and cache misses.
#define REDIS_ENCODING_EMBSTR 8     /* Embedded sds string encoding，const 的紧凑型，最大 39 个 char（为了充分利用 malloc 的分配） */

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|000000 => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|000000 00000000 =>  01, the len is 14 byes, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => if it's 10, a full 32 bit len will follow
 * 11|000000 this means: specially encoded object will follow. The six bits
 *           number specify the kind of object that follows.
 *           See the REDIS_RDB_ENC_* defines.
 *
 * Lengths up to 63 are stored using a single byte, most DB keys, and may
 * values, will fit inside. */
#define REDIS_RDB_6BITLEN 0
#define REDIS_RDB_14BITLEN 1
#define REDIS_RDB_32BITLEN 2
#define REDIS_RDB_ENCVAL 3
#define REDIS_RDB_LENERR UINT_MAX

/* When a length of a string object stored on disk has the first two bits
 * set, the remaining two bits specify a special encoding for the object
 * accordingly to the following defines: */
#define REDIS_RDB_ENC_INT8 0        /* 8 bit signed integer */
#define REDIS_RDB_ENC_INT16 1       /* 16 bit signed integer */
#define REDIS_RDB_ENC_INT32 2       /* 32 bit signed integer */
#define REDIS_RDB_ENC_LZF 3         /* string compressed with FASTLZ */

/* AOF states */
#define REDIS_AOF_OFF 0             /* AOF is off */
#define REDIS_AOF_ON 1              /* AOF is on */
#define REDIS_AOF_WAIT_REWRITE 2    /* AOF waits rewrite to start appending */

/* Client flags */
#define REDIS_SLAVE (1<<0)   /* This client is a slave server */
#define REDIS_MASTER (1<<1)  /* This client is a master server */
#define REDIS_MONITOR (1<<2) /* This client is a slave monitor, see MONITOR */
#define REDIS_MULTI (1<<3)   /* This client is in a MULTI context */
#define REDIS_BLOCKED (1<<4) /* The client is waiting in a blocking operation */
#define REDIS_DIRTY_CAS (1<<5) /* Watched keys modified. EXEC will fail. */
#define REDIS_CLOSE_AFTER_REPLY (1<<6) /* Close after writing entire reply. */
#define REDIS_UNBLOCKED (1<<7) /* This client was unblocked and is stored in
                                  server.unblocked_clients */
#define REDIS_LUA_CLIENT (1<<8) /* This is a non connected client used by Lua */
#define REDIS_ASKING (1<<9)     /* Client issued the ASKING command */
#define REDIS_CLOSE_ASAP (1<<10)/* Close this client ASAP */
#define REDIS_UNIX_SOCKET (1<<11) /* Client connected via Unix domain socket */
#define REDIS_DIRTY_EXEC (1<<12)  /* EXEC will fail for errors while queueing */
#define REDIS_MASTER_FORCE_REPLY (1<<13)  /* Queue replies even if is master */
#define REDIS_FORCE_AOF (1<<14)   /* Force AOF propagation of current cmd. */
#define REDIS_FORCE_REPL (1<<15)  /* Force replication of current cmd. */
#define REDIS_PRE_PSYNC (1<<16)   /* Instance don't understand PSYNC. */
#define REDIS_READONLY (1<<17)    /* Cluster client is in read-only state. */

/* Client block type (btype field in client structure)
 * if REDIS_BLOCKED flag is set. */
#define REDIS_BLOCKED_NONE 0    /* Not blocked, no REDIS_BLOCKED flag set. */
#define REDIS_BLOCKED_LIST 1    /* BLPOP & co. */
#define REDIS_BLOCKED_WAIT 2    /* WAIT for synchronous replication. */

/* Client request types */
#define REDIS_REQ_INLINE 1
#define REDIS_REQ_MULTIBULK 2

/* Client classes for client limits, currently used only for
 * the max-client-output-buffer limit implementation. */
/**
 * 1. REDIS_CLIENT_LIMIT_CLASS_NORMAL
 * have a default limit of 0, that means, no limit at all, because most normal
 * clients use blocking implementations sending a single command and waiting for
 * the reply to be completely read before sending the next command, so it is
 * always not desirable to close the connection in case of a normal client.（因此，对于普通的客户端，总是不希望关闭连接。）
 * 
 * 2. REDIS_CLIENT_LIMIT_CLASS_PUBSUB
 * have a default hard limit of 32 megabytes and a soft limit of 8 megabytes per 60 seconds.（TODO: 为什么？）
 * 
 * 3. REDIS_CLIENT_LIMIT_CLASS_SLAVE
 * have a default hard limit of 256 megabytes and a soft limit of 64 megabyte per 60 second.
 * 
 * 4. REDIS_CLIENT_LIMIT_NUM_CLASSES
 * 仅仅是作为一个末尾的计数值
*/
#define REDIS_CLIENT_LIMIT_CLASS_NORMAL 0
#define REDIS_CLIENT_LIMIT_CLASS_SLAVE 1
#define REDIS_CLIENT_LIMIT_CLASS_PUBSUB 2
#define REDIS_CLIENT_LIMIT_NUM_CLASSES 3

/* Slave replication state - from the point of view of the slave. */
#define REDIS_REPL_NONE 0 /* No active replication */
#define REDIS_REPL_CONNECT 1 /* Must connect to master（想要连接，但是还没有连接） */
// 增加一个 ping\pong 过程是为了确认 slave-master 之间确确实实能够传递数据，毕竟这个过程耗时可能很长的
#define REDIS_REPL_CONNECTING 2 /* Connecting to master（正在连接，握手请求已经发送了） */
#define REDIS_REPL_RECEIVE_PONG 3 /* Wait for PING reply（slave 已经发送 ping，正在等待 pong） */
#define REDIS_REPL_TRANSFER 4 /* slave 正在等待 Receiving .rdb from master */
#define REDIS_REPL_CONNECTED 5 /* Connected to master（成功连接） */

/* Slave replication state - from the point of view of the master.
 * In SEND_BULK and ONLINE state the slave receives new updates
 * in its output queue. In the WAIT_BGSAVE state instead the server is waiting
 * to start the next background saving in order to send updates to it. */
#define REDIS_REPL_WAIT_BGSAVE_START 6 /* We need to produce a new RDB file. */
#define REDIS_REPL_WAIT_BGSAVE_END 7 /* Waiting RDB file creation to finish. */
#define REDIS_REPL_SEND_BULK 8 /* Sending RDB file to slave. */
#define REDIS_REPL_ONLINE 9 /* RDB file transmitted, sending just updates. */

/* Synchronous read timeout - slave side */
#define REDIS_REPL_SYNCIO_TIMEOUT 5

/* List related stuff */
#define REDIS_HEAD 0
#define REDIS_TAIL 1

/* Sort operations */
#define REDIS_SORT_GET 0
#define REDIS_SORT_ASC 1
#define REDIS_SORT_DESC 2
#define REDIS_SORTKEY_MAX 1024

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_VERBOSE 1
#define REDIS_NOTICE 2
#define REDIS_WARNING 3
#define REDIS_LOG_RAW (1<<10) /* Modifier to log without timestamp */
#define REDIS_DEFAULT_VERBOSITY REDIS_NOTICE

/* Anti-warning macro... */
#define REDIS_NOTUSED(V) ((void) V)

#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^32 elements */
#define ZSKIPLIST_P 0.25      /* Skiplist P = 1/4 */

/* Append only defines */
/**
 * 所谓的 AOF policy 是指：以怎样的频率进行落盘保存
 * 1. always 也就是来一个 CMD, 就落盘保存一次，追求的是绝对的安全
 *    不能够容忍任何的延迟，所以是让主线程来进行同步 fsync() 落盘保存的
 * 2. every-second 是利用 buf，每秒钟落盘一次，保存过去一秒内的所有 CMD
 *    本身能够容忍一定的延迟。所以让子线程进行 fsync() 落盘保存
 * 3. no 不落盘
*/
// fsync() 会阻塞 write()，但是 fsync() 跟 write() 的调用都必然是原子完成的
#define AOF_FSYNC_NO 0
#define AOF_FSYNC_ALWAYS 1
#define AOF_FSYNC_EVERYSEC 2
#define REDIS_DEFAULT_AOF_FSYNC AOF_FSYNC_EVERYSEC

/* Zip structure related defaults */
#define REDIS_HASH_MAX_ZIPLIST_ENTRIES 512
#define REDIS_HASH_MAX_ZIPLIST_VALUE 64
#define REDIS_LIST_MAX_ZIPLIST_ENTRIES 512
#define REDIS_LIST_MAX_ZIPLIST_VALUE 64
#define REDIS_SET_MAX_INTSET_ENTRIES 512
#define REDIS_ZSET_MAX_ZIPLIST_ENTRIES 128
#define REDIS_ZSET_MAX_ZIPLIST_VALUE 64

/* HyperLogLog defines */
#define REDIS_DEFAULT_HLL_SPARSE_MAX_BYTES 3000

/* Sets operations codes */
#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1
#define REDIS_OP_INTER 2

/* Redis maxmemory strategies */
#define REDIS_MAXMEMORY_VOLATILE_LRU 0
#define REDIS_MAXMEMORY_VOLATILE_TTL 1
#define REDIS_MAXMEMORY_VOLATILE_RANDOM 2
#define REDIS_MAXMEMORY_ALLKEYS_LRU 3
#define REDIS_MAXMEMORY_ALLKEYS_RANDOM 4
#define REDIS_MAXMEMORY_NO_EVICTION 5
#define REDIS_DEFAULT_MAXMEMORY_POLICY REDIS_MAXMEMORY_NO_EVICTION

/* Scripting */
#define REDIS_LUA_TIME_LIMIT 5000 /* milliseconds */

/* Units */
#define UNIT_SECONDS 0
#define UNIT_MILLISECONDS 1

/* SHUTDOWN flags */
#define REDIS_SHUTDOWN_SAVE 1       /* Force SAVE on SHUTDOWN even if no save
                                       points are configured. */
#define REDIS_SHUTDOWN_NOSAVE 2     /* Don't SAVE on SHUTDOWN. */

/* Command call flags, see call() function */
#define REDIS_CALL_NONE 0
#define REDIS_CALL_SLOWLOG 1
#define REDIS_CALL_STATS 2
#define REDIS_CALL_PROPAGATE 4
#define REDIS_CALL_FULL (REDIS_CALL_SLOWLOG | REDIS_CALL_STATS | REDIS_CALL_PROPAGATE)

/* Command propagation flags, see propagate() function */
#define REDIS_PROPAGATE_NONE 0
#define REDIS_PROPAGATE_AOF 1
#define REDIS_PROPAGATE_REPL 2

/* Keyspace changes notification classes. Every class is associated with a
 * character for configuration purposes. */
#define REDIS_NOTIFY_KEYSPACE (1<<0)    /* K */
#define REDIS_NOTIFY_KEYEVENT (1<<1)    /* E */
#define REDIS_NOTIFY_GENERIC (1<<2)     /* g */
#define REDIS_NOTIFY_STRING (1<<3)      /* $ */
#define REDIS_NOTIFY_LIST (1<<4)        /* l */
#define REDIS_NOTIFY_SET (1<<5)         /* s */
#define REDIS_NOTIFY_HASH (1<<6)        /* h */
#define REDIS_NOTIFY_ZSET (1<<7)        /* z */
#define REDIS_NOTIFY_EXPIRED (1<<8)     /* x */
#define REDIS_NOTIFY_EVICTED (1<<9)     /* e */
#define REDIS_NOTIFY_ALL (REDIS_NOTIFY_GENERIC | REDIS_NOTIFY_STRING | REDIS_NOTIFY_LIST | REDIS_NOTIFY_SET | REDIS_NOTIFY_HASH | REDIS_NOTIFY_ZSET | REDIS_NOTIFY_EXPIRED | REDIS_NOTIFY_EVICTED)      /* A */

/* Using the following macro you can run code inside serverCron() with the
 * specified period, specified in milliseconds.
 * The actual resolution depends on server.hz. */
// 实际上这个周期性执行并不是很精确的，因为阻塞操作可能会带来延时
#define run_with_period(_ms_) if ((_ms_ <= 1000/server.hz) || !(server.cronloops%((_ms_)/(1000/server.hz))))

/* We can print the stacktrace, so our assert is defined this way: */
#define redisAssertWithInfo(_c,_o,_e) ((_e)?(void)0 : (_redisAssertWithInfo(_c,_o,#_e,__FILE__,__LINE__),_exit(1)))
#define redisAssert(_e) ((_e)?(void)0 : (_redisAssert(#_e,__FILE__,__LINE__),_exit(1)))
#define redisPanic(_e) _redisPanic(#_e,__FILE__,__LINE__),_exit(1)

/*-----------------------------------------------------------------------------
 * Data types
 *----------------------------------------------------------------------------*/

typedef long long mstime_t; /* millisecond time type. */

/* A redis object, that is a type able to hold a string / list / set */

/* The actual Redis Object */
/*
 * Redis 对象
 * 
 * |<---             4 byte            --->|<- 4 byte ->|<- 8 byte ->|
 * |<- 4 bit ->|<--  4 bit -->|<- 24 bit ->|
 * +-----------+--------------+------------+------------+------------+
 * |  type:4   |  encoding:4  |   lru:24   |  refcount  |     ptr    |
 * +-----------+--------------+------------+------------+------------+
 *                    |                                      | point to
 *                    |                                      |
 *                    |                         +-------------------------+
 *                    +------- identify ---->   |  low-level data struct  |
 *                                              +-------------------------+
 */
#define REDIS_LRU_BITS 24
#define REDIS_LRU_CLOCK_MAX ((1<<REDIS_LRU_BITS)-1) /* Max value of obj->lru */
#define REDIS_LRU_CLOCK_RESOLUTION 1000 /* LRU clock resolution in ms */
// NOTE: robj 这是一个很可怕的 struct，极度抽象化，复用度极高
//       robj 实际上就像是一个父类，基于 robj + type + encoding 实现了很多 OO 的事情：
//       多态 操作（比如：setTypeAdd()，通过 type 来确认继承关系，通过 encoding 来确认派生类的种类，并转发到对应的 override 函数），
//       模板方法（比如：struct dictType 填写 callback，然后通过 api dictSetKey() 来调用，跟多态一样，利用 encoding 决定怎么转发调用），
//       引用计数（比如：incrRefCount()），
//       公共接口（所有的顶层抽象，都是一个个 robj），
//       等等......
typedef struct redisObject {

    // 类型(unsigned, 4 byte; but here is 4 bits)
    // 0--4, 4 bits 足矣
    // 对应的是 redis 的五个数据结构：string\list\hash\sorted set(zset)\set
    unsigned type:4;

    // 编码
    // 每一个数据结构的底层采用了什么底层数据结构来进行保存
    // sds\adlist\ziplist\dict\skip-list\intset ;
    unsigned encoding:4;

    // 对象最后一次被访问的时间
    // TODO:(DONE) 既然 lru 是存在于 robj 的，那么当 hash-dict 需要进行 key 的比较的时候，
    //       CMD 来的 lru 跟 redis-dict 里面的 robj lru 自然会是不一样的，这时候怎么比较？
    //       比较的结果为什么还能一样？
    // _dictKeyIndex() dictCompareKeys(), 要是有设置 keyCompare() 的回调函数，
    // 那你就在自己版本的回调函数里面单独将 key 从 void* 里转换为 robj* 再取出 key 来进行单独的比较。
    // 没有的话，那就直接比较两个 robj 的地址
    unsigned lru:REDIS_LRU_BITS; /* lru time (relative to server.lruclock) */

    // 引用计数
    int refcount;

    // 指向实际值的指针
    void *ptr;

} robj;

/* Macro used to obtain the current LRU clock.
 * If the current resolution is lower than the frequency we refresh the
 * LRU clock (as it should be in production servers) we return the
 * precomputed value, otherwise we need to resort to a function call. */
#define LRU_CLOCK() ((1000/server.hz <= REDIS_LRU_CLOCK_RESOLUTION) ? server.lruclock : getLRUClock())

/* Macro used to initialize a Redis object allocated on the stack.
 * Note that this macro is taken near the structure definition to make sure
 * we'll update it when the structure is changed, to avoid bugs like
 * bug #85 introduced exactly in this way. */
#define initStaticStringObject(_var,_ptr) do { \
    _var.refcount = 1; \
    _var.type = REDIS_STRING; \
    _var.encoding = REDIS_ENCODING_RAW; \
    _var.ptr = _ptr; \
} while(0);

/* To improve the quality of the LRU approximation we take a set of keys
 * that are good candidate for eviction across freeMemoryIfNeeded() calls.
 *
 * Entries inside the eviciton pool are taken ordered by idle time, putting
 * greater idle times to the right (ascending order).
 *
 * Empty entries have the key pointer set to NULL. */
#define REDIS_EVICTION_POOL_SIZE 16
struct evictionPoolEntry {
    unsigned long long idle;    /* Object idle time. */
    sds key;                    /* Key name. */
};

/* Redis database representation. There are multiple databases identified
 * by integers from 0 (the default database) up to the max configured
 * database. The database number is the 'id' field in the structure. */
// NOTE: redis 的过期策略都是针对一个 key 来进行设置的，一旦这个 key 过期，这个 key 所关联的 value（ZSET、SET、HASH 整体就会被删除）
typedef struct redisDb {

    // 数据库键空间，保存着数据库中的所有键值对
    // key 是用户日后 CMD 输入的 key，value 则是底层组织方式（由用户的 set\zadd\hset 之类的决定采用什么数据组织结构）
    // key 将会是一个没有预留任何空间的 sds（统一的，因为 key 的经常查询，就没有必要压缩解压了），而不是一个 robj
    dict *dict;                 /* The keyspace for this DB */

    // 键的过期时间，字典的键为键，字典的值为过期事件 UNIX 时间戳
    // 参考 dbRandomKey()
    // 这个 dict 负责记录这个 DB 里面所有设置了 expire-timeout 的 key；
    // value 则是填写了这个 key 相应的过期时间
    dict *expires;              /* Timeout of keys with a timeout set */

    // 正处于阻塞状态的键
    // key -> value <===> string -> list
    // 具体被阻塞的 key -> 阻塞在这个 key 上面的 client，以及阻塞的顺序
    dict *blocking_keys;        /* Keys with clients waiting for data (BLPOP) */

    // 可以解除阻塞的键
    // 用来去重
    dict *ready_keys;           /* Blocked keys that received a PUSH */

    // 正在被 WATCH 命令监视的键(TODO: 结合 multi.c 来看)
    dict *watched_keys;         /* WATCHED keys for MULTI/EXEC CAS */

    struct evictionPoolEntry *eviction_pool;    /* Eviction pool of keys */

    // 数据库号码
    int id;                     /* Database ID */

    // 数据库的键的平均 TTL ，统计信息
    long long avg_ttl;          /* Average TTL, just for stats */

} redisDb;

/* Client MULTI/EXEC state */

/*
 * 事务命令
 */
typedef struct multiCmd {

    // 参数
    robj **argv;

    // 参数数量
    int argc;

    // 命令指针
    struct redisCommand *cmd;

} multiCmd;

/*
 * 事务状态
 */
typedef struct multiState {

    // 事务队列，FIFO 顺序
    multiCmd *commands;     /* Array of MULTI commands */

    // 已入队命令计数
    int count;              /* Total number of MULTI commands */
    int minreplicas;        /* MINREPLICAS for synchronous replication */
    time_t minreplicas_timeout; /* MINREPLICAS timeout as unixtime. */
} multiState;

/* This structure holds the blocking operation state for a client.
 * The fields used depend on client->btype. */
// 阻塞状态
typedef struct blockingState {

    /* Generic fields. */
    // 阻塞时限
    mstime_t timeout;       /* Blocking operation timeout. If UNIX current time
                             * is > timeout then the operation timed out. */

    /* REDIS_BLOCK_LIST */
    // 造成阻塞的键
    dict *keys;             /* The keys we are waiting to terminate a blocking
                             * operation such as BLPOP. Otherwise NULL. */
    // 在被阻塞的键有新元素进入时，需要将这些新元素添加到哪里的目标键
    // 用于 BRPOPLPUSH 命令
    robj *target;           /* The key that should receive the element,
                             * for BRPOPLPUSH. */

    /* REDIS_BLOCK_WAIT */
    // 等待 ACK 的复制节点数量
    int numreplicas;        /* Number of replicas we are waiting for ACK. */
    // 复制偏移量
    long long reploffset;   /* Replication offset to reach. */

} blockingState;

/* The following structure represents a node in the server.ready_keys list,
 * where we accumulate all the keys that had clients blocked with a blocking
 * operation such as B[LR]POP, but received new data in the context of the
 * last executed command.
 *
 * After the execution of every command or script, we run this list to check
 * if as a result we should serve data to clients blocked, unblocking them.
 * Note that server.ready_keys will not have duplicates as there dictionary
 * also called ready_keys in every structure representing a Redis database,
 * where we make sure to remember if a given key was already added in the
 * server.ready_keys list. */
// 记录解除了客户端的阻塞状态的键，以及键所在的数据库。
typedef struct readyList {
    redisDb *db;
    robj *key;
} readyList;

/* With multiplexing we need to take per-client state.
 * Clients are taken in a liked list.
 *
 * 因为 I/O 复用的缘故，需要为每个客户端维持一个状态。
 *
 * 多个客户端状态被服务器用链表连接起来。
 */
typedef struct redisClient {

    // 套接字描述符（socket fd）
    int fd;

    // 当前正在使用的数据库
    redisDb *db;

    // 当前正在使用的数据库的 id （号码）
    int dictid;

    // 客户端的名字
    robj *name;             /* As set by CLIENT SETNAME */

//==============================================================================
// redis-server 读取 redis-cli 发送过来的数据
    // client 发过来的 req buf，将 read() 中的  RESP 协议内容，直接放到这个 buf 里面
    // (TODO:(DONE) 为什么是 sds？)，因为 RESP 协议的内容就是字符串，而 sds 刚好就能够处理二进制安全的字符串
    // 而且用 sds 来管理，还省了 buf 的长度等信息的管理，相应 api 的重新封装
    sds querybuf;

    // 查询缓冲区长度峰值
    size_t querybuf_peak;   /* Recent (100ms or more) peak of querybuf size */

    // 参数数量
    int argc;
    // 举例：set key string，argc = 3, argv[0].ptr = "set", argv[1].ptr = "key", argv[2].ptr = "string"
    // 参数对象数组, 之所以要用二维指针，是因为，最终的 robj 不确定有多少个
    // 而且 robj 并不是每一个都不一样的，有时候可以通过 share 的方式来复用的，所以没必要每次都为 CLI 中的内容，都创建一个 robj
    robj **argv;

    // 请求的类型：内联命令还是多条命令（基本上由 client 的种类就可以决定了）
    // 一个 CMD 用一次，用完后清零
    int reqtype;

    // 从 RESP 里面发现 *3xxxxxx 格式的内容，然后这个 multibulklen = 3；意味着，这个 multi-bulk 类型的内容
    // 由 3 个小命令构成，一个 CMD 用一次，用完后清零
    int multibulklen;       /* number of multi bulk arguments left to read */

    // 命令内容的长度（每一个小的命令，具体有多长）
    // $100, 就意味着这个小命令的长度是 100，一个 CMD 用一次，用完后 reset
    long bulklen;           /* length of bulk argument in multi bulk request */
//==============================================================================

    // 记录被客户端执行的命令
    struct redisCommand *cmd, *lastcmd;



    // 创建客户端的时间
    time_t ctime;           /* Client creation time */

    // 客户端最后一次和服务器互动的时间
    time_t lastinteraction; /* time of the last interaction, used for timeout */

    // 客户端的输出缓冲区超过软性限制的时间
    time_t obuf_soft_limit_reached_time;

    // 客户端状态标志
    int flags;              /* REDIS_SLAVE | REDIS_MONITOR | REDIS_MULTI ... */

    // 当 server.requirepass 不为 NULL 时
    // 代表认证的状态
    // 0 代表未认证， 1 代表已认证
    int authenticated;      /* when requirepass is non-NULL */

    // 复制状态(sync 时，master 使用的状态机)
    int replstate;          /* replication state if this is a slave */
    // master 上看：这个 fd 指向用来进行同步的那个 RDB 文件
    int repldbfd;           /* replication DB file descriptor */

    // master 上看：记录正在传递的 RDB 文件传递 offset，因为面对体积较大的 RDB 文件。
    // 是不可能一个 write 就完成了全部传送的；而传递 RDB 的工作是交由主进程来完成的，
    // 所以主进程传递 RDB 的过程也是一个多次、异步的过程。
    // 既然是异步，那必然是需要记录一下：这个 RDB 文件上一次传递到哪里，这便是 repldboff(repl_db_offset)
    off_t repldboff;        /* replication DB file offset */
    // slave 上看：master 传来的 RDB 文件的大小
    // master 上看：要向这个 slave 传递的 RDB 文件大小
    off_t repldbsize;       /* replication DB file size */
    // repldbsize 的 SDS RESP 形式，如："$%31\r\n"；在传送 RDB 前，先告知 slave，即将传送的 RDB 究竟有多大
    sds replpreamble;       /* replication DB preamble.(前缀，说明 RDB 文件的大小) */

    // 在 slave 上才会用，当前这个 client 就是 slave --> master 的 client
    // 这个 offset 就是 repl 到什么位置，这个是 master 的 buf offset
    long long reploff;      /* replication offset if this is our master */
    //  slave 最后一次发送 REPLCONF ACK 时的偏移量
    long long repl_ack_off; /* replication ack offset, if this is a slave */
    //  slave 最后一次发送 REPLCONF ACK 的时间
    long long repl_ack_time;/* replication ack time, if this is a slave */
    //  master 的 master run ID
    // 保存在客户端，用于执行部分重同步
    char replrunid[REDIS_RUN_ID_SIZE+1]; /* master run id if this is a master */
    //  slave 的监听端口号
    int slave_listening_port; /* As configured with: SLAVECONF listening-port */

    // 事务状态
    multiState mstate;      /* MULTI/EXEC state */

    // 阻塞类型
    int btype;              /* Type of blocking op if REDIS_BLOCKED. */
    // 阻塞状态
    blockingState bpop;     /* blocking state */

    // 最后被写入的全局复制偏移量
    long long woff;         /* Last write global replication offset. */

    // 被监视的键
    list *watched_keys;     /* Keys WATCHED for MULTI/EXEC CAS */
    sds peerid;             /* Cached peer ID. format: ip:port or [ipv6]:port */

//==============================================================================
    // TODO:(DONE) 这个 channel 的 dict 存放在 redisClient 干嘛？总不可能每一个 redisClient 的 pubsub 一更新就要全部更新吧？
    // redisClient->pubsub_channels 仅仅是作为一个 set 来进行去重使用
    // server->pubsub_channels 这里的才是真正作为 pubsub 查询用的 dict，key->channel, value->client-list;
    // 这样就能采用 O(1) 的方式来进行 pubsub 了
    /* pubsub 相关 */
    // 这个字典记录了本客户端所有订阅的频道
    // key --> channel name，value --> NULL
    // 也即是，一个频道的集合
    dict *pubsub_channels;  /* channels a client is interested in (SUBSCRIBE) */

    // 链表，包含多个 pubsubPattern 结构
    // 由于 patterns 方式订阅的 channel 并不会太多（需要使用者自己克制，所以直接采用 list 管理）
    // 再者，因为是 pattern 的关系，必然是要检查所有 pattern 是否匹配的，所以采用 list
    // 新 pubsubPattern 结构总是被添加到表尾
    list *pubsub_patterns;  /* patterns a client is interested in (SUBSCRIBE) */

//==============================================================================
    /* Response buffer */
    // 回复链表(可能要随时释放掉、更换 list，所以采用指针的方式，而不是拥有一个头节点或管理节点)
    // TODO:(DONE) 这个 list 里面装的是什么？为什么 _addReplyToBuffer() 中，if (listLength(c->reply) > 0) return REDIS_ERR;
    // Redis needs to handle a variable-length output buffer for every client
    // 一旦使用了 reply-list 就可以认为是 buf 已经满了，接下来之后利用 reply-list 来进一步容纳大量数据了
    list *reply;


    /* 通过 bufpos 跟 sentlen 来 handle buf 里面的 data 发送情况
     * bufpos  是指 buf 里面有多少数据，buf 用到了哪里；
     * sentlen 则是 buf 里面的数据哪些是已经被发送出去了的，发送到了什么程度
     * reply_bytes 是 reply-list 里面有一共有多少 byte 的 data 部分需要发送
     * 
     * 所以当 buf 里面的内容被清空之后，上面这两个计数器也是要被 reset 为 0 的
     * 通常是 sentlen + bufpos 或 sentlen + reply_bytes 配合使用
     */

    // 回复链表中对象的总大小(仅仅是所有 data 部分加起来，不包含 header 部分)
    unsigned long reply_bytes; /* Tot bytes of objects in reply list */

    // 已发送字节，处理 short write 用
    int sentlen;            /* Amount of bytes already sent in the current
                               buffer or object being sent. */


    // 回复偏移量(不是 address，而是类似于 index 的)，相当于记录 buf 用到了哪里
    int bufpos;
    // (output-buf)回复缓冲区
    // TODO:(DONE) 超出了这个 buff 该怎么办？
    // 1. 满了就使用 reply 这里 list 进一步拓展缓冲区
    // 2. reply-list 也装得太多了，那就只能够直接异步 close 掉这个 client，不然内存占用实在是过多了
    char buf[REDIS_REPLY_CHUNK_BYTES];

} redisClient;

// 服务器的保存条件（BGSAVE 自动执行的条件）
struct saveparam {

    // 多少秒之内
    time_t seconds;

    // 发生多少次修改
    int changes;

};

// 通过复用来减少内存碎片，以及减少操作耗时的共享对象
// 预分配的，一开始就创建好了
struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *cnegone, *pong, *space,
    *colon, *nullbulk, *nullmultibulk, *queued,
    *emptymultibulk, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *noscripterr, *loadingerr, *slowscripterr, *bgsaveerr,
    *masterdownerr, *roslaveerr, *execaborterr, *noautherr, *noreplicaserr,
    *busykeyerr, *oomerr, *plus, *messagebulk, *pmessagebulk, *subscribebulk,
    *unsubscribebulk, *psubscribebulk, *punsubscribebulk, *del, *rpop, *lpop,
    *lpush, *emptyscan, *minstring, *maxstring,
    *select[REDIS_SHARED_SELECT_CMDS],
    *integers[REDIS_SHARED_INTEGERS],
    *mbulkhdr[REDIS_SHARED_BULKHDR_LEN], /* "*<value>\r\n" */
    *bulkhdr[REDIS_SHARED_BULKHDR_LEN];  /* "$<value>\r\n" */
};

/* ZSETs use a specialized version of Skiplists */
/*
 * 跳跃表节点，仅仅针对 score 进行数据组织，obj 则是数据部分
 */
// TODO:(DONE) 画一下 zskipNode 之间的样子
// 在 t_zset.c 里面
// 假设 zskiplist 中间的某一个节点有 8 level，那也就意味着这个节点同时在 1、2、3 ... 8 这几个 level 上，都是作为其中一个 node 使用的！
// 每一层都会尽可能跳远一些
typedef struct zskiplistNode {

    // 成员对象
    // 采用引用计数的方式指向实际 obj，要记得相应的加减引用计数
    robj *obj;

    // 分值, 进行排序的依据
    double score;

    // 后退指针
    // TODO:(DONE) 为什么 backward 不放进 zskiplistlevel 里面？
    // （只会在 level 1 出现，仅仅用来进行 skiplist 的 range 遍历）
    /* c) there is a back pointer, so it's a doubly linked list with the back
     * pointers being only at "level 1". This allows to traverse the list
     * from tail to head, useful for ZREVRANGE. 
     */
    struct zskiplistNode *backward;

    // 层(TODO:(DONE) 为什么要采用柔性数组？ TODO:(DONE) 在 step-down 的时候用？)
    // 因为层数是不确定的。没错，之所以要记录这么多 level，就是为了能够原地 drill down
    struct zskiplistLevel {

        // 前进指针
        struct zskiplistNode *forward;

        // 跨度（TODO:(DONE) 以什么 level 的 node 作为单位？）最底层
        // 记录最底层 level 中，当前 node 与 forward node 之间，相隔了多少个最底层的 node 
        // 这是用来计算：node x，在整个 skip-list 里面的 rank（名次、排名，跟 index 类似）
        unsigned int span;

    } level[];

} zskiplistNode;

/*
 * 跳跃表
 * 小结点总是会更接近 header
 * score 相同的节点，将会按照 compareStringObjects 的结果大小来进行排序
 * 可以多个 robj 拥有同一个 score，但是不能同一个 robj 拥有不同的 score（这是一个 sorted_set，而不是 multi-set ！不允许重复的 element，也就是 robj）
 */
typedef struct zskiplist {

    // 表头节点和表尾节点
    // header、tail 会不会实际存放数据？
    // header 并不实际存放数据，是一个管理用的 node
    struct zskiplistNode *header, *tail;

    // 表中节点的数量（header 节点不计算在内）
    // 要是计算在内的话，向第一个 node 插入的时候，会有很多的麻烦（毕竟 header 节点是所有 level 都已经初始化好了的）
    unsigned long length;

    // 表中层数最大的节点的层数（header 的最高层数不在考虑范围内。header 的最高层总是直接指向 NULL，为了方便计数而留空的）
    int level;

} zskiplist;

/*
 * 有序集合
 * zset 是同时采用 dict + skiplist 来进行数据结构化管理
 * 而不是 zset 可以采用 skip list 或 dict 这种两底层 encoding 方式进行编码
 * struct zset 下面是同时管着一个 dict 跟 zsl 的
 * 这种情况下，score 既会保存在 zsl-node 里面，又会保存在 dict 里面（member 作为 key，score 作为 value）
 * 同样，member 的指针，在 zsl-node、dict-node 里面都会保存一份（访问起来更快）
 * 
 *    redis_obj           
 * +------------+         zset
 * |  robj.ptr  | ---> +---------+
 * +------------+      |  *dict  | ------> 
 *                     +---------+
 *                     |  *zsl   | ------> 
 *                     +---------+
 * 
 * 还有另一种情况就是，zset 整体 node 少，将会采用 ziplist 来进行管理，每一个 node 都会保存 (member, score) pair
 * 
 *    redis_obj         
 * +------------+      +----------+----------+---------+------------------+---------+
 * |  robj.ptr  | ---> | zlbytes  |  zltail  |  zllen  |  ... entrys ...  |  zlend  |
 * +------------+      +----------+----------+---------+------------------+---------+
 *                     ^
 *                     |
 *             unsigned char *ziplist
 */
typedef struct zset {

    // 字典当中，key 为 member，value 为 score
    // 用于支持 O(1) 复杂度的按成员取分值操作
    dict *dict;

    // 跳跃表，按 score 排序成员
    // 用于支持平均复杂度为 O(log N) 的按分值定位成员操作
    // 以及范围操作
    zskiplist *zsl;

} zset;

// 客户端缓冲区限制
typedef struct clientBufferLimitsConfig {
    // 硬限制
    unsigned long long hard_limit_bytes;
    // 软限制
    unsigned long long soft_limit_bytes;
    // 软限制时限
    time_t soft_limit_seconds;
} clientBufferLimitsConfig;

// 限制可以有多个
extern clientBufferLimitsConfig clientBufferLimitsDefaults[REDIS_CLIENT_LIMIT_NUM_CLASSES];

/* The redisOp structure defines a Redis Operation, that is an instance of
 * a command with an argument vector, database ID, propagation target
 * (REDIS_PROPAGATE_*), and command pointer.
 *
 * redisOp 结构定义了一个 Redis 操作，
 * 它包含指向被执行命令的指针、命令的参数、数据库 ID 、传播目标（REDIS_PROPAGATE_*）。
 *
 * Currently only used to additionally propagate more commands to AOF/Replication
 * after the propagation of the executed command. 
 *
 * 目前只用于在传播被执行命令之后，传播附加的其他命令到 AOF 或 Replication 中。
 */
typedef struct redisOp {

    // 参数
    robj **argv;

    // 参数数量、数据库 ID 、传播目标
    int argc, dbid, target;

    // 被执行命令的指针
    struct redisCommand *cmd;

} redisOp;

/* Defines an array of Redis operations. There is an API to add to this
 * structure in a easy way.
 *
 * redisOpArrayInit();
 * redisOpArrayAppend();
 * redisOpArrayFree();
 */
typedef struct redisOpArray {
    redisOp *ops;
    int numops;
} redisOpArray;

/*-----------------------------------------------------------------------------
 * Global server state
 *----------------------------------------------------------------------------*/

struct clusterState;

struct redisServer {

    /* General */

    // 配置文件的绝对路径
    char *configfile;           /* Absolute config file path, or NULL */

    // serverCron() 每秒调用的次数
    int hz;                     /* serverCron() calls frequency in hertz */

    // 数据库
    redisDb *db;

    // 命令表（受到 rename 配置选项的作用）
    dict *commands;             /* Command table */
    // 命令表（无 rename 配置选项的作用）
    dict *orig_commands;        /* Command table before command renaming. */

    // 事件状态
    aeEventLoop *el;

    // 最近一次使用时钟
    unsigned lruclock:REDIS_LRU_BITS; /* Clock for LRU eviction */

    // 关闭服务器的标识
    int shutdown_asap;          /* SHUTDOWN needed ASAP */

    // 在执行 serverCron() 时进行渐进式 rehash
    int activerehashing;        /* Incremental rehash in serverCron() */

    // 是否设置了密码
    char *requirepass;          /* Pass for AUTH command, or NULL */

    // PID 文件
    char *pidfile;              /* PID file path */

    // 架构类型
    int arch_bits;              /* 32 or 64 depending on sizeof(long) */

    // serverCron() 函数的运行次数计数器
    int cronloops;              /* Number of times the cron function run */

    // 本服务器的 RUN ID
    // TODO:(DONE) 现在看起来，这个 runid 所谓的每次执行都不同，是指：每一个 redis-server 的 runid 都会是不一样的
    // run id 是 2.8 版本之后，进行 replication 的核心标识之一；但是 4.0 版本之后，则是采用 repl-id
    // Redis服务器的随机标识符(用于Sentinel和集群)，重启后会改变。当复制时发现和之前的run_id不同时（说明 master 发生了变化），将会对数据进行全量同步。
    char runid[REDIS_RUN_ID_SIZE+1];  /* ID always different at every exec. */

    // 服务器是否运行在 SENTINEL 模式
    int sentinel_mode;          /* True if this instance is a Sentinel. */


    /* Networking */

    // TCP 监听端口
    int port;                   /* TCP listening port */

    int tcp_backlog;            /* TCP listen() backlog */

    // 地址
    char *bindaddr[REDIS_BINDADDR_MAX]; /* Addresses we should bind to */
    // 地址数量
    int bindaddr_count;         /* Number of addresses in server.bindaddr[] */

    // UNIX 套接字
    char *unixsocket;           /* UNIX socket path */
    mode_t unixsocketperm;      /* UNIX socket permission */

    // 描述符
    int ipfd[REDIS_BINDADDR_MAX]; /* TCP socket file descriptors */
    // 描述符数量
    int ipfd_count;             /* Used slots in ipfd[] */

    // UNIX 套接字文件描述符
    int sofd;                   /* Unix socket file descriptor */

    // 通常 cfd_count = 2, one for ipv4, another for ipv6
    int cfd[REDIS_BINDADDR_MAX];/* Cluster bus **listening** socket */
    int cfd_count;              /* Used slots in cfd[] */

    // 一个链表，保存了所有客户端状态结构
    list *clients;              /* List of active clients */
    // 链表，保存了所有待关闭的客户端，实现异步关闭（参考：freeClientAsync() 函数，加入；freeClientsInAsyncFreeQueue() 函数，释放）
    list *clients_to_close;     /* Clients to close asynchronously */

    // 链表，保存了所有 slave ，以及所有监视器
    list *slaves, *monitors;    /* List of slaves and MONITORs */

    // 服务器的当前客户端，仅用于崩溃报告
    redisClient *current_client; /* Current client, only used on crash report */

    int clients_paused;         /* True if clients are currently paused */
    mstime_t clients_pause_end_time; /* Time when we undo clients_paused */

    // 网络错误
    char neterr[ANET_ERR_LEN];   /* Error buffer for anet.c */

    // MIGRATE 缓存(毕竟真实场景中，同一个 slot 是会有很多 key-value pair 的)
    dict *migrate_cached_sockets;/* MIGRATE cached sockets */

//===========================================================
    /* RDB / AOF loading information */

    // 这个值为真时，表示服务器正在进行载入
    int loading;                /* We are loading data from disk if true */

    // 正在载入的数据的大小
    off_t loading_total_bytes;

    // 已载入数据的大小
    off_t loading_loaded_bytes;

    // 开始进行载入的时间
    time_t loading_start_time;
    off_t loading_process_events_interval_bytes;
//===========================================================

    /* Fast pointers to often looked up command */
    // 常用命令的快捷连接
    struct redisCommand *delCommand, *multiCommand, *lpushCommand, *lpopCommand,
                        *rpopCommand;


    /* Fields used only for stats */

    // 服务器启动时间
    time_t stat_starttime;          /* Server start time */

    // 已处理命令的数量
    long long stat_numcommands;     /* Number of processed commands */

    // 服务器接到的连接请求数量
    long long stat_numconnections;  /* Number of connections received */

    // 已过期的键数量
    long long stat_expiredkeys;     /* Number of expired keys */

    // 因为回收内存而被释放的过期键的数量
    long long stat_evictedkeys;     /* Number of evicted keys (maxmemory) */

    // 成功查找键的次数
    long long stat_keyspace_hits;   /* Number of successful lookups of keys */

    // 查找键失败的次数
    long long stat_keyspace_misses; /* Number of failed lookups of keys */

    // 已使用内存峰值
    size_t stat_peak_memory;        /* Max used memory record */

    // 最后一次执行 fork() 时消耗的时间
    // 仅仅是作为记录，能够通过 redis-cli 查看，作为 debug 性能的依据之一
    long long stat_fork_time;       /* Time needed to perform latest fork() */

    // 服务器因为客户端数量过多而拒绝客户端连接的次数
    long long stat_rejected_conn;   /* Clients rejected because of maxclients */

    // 执行 full sync 的次数
    long long stat_sync_full;       /* Number of full resyncs with slaves. */

    // PSYNC 成功执行的次数
    long long stat_sync_partial_ok; /* Number of accepted PSYNC requests. */

    // PSYNC 执行失败的次数
    long long stat_sync_partial_err;/* Number of unaccepted PSYNC requests. */


    /* slowlog */

    // 保存了所有慢查询日志的链表
    list *slowlog;                  /* SLOWLOG list of commands */

    // 下一条慢查询日志的 ID
    long long slowlog_entry_id;     /* SLOWLOG current entry ID */

    // 服务器配置 slowlog-log-slower-than 选项的值
    long long slowlog_log_slower_than; /* SLOWLOG time limit (to get logged) */

    // 服务器配置 slowlog-max-len 选项的值
    unsigned long slowlog_max_len;     /* SLOWLOG max number of items logged */
    size_t resident_set_size;       /* RSS sampled in serverCron(). */
    /* The following two are used to track instantaneous "load" in terms
     * of operations per second. */
    // 最后一次进行抽样的时间
    long long ops_sec_last_sample_time; /* Timestamp of last sample (in ms) */
    // 最后一次抽样时，服务器已执行命令的数量
    long long ops_sec_last_sample_ops;  /* numcommands in last sample */
    // 抽样结果
    long long ops_sec_samples[REDIS_OPS_SEC_SAMPLES];
    // 数组索引，用于保存抽样结果，并在需要时回绕到 0
    int ops_sec_idx;


    /* Configuration */

    // 日志可见性
    int verbosity;                  /* Loglevel in redis.conf */

    // 客户端最大空转时间
    int maxidletime;                /* Client timeout in seconds */

    // 是否开启 SO_KEEPALIVE 选项
    int tcpkeepalive;               /* Set SO_KEEPALIVE if non-zero. */
    int active_expire_enabled;      /* Can be disabled for testing purposes. */
    size_t client_max_querybuf_len; /* Limit for client query buffer length */
    int dbnum;                      /* Total number of configured DBs */
    int daemonize;                  /* True if running as a daemon */
    // 客户端输出缓冲区大小限制
    // 数组的元素有 REDIS_CLIENT_LIMIT_NUM_CLASSES 个
    // 每个代表一类客户端：普通、 slave 、pubsub，诸如此类
    clientBufferLimitsConfig client_obuf_limits[REDIS_CLIENT_LIMIT_NUM_CLASSES];

//==============================================================================
    /* AOF persistence */

    // AOF 状态（开启/关闭/可写）
    int aof_state;                  /* REDIS_AOF_(ON|OFF|WAIT_REWRITE) */

    // 所使用的 fsync 策略（每个写入/每秒/从不）
    int aof_fsync;                  /* Kind of fsync() policy */
    char *aof_filename;             /* Name of the AOF file */
    int aof_no_fsync_on_rewrite;    /* Don't fsync if a rewrite is in prog. */
    int aof_rewrite_perc;           /* Rewrite AOF if % growth is > M and... */
    off_t aof_rewrite_min_size;     /* the AOF file is at least N bytes. */

    // 最后一次执行 BGREWRITEAOF 时， AOF 文件的大小
    // 用来让 radis-server 决定是否需要自动进行 AOF rewrite
    off_t aof_rewrite_base_size;    /* AOF size on latest startup or rewrite. */

    // AOF 文件的当前字节大小
    off_t aof_current_size;         /* AOF current size. */
    int aof_rewrite_scheduled;      /* Rewrite once BGSAVE terminates. */

    // 负责进行 AOF 重写的子进程 ID
    pid_t aof_child_pid;            /* PID if rewriting process */

    // AOF 重写缓存链表，链接着多个缓存块
    // 缓存块是：struct aofrwblock，每一个 block 能够容纳 10 MB 的内容，
    // 所有的 aof-rewirte-buf 都有这个链表进行管理
    list *aof_rewrite_buf_blocks;   /* Hold changes during an AOF rewrite. */

    // AOF 缓冲区
    // 因为生成 AOF 记录跟 AOF 记录落盘，代码是分开的，所以要有个 heap buf 暂缓一下临时数据
    // 利用 heap-buf 的方式共享\通信数据
    sds aof_buf;      /* AOF buffer, written before entering the event loop */

    // AOF 文件的描述符
    int aof_fd;       /* File descriptor of currently selected AOF file */

    // AOF 的当前目标数据库
    // 当 redis-server 被使用了两个以上的不同 DB，将会非常有用
    int aof_selected_db; /* Currently selected DB in AOF */

    // 推迟 write 操作的时间
    time_t aof_flush_postponed_start; /* UNIX time of postponed AOF flush */

    // 最后一直执行 fsync 的时间
    time_t aof_last_fsync;            /* UNIX time of last fsync() */
    time_t aof_rewrite_time_last;   /* Time used by last AOF rewrite run. */

    // AOF 重写的开始时间
    time_t aof_rewrite_time_start;  /* Current AOF rewrite start time. */

    // 最后一次执行 BGREWRITEAOF 的结果
    int aof_lastbgrewrite_status;   /* REDIS_OK or REDIS_ERR */

    // 记录 AOF 的 write 操作被推迟了多少次
    unsigned long aof_delayed_fsync;  /* delayed AOF fsync() counter */

    // 指示是否需要每写入一定量的数据，就主动执行一次 fsync()
    int aof_rewrite_incremental_fsync;/* fsync incrementally while rewriting? */
    int aof_last_write_status;      /* REDIS_OK or REDIS_ERR */
    int aof_last_write_errno;       /* Valid if aof_last_write_status is ERR */
    
//==============================================================================
    /* RDB persistence */

    // dirty、dirty_before_bgsave 的配合使用，可以得出：还有多少个数据操作是没有被持久化至硬盘上的
    // 自从上次 SAVE 执行以来，数据库被修改的次数
    // TODO:(DONE) 这个 dirty 仅仅是记录一下脏数据的数量？没有在哪里用来进行判断啥的（例如 AOF，RDB 追加）
    // 当然不止，参考 redis.c:call() 中：
    // 只有数据库被修改过了（脏了），才会执行 AOF、repl 这些命令的传播
    /*
        // 如果数据库有被修改，那么启用 REPL 和 AOF 传播
        if (dirty)
            flags |= (REDIS_PROPAGATE_REPL | REDIS_PROPAGATE_AOF);
    */
    // TODO:(DONE) 既然如此，dirty 没有及时清零的话，岂不是一直会触发 AOF（哪怕是一个 get 命令）
    // 当然不会，上面的 dirty 并不是 server.dirty，在执行前后，都会更新一次 dirty
    // 这样就能够正确计算出：执行当前这个命令，有没有修改过数据库
    /*
        // 保留旧 dirty 计数器值
        dirty = server.dirty;

        // 执行实现函数
        c->cmd->proc(c);

        // 计算命令执行之后的 dirty 值
        dirty = server.dirty-dirty;    
    */
    // TODO: 这个 dirty 什么时候会被清零？
    long long dirty;                /* Changes to DB from the last save */

    // BGSAVE 执行前的数据库被修改次数
    long long dirty_before_bgsave;  /* Used to restore dirty on failed BGSAVE */

    // 负责执行 BGSAVE 的子进程的 ID
    // 没在执行 BGSAVE 时，设为 -1
    pid_t rdb_child_pid;            /* PID of RDB saving child */
    struct saveparam *saveparams;   /* Save points array for RDB */
    int saveparamslen;              /* Number of saving points */
    char *rdb_filename;             /* Name of RDB file */
    int rdb_compression;            /* Use compression in RDB? */
    int rdb_checksum;               /* Use RDB checksum? */

    // 最后一次完成 SAVE 的时间
    time_t lastsave;                /* Unix time of last successful save */

    // 最后一次尝试执行 BGSAVE 的时间
    time_t lastbgsave_try;          /* Unix time of last attempted bgsave */

    // 最近一次 BGSAVE 执行耗费的时间
    time_t rdb_save_time_last;      /* Time used by last RDB save run. */

    // 数据库最近一次开始执行 BGSAVE 的时间
    time_t rdb_save_time_start;     /* Current RDB save start time. */

    // 最后一次执行 SAVE 的状态
    int lastbgsave_status;          /* REDIS_OK or REDIS_ERR */
    int stop_writes_on_bgsave_err;  /* Don't allow writes if can't BGSAVE */


    /* Propagation of commands in AOF / replication */
    redisOpArray also_propagate;    /* Additional command to propagate. */


    /* Logging */
    char *logfile;                  /* Path of log file */
    int syslog_enabled;             /* Is syslog enabled? */
    char *syslog_ident;             /* Syslog ident */
    int syslog_facility;            /* Syslog facility */


    /* Replication (master) */
    int slaveseldb;                 /* Last SELECTed DB in replication output */

    //  master 发送 PING 的频率
    int repl_ping_slave_period;     /* Master pings the slave every N seconds */


    // backlog 本身 
    // TODO:(DONE) 环形缓冲？是的，环形，实际上就是条状，多了的从头开始覆盖罢了；
    // 参考 feedReplicationBacklog()
    char *repl_backlog;             /* Replication backlog for partial syncs */
    // backlog 的长度，REDIS_DEFAULT_REPL_BACKLOG_SIZE 1 MB，这个的大小基本上是不会变的
    long long repl_backlog_size;    /* Backlog circular buffer size */
    // 是记录下一次写入的时候，从哪里开始追加写入
    long long repl_backlog_idx;     /* Backlog circular buffer current offset */

    // 实话说，这个一旦到达了 repl_backlog_size 之后，大小就不会变化了，一直都是 repl_backlog_size
    // 相当于一个溢出标志位罢了
    // 因为环形队列需要做到：
    // 1. 被全部 slave ack 的数据，会被标记为可以覆盖；（实际上 redis 不会这样，因为这只是一个 PSYNC 的判断依据）
    // 2. 还没有被 ack 的数据，依旧保留
    // 为什么要有这个 histlen 呢？适配缓冲区还没有溢出的情况，溢出且开始覆盖之后，histlen 恒等于 repl_backlog_size
    // 无论溢出与否，都是 backlog 里面的有效数据长度（预处之后，有效数据长度就固定为 repl_backlog_size 了）
    long long repl_backlog_histlen; /* Backlog actual data length */

    // **缓冲区中的数据，最开头那一位的版本号**
    // backlog 的当前索引（范围是：0 -- repl_backlog_size）
    // backlog 中可以被还原的第一个字节的偏移量
    /* Each master also takes an offset that increments for every byte of replication stream 
     * that it is produced to be sent to replicas, in order to update the state of the replicas 
     * with the new changes modifying the dataset. The replication offset is incremented
     * even if no replica is actually connected
     * 
     * **Identifies an exact version of the dataset of a master.**
     */
    long long repl_backlog_off;     /* Replication offset of first byte in the
                                       backlog buffer. */
    // 全局复制偏移量（一个累计值），作为 master 的 redis-server 负责记录
    /* master 在 handle 完每一个 CMD 之后，就会根据 repl 的开关，看看要不要进行 CMD 的 repl 传播
     * 需要且 DB dirty 之后，就会在 cron 中进行传播，然后调用 feedReplicationBacklog() 
     * 把 CMD 记录在 server.repl_backlog 里面，并更新 master_repl_offset。
     * 
     * 然后 slave 每次跟 master 沟通的时候，就会向 master 回馈：
     * 目前我这个 slave 读取到了你缓冲里面的 master_repl_offset 位置的命令，
     * 你看看是不是最新的，不是的话，继续同步其他 CMD 给我。
     * 啥？我进度落后太多了？已经没办法进行 PSYNC 了？好吧，我准备一下重新 FULL SYNC
     * 
     * 每次，这个位于 server 上面的 repl_backlog 环形缓冲区，是只有一个的，而且只会在 server 上面
     * 而 master_repl_offset 的最新值也是在 server 上面的；
     * 在 slave 的 redis-server 上面，都会有各自的同步进度，都是奔着 master_repl_offset 去的
     * 
     * 在进行 FULL SYNC 的时候，master 把这个当前的 master_repl_offset 发送给 slave，就像下面这样：
     * "+FULLRESYNC 21a931865c82d28ec842c7c60bdc0ae6ceeaa12a 1\r\n\000"
     */
    // **缓冲区中的数据，最末尾那一位的版本号**
    // 不停累计，作为一个修改的版本号。master 会在 masterTryPartialResynchronization() 的时候
    // psync_offset = server.master_repl_offset; 当前的版本号同步给 slave
    // 每次 slave 想要进行 PSYNC 的时候，master 看看 slave 的版本号，是否能够从 backlog 这个环形缓冲区中恢复
    long long master_repl_offset;   /* Global replication offset */
//==========================================================
// NOTE: master_repl_offset\repl_backlog_off 这两个都是不断累加的，相当于一个版本号
//       master_repl_offset 缓冲区中的数据，最末尾那一位的版本号
//       repl_backlog_off   缓冲区中的数据，最开头那一位的版本号
// NOTE: repl_backlog_idx\repl_backlog_size 才是作为数组的下标使用
//==========================================================

    // backlog 的过期时间
    time_t repl_backlog_time_limit; /* Time without slaves after the backlog
                                       gets released. */


    // 距离上一次有 slave 的时间
    time_t repl_no_slaves_since;    /* We have no slaves since that time.
                                       Only valid if server.slaves len is 0. */

    // 是否开启最小数量 slave 写入功能
    int repl_min_slaves_to_write;   /* Min number of slaves to write. */
    // 定义最小数量 slave 的最大延迟值
    int repl_min_slaves_max_lag;    /* Max lag of <count> slaves to write. */
    // 延迟良好的 slave 的数量
    int repl_good_slaves_count;     /* Number of slaves with lag <= max_lag. */

//=====================================================================
    /* Replication (slave), 当本机是 slave 的时候，将会利用下面的结构体保存相连接的 master 信息 */
    // 对应的 master 验证密码
    char *masterauth;               /* AUTH with this password with master */
    // 对应的 master 地址
    char *masterhost;               /* Hostname of master */
    // 对应的 master 端口
    int masterport;                 /* Port of master */
    // 超时时间，超时则视为 master <--> slave 之间的连接断开；默认 REDIS_REPL_TIMEOUT 60s
    // 会在 replicationCron 进行周期性的检查
    int repl_timeout;               /* Timeout after N seconds of master idle */
    // slave 将会使用这个 redisClient 跟 master 进行通信
    redisClient *master;     /* Client that is master for this slave */

    /* SYNC 过程中的使用 */
    // slave 才会使用：被缓存的 master ，PSYNC 时使用（为了处理 slave、master 短暂失联的情况）
    // 将原本的 master 放到 cached_master 这里来，不能彻底销毁，因为 slave --> master 的 client，
    // 保存至 repl 同步进度的信息
    redisClient *cached_master; /* Cached master to be reused for PSYNC. */


    int repl_syncio_timeout; /* Timeout for synchronous I/O calls */
    // 复制的状态（服务器是 slave 时使用）
    int repl_state;          /* Replication status if the instance is a slave */
    // RDB 文件的大小
    off_t repl_transfer_size; /* Size of RDB to read from master during sync. */
    // 已读 RDB 文件内容的字节数
    off_t repl_transfer_read; /* Amount of RDB read from master during sync. */
    // 最近一次执行 fsync 时的偏移量
    // 用于 sync_file_range 函数
    off_t repl_transfer_last_fsync_off; /* Offset when we fsync-ed last time. */
    //  在 slave 中，slave 连接向 master 的套接字
    int repl_transfer_s;     /* Slave -> Master SYNC socket */
    // 保存 RDB 文件的临时文件的描述符
    int repl_transfer_fd;    /* Slave -> Master SYNC temp file descriptor */
    // 保存 RDB 文件的临时文件名字
    char *repl_transfer_tmpfile; /* Slave-> master SYNC temp file name */
    // 最近一次读入 RDB 内容的时间
    time_t repl_transfer_lastio; /* Unix time of the latest read, for timeout */


    int repl_serve_stale_data; /* Serve stale data when link is down? */
    // 是否只读 slave ？
    int repl_slave_ro;          /* Slave is read only? */
    // 记录了 slave 连接断开的时间点
    time_t repl_down_since; /* Unix time at which link with master went down */
    // 是否要在 SYNC 之后关闭 NODELAY ？
    /*  Disable TCP_NODELAY on the slave socket after SYNC?
    * 
    *  If you select "yes" Redis will use a smaller number of TCP packets and
    *  less bandwidth to send data to slaves. But this can add a delay for
    *  the data to appear on the slave side, up to 40 milliseconds with
    *  Linux kernels using a default configuration.
    * 
    *  If you select "no" the delay for data to appear on the slave side will
    *  be reduced but more bandwidth will be used for replication.
    * 
    *  By default we optimize for low latency, but in very high traffic conditions
    *  or when the master and slaves are many hops away, turning this to "yes" may
    *  be a good idea.
    * 
    * 采用聚合等待的方式，能够更好的利用网络带宽进行 repl，但是 slave 的同步延迟会比较大
    * 不采用聚合等待的方式，将会占用更多的网络资源，但是能够很好的降低 slave 的网络延迟问题（默认选择）
    */
    int repl_disable_tcp_nodelay;   /* Disable TCP_NODELAY after SYNC? */

    //  slave 优先级
    int slave_priority;             /* Reported in INFO and used by Sentinel. */
    // 本服务器（ slave ）当前 master 的 RUN ID
    char repl_master_runid[REDIS_RUN_ID_SIZE+1];  /* Master run id for PSYNC. */
    // 初始化偏移量，将会在第一次进行 FULL SYNC 的时候，由 master 告知 slave：master 现在的 server.master_repl_offset 是多少
    // 因为 PSYNC 的关键是：master 不停地向 slave 推送数据，当断开连接时，自然是让 slave 告知 master：上次 slave 都到哪里了，我们继续
    long long repl_master_initial_offset;         /* Master PSYNC offset. */


    /* Replication script cache. */
    // 复制脚本缓存
    // 字典
    dict *repl_scriptcache_dict;        /* SHA1 all slaves are aware of. */
    // FIFO 队列
    list *repl_scriptcache_fifo;        /* First in, first out LRU eviction. */
    // 缓存的大小
    int repl_scriptcache_size;          /* Max number of elements. */

    /* Synchronous replication. */
    list *clients_waiting_acks;         /* Clients waiting in WAIT command. */
    int get_ack_from_slaves;            /* If true we send REPLCONF GETACK. */
    /* Limits */
    int maxclients;                 /* Max number of simultaneous clients */
    unsigned long long maxmemory;   /* Max number of memory bytes to use */
    int maxmemory_policy;           /* Policy for key eviction */
    int maxmemory_samples;          /* Pricision of random sampling */


    /* Blocked clients */
    unsigned int bpop_blocked_clients; /* Number of clients blocked by lists */
    list *unblocked_clients; /* list of clients to unblock before next loop */
    list *ready_keys;        /* List of readyList structures for BLPOP & co，将会被 handleClientsBlockedOnLists() 处理 */
    // 里面保存了刚刚执行了 push 的 key(顺序保存)，能被加入 ready_keys 的前提是：这个 key 是被 block 的

    /* Sort parameters - qsort_r() is only available under BSD so we
     * have to take this state global, in order to pass it to sortCompare() */
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    int sort_store;


    /* Zip structure config, see redis.conf for more information  */
    size_t hash_max_ziplist_entries;
    size_t hash_max_ziplist_value;
    size_t list_max_ziplist_entries;
    size_t list_max_ziplist_value;
    size_t set_max_intset_entries;
    size_t zset_max_ziplist_entries;
    size_t zset_max_ziplist_value;
    size_t hll_sparse_max_bytes;
    time_t unixtime;        /* Unix time sampled every cron cycle. */
    long long mstime;       /* Like 'unixtime' but with milliseconds resolution. */


    /* Pubsub */
    // 字典，键为频道，值为链表
    // 链表中保存了所有订阅某个频道的客户端
    // 新客户端总是被添加到链表的表尾
    dict *pubsub_channels;  /* Map channels to list of subscribed clients */

    // 这个链表记录了客户端订阅的所有模式的名字
    list *pubsub_patterns;  /* A list of pubsub_patterns */

    int notify_keyspace_events; /* Events to propagate via Pub/Sub. This is an
                                   xor of REDIS_NOTIFY... flags. */


    /* Cluster */
    // 不仅仅是影响相关的 CLI 执行权限，还会触发不同的 server-cron
    int cluster_enabled;      /* Is cluster enabled? */
    mstime_t cluster_node_timeout; /* Cluster node timeout. */
    char *cluster_configfile; /* Cluster auto-generated config file name. */
    struct clusterState *cluster;  /* State of the cluster, 这是本 node 对于整个 cluster 的看法，记录了本 node 对于 cluster 的判断 */

    // 在 replicas migration 之前，会确保每一个 master 都至少有 server.cluster_migration_barrier 个 slave，才能进行 slave(replicas) migration
    int cluster_migration_barrier; /* Cluster replicas migration barrier. */
    /* Scripting */

    // Lua 环境
    lua_State *lua; /* The Lua interpreter. We use just one for all clients */
    
    // 复制执行 Lua 脚本中的 Redis 命令的伪客户端
    redisClient *lua_client;   /* The "fake client" to query Redis from Lua */

    // 当前正在执行 EVAL 命令的客户端，如果没有就是 NULL
    redisClient *lua_caller;   /* The client running EVAL right now, or NULL */

    // 一个字典，值为 Lua 脚本，键为脚本的 SHA1 校验和
    dict *lua_scripts;         /* A dictionary of SHA1 -> Lua scripts */
    // Lua 脚本的执行时限
    mstime_t lua_time_limit;  /* Script timeout in milliseconds */
    // 脚本开始执行的时间
    mstime_t lua_time_start;  /* Start time of script, milliseconds time */

    // 脚本是否执行过写命令
    int lua_write_dirty;  /* True if a write command was called during the
                             execution of the current script. */

    // 脚本是否执行过带有随机性质的命令
    int lua_random_dirty; /* True if a random command was called during the
                             execution of the current script. */

    // 脚本是否超时
    int lua_timedout;     /* True if we reached the time limit for script
                             execution. */

    // 是否要杀死脚本
    int lua_kill;         /* Kill the script if true. */


    /* Assert & bug reporting */

    char *assert_failed;
    char *assert_file;
    int assert_line;
    int bug_report_start; /* True if bug report header was already logged. */
    int watchdog_period;  /* Software watchdog period in ms. 0 = off */
};

/*
 * 记录订阅模式的结构
 */
typedef struct pubsubPattern {

    // 订阅模式的客户端
    redisClient *client;

    // 被订阅的模式
    robj *pattern;

} pubsubPattern;

typedef void redisCommandProc(redisClient *c);
typedef int *redisGetKeysProc(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);

/*
 * Redis 命令
 * 所有的命令，都在程序起来的时候被初始化为全局变量
 * name 就是对外公开的 CLI 名称，proc 就是相应命令的函数指针，回调函数全部定义在 t_ 开头的文件里面
 */
struct redisCommand {

    // 命令名字
    // 举例：set\hset\get\hget 之类得
    char *name;

    // 实现函数
    // 与 name 相对应的函数指针
    redisCommandProc *proc;

    // 参数个数
    int arity;

    // 字符串表示的 FLAG
    char *sflags; /* Flags as string representation, one char per flag. */

    // 实际 FLAG
    int flags;    /* The actual flags, obtained from the 'sflags' field. */

    /* Use a function to determine keys arguments in a command line.
     * Used for Redis Cluster redirect. */
    // 从命令中判断命令的键参数。在 Redis 集群转向时使用。
    // TODO: 怎么用的？cluser.c: getNodeByQuery() ---> getKeysFromCommand()
    redisGetKeysProc *getkeys_proc;

    /* What keys should be loaded in background when calling this command? */
    // 指定哪些参数是 key
    int firstkey; /* The first argument that's a key (0 = no keys) */
    int lastkey;  /* The last argument that's a key */
    int keystep;  /* The step between first and last key */

    // 统计信息
    // microseconds 记录了命令执行耗费的总毫微秒数
    // calls 是命令被执行的总次数
    long long microseconds, calls;
};

struct redisFunctionSym {
    char *name;
    unsigned long pointer;
};

// 用于保存被排序值及其权重的结构
typedef struct _redisSortObject {

    // 被排序键的值
    robj *obj;

    // 权重
    union {

        // 排序数字值时使用
        double score;

        // 排序字符串时使用
        robj *cmpobj;

    } u;

} redisSortObject;

// 排序操作
typedef struct _redisSortOperation {

    // 操作的类型，可以是 GET 、 DEL 、INCR 或者 DECR
    // 目前只实现了 GET
    int type;

    // 用户给定的模式
    robj *pattern;

} redisSortOperation;

// Type 类型的结构体，是为了能够兼容底层多种编码方式的存在
// 相当于一层中间转发

/* Structure to hold list iteration abstraction.
 *
 * 列表迭代器对象
 */
typedef struct {

    // 列表对象
    robj *subject;

    // 对象所使用的编码（ziplist\link-list）
    unsigned char encoding;

    // 迭代的方向
    unsigned char direction; /* Iteration direction */

    // ziplist 索引，迭代 ziplist 编码的列表时使用（指向的这一个节点，需要多少 offset）
    // 总是指向下一个 node
    unsigned char *zi;

    // 链表节点的指针，迭代双端链表编码的列表时使用
    // 总是指向下一个 node
    listNode *ln;

} listTypeIterator;

/* Structure for an entry while iterating over a list.
 *
 * 迭代列表时使用的记录结构，
 * 用于保存迭代器，以及迭代器返回的列表节点。
 * 总是需要跟 listTypeIterator 一起配合使用，因为 listTypeEenntry 并没有记录 encoding field
 * listTypeIterator 里面的指针，总是指向下一次即将访问的 node（防止迭代器失效），被 li 指向的 node 总是还没有被访问的
 * listTypeEenntry 里面的指针，则总是指向当前需要访问的指针（本次 while-loop 的访问数据）
 */
typedef struct {

    // 列表迭代器（避免迭代器失效）
    // listTypeGet() 里面会用到
    // 之所以要这样存一下，是因为，当你只拿到了 listTypeEntry 这一个 struct
    // 你是没办法确定究竟应该用 zi 还是 ln 来读取数据（你没办法判断 zi == NULL, ln == NULL 究竟是正确的，还是一个异常的行为）
    // 所以才要利用到 li->encoding
    // 另一个作用的地方是 listTypeDelete()，更新链表头、更新 zi（ln 不需要，因为 ln 不会整体进行内存 realloc）
    listTypeIterator *li;

    // ziplist 节点索引
    // 指向当前的 node
    unsigned char *zi;  /* Entry in ziplist */

    // 双端链表节点指针
    // 指向当前的 node
    listNode *ln;       /* Entry in linked list */

} listTypeEntry;

/* Structure to hold set iteration abstraction. */
/*
 * 多态集合迭代器
 */
typedef struct {

    // 被迭代的对象
    robj *subject;

    // 对象的编码
    int encoding;

    // 索引值，编码为 intset 时使用，intset 的遍历是直接通过 index 来完成的，所以 li 也是直接记录遍历的 index 就好
    int ii; /* intset iterator */

    // 字典迭代器，编码为 HT 时使用
    dictIterator *di;

} setTypeIterator;

/* Structure to hold hash iteration abstraction. Note that iteration over
 * hashes involves both fields and values. Because it is possible that
 * not both are required, store pointers in the iterator to avoid
 * unnecessary memory allocation for fields/values. */
/*
 * 哈希对象的迭代器
 */
typedef struct {

    // 被迭代的哈希对象
    robj *subject;

    // 哈希对象的编码（可以优化掉，毕竟在 subject 里面已经有了）
    int encoding;

    // 域指针和值指针
    // 在迭代 ZIPLIST 编码的哈希对象时使用
    unsigned char *fptr, *vptr;

    // 字典迭代器和指向当前迭代字典节点的指针
    // 在迭代 HT 编码的哈希对象时使用
    dictIterator *di;
    dictEntry *de;
} hashTypeIterator;

// 取出 field 还是 value 字段
#define REDIS_HASH_KEY 1
#define REDIS_HASH_VALUE 2

/*-----------------------------------------------------------------------------
 * Extern declarations
 *----------------------------------------------------------------------------*/

extern struct redisServer server;
extern struct sharedObjectsStruct shared;
extern dictType setDictType;
extern dictType zsetDictType;
extern dictType clusterNodesDictType;
extern dictType clusterNodesBlackListDictType;
extern dictType dbDictType;
extern dictType shaScriptObjectDictType;
extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
extern dictType hashDictType;
extern dictType replScriptCacheDictType;

/*-----------------------------------------------------------------------------
 * Functions prototypes
 *----------------------------------------------------------------------------*/

/* Utils */
long long ustime(void);
long long mstime(void);
void getRandomHexChars(char *p, unsigned int len);
uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);
void exitFromChild(int retcode);
size_t redisPopcount(void *s, long count);
void redisSetProcTitle(char *title);

/* networking.c -- Networking and Client related operations */
redisClient *createClient(int fd);
void closeTimedoutClients(void);
void freeClient(redisClient *c);
void freeClientAsync(redisClient *c);
void resetClient(redisClient *c);
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask);
void addReply(redisClient *c, robj *obj);
void *addDeferredMultiBulkLength(redisClient *c);
void setDeferredMultiBulkLength(redisClient *c, void *node, long length);
void addReplySds(redisClient *c, sds s);
void processInputBuffer(redisClient *c);
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
void addReplyBulk(redisClient *c, robj *obj);
void addReplyBulkCString(redisClient *c, char *s);
void addReplyBulkCBuffer(redisClient *c, void *p, size_t len);
void addReplyBulkLongLong(redisClient *c, long long ll);
void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void addReply(redisClient *c, robj *obj);
void addReplySds(redisClient *c, sds s);
void addReplyError(redisClient *c, char *err);
void addReplyStatus(redisClient *c, char *status);
void addReplyDouble(redisClient *c, double d);
void addReplyLongLong(redisClient *c, long long ll);
void addReplyMultiBulkLen(redisClient *c, long length);
void copyClientOutputBuffer(redisClient *dst, redisClient *src);
void *dupClientReplyValue(void *o);
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer);
void formatPeerId(char *peerid, size_t peerid_len, char *ip, int port);
char *getClientPeerId(redisClient *client);
sds catClientInfoString(sds s, redisClient *client);
sds getAllClientsInfoString(void);
void rewriteClientCommandVector(redisClient *c, int argc, ...);
void rewriteClientCommandArgument(redisClient *c, int i, robj *newval);
unsigned long getClientOutputBufferMemoryUsage(redisClient *c);
void freeClientsInAsyncFreeQueue(void);
void asyncCloseClientOnOutputBufferLimitReached(redisClient *c);
int getClientLimitClassByName(char *name);
char *getClientLimitClassName(int class);
void flushSlavesOutputBuffers(void);
void disconnectSlaves(void);
int listenToPort(int port, int *fds, int *count);
void pauseClients(mstime_t duration);
int clientsArePaused(void);
int processEventsWhileBlocked(void);

#ifdef __GNUC__
void addReplyErrorFormat(redisClient *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void addReplyStatusFormat(redisClient *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void addReplyErrorFormat(redisClient *c, const char *fmt, ...);
void addReplyStatusFormat(redisClient *c, const char *fmt, ...);
#endif

/* List data type */
void listTypeTryConversion(robj *subject, robj *value);
void listTypePush(robj *subject, robj *value, int where);
robj *listTypePop(robj *subject, int where);
unsigned long listTypeLength(robj *subject);
listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction);
void listTypeReleaseIterator(listTypeIterator *li);
int listTypeNext(listTypeIterator *li, listTypeEntry *entry);
robj *listTypeGet(listTypeEntry *entry);
void listTypeInsert(listTypeEntry *entry, robj *value, int where);
int listTypeEqual(listTypeEntry *entry, robj *o);
void listTypeDelete(listTypeEntry *entry);
void listTypeConvert(robj *subject, int enc);
void unblockClientWaitingData(redisClient *c);
void handleClientsBlockedOnLists(void);
void popGenericCommand(redisClient *c, int where);

/* MULTI/EXEC/WATCH... */
void unwatchAllKeys(redisClient *c);
void initClientMultiState(redisClient *c);
void freeClientMultiState(redisClient *c);
void queueMultiCommand(redisClient *c);
void touchWatchedKey(redisDb *db, robj *key);
void touchWatchedKeysOnFlush(int dbid);
void discardTransaction(redisClient *c);
void flagTransaction(redisClient *c);

/* Redis object implementation */
void decrRefCount(robj *o);
void decrRefCountVoid(void *o);
void incrRefCount(robj *o);
robj *resetRefCount(robj *obj);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
robj *createObject(int type, void *ptr);
robj *createStringObject(char *ptr, size_t len);
robj *createRawStringObject(char *ptr, size_t len);
robj *createEmbeddedStringObject(char *ptr, size_t len);
robj *dupStringObject(robj *o);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *tryObjectEncoding(robj *o);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongDouble(long double value);
robj *createListObject(void);
robj *createZiplistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetZiplistObject(void);
int getLongFromObjectOrReply(redisClient *c, robj *o, long *target, const char *msg);
int checkType(redisClient *c, robj *o, int type);
int getLongLongFromObjectOrReply(redisClient *c, robj *o, long long *target, const char *msg);
int getDoubleFromObjectOrReply(redisClient *c, robj *o, double *target, const char *msg);
int getLongLongFromObject(robj *o, long long *target);
int getLongDoubleFromObject(robj *o, long double *target);
int getLongDoubleFromObjectOrReply(redisClient *c, robj *o, long double *target, const char *msg);
char *strEncoding(int encoding);
int compareStringObjects(robj *a, robj *b);
int collateStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);
unsigned long long estimateObjectIdleTime(robj *o);
#define sdsEncodedObject(objptr) (objptr->encoding == REDIS_ENCODING_RAW || objptr->encoding == REDIS_ENCODING_EMBSTR)

/* Synchronous I/O with timeout */
ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout);

/* Replication */
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);
void replicationFeedMonitors(redisClient *c, list *monitors, int dictid, robj **argv, int argc);
void updateSlavesWaitingBgsave(int bgsaveerr);
void replicationCron(void);
void replicationHandleMasterDisconnection(void);
void replicationCacheMaster(redisClient *c);
void resizeReplicationBacklog(long long newsize);
void replicationSetMaster(char *ip, int port);
void replicationUnsetMaster(void);
void refreshGoodSlavesCount(void);
void replicationScriptCacheInit(void);
void replicationScriptCacheFlush(void);
void replicationScriptCacheAdd(sds sha1);
int replicationScriptCacheExists(sds sha1);
void processClientsWaitingReplicas(void);
void unblockClientWaitingReplicas(redisClient *c);
int replicationCountAcksByOffset(long long offset);
void replicationSendNewlineToMaster(void);
long long replicationGetSlaveOffset(void);

/* Generic persistence functions */
void startLoading(FILE *fp);
void loadingProgress(off_t pos);
void stopLoading(void);

/* RDB persistence */
#include "rdb.h"

/* AOF persistence */
void flushAppendOnlyFile(int force);
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc);
void aofRemoveTempFile(pid_t childpid);
int rewriteAppendOnlyFileBackground(void);
int loadAppendOnlyFile(char *filename);
void stopAppendOnly(void);
int startAppendOnly(void);
void backgroundRewriteDoneHandler(int exitcode, int bysignal);
void aofRewriteBufferReset(void);
unsigned long aofRewriteBufferSize(void);

/* Sorted sets data type */

/* Struct to hold a inclusive/exclusive range spec by score comparison. */
// 表示开区间/闭区间范围的结构
typedef struct {

    // 最小值和最大值
    double min, max;

    // 指示最小值和最大值是否*不*包含在范围之内
    // 值为 1 表示不包含，值为 0 表示包含
    int minex, maxex; /* are min or max exclusive? */
} zrangespec;

/* Struct to hold an inclusive/exclusive range spec by lexicographic comparison. */
typedef struct {
    robj *min, *max;  /* May be set to shared.(minstring|maxstring) */
    int minex, maxex; /* are min or max exclusive? */
} zlexrangespec;

/* zsl, zip-skip-list */
zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, robj *obj);
unsigned char *zzlInsert(unsigned char *zl, robj *ele, double score);
int zslDelete(zskiplist *zsl, double score, robj *obj);
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);
double zzlGetScore(unsigned char *sptr);
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
unsigned int zsetLength(robj *zobj);
void zsetConvert(robj *zobj, int encoding);
unsigned long zslGetRank(zskiplist *zsl, double score, robj *o);

/* Core functions */
int freeMemoryIfNeeded(void);
int processCommand(redisClient *c);
void setupSignalHandlers(void);
struct redisCommand *lookupCommand(sds name);
struct redisCommand *lookupCommandByCString(char *s);
struct redisCommand *lookupCommandOrOriginal(sds name);
void call(redisClient *c, int flags);
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int flags);
void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int target);
void forceCommandPropagation(redisClient *c, int flags);
int prepareForShutdown();
#ifdef __GNUC__
void redisLog(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void redisLog(int level, const char *fmt, ...);
#endif
void redisLogRaw(int level, const char *msg);
void redisLogFromHandler(int level, const char *msg);
void usage();
void updateDictResizePolicy(void);
int htNeedsResize(dict *dict);
void oom(const char *msg);
void populateCommandTable(void);
void resetCommandTableStats(void);
void adjustOpenFilesLimit(void);
void closeListeningSockets(int unlink_unix_socket);
void updateCachedTime(void);
void resetServerStats(void);
unsigned int getLRUClock(void);

/* Set data type */
robj *setTypeCreate(robj *value);
int setTypeAdd(robj *subject, robj *value);
int setTypeRemove(robj *subject, robj *value);
int setTypeIsMember(robj *subject, robj *value);
setTypeIterator *setTypeInitIterator(robj *subject);
void setTypeReleaseIterator(setTypeIterator *si);
int setTypeNext(setTypeIterator *si, robj **objele, int64_t *llele);
robj *setTypeNextObject(setTypeIterator *si);
int setTypeRandomElement(robj *setobj, robj **objele, int64_t *llele);
unsigned long setTypeSize(robj *subject);
void setTypeConvert(robj *subject, int enc);

/* Hash data type */
void hashTypeConvert(robj *o, int enc);
void hashTypeTryConversion(robj *subject, robj **argv, int start, int end);
void hashTypeTryObjectEncoding(robj *subject, robj **o1, robj **o2);
robj *hashTypeGetObject(robj *o, robj *key);
int hashTypeExists(robj *o, robj *key);
int hashTypeSet(robj *o, robj *key, robj *value);
int hashTypeDelete(robj *o, robj *key);
unsigned long hashTypeLength(robj *o);
hashTypeIterator *hashTypeInitIterator(robj *subject);
void hashTypeReleaseIterator(hashTypeIterator *hi);
int hashTypeNext(hashTypeIterator *hi);
void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                unsigned char **vstr,
                                unsigned int *vlen,
                                long long *vll);
void hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what, robj **dst);
robj *hashTypeCurrentObject(hashTypeIterator *hi, int what);
robj *hashTypeLookupWriteOrCreate(redisClient *c, robj *key);

/* Pub / Sub */
int pubsubUnsubscribeAllChannels(redisClient *c, int notify);
int pubsubUnsubscribeAllPatterns(redisClient *c, int notify);
void freePubsubPattern(void *p);
int listMatchPubsubPattern(void *a, void *b);
int pubsubPublishMessage(robj *channel, robj *message);

/* Keyspace events notification */
void notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);
int keyspaceEventsStringToFlags(char *classes);
sds keyspaceEventsFlagsToString(int flags);

/* Configuration */
void loadServerConfig(char *filename, char *options);
void appendServerSaveParams(time_t seconds, int changes);
void resetServerSaveParams();
struct rewriteConfigState; /* Forward declaration to export API. */
void rewriteConfigRewriteLine(struct rewriteConfigState *state, char *option, sds line, int force);
int rewriteConfig(char *path);

/* db.c -- Keyspace access API */
int removeExpire(redisDb *db, robj *key);
void propagateExpire(redisDb *db, robj *key);
int expireIfNeeded(redisDb *db, robj *key);
long long getExpire(redisDb *db, robj *key);
void setExpire(redisDb *db, robj *key, long long when);
robj *lookupKey(redisDb *db, robj *key);
robj *lookupKeyRead(redisDb *db, robj *key);
robj *lookupKeyWrite(redisDb *db, robj *key);
robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply);
robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply);
void dbAdd(redisDb *db, robj *key, robj *val);
void dbOverwrite(redisDb *db, robj *key, robj *val);
void setKey(redisDb *db, robj *key, robj *val);
int dbExists(redisDb *db, robj *key);
robj *dbRandomKey(redisDb *db);
int dbDelete(redisDb *db, robj *key);
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);
long long emptyDb(void(callback)(void*));
int selectDb(redisClient *c, int id);
void signalModifiedKey(redisDb *db, robj *key);
void signalFlushedDb(int dbid);
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count);
unsigned int countKeysInSlot(unsigned int hashslot);
unsigned int delKeysInSlot(unsigned int hashslot);
int verifyClusterConfigWithData(void);
void scanGenericCommand(redisClient *c, robj *o, unsigned long cursor);
int parseScanCursorOrReply(redisClient *c, robj *o, unsigned long *cursor);

/* API to get key arguments from commands */
int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
void getKeysFreeResult(int *result);
int *zunionInterGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys);
int *evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
int *sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);

/* Cluster */
void clusterInit(void);
unsigned short crc16(const char *buf, int len);
unsigned int keyHashSlot(char *key, int keylen);
void clusterCron(void);
void clusterPropagatePublish(robj *channel, robj *message);
void migrateCloseTimedoutSockets(void);
void clusterBeforeSleep(void);

/* Sentinel */
void initSentinelConfig(void);
void initSentinel(void);
void sentinelTimer(void);
char *sentinelHandleConfiguration(char **argv, int argc);
void sentinelIsRunning(void);

/* Scripting */
void scriptingInit(void);

/* Blocked clients */
void processUnblockedClients(void);
void blockClient(redisClient *c, int btype);
void unblockClient(redisClient *c);
void replyToBlockedClientTimedOut(redisClient *c);
int getTimeoutFromObjectOrReply(redisClient *c, robj *object, mstime_t *timeout, int unit);

/* Git SHA1 */
char *redisGitSHA1(void);
char *redisGitDirty(void);
uint64_t redisBuildId(void);

/* Commands prototypes */
void authCommand(redisClient *c);
void pingCommand(redisClient *c);
void echoCommand(redisClient *c);
void setCommand(redisClient *c);
void setnxCommand(redisClient *c);
void setexCommand(redisClient *c);
void psetexCommand(redisClient *c);
void getCommand(redisClient *c);
void delCommand(redisClient *c);
void existsCommand(redisClient *c);
void setbitCommand(redisClient *c);
void getbitCommand(redisClient *c);
void setrangeCommand(redisClient *c);
void getrangeCommand(redisClient *c);
void incrCommand(redisClient *c);
void decrCommand(redisClient *c);
void incrbyCommand(redisClient *c);
void decrbyCommand(redisClient *c);
void incrbyfloatCommand(redisClient *c);
void selectCommand(redisClient *c);
void randomkeyCommand(redisClient *c);
void keysCommand(redisClient *c);
void scanCommand(redisClient *c);
void dbsizeCommand(redisClient *c);
void lastsaveCommand(redisClient *c);
void saveCommand(redisClient *c);
void bgsaveCommand(redisClient *c);
void bgrewriteaofCommand(redisClient *c);
void shutdownCommand(redisClient *c);
void moveCommand(redisClient *c);
void renameCommand(redisClient *c);
void renamenxCommand(redisClient *c);
void lpushCommand(redisClient *c);
void rpushCommand(redisClient *c);
void lpushxCommand(redisClient *c);
void rpushxCommand(redisClient *c);
void linsertCommand(redisClient *c);
void lpopCommand(redisClient *c);
void rpopCommand(redisClient *c);
void llenCommand(redisClient *c);
void lindexCommand(redisClient *c);
void lrangeCommand(redisClient *c);
void ltrimCommand(redisClient *c);
void typeCommand(redisClient *c);
void lsetCommand(redisClient *c);
void saddCommand(redisClient *c);
void sremCommand(redisClient *c);
void smoveCommand(redisClient *c);
void sismemberCommand(redisClient *c);
void scardCommand(redisClient *c);
void spopCommand(redisClient *c);
void srandmemberCommand(redisClient *c);
void sinterCommand(redisClient *c);
void sinterstoreCommand(redisClient *c);
void sunionCommand(redisClient *c);
void sunionstoreCommand(redisClient *c);
void sdiffCommand(redisClient *c);
void sdiffstoreCommand(redisClient *c);
void sscanCommand(redisClient *c);
void syncCommand(redisClient *c);
void flushdbCommand(redisClient *c);
void flushallCommand(redisClient *c);
void sortCommand(redisClient *c);
void lremCommand(redisClient *c);
void rpoplpushCommand(redisClient *c);
void infoCommand(redisClient *c);
void mgetCommand(redisClient *c);
void monitorCommand(redisClient *c);
void expireCommand(redisClient *c);
void expireatCommand(redisClient *c);
void pexpireCommand(redisClient *c);
void pexpireatCommand(redisClient *c);
void getsetCommand(redisClient *c);
void ttlCommand(redisClient *c);
void pttlCommand(redisClient *c);
void persistCommand(redisClient *c);
void slaveofCommand(redisClient *c);
void debugCommand(redisClient *c);
void msetCommand(redisClient *c);
void msetnxCommand(redisClient *c);
void zaddCommand(redisClient *c);
void zincrbyCommand(redisClient *c);
void zrangeCommand(redisClient *c);
void zrangebyscoreCommand(redisClient *c);
void zrevrangebyscoreCommand(redisClient *c);
void zrangebylexCommand(redisClient *c);
void zrevrangebylexCommand(redisClient *c);
void zcountCommand(redisClient *c);
void zlexcountCommand(redisClient *c);
void zrevrangeCommand(redisClient *c);
void zcardCommand(redisClient *c);
void zremCommand(redisClient *c);
void zscoreCommand(redisClient *c);
void zremrangebyscoreCommand(redisClient *c);
void zremrangebylexCommand(redisClient *c);
void multiCommand(redisClient *c);
void execCommand(redisClient *c);
void discardCommand(redisClient *c);
void blpopCommand(redisClient *c);
void brpopCommand(redisClient *c);
void brpoplpushCommand(redisClient *c);
void appendCommand(redisClient *c);
void strlenCommand(redisClient *c);
void zrankCommand(redisClient *c);
void zrevrankCommand(redisClient *c);
void hsetCommand(redisClient *c);
void hsetnxCommand(redisClient *c);
void hgetCommand(redisClient *c);
void hmsetCommand(redisClient *c);
void hmgetCommand(redisClient *c);
void hdelCommand(redisClient *c);
void hlenCommand(redisClient *c);
void zremrangebyrankCommand(redisClient *c);
void zunionstoreCommand(redisClient *c);
void zinterstoreCommand(redisClient *c);
void zscanCommand(redisClient *c);
void hkeysCommand(redisClient *c);
void hvalsCommand(redisClient *c);
void hgetallCommand(redisClient *c);
void hexistsCommand(redisClient *c);
void hscanCommand(redisClient *c);
void configCommand(redisClient *c);
void hincrbyCommand(redisClient *c);
void hincrbyfloatCommand(redisClient *c);
void subscribeCommand(redisClient *c);
void unsubscribeCommand(redisClient *c);
void psubscribeCommand(redisClient *c);
void punsubscribeCommand(redisClient *c);
void publishCommand(redisClient *c);
void pubsubCommand(redisClient *c);
void watchCommand(redisClient *c);
void unwatchCommand(redisClient *c);
void clusterCommand(redisClient *c);
void restoreCommand(redisClient *c);
void migrateCommand(redisClient *c);
void askingCommand(redisClient *c);
void readonlyCommand(redisClient *c);
void readwriteCommand(redisClient *c);
void dumpCommand(redisClient *c);
void objectCommand(redisClient *c);
void clientCommand(redisClient *c);
void evalCommand(redisClient *c);
void evalShaCommand(redisClient *c);
void scriptCommand(redisClient *c);
void timeCommand(redisClient *c);
void bitopCommand(redisClient *c);
void bitcountCommand(redisClient *c);
void bitposCommand(redisClient *c);
void replconfCommand(redisClient *c);
void waitCommand(redisClient *c);
void pfselftestCommand(redisClient *c);
void pfaddCommand(redisClient *c);
void pfcountCommand(redisClient *c);
void pfmergeCommand(redisClient *c);
void pfdebugCommand(redisClient *c);

#if defined(__GNUC__)
void *calloc(size_t count, size_t size) __attribute__ ((deprecated));
void free(void *ptr) __attribute__ ((deprecated));
void *malloc(size_t size) __attribute__ ((deprecated));
void *realloc(void *ptr, size_t size) __attribute__ ((deprecated));
#endif

/* Debugging stuff */
void _redisAssertWithInfo(redisClient *c, robj *o, char *estr, char *file, int line);
void _redisAssert(char *estr, char *file, int line);
void _redisPanic(char *msg, char *file, int line);
void bugReportStart(void);
void redisLogObjectDebugInfo(robj *o);
void sigsegvHandler(int sig, siginfo_t *info, void *secret);
sds genRedisInfoString(char *section);
void enableWatchdog(int period);
void disableWatchdog(void);
void watchdogScheduleSignal(int period);
void redisLogHexDump(int level, char *descr, void *value, size_t len);

#define redisDebug(fmt, ...) \
    printf("DEBUG %s:%d > " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)
#define redisDebugMark() \
    printf("-- MARK %s:%d --\n", __FILE__, __LINE__)

#endif
