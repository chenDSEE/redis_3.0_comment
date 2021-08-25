/* Redis Sentinel implementation
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

// NOTE: 为了解决脑裂问题，民主式决策的系统一般都采用“投票节点数必须超过系统总节点数一半”规则来处理。
// NOTE: 多个独立的个体通过投票的方式来进行状态决策。其基础都是独立的个体之间交换信息，每个个体做出自己的决策，然后按照“多数取胜”的规则来确定最终的状态。
/* 主动轮询的异步执行结构
 *
 * 整个 sentinel 的核心是异步处理, 周期性处理任务
 * 所有的状态转换, 重试, reconnect 等等, 都是通过 频率 控制来做到的, 而不是定时器一般去做某件事
 * 所以, 每一次 cron 都会把所有函数, 所有 instance 都检查一次, 遍历一次
 * 根据 instance 内部记录的 now - last-do-time 跟配置的执行周期, 再加上当前 instance, 决定触发那个动作;
 * 
 * 另一个动作, 状态的触发源是: 异步发送的 CMD, 来了 reply, 根据 reply 的回复, 改变相应的 last-reply-time, instance 状态
 * 进而在下一次 cron 中, 满足某些状态的触发条件, 并执行相应的触发动作
 * 
 * sentinelRefreshInstanceInfo() + 状态机 switch-case + sentinel 发送异步 CMD + sentinel 定期发送 INFO 获取 slave\master 信息
 * 共同支撑了整个 sentinel 针对 instance 的状态轮转, 控制系统
 */

/* 实际上，sentinel 的收发模型，是采用 redis-cli 那一套的，比较麻烦
 * sentinel 填一次 CMD 的 callback，然后在 async.c 里面，还有一个 read-able\write-able 的 callback
 * 这样才得已实现一套完整的异步收发方案，具体流程如下：
 * 1. sentinel 发起 CMD，并提供当这个 CMD 被执行完之后的相应 callback
 * 2. sentinel 通过 redisAsyncCommand 的 api，将 CMD 的具体内容写入 redisContext 的 buf 里面；
 * 3. 当 sentinel 的 read-able\write-able 触发之后，开始调用 async.c 里面的 read_callback\write_callback
 *    将相应的内容在 fd 跟 buf 之间相互搬运（实际上吧，api 里面还有一层 buf 的，甚至操作系统也有一层 buf）
 *   （毕竟，Linux\libc 提供的 io api，都是各自带有一层 buf 的，这样的为了在用户程序中实现异步 io 的方案，我也不确定是否高效）
 * 
 * ..... date\CMD send to redis-server .....
 * ..... date\CMD reply to sentinel .....
 *
 * 4. read-able，然后触发 read_callback，将 reply 从 fd 中搬到 buf
 * 5. 然后利用 step 1 里面注册的 callback 来完成异步过程的最后一步
 */
// TODO: 针对下面三种分布式信息思考：是否成功达成分布式共识？什么时候会失败？为什么能达成共识？遇上失效节点、恶意节点会发生什么？跟我们部署 sentinel 的数量、法定认数的设置有什么关系？一致性是怎么达到的？
// 无论是 leader sentinel 的选举，还是 master-redis-server ODOWN 的认定，都需要超过 quorum 的法定人数同意才可以
// 1. SDOWN --> ODOWN 是中心化的，以发现被监控的 master 长时间失联，的那个 sentinel 作为中心，分别向其他 sentinel 询问
//    最终当前 sentinel 要知晓超过 quorum 认为 SDOWN 之后，才设置 ODOWN；不需要全部 sentinel 最终一致认为 master SDOWN\ODOWN

// 2. master\sentinel\slave 之间的相互发现也是中心化的，以 master 为中心，无论是 slave 还是 sentinel 都是连接向 master，向 master 发送信息
//    然后由 master 来广播出去；通过 master 的不停广播、sentinel 的 hello，最终每一个 sentinel 都会知晓整个网络环境的 instance 情况

// 3. leader sentinel 是去中心化的选举。但是又要保证最终全局只会有一个 leader sentinel。
//    每一个 sentinel 都可以作为候选人，并发起选举

// TODO:(DONE) 怎么确保其他 sentinel 不会在 failover 的过程中, 错误启动新一轮的选举?
//      在 failover 操作成功之后，将会把这个消息广播出去的
// TODO:(DONE) 万一 failover 刚完成，其他 sentinel 就 timeout 了，会发生什么？
//      其实关系不大，redis 的实现，容错性挺强的。
//      核心：leader sentinel 自己检查 failover 是不是超时了，超时了自己 abort 就好，也不需要通知啥的
//            有投票请求，那就照旧投票，然后新 leader 执行 failover，最后会向每一个 master\slave instance 发送 SLAVEOF 的
//            确保全部面向同一个 master，包括曾经操作失败的那个

/* instance(sentinel\master\slave) 之间的联系建立过程：
 * 1. sentinel <---> master:
 *    这个是直接读 sentinel.conf 的，再不然就是在 redis-cli 里面直接命令的
 *    当 sentinel 会同时向 master 发起两个连接，一个用于 CMD 的发送；另一个用于 hello 广播的接收
 * 2. sentinel <---> slave:
 *    当 sentinel 成功跟 master 连接之后，会定期向 master 发送 INFO 的 CMD，然后解析相应的 reply
 *    进而获得这个 master 所有的 slave 信息，有新 slave 加入的话，也会即使建立连接
 *    同样，也是会建立 CMD、hello 两个连接
 * 3. sentinel <---> sentinel:
 *    一个 sentinel 主动向 master\slave 发送 hello 信息，然后通过 master\slave 将自身的信息广播出去
 *    因为每一个 sentinel 必然会订阅这个 redis 集群中的其中一个 master\slave 的 hello 通道，
 *    经过若干的传播，最终一定能够获得新 sentinel 的 hello 的，然后根据 hello 中的信息，建立连接
 *    只建立一个 CMD 的连接，用来进行 PING\PONG，投票选举呀之类的
 */

/* failover 过程：
 * 总流程：SDOWN ---> 搜集 SRI_MASTER_DOWN ---> 确认 ODOWN ---> leader sentinel ---> failover
 * ================= 各个 sentinel 发现 SDOWN 并尝试确认 ODOWN =======================
 * 1. sentinel 发现 master SDOWN（master 很久都没有回复 PING、CMD 了）;
 * 2. 该 sentinel 通过 SENTINEL is-master-down-by-addr 的 CMD 方式，
 *    一个个单独问其他 sentinel 是否也觉得这个 master 也是 SDOWN 的
 *   （不带 runid，因为这是 ODOWN 的确认，并不是 leader sentinel 的选举）
 * 3. 当该 sentinel 收到其他的 sentinel 回复超过 <quorum> 的数量是，就可以认为这个 master 是 ODOWN
 * 
 * note: 每一个 sentinel 一旦发现了 SDOWN 都会主动向其他 sentinel 确认 ODOWN
 * 
 * ================= leader sentinel 选举 =======================
 * 4. 某一个确认了 ODOWN 的 sentinel 通过 SENTINEL is-master-down-by-addr + current-sentinel-runid 的方式，
 *    发起了一场 leader sentinel 的选举，并且要求其他 sentinel 投自己一票，并且汇报：你这个 sentinel 最终投了谁一票
 * 5. 一旦 sentinel 开始尝试竞选 leader，就会为 master 设置 SENTINEL_FAILOVER_STATE_SELECT_SLAVE 的标识
 *    master 就开始等候 leader master 选举的完成
 * 6. 发起了 leader 选举的 sentinel 会定期检查跟自己连接的 sentinel 的投票结果，
 *    一旦满足：过半数 sentinel 投票 + 达到 quorum 的投票数，那就可以认定自己是 leader 了
 *    无需主动告知其他 sentinel 选举的结果；
 *    Error handle：
 *    一旦这个 leader 在 time 之内没有完成 failover 让其他 sentinel reset timer，
 *    自己只需要直接中断本次 failover 过程即可（由于每个 sentinel 都有 failover 执行的频率限制，所以这个失败的 leader 不会马上再次发起选举并当选）
 *    其他 sentinel 就会主动发起新一轮选举，尝试执行 failover；
 * 7. 成功选举成为 leader 的 sentinel，会开始设置相应的旧 master failover_state 为 SENTINEL_FAILOVER_STATE_SELECT_SLAVE
 *    准备选出新的 master（promote slave）
 * 
 * note: 每一个确认了 ODOWN 的 sentinel，都有资格，且都想成为 leader；
 *       但是在确认 ODOWN 之前，这个 sentinel 已经 vote for 其他 sentinel 了的话
 *       需要等待这一轮 failover 超时之后，才能发起新一轮选举，竞选成为 leader sentinel
 * 
 * ================= failover 状态切换 =======================
 * 整个 failover 将会根据旧 master 上面的状态来进行：
 * #define SENTINEL_FAILOVER_STATE_NONE
 * #define SENTINEL_FAILOVER_STATE_WAIT_START
 * #define SENTINEL_FAILOVER_STATE_SELECT_SLAVE
 * #define SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE
 * #define SENTINEL_FAILOVER_STATE_WAIT_PROMOTION
 * #define SENTINEL_FAILOVER_STATE_RECONF_SLAVES
 * #define SENTINEL_FAILOVER_STATE_UPDATE_CONFIG
 * 
 * #define SRI_MASTER  (1<<0)
 * #define SRI_SLAVE   (1<<1)
 * #define SRI_FAILOVER_IN_PROGRESS (1<<7)
 * #define SRI_PROMOTED (1<<8)
 * 
 * 被挑选出来即将成为 master 的 slave 仍然是一个 SRI_SLAVE，但是会加上 SRI_PROMOTED 的 flag
 * 即将被 down 掉的旧 master 仍然是一个 SRI_MASTER，但是会被加上 SRI_FAILOVER_IN_PROGRESS 的标识
 *
 * 8. 由于 sentinel 确认了 master ODOWN 之后，就会增加发送 INFO CMD 的频率，所以能够更快的获取各个 slave 的状态信息；
 * 9. 在不停异步 INFO 收集 slave 状态信息的同时，leader sentinel 在 sentinelSelectSlave() 定期检查：
 *    是不是有一个或多个 slave 能够满足被 promoted 的基本条件，是的话选择跟 master 内容最相近的那个 slave，
 *    并将 flag 设置为 SRI_PROMOTED；状态转换为 SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE
 * 10.leader sentinel 针对 promoted_slave 发送 SLAVEOF no one CMD，让它成为 master，不再是一个 slave；
 *    设置 SENTINEL_FAILOVER_STATE_WAIT_PROMOTION，开始等待 promoted_slave 执行 CMD
 * 11.leader sentinel 定期通过 INFO 了解 promoted_slave 是否成功执行 SLAVEOF no one CMD，是否成功
 *    一旦发现成功了，那就直接 SENTINEL_FAILOVER_STATE_RECONF_SLAVES
 * 12.leader sentinel 根据以前的记录，向以前的 slave 发送 slave of promoted_slave 的 CMD，让所有的 slave 转向
 *    promoted_slave 并开始进行 SYNC\PSYNC；（为了防止网络瞬间流量激增，将会限制同一时间内进行 SYNC 的 slave 数量）
 * 13.leader sentinel 通过 INFO CMD 的方式，检查是不是全部还活跃的 slave 都已经转向了 promoted_slave
 *    是的话，那就进入最后的阶段 SENTINEL_FAILOVER_STATE_UPDATE_CONFIG
 * 14.这个阶段之后，将不需要在进入状态机轮转了，毕竟这是最后一个阶段了
 *    slave 将会被正式视为 SRI_MASTER，SRI_FAILOVER_IN_PROGRESS 也会被取下；
 *    sentinel->master 的信息也会被更新的新的 master，所有连接也会被 reset
 * 15.因为 sentinel->master 的改变，将会导致这个 leader sentinel 的 hello 信息发生变化，
 *    然后其他 sentinel 也会因为 hello 广播的缘故，看到 leader sentinel 已经成功转向了新的 master
 *    自然而然地，这个 sentinel 也就取消 failover_start_time 的超时计时
 * 16.至此，整个 failover 过程结束了
 *
 * Timeout Error Handle:
 * 如果超过了一定时间，leader sentinel 都没有办法完成整个 failover过程的话，
 * 其他 sentinel 将会发生 failover_start_time 超时，
 * 然后会发起新的一轮选举，更新 epoch，选出一个新的 leader sentinel，再次进行 promoted_slave 选择；
 * 而原本的 leader sentinel 也会因为超时的缘故，自己 abort failover 操作
 *
 * TODO:(DONE) 完成了的话，怎么告知其他 sentinel 呢？ step 15，通过 hello 广播告知；下面也有更详细的，这是一个异步的设计
 */

/* leader sentinel 以及 failover 的超时保证机制：
 * 1. 每一个向其他 sentinel 确认过 master ODOWN 的 sentinel 都有资格成为 leader sentinel, 都有资格要求其他 sentinel 进行投票
 * 2. 每一个 sentinel 都会记录一个 failover_start_time; 
 *    主动竞选的, 会在自己要求别人投票的时候, 更新这个时间;
 *    被动要求投票的, 将会在自己投票的那一瞬间, 记录这个时间;
 * 3. 每个确认过 ODOWN 的 sentinel, 都有资格进入这个函数, 并在这个函数里面, 看看距离 failover_start_time 很久了
 *    是的话, 将会发起新一轮选举(因为都这么久了, 有理由怀疑上一次的选举失败了)
 * 4. 当 leader sentinel 成功完成 failover 的全过程之后, 会更新 leader sentinel 里面的 master SRI 信息,
 *    然后向外发送新的 hello 信息, 引发其他 sentinel 更新 master 信息, 进而 reset 掉 ODOWN 状态, failover_start_time
 *    从而将重试的准备停止掉
 */

#include "redis.h"
#include "hiredis.h"
#include "async.h"

#include <ctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>

extern char **environ;

// sentinel 的默认端口号
#define REDIS_SENTINEL_PORT 26379

/* ======================== Sentinel global state =========================== */

/* Address object, used to describe an ip:port pair. */
/* 地址对象，用于保存 IP 地址和端口 */
typedef struct sentinelAddr {
    char *ip;
    int port;
} sentinelAddr;

// NOTE: instance 是指 sentinel 里面跟 master\slave\sentinel 的本体；sentinel 内部是采用 RSI 来进行管理连接的
/* A Sentinel Redis Instance object is monitoring. */
/* 每个被监视的 Redis  instance 都会创建一个 sentinelRedisInstance 结构
 * 而每个结构的 flags 值会是以下常量的一个或多个的并 */
// 每一个正在监控、正打算监控、想要监控的 redis-server(role master)  instance ，当前的状态
//  instance 是一个 master 
#define SRI_MASTER  (1<<0)
//  instance 是一个 slave 
#define SRI_SLAVE   (1<<1)
//  instance 是一个 Sentinel
#define SRI_SENTINEL (1<<2)
//  instance 已断线
#define SRI_DISCONNECTED (1<<3)
//  instance 已处于 SDOWN 状态
#define SRI_S_DOWN (1<<4)   /* Subjectively down (no quorum). */
//  instance 已处于 ODOWN 状态
#define SRI_O_DOWN (1<<5)   /* Objectively down (confirmed by others). */
// 其他 Sentinel 认为 master 已下线
#define SRI_MASTER_DOWN (1<<6) /* A Sentinel with this flag set thinks that
                                   its master is down. */
// 正在对 master 进行故障迁移
#define SRI_FAILOVER_IN_PROGRESS (1<<7) /* Failover is in progress for
                                           this master. */
//  instance 是被选中的新 master （目前仍是 slave ）
#define SRI_PROMOTED (1<<8)            /* Slave selected for promotion. */
// 向 slave 发送 SLAVEOF 命令，让它们转向复制新 master 
#define SRI_RECONF_SENT (1<<9)     /* SLAVEOF <newmaster> sent. */
//  slave 正在与新 master 进行同步
#define SRI_RECONF_INPROG (1<<10)   /* Slave synchronization in progress. */
//  slave 与新 master 同步完毕，开始复制新 master 
#define SRI_RECONF_DONE (1<<11)     /* Slave synchronized with new master. */
// 对 master 强制执行故障迁移操作
#define SRI_FORCE_FAILOVER (1<<12)  /* Force failover with master up. */
// 已经对返回 -BUSY 的服务器发送 SCRIPT KILL 命令
#define SRI_SCRIPT_KILL_SENT (1<<13) /* SCRIPT KILL already sent on -BUSY */

/* Note: times are in milliseconds. */
/* 各种时间常量，以毫秒为单位 */
// 发送 INFO 命令的间隔
#define SENTINEL_INFO_PERIOD 10000
// 发送 PING 命令的间隔
#define SENTINEL_PING_PERIOD 1000
// 发送 ASK 命令的间隔
#define SENTINEL_ASK_PERIOD 1000
// 发送 PUBLISH 命令的间隔
#define SENTINEL_PUBLISH_PERIOD 2000
// 默认的判断服务器已下线的时长
#define SENTINEL_DEFAULT_DOWN_AFTER 30000
// 默认的信息频道
#define SENTINEL_HELLO_CHANNEL "__sentinel__:hello"
// 默认的 TILT 触发时长
#define SENTINEL_TILT_TRIGGER 2000
// 默认的 TILT 环境时长（要多久才能退出 TITL 模式）
#define SENTINEL_TILT_PERIOD (SENTINEL_PING_PERIOD*30)
// 默认 slave 优先级
#define SENTINEL_DEFAULT_SLAVE_PRIORITY 100
#define SENTINEL_SLAVE_RECONF_TIMEOUT 10000
// 默认的同时对新 master 进行复制的 slave 个数
#define SENTINEL_DEFAULT_PARALLEL_SYNCS 1
// 默认的最少重连接间隔
#define SENTINEL_MIN_LINK_RECONNECT_PERIOD 15000
// 默认的故障迁移执行时长
#define SENTINEL_DEFAULT_FAILOVER_TIMEOUT (60*3*1000)
// 默认的最大积压命令数量
#define SENTINEL_MAX_PENDING_COMMANDS 100
// 默认的选举超时时长
#define SENTINEL_ELECTION_TIMEOUT 10000
#define SENTINEL_MAX_DESYNC 1000

/* Failover machine different states. */
/* 故障转移时的状态 */
// 没在执行故障迁移
#define SENTINEL_FAILOVER_STATE_NONE 0  /* No failover in progress. */
// 正在等待开始故障迁移
#define SENTINEL_FAILOVER_STATE_WAIT_START 1  /* Wait for failover_start_time*/ 
// 正在挑选作为新 master 的 slave 
#define SENTINEL_FAILOVER_STATE_SELECT_SLAVE 2 /* Select slave to promote */
// 向被选中的 slave 发送 SLAVEOF no one
#define SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE 3 /* Slave -> Master */
// 等待 slave 转变成 master  
#define SENTINEL_FAILOVER_STATE_WAIT_PROMOTION 4 /* Wait slave to change role */
// 向已下线 master 的其他 slave 发送 SLAVEOF 命令
// 让它们复制新的 master 
#define SENTINEL_FAILOVER_STATE_RECONF_SLAVES 5 /* SLAVEOF newmaster */
// 监视被升级的 slave 
#define SENTINEL_FAILOVER_STATE_UPDATE_CONFIG 6 /* Monitor promoted slave. */

/* 主 slave 之间的连接状态 */
// 连接正常
#define SENTINEL_MASTER_LINK_STATUS_UP 0
// 连接断开
#define SENTINEL_MASTER_LINK_STATUS_DOWN 1

/* Generic flags that can be used with different functions.
 * They use higher bits to avoid colliding with the function specific
 * flags. */
/* 可以用于多个函数的通用标识。
 * 使用高位来避免与一般标识冲突。 */
// 没有标识
#define SENTINEL_NO_FLAGS 0
// 生成事件
#define SENTINEL_GENERATE_EVENT (1<<16)
// 领头
#define SENTINEL_LEADER (1<<17)
// 观察者
#define SENTINEL_OBSERVER (1<<18)

/* Script execution flags and limits. */
/* 脚本执行状态和限制 */
// 脚本目前没有被执行
#define SENTINEL_SCRIPT_NONE 0
// 脚本正在执行
#define SENTINEL_SCRIPT_RUNNING 1
// 脚本队列保存脚本数量的最大值
#define SENTINEL_SCRIPT_MAX_QUEUE 256
// 同一时间可执行脚本的最大数量
#define SENTINEL_SCRIPT_MAX_RUNNING 16
// 脚本的最大执行时长
#define SENTINEL_SCRIPT_MAX_RUNTIME 60000 /* 60 seconds max exec time. */
// 脚本最大重试数量
#define SENTINEL_SCRIPT_MAX_RETRY 10
// 脚本重试之前的延迟时间
#define SENTINEL_SCRIPT_RETRY_DELAY 30000 /* 30 seconds between retries. */

// Sentinel 会为每个被监视的 Redis  instance 创建相应的 sentinelRedisInstance  instance 
// （被监视的 instance 可以是 master 、 slave 、或者其他 Sentinel ）
typedef struct sentinelRedisInstance {
    
    // 标识值，记录了 instance 的类型，以及该 instance 的当前状态
    int flags;      /* See SRI_... defines */
    
    //  instance 的名字
    //  master 的名字由用户在配置文件中设置
    //  slave 以及 Sentinel 的名字由 Sentinel 自动设置
    // 格式为 ip:port ，例如 "127.0.0.1:26379"
    char *name;     /* Master name from the point of view of this sentinel. */

    //  instance 的运行 ID
    char *runid;    /* run ID of this instance. */

    // 配置纪元，用于实现故障转移
    uint64_t config_epoch;  /* Configuration epoch. */

    //  instance 的地址
    sentinelAddr *addr; /* Master host. */

    // 用于发送命令的异步连接
    // NOTE: 这才是 sentinel 时，sentinel 跟其他模块的网络连接实体！
    // 连接调用链：现在 createSentinelRedisInstance() 里面进行初始化，并且设置为 SRI_DISCONNECTED
    // 然后将会由 timer 进行采用异步的方式，真正建立连接：
    // sentinelTimer() --> sentinelHandleRedisInstance() --> sentinelReconnectInstance()
    redisAsyncContext *cc; /* Hiredis context for commands. */

    // 用于执行 SUBSCRIBE 命令、接收频道信息的异步连接
    // TODO:(DONE) 仅在 instance 为 master 时使用？订阅之后能收到什么信息？收到之后又会做什么？
    // sentinel 用来监听 master\slave 在 hello channel 上面广播的 hello 消息，以便发现新的 sentinel，以及更新 master 信息
    // pubsub context
    // 只会从 master\slave 的 redis-server 身上订阅，sentinel 并不订阅
    redisAsyncContext *pc; /* Hiredis context for Pub / Sub. */
/* TODO:(DONE) 为什么需要两个 redisAsyncContext 呢？
 * cc（command context）是 sentinel 向这个被监控的 master\slave redis-server 发送 CMD，作为控制通道来使用的
 * pc（pubsub context）是 sentinel 用来监听\订阅其他 master\slave\sentinel 的广播通知的
 *                    因为你实际用一下 pubsub 的 CMD 就知道了，这个模式下的 redis-cli 实际上是整个被占用的，
 *                    不能够再进行任何的 CMD 操作了（也不适合再进行这些操作了）
 * 
 * 哨兵会与其监控的所有主节点、该主节点下属的所有从节点，以及与之监控相同主节点的其他哨兵之间建立TCP连接。
 * 哨兵与主节点和从节点之间会建立两个TCP连接，分别用于发送命令和订阅HELLO频道；
 * 哨兵与其他哨兵之间只建立一个发送命令的TCP连接（因为哨兵本身不支持订阅模式）；
 */


    // 已发送但尚未回复的命令数量
    int pending_commands;   /* Number of commands sent waiting for a reply. */

    // cc 连接的创建时间
    mstime_t cc_conn_time; /* cc connection time. */
    
    // pc 连接的创建时间
    mstime_t pc_conn_time; /* pc connection time. */

    // 最后一次从这个 instance 接收信息的时间
    mstime_t pc_last_activity; /* Last time we received any message. */

    //  instance 最后一次返回正确的 PING 命令回复的时间 (从 sentinel 的角度上观察而来)
    mstime_t last_avail_time; /* Last time the instance replied to ping with
                                 a reply we consider valid. */
    //  instance 最后一次发送 PING 命令的时间
    mstime_t last_ping_time;  /* Last time a pending ping was sent in the
                                 context of the current command connection
                                 with the instance. 0 if still not sent or
                                 if pong already received. */
    //  instance 最后一次返回 PING 命令的时间，无论内容正确与否
    mstime_t last_pong_time;  /* Last time the instance replied to ping,
                                 whatever the reply was. That's used to check
                                 if the link is idle and must be reconnected. */

    // 最后一次向频道发送问候信息的时间
    // 只在当前 instance 为 sentinel 时使用
    mstime_t last_pub_time;   /* Last time we sent hello via Pub/Sub. */

    // 最后一次接收到这个 sentinel 发来的问候信息的时间
    // 只在当前 instance 为 sentinel 时使用
    mstime_t last_hello_time; /* Only used if SRI_SENTINEL is set. Last time
                                 we received a hello from this Sentinel
                                 via Pub/Sub. */

    // 最后一次回复 SENTINEL is-master-down-by-addr 命令的时间
    // 只在当前 instance 为 sentinel 时使用
    mstime_t last_master_down_reply_time; /* Time of last reply to
                                             SENTINEL is-master-down command. */

    //  instance 被判断为 SDOWN 状态的时间
    mstime_t s_down_since_time; /* Subjectively down since time. */

    //  instance 被判断为 ODOWN 状态的时间
    mstime_t o_down_since_time; /* Objectively down since time. */

    // SENTINEL down-after-milliseconds 选项所设定的值，down-after-milliseconds <master-name> <milliseconds>
    //  instance 无响应多少毫秒之后才会被判断为主观下线（subjectively down）
    mstime_t down_after_period; /* Consider it down after that period. */

    // 从 instance 获取 INFO 命令的回复的时间
    mstime_t info_refresh;  /* Time at which we received INFO output from it. */

    /* Role and the first time we observed it.
     * This is useful in order to delay replacing what the instance reports
     * with our own configuration. We need to always wait some time in order
     * to give a chance to the leader to report the new configuration before
     * we do silly things. */
    //  instance 的角色
    int role_reported;
    // 角色的更新时间
    mstime_t role_reported_time;

    // 最后一次 slave 的 master 地址变更的时间
    mstime_t slave_conf_change_time; /* Last time slave master addr changed. */

    /* Master RSI specific in sentinel mode redis-server */
    /*  master 对应的 RSI 特有的属性 -------------------------------------------------------------*/

    // 其他同样监控这个 master 的所有 sentinel
    // 每一套不同的 redis 主从集群是隔离开来的（key --> master, value --> all_sentinel_for_this_master）
    dict *sentinels;    /* Other sentinels monitoring the same master. */

    // 如果这个 RSI 对应的是一个 master redis-server
    // 那么这个字典保存着属于 master 的所有 slave RSI 信息
    // 字典的键是 slave 的名字，字典的值是 slave 对应的 sentinelRedisInstance 结构
    // 这样 sentinel 才能对 RSI 做到去重
    dict *slaves;       /* Slaves for this master instance. */

    // SENTINEL monitor <master-name> <IP> <port> <quorum> 选项中的 quorum 参数
    // 判断这个 instance 为客观下线（objectively down）所需的支持投票数量
    int quorum;         /* Number of sentinels that need to agree on failure. */

    // SENTINEL parallel-syncs <master-name> <number> 选项的值
    // 在执行故障转移操作时，可以同时对新的 master 进行同步的 slave 数量
    int parallel_syncs; /* How many slaves to reconfigure at same time. */

    // 连接 master 和 slave 所需的密码
    char *auth_pass;    /* Password to use for AUTH against master & slaves. */

    /* Slave specific. */
    /*  slave  instance 特有的属性 -------------------------------------------------------------*/

    // master <---> slave 连接断开的时间(通过 slave 发送的 info CMD 知晓)
    mstime_t master_link_down_time; /* Slave replication link down time. */

    //  slave 优先级
    // It is used by Redis Sentinel in order to select a slave to promote into
    // a master if the master is no longer working correctly.
    int slave_priority; /* Slave priority according to its INFO output. */

    // 执行故障转移操作时， slave 发送 SLAVEOF <new-master> 命令的时间
    mstime_t slave_reconf_sent_time; /* Time at which we sent SLAVE OF <new> */

    //  master 的 instance （在本 RSI 的 对端为 slave 时使用）
    struct sentinelRedisInstance *master; /* Master instance if it's slave. */

    // INFO 命令的回复中记录的 master  IP
    char *slave_master_host;    /* Master host as reported by INFO */
    
    // INFO 命令的回复中记录的 master 端口号
    int slave_master_port;      /* Master port as reported by INFO */

    // INFO 命令的回复中记录的主 slave 连接状态
    int slave_master_link_status; /* Master link status as reported by INFO */

    //  slave 的复制偏移量
    unsigned long long slave_repl_offset; /* Slave replication offset. */

    /* Failover */
    /* 故障转移相关属性 -------------------------------------------------------------------*/


    // 如果这是一个 master  instance ，那么 leader 将是负责进行故障转移的 Sentinel 的运行 ID 。
    // 如果这是一个 Sentinel  instance ，那么 leader 就是被选举出来的 leader Sentinel 。
    // 这个域只在 Sentinel  instance 的 flags 属性的 SRI_MASTER_DOWN 标志处于打开状态时才有效。
    char *leader;       /* If this is a master instance, this is the runid of
                           the Sentinel that should perform the failover. If
                           this is a Sentinel, this is the runid of the Sentinel
                           that this Sentinel voted as leader. */
    //  leader sentinel 的纪元
    uint64_t leader_epoch; /* Epoch of the 'leader' field. */

    // 当前执行中的故障转移的纪元
    uint64_t failover_epoch; /* Epoch of the currently started failover. */
    // 故障转移操作的当前状态
    int failover_state; /* See SENTINEL_FAILOVER_STATE_* defines. */

    // 状态改变的时间
    mstime_t failover_state_change_time;

    // 最后一次进行故障迁移的时间
    mstime_t failover_start_time;   /* Last failover attempt start time. */

    // SENTINEL failover-timeout <master-name> <ms> 选项的值
    // 刷新故障迁移状态的最大时限
    mstime_t failover_timeout;      /* Max time to refresh failover state. */

    mstime_t failover_delay_logged; /* For what failover_start_time value we
                                       logged the failover delay. */
    // 指向被提升为新 master 的 slave 的指针, 将会在旧 master 的 sentinelRedisInstance 里面记录(刚好一一对应嘛)
    struct sentinelRedisInstance *promoted_slave; /* Promoted slave instance. */

    /* Scripts executed to notify admin or reconfigure clients: when they
     * are set to NULL no script is executed. */
    // 一个文件路径，保存着 WARNING 级别的事件发生时执行的，
    // 用于通知管理员的脚本的地址
    char *notification_script;

    // 一个文件路径，保存着故障转移执行之前、之后、或者被中止时，
    // 需要执行的脚本的地址
    char *client_reconfig_script;

} sentinelRedisInstance;

/* Main state. */
/* Sentinel 的状态结构 */
struct sentinelState {

    // 当前纪元 TODO:（DONE） 相当于版本号？no，epoch 代表的意思是：当前的投票选举是第几轮
    /* 所谓的选举纪元，直白的解释就是 "第几届选举"。当哨兵进程启动时，其选举纪元就被初始化，默认的初始化值为0，
     * 在同一届选举（同一个选举纪元的值）中，每个哨兵只会投一次票。因此，在一界选举中，只可能有一个哨兵能获得超过半数的投票，从而赢得选举。
     *
     * 当哨兵A发现某个主节点客观下线后，它就会发起新一届的选举。第一件事就是将本地的选举纪元加1，
     * 这个加1的意思，实际上就是表示“发起新一届选举”。之后，哨兵A就会向其他哨兵发送”is-master-down-by-addr”命令，
     * 用于拉票，其中就包含了A的选举纪元。
     */
    // TODO:(DONE) 不针对不同的 master 分别记录吗？（对）
    uint64_t current_epoch;     /* Current epoch. */

    // 保存了所有被这个 sentinel 监视的 master (TODO:(DONE) master ? 还是 redis-server ? 就是作为 master 角色的 redis-server)
    /* TODO:（DONE） master 不是只有一台吗？为什么要用 dict 呢？
    /* 因为 sentinel 本身是不跟 master 的 redis-server 强耦合的
     * 你一个 sentinel 可以监控多个不同的 redis-server 集群中的 master 呀
     * 而且在切片模式下，master 确实就成了多个呀。所以用 dict 来记录作为 master 的 redis-server 这并没有毛病
     *  不要有一个 sentinel 一定要跟一个 sentinel 部署在一起的，虽然是经典部署方式，但不是只能这样的
     */
    // 字典的键是 master 的名字
    // 字典的值则是一个指向 sentinelRedisInstance 结构的指针(也就是 sentinel 上面管理的一个对端)
    dict *masters;      /* Dictionary of master sentinelRedisInstances.
                           Key is the instance name, value is the
                           sentinelRedisInstance structure pointer. */

    // 是否进入了 TILT 模式？
    int tilt;           /* Are we in TILT mode? */

    // 目前正在执行的脚本的数量
    int running_scripts;    /* Number of scripts in execution right now. */

    // 进入 TILT 模式的时间
    mstime_t tilt_start_time;   /* When TITL started. */

    // 最后一次执行时间处理器的时间
    mstime_t previous_time;     /* Last time we ran the time handler. */

    // 一个 FIFO 队列，包含了所有需要执行的用户脚本
    list *scripts_queue;    /* Queue of user scripts to execute. */

} sentinel;

/* A script execution job. */
// 脚本运行状态
typedef struct sentinelScriptJob {

    // 标志，记录了脚本是否运行
    int flags;              /* Script job flags: SENTINEL_SCRIPT_* */

    // 该脚本的已尝试执行次数
    int retry_num;          /* Number of times we tried to execute it. */

    // 要传给脚本的参数
    char **argv;            /* Arguments to call the script. */

    // 开始运行脚本的时间
    mstime_t start_time;    /* Script execution time if the script is running,
                               otherwise 0 if we are allowed to retry the
                               execution at any time. If the script is not
                               running and it's not 0, it means: do not run
                               before the specified time. */

    // 脚本由子进程执行，该属性记录子进程的 pid
    pid_t pid;              /* Script execution pid. */

} sentinelScriptJob;

/* ======================= hiredis ae.c adapters =============================
 * Note: this implementation is taken from hiredis/adapters/ae.h, however
 * we have our modified copy for Sentinel in order to use our allocator
 * and to have full control over how the adapter works. */

// 客户端适配器（adapter）结构
typedef struct redisAeEvents {

    // 客户端连接上下文
    redisAsyncContext *context;

    // 服务器的事件循环
    aeEventLoop *loop;

    // 套接字
    int fd;

    // 记录读事件以及写事件是否就绪
    int reading, writing;

} redisAeEvents;

// 读事件处理器 
static void redisAeReadEvent(aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void)el); ((void)fd); ((void)mask);

    redisAeEvents *e = (redisAeEvents*)privdata;
    // 从连接中进行读取
    redisAsyncHandleRead(e->context);
}

// 写事件处理器
static void redisAeWriteEvent(aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void)el); ((void)fd); ((void)mask);

    redisAeEvents *e = (redisAeEvents*)privdata;
    // 从连接中进行写入
    redisAsyncHandleWrite(e->context);
}

// 将读事件处理器安装到事件循环中
static void redisAeAddRead(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    // 如果读事件处理器未安装，那么进行安装
    if (!e->reading) {
        e->reading = 1;
        aeCreateFileEvent(loop,e->fd,AE_READABLE,redisAeReadEvent,e);
    }
}

// 从事件循环中删除读事件处理器
static void redisAeDelRead(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    // 仅在读事件处理器已安装的情况下进行删除
    if (e->reading) {
        e->reading = 0;
        aeDeleteFileEvent(loop,e->fd,AE_READABLE);
    }
}

// 将写事件处理器安装到事件循环中
static void redisAeAddWrite(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    if (!e->writing) {
        e->writing = 1;
        aeCreateFileEvent(loop,e->fd,AE_WRITABLE,redisAeWriteEvent,e);
    }
}

// 从事件循环中删除写事件处理器
static void redisAeDelWrite(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    if (e->writing) {
        e->writing = 0;
        aeDeleteFileEvent(loop,e->fd,AE_WRITABLE);
    }
}

// 清理事件
static void redisAeCleanup(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    redisAeDelRead(privdata);
    redisAeDelWrite(privdata);
    zfree(e);
}

// 为上下文 ae 和事件循环 loop 创建 hiredis 适配器
// 并设置相关的异步处理函数
static int redisAeAttach(aeEventLoop *loop, redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    redisAeEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return REDIS_ERR;

    /* Create container for context and r/w events */
    // 创建适配器
    e = (redisAeEvents*)zmalloc(sizeof(*e));
    e->context = ac;
    e->loop = loop;
    e->fd = c->fd;
    e->reading = e->writing = 0;

    /* Register functions to start/stop listening for events */
    // 设置异步调用函数
    ac->ev.addRead = redisAeAddRead;
    ac->ev.delRead = redisAeDelRead;
    ac->ev.addWrite = redisAeAddWrite;
    ac->ev.delWrite = redisAeDelWrite;
    ac->ev.cleanup = redisAeCleanup;
    ac->ev.data = e;

    return REDIS_OK;
}

/* ============================= Prototypes ================================= */

void sentinelLinkEstablishedCallback(const redisAsyncContext *c, int status);
void sentinelDisconnectCallback(const redisAsyncContext *c, int status);
void sentinelReceiveHelloMessages(redisAsyncContext *c, void *reply, void *privdata);
sentinelRedisInstance *sentinelGetMasterByName(char *name);
char *sentinelGetSubjectiveLeader(sentinelRedisInstance *master);
char *sentinelGetObjectiveLeader(sentinelRedisInstance *master);
int yesnotoi(char *s);
void sentinelDisconnectInstanceFromContext(const redisAsyncContext *c);
void sentinelKillLink(sentinelRedisInstance *ri, redisAsyncContext *c);
const char *sentinelRedisInstanceTypeStr(sentinelRedisInstance *ri);
void sentinelAbortFailover(sentinelRedisInstance *ri);
void sentinelEvent(int level, char *type, sentinelRedisInstance *ri, const char *fmt, ...);
sentinelRedisInstance *sentinelSelectSlave(sentinelRedisInstance *master);
void sentinelScheduleScriptExecution(char *path, ...);
void sentinelStartFailover(sentinelRedisInstance *master);
void sentinelDiscardReplyCallback(redisAsyncContext *c, void *reply, void *privdata);
int sentinelSendSlaveOf(sentinelRedisInstance *ri, char *host, int port);
char *sentinelVoteLeader(sentinelRedisInstance *master, uint64_t req_epoch, char *req_runid, uint64_t *leader_epoch);
void sentinelFlushConfig(void);
void sentinelGenerateInitialMonitorEvents(void);
int sentinelSendPing(sentinelRedisInstance *ri);

/* ========================= Dictionary types =============================== */

unsigned int dictSdsHash(const void *key);
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);
void releaseSentinelRedisInstance(sentinelRedisInstance *ri);

void dictInstancesValDestructor (void *privdata, void *obj) {
    releaseSentinelRedisInstance(obj);
}

/* Instance name (sds) -> instance (sentinelRedisInstance pointer)
 *
 * also used for: sentinelRedisInstance->sentinels dictionary that maps
 * sentinels ip:port to last seen time in Pub/Sub hello message. */
// 这个字典类型有两个作用：
// 1） 将 instance 名字映射到一个 sentinelRedisInstance 指针
// 2） 将 sentinelRedisInstance 指针映射到一个字典，
//     字典的键是 Sentinel 的 ip:port 地址，
//     字典的值是该 Sentinel 最后一次向频道发送信息的时间
dictType instancesDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    dictInstancesValDestructor /* val destructor */
};

/* Instance runid (sds) -> votes (long casted to void*)
 *
 * This is useful into sentinelGetObjectiveLeader() function in order to
 * count the votes and understand who is the leader. */
// 将一个运行 ID 映射到一个 cast 成 void* 类型的 long 值的投票数量上
// 用于统计客观 leader sentinel
dictType leaderVotesDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    NULL                       /* val destructor */
};

/* =========================== Initialization =============================== */

void sentinelCommand(redisClient *c);
void sentinelInfoCommand(redisClient *c);
void sentinelSetCommand(redisClient *c);
void sentinelPublishCommand(redisClient *c);

// 服务器在 sentinel 模式下可执行的命令
struct redisCommand sentinelcmds[] = {
    {"ping",pingCommand,1,"",0,NULL,0,0,0,0,0},
    {"sentinel",sentinelCommand,-2,"",0,NULL,0,0,0,0,0},
    {"subscribe",subscribeCommand,-2,"",0,NULL,0,0,0,0,0},
    {"unsubscribe",unsubscribeCommand,-1,"",0,NULL,0,0,0,0,0},
    {"psubscribe",psubscribeCommand,-2,"",0,NULL,0,0,0,0,0},
    {"punsubscribe",punsubscribeCommand,-1,"",0,NULL,0,0,0,0,0},
    {"publish",sentinelPublishCommand,3,"",0,NULL,0,0,0,0,0},
    {"info",sentinelInfoCommand,-1,"",0,NULL,0,0,0,0,0},
    {"shutdown",shutdownCommand,-1,"",0,NULL,0,0,0,0,0}
};

/* This function overwrites a few normal Redis config default with Sentinel
 * specific defaults. */
// 这个函数会用 Sentinel 所属的属性覆盖服务器默认的属性
void initSentinelConfig(void) {
    server.port = REDIS_SENTINEL_PORT;
}

/* Perform the Sentinel mode initialization. */
// 以 Sentinel 模式初始化服务器，需要清除常规 redis-server 模式的配置
void initSentinel(void) {
    int j;

    /* Remove usual Redis commands from the command table, then just add
     * the SENTINEL command. */

    // 清空 Redis 服务器的命令表（该表用于普通模式）
    dictEmpty(server.commands,NULL);
    // 将 SENTINEL 模式所用的命令添加进命令表
    for (j = 0; j < sizeof(sentinelcmds)/sizeof(sentinelcmds[0]); j++) {
        int retval;
        struct redisCommand *cmd = sentinelcmds+j;  // 采用 sizeof(struct redisCommand) 作为步长

        retval = dictAdd(server.commands, sdsnew(cmd->name), cmd);
        redisAssert(retval == DICT_OK);
    }

    /* Initialize various data structures. */
    /* 初始化 Sentinel 的状态 */
    // 初始化纪元
    sentinel.current_epoch = 0;

    // 初始化保存 master 信息的字典
    sentinel.masters = dictCreate(&instancesDictType,NULL);

    // 初始化 TILT 模式的相关选项
    sentinel.tilt = 0;
    sentinel.tilt_start_time = 0;
    sentinel.previous_time = mstime();

    // 初始化脚本相关选项
    sentinel.running_scripts = 0;
    sentinel.scripts_queue = listCreate();
}

/* This function gets called when the server is in Sentinel mode, started,
 * loaded the configuration, and is ready for normal operations. */
// 这个函数在 Sentinel 准备就绪，可以执行操作时执行
void sentinelIsRunning(void) {
    redisLog(REDIS_WARNING,"Sentinel runid is %s", server.runid);

    // Sentinel 不能在没有配置文件的情况下执行
    if (server.configfile == NULL) {
        redisLog(REDIS_WARNING,
            "Sentinel started without a config file. Exiting...");
        exit(1);
    } else if (access(server.configfile,W_OK) == -1) {
        redisLog(REDIS_WARNING,
            "Sentinel config file %s is not writable: %s. Exiting...",
            server.configfile,strerror(errno));
        exit(1);
    }

    /* We want to generate a +monitor event for every configured master
     * at startup. */
    sentinelGenerateInitialMonitorEvents();

    // 这个函数完成之后，就是直接进行 epoll 里面等候调度了
    // 接下来一般是 epoll timeout，然后 redis.c/serverCron() 触发 sentinel 的主要运行逻辑 sentinelTimer()

    // 当配置里面有 sentinel monitor master_server_name  5.5.75.110 6379 2 的话
    // 会先为这个 master 创建一个 SRI 对象，但是并没有连接，而是等到 cron 的时候，再去真正发起连接
}

/* ============================== sentinelAddr ============================== */

/* Create a sentinelAddr object and return it on success.
 *
 * 创建一个 sentinel 地址对象，并在创建成功时返回该对象。
 *
 * On error NULL is returned and errno is set to:
 *
 * 函数在出错时返回 NULL ，并将 errnor 设为以下值：
 *
 *  ENOENT: Can't resolve the hostname.
 *          不能解释 hostname
 *
 *  EINVAL: Invalid port number.
 *          端口号不正确
 */
sentinelAddr *createSentinelAddr(char *hostname, int port) {
    char buf[32];
    sentinelAddr *sa;

    // 检查端口号
    if (port <= 0 || port > 65535) {
        errno = EINVAL;
        return NULL;
    }

    // 检查并创建地址
    if (anetResolve(NULL,hostname,buf,sizeof(buf)) == ANET_ERR) {
        errno = ENOENT;
        return NULL;
    }

    // 创建并返回地址结构
    sa = zmalloc(sizeof(*sa));
    sa->ip = sdsnew(buf);
    sa->port = port;
    return sa;
}

/* Return a duplicate of the source address. */
// 复制并返回给定地址的一个副本
sentinelAddr *dupSentinelAddr(sentinelAddr *src) {
    sentinelAddr *sa;

    sa = zmalloc(sizeof(*sa));
    sa->ip = sdsnew(src->ip);
    sa->port = src->port;
    return sa;
}

/* Free a Sentinel address. Can't fail. */
// 释放 Sentinel 地址
void releaseSentinelAddr(sentinelAddr *sa) {
    sdsfree(sa->ip);
    zfree(sa);
}

/* Return non-zero if two addresses are equal. */
// 如果两个地址相同，那么返回 0
int sentinelAddrIsEqual(sentinelAddr *a, sentinelAddr *b) {
    return a->port == b->port && !strcasecmp(a->ip,b->ip);
}

/* =========================== Events notification ========================== */
/* 所谓的 Events notification 是指：
 * 这是向每一个订阅了自己的 channel 发送 EVENT 信息：告诉自己的所有 subscriber，我发生了什么事（EVENT）
 * server.pubsub_channels 这里面的全部发送一次
 * 必要情况下，还要把这个 EVNET 记录在日志里面
 * 就比如 +monitor 这个 EVENT，是通过 channel、log 的方式告知大家，我这个 sentinel 又多 monitor 一个 redis-server，具体信息是 xxxx
 */

/* Send an event to log, pub/sub, user notification script.
 *
 * 将事件发送到日志、频道，以及用户提醒脚本。
 * 
 * 'level' is the log level for logging. Only REDIS_WARNING events will trigger
 * the execution of the user notification script.
 *
 * level 是日志的级别。只有 REDIS_WARNING 级别的日志会触发用户提醒脚本。
 *
 * 'type' is the message type, also used as a pub/sub channel name.
 *
 * type 是信息的类型，也用作频道的名字。
 *
 * 'ri', is the redis instance target of this event if applicable, and is
 * used to obtain the path of the notification script to execute.
 *
 * ri 是引发事件的 Redis  instance ，它可以用来获取可执行的用户脚本。
 *
 * The remaining arguments are printf-alike.
 *
 * 剩下的都是类似于传给 printf 函数的参数。
 *
 * If the format specifier starts with the two characters "%@" then ri is
 * not NULL, and the message is prefixed with an instance identifier in the
 * following format:
 *
 * 如果格式指定以 "%@" 两个字符开头，并且 ri 不为空，
 * 那么信息将使用以下 instance 标识符为开头：
 *
 *  <instance type> <instance name> <ip> <port>
 *
 *  If the instance type is not master, than the additional string is
 *  added to specify the originating master:
 *
 *  如果 instance 的类型不是 master ，那么以下内容会被追加到信息的后面，
 *  用于指定目标 master ：
 *
 *  @ <master name> <master ip> <master port>
 *
 *  Any other specifier after "%@" is processed by printf itself.
 *
 * "%@" 之后的其他指派器（specifier）都和 printf 函数所使用的指派器一样。
 */
// TODO:(DONE) 为什么要加入这个 monitor event ？监听了什么？
// 其实这个仅仅是通知罢了，并不会发生实际作用
// type 指定了 channel, 例如：+monitor

// 例如：sentinel MONITOR master_server 5.5.75.111 6379 2 触发的
//       sentinelEvent(REDIS_WARNING,"+monitor",ri,"%@ quorum %d",ri->quorum);
//       ri 就是当前 sentinel 管理连接向 master_server 的结构
void sentinelEvent(int level, char *type, sentinelRedisInstance *ri,
                   const char *fmt, ...) {
    va_list ap;
    // 日志字符串
    char msg[REDIS_MAX_LOGMSG_LEN];
    robj *channel, *payload;

    /* Handle %@ */
    // 处理 %@
    if (fmt[0] == '%' && fmt[1] == '@') {

        // 如果 ri  instance 是 master ，那么 master 就是 NULL 
        // 否则 ri 就是一个 slave 或者 sentinel ，而 master 就是该 instance 的 master 
        //
        // sentinelRedisInstance *master = NULL;
        // if (~(ri->flags & SRI_MASTER))
        //     master = ri->master;
        sentinelRedisInstance *master = (ri->flags & SRI_MASTER) ?
                                         NULL : ri->master;

        if (master) {
            
            // ri 不是 master 

            snprintf(msg, sizeof(msg), "%s %s %s %d @ %s %s %d",
                // 打印 ri 的类型
                sentinelRedisInstanceTypeStr(ri),
                // 打印 ri 的名字、IP 和端口号
                ri->name, ri->addr->ip, ri->addr->port,
                // 打印 ri 的 master 的名字、 IP 和端口号
                master->name, master->addr->ip, master->addr->port);
        } else {

            // ri 是 master 

            snprintf(msg, sizeof(msg), "%s %s %s %d",
                // 打印 ri 的类型
                sentinelRedisInstanceTypeStr(ri),
                // 打印 ri 的名字、IP 和端口号
                ri->name, ri->addr->ip, ri->addr->port);
        }

        // 跳过已处理的 "%@" 字符
        fmt += 2;

    } else {
        msg[0] = '\0';
    }

    /* Use vsprintf for the rest of the formatting if any. */
    // 打印之后的内容，格式和平常的 printf 一样
    if (fmt[0] != '\0') {
        va_start(ap, fmt);
        vsnprintf(msg+strlen(msg), sizeof(msg)-strlen(msg), fmt, ap);
        va_end(ap);
    }

    /* Log the message if the log level allows it to be logged. */
    // 如果日志的级别足够高的话，那么记录到本地日志中
    if (level >= server.verbosity)
        redisLog(level,"%s %s",type,msg);

    /* Publish the message via Pub/Sub if it's not a debugging one. */
    // 如果日志不是 DEBUG 日志，那么将它发送到频道中
    if (level != REDIS_DEBUG) {
        // 频道
        channel = createStringObject(type,strlen(type));
        // 内容
        payload = createStringObject(msg,strlen(msg));
        // 发送信息
        // TODO:(DONE) 你咋知道有谁在订阅这个 channel ？不需要知道，master\slave 会广播就好了，核心目的是让系统内的所有 sentinel 都能够收到某些广播信息
        // TODO:(DONE) 这个 channel 都有谁在订阅？所有 sentinel ？其他 redis-server(slave\master) ？ 都可以，只要你想，redis-cli 也可以的
        // TODO:(DONE) 看看究竟是谁会用这里，广播 xxxx event（+monitor、+salve 是不会用的了）
        // init 过程中，或者是第一次监控 master 的话，里面的 server.pubsub_channels 会是空的
        // 所以第一次的时候，并不会 publish 任何信息出去
        pubsubPublishMessage(channel,payload);
        decrRefCount(channel);
        decrRefCount(payload);
    }

    /* Call the notification script if applicable. */
    // 如果有需要的话，调用提醒脚本
    if (level == REDIS_WARNING && ri != NULL) {
        sentinelRedisInstance *master = (ri->flags & SRI_MASTER) ?
                                         ri : ri->master;
        if (master->notification_script) {
            sentinelScheduleScriptExecution(master->notification_script,
                type,msg,NULL);
        }
    }
}

/* This function is called only at startup and is used to generate a
 * +monitor event for every configured master. The same events are also
 * generated when a master to monitor is added at runtime via the
 * SENTINEL MONITOR command. */
// 在 Sentinel 启动时执行，用于创建并生成 +monitor 事件
void sentinelGenerateInitialMonitorEvents(void) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(sentinel.masters);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        sentinelEvent(REDIS_WARNING,"+monitor",ri,"%@ quorum %d",ri->quorum);
    }
    dictReleaseIterator(di);
}

/* ============================ script execution ============================ */

/* Release a script job structure and all the associated data. */
// 释放一个脚本任务结构，以及该任务的相关数据。
void sentinelReleaseScriptJob(sentinelScriptJob *sj) {
    int j = 0;

    while(sj->argv[j]) sdsfree(sj->argv[j++]);
    zfree(sj->argv);
    zfree(sj);
}

// 将给定参数和脚本放入队列
#define SENTINEL_SCRIPT_MAX_ARGS 16
void sentinelScheduleScriptExecution(char *path, ...) {
    va_list ap;
    char *argv[SENTINEL_SCRIPT_MAX_ARGS+1];
    int argc = 1;
    sentinelScriptJob *sj;

    // 生成参数
    va_start(ap, path);
    while(argc < SENTINEL_SCRIPT_MAX_ARGS) {
        argv[argc] = va_arg(ap,char*);
        if (!argv[argc]) break;
        argv[argc] = sdsnew(argv[argc]); /* Copy the string. */
        argc++;
    }
    va_end(ap);
    argv[0] = sdsnew(path);
    
    // 初始化脚本结构
    sj = zmalloc(sizeof(*sj));
    sj->flags = SENTINEL_SCRIPT_NONE;
    sj->retry_num = 0;
    sj->argv = zmalloc(sizeof(char*)*(argc+1));
    sj->start_time = 0;
    sj->pid = 0;
    memcpy(sj->argv,argv,sizeof(char*)*(argc+1));

    // 添加到等待执行脚本队列的末尾， FIFO
    listAddNodeTail(sentinel.scripts_queue,sj);

    /* Remove the oldest non running script if we already hit the limit. */
    // 如果入队的脚本数量太多，那么移除最旧的未执行脚本
    if (listLength(sentinel.scripts_queue) > SENTINEL_SCRIPT_MAX_QUEUE) {
        listNode *ln;
        listIter li;

        listRewind(sentinel.scripts_queue,&li);
        while ((ln = listNext(&li)) != NULL) {
            sj = ln->value;

            // 不删除正在运行的脚本
            if (sj->flags & SENTINEL_SCRIPT_RUNNING) continue;
            /* The first node is the oldest as we add on tail. */
            listDelNode(sentinel.scripts_queue,ln);
            sentinelReleaseScriptJob(sj);
            break;
        }
        redisAssert(listLength(sentinel.scripts_queue) <=
                    SENTINEL_SCRIPT_MAX_QUEUE);
    }
}

/* Lookup a script in the scripts queue via pid, and returns the list node
 * (so that we can easily remove it from the queue if needed). */
// 根据 pid ，查找正在运行中的脚本
listNode *sentinelGetScriptListNodeByPid(pid_t pid) {
    listNode *ln;
    listIter li;

    listRewind(sentinel.scripts_queue,&li);
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;

        if ((sj->flags & SENTINEL_SCRIPT_RUNNING) && sj->pid == pid)
            return ln;
    }
    return NULL;
}

/* Run pending scripts if we are not already at max number of running
 * scripts. */
// 运行等待执行的脚本
void sentinelRunPendingScripts(void) {
    listNode *ln;
    listIter li;
    mstime_t now = mstime();

    /* Find jobs that are not running and run them, from the top to the
     * tail of the queue, so we run older jobs first. */
    // 如果运行的脚本数量未超过最大值，
    // 那么从 FIFO 队列中取出未运行的脚本，并运行该脚本
    listRewind(sentinel.scripts_queue,&li);
    while (sentinel.running_scripts < SENTINEL_SCRIPT_MAX_RUNNING &&
           (ln = listNext(&li)) != NULL)
    {
        sentinelScriptJob *sj = ln->value;
        pid_t pid;

        /* Skip if already running. */
        // 跳过已运行脚本
        if (sj->flags & SENTINEL_SCRIPT_RUNNING) continue;

        /* Skip if it's a retry, but not enough time has elapsed. */
        // 这是一个重试脚本，但它刚刚执行完，稍后再重试
        if (sj->start_time && sj->start_time > now) continue;

        // 打开运行标记
        sj->flags |= SENTINEL_SCRIPT_RUNNING;
        // 记录开始时间
        sj->start_time = mstime();
        // 增加重试计数器
        sj->retry_num++;

        // 创建子进程
        pid = fork();

        if (pid == -1) {
            
            // 创建子进程失败

            /* Parent (fork error).
             * We report fork errors as signal 99, in order to unify the
             * reporting with other kind of errors. */
            sentinelEvent(REDIS_WARNING,"-script-error",NULL,
                          "%s %d %d", sj->argv[0], 99, 0);
            sj->flags &= ~SENTINEL_SCRIPT_RUNNING;
            sj->pid = 0;
        } else if (pid == 0) {

            // 子进程执行脚本

            /* Child */
            execve(sj->argv[0],sj->argv,environ);
            /* If we are here an error occurred. */
            _exit(2); /* Don't retry execution. */
        } else {

            // 父进程
            
            // 增加运行脚本计数器
            sentinel.running_scripts++;

            // 记录 pid
            sj->pid = pid;

            // 发送脚本运行信号
            sentinelEvent(REDIS_DEBUG,"+script-child",NULL,"%ld",(long)pid);
        }
    }
}

/* How much to delay the execution of a script that we need to retry after
 * an error?
 *
 * We double the retry delay for every further retry we do. So for instance
 * if RETRY_DELAY is set to 30 seconds and the max number of retries is 10
 * starting from the second attempt to execute the script the delays are:
 * 30 sec, 60 sec, 2 min, 4 min, 8 min, 16 min, 32 min, 64 min, 128 min. */
// 计算重试脚本前的延迟时间
mstime_t sentinelScriptRetryDelay(int retry_num) {
    mstime_t delay = SENTINEL_SCRIPT_RETRY_DELAY;

    while (retry_num-- > 1) delay *= 2;
    return delay;
}

/* Check for scripts that terminated, and remove them from the queue if the
 * script terminated successfully. If instead the script was terminated by
 * a signal, or returned exit code "1", it is scheduled to run again if
 * the max number of retries did not already elapsed. */
// 检查脚本的退出状态，并在脚本成功退出时，将脚本从队列中删除。
// 如果脚本被信号终结，或者返回退出代码 1 ，那么只要该脚本的重试次数未超过限制
// 那么该脚本就会被调度，并等待重试
void sentinelCollectTerminatedScripts(void) {
    int statloc;
    pid_t pid;

    // 获取子进程信号
    while ((pid = wait3(&statloc,WNOHANG,NULL)) > 0) {
        int exitcode = WEXITSTATUS(statloc);
        int bysignal = 0;
        listNode *ln;
        sentinelScriptJob *sj;

        // 发送脚本终结信号
        if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);
        sentinelEvent(REDIS_DEBUG,"-script-child",NULL,"%ld %d %d",
            (long)pid, exitcode, bysignal);
        
        // 在队列中安 pid 查找脚本
        ln = sentinelGetScriptListNodeByPid(pid);
        if (ln == NULL) {
            redisLog(REDIS_WARNING,"wait3() returned a pid (%ld) we can't find in our scripts execution queue!", (long)pid);
            continue;
        }
        sj = ln->value;

        /* If the script was terminated by a signal or returns an
         * exit code of "1" (that means: please retry), we reschedule it
         * if the max number of retries is not already reached. */
        if ((bysignal || exitcode == 1) &&
            sj->retry_num != SENTINEL_SCRIPT_MAX_RETRY)
        {
            // 重试脚本

            sj->flags &= ~SENTINEL_SCRIPT_RUNNING;
            sj->pid = 0;
            sj->start_time = mstime() +
                             sentinelScriptRetryDelay(sj->retry_num);
        } else {
            /* Otherwise let's remove the script, but log the event if the
             * execution did not terminated in the best of the ways. */

            // 发送脚本执行错误事件
            if (bysignal || exitcode != 0) {
                sentinelEvent(REDIS_WARNING,"-script-error",NULL,
                              "%s %d %d", sj->argv[0], bysignal, exitcode);
            }

            // 将脚本从队列中删除
            listDelNode(sentinel.scripts_queue,ln);
            sentinelReleaseScriptJob(sj);
            sentinel.running_scripts--;
        }
    }
}

/* Kill scripts in timeout, they'll be collected by the
 * sentinelCollectTerminatedScripts() function. */
// 杀死超时脚本，这些脚本会被 sentinelCollectTerminatedScripts 函数回收处理
void sentinelKillTimedoutScripts(void) {
    listNode *ln;
    listIter li;
    mstime_t now = mstime();

    // 遍历队列中的所有脚本
    listRewind(sentinel.scripts_queue,&li);
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;

        // 选出那些正在执行，并且执行时间超过限制的脚本
        if (sj->flags & SENTINEL_SCRIPT_RUNNING &&
            (now - sj->start_time) > SENTINEL_SCRIPT_MAX_RUNTIME)
        {
            // 发送脚本超时事件
            sentinelEvent(REDIS_WARNING,"-script-timeout",NULL,"%s %ld",
                sj->argv[0], (long)sj->pid);

            // 杀死脚本进程
            kill(sj->pid,SIGKILL);
        }
    }
}

/* Implements SENTINEL PENDING-SCRIPTS command. */
// 打印脚本队列中所有脚本的状态
void sentinelPendingScriptsCommand(redisClient *c) {
    listNode *ln;
    listIter li;

    addReplyMultiBulkLen(c,listLength(sentinel.scripts_queue));
    listRewind(sentinel.scripts_queue,&li);
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;
        int j = 0;

        addReplyMultiBulkLen(c,10);

        addReplyBulkCString(c,"argv");
        while (sj->argv[j]) j++;
        addReplyMultiBulkLen(c,j);
        j = 0;
        while (sj->argv[j]) addReplyBulkCString(c,sj->argv[j++]);

        addReplyBulkCString(c,"flags");
        addReplyBulkCString(c,
            (sj->flags & SENTINEL_SCRIPT_RUNNING) ? "running" : "scheduled");

        addReplyBulkCString(c,"pid");
        addReplyBulkLongLong(c,sj->pid);

        if (sj->flags & SENTINEL_SCRIPT_RUNNING) {
            addReplyBulkCString(c,"run-time");
            addReplyBulkLongLong(c,mstime() - sj->start_time);
        } else {
            mstime_t delay = sj->start_time ? (sj->start_time-mstime()) : 0;
            if (delay < 0) delay = 0;
            addReplyBulkCString(c,"run-delay");
            addReplyBulkLongLong(c,delay);
        }

        addReplyBulkCString(c,"retry-num");
        addReplyBulkLongLong(c,sj->retry_num);
    }
}

/* This function calls, if any, the client reconfiguration script with the
 * following parameters:
 *
 * 当该函数执行时，使用以下格式的参数调用客户端重配置脚本
 *
 * <master-name> <role> <state> <from-ip> <from-port> <to-ip> <to-port>
 *
 * It is called every time a failover is performed.
 *
 * 这个函数在每次执行故障迁移时都会执行一次。
 *
 * <state> is currently always "failover".
 * <role> is either "leader" or "observer".
 *
 * <state> 总是 "failover" ，而 <role> 可以是 "leader" 或者 "observer"
 *
 * from/to fields are respectively master -> promoted slave addresses for
 * "start" and "end". 
 */
void sentinelCallClientReconfScript(sentinelRedisInstance *master, int role, char *state, sentinelAddr *from, sentinelAddr *to) {
    char fromport[32], toport[32];

    if (master->client_reconfig_script == NULL) return;
    ll2string(fromport,sizeof(fromport),from->port);
    ll2string(toport,sizeof(toport),to->port);
    // 将给定参数和脚本放进度列，等待执行
    sentinelScheduleScriptExecution(master->client_reconfig_script,
        master->name,
        (role == SENTINEL_LEADER) ? "leader" : "observer",
        state, from->ip, fromport, to->ip, toport, NULL);
}

/* ========================== sentinelRedisInstance ========================= */
/** 
 * 这是 sentinel 用来管理被监控的对端(可以是 slave、master、sentinel 的 redis-server)
 */

/* Create a redis instance, the following fields must be populated by the
 * caller if needed:
 *
 * 创建一个 Redis  instance ，在有需要时，以下两个 field需要从调用者提取：
 *
 * runid: set to NULL but will be populated once INFO output is received.
 *        设置为 NULL ，并在接收到 INFO 命令的回复时设置
 *
 * info_refresh: is set to 0 to mean that we never received INFO so far.
 *               如果这个值为 0 ，那么表示我们未收到过 INFO 信息。
 *
 * If SRI_MASTER is set into initial flags the instance is added to
 * sentinel.masters table.
 *
 * 如果 flags 参数为 SRI_MASTER ，
 * 那么这个 instance 会被添加到 sentinel.masters 表。
 *
 * if SRI_SLAVE or SRI_SENTINEL is set then 'master' must be not NULL and the
 * instance is added into master->slaves or master->sentinels table.
 *
 * TODO: 这两种 case 的 SRI 配置是从那里来的？INFO CMD ？
 * 如果 flags 为 SRI_SLAVE 或者 SRI_SENTINEL ，
 * 那么 master 参数不能为 NULL ，
 * SRI_SLAVE 类型的 instance 会被添加到 master->slaves 表中，
 * 而 SRI_SENTINEL 类型的 instance 则会被添加到 master->sentinels 表中。
 *
 * If the instance is a slave or sentinel, the name parameter is ignored and
 * is created automatically as hostname:port.
 *
 * 如果 instance 是 slave 或者 sentinel ，那么 name 参数会被自动忽略，
 *  instance 的名字会被自动设置为 hostname:port 。
 *
 * The function fails if hostname can't be resolved or port is out of range.
 * When this happens NULL is returned and errno is set accordingly to the
 * createSentinelAddr() function.
 *
 * 当 hostname 不能被解析，或者超出范围时，函数将失败。
 * 函数将返回 NULL ，并设置 errno 变量，
 * 具体的出错值请参考 createSentinelAddr() 函数。
 *
 * The function may also fail and return NULL with errno set to EBUSY if
 * a master or slave with the same name already exists. 
 *
 * 当相同名字的 master 或者 slave 已经存在时，函数返回 NULL ，
 * 并将 errno 设为 EBUSY 。
 */
// sentinelRedisInstance *master 只有在 SRI_SLAVE\SRI_SENTINEL 的情况下才会被设置
sentinelRedisInstance *createSentinelRedisInstance(char *name, int flags, char *hostname,
    int port, int quorum, sentinelRedisInstance *master)
{
    sentinelRedisInstance *ri;
    sentinelAddr *addr;
    dict *table = NULL;
    char slavename[128], *sdsname;

    redisAssert(flags & (SRI_MASTER|SRI_SLAVE|SRI_SENTINEL));
    redisAssert((flags & SRI_MASTER) || master != NULL);

    /* Check address validity. */
    // 保存 IP 地址和端口号到 addr
    addr = createSentinelAddr(hostname,port);
    if (addr == NULL) return NULL;

    /* For slaves and sentinel we use ip:port as name. */
    // 如果 instance 是 slave 或者 sentinel ，那么使用 ip:port 格式为 instance 设置名字
    if (flags & (SRI_SLAVE|SRI_SENTINEL)) {
        snprintf(slavename,sizeof(slavename),
            strchr(hostname,':') ? "[%s]:%d" : "%s:%d",
            hostname,port);
        name = slavename;
    }

    /* Make sure the entry is not duplicated. This may happen when the same
     * name for a master is used multiple times inside the configuration or
     * if we try to add multiple times a slave or sentinel with same ip/port
     * to a master. */
    // 配置文件中添加了重复的 master 配置
    // 或者尝试添加一个相同 ip 或者端口号的 slave 或者 sentinel 时
    // 就可能出现重复添加同一个 instance 的情况
    // 为了避免这种现象，程序在添加新 instance 之前，需要先检查 instance 是否已存在
    // 只有不存在的 instance 会被添加

    // 选择要添加的表
    // 注意 master role 的 redis-server 会被添加到 sentinel.masters 表
    // 而 slave 和 sentinel 则会被添加到 master 所属的 slaves 表和 sentinels 表中
    // （这样 sentinel 才能进行去重，避免当前 sentinel 对同一个 redis-server 多次发起同样的连接）
    if (flags & SRI_MASTER) table = sentinel.masters;
    else if (flags & SRI_SLAVE) table = master->slaves;
    else if (flags & SRI_SENTINEL) table = master->sentinels;
    sdsname = sdsnew(name);
    if (dictFind(table,sdsname)) {

        //  instance 已存在，函数直接返回

        sdsfree(sdsname);
        errno = EBUSY;
        return NULL;
    }

    /* Create the instance object. */
    // 创建 instance 对象
    ri = zmalloc(sizeof(*ri));
    /* Note that all the instances are started in the disconnected state,
     * the event loop will take care of connecting them. */
    // 所有连接都已断线为起始状态，sentinel 会在需要时自动为它创建连接
    ri->flags = flags | SRI_DISCONNECTED;
    ri->name = sdsname;
    ri->runid = NULL;
    ri->config_epoch = 0;
    ri->addr = addr;
    ri->cc = NULL;
    ri->pc = NULL;
    ri->pending_commands = 0;
    ri->cc_conn_time = 0;
    ri->pc_conn_time = 0;
    ri->pc_last_activity = 0;
    /* We set the last_ping_time to "now" even if we actually don't have yet
     * a connection with the node, nor we sent a ping.
     * This is useful to detect a timeout in case we'll not be able to connect
     * with the node at all. */
    ri->last_ping_time = mstime();
    ri->last_avail_time = mstime();
    ri->last_pong_time = mstime();
    ri->last_pub_time = mstime();
    ri->last_hello_time = mstime();
    ri->last_master_down_reply_time = mstime();
    ri->s_down_since_time = 0;
    ri->o_down_since_time = 0;
    ri->down_after_period = master ? master->down_after_period :
                            SENTINEL_DEFAULT_DOWN_AFTER;
    ri->master_link_down_time = 0;
    ri->auth_pass = NULL;
    ri->slave_priority = SENTINEL_DEFAULT_SLAVE_PRIORITY;
    ri->slave_reconf_sent_time = 0;
    ri->slave_master_host = NULL;
    ri->slave_master_port = 0;
    ri->slave_master_link_status = SENTINEL_MASTER_LINK_STATUS_DOWN;
    ri->slave_repl_offset = 0;
    ri->sentinels = dictCreate(&instancesDictType,NULL);
    ri->quorum = quorum;
    ri->parallel_syncs = SENTINEL_DEFAULT_PARALLEL_SYNCS;
    ri->master = master;
    ri->slaves = dictCreate(&instancesDictType,NULL);
    ri->info_refresh = 0;

    /* Failover state. */
    ri->leader = NULL;
    ri->leader_epoch = 0;
    ri->failover_epoch = 0;
    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    ri->failover_state_change_time = 0;
    ri->failover_start_time = 0;
    ri->failover_timeout = SENTINEL_DEFAULT_FAILOVER_TIMEOUT;
    ri->failover_delay_logged = 0;
    ri->promoted_slave = NULL;
    ri->notification_script = NULL;
    ri->client_reconfig_script = NULL;

    /* Role */
    ri->role_reported = ri->flags & (SRI_MASTER|SRI_SLAVE);
    ri->role_reported_time = mstime();
    ri->slave_conf_change_time = mstime();

    /* Add into the right table. */
    // 将 instance 添加到适当的表中
    dictAdd(table, ri->name, ri);

    // 返回 instance 
    return ri;
}

/* Release this instance and all its slaves, sentinels, hiredis connections.
 *
 * 释放一个 instance ，以及它的所有 slave 、sentinel ，以及 hiredis 连接。
 *
 * This function does not take care of unlinking the instance from the main
 * masters table (if it is a master) or from its master sentinels/slaves table
 * if it is a slave or sentinel. 
 *
 * 如果这个 instance 是一个 slave 或者 sentinel ，
 * 那么这个函数也会从该 instance 所属的 master 表中删除这个 slave /sentinel 。
 */
void releaseSentinelRedisInstance(sentinelRedisInstance *ri) {

    /* Release all its slaves or sentinels if any. */
    // 释放（可能有的）sentinel 和 slave
    dictRelease(ri->sentinels);
    dictRelease(ri->slaves);

    /* Release hiredis connections. */
    // 释放连接
    if (ri->cc) sentinelKillLink(ri,ri->cc);
    if (ri->pc) sentinelKillLink(ri,ri->pc);

    /* Free other resources. */
    // 释放其他资源
    sdsfree(ri->name);
    sdsfree(ri->runid);
    sdsfree(ri->notification_script);
    sdsfree(ri->client_reconfig_script);
    sdsfree(ri->slave_master_host);
    sdsfree(ri->leader);
    sdsfree(ri->auth_pass);
    releaseSentinelAddr(ri->addr);

    /* Clear state into the master if needed. */
    // 清除故障转移带来的状态
    if ((ri->flags & SRI_SLAVE) && (ri->flags & SRI_PROMOTED) && ri->master)
        ri->master->promoted_slave = NULL;

    zfree(ri);
}

/* Lookup a slave in a master Redis instance, by ip and port. */
// 根据 IP 和端口号，查找 master  instance 的 slave 
sentinelRedisInstance *sentinelRedisInstanceLookupSlave(
                sentinelRedisInstance *ri, char *ip, int port)
{
    sds key;
    sentinelRedisInstance *slave;
  
    redisAssert(ri->flags & SRI_MASTER);
    key = sdscatprintf(sdsempty(),
        strchr(ip,':') ? "[%s]:%d" : "%s:%d",
        ip,port);
    slave = dictFetchValue(ri->slaves,key);
    sdsfree(key);
    return slave;
}

/* Return the name of the type of the instance as a string. */
// 以字符串形式返回 instance 的类型
const char *sentinelRedisInstanceTypeStr(sentinelRedisInstance *ri) {
    if (ri->flags & SRI_MASTER) return "master";
    else if (ri->flags & SRI_SLAVE) return "slave";
    else if (ri->flags & SRI_SENTINEL) return "sentinel";
    else return "unknown";
}

/* This function removes all the instances found in the dictionary of
 * sentinels in the specified 'master', having either:
 * 
 * 1) The same ip/port as specified.
 *     instance 给定的 IP 和端口号和字典中已有的 instance 相同
 *
 * 2) The same runid.
 *     instance 给定的运行 ID 和字典中已有的 instance 相同
 *
 * "1" and "2" don't need to verify at the same time, just one is enough.
 *
 * 以上条件任意满足一个，移除操作就会被执行。
 *
 * If "runid" is NULL it is not checked.
 * Similarly if "ip" is NULL it is not checked.
 *
 * 如果 runid 参数为 NULL ，那么不检查该参数。
 * 如果 ip 参数为 NULL ，那么不检查该参数。
 *
 * This function is useful because every time we add a new Sentinel into
 * a master's Sentinels dictionary, we want to be very sure about not
 * having duplicated instances for any reason. This is important because
 * other sentinels are needed to reach ODOWN quorum, and later to get
 * voted for a given configuration epoch in order to perform the failover.
 *
 * 因为 sentinel 的操作比如故障转移，需要多个 sentinel 投票才能进行。
 * 所以我们必须保证所添加的各个 sentinel 都是不相同、独一无二的，
 * 这样才能确保投票的合法性。
 *
 * The function returns the number of Sentinels removed. 
 *
 * 函数的返回值为被移除 sentinel 的数量
 */
int removeMatchingSentinelsFromMaster(sentinelRedisInstance *master, char *ip, int port, char *runid) {
    dictIterator *di;
    dictEntry *de;
    int removed = 0;

    di = dictGetSafeIterator(master->sentinels);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        // 运行 ID 相同，或者 IP 和端口号相同，那么移除该 instance 
        if ((ri->runid && runid && strcmp(ri->runid,runid) == 0) ||
            (ip && strcmp(ri->addr->ip,ip) == 0 && port == ri->addr->port))
        {
            dictDelete(master->sentinels,ri->name);
            removed++;
        }
    }
    dictReleaseIterator(di);

    return removed;
}

/* Search an instance with the same runid, ip and port into a dictionary
 * of instances. Return NULL if not found, otherwise return the instance
 * pointer.
 *
 * 在给定的 instance 中查找具有相同 runid 、ip 、port 的 instance ，
 * 没找到则返回 NULL 。
 *
 * runid or ip can be NULL. In such a case the search is performed only
 * by the non-NULL field. 
 *
 * runid 或者 ip 都可以为 NULL ，在这种情况下，函数只检查非空域。
 */
sentinelRedisInstance *getSentinelRedisInstanceByAddrAndRunID(dict *instances, char *ip, int port, char *runid) {
    dictIterator *di;
    dictEntry *de;
    sentinelRedisInstance *instance = NULL;

    redisAssert(ip || runid);   /* User must pass at least one search param. */

    // 遍历所有输入 instance 
    di = dictGetIterator(instances);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        // runid 不相同，忽略该 instance 
        if (runid && !ri->runid) continue;

        // 检查 ip 和端口号是否相同
        if ((runid == NULL || strcmp(ri->runid, runid) == 0) &&
            (ip == NULL || (strcmp(ri->addr->ip, ip) == 0 &&
                            ri->addr->port == port)))
        {
            instance = ri;
            break;
        }
    }
    dictReleaseIterator(di);

    return instance;
}

// 根据名字查找 master 
/* Master lookup by name */
sentinelRedisInstance *sentinelGetMasterByName(char *name) {
    sentinelRedisInstance *ri;
    sds sdsname = sdsnew(name);

    ri = dictFetchValue(sentinel.masters,sdsname);
    sdsfree(sdsname);
    return ri;
}

/* Add the specified flags to all the instances in the specified dictionary. */
// 为输入的所有 instance 打开指定的 flags
void sentinelAddFlagsToDictOfRedisInstances(dict *instances, int flags) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(instances);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        ri->flags |= flags;
    }
    dictReleaseIterator(di);
}

/* Remove the specified flags to all the instances in the specified
 * dictionary. */
// 从字典中移除所有 instance 的给定 flags
void sentinelDelFlagsToDictOfRedisInstances(dict *instances, int flags) {
    dictIterator *di;
    dictEntry *de;

    // 遍历所有 instance 
    di = dictGetIterator(instances);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        // 移除 flags
        ri->flags &= ~flags;
    }
    dictReleaseIterator(di);
}

/* Reset the state of a monitored master:
 *
 * 重置 master SRI 的监控状态(基本是重置为初始化状态)
 *
 * 1) Remove all slaves.
 *    移除 master 的所有 slave 
 * 2) Remove all sentinels.
 *    移除 master 的所有 sentinel
 * 3) Remove most of the flags resulting from runtime operations.
 *    移除大部分运行时操作标志
 * 4) Reset timers to their default value.
 *    重置计时器为默认值
 * 5) In the process of doing this undo the failover if in progress.
 *    如果故障转移正在执行的话，那么取消该它
 * 6) Disconnect the connections with the master (will reconnect automatically).
 *    断开 sentinel 与 master 的连接（之后会自动重连）
 */

#define SENTINEL_RESET_NO_SENTINELS (1<<0)
void sentinelResetMaster(sentinelRedisInstance *ri, int flags) {

    redisAssert(ri->flags & SRI_MASTER);

    dictRelease(ri->slaves);
    ri->slaves = dictCreate(&instancesDictType,NULL);

    if (!(flags & SENTINEL_RESET_NO_SENTINELS)) {
        dictRelease(ri->sentinels);
        ri->sentinels = dictCreate(&instancesDictType,NULL);
    }

    if (ri->cc) sentinelKillLink(ri,ri->cc);

    if (ri->pc) sentinelKillLink(ri,ri->pc);

    // 设置标识为断线的 master 
    ri->flags &= SRI_MASTER|SRI_DISCONNECTED;

    if (ri->leader) {
        sdsfree(ri->leader);
        ri->leader = NULL;
    }

    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    ri->failover_state_change_time = 0;
    ri->failover_start_time = 0;
    ri->promoted_slave = NULL;
    sdsfree(ri->runid);
    sdsfree(ri->slave_master_host);
    ri->runid = NULL;
    ri->slave_master_host = NULL;
    ri->last_ping_time = mstime();
    ri->last_avail_time = mstime();
    ri->last_pong_time = mstime();
    ri->role_reported_time = mstime();
    ri->role_reported = SRI_MASTER;
    // 发送 master 重置事件
    if (flags & SENTINEL_GENERATE_EVENT)
        sentinelEvent(REDIS_WARNING,"+reset-master",ri,"%@");
}

/* Call sentinelResetMaster() on every master with a name matching the specified
 * pattern. */
// 重置所有符合给定模式的 master 
int sentinelResetMastersByPattern(char *pattern, int flags) {
    dictIterator *di;
    dictEntry *de;
    int reset = 0;

    di = dictGetIterator(sentinel.masters);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        if (ri->name) {
            if (stringmatch(pattern,ri->name,0)) {
                sentinelResetMaster(ri,flags);
                reset++;
            }
        }
    }
    dictReleaseIterator(di);
    return reset;
}

/* Reset the specified master with sentinelResetMaster(), and also change
 * the ip:port address, but take the name of the instance unmodified.
 *
 * 将 master  instance 的 IP 和端口号修改成给定的 ip 和 port ，
 * 但保留 master 原来的名字。
 *
 * This is used to handle the +switch-master event.
 *
 * 这个函数用于处理 +switch-master 事件
 *
 * The function returns REDIS_ERR if the address can't be resolved for some
 * reason. Otherwise REDIS_OK is returned.  
 *
 * 函数在无法解析地址时返回 REDIS_ERR ，否则返回 REDIS_OK 。
 */
// 更新 sentinel 针对 master 的 SRI 管理信息
int sentinelResetMasterAndChangeAddress(sentinelRedisInstance *master, char *ip, int port) {
    // master 是旧的 master
    sentinelAddr *oldaddr, *newaddr;
    sentinelAddr **slaves = NULL;
    int numslaves = 0, j;
    dictIterator *di;
    dictEntry *de;

    // 根据 ip 和 port 参数，创建地址结构
    newaddr = createSentinelAddr(ip,port);
    if (newaddr == NULL) return REDIS_ERR;

    /* Make a list of slaves to add back after the reset.
     * Don't include the one having the address we are switching to. */
    // 创建一个包含原 master 所有 slave  instance 的数组
    // 用于在重置地址之后进行检查
    // 新 master （原 master 的其中一个 slave ）的地址不会包含在这个数组中
    di = dictGetIterator(master->slaves);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);

        // 跳过新 master 
        if (sentinelAddrIsEqual(slave->addr,newaddr)) continue;

        // 将 slave 保存到数组中
        slaves = zrealloc(slaves,sizeof(sentinelAddr*)*(numslaves+1));
        slaves[numslaves++] = createSentinelAddr(slave->addr->ip,
                                                 slave->addr->port);
    }
    dictReleaseIterator(di);
    
    /* If we are switching to a different address, include the old address
     * as a slave as well, so that we'll be able to sense / reconfigure
     * the old master. */
    // 如果新地址和 master 的地址不相同，
    // 将 master 的地址也作为 slave 地址添加到保存了所有 slave 地址的数组中
    // （这样等于将下线 master 设置为新 master 的 slave ）
    if (!sentinelAddrIsEqual(newaddr,master->addr)) {
        // 这样一来, 旧的 master 再次 up 起来之后, 能够被自动设置为 slave
        slaves = zrealloc(slaves,sizeof(sentinelAddr*)*(numslaves+1));
        slaves[numslaves++] = createSentinelAddr(master->addr->ip,
                                                 master->addr->port);
    }

    /* Reset and switch address. */
    // 重置 master  instance 结构
    sentinelResetMaster(master,SENTINEL_RESET_NO_SENTINELS);
    oldaddr = master->addr;
    // 为 master  instance 设置新的地址
    master->addr = newaddr;
    master->o_down_since_time = 0;
    master->s_down_since_time = 0;

    /* Add slaves back. */
    // 为 instance 加回之前保存的所有 slave
    // 不能直接用回以前的, 既然 master 会发生 down 这种 case, 网络可能是有问题的,
    // 要进行重连确认, 在选举之后, slave 也可能发生了变化
    for (j = 0; j < numslaves; j++) {
        sentinelRedisInstance *slave;

        slave = createSentinelRedisInstance(NULL,SRI_SLAVE,slaves[j]->ip,
                    slaves[j]->port, master->quorum, master);

        releaseSentinelAddr(slaves[j]);

        if (slave) {
            sentinelEvent(REDIS_NOTICE,"+slave",slave,"%@");
            sentinelFlushConfig();
        }
    }
    zfree(slaves);

    /* Release the old address at the end so we are safe even if the function
     * gets the master->addr->ip and master->addr->port as arguments. */
    // 释放旧地址
    releaseSentinelAddr(oldaddr);
    sentinelFlushConfig();
    return REDIS_OK;
}

/* Return non-zero if there was no SDOWN or ODOWN error associated to this
 * instance in the latest 'ms' milliseconds. */
// 如果 instance 在给定 ms 中没有出现过 SDOWN 或者 ODOWN 状态
// 那么函数返回一个非零值
int sentinelRedisInstanceNoDownFor(sentinelRedisInstance *ri, mstime_t ms) {
    mstime_t most_recent;

    most_recent = ri->s_down_since_time;
    if (ri->o_down_since_time > most_recent)
        most_recent = ri->o_down_since_time;
    return most_recent == 0 || (mstime() - most_recent) > ms;
}

/* Return the current master address, that is, its address or the address
 * of the promoted slave if already operational. */
// 返回当前 master 的地址
// 如果 Sentinel 正在对 master 进行故障迁移，那么返回新 master 的地址
sentinelAddr *sentinelGetCurrentMasterAddress(sentinelRedisInstance *master) {
    /* If we are failing over the master, and the state is already
     * SENTINEL_FAILOVER_STATE_RECONF_SLAVES or greater, it means that we
     * already have the new configuration epoch in the master, and the
     * slave acknowledged the configuration switch. Advertise the new
     * address. */
    if ((master->flags & SRI_FAILOVER_IN_PROGRESS) &&
        master->promoted_slave &&
        master->failover_state >= SENTINEL_FAILOVER_STATE_RECONF_SLAVES)
    {
        return master->promoted_slave->addr;
    } else {
        return master->addr;
    }
}

/* This function sets the down_after_period field value in 'master' to all
 * the slaves and sentinel instances connected to this master. */
void sentinelPropagateDownAfterPeriod(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;
    int j;
    dict *d[] = {master->slaves, master->sentinels, NULL};

    for (j = 0; d[j]; j++) {
        di = dictGetIterator(d[j]);
        while((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);
            ri->down_after_period = master->down_after_period;
        }
        dictReleaseIterator(di);
    }
}

/* ============================ Config handling ============================= */

// Sentinel 配置文件分析器（当 redis-server 以 --sentinel 启动的时候，就会调用该函数进行配置加载）
char *sentinelHandleConfiguration(char **argv, int argc) {
    sentinelRedisInstance *ri;

    if (!strcasecmp(argv[0],"monitor") && argc == 5) {
        /* sentinel monitor <master-name> <ip> <redis-port> <quorum> */
        /* sentinel monitor mymaster 172.18.96.81 6379 1 */

        // 读入 quorum 参数
        int quorum = atoi(argv[4]);

        // 检查 quorum 参数必须大于 0
        if (quorum <= 0) return "Quorum must be 1 or greater.";

        // 创建 master  instance ，并加载进 sentinel.masters 里面
        if (createSentinelRedisInstance(argv[1],SRI_MASTER,argv[2],
                                        atoi(argv[3]),quorum,NULL) == NULL)
        {
            switch(errno) {
            case EBUSY: return "Duplicated master name.";
            case ENOENT: return "Can't resolve master instance hostname.";
            case EINVAL: return "Invalid port number";
            }
        }

    } else if (!strcasecmp(argv[0],"down-after-milliseconds") && argc == 3) {

        /* down-after-milliseconds <name> <milliseconds> */

        // 查找 master 
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";

        // 设置选项
        ri->down_after_period = atoi(argv[2]);
        if (ri->down_after_period <= 0)
            return "negative or zero time parameter.";

        sentinelPropagateDownAfterPeriod(ri);

    // SENTINEL failover-timeout 选项
    } else if (!strcasecmp(argv[0],"failover-timeout") && argc == 3) {

        /* failover-timeout <name> <milliseconds> */

        // 查找 master 
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";

        // 设置选项
        ri->failover_timeout = atoi(argv[2]);
        if (ri->failover_timeout <= 0)
            return "negative or zero time parameter.";

   // Sentinel parallel-syncs 选项
   } else if (!strcasecmp(argv[0],"parallel-syncs") && argc == 3) {

        /* parallel-syncs <name> <milliseconds> */

        // 查找 master 
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";

        // 设置选项
        ri->parallel_syncs = atoi(argv[2]);

    // SENTINEL notification-script 选项
   } else if (!strcasecmp(argv[0],"notification-script") && argc == 3) {

        /* notification-script <name> <path> */
        
        // 查找 master 
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";

        // 检查给定路径所指向的文件是否存在，以及是否可执行
        if (access(argv[2],X_OK) == -1)
            return "Notification script seems non existing or non executable.";

        // 设置选项
        ri->notification_script = sdsnew(argv[2]);

    // SENTINEL client-reconfig-script 选项
   } else if (!strcasecmp(argv[0],"client-reconfig-script") && argc == 3) {

        /* client-reconfig-script <name> <path> */

        // 查找 master 
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        // 检查给定路径所指向的文件是否存在，以及是否可执行
        if (access(argv[2],X_OK) == -1)
            return "Client reconfiguration script seems non existing or "
                   "non executable.";

        // 设置选项
        ri->client_reconfig_script = sdsnew(argv[2]);

    // 设置 SENTINEL auth-pass 选项
   } else if (!strcasecmp(argv[0],"auth-pass") && argc == 3) {

        /* auth-pass <name> <password> */

        // 查找 master 
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";

        // 设置选项
        ri->auth_pass = sdsnew(argv[2]);

    } else if (!strcasecmp(argv[0],"current-epoch") && argc == 2) {
        /* current-epoch <epoch> */
        unsigned long long current_epoch = strtoull(argv[1],NULL,10);
        if (current_epoch > sentinel.current_epoch)
            sentinel.current_epoch = current_epoch;

    // SENTINEL config-epoch 选项
    } else if (!strcasecmp(argv[0],"config-epoch") && argc == 3) {

        /* config-epoch <name> <epoch> */

        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";

        ri->config_epoch = strtoull(argv[2],NULL,10);
        /* The following update of current_epoch is not really useful as
         * now the current epoch is persisted on the config file, but
         * we leave this check here for redundancy. */
        if (ri->config_epoch > sentinel.current_epoch)
            sentinel.current_epoch = ri->config_epoch;

    } else if (!strcasecmp(argv[0],"leader-epoch") && argc == 3) {
        /* leader-epoch <name> <epoch> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        ri->leader_epoch = strtoull(argv[2],NULL,10);

    // SENTINEL known-slave 选项
    } else if (!strcasecmp(argv[0],"known-slave") && argc == 4) {
        sentinelRedisInstance *slave;

        /* known-slave <name> <ip> <port> */

        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        if ((slave = createSentinelRedisInstance(NULL,SRI_SLAVE,argv[2],
                    atoi(argv[3]), ri->quorum, ri)) == NULL)
        {
            return "Wrong hostname or port for slave.";
        }

    // SENTINEL known-sentinel 选项
    } else if (!strcasecmp(argv[0],"known-sentinel") &&
               (argc == 4 || argc == 5)) {
        sentinelRedisInstance *si;

        /* known-sentinel <name> <ip> <port> [runid] */

        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        if ((si = createSentinelRedisInstance(NULL,SRI_SENTINEL,argv[2],
                    atoi(argv[3]), ri->quorum, ri)) == NULL)
        {
            return "Wrong hostname or port for sentinel.";
        }
        if (argc == 5) si->runid = sdsnew(argv[4]);

    } else {
        return "Unrecognized sentinel configuration statement.";
    }
    return NULL;
}

/* Implements CONFIG REWRITE for "sentinel" option.
 * This is used not just to rewrite the configuration given by the user
 * (the configured masters) but also in order to retain the state of
 * Sentinel across restarts: config epoch of masters, associated slaves
 * and sentinel instances, and so forth. */
// CONFIG REWIRTE 命令中和 sentinel 选项有关的部分
// 这个函数不仅用于用户执行 CONFIG REWRITE 的时候，
// 也用于保存 Sentinel 状态，以备 Sentinel 重启时载入状态使用
void rewriteConfigSentinelOption(struct rewriteConfigState *state) {
    dictIterator *di, *di2;
    dictEntry *de;
    sds line;

    /* For every master emit a "sentinel monitor" config entry. */
    di = dictGetIterator(sentinel.masters);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *master, *ri;
        sentinelAddr *master_addr;

        /* sentinel monitor */
        master = dictGetVal(de);
        master_addr = sentinelGetCurrentMasterAddress(master);
        line = sdscatprintf(sdsempty(),"sentinel monitor %s %s %d %d",
            master->name, master_addr->ip, master_addr->port,
            master->quorum);
        rewriteConfigRewriteLine(state,"sentinel",line,1);

        /* sentinel down-after-milliseconds */
        if (master->down_after_period != SENTINEL_DEFAULT_DOWN_AFTER) {
            line = sdscatprintf(sdsempty(),
                "sentinel down-after-milliseconds %s %ld",
                master->name, (long) master->down_after_period);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel failover-timeout */
        if (master->failover_timeout != SENTINEL_DEFAULT_FAILOVER_TIMEOUT) {
            line = sdscatprintf(sdsempty(),
                "sentinel failover-timeout %s %ld",
                master->name, (long) master->failover_timeout);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel parallel-syncs */
        if (master->parallel_syncs != SENTINEL_DEFAULT_PARALLEL_SYNCS) {
            line = sdscatprintf(sdsempty(),
                "sentinel parallel-syncs %s %d",
                master->name, master->parallel_syncs);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel notification-script */
        if (master->notification_script) {
            line = sdscatprintf(sdsempty(),
                "sentinel notification-script %s %s",
                master->name, master->notification_script);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel client-reconfig-script */
        if (master->client_reconfig_script) {
            line = sdscatprintf(sdsempty(),
                "sentinel client-reconfig-script %s %s",
                master->name, master->client_reconfig_script);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel auth-pass */
        if (master->auth_pass) {
            line = sdscatprintf(sdsempty(),
                "sentinel auth-pass %s %s",
                master->name, master->auth_pass);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel config-epoch */
        line = sdscatprintf(sdsempty(),
            "sentinel config-epoch %s %llu",
            master->name, (unsigned long long) master->config_epoch);
        rewriteConfigRewriteLine(state,"sentinel",line,1);

        /* sentinel leader-epoch */
        line = sdscatprintf(sdsempty(),
            "sentinel leader-epoch %s %llu",
            master->name, (unsigned long long) master->leader_epoch);
        rewriteConfigRewriteLine(state,"sentinel",line,1);

        /* sentinel known-slave */
        di2 = dictGetIterator(master->slaves);
        while((de = dictNext(di2)) != NULL) {
            sentinelAddr *slave_addr;

            ri = dictGetVal(de);
            slave_addr = ri->addr;

            /* If master_addr (obtained using sentinelGetCurrentMasterAddress()
             * so it may be the address of the promoted slave) is equal to this
             * slave's address, a failover is in progress and the slave was
             * already successfully promoted. So as the address of this slave
             * we use the old master address instead. */
            if (sentinelAddrIsEqual(slave_addr,master_addr))
                slave_addr = master->addr;
            line = sdscatprintf(sdsempty(),
                "sentinel known-slave %s %s %d",
                master->name, ri->addr->ip, ri->addr->port);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }
        dictReleaseIterator(di2);

        /* sentinel known-sentinel */
        di2 = dictGetIterator(master->sentinels);
        while((de = dictNext(di2)) != NULL) {
            ri = dictGetVal(de);
            line = sdscatprintf(sdsempty(),
                "sentinel known-sentinel %s %s %d%s%s",
                master->name, ri->addr->ip, ri->addr->port,
                ri->runid ? " " : "",
                ri->runid ? ri->runid : "");
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }
        dictReleaseIterator(di2);
    }

    /* sentinel current-epoch is a global state valid for all the masters. */
    line = sdscatprintf(sdsempty(),
        "sentinel current-epoch %llu", (unsigned long long) sentinel.current_epoch);
    rewriteConfigRewriteLine(state,"sentinel",line,1);

    dictReleaseIterator(di);
}

/* This function uses the config rewriting Redis engine in order to persist
 * the state of the Sentinel in the current configuration file.
 *
 * 使用 CONFIG REWRITE 功能，将当前 Sentinel 的状态持久化到配置文件里面。
 *
 * Before returning the function calls fsync() against the generated
 * configuration file to make sure changes are committed to disk.
 *
 * 在函数返回之前，程序会调用一次 fsync() ，确保文件已经被保存到磁盘里面。
 *
 * On failure the function logs a warning on the Redis log. 
 *
 * 如果保存失败，那么打印一条警告日志。
 */
void sentinelFlushConfig(void) {
    int fd = -1;
    int saved_hz = server.hz;
    int rewrite_status;

    server.hz = REDIS_DEFAULT_HZ;
    rewrite_status = rewriteConfig(server.configfile);
    server.hz = saved_hz;

    if (rewrite_status == -1) goto werr;
    if ((fd = open(server.configfile,O_RDONLY)) == -1) goto werr;
    if (fsync(fd) == -1) goto werr;
    if (close(fd) == EOF) goto werr;
    return;

werr:
    if (fd != -1) close(fd);
    redisLog(REDIS_WARNING,"WARNING: Sentinel was not able to save the new configuration on disk!!!: %s", strerror(errno));
}

/* ====================== hiredis connection handling（适配层） ======================= */

/* Completely disconnect a hiredis link from an instance. */
// 断开 instance 的连接
void sentinelKillLink(sentinelRedisInstance *ri, redisAsyncContext *c) {
    if (ri->cc == c) {
        ri->cc = NULL;
        ri->pending_commands = 0;
    }
    if (ri->pc == c) ri->pc = NULL;
    c->data = NULL;

    // 打开断线标志
    ri->flags |= SRI_DISCONNECTED;

    // 断开连接
    redisAsyncFree(c);
}

/* This function takes a hiredis context that is in an error condition
 * and make sure to mark the instance as disconnected performing the
 * cleanup needed.
 *
 * 函数将一个出错连接设置正确的断线标志，并执行清理操作
 *
 * Note: we don't free the hiredis context as hiredis will do it for us
 * for async connections. 
 *
 * 这个函数没有手动释放连接，因为异步连接会自动释放
 */
void sentinelDisconnectInstanceFromContext(const redisAsyncContext *c) {
    sentinelRedisInstance *ri = c->data;
    int pubsub;

    if (ri == NULL) return; /* The instance no longer exists. */

    // 发送断线事件
    pubsub = (ri->pc == c);
    sentinelEvent(REDIS_DEBUG, pubsub ? "-pubsub-link" : "-cmd-link", ri,
        "%@ #%s", c->errstr);

    if (pubsub)
        ri->pc = NULL;
    else
        ri->cc = NULL;

    // 打开标志
    ri->flags |= SRI_DISCONNECTED;
}

// 异步连接的连接回调函数
// epoll-instance ---> redisAsyncHandleWrite() ---> __redisAsyncHandleConnect() 
// ---> ac->onConnect() ---> sentinelLinkEstablishedCallback()
void sentinelLinkEstablishedCallback(const redisAsyncContext *c, int status) {
    if (status != REDIS_OK) {
        sentinelDisconnectInstanceFromContext(c);
    } else {
        sentinelRedisInstance *ri = c->data;
        int pubsub = (ri->pc == c);

        // 发送连接事件
        sentinelEvent(REDIS_DEBUG, pubsub ? "+pubsub-link" : "+cmd-link", ri,
            "%@");
    }
}

// 异步连接的断线回调函数
void sentinelDisconnectCallback(const redisAsyncContext *c, int status) {
    sentinelDisconnectInstanceFromContext(c);
}

/* Send the AUTH command with the specified master password if needed.
 * Note that for slaves the password set for the master is used.
 *
 * 如果 sentinel 设置了 auth-pass 选项，那么向 master 或者 slave 发送验证密码。
 * 注意 slave 使用的是 master 的密码。
 *
 * We don't check at all if the command was successfully transmitted
 * to the instance as if it fails Sentinel will detect the instance down,
 * will disconnect and reconnect the link and so forth. 
 *
 * 函数不检查命令是否被成功发送，因为如果目标服务器掉线了的话， sentinel 会识别到，
 * 并对它进行重连接，然后又重新发送 AUTH 命令。
 */
void sentinelSendAuthIfNeeded(sentinelRedisInstance *ri, redisAsyncContext *c) {

    // 如果 ri 是 master ，那么使用 instance 自己的密码
    // 如果 ri 是 slave ，那么使用 master 的密码
    char *auth_pass = (ri->flags & SRI_MASTER) ? ri->auth_pass :
                                                 ri->master->auth_pass;

    // 发送 AUTH 命令
    if (auth_pass) {
        if (redisAsyncCommand(c, sentinelDiscardReplyCallback, NULL, "AUTH %s",
            auth_pass) == REDIS_OK) ri->pending_commands++;
    }
}

/* Use CLIENT SETNAME to name the connection in the Redis instance as
 * sentinel-<first_8_chars_of_runid>-<connection_type>
 * The connection type is "cmd" or "pubsub" as specified by 'type'.
 *
 * This makes it possible to list all the sentinel instances connected
 * to a Redis servewr with CLIENT LIST, grepping for a specific name format. */
// 使用 CLIENT SETNAME 命令，为给定的客户端设置名字。
void sentinelSetClientName(sentinelRedisInstance *ri, redisAsyncContext *c, char *type) {
    char name[64];

    snprintf(name,sizeof(name),"sentinel-%.8s-%s",server.runid,type);
    if (redisAsyncCommand(c, sentinelDiscardReplyCallback, NULL,
        "CLIENT SETNAME %s", name) == REDIS_OK)
    {
        ri->pending_commands++;
    }
}

/* Create the async connections for the specified instance if the instance
 * is disconnected. Note that the SRI_DISCONNECTED flag is set even if just
 * one of the two links (commands and pub/sub) is missing. */
// 如果 sentinel 与 instance 处于断线（未连接）状态，那么创建连向 instance 的异步连接。
// SRI_DISCONNECTED flag is set even if just one of the two links (commands and pub/sub) is missing.
// 每一个 sentinel 跟一个被监控的 master redis-server 之间，是会建立两个不同的连接的
// 分别用于 sentinel handle CMD；sentinel subscribe information from redis-server
void sentinelReconnectInstance(sentinelRedisInstance *ri) {

    // 示例未断线（已连接），返回
    if (!(ri->flags & SRI_DISCONNECTED)) return;

    /* Commands connection. */
    // 对所有 instance 创建一个用于发送 Redis 命令的连接
    // TODO:(DONE) 对于 sentinel 跟 sentinel 之间会不会建立 command context ？ 会，不然怎么 PING、PONG 呢
    if (ri->cc == NULL) {

        // 连接 instance （采用异步 connect 的方式，实现上现在并不知道是不是成功进行三次握手的）
        ri->cc = redisAsyncConnect(ri->addr->ip,ri->addr->port);

        // 连接出错
        if (ri->cc->err) {
            sentinelEvent(REDIS_DEBUG,"-cmd-link-reconnection",ri,"%@ #%s",
                ri->cc->errstr);
            sentinelKillLink(ri,ri->cc);

        // 连接成功（仅仅只是发起 connect 这个过程没有发生 error）
        } else {
            // 设置连接属性
            ri->cc_conn_time = mstime();
            ri->cc->data = ri;
            redisAeAttach(server.el,ri->cc);
            // 设置连线 callback(同时向 epoll-instance 注册 write 事件)
            redisAsyncSetConnectCallback(ri->cc,
                                            sentinelLinkEstablishedCallback);
            // 设置断线 callback(这个一般是收到对端要求断开、连接 broken、本端主动断开)
            redisAsyncSetDisconnectCallback(ri->cc,
                                            sentinelDisconnectCallback);
            // 发送 AUTH 命令，验证身份
            sentinelSendAuthIfNeeded(ri,ri->cc);
            sentinelSetClientName(ri,ri->cc,"cmd");

            /* Send a PING ASAP when reconnecting. */
            sentinelSendPing(ri);
        }
    }

    /* Pub / Sub */
    // 针对跟 sentinel 连接的 master、slave role redis-server，创建一个用于订阅频道的连接
    // 并不针对 sentinel 跟 sentinel 之间建立 pub\sub context 连接
    if ((ri->flags & (SRI_MASTER|SRI_SLAVE)) && ri->pc == NULL) {

        // 连接 instance 
        ri->pc = redisAsyncConnect(ri->addr->ip,ri->addr->port);

        // 连接出错
        if (ri->pc->err) {
            sentinelEvent(REDIS_DEBUG,"-pubsub-link-reconnection",ri,"%@ #%s",
                ri->pc->errstr);
            sentinelKillLink(ri,ri->pc);

        // 连接成功
        } else {
            int retval;

            // 设置连接属性
            ri->pc_conn_time = mstime();
            ri->pc->data = ri;
            redisAeAttach(server.el,ri->pc);
            // 设置连接 callback
            redisAsyncSetConnectCallback(ri->pc,
                                            sentinelLinkEstablishedCallback);
            // 设置断线 callback
            redisAsyncSetDisconnectCallback(ri->pc,
                                            sentinelDisconnectCallback);
            // 发送 AUTH 命令，验证身份
            sentinelSendAuthIfNeeded(ri,ri->pc);

            // 为客户但设置名字 "pubsub"
            sentinelSetClientName(ri,ri->pc,"pubsub");

            /* Now we subscribe to the Sentinels "Hello" channel. */
            // 发送 SUBSCRIBE __sentinel__:hello 命令，订阅频道
            // 注意，是 sentinel 向每一个 master\slave 注册自己要监听 __sentinel__:hello 这个 channel
            /* 然后，每当有 sentinel 向 __sentinel__:hello 发出 hello 信息的
             * 时候，就会被 master 收到，然后 master 发现是 publish 到 __sentinel__:hello 的，然后就会向跟自己
             * 订阅了这个 channel 的 sentinel\salve 广播这个 hello message
             * 当另一个连接上了这个 master 的 sentinel 在接收到 hello 之后，发现这是一个自己所不知道的 sentinel
             * 就会尝试跟他建立连接（参考 sentinelReceiveHelloMessages() 函数）
             */
            retval = redisAsyncCommand(ri->pc,
                sentinelReceiveHelloMessages, NULL, "SUBSCRIBE %s",
                    SENTINEL_HELLO_CHANNEL);
            
            // 订阅出错，断开连接
            if (retval != REDIS_OK) {
                /* If we can't subscribe, the Pub/Sub connection is useless
                 * and we can simply disconnect it and try again. */
                sentinelKillLink(ri,ri->pc);
                return;
            }
        }
    }

    /* Clear the DISCONNECTED flags only if we have both the connections
     * (or just the commands connection if this is a sentinel instance). */
    // 如果 instance 是 master 或者 slave ，那么当 cc 和 pc 两个连接都创建成功时，关闭 DISCONNECTED 标识
    // 如果 instance 是 Sentinel ，那么当 cc 连接创建成功时，关闭 DISCONNECTED 标识
    if (ri->cc && (ri->flags & SRI_SENTINEL || ri->pc))
        ri->flags &= ~SRI_DISCONNECTED;
}

/* ======================== Redis instances pinging  ======================== */

/* Return true if master looks "sane", that is:
 *
 * 如果 master 看上去是合理（sane），那么返回真。判断是否合理的条件如下：
 *
 * 1) It is actually a master in the current configuration.
 *    它在当前配置中的角色为 master 
 * 2) It reports itself as a master.
 *    它报告自己是一个 master 
 * 3) It is not SDOWN or ODOWN.
 *    这个 master 不处于 SDOWN 或者 ODOWN 状态
 * 4) We obtained last INFO no more than two times the INFO period time ago. 
 *     master 最近一次刷新 INFO 信息距离现在不超过 SENTINEL_INFO_PERIOD 的两倍时间
 */
int sentinelMasterLooksSane(sentinelRedisInstance *master) {
    return
        master->flags & SRI_MASTER &&
        master->role_reported == SRI_MASTER &&
        (master->flags & (SRI_S_DOWN|SRI_O_DOWN)) == 0 &&
        (mstime() - master->info_refresh) < SENTINEL_INFO_PERIOD*2;
}

/* Process the INFO output from masters. */
// 从 master 或者 slave 所返回的 INFO 命令的回复中分析相关信息
// （上面的英文注释错了，这个函数不仅处理 master 的 INFO 回复，还处理 slave 的 INFO 回复）
/* 目前改函数关系的信息：
 * 对应 redis-server 的 run_id，"run_id:5e17f0465181b297c1637ffc6b96aa32f00f2a03"
 * 针对 master 的 redis-server，解析返回信息中的 slave ip + port，"slave0:ip=172.18.96.83,port=6379,state=online,offset=918,lag=0"
 * 
 */
// 这是一个很重要的函数, 是 sentinel 知晓, 更新 master\slave INFO 状态信息的重要途经
// sentinel 会根据 INFO 信息的改变, 进而改变相应的 SRI 状态, 进而触发状态机的改变, 处理流程的改变
void sentinelRefreshInstanceInfo(sentinelRedisInstance *ri, const char *info) {
    sds *lines;
    int numlines, j;
    int role = 0;

    /* The following fields must be reset to a given value in the case they
     * are not found at all in the INFO output. */
    // 将该变量重置为 0 ，避免 INFO 回复中无该值的情况
    ri->master_link_down_time = 0;

    /* Process line by line. */
    // 对 INFO 命令的回复进行逐行分析
    lines = sdssplitlen(info,strlen(info),"\r\n",2,&numlines);
    for (j = 0; j < numlines; j++) {
        sentinelRedisInstance *slave;
        sds l = lines[j];   // 更新待分析的 INFO 信息行

        /* run_id:<40 hex chars>*/
        // 读取并分析 runid
        if (sdslen(l) >= 47 && !memcmp(l,"run_id:",7)) {

            // 新设置 runid
            if (ri->runid == NULL) {
                ri->runid = sdsnewlen(l+7,40);
            } else {
                // RUNID 不同，说明服务器已重启
                if (strncmp(ri->runid,l+7,40) != 0) {
                    sentinelEvent(REDIS_NOTICE,"+reboot",ri,"%@");

                    // 释放旧 ID ，设置新 ID
                    sdsfree(ri->runid);
                    ri->runid = sdsnewlen(l+7,40);
                }
            }
        }

        // 读取 slave 的 ip 和端口号
        /* old versions: slave0:<ip>,<port>,<state>
         * new versions: slave0:ip=127.0.0.1,port=9999,... */
        if ((ri->flags & SRI_MASTER) &&
            sdslen(l) >= 7 &&
            !memcmp(l,"slave",5) && isdigit(l[5]))
        {
            char *ip, *port, *end;

            if (strstr(l,"ip=") == NULL) {
                /* Old format. */
                ip = strchr(l,':'); if (!ip) continue;
                ip++; /* Now ip points to start of ip address. */
                port = strchr(ip,','); if (!port) continue;
                *port = '\0'; /* nul term for easy access. */
                port++; /* Now port points to start of port number. */
                end = strchr(port,','); if (!end) continue;
                *end = '\0'; /* nul term for easy access. */
            } else {
                /* New format. */
                ip = strstr(l,"ip="); if (!ip) continue;
                ip += 3; /* Now ip points to start of ip address. */
                port = strstr(l,"port="); if (!port) continue;
                port += 5; /* Now port points to start of port number. */
                /* Nul term both fields for easy access. */
                end = strchr(ip,','); if (end) *end = '\0';
                end = strchr(port,','); if (end) *end = '\0';
            }

            /* Check if we already have this slave into our table,
             * otherwise add it. */
            // 如果发现有新的 slave 出现，那么为它添加 instance 
            // createSentinelRedisInstance() 的时候，就会将 slave-rsi 加入 master->slaves 的 dict 里面，以备日后去重
            // 也就是 sentinelRedisInstanceLookupSlave() 会用到
            // 然后等待 cron 的时候，开始尝试发起连接
            if (sentinelRedisInstanceLookupSlave(ri,ip,atoi(port)) == NULL) {
                if ((slave = createSentinelRedisInstance(NULL,SRI_SLAVE,ip,
                            atoi(port), ri->quorum, ri)) != NULL)
                {
                    sentinelEvent(REDIS_NOTICE,"+slave",slave,"%@");
                }
            }
        }

        /* master_link_down_since_seconds:<seconds> */
        // 读取 master <---> slave 的断线时长
        // 这个只会在 instance 是 slave ，并且主从连接断开的情况下出现
        // 当 slave != REDIS_REPL_CONNECTED 的时候, slave 才会通过 info CMD 通报这个信息
        if (sdslen(l) >= 32 &&
            !memcmp(l,"master_link_down_since_seconds",30))
        {
            ri->master_link_down_time = strtoll(l+31,NULL,10)*1000;
            // = server.unixtime - server.repl_down_since
            // slave 自己计算了: 从跟 master 失联, 到现在, 经过了多长的时间
        }

        /* role:<role> */
        // 读取 instance 的角色
        if (!memcmp(l,"role:master",11)) role = SRI_MASTER;
        else if (!memcmp(l,"role:slave",10)) role = SRI_SLAVE;

        // 处理 slave 
        if (role == SRI_SLAVE) {

            /* master_host:<host> */
            // 读入 master 的 IP
            if (sdslen(l) >= 12 && !memcmp(l,"master_host:",12)) {
                if (ri->slave_master_host == NULL ||
                    strcasecmp(l+12,ri->slave_master_host))
                {
                    sdsfree(ri->slave_master_host);
                    ri->slave_master_host = sdsnew(l+12);
                    ri->slave_conf_change_time = mstime();
                }
            }

            /* master_port:<port> */
            // 读入 master 的端口号
            if (sdslen(l) >= 12 && !memcmp(l,"master_port:",12)) {
                int slave_master_port = atoi(l+12);

                if (ri->slave_master_port != slave_master_port) {
                    ri->slave_master_port = slave_master_port;
                    ri->slave_conf_change_time = mstime();
                }
            }
            
            /* master_link_status:<status> */
            // 读入 master 的状态
            if (sdslen(l) >= 19 && !memcmp(l,"master_link_status:",19)) {
                ri->slave_master_link_status =
                    (strcasecmp(l+19,"up") == 0) ?
                    SENTINEL_MASTER_LINK_STATUS_UP :
                    SENTINEL_MASTER_LINK_STATUS_DOWN;
            }

            /* slave_priority:<priority> */
            // 读入 slave 的优先级
            if (sdslen(l) >= 15 && !memcmp(l,"slave_priority:",15))
                ri->slave_priority = atoi(l+15);

            /* slave_repl_offset:<offset> */
            // 读入 slave 的复制偏移量
            if (sdslen(l) >= 18 && !memcmp(l,"slave_repl_offset:",18))
                ri->slave_repl_offset = strtoull(l+18,NULL,10);
        }
    }

    // 更新刷新 INFO 命令回复的时间
    ri->info_refresh = mstime();
    sdsfreesplitres(lines,numlines);

    /* ---------------------------- Acting half -----------------------------
     * Some things will not happen if sentinel.tilt is true, but some will
     * still be processed. 
     *
     * 如果 sentinel 进入了 TILT 模式，那么可能只有一部分动作会被执行
     */

    /* Remember when the role changed. */
    if (role != ri->role_reported) {
        ri->role_reported_time = mstime();
        ri->role_reported = role;
        if (role == SRI_SLAVE) ri->slave_conf_change_time = mstime();
        /* Log the event with +role-change if the new role is coherent or
         * with -role-change if there is a mismatch with the current config. */
        sentinelEvent(REDIS_VERBOSE,
            ((ri->flags & (SRI_MASTER|SRI_SLAVE)) == role) ?
            "+role-change" : "-role-change",
            ri, "%@ new reported role is %s",
            role == SRI_MASTER ? "master" : "slave",
            ri->flags & SRI_MASTER ? "master" : "slave");
    }

    /* None of the following conditions are processed when in tilt mode, so
     * return asap. */
    // 如果 Sentinel 正处于 TILT 模式，那么它不能执行以下的语句。
    if (sentinel.tilt) return;

    /* Handle master -> slave role switch. */
    //  instance 被 Sentinel 标识为 master ，但根据 INFO 命令的回复
    // 这个 instance 的身份为 slave 
    if ((ri->flags & SRI_MASTER) && role == SRI_SLAVE) {
        /* Nothing to do, but masters claiming to be slaves are
         * considered to be unreachable by Sentinel, so eventually
         * a failover will be triggered. */
        // 如果一个 master 变为 slave ，那么 Sentinel 将这个 master 看作是不可用的
    }

    /* Handle slave -> master role switch. */
    // 处理 slave 转变为 master 的情况
    // sentinel 还认为这个 instance 是 slave; 但是通过这个 instance 的 INFO reply 得知: 这个 instance 已经完成了 SLAVEOF NO ONE
    // 已经 promote 为 master, sentinel 要更新自己对于这个 sentinel 的看法
    if ((ri->flags & SRI_SLAVE) && role == SRI_MASTER) {
        /* If this is a promoted slave we can change state to the
         * failover state machine. */

        // 如果这是被选中升级为新 master 的 slave 
        // 那么更新相关的故障转移属性
        // 只有 leader 才会被 set 相应的 FAILOVER flag, 也就只有 slave 会因为 role 的变化而作出反应
        if ((ri->master->flags & SRI_FAILOVER_IN_PROGRESS) &&
            (ri->master->failover_state ==
                SENTINEL_FAILOVER_STATE_WAIT_PROMOTION))
        {
            /* Now that we are sure the slave was reconfigured as a master
             * set the master configuration epoch to the epoch we won the
             * election to perform this failover. This will force the other
             * Sentinels to update their config (assuming there is not
             * a newer one already available). */
            // 这是一个被 Sentinel 发送 SLAVEOF no one 之后由 slave 变为 master 的 instance(ri)
            // 将这个新 master 的配置纪元设置为 Sentinel 赢得 leader 选举的纪元(TODO: why? 哪怕其他 sentinel 的 epoch 更大?)
            // 这一操作会强制其他 Sentinel 更新它们自己的配置
            // （假设没有一个更新的纪元存在的话, TODO: 有更新的话, 会发生什么 ?）
            // 更新 ri(slave, new master) 的旧 master （已下线）的配置纪元
            ri->master->config_epoch = ri->master->failover_epoch;
            // 设置 slave 的 master （已下线）的故障转移状态
            // 这个状态会让 slave 开始同步新的 master 
            // 剩下的交给 状态机 完成
            ri->master->failover_state = SENTINEL_FAILOVER_STATE_RECONF_SLAVES;
            // 更新 slave 的 master （已下线）的故障转移状态变更时间
            ri->master->failover_state_change_time = mstime();
            // 将当前 Sentinel 状态保存到配置文件里面
            sentinelFlushConfig();
            // 发送事件
            sentinelEvent(REDIS_WARNING,"+promoted-slave",ri,"%@");
            sentinelEvent(REDIS_WARNING,"+failover-state-reconf-slaves",
                ri->master,"%@");
            // 执行脚本
            sentinelCallClientReconfScript(ri->master,SENTINEL_LEADER,
                "start",ri->master->addr,ri->addr);

        // 这个 instance 由 slave 变为了 master ，并且没有进入 TILT 模式
        // （可能是因为重启造成的，或者之前的下线 master 重新上线了）
        // TODO: 看看这个 case
        } else {
            /* A slave turned into a master. We want to force our view and
             * reconfigure as slave. Wait some time after the change before
             * going forward, to receive new configs if any. */
            // 如果一个 slave 变为了 master ，那么我们会考虑将它变回一个 slave 

            // 将 PUBLISH 命令的发送时间乘以 4 ，给于一定缓冲时间
            mstime_t wait_time = SENTINEL_PUBLISH_PERIOD*4;

            // 如果这个 instance 的 master 运作正常
            // 并且 instance 在一段时间内没有进入过 SDOWN 状态或者 ODOWN 状态
            // 并且 instance 报告它是 master 的时间已经超过 wait_time
            if (sentinelMasterLooksSane(ri->master) &&
               sentinelRedisInstanceNoDownFor(ri,wait_time) &&
               mstime() - ri->role_reported_time > wait_time)
            {
                // 重新将 instance 设置为 slave 
                int retval = sentinelSendSlaveOf(ri,
                        ri->master->addr->ip,
                        ri->master->addr->port);
                
                // 发送事件
                if (retval == REDIS_OK)
                    sentinelEvent(REDIS_NOTICE,"+convert-to-slave",ri,"%@");
            }
        }
    }

    /* Handle slaves replicating to a different master address. */
    // 让 slave 重新复制回正确的 master 
    if ((ri->flags & SRI_SLAVE) &&
        role == SRI_SLAVE &&
        //  slave 现在的 master 地址和 Sentinel 保存的信息不一致
        (ri->slave_master_port != ri->master->addr->port ||
         strcasecmp(ri->slave_master_host,ri->master->addr->ip)))
    {
        mstime_t wait_time = ri->master->failover_timeout;

        /* Make sure the master is sane before reconfiguring this instance
         * into a slave. */
        // 1) 检查 instance 的 master 状态是否正常
        // 2) 检查 instance 在给定时间内是否进入过 SDOWN 或者 ODOWN 状态
        // 3) 检查 instance 身份变更的时长是否已经超过了指定时长
        // 如果是的话，执行代码。。。
        if (sentinelMasterLooksSane(ri->master) &&
            sentinelRedisInstanceNoDownFor(ri,wait_time) &&
            mstime() - ri->slave_conf_change_time > wait_time)
        {
            // 重新将 instance 指向原本的 master 
            int retval = sentinelSendSlaveOf(ri,
                    ri->master->addr->ip,
                    ri->master->addr->port);

            if (retval == REDIS_OK)
                sentinelEvent(REDIS_NOTICE,"+fix-slave-config",ri,"%@");
        }
    }

    /* Detect if the slave that is in the process of being reconfigured
     * changed state.**(part of failover)** */
    // Sentinel 监视的 instance 为 slave ，并且已经向它发送 SLAVEOF 命令
    // 分为两个阶段: 1. slave 接受了 SLAVEOF CMD, 并且开始进行 sync; 2. slave 完成了 sync, 可以开始正常工作, slave <---> naster up
    if ((ri->flags & SRI_SLAVE) && role == SRI_SLAVE &&
        (ri->flags & (SRI_RECONF_SENT|SRI_RECONF_INPROG)))
    {
        /* SRI_RECONF_SENT -> SRI_RECONF_INPROG. */
        // 将 SENT 状态改为 INPROG 状态，表示同步正在进行
        if ((ri->flags & SRI_RECONF_SENT) &&
            ri->slave_master_host &&
            strcmp(ri->slave_master_host,
                    ri->master->promoted_slave->addr->ip) == 0 &&   // 检查这个 slave 是否成功执行 SLAVEOF new-master
            ri->slave_master_port == ri->master->promoted_slave->addr->port)
        {
            ri->flags &= ~SRI_RECONF_SENT;
            ri->flags |= SRI_RECONF_INPROG;
            sentinelEvent(REDIS_NOTICE,"+slave-reconf-inprog",ri,"%@");
        }

        /* SRI_RECONF_INPROG -> SRI_RECONF_DONE */
        // 将 INPROG 状态改为 DONE 状态，表示同步已完成
        // 通过 INFO 了解到 SYNC 完成, slave <---> master 直接可以开始正常 handle request
        if ((ri->flags & SRI_RECONF_INPROG) &&
            ri->slave_master_link_status == SENTINEL_MASTER_LINK_STATUS_UP)
        {
            ri->flags &= ~SRI_RECONF_INPROG;
            ri->flags |= SRI_RECONF_DONE;
            sentinelEvent(REDIS_NOTICE,"+slave-reconf-done",ri,"%@");
        }
        // 剩下的交由状态机进行检查, 因为向下一个状态的切换, 需要所有的 slave 就绪
        // 这里仅仅是一个 slave 就绪了. 参考 sentinelFailoverReconfNextSlave()
    }
}

// 处理 INFO 命令的回复
void sentinelInfoReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;
    redisReply *r;

    if (ri) ri->pending_commands--;
    if (!reply || !ri) return;
    r = reply;

    if (r->type == REDIS_REPLY_STRING) {
        sentinelRefreshInstanceInfo(ri,r->str);
    }
}

/* Just discard the reply. We use this when we are not monitoring the return
 * value of the command but its effects（ri->pending_commands--; 所谓的 effect 就是这个计数器自减） directly. */
// 这个回调函数用于处理不需要检查回复的命令（只使用命令的副作用）
void sentinelDiscardReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;

    if (ri) ri->pending_commands--;
}

// 处理 PING 命令的回复
void sentinelPingReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;
    redisReply *r;

    if (ri) ri->pending_commands--;
    if (!reply || !ri) return;
    r = reply;

    if (r->type == REDIS_REPLY_STATUS ||
        r->type == REDIS_REPLY_ERROR) {

        /* Update the "instance available" field only if this is an
         * acceptable reply. */
        // 只在 instance 返回 acceptable 回复时更新 last_avail_time
        if (strncmp(r->str,"PONG",4) == 0 ||
            strncmp(r->str,"LOADING",7) == 0 ||
            strncmp(r->str,"MASTERDOWN",10) == 0)
        {
            //  instance 运作正常
            ri->last_avail_time = mstime();
            ri->last_ping_time = 0; /* Flag the pong as received. */
            // PING-PONG 的丢失，将会在 sentinelCheckSubjectivelyDown() 发生作用，进而引发 failover
        } else {

            //  instance 运作不正常

            /* Send a SCRIPT KILL command if the instance appears to be
             * down because of a busy script. */
            // 如果服务器因为执行脚本而进入 BUSY 状态，
            // 那么尝试通过发送 SCRIPT KILL 来恢复服务器
            if (strncmp(r->str,"BUSY",4) == 0 &&
                (ri->flags & SRI_S_DOWN) &&
                !(ri->flags & SRI_SCRIPT_KILL_SENT))
            {
                if (redisAsyncCommand(ri->cc,
                        sentinelDiscardReplyCallback, NULL,
                        "SCRIPT KILL") == REDIS_OK)
                    ri->pending_commands++;
                ri->flags |= SRI_SCRIPT_KILL_SENT;
            }
        }
    }

    // 更新 instance 最后一次回复 PING 命令的时间
    ri->last_pong_time = mstime();
}

/* This is called when we get the reply about the PUBLISH command we send
 * to the master to advertise this sentinel. */
// 处理 PUBLISH 命令的回复
void sentinelPublishReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;
    redisReply *r;

    if (ri) ri->pending_commands--;
    if (!reply || !ri) return;
    r = reply;

    /* Only update pub_time if we actually published our message. Otherwise
     * we'll retry against in 100 milliseconds. */
    // 如果命令发送成功，那么更新 last_pub_time
    if (r->type != REDIS_REPLY_ERROR)
        ri->last_pub_time = mstime();
}

/* Process an hello message received via Pub/Sub in master or slave instance,
 * or sent directly to this sentinel via the (fake) PUBLISH command of Sentinel.
 *
 * 来源一：sentinel 向 master\slave publish，然后由 master\salve 广播给当前 sentinel
 * 来源二：sentinel 直接通过 publish 广播给当前 sentinel
 *
 * If the master name specified in the message is not known, the message is
 * discareded. 
 *
 * 如果消息里面指定的 master 的名字是未知的，那么这条消息将被丢弃。（显然，一个 sentinel 是可以为多套不同的 redis 主从服务的）
 */
// char *hello = 5.5.75.109,26110,8a0c2e23ff0a918c7b10e3733731fa479eff9f23,0,SER,5.5.75.109,11109,0
void sentinelProcessHelloMessage(char *hello, int hello_len) {
    /* Format is composed of 8 tokens:
     * 0=ip,1=port,2=runid,3=current_epoch,4=master_name,
     * 5=master_ip,6=master_port,7=master_config_epoch. */
    int numtokens, port, removed, master_port;
    uint64_t current_epoch, master_config_epoch;
    char **token = sdssplitlen(hello, hello_len, ",", 1, &numtokens);
    sentinelRedisInstance *si, *master;

    if (numtokens == 8) {
        /* Obtain a reference to the master this hello message is about */
        // 获取 master 的名字，并丢弃和未知 master 相关的消息。
        master = sentinelGetMasterByName(token[4]);
        if (!master) goto cleanup; /* Unknown master, skip the message. */

        /* First, try to see if we already have this sentinel. */
        // 看这个 Sentinel 是否已经认识发送消息的 Sentinel
        port = atoi(token[1]);
        master_port = atoi(token[6]);
        /* 因为是根据 runid + ip:port 的方式对一个 sentinel 进行判重，这将会导致 si == NULL 包含两种情况：
         * 1. ip:port 相同，runid 不同 ===> sentinel 发生过重启，要 reset 对应的 RSI
         * 2. ip:port 不同，runid 相同 ===> sentinel 的网络发生了变化，也是要重新连接的
         * 3. ip:port、runid 都不相同  ===> 必然是要重连的
         */
        si = getSentinelRedisInstanceByAddrAndRunID(
                        master->sentinels,token[0],port,token[2]);
        current_epoch = strtoull(token[3],NULL,10);
        master_config_epoch = strtoull(token[7],NULL,10);   // TODO: 为什么还要广播这个 master_config_epoch ？什么时候会发生改变？具有什么含义、作用？

        if (!si) {

            // 这个 Sentinel 不认识发送消息的 Sentinel 
            // 将对方加入到 Sentinel 列表中

            /* If not, remove all the sentinels that have the same runid
             * OR the same ip/port, because it's either a restart or a
             * network topology change. */
            // 这个 remove 操作对于 vote 来说很重要，因为 sentinel 的唯一，是投票基数有效的前提
            removed = removeMatchingSentinelsFromMaster(master,token[0],port,
                            token[2]);
            if (removed) {
                sentinelEvent(REDIS_NOTICE,"-dup-sentinel",master,
                    "%@ #duplicate of %s:%d or %s",
                    token[0],port,token[2]);
            }

            /* Add the new sentinel. */
            si = createSentinelRedisInstance(NULL,SRI_SENTINEL,
                            token[0],port,master->quorum,master);
            if (si) {
                sentinelEvent(REDIS_NOTICE,"+sentinel",si,"%@");
                /* The runid is NULL after a new instance creation and
                 * for Sentinels we don't have a later chance to fill it,
                 * so do it now. */
                si->runid = sdsnew(token[2]);
                sentinelFlushConfig();
            }
        }

        /* Update local current_epoch if received current_epoch is greater.*/
        // 如果消息中记录的纪元比 Sentinel 当前的纪元要高，那么更新纪元
        if (current_epoch > sentinel.current_epoch) {
            sentinel.current_epoch = current_epoch;
            sentinelFlushConfig();
            sentinelEvent(REDIS_WARNING,"+new-epoch",master,"%llu",
                (unsigned long long) sentinel.current_epoch);
        }

        /* Update master info if received configuration is newer. */
        // 如果消息中记录的配置信息更新，那么对 master 的信息进行更新 
        // TODO: why? 有啥用？不然 sentinel 怎么知道选举, failover 已经完成了, 终止自己重新发起 leader 选举的欲望呢 ?
        // 因为 master 发生了切换
        // master 发生了切换之后, 新 master 的的信息, 将会由 leader sentinel 通过以前的 slave 用 hello 的方式, 广而告之
        if (master->config_epoch < master_config_epoch) {
            master->config_epoch = master_config_epoch;
            if (master_port != master->addr->port ||
                strcmp(master->addr->ip, token[5]))
            {
                sentinelAddr *old_addr;

                sentinelEvent(REDIS_WARNING,"+config-update-from",si,"%@");
                sentinelEvent(REDIS_WARNING,"+switch-master",
                    master,"%s %s %d %s %d",
                    master->name,
                    master->addr->ip, master->addr->port,
                    token[5], master_port);

                old_addr = dupSentinelAddr(master->addr);
                sentinelResetMasterAndChangeAddress(master, token[5], master_port);
                sentinelCallClientReconfScript(master,
                    SENTINEL_OBSERVER,"start",
                    old_addr,master->addr);
                releaseSentinelAddr(old_addr);
            }
        }

        /* Update the state of the Sentinel. */
        // 更新我方 Sentinel 记录的对方 Sentinel 的信息。
        if (si) si->last_hello_time = mstime();
    }

cleanup:
    sdsfreesplitres(token,numtokens);
}


/* This is our Pub/Sub callback for the Hello channel. It's useful in order
 * to discover other sentinels attached at the same master. */
// 此回调函数用于处理 Hello 频道的返回值，它可以发现其他正在订阅同一 master 的 Sentinel
// （因为 master 充当了广播站的中转站，将所有的 sentinel 发出的 hello 都广播出去给其他的 sentinel）
void sentinelReceiveHelloMessages(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;
    redisReply *r;  // 从 redisGetReply() 中解析出来的

    if (!reply || !ri) return;
    r = reply;

    /* Update the last activity in the pubsub channel. Note that since we
     * receive our messages as well this timestamp can be used to detect
     * if the link is probably disconnected even if it seems otherwise. */
    // 更新最后一次接收频道命令的时间
    ri->pc_last_activity = mstime();
   
    /* Sanity check in the reply we expect, so that the code that follows
     * can avoid to check for details. */
    // 只处理频道发来的信息，不处理订阅时和退订时产生的信息
    if (r->type != REDIS_REPLY_ARRAY ||
        r->elements != 3 ||
        r->element[0]->type != REDIS_REPLY_STRING ||
        r->element[1]->type != REDIS_REPLY_STRING ||
        r->element[2]->type != REDIS_REPLY_STRING ||
        strcmp(r->element[0]->str,"message") != 0) return;

    /* We are not interested in meeting ourselves */
    // 只处理非自己发送的信息
    if (strstr(r->element[2]->str,server.runid) != NULL) return;

    sentinelProcessHelloMessage(r->element[2]->str, r->element[2]->len);
}

/* Send an "Hello" message via Pub/Sub to the specified 'ri' Redis
 * instance in order to broadcast the current configuraiton for this
 * master, and to advertise the existence of this Sentinel at the same time.
 *
 * 向给定 ri  instance 的频道发送信息，
 * 从而传播关于给定 master 的配置，
 * 并向其他 Sentinel 宣告本 Sentinel 的存在。
 *
 * The message has the following format:
 *
 * 发送信息的格式如下： 
 *
 * sentinel_ip,sentinel_port,sentinel_runid,current_epoch,
 * master_name,master_ip,master_port,master_config_epoch.
 *
 * Sentinel IP,Sentinel 端口号,Sentinel 的运行 ID,Sentinel 当前的纪元,
 *  master 的名称, master 的 IP, master 的端口号, master 的配置纪元.
 *
 * Returns REDIS_OK if the PUBLISH was queued correctly, otherwise
 * REDIS_ERR is returned. 
 *
 * PUBLISH 命令成功入队时返回 REDIS_OK ，
 * 否则返回 REDIS_ERR 。
 */
int sentinelSendHello(sentinelRedisInstance *ri) {
    char ip[REDIS_IP_STR_LEN];
    char payload[REDIS_IP_STR_LEN+1024];
    int retval;

    // 如果 instance 是 master ，那么使用此 instance 的信息
    // 如果 instance 是 slave ，那么使用这个 slave 的 master 的信息
    sentinelRedisInstance *master = (ri->flags & SRI_MASTER) ? ri : ri->master;

    // 获取地址信息
    sentinelAddr *master_addr = sentinelGetCurrentMasterAddress(master);

    /* Try to obtain our own IP address. */
    // 获取 instance 自身的地址
    if (anetSockName(ri->cc->c.fd,ip,sizeof(ip),NULL) == -1) return REDIS_ERR;
    if (ri->flags & SRI_DISCONNECTED) return REDIS_ERR;

    /* Format and send the Hello message. */
    // 格式化信息
    snprintf(payload,sizeof(payload),
        "%s,%d,%s,%llu," /* Info about this sentinel. */
        "%s,%s,%d,%llu", /* Info about current master. */
        ip, server.port, server.runid,
        (unsigned long long) sentinel.current_epoch,
        /* --- */
        master->name,master_addr->ip,master_addr->port,
        (unsigned long long) master->config_epoch);
    
    // 发送信息
    retval = redisAsyncCommand(ri->cc,
        sentinelPublishReplyCallback, NULL, "PUBLISH %s %s",
            SENTINEL_HELLO_CHANNEL,payload);

    if (retval != REDIS_OK) return REDIS_ERR;

    ri->pending_commands++;

    return REDIS_OK;
}

/* Send a PING to the specified instance and refresh the last_ping_time
 * if it is zero (that is, if we received a pong for the previous ping).
 *
 * On error zero is returned, and we can't consider the PING command
 * queued in the connection. */
// 向指定的 Sentinel 发送 PING 命令。
int sentinelSendPing(sentinelRedisInstance *ri) {
    int retval = redisAsyncCommand(ri->cc,
        sentinelPingReplyCallback, NULL, "PING");
    if (retval == REDIS_OK) {
        ri->pending_commands++;
        /* We update the ping time only if we received the pong for
         * the previous ping, otherwise we are technically waiting
         * since the first ping that did not received a reply. */
        if (ri->last_ping_time == 0) ri->last_ping_time = mstime();
        return 1;
    } else {
        return 0;
    }
}

// 根据时间和 instance 类型等情况，向 instance (master\slave\sentinel)发送命令，比如 INFO 、PING 和 PUBLISH
/* Send periodic PING, INFO, and PUBLISH to the Hello channel to
 * the specified master or slave instance. */
void sentinelSendPeriodicCommands(sentinelRedisInstance *ri) {
    mstime_t now = mstime();
    mstime_t info_period, ping_period;
    int retval;

    /* Return ASAP if we have already a PING or INFO already pending, or
     * in the case the instance is not properly connected. */
    // 函数不能在网络连接未创建时执行
    if (ri->flags & SRI_DISCONNECTED) return;

    /* For INFO, PING, PUBLISH that are not critical commands to send we
     * also have a limit of SENTINEL_MAX_PENDING_COMMANDS. We don't
     * want to use a lot of memory just because a link is not working
     * properly (note that anyway there is a redundant protection about this,
     * that is, the link will be disconnected and reconnected if a long
     * timeout condition is detected. */
    // 为了避免 sentinel 在 instance 处于不正常状态时，发送过多命令
    // sentinel 只在待发送命令的数量未超过 SENTINEL_MAX_PENDING_COMMANDS 常量时
    // 才进行命令发送
    if (ri->pending_commands >= SENTINEL_MAX_PENDING_COMMANDS) return;

    /* If this is a slave of a master in O_DOWN condition we start sending
     * it INFO every second, instead of the usual SENTINEL_INFO_PERIOD
     * period. In this state we want to closely monitor slaves in case they
     * are turned into masters by another Sentinel, or by the sysadmin. */
    // 对于 slave 来说， sentinel 默认每 SENTINEL_INFO_PERIOD 秒向它发送一次 INFO 命令
    // 但是，当 slave 的 master 处于 SDOWN 状态，或者正在执行故障转移时
    // 为了更快速地捕捉 slave 的变动，更及时的获得 slave 的 INFO， sentinel 会将发送 INFO 命令的频率该为每秒一次
    // 以辅助 sentinel 判断每一个 slave 的状态, 选出更好的 new master
    // TODO:(DONE) this case; 当 sentinel 们正在进行 failover 的时候, 需要更新鲜的 slave 信息, 才能够更好提拔 slave 为 new master
    // 变更 sentinel 向 slave 发送 INFO 请求的频率
    if ((ri->flags & SRI_SLAVE) &&
        (ri->master->flags & (SRI_O_DOWN|SRI_FAILOVER_IN_PROGRESS))) {
        info_period = 1000;
    } else {
        info_period = SENTINEL_INFO_PERIOD;
    }
    /* We ping instances every time the last received pong is older than
     * the configured 'down-after-milliseconds' time, but every second
     * anyway if 'down-after-milliseconds' is greater than 1 second. */
    // TODO: this case
    ping_period = ri->down_after_period;
    if (ping_period > SENTINEL_PING_PERIOD) ping_period = SENTINEL_PING_PERIOD;

    //  instance 不是 Sentinel （ master 或者 slave ）
    // 并且以下条件的其中一个成立：
    // 1）SENTINEL 未收到过这个服务器的 INFO 命令回复
    // 2）距离上一次该 instance 回复 INFO 命令已经超过 info_period 间隔
    // 那么向 instance 发送 INFO 命令
    // TODO:（DONE） sentinel 是怎么跟所有的 master、slave、sentinel 分别取得联系的？
    // 详细的请参考抓包，是 sentinel 主动向 master 发起 INFO，然后从里面获取这个 master 的 slave 信息，然后再向 slave 发起连接
    // 解析函数 sentinelRefreshInstanceInfo()
    // 而且这个 INFO 是会定期发送的，不然没办法更新数据
    // TODO:（DONE） 有新的 slave，master 会通知 sentinel 吗？怎么通知？
    // 主要通过自己的 hello 或者是别人的 hello 来发起连接
    if ((ri->flags & SRI_SENTINEL) == 0 &&  // 不会向 sentinel 发送 INFO，没有啥用
        (ri->info_refresh == 0 || (now - ri->info_refresh) > info_period))  // 没发送过 INFO 就要立马发送 || 到了发送 INFO 的周期了
    {
        /* Send INFO to masters and slaves, not sentinels. */
        // 没错, slave 也会被发送 INFO CMD 的,这样才能获取每一个 slave 的信息, 这样才能在 failover 的时候, 抉择哪一个 slave 作为新的 master 更适合
        retval = redisAsyncCommand(ri->cc,
            sentinelInfoReplyCallback, NULL, "INFO");
        if (retval == REDIS_OK) ri->pending_commands++;
    } else if ((now - ri->last_pong_time) > ping_period) {
        /* Send PING to all the three kinds of instances. */
        sentinelSendPing(ri);
    } else if ((now - ri->last_pub_time) > SENTINEL_PUBLISH_PERIOD) {
        /* PUBLISH hello messages to all the three kinds of instances. */
        sentinelSendHello(ri);  
        // TODO:（DONE） sentinel 怎么知道有哪些 master\slave\sentinel 订阅了这个 SENTINEL_HELLO_CHANNEL "__sentinel__:hello" 的 channel ？
        // sentinel 是不知道的，它仅仅是向作为广播站的 master publish 罢了，然后具体向谁广播，是由 master 决定的
        // master 会向跟自己订阅了相关 channel 的 sentinel 发送广播
    }
    // TODO:（DONE） ping 跟 hello 有什么区别？除了种类的区别外，有什么不同的作用吗？
    // 发送 hello 是向其他 sentinel 告知自己这个 sentinel 的存在（只向 master publish specify channel）
    // 发送 ping，是定期检查当前 sentinel 中，每一个连接是否存活（对端可能是 master、slave、sentinel）
}

/* =========================== SENTINEL command ============================= */
// sentinel 系列命令，只能是连接了 sentinel 模式的 redis-server 才能使用
// 这里的函数，是专门处理 redis-cli --> sentinel mode redis-server 的
// 文件配置的话，会走相应的文件配置函数，不是同一套

// 返回字符串表示的故障转移状态
const char *sentinelFailoverStateStr(int state) {
    switch(state) {
    case SENTINEL_FAILOVER_STATE_NONE: return "none";
    case SENTINEL_FAILOVER_STATE_WAIT_START: return "wait_start";
    case SENTINEL_FAILOVER_STATE_SELECT_SLAVE: return "select_slave";
    case SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE: return "send_slaveof_noone";
    case SENTINEL_FAILOVER_STATE_WAIT_PROMOTION: return "wait_promotion";
    case SENTINEL_FAILOVER_STATE_RECONF_SLAVES: return "reconf_slaves";
    case SENTINEL_FAILOVER_STATE_UPDATE_CONFIG: return "update_config";
    default: return "unknown";
    }
}

/* Redis instance to Redis protocol representation. */
// 以 Redis 协议的形式返回 Redis  instance 的情况
void addReplySentinelRedisInstance(redisClient *c, sentinelRedisInstance *ri) {
    char *flags = sdsempty();
    void *mbl;
    int fields = 0;

    mbl = addDeferredMultiBulkLength(c);

    addReplyBulkCString(c,"name");
    addReplyBulkCString(c,ri->name);
    fields++;

    addReplyBulkCString(c,"ip");
    addReplyBulkCString(c,ri->addr->ip);
    fields++;

    addReplyBulkCString(c,"port");
    addReplyBulkLongLong(c,ri->addr->port);
    fields++;

    addReplyBulkCString(c,"runid");
    addReplyBulkCString(c,ri->runid ? ri->runid : "");
    fields++;

    addReplyBulkCString(c,"flags");
    if (ri->flags & SRI_S_DOWN) flags = sdscat(flags,"s_down,");
    if (ri->flags & SRI_O_DOWN) flags = sdscat(flags,"o_down,");
    if (ri->flags & SRI_MASTER) flags = sdscat(flags,"master,");
    if (ri->flags & SRI_SLAVE) flags = sdscat(flags,"slave,");
    if (ri->flags & SRI_SENTINEL) flags = sdscat(flags,"sentinel,");
    if (ri->flags & SRI_DISCONNECTED) flags = sdscat(flags,"disconnected,");
    if (ri->flags & SRI_MASTER_DOWN) flags = sdscat(flags,"master_down,");
    if (ri->flags & SRI_FAILOVER_IN_PROGRESS)
        flags = sdscat(flags,"failover_in_progress,");
    if (ri->flags & SRI_PROMOTED) flags = sdscat(flags,"promoted,");
    if (ri->flags & SRI_RECONF_SENT) flags = sdscat(flags,"reconf_sent,");
    if (ri->flags & SRI_RECONF_INPROG) flags = sdscat(flags,"reconf_inprog,");
    if (ri->flags & SRI_RECONF_DONE) flags = sdscat(flags,"reconf_done,");

    if (sdslen(flags) != 0) sdsrange(flags,0,-2); /* remove last "," */
    addReplyBulkCString(c,flags);
    sdsfree(flags);
    fields++;

    addReplyBulkCString(c,"pending-commands");
    addReplyBulkLongLong(c,ri->pending_commands);
    fields++;

    if (ri->flags & SRI_FAILOVER_IN_PROGRESS) {
        addReplyBulkCString(c,"failover-state");
        addReplyBulkCString(c,(char*)sentinelFailoverStateStr(ri->failover_state));
        fields++;
    }

    addReplyBulkCString(c,"last-ping-sent");
    addReplyBulkLongLong(c,
        ri->last_ping_time ? (mstime() - ri->last_ping_time) : 0);
    fields++;

    addReplyBulkCString(c,"last-ok-ping-reply");
    addReplyBulkLongLong(c,mstime() - ri->last_avail_time);
    fields++;

    addReplyBulkCString(c,"last-ping-reply");
    addReplyBulkLongLong(c,mstime() - ri->last_pong_time);
    fields++;

    if (ri->flags & SRI_S_DOWN) {
        addReplyBulkCString(c,"s-down-time");
        addReplyBulkLongLong(c,mstime()-ri->s_down_since_time);
        fields++;
    }

    if (ri->flags & SRI_O_DOWN) {
        addReplyBulkCString(c,"o-down-time");
        addReplyBulkLongLong(c,mstime()-ri->o_down_since_time);
        fields++;
    }

    addReplyBulkCString(c,"down-after-milliseconds");
    addReplyBulkLongLong(c,ri->down_after_period);
    fields++;

    /* Masters and Slaves */
    if (ri->flags & (SRI_MASTER|SRI_SLAVE)) {
        addReplyBulkCString(c,"info-refresh");
        addReplyBulkLongLong(c,mstime() - ri->info_refresh);
        fields++;

        addReplyBulkCString(c,"role-reported");
        addReplyBulkCString(c, (ri->role_reported == SRI_MASTER) ? "master" :
                                                                   "slave");
        fields++;

        addReplyBulkCString(c,"role-reported-time");
        addReplyBulkLongLong(c,mstime() - ri->role_reported_time);
        fields++;
    }

    /* Only masters */
    if (ri->flags & SRI_MASTER) {
        addReplyBulkCString(c,"config-epoch");
        addReplyBulkLongLong(c,ri->config_epoch);
        fields++;

        addReplyBulkCString(c,"num-slaves");
        addReplyBulkLongLong(c,dictSize(ri->slaves));
        fields++;

        addReplyBulkCString(c,"num-other-sentinels");
        addReplyBulkLongLong(c,dictSize(ri->sentinels));
        fields++;

        addReplyBulkCString(c,"quorum");
        addReplyBulkLongLong(c,ri->quorum);
        fields++;

        addReplyBulkCString(c,"failover-timeout");
        addReplyBulkLongLong(c,ri->failover_timeout);
        fields++;

        addReplyBulkCString(c,"parallel-syncs");
        addReplyBulkLongLong(c,ri->parallel_syncs);
        fields++;

        if (ri->notification_script) {
            addReplyBulkCString(c,"notification-script");
            addReplyBulkCString(c,ri->notification_script);
            fields++;
        }

        if (ri->client_reconfig_script) {
            addReplyBulkCString(c,"client-reconfig-script");
            addReplyBulkCString(c,ri->client_reconfig_script);
            fields++;
        }
    }

    /* Only slaves */
    if (ri->flags & SRI_SLAVE) {
        addReplyBulkCString(c,"master-link-down-time");
        addReplyBulkLongLong(c,ri->master_link_down_time);
        fields++;

        addReplyBulkCString(c,"master-link-status");
        addReplyBulkCString(c,
            (ri->slave_master_link_status == SENTINEL_MASTER_LINK_STATUS_UP) ?
            "ok" : "err");
        fields++;

        addReplyBulkCString(c,"master-host");
        addReplyBulkCString(c,
            ri->slave_master_host ? ri->slave_master_host : "?");
        fields++;

        addReplyBulkCString(c,"master-port");
        addReplyBulkLongLong(c,ri->slave_master_port);
        fields++;

        addReplyBulkCString(c,"slave-priority");
        addReplyBulkLongLong(c,ri->slave_priority);
        fields++;

        addReplyBulkCString(c,"slave-repl-offset");
        addReplyBulkLongLong(c,ri->slave_repl_offset);
        fields++;
    }

    /* Only sentinels */
    if (ri->flags & SRI_SENTINEL) {
        addReplyBulkCString(c,"last-hello-message");
        addReplyBulkLongLong(c,mstime() - ri->last_hello_time);
        fields++;

        addReplyBulkCString(c,"voted-leader");
        addReplyBulkCString(c,ri->leader ? ri->leader : "?");
        fields++;

        addReplyBulkCString(c,"voted-leader-epoch");
        addReplyBulkLongLong(c,ri->leader_epoch);
        fields++;
    }

    setDeferredMultiBulkLength(c,mbl,fields*2);
}

/* Output a number of instances contained inside a dictionary as
 * Redis protocol. */
// 打印各个 instance 的情况
void addReplyDictOfRedisInstances(redisClient *c, dict *instances) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(instances);
    addReplyMultiBulkLen(c,dictSize(instances));
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        addReplySentinelRedisInstance(c,ri);
    }
    dictReleaseIterator(di);
}

/* Lookup the named master into sentinel.masters.
 * If the master is not found reply to the client with an error and returns
 * NULL. */
// 在 sentinel.masters 字典中查找给定名字的 master
// 没找到则返回 NULL
sentinelRedisInstance *sentinelGetMasterByNameOrReplyError(redisClient *c,
                        robj *name)
{
    sentinelRedisInstance *ri;

    ri = dictFetchValue(sentinel.masters,c->argv[2]->ptr);
    if (!ri) {
        addReplyError(c,"No such master with that name");
        return NULL;
    }
    return ri;
}

// SENTINEL 命令的实现
void sentinelCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"masters")) {
        /* SENTINEL MASTERS */
        if (c->argc != 2) goto numargserr;
        addReplyDictOfRedisInstances(c,sentinel.masters);
    } else if (!strcasecmp(c->argv[1]->ptr,"master")) {
        /* SENTINEL MASTER <name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2]))
            == NULL) return;
        addReplySentinelRedisInstance(c,ri);
    } else if (!strcasecmp(c->argv[1]->ptr,"slaves")) {
        /* SENTINEL SLAVES <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2])) == NULL)
            return;
        addReplyDictOfRedisInstances(c,ri->slaves);
    } else if (!strcasecmp(c->argv[1]->ptr,"sentinels")) {
        /* SENTINEL SENTINELS <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2])) == NULL)
            return;
        addReplyDictOfRedisInstances(c,ri->sentinels);
    } else if (!strcasecmp(c->argv[1]->ptr,"is-master-down-by-addr")) {
        /* SENTINEL IS-MASTER-DOWN-BY-ADDR <ip> <port> <current-epoch> <runid>*/
        // 当 runid == * 的时候，意味着仅仅是某一个 sentinel 向其他 sentinel 确认：master 是否 ODOWN 罢了
        // 当 runid == "xxxxxx", 的时候，就意味着：某一个 sentinel 把自己的 runid 发过来了，希望大家选择它作为 leader sentinel
        sentinelRedisInstance *ri;
        long long req_epoch;
        uint64_t leader_epoch = 0;
        char *leader = NULL;
        long port;
        int isdown = 0;

        if (c->argc != 6) goto numargserr;
        if (getLongFromObjectOrReply(c,c->argv[3],&port,NULL) != REDIS_OK ||
            getLongLongFromObjectOrReply(c,c->argv[4],&req_epoch,NULL)
                                                              != REDIS_OK)
            return;
        ri = getSentinelRedisInstanceByAddrAndRunID(sentinel.masters,
            c->argv[2]->ptr,port,NULL); // 看看这个 ip:port 的 master 是不是自己监控的 redis-server

        /* It exists? Is actually a master? Is subjectively down? It's down.
         * Note: if we are in tilt mode we always reply with "0". */
        if (!sentinel.tilt && ri && (ri->flags & SRI_S_DOWN) &&
                                    (ri->flags & SRI_MASTER))
            isdown = 1; // 本 sentinel 对于这个 master 的看法是 SDOWN

        /* Vote for the master (or fetch the previous vote) if the request
         * includes a runid, otherwise the sender is not seeking for a vote. */
        // TODO: * 的 runid 意味着什么？
        if (ri && ri->flags & SRI_MASTER && strcasecmp(c->argv[5]->ptr,"*")) {
            leader = sentinelVoteLeader(ri,(uint64_t)req_epoch,
                                            c->argv[5]->ptr,
                                            &leader_epoch);
        }

        /* Reply with a three-elements multi-bulk reply:
         * down state, leader, vote epoch. */
        // 多条回复
        // 1) <down_state>    1 代表下线， 0 代表未下线
        // 2) <leader_runid>  Sentinel 选举作为 leader Sentinel 的运行 ID
        // 3) <leader_epoch>  leader Sentinel 目前的配置纪元
        addReplyMultiBulkLen(c,3);
        addReply(c, isdown ? shared.cone : shared.czero);
        // leader 的 runid 可能是 req-runid，也可能是当前 sentinel 原本投的那个 leader runid，取决于 req-epoch 跟 master_ri->leader_epoch
        addReplyBulkCString(c, leader ? leader : "*");  // 发送 leader 的 runid，表达：我这个 sentinel 投 runid 为 xxxx 的 sentinel
        addReplyLongLong(c, (long long)leader_epoch);
        if (leader) sdsfree(leader);
    } else if (!strcasecmp(c->argv[1]->ptr,"reset")) {
        /* SENTINEL RESET <pattern> */
        if (c->argc != 3) goto numargserr;
        addReplyLongLong(c,sentinelResetMastersByPattern(c->argv[2]->ptr,SENTINEL_GENERATE_EVENT));
    } else if (!strcasecmp(c->argv[1]->ptr,"get-master-addr-by-name")) {
        /* SENTINEL GET-MASTER-ADDR-BY-NAME <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        ri = sentinelGetMasterByName(c->argv[2]->ptr);
        if (ri == NULL) {
            addReply(c,shared.nullmultibulk);
        } else {
            sentinelAddr *addr = sentinelGetCurrentMasterAddress(ri);

            addReplyMultiBulkLen(c,2);
            addReplyBulkCString(c,addr->ip);
            addReplyBulkLongLong(c,addr->port);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"failover")) {
        /* SENTINEL FAILOVER <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2])) == NULL)
            return;
        if (ri->flags & SRI_FAILOVER_IN_PROGRESS) {
            addReplySds(c,sdsnew("-INPROG Failover already in progress\r\n"));
            return;
        }
        if (sentinelSelectSlave(ri) == NULL) {
            addReplySds(c,sdsnew("-NOGOODSLAVE No suitable slave to promote\r\n"));
            return;
        }
        redisLog(REDIS_WARNING,"Executing user requested FAILOVER of '%s'",
            ri->name);
        sentinelStartFailover(ri);
        ri->flags |= SRI_FORCE_FAILOVER;
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"pending-scripts")) {
        /* SENTINEL PENDING-SCRIPTS */

        if (c->argc != 2) goto numargserr;
        sentinelPendingScriptsCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"monitor")) {
        /* SENTINEL MONITOR <name> <ip> <port> <quorum> */
        sentinelRedisInstance *ri;
        long quorum, port;
        char buf[32];

        if (c->argc != 6) goto numargserr;
        if (getLongFromObjectOrReply(c,c->argv[5],&quorum,"Invalid quorum")
            != REDIS_OK) return;
        if (getLongFromObjectOrReply(c,c->argv[4],&port,"Invalid port")
            != REDIS_OK) return;
        /* Make sure the IP field is actually a valid IP before passing it
         * to createSentinelRedisInstance(), otherwise we may trigger a
         * DNS lookup at runtime.(DNS 可能会导致同步阻塞) */
        if (anetResolveIP(NULL,c->argv[3]->ptr,buf,sizeof(buf)) == ANET_ERR) {
            addReplyError(c,"Invalid IP address specified");
            return;
        }

        /* Parameters are valid. Try to create the master instance. */
        ri = createSentinelRedisInstance(c->argv[2]->ptr,SRI_MASTER,
                c->argv[3]->ptr,port,quorum,NULL);
        if (ri == NULL) {
            switch(errno) {
            case EBUSY:
                addReplyError(c,"Duplicated master name");
                break;
            case EINVAL:
                addReplyError(c,"Invalid port number");
                break;
            default:
                addReplyError(c,"Unspecified error adding the instance");
                break;
            }
        } else {
            sentinelFlushConfig();
            sentinelEvent(REDIS_WARNING,"+monitor",ri,"%@ quorum %d",ri->quorum);
            addReply(c,shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"remove")) {
        /* SENTINEL REMOVE <name> */
        sentinelRedisInstance *ri;

        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2]))
            == NULL) return;
        sentinelEvent(REDIS_WARNING,"-monitor",ri,"%@");
        dictDelete(sentinel.masters,c->argv[2]->ptr);
        sentinelFlushConfig();
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"set")) {
        if (c->argc < 3 || c->argc % 2 == 0) goto numargserr;
        sentinelSetCommand(c);
    } else {
        addReplyErrorFormat(c,"Unknown sentinel subcommand '%s'",
                               (char*)c->argv[1]->ptr);
    }
    return;

numargserr:
    addReplyErrorFormat(c,"Wrong number of arguments for 'sentinel %s'",
                          (char*)c->argv[1]->ptr);
}

/* SENTINEL INFO [section] */
// sentinel 模式下的 INFO 命令实现
void sentinelInfoCommand(redisClient *c) {
    char *section = c->argc == 2 ? c->argv[1]->ptr : "default";
    sds info = sdsempty();
    int defsections = !strcasecmp(section,"default");
    int sections = 0;

    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    }

    if (!strcasecmp(section,"server") || defsections) {
        if (sections++) info = sdscat(info,"\r\n");
        sds serversection = genRedisInfoString("server");
        info = sdscatlen(info,serversection,sdslen(serversection));
        sdsfree(serversection);
    }

    if (!strcasecmp(section,"sentinel") || defsections) {
        dictIterator *di;
        dictEntry *de;
        int master_id = 0;

        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Sentinel\r\n"
            "sentinel_masters:%lu\r\n"
            "sentinel_tilt:%d\r\n"
            "sentinel_running_scripts:%d\r\n"
            "sentinel_scripts_queue_length:%ld\r\n",
            dictSize(sentinel.masters),
            sentinel.tilt,
            sentinel.running_scripts,
            listLength(sentinel.scripts_queue));

        di = dictGetIterator(sentinel.masters);
        while((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);
            char *status = "ok";

            if (ri->flags & SRI_O_DOWN) status = "odown";
            else if (ri->flags & SRI_S_DOWN) status = "sdown";
            info = sdscatprintf(info,
                "master%d:name=%s,status=%s,address=%s:%d,"
                "slaves=%lu,sentinels=%lu\r\n",
                master_id++, ri->name, status,
                ri->addr->ip, ri->addr->port,
                dictSize(ri->slaves),
                dictSize(ri->sentinels)+1);
        }
        dictReleaseIterator(di);
    }

    addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
        (unsigned long)sdslen(info)));
    addReplySds(c,info);
    addReply(c,shared.crlf);
}

/* SENTINEL SET <mastername> [<option> <value> ...] */
void sentinelSetCommand(redisClient *c) {
    sentinelRedisInstance *ri;
    int j, changes = 0;
    char *option, *value;

    if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2]))
        == NULL) return;

    /* Process option - value pairs. */
    for (j = 3; j < c->argc; j += 2) {
        option = c->argv[j]->ptr;
        value = c->argv[j+1]->ptr;
        robj *o = c->argv[j+1];
        long long ll;

        if (!strcasecmp(option,"down-after-milliseconds")) {
            /* down-after-millisecodns <milliseconds> */
            if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll <= 0)
                goto badfmt;
            ri->down_after_period = ll;
            sentinelPropagateDownAfterPeriod(ri);
            changes++;
        } else if (!strcasecmp(option,"failover-timeout")) {
            /* failover-timeout <milliseconds> */
            if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll <= 0)
                goto badfmt;
            ri->failover_timeout = ll;
            changes++;
       } else if (!strcasecmp(option,"parallel-syncs")) {
            /* parallel-syncs <milliseconds> */
            if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll <= 0)
                goto badfmt;
            ri->parallel_syncs = ll;
            changes++;
       } else if (!strcasecmp(option,"notification-script")) {
            /* notification-script <path> */
            if (strlen(value) && access(value,X_OK) == -1) {
                addReplyError(c,
                    "Notification script seems non existing or non executable");
                if (changes) sentinelFlushConfig();
                return;
            }
            sdsfree(ri->notification_script);
            ri->notification_script = strlen(value) ? sdsnew(value) : NULL;
            changes++;
       } else if (!strcasecmp(option,"client-reconfig-script")) {
            /* client-reconfig-script <path> */
            if (strlen(value) && access(value,X_OK) == -1) {
                addReplyError(c,
                    "Client reconfiguration script seems non existing or "
                    "non executable");
                if (changes) sentinelFlushConfig();
                return;
            }
            sdsfree(ri->client_reconfig_script);
            ri->client_reconfig_script = strlen(value) ? sdsnew(value) : NULL;
            changes++;
       } else if (!strcasecmp(option,"auth-pass")) {
            /* auth-pass <password> */
            sdsfree(ri->auth_pass);
            ri->auth_pass = strlen(value) ? sdsnew(value) : NULL;
            changes++;
       } else if (!strcasecmp(option,"quorum")) {
            /* quorum <count> */
            if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll <= 0)
                goto badfmt;
            ri->quorum = ll;
            changes++;
        } else {
            addReplyErrorFormat(c,"Unknown option '%s' for SENTINEL SET",
                option);
            if (changes) sentinelFlushConfig();
            return;
        }
        sentinelEvent(REDIS_WARNING,"+set",ri,"%@ %s %s",option,value);
    }

    if (changes) sentinelFlushConfig();
    addReply(c,shared.ok);
    return;

badfmt: /* Bad format errors */
    if (changes) sentinelFlushConfig();
    addReplyErrorFormat(c,"Invalid argument '%s' for SENTINEL SET '%s'",
            value, option);
}

/* Our fake PUBLISH command: it is actually useful only to receive hello messages
 * from the other sentinel instances, and publishing to a channel other than
 * SENTINEL_HELLO_CHANNEL is forbidden.
 *
 * Because we have a Sentinel PUBLISH, the code to send hello messages is the same
 * for all the three kind of instances: masters, slaves, sentinels. */
void sentinelPublishCommand(redisClient *c) {
    if (strcmp(c->argv[1]->ptr,SENTINEL_HELLO_CHANNEL)) {
        addReplyError(c, "Only HELLO messages are accepted by Sentinel instances.");
        return;
    }
    sentinelProcessHelloMessage(c->argv[2]->ptr,sdslen(c->argv[2]->ptr));
    addReplyLongLong(c,1);
}

/* ===================== SENTINEL availability checks ======================= */

/* Is this instance down from our point of view? */
// 检查 instance 是否以下线（从本 Sentinel 的角度来看）
void sentinelCheckSubjectivelyDown(sentinelRedisInstance *ri) {

    mstime_t elapsed = 0;

    if (ri->last_ping_time) // 说明有 ping 没有回；要是是有的 ping 都被 pong 了的话，这个会被 reset to 0
        elapsed = mstime() - ri->last_ping_time;

    /* Check if we are in need for a reconnection of one of the 
     * links, because we are detecting low activity.
     *
     * 如果检测到连接的活跃度（activity）很低，那么考虑重断开连接，并进行重连
     *
     * 1) Check if the command link seems connected, was connected not less
     *    than SENTINEL_MIN_LINK_RECONNECT_PERIOD, but still we have a
     *    pending ping for more than half the timeout. */
    // 考虑断开 instance 的 cc 连接
    if (ri->cc 
        && (mstime() - ri->cc_conn_time) > SENTINEL_MIN_LINK_RECONNECT_PERIOD // 控制发起重连的频率（针对网络 flap 的情况）
        && ri->last_ping_time != 0 /* Ther is a pending ping... */
        /* The pending ping is delayed, and we did not received
         * error replies as well. */
        && (mstime() - ri->last_ping_time) > (ri->down_after_period/2)
        && (mstime() - ri->last_pong_time) > (ri->down_after_period/2))
        
    {
        sentinelKillLink(ri,ri->cc);
    }

    /* 2) Check if the pubsub link seems connected, was connected not less
     *    than SENTINEL_MIN_LINK_RECONNECT_PERIOD, but still we have no
     *    activity in the Pub/Sub channel for more than
     *    SENTINEL_PUBLISH_PERIOD * 3.
     */
    // 考虑断开 instance 的 pc 连接
    if (ri->pc &&
        (mstime() - ri->pc_conn_time) > SENTINEL_MIN_LINK_RECONNECT_PERIOD &&
        (mstime() - ri->pc_last_activity) > (SENTINEL_PUBLISH_PERIOD*3))
    {
        sentinelKillLink(ri,ri->pc);
    }

    /* Update the SDOWN flag. We believe the instance is SDOWN if:
     *
     * 更新 SDOWN 标识。如果以下条件被满足，那么 Sentinel 认为 instance 已下线：
     *
     * 1) It is not replying.
     *    它没有回应命令
     * 2) We believe it is a master, it reports to be a slave for enough time
     *    to meet the down_after_period, plus enough time to get two times
     *    INFO report from the instance. 
     *    Sentinel 认为 instance 是 master ，这个服务器向 Sentinel 报告它将成为 slave ，
     *    但在超过给定时限之后，服务器仍然没有完成这一角色转换。
     */
    if (elapsed > ri->down_after_period     // 超过了配置上，视为 DOWN 的时长
        || (ri->flags & SRI_MASTER && ri->role_reported == SRI_SLAVE    // TODO: role_reported 跟 flag 有啥区别？什么时候回发生变化？
            && mstime() - ri->role_reported_time > (ri->down_after_period+SENTINEL_INFO_PERIOD*2)))
    {
        /* Is subjectively down */
        if ((ri->flags & SRI_S_DOWN) == 0) {
            // 发送事件
            sentinelEvent(REDIS_WARNING,"+sdown",ri,"%@");
            // 记录进入 SDOWN 状态的时间
            ri->s_down_since_time = mstime();
            // 打开 SDOWN 标志
            ri->flags |= SRI_S_DOWN;
        }
    } else {
        // 移除（可能有的） SDOWN 状态
        /* Is subjectively up */
        if (ri->flags & SRI_S_DOWN) {
            // 发送事件
            sentinelEvent(REDIS_WARNING,"-sdown",ri,"%@");
            // 移除相关标志
            ri->flags &= ~(SRI_S_DOWN|SRI_SCRIPT_KILL_SENT);
        }
    }
}


/* Is this instance down according to the configured quorum?
 *
 * 根据给定数量的 Sentinel 投票，判断 instance 是否已下线。（通过 SENTINEL is-master-down-by-addr 主动询问其他 sentinel）
 *
 * Note that ODOWN is a weak quorum, it only means that enough Sentinels
 * reported in a given time range that the instance was not reachable.
 *
 * TODO: 何谓 a weak quorum ？
 * 注意 ODOWN 是一个 weak quorum ，它只意味着有足够多的 Sentinel 
 * 在**给定的时间范围内**报告 instance 不可达。
 *
 * However messages can be delayed so there are no strong guarantees about
 * N instances agreeing at the same time about the down state. 
 *
 * 因为 Sentinel 对 instance 的检测信息可能带有延迟，
 * 所以实际上 N 个 Sentinel **不可能在同一时间内**判断 master 进入了下线状态。
 */
void sentinelCheckObjectivelyDown(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;
    int quorum = 0, odown = 0;

    // 如果当前 Sentinel 将 master 判断为主观下线
    // 那么检查是否有其他 Sentinel 同意这一判断
    // 当同意的数量足够时，将 master 判断为客观下线
    if (master->flags & SRI_S_DOWN) {
        /* Is down for enough sentinels? */

        // 统计同意的 Sentinel 数量（起始的 1 代表本 Sentinel）
        quorum = 1; /* the current sentinel. */

        /* Count all the other sentinels. */
        // 统计其他认为 master 进入下线状态的 Sentinel 的数量
        di = dictGetIterator(master->sentinels);
        while((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);
                
            // 该 SENTINEL 也认为 master 已下线
            // TODO: (DONE) 其他 sentinel 认为 ODOWN 的信息，是什么时候？怎么传播到这里的？
            // 当 sentinel 第一次觉得 master SDOWN 的时候，这个函数并不会起作用
            // 然后 sentinel 就会 利用 SENTINEL is-master-down-by-addr 去问 其他 sentinel 对于 master 的看法
            // 然后通过 sentinelReceiveIsMasterDownReply() 记录其他 sentinel 对于 master 的看法，确定要不要设置为 ODOWN
            if (ri->flags & SRI_MASTER_DOWN) quorum++;
        }
        dictReleaseIterator(di);
        
        // 如果投票得出的支持数目大于等于判断 ODOWN 所需的票数
        // 那么进入 ODOWN 状态
        if (quorum >= master->quorum) odown = 1;
    }

    /* Set the flag accordingly to the outcome. */
    if (odown) {

        // master 被认定为 ODOWN

        if ((master->flags & SRI_O_DOWN) == 0) {    // 第一次被视为 ODOWN，设置相应的 flag
            // 发送事件
            sentinelEvent(REDIS_WARNING,"+odown",master,"%@ #quorum %d/%d",
                quorum, master->quorum);
            // 打开 ODOWN 标志
            master->flags |= SRI_O_DOWN;
            // 记录进入 ODOWN 的时间
            master->o_down_since_time = mstime();
        }
    } else {

        // master 未被认定 ODOWN

        if (master->flags & SRI_O_DOWN) {

            // 如果 master 曾经进入过 ODOWN 状态，那么移除该状态
            // 因为 ODOWN 跟发起 failover 是两码事，ODOWN 之后，是要完成投票选举之后，才能进行切换的

            // 发送事件
            sentinelEvent(REDIS_WARNING,"-odown",master,"%@");
            // 移除 ODOWN 标志
            master->flags &= ~SRI_O_DOWN;
        }
    }
}

/* Receive the SENTINEL is-master-down-by-addr reply, see the
 * sentinelAskMasterStateToOtherSentinels() function for more information. */
// 其他 sentinel 对于 SENTINEL is-master-down-by-addr 命令的回复
/**
 * EXAMPLE:
 * SENTINEL IS-MASTER-DOWN-BY-ADDR <ip> <port> <current-epoch> <issue-CMD-sentinel-runid>
 * 5.5.75.110:26110> SENTINEL is-master-down-by-addr 5.5.75.109 11109 1 *
 *                                            (master-ip) (master-port) (issue this CMD sentinel's runid)
 * 1) (integer) 0   // 1 --> master DONW, 0 ---> master not DOWN
 * 2) "*"           // 这个 sentinel 投票的 leader sentinel runid？是的, 当 * 号时, 意味着着仅仅是 sentinel 之间询问 master SDOWN 状态罢了
 * 3) (integer) 0   // leader_epoch
*/
void sentinelReceiveIsMasterDownReply(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;
    redisReply *r;

    if (ri) ri->pending_commands--;
    if (!reply || !ri) return;
    r = reply;

    /* Ignore every error or unexpected reply.
     * 忽略错误回复
     * Note that if the command returns an error for any reason we'll
     * end clearing the SRI_MASTER_DOWN flag for timeout anyway. */
    if (r->type == REDIS_REPLY_ARRAY && r->elements == 3 &&
        r->element[0]->type == REDIS_REPLY_INTEGER &&
        r->element[1]->type == REDIS_REPLY_STRING &&
        r->element[2]->type == REDIS_REPLY_INTEGER)
    {
        // 更新最后一次回复询问的时间
        ri->last_master_down_reply_time = mstime();

        // 设置 SENTINEL 认为 master 的状态
        if (r->element[0]->integer == 1) {
            // 已下线
            ri->flags |= SRI_MASTER_DOWN;
        } else {
            // 未下线
            ri->flags &= ~SRI_MASTER_DOWN;
        }

        // 如果运行 ID 不是 "*" 的话，那么这是那个 sentinel 所投的 leader sentinel
        if (strcmp(r->element[1]->str,"*")) {
            /* If the runid in the reply is not "*" the Sentinel actually
             * replied with a vote. */
            sdsfree(ri->leader);
            // 打印日志
            if (ri->leader_epoch != r->element[2]->integer)
                redisLog(REDIS_WARNING,
                    "%s voted for %s %llu", ri->name,
                    r->element[1]->str,
                    (unsigned long long) r->element[2]->integer);
            // 设置这个回复的 sentinel 所推举的 leader sentinel
            // 当本 sentinel 在统计投票结果的时候，就会用上，参考: sentinelGetLeader()
            ri->leader = sdsnew(r->element[1]->str);
            ri->leader_epoch = r->element[2]->integer;
        }
    }
}

/* If we think the master is down, we start sending
 * SENTINEL IS-MASTER-DOWN-BY-ADDR requests to other sentinels
 * in order to get the replies that allow to reach the quorum
 * needed to mark the master in ODOWN state and trigger a failover. */
// 如果 Sentinel 认为 master 已下线，
// 那么它会通过向其他 Sentinel 发送 SENTINEL is-master-down-by-addr 命令，
// 尝试获得足够的票数，将 master 标记为 ODOWN 状态，并开始一次故障转移操作
#define SENTINEL_ASK_FORCED (1<<0)  // 这个 flags 在当前版本里面就意味着：是 leader sentinel 的选举投票
void sentinelAskMasterStateToOtherSentinels(sentinelRedisInstance *master, int flags) {
    dictIterator *di;
    dictEntry *de;

    // 遍历正在监视相同 master 的所有 sentinel
    // 向它们发送 SENTINEL is-master-down-by-addr 命令
    di = dictGetIterator(master->sentinels);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        // 距离该 sentinel 最后一次回复 SENTINEL master-down-by-addr 命令已经过了多久
        mstime_t elapsed = mstime() - ri->last_master_down_reply_time;

        char port[32];
        int retval;

        /* If the master state from other sentinel is too old, we clear it. */
        // 如果目标 Sentinel 关于 master 的信息已经太久没更新，那么我们清除它
        if (elapsed > SENTINEL_ASK_PERIOD*5) {
            ri->flags &= ~SRI_MASTER_DOWN;
            sdsfree(ri->leader);
            ri->leader = NULL;
        }

        /* Only ask if master is down to other sentinels if:
         *
         * 只在以下情况满足时，才向其他 sentinel 询问 master 是否已下线
         *
         * 1) We believe it is down, or there is a failover in progress.
         *    本 sentinel 相信服务器已经下线，或者针对该 master 的故障转移操作正在执行
         * 2) Sentinel is connected.
         *    目标 Sentinel 与本 Sentinel 已连接
         * 3) We did not received the info within SENTINEL_ASK_PERIOD ms. 
         *    当前 Sentinel 在 SENTINEL_ASK_PERIOD 毫秒内没有获得过目标 Sentinel 发来的信息
         * 4) 条件 1 和条件 2 满足而条件 3 不满足，但是 flags 参数给定了 SENTINEL_ASK_FORCED 标识
         */
        // 既然自己都不觉得 master SDOWN 了，那就没必要主动发起 master 状态询问，这时候，等待其他觉得 master SDOWN 的 sentinel 来询问就好了
        if ((master->flags & SRI_S_DOWN) == 0) continue;
        
        // 都没初始化好，也没法问，直接跳过。
        // TODO: 不会造成投票、ODOWN 失误吗？这是要通过你创建 sentinel 的数量 + quorum 来确保的
        if (ri->flags & SRI_DISCONNECTED) continue;
        // 不是 SENTINEL_ASK_FORCED 这种必须要立即发送的情况时，会减少发送频率，到了 SENTINEL_ASK_PERIOD 才周期性问一下
        if (!(flags & SENTINEL_ASK_FORCED) &&
            mstime() - ri->last_master_down_reply_time < SENTINEL_ASK_PERIOD)
            continue;

        /* Ask */
        // 发送其他 SENTINEL is-master-down-by-addr 命令，询问他们对于 master x.x.x.x:port 的看法
        ll2string(port,sizeof(port),master->addr->port);
        retval = redisAsyncCommand(ri->cc,  // sentinel 跟 sentinel 之间必定有 command context
                    sentinelReceiveIsMasterDownReply, NULL,
                    "SENTINEL is-master-down-by-addr %s %s %llu %s",
                    master->addr->ip, port,
                    sentinel.current_epoch,
                    // 如果本 Sentinel 已经检测到 master 进入 ODOWN 
                    // 并且要开始一次故障转移，那么向其他 Sentinel 发送自己的运行 ID
                    // 让对方将给自己投一票（如果对方在这个纪元内还没有投票的话）
                    (master->failover_state > SENTINEL_FAILOVER_STATE_NONE) ?
                    server.runid : "*");
                    // TODO:（DONE） 发 runid 跟不发 runid 分别是什么情况？
                    // 发 runid 就是想要别人投票选自己作为 leader sentinel
                    // 不发 runid，就是单纯的想问一下其他 sentinel 是不是也觉得这个 master SDOWN 了
        if (retval == REDIS_OK) ri->pending_commands++;
    }
    dictReleaseIterator(di);
}

/* =============================== FAILOVER ================================= */

/* Vote for the sentinel with 'req_runid' or return the old vote if already
 * voted for the specifed 'req_epoch' or one greater.
 *
 * 为 runid 为 req_runid 的 Sentinel 投上一票，有两种额外情况可能出现：
 * 1) 如果 Sentinel 在 req_epoch 纪元已经投过票了，那么返回之前投的票。
 * 2) 如果 Sentinel 已经为大于 req_epoch 的纪元投过票了，那么返回更大纪元的投票。
 * 3) 常规 case：req_epoch 大于 sentinel epoch，也就是新一轮选举，直接投票
 *
 * If a vote is not available returns NULL, otherwise return the Sentinel
 * runid and populate the leader_epoch with the epoch of the vote. 
 *
 * 如果投票暂时不可用，那么返回 NULL 。
 * 否则返回 Sentinel 的运行 ID ，并将被投票的纪元保存到 leader_epoch 指针的值里面。
 */
// 每一个 sentinel 都可以监控多个不同集群的 master，
// 也就意味着：每一个 leader sentinel 的选举都是会因为 集群的不同、epoch 的不同，而相互独立

// 现在是站在一个 sentinel 上面看：
// 我觉得 runid 为 aaaaa 的 sentinel 可以作为 master-redis-server 的 leader sentinel
// 所以我把这个信息记录在 master->leader 里面
// 一个集群里面，只会有一个 master-redis-server，以及一个负责进行 failover 的 leader sentinel（不考虑切片的问题）
char *sentinelVoteLeader(sentinelRedisInstance *master, uint64_t req_epoch, char *req_runid, uint64_t *leader_epoch) {
    if (req_epoch > sentinel.current_epoch) {
        // 常规 case：req_epoch 大于 sentinel epoch，也就是新一轮选举，直接投票
        sentinel.current_epoch = req_epoch;
        sentinelFlushConfig();
        sentinelEvent(REDIS_WARNING,"+new-epoch",master,"%llu",
            (unsigned long long) sentinel.current_epoch);
    }

    // 只有当 sentinel.current_epoch <= req_epoch 的时候，才说明：当前 sentinel 收到了别人发起的，新一轮选举；
    // 这时候，当前 sentinel 才能够进行投票（因为这一轮新的投票选举中，当前 sentinel 还没投过票）
    // 投完票之后，当前 sentinel 是要增加 epoch 计数的
    if (master->leader_epoch < req_epoch && sentinel.current_epoch <= req_epoch)
    {
        sdsfree(master->leader);    // 因为 master->leader_epoch < req_epoch，这是新的一轮
        master->leader = sdsnew(req_runid); // 更新自己投的 leader
        master->leader_epoch = sentinel.current_epoch;
        sentinelFlushConfig();
        sentinelEvent(REDIS_WARNING,"+vote-for-leader",master,"%s %llu",
            master->leader, (unsigned long long) master->leader_epoch);

        // TODO: 这个是为了什么？超时, 重新进行 leader 选举, 配合 sentinelStartFailoverIfNeeded() 一起看
        /* If we did not voted for ourselves, set the master failover start
         * time to now, in order to force a delay before we can start a
         * failover for the same master. */
        if (strcasecmp(master->leader,server.runid))
            master->failover_start_time = mstime()+rand()%SENTINEL_MAX_DESYNC;
    }

    // 当遇到了能够重新投票的 case，就会在上面的代码中，改变 master->leader_epoch, master->leader
    // 所以这里直接返回 runid\NULL 就好了，因为这就这就包含了：可能投出新 leader 或 返回原本的 leader 两种情况
    *leader_epoch = master->leader_epoch;
    return master->leader ? sdsnew(master->leader) : NULL;
}

// 记录客观 leader 投票的结构
struct sentinelLeader {

    // sentinel 的运行 id
    char *runid;

    // 该 sentinel 获得的票数
    unsigned long votes;
};

/* Helper function for sentinelGetLeader, increment the counter
 * relative to the specified runid. */
// 为给定 ID 的 Sentinel  instance 增加一票
int sentinelLeaderIncr(dict *counters, char *runid) {
    dictEntry *de = dictFind(counters,runid);
    uint64_t oldval;

    if (de) {
        oldval = dictGetUnsignedIntegerVal(de);
        dictSetUnsignedIntegerVal(de,oldval+1);
        return oldval+1;
    } else {
        de = dictAddRaw(counters,runid);
        redisAssert(de != NULL);
        dictSetUnsignedIntegerVal(de,1);
        return 1;
    }
}

/* Scan all the Sentinels attached to this master to check if there
 * is a leader for the specified epoch.
 *
 * 扫描所有监视 master 的 Sentinels ，查看是否有 Sentinels 是这个纪元的 leader。
 *
 * To be a leader for a given epoch, we should have the majorify of
 * the Sentinels we know that reported the same instance as
 * leader for the same epoch. 
 *
 * 要让一个 Sentinel 成为本纪元的 leader，
 * 这个 Sentinel 必须让大多数其他 Sentinel 承认它是该纪元的 leader才行。
 */
// leader sentinel 的要求：1. 超过 quorum 的法定人数；2. 超过半数的 sentinel 同意
// 选举出 master 在指定 epoch 上的 leader
char *sentinelGetLeader(sentinelRedisInstance *master, uint64_t epoch) {
    dict *counters;
    dictIterator *di;
    dictEntry *de;
    unsigned int voters = 0, voters_quorum;
    char *myvote;
    char *winner = NULL;
    uint64_t leader_epoch;
    uint64_t max_votes = 0;

    // 便于状态机的 bug 发现
    redisAssert(master->flags & (SRI_O_DOWN|SRI_FAILOVER_IN_PROGRESS));

    // 统计器（临时使用，方便统计各个 sentinel 的得票）
    counters = dictCreate(&leaderVotesDictType,NULL);

    /* Count other sentinels votes */
    // 统计其他 sentinel 的主观 leader 投票
    di = dictGetIterator(master->sentinels);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        // 为目标 Sentinel 选出的 leader Sentinel 增加一票（不同 epoch 的投票没用）
        if (ri->leader != NULL && ri->leader_epoch == sentinel.current_epoch)
            sentinelLeaderIncr(counters,ri->leader);

        // 统计可参与投票的 sentinel 总数（无论是否投票都要统计）
        voters++;
    }
    dictReleaseIterator(di);

    /* Check what's the winner. For the winner to win, it needs two conditions:
     *
     * 选出 leader leader ，它必须满足以下两个条件：
     *
     * 1) Absolute majority between voters (50% + 1).
     *    有多于一半的 Sentinel 支持
     * 2) And anyway at least master->quorum votes. 
     *    投票数至少要有 master->quorum 那么多
     */
    di = dictGetIterator(counters);
    while((de = dictNext(di)) != NULL) {

        // 取出票数
        uint64_t votes = dictGetUnsignedIntegerVal(de);

        // 选出票数最大的人
        if (votes > max_votes) {
            max_votes = votes;
            winner = dictGetKey(de);
        }
    }
    dictReleaseIterator(di);

    /* Count this Sentinel vote:
     * if this Sentinel did not voted yet, either vote for the most
     * common voted sentinel, or for itself if no vote exists at all. */
    // 本 Sentinel 进行投票(因为除了投票之外，还要过半数，所以当前 sentinel 的投票也是必要的)
    // 如果 Sentinel 之前还没有进行投票，那么有两种选择：
    // 1）如果选出了 winner （最多票数支持的 Sentinel ），那么这个 Sentinel 也投 winner 一票
    // 2）如果没有选出 winner ，那么 Sentinel 投自己一票
    if (winner)
        myvote = sentinelVoteLeader(master,epoch,winner,&leader_epoch);
    else
        myvote = sentinelVoteLeader(master,epoch,server.runid,&leader_epoch);

    // sentinel Sentinel 已选出，并且 leader 的纪元和给定的纪元一样
    if (myvote && leader_epoch == epoch) {

        // 为 leader Sentinel 增加一票（这一票来自本 Sentinel ）
        uint64_t votes = sentinelLeaderIncr(counters,myvote);

        // 如果投票之后的票数比最大票数要大，那么更换 leader Sentinel
        if (votes > max_votes) {
            max_votes = votes;
            winner = myvote;
        }
    }
    voters++; /* Anyway, count me as one of the voters. */

    // 如果支持 leader的投票数量不超过半数
    // 并且支持票数不超过 master 配置指定的投票数量
    // 那么这次 leader选举无效
    voters_quorum = voters/2+1;
    if (winner && (max_votes < voters_quorum || max_votes < master->quorum))
        winner = NULL;

    // 返回 leader Sentinel ，或者 NULL
    winner = winner ? sdsnew(winner) : NULL;
    sdsfree(myvote);
    dictRelease(counters);
    return winner;
}

/* Send SLAVEOF to the specified instance, always followed by a
 * CONFIG REWRITE command in order to store the new configuration on disk
 * when possible (that is, if the Redis instance is recent enough to support
 * config rewriting, and if the server was started with a configuration file).
 *
 * 向指定 instance 发送 SLAVEOF 命令，并在可能时，执行 CONFIG REWRITE 命令，
 * 将当前配置保存到磁盘中。
 *
 * If Host is NULL the function sends "SLAVEOF NO ONE".
 *
 * 如果 host 参数为 NULL ，那么向 instance 发送 SLAVEOF NO ONE 命令
 *
 * The command returns REDIS_OK if the SLAVEOF command was accepted for
 * (later) delivery otherwise REDIS_ERR. The command replies are just
 * discarded. 
 *
 * 命令入队成功（异步发送）时，函数返回 REDIS_OK ，
 * 入队失败时返回 REDIS_ERR ，
 * 命令回复会被丢弃。
 */
int sentinelSendSlaveOf(sentinelRedisInstance *ri, char *host, int port) {
    char portstr[32];
    int retval;

    ll2string(portstr,sizeof(portstr),port);

    if (host == NULL) {
        host = "NO";
        memcpy(portstr,"ONE",4);
    }

    // 发送 SLAVEOF NO ONE
    retval = redisAsyncCommand(ri->cc,
        sentinelDiscardReplyCallback, NULL, "SLAVEOF %s %s", host, portstr);
    if (retval == REDIS_ERR) return retval;

    ri->pending_commands++;

    // 发送 CONFIG REWRITE
    if (redisAsyncCommand(ri->cc,
        sentinelDiscardReplyCallback, NULL, "CONFIG REWRITE") == REDIS_OK)
    {
        ri->pending_commands++;
    }

    return REDIS_OK;
}

/* Setup the master state to start a failover. */
// 设置 master 的状态，开始一次故障转移
void sentinelStartFailover(sentinelRedisInstance *master) {
    redisAssert(master->flags & SRI_MASTER);

    // 更新故障转移状态
    master->failover_state = SENTINEL_FAILOVER_STATE_WAIT_START;

    // 更新 master 状态
    master->flags |= SRI_FAILOVER_IN_PROGRESS;

    // 更新纪元(不然其他 sentinel 不会认为自己需要进行投票的)
    master->failover_epoch = ++sentinel.current_epoch;

    sentinelEvent(REDIS_WARNING,"+new-epoch",master,"%llu",
        (unsigned long long) sentinel.current_epoch);

    sentinelEvent(REDIS_WARNING,"+try-failover",master,"%@");

    // 记录故障转移状态的变更时间
    master->failover_start_time = mstime()+rand()%SENTINEL_MAX_DESYNC;
    master->failover_state_change_time = mstime();
}

/* This function checks if there are the conditions to start the failover,
 * that is:
 *
 * 这个函数检查是否需要开始一次故障转移操作：
 *
 * 1) Master must be in ODOWN condition.
 *     master 已经计入 ODOWN 状态。
 * 2) No failover already in progress.
 *    当前没有针对同一 master 的故障转移操作在执行。
 * 3) No failover already attempted recently.
 *    最近时间内，这个 master 没有尝试过执行故障转移
 *    （应该是为了防止频繁执行）。
 * 
 * We still don't know if we'll win the election(become leader sentinel, who handle the failover)
 * so it is possible that we start the failover but that we'll not be able to act.
 *
 * 虽然 Sentinel 可以发起一次故障转移，但因为故障转移操作是由 leader Sentinel 执行的，
 * 所以发起故障转移的 Sentinel 不一定就是执行故障转移的 Sentinel 。
 *
 * Return non-zero if a failover was started. 
 *
 * 如果故障转移操作成功开始，那么函数返回非 0 值。
 */
int sentinelStartFailoverIfNeeded(sentinelRedisInstance *master) {

    /* We can't failover if the master is not in O_DOWN state. */
    if (!(master->flags & SRI_O_DOWN)) return 0;

    /* Failover already in progress? */
    if (master->flags & SRI_FAILOVER_IN_PROGRESS) return 0;

    // TODO:(DONE) 没有超时再次发起投票的机制吗？配合 sentinelVoteLeader() 一起看
    // 这个就是超时重试的保证机制.
    /* 1. 每一个向其他 sentinel 确认过 master ODOWN 的 sentinel 都有资格成为 leader sentinel, 都有资格要求其他 sentinel 进行投票
     * 2. 每一个 sentinel 都会记录一个 failover_start_time; 
     *    主动竞选的, 会在自己要求别人投票的时候, 更新这个时间;
     *    被动要求投票的, 将会在自己投票的那一瞬间, 记录这个时间;
     * 3. 每个确认过 ODOWN 的 sentinel, 都有资格进入这个函数, 并在这个函数里面, 看看距离 failover_start_time 很久了
     *    是的话, 将会发起新一轮选举(因为都这么久了, 有理由怀疑上一次的选举失败了)
     * 4. 当 leader sentinel 成功完成 failover 的全过程之后, 会更新 leader sentinel 里面的 master SRI 信息,
     *    然后向外发送新的 hello 信息, 引发其他 sentinel 更新 master 信息, 进而 reset 掉 ODOWN 状态, failover_start_time
     *    从而将重试的准备停止掉
     */
    /* Last failover attempt started too little time ago? */
    // master->failover_start_time 是上一次 failover 的时间，会在 sentinelStartFailover() 更新
    // 当非 leader sentinel 投票的时候, 会用 failover_start_time 记录投票的时候
    // 当距离 failover_start_time 时间太久的话, 将会才是自己
    if (mstime() - master->failover_start_time < master->failover_timeout*2)
    {
        // 1. 检查是否 failover 过于频繁了；（failover_delay_logged 看起来只是为了不要重复打 log 罢了）
        if (master->failover_delay_logged != master->failover_start_time) {
            time_t clock = (master->failover_start_time +
                            master->failover_timeout*2) / 1000;
            char ctimebuf[26];

            ctime_r(&clock,ctimebuf);
            ctimebuf[24] = '\0'; /* Remove newline. */
            master->failover_delay_logged = master->failover_start_time;
            redisLog(REDIS_WARNING,
                "Next failover delay: I will not start a failover before %s",
                ctimebuf);
        }
        return 0;
    }

    // 开始一次故障转移
    sentinelStartFailover(master);

    // 本函数一旦 return 1, 就会向其他 sentinel 要求投票了, 开始竞争成为 leader
    return 1;
}

/* Select a suitable slave to promote. The current algorithm only uses
 * the following parameters:
 *
 * 在多个 slave 中选出一个作为新的 master 。
 * 算法使用以下参数：
 *
 * 1) None of the following conditions: S_DOWN, O_DOWN, DISCONNECTED.
 *    带有 S_DOWN 、 O_DOWN 和 DISCONNECTED 状态的 slave 不会被选中。
 * 2) Last time the slave replied to ping no more than 5 times the PING period.
 *    距离最近一次回复 PING 命令超过 5 个 PING period 以上的 slave 不会被选中。
 * 3) info_refresh not older than 3 times the INFO refresh period.
 *    距离最后一次回复 INFO 命令的时间超过 info_refresh 时长三倍的 slave 不会被考虑。
 * 4) master_link_down_time no more than:
 *    主 slave 之间的连接断开时间不能超过：
 * 
 *     (now - master->s_down_since_time) + (master->down_after_period * 10). (这个时间最终会被拿来跟 slave->master_link_down_time 比较)
 *    Basically since the master is down from our POV, the slave reports
 *    to be disconnected no more than 10 times the configured down-after-period.
 *    因为从当前 Sentinel 来看， master 已经处于下线状态，
 *    所以正常来说，slave 与 master 之间的连接断开时间不应该超过 down-after-period 的十倍。
 * 
 *    This is pretty much black magic but the idea is, the master was not
 *    available so the slave may be lagging, but not over a certain time.
 *    这听上去有点像黑魔法，不过这个判断的主意是这样的：
 *    当 master 下线之后，master, slave 的连接就会断开，但只要先下线的是 master 而不是 slave 
 *    （换句话说，连接断开是由 master 而不是 slave 造成的）
 *    那么 master, slave 之间的连接断开时间就不会太长。
 * 
 *    Anyway we'll select the best slave according to replication offset.
 *    不过这只是一个辅助手段，因为最终我们都会使用复制偏移量来挑选 slave 。
 * 
 * 5) Slave priority can't be zero, otherwise the slave is discarded.
 *     slave 的优先级不能为 0 ，优先级为 0 的 slave 表示被禁用。
 *
 * Among all the slaves matching the above conditions we select the slave
 * with, in order of sorting key:
 *
 * 符合以上条件的 slave ，我们会按以下条件对 slave 进行排序：
 *
 * - lower slave_priority.
 *   优先级更小的 slave 优先。
 * - bigger processed replication offset.
 *   复制偏移量较大的 slave 优先。
 * - lexicographically smaller runid.
 *   运行 ID 较小的 slave 优先。
 *
 * Basically if runid is the same, the slave that processed more commands
 * from the master is selected.
 *
 * 如果运行 ID 相同，那么执行命令数量较多的那个 slave 会被选中。
 *
 * The function returns the pointer to the selected slave, otherwise
 * NULL if no suitable slave was found.
 *
 * sentinelSelectSlave 函数返回被选中 slave 的 instance 指针，
 * 如果没有何时的 slave ，那么返回 NULL 。
 */

/* Helper for sentinelSelectSlave(). This is used by qsort() in order to
 * sort suitable slaves in a "better first" order, to take the first of
 * the list. */
// 排序函数，用于选出更好的 slave, 分值最低的那个, 将会被选中并 promote 为 master
/* 1. lower slave_priority.
 *  优先级更小的 slave 优先。
 *
 * 2. bigger processed replication offset.
 *  复制偏移量较大的 slave 优先。
 * 
 * 3. lexicographically smaller runid.
 *  运行 ID 较小的 slave 优先。
 */ 
int compareSlavesForPromotion(const void *a, const void *b) {
    sentinelRedisInstance **sa = (sentinelRedisInstance **)a,
                          **sb = (sentinelRedisInstance **)b;
    char *sa_runid, *sb_runid;

    // 优先级较小的 slave 胜出
    if ((*sa)->slave_priority != (*sb)->slave_priority)
        return (*sa)->slave_priority - (*sb)->slave_priority;

    /* If priority is the same, select the slave with greater replication
     * offset (processed more data frmo the master). */
    // 如果优先级相同，那么复制偏移量较大的那个 slave 胜出
    // （偏移量较大表示从 master 获取的数据更多，更完整）
    if ((*sa)->slave_repl_offset > (*sb)->slave_repl_offset) {
        return -1; /* a < b */
    } else if ((*sa)->slave_repl_offset < (*sb)->slave_repl_offset) {
        return 1; /* b > a */
    }

    /* If the replication offset is the same select the slave with that has
     * the lexicographically smaller runid. Note that we try to handle runid
     * == NULL as there are old Redis versions that don't publish runid in
     * INFO. A NULL runid is considered bigger than any other runid. */
    // 如果复制偏移量也相同，那么选出运行 ID 较低的那个 slave 
    // 注意，对于没有运行 ID 的旧版 Redis 来说，默认的运行 ID 为 NULL
    sa_runid = (*sa)->runid;
    sb_runid = (*sb)->runid;
    if (sa_runid == NULL && sb_runid == NULL) return 0;
    else if (sa_runid == NULL) return 1;  /* a > b */
    else if (sb_runid == NULL) return -1; /* a < b */
    return strcasecmp(sa_runid, sb_runid);
}

// 从 master 的所有 slave 中，挑选一个作为新的 master 
// 如果没有合格的新 master ，那么返回 NULL
// 目标：找到一个没有失联，近期有回复 INFO 报告自己状态的 slave，然后根据每个 slave 跟 master 内容的差距，选择内容差距小的那个 slave，作为新的 master
sentinelRedisInstance *sentinelSelectSlave(sentinelRedisInstance *master) {

    sentinelRedisInstance **instance =
        zmalloc(sizeof(instance[0])*dictSize(master->slaves));  // 记录满足基本条件的候选 slave
    sentinelRedisInstance *selected = NULL;
    int instances = 0;
    dictIterator *di;
    dictEntry *de;
    mstime_t max_master_down_time = 0;  // 一个衡量标准, 衡量 slave 跟这个即将切换掉的 master 是否失联太久了

    // 计算可以接受的， slave 与 master 之间的最大下线时间
    // 这个值可以保证被选中的 slave 的数据库不会太旧
    if (master->flags & SRI_S_DOWN)
        max_master_down_time += mstime() - master->s_down_since_time;   // master 被这个 sentinel 观察到 SDOWN 的时长
    max_master_down_time += master->down_after_period * 10; // 在 master 失联之前的 master->down_after_period * 10 这么长时间里, slave 是否回复过信息

    // 遍历所有 slave, 进行初步筛选
    di = dictGetIterator(master->slaves);
    while((de = dictNext(di)) != NULL) {

        // get slave instance 
        sentinelRedisInstance *slave = dictGetVal(de);
        mstime_t info_validity_time;

        // 忽略所有 SDOWN 、ODOWN 或者已断线的 slave 
        if (slave->flags & (SRI_S_DOWN|SRI_O_DOWN|SRI_DISCONNECTED)) continue;
        if (mstime() - slave->last_avail_time > SENTINEL_PING_PERIOD*5) continue;
        if (slave->slave_priority == 0) continue;

        /* If the master is in SDOWN state we get INFO for slaves every second.
         * Otherwise we get it with the usual period so we need to account for
         * a larger delay. */
        // 如果 master 处于 SDOWN 状态，那么 Sentinel 以每秒一次的频率向 slave 发送 INFO 命令
        // 否则以平常频率向 slave 发送 INFO 命令
        // 这里要检查 INFO 命令的返回值是否合法，检查的时间会乘以一个倍数，以计算延迟
        // 参考 sentinelSendPeriodicCommands() 的时候, 将会根据不同的情况, 改变向 instance 发送 INFO 请求的速率
        if (master->flags & SRI_S_DOWN)
            info_validity_time = SENTINEL_PING_PERIOD*5;
        else
            info_validity_time = SENTINEL_INFO_PERIOD*3;    // 被选为 leader sentinel 不一定就认为 master 是 SDOWN 的, 仅仅是大家都认为 ODOWN 了

        // INFO 回复已过期，不考虑(当前 sentinel 掌握的 slave info 信息太旧了, 会影响 slave 的挑选质量)
        if (mstime() - slave->info_refresh > info_validity_time) continue;

        //  slave 下线的时间过长，不考虑
        // slave->master_link_down_time 是由 sentinel 通过 INFO 向问一个 slave 定期收集的
        // 这个 link_down_time 是 from slave point of view 的
        // 这样就能够评判: slave 是不是跟 master 失联太久了, 这个太久的衡量标准就是 max_master_down_time
        if (slave->master_link_down_time > max_master_down_time) continue;

        // 将被选中的 slave 保存到数组中
        instance[instances++] = slave;
    }
    dictReleaseIterator(di);

    if (instances) {

        // 对被选中的 slave 进行排序
        qsort(instance,instances,sizeof(sentinelRedisInstance*),
            compareSlavesForPromotion);
        
        // 分值最低的 slave 为被选中服务器
        selected = instance[0];
    }
    zfree(instance);

    // 返回被选中的从服务区
    return selected;
}

/* ---------------- Failover state machine implementation ------------------- */

// 准备执行故障转移，ri 必然是 master, 这个函数是在觉得自己能够成为 leader (确认过 master ODOWN 了)的 sentinel 中执行
void sentinelFailoverWaitStart(sentinelRedisInstance *ri) {
    char *leader;
    int isleader;

    /* Check if we are the leader for the failover epoch. */
    // 获取给定纪元的 leader Sentinel(不一定选出了 leader 的)
    leader = sentinelGetLeader(ri, ri->failover_epoch);
    // 本 Sentinel 是否为 leader Sentinel ？
    isleader = leader && strcasecmp(leader,server.runid) == 0;
    sdsfree(leader);

    /* If I'm not the leader, and it is not a forced failover via
     * SENTINEL FAILOVER, then I can't continue with the failover. */
    // 如果本 Sentinel 不是 leader，并且这次故障迁移不是一次强制故障迁移操作
    // 那么本 Sentinel 不做动作
    if (!isleader && !(ri->flags & SRI_FORCE_FAILOVER)) {
        int election_timeout = SENTINEL_ELECTION_TIMEOUT;

        /* The election timeout is the MIN between SENTINEL_ELECTION_TIMEOUT
         * and the configured failover timeout. */
        // 当选的时长（类似于任期）是 SENTINEL_ELECTION_TIMEOUT
        // 和 Sentinel 设置的故障迁移时长之间的较小那个值
        if (election_timeout > ri->failover_timeout)
            election_timeout = ri->failover_timeout;

        /* Abort the failover if I'm not the leader after some time. */
        // Sentinel 的当选时间已过，取消故障转移计划
        if (mstime() - ri->failover_start_time > election_timeout) {
            sentinelEvent(REDIS_WARNING,"-failover-abort-not-elected",ri,"%@");
            // 取消故障转移
            sentinelAbortFailover(ri);
        }
        return;
    }

    // 本 Sentinel 作为 leader，开始执行故障迁移操作...

    sentinelEvent(REDIS_WARNING,"+elected-leader",ri,"%@");

    // 进入选择 slave 状态
    ri->failover_state = SENTINEL_FAILOVER_STATE_SELECT_SLAVE;  // 下一个 sentinel cron 的时候,才会再次进入状态机里面
    ri->failover_state_change_time = mstime();

    sentinelEvent(REDIS_WARNING,"+failover-state-select-slave",ri,"%@");
}

// 选择合适的 slave 作为新的 master 
void sentinelFailoverSelectSlave(sentinelRedisInstance *ri) {

    // 在旧 master 所属的 slave 中，选择新服务器
    sentinelRedisInstance *slave = sentinelSelectSlave(ri);

    /* We don't handle the timeout in this state as the function aborts
     * the failover or go forward in the next state. */
    // 没有合适的 slave ，直接终止故障转移操作
    if (slave == NULL) {

        // 没有可用的 slave 可以提升为新 master ，故障转移操作无法执行
        sentinelEvent(REDIS_WARNING,"-failover-abort-no-good-slave",ri,"%@");

        // 中止故障转移
        sentinelAbortFailover(ri);

    } else {

        // 成功选定新 master 

        // 发送事件
        sentinelEvent(REDIS_WARNING,"+selected-slave",slave,"%@");

        // 打开 instance 的升级标记
        slave->flags |= SRI_PROMOTED;

        // 记录被选中的 slave 
        ri->promoted_slave = slave;

        // 更新故障转移状态
        ri->failover_state = SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE;

        // 更新状态改变时间
        ri->failover_state_change_time = mstime();

        // 发送事件
        sentinelEvent(REDIS_NOTICE,"+failover-state-send-slaveof-noone",
            slave, "%@");
    }
}

// 向被选中的 slave 发送 SLAVEOF no one 命令
// 将它升级为新的 master 
void sentinelFailoverSendSlaveOfNoOne(sentinelRedisInstance *ri) {
    int retval;

    /* We can't send the command to the promoted slave if it is now
     * disconnected. Retry again and again with this state until the timeout
     * is reached, then abort the failover. */
    // 如果选中的 slave 断线了，那么在给定的时间内重试
    // 如果给定时间内选中的 slave 也没有上线，那么终止故障迁移操作
    // （一般来说出现这种情况的机会很小，因为在选择新的 master 时，
    // 已经断线的 slave 是不会被选中的，所以这种情况只会出现在
    //  slave 被选中，并且发送 SLAVEOF NO ONE 命令之前的这段时间内）
    if (ri->promoted_slave->flags & SRI_DISCONNECTED) {

        // 如果超过时限，就不再重试
        if (mstime() - ri->failover_state_change_time > ri->failover_timeout) {
            sentinelEvent(REDIS_WARNING,"-failover-abort-slave-timeout",ri,"%@");
            sentinelAbortFailover(ri);
        }
        return; // 下一次 cron 再试一试
    }

    /* Send SLAVEOF NO ONE command to turn the slave into a master.
     *
     * 向被升级的 slave 发送 SLAVEOF NO ONE 命令，将它变为一个 master 。
     *
     * We actually register a generic callback for this command as we don't
     * really care about the reply. We check if it worked indirectly observing
     * if INFO returns a different role (master instead of slave). 
     *
     * 这里没有为命令回复关联一个回调函数，因为 slave 是否已经转变为 master 可以
     * 通过向 slave 发送 INFO 命令来确认
     */
    retval = sentinelSendSlaveOf(ri->promoted_slave,NULL,0);
    if (retval != REDIS_OK) return;
    sentinelEvent(REDIS_NOTICE, "+failover-state-wait-promotion",
        ri->promoted_slave,"%@");

    // 更新状态
    // 这个状态会让 Sentinel 等待被选中的 slave 升级为 master 
    ri->failover_state = SENTINEL_FAILOVER_STATE_WAIT_PROMOTION;

    // 更新状态改变的时间
    ri->failover_state_change_time = mstime();
}

/* We actually wait for promotion indirectly checking with INFO when the
 * slave turns into a master. */
// Sentinel 会通过 INFO 命令的回复检查 slave 是否已经转变为 master 
// 这里只负责检查时限
void sentinelFailoverWaitPromotion(sentinelRedisInstance *ri) {
    /* Just handle the timeout. Switching to the next state is handled
     * by the function parsing the INFO command of the promoted slave. */
    if (mstime() - ri->failover_state_change_time > ri->failover_timeout) {
        sentinelEvent(REDIS_WARNING,"-failover-abort-slave-timeout",ri,"%@");
        sentinelAbortFailover(ri);
    }
}

// 判断故障转移操作是否结束
// 结束可以因为超时，也可以因为所有 slave 已经同步到新 master 
void sentinelFailoverDetectEnd(sentinelRedisInstance *master) {
    int not_reconfigured = 0, timeout = 0;
    dictIterator *di;
    dictEntry *de;

    // 上次 failover 状态更新以来，经过的时间
    mstime_t elapsed = mstime() - master->failover_state_change_time;

    /* We can't consider failover finished if the promoted slave is
     * not reachable. */
    // 如果新 master 已经下线，那么故障转移操作不成功
    if (master->promoted_slave == NULL ||
        master->promoted_slave->flags & SRI_S_DOWN) return;

    /* The failover terminates once all the reachable slaves are properly
     * configured. */
    // 计算未完成同步的 slave 的数量
    di = dictGetIterator(master->slaves);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);

        // 新 master 和已完成同步的 slave 不计算在内
        if (slave->flags & (SRI_PROMOTED|SRI_RECONF_DONE)) continue;

        // 已下线的 slave 不计算在内
        if (slave->flags & SRI_S_DOWN) continue;

        // 增一
        not_reconfigured++;
    }
    dictReleaseIterator(di);

    /* Force end of failover on timeout. */
    // 故障操作因为超时而结束
    if (elapsed > master->failover_timeout) {
        // 忽略未完成的 slave 
        not_reconfigured = 0;
        // 打开超时标志
        timeout = 1;
        // 发送超时事件
        sentinelEvent(REDIS_WARNING,"+failover-end-for-timeout",master,"%@");
    }

    // 所有 slave 都已完成同步，故障转移结束
    if (not_reconfigured == 0) {
        sentinelEvent(REDIS_WARNING,"+failover-end",master,"%@");
        // 更新故障转移状态
        // 这一状态将告知 Sentinel ，所有 slave 都已经同步到新 master 
        master->failover_state = SENTINEL_FAILOVER_STATE_UPDATE_CONFIG;
        // 更新状态改变的时间
        master->failover_state_change_time = mstime();
    }

    /* If I'm the leader it is a good idea to send a best effort SLAVEOF
     * command to all the slaves still not reconfigured to replicate with
     * the new master. */
    if (timeout) {
        dictIterator *di;
        dictEntry *de;

        // 遍历所有 slave 
        di = dictGetIterator(master->slaves);
        while((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *slave = dictGetVal(de);
            int retval;

            // 跳过已发送 SLAVEOF 命令，以及已经完成同步的所有 slave 
            if (slave->flags &
                (SRI_RECONF_DONE|SRI_RECONF_SENT|SRI_DISCONNECTED)) continue;

            // 发送命令
            retval = sentinelSendSlaveOf(slave,
                    master->promoted_slave->addr->ip,
                    master->promoted_slave->addr->port);
            if (retval == REDIS_OK) {
                sentinelEvent(REDIS_NOTICE,"+slave-reconf-sent-be",slave,"%@");
                // 打开 slave 的 SLAVEOF 命令已发送标记
                slave->flags |= SRI_RECONF_SENT;
            }
        }
        dictReleaseIterator(di);
    }
}

/* Send SLAVE OF <new master address> to all the remaining slaves that
 * still don't appear to have the configuration updated. */
// 向所有尚未同步新 master 的 slave 发送 SLAVEOF <new-master-address> 命令
void sentinelFailoverReconfNextSlave(sentinelRedisInstance *master) {   // NOTE: 还没有彻底 failover 完毕, 这个 master 依然是旧的 master SRI
    dictIterator *di;
    dictEntry *de;
    int in_progress = 0;

    // 计算正在同步新 master 的 slave 数量
    di = dictGetIterator(master->slaves);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);

        // SLAVEOF 命令已发送，或者同步正在进行
        if (slave->flags & (SRI_RECONF_SENT|SRI_RECONF_INPROG))
            in_progress++;  // 
    }
    dictReleaseIterator(di);

    // 如果正在同步的 slave 的数量少于 parallel-syncs 选项的值
    // 那么继续遍历 slave ，并让 slave 对新 master 进行同步
    di = dictGetIterator(master->slaves);
    while(in_progress < master->parallel_syncs &&   // 限制同时进行 SYNC 的数量, 避免直接压垮新的 master(其他 slave )
          (de = dictNext(di)) != NULL)
    {
        sentinelRedisInstance *slave = dictGetVal(de);
        int retval;

        /* Skip the promoted slave, and already configured slaves. */
        // 跳过新 master ，以及已经完成了同步的 slave 
        if (slave->flags & (SRI_PROMOTED|SRI_RECONF_DONE)) continue;

        /* If too much time elapsed without the slave moving forward to
         * the next state, consider it reconfigured even if it is not.
         * Sentinels will detect the slave as misconfigured and fix its
         * configuration later. */
        if ((slave->flags & SRI_RECONF_SENT) &&
            (mstime() - slave->slave_reconf_sent_time) >
            SENTINEL_SLAVE_RECONF_TIMEOUT)
        {
            // 发送重拾同步事件
            sentinelEvent(REDIS_NOTICE,"-slave-reconf-sent-timeout",slave,"%@");
            // 清除已发送 SLAVEOF 命令的标记
            slave->flags &= ~SRI_RECONF_SENT;
            slave->flags |= SRI_RECONF_DONE;
            // 稍后再次进行尝试 
        }

        /* Nothing to do for instances that are disconnected or already
         * in RECONF_SENT state. */
        // 如果已向 slave 发送 SLAVEOF 命令，或者同步正在进行
        // 又或者 slave 已断线，那么略过该服务器
        if (slave->flags & (SRI_DISCONNECTED|SRI_RECONF_SENT|SRI_RECONF_INPROG))
            continue;

        /* Send SLAVEOF <new master>. */
        // 向 slave 发送 SLAVEOF 命令，让它同步新 master 
        // 发送 SLAVEOF 之后, sync 的过程是直接交给这个 slave 跟 new master 的, leader sentinel 并不直接参与
        // slave SRI_RECONF_DONE 的设置, 参考: sentinelRefreshInstanceInfo()
        retval = sentinelSendSlaveOf(slave,
                master->promoted_slave->addr->ip,
                master->promoted_slave->addr->port);
        if (retval == REDIS_OK) {

            // 将状态改为 SLAVEOF 命令已发送
            slave->flags |= SRI_RECONF_SENT;
            // 更新发送 SLAVEOF 命令的时间
            slave->slave_reconf_sent_time = mstime();
            sentinelEvent(REDIS_NOTICE,"+slave-reconf-sent",slave,"%@");
            // 增加当前正在同步的 slave 的数量
            in_progress++;
        }
    }
    dictReleaseIterator(di);

    /* Check if all the slaves are reconfigured and handle timeout. */
    // 判断是否所有 slave 的同步都已经完成
    sentinelFailoverDetectEnd(master);
}

/* This function is called when the slave is in
 * SENTINEL_FAILOVER_STATE_UPDATE_CONFIG state. In this state we need
 * to remove it from the master table and add the promoted slave instead. */
// 这个函数在 master 已下线，并且对这个 master 的故障迁移操作已经完成时调用
// 这个 master 会被移除出 master 表格，并由新的 master 代替
void sentinelFailoverSwitchToPromotedSlave(sentinelRedisInstance *master) {

    /// 选出要添加的 master, 目前版本只会是 master->promoted_slave, master 必然是旧的 master
    sentinelRedisInstance *ref = master->promoted_slave ?
                                 master->promoted_slave : master;

    // 发送更新 master 事件
    sentinelEvent(REDIS_WARNING,"+switch-master",master,"%s %s %d %s %d",
        // 原 master 信息
        master->name, master->addr->ip, master->addr->port,
        // 新 master 信息
        ref->addr->ip, ref->addr->port);

    // 用新 master 的信息代替原 master 的信息(方便这个 sentinel 管理)
    sentinelResetMasterAndChangeAddress(master,ref->addr->ip,ref->addr->port);
}

// 执行故障转移(状态机切换), 每次的状态轮转,都要等一次 sentinel timer 的 cron 调度
void sentinelFailoverStateMachine(sentinelRedisInstance *ri) {
    redisAssert(ri->flags & SRI_MASTER);
    // ri 只会是，也只能是旧 master
    // 新的 master, 即将被 promote 的 slave 会记录在 master->promoted_slave 里面

    // master 未进入故障转移状态，直接返回
    if (!(ri->flags & SRI_FAILOVER_IN_PROGRESS)) return;

    switch(ri->failover_state) {

        // 等待故障转移开始
        case SENTINEL_FAILOVER_STATE_WAIT_START:
            sentinelFailoverWaitStart(ri);
            break;

        // 选择新 master 
        case SENTINEL_FAILOVER_STATE_SELECT_SLAVE:
            sentinelFailoverSelectSlave(ri);
            break;
        
        // 升级被选中的 slave 为新 master 
        case SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE:
            sentinelFailoverSendSlaveOfNoOne(ri);
            break;

        // TODO:(DONE) 升级超时了怎么办? leader sentinel 在很多地方都有做超时检查，
        // 一旦超时，旧直接 abort failover，然后其他 sentinel 超时后，
        // 就会更新 epoch，然后发起新一轮的选举，新一轮的 failvoer
        // 等待升级生效，如果升级超时，那么重新选择新 master 
        // 具体情况请看 sentinelRefreshInstanceInfo() 函数
        case SENTINEL_FAILOVER_STATE_WAIT_PROMOTION:
            sentinelFailoverWaitPromotion(ri);
            break;

        // 向 slave 发送 SLAVEOF 命令，让它们同步新 master 
        case SENTINEL_FAILOVER_STATE_RECONF_SLAVES:
            sentinelFailoverReconfNextSlave(ri);
            break;
    }
}

/* Abort a failover in progress:
 *
 * 关于中途停止执行故障转移：
 *
 * This function can only be called before the promoted slave acknowledged
 * the slave -> master switch. Otherwise the failover can't be aborted and
 * will reach its end (possibly by timeout). 
 *
 * 这个函数只能在被选中的 slave 升级为新的 master 之前调用，
 * 否则故障转移就不能中途停止，
 * 并且会一直执行到结束。
 */
void sentinelAbortFailover(sentinelRedisInstance *ri) {
    redisAssert(ri->flags & SRI_FAILOVER_IN_PROGRESS);
    redisAssert(ri->failover_state <= SENTINEL_FAILOVER_STATE_WAIT_PROMOTION);

    // 移除相关标识
    ri->flags &= ~(SRI_FAILOVER_IN_PROGRESS|SRI_FORCE_FAILOVER);

    // 清除状态, ODOWN 还是在, 下一轮 cron 会再次发起的, sentinelStartFailoverIfNeeded() 里面
    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    ri->failover_state_change_time = mstime();

    // 清除新 master 的升级标识
    if (ri->promoted_slave) {
        ri->promoted_slave->flags &= ~SRI_PROMOTED;
        // 清空新服务器
        ri->promoted_slave = NULL;
    }
}

/* ======================== SENTINEL timer handler ==========================
 * This is the "main" our Sentinel, being sentinel completely non blocking
 * in design. The function is called every second.
 * 哨兵是一个非阻塞的设计
 * -------------------------------------------------------------------------- */

/* Perform scheduled operations for the specified Redis instance. */
// 对给定的 instance 执行定期操作
void sentinelHandleRedisInstance(sentinelRedisInstance *ri) {

    /* ========== MONITORING HALF ============ */
    /* ==========     监控操作    =========*/

    /* Every kind of instance */
    /* 对所有类型 instance 进行处理 */

    // 如果有需要的话（例如第一次连接的时候），创建连向 instance 的网络连接
    sentinelReconnectInstance(ri);

    // 根据情况，向 instance 发送 PING、 INFO 或者 PUBLISH 命令
    sentinelSendPeriodicCommands(ri);

    /* ============== ACTING HALF ============= */
    /* ==============  故障检测   ============= */

    /* We don't proceed with the acting half if we are in TILT mode.
     * TILT happens when we find something odd with the time, like a
     * sudden change in the clock. */
    // 如果 Sentinel 处于 TILT 模式，那么不执行故障检测。
    if (sentinel.tilt) {

        // 如果 TILI 模式未解除，那么不执行动作
        if (mstime()-sentinel.tilt_start_time < SENTINEL_TILT_PERIOD) return;

        // 时间已过，退出 TILT 模式
        sentinel.tilt = 0;
        sentinelEvent(REDIS_WARNING,"-tilt",NULL,"#tilt mode exited");
    }

    /* Every kind of instance */
    // 检查给定 instance 是否进入 SDOWN 状态(无论是 slave 还是 master 都会标记 SDOWN)
    // salve 的 SDOWN 不会在这里直接起作用，
    sentinelCheckSubjectivelyDown(ri);

    /* Masters and slaves */
    if (ri->flags & (SRI_MASTER|SRI_SLAVE)) {
        /* Nothing so far. */
    }

    /* Only masters */
    /* 对 master 进行处理 */
    if (ri->flags & SRI_MASTER) {

        // 判断 master 是否进入 ODOWN 状态
        sentinelCheckObjectivelyDown(ri);

        // 如果 master 进入了 ODOWN 状态，那么开始一次故障转移操作
        // 强制向其他 Sentinel 发送 SENTINEL is-master-down-by-addr 命令
        // 刷新其他 Sentinel 关于 master 的状态
        // TODO: 是怎么做到：投过票的 sentinel 不会自己主动发起 leader sentinel 竞选的？
        if (sentinelStartFailoverIfNeeded(ri))
            sentinelAskMasterStateToOtherSentinels(ri,SENTINEL_ASK_FORCED); 
            // TODO:（DONE）为什么这里要 ASK_FORCED ？因为这个时投票，投票自然是 ASAP 的

        // 执行故障转移
        sentinelFailoverStateMachine(ri);

        // 如果有需要的话，向其他 Sentinel 发送 SENTINEL is-master-down-by-addr 命令
        // 刷新其他 Sentinel 关于 master 的状态
        // 这一句是对那些没有进入 if(sentinelStartFailoverIfNeeded(ri)) { /* ... */ }
        // 语句的 master 使用的（因为一开始，仅仅是当前 sentinel 觉得 SDOWN，还没有其他 sentinel 的看法，所以是不能 ODOWN 的，也就不可能 failover）
        // 收集其他 sentinel 对于当前 master 的看法（可能自己不觉得 master SDOWN 了，但是其他 sentinel 觉得 master 已经 SDOWN 了）
        sentinelAskMasterStateToOtherSentinels(ri,SENTINEL_NO_FLAGS);
    }
}

/* Perform scheduled operations for all the instances in the dictionary.
 * Recursively call the function against dictionaries of slaves. */
// 对被 Sentinel 监视的所有 instance （包括 master 、 slave 和其他 Sentinel ）
// 进行定期操作
//
//  sentinelHandleRedisInstance
//              |
//              |
//              v
//            master
//             /  \
//            /    \
//           v      v
//       slaves    sentinels
void sentinelHandleDictOfRedisInstances(dict *instances) {
    dictIterator *di;
    dictEntry *de;
    sentinelRedisInstance *switch_to_promoted = NULL;

    /* There are a number of things we need to perform against every master. */
    // 遍历多个 instance ，这些 instance 可以是多个 master 、多个 slave 或者多个 sentinel
    di = dictGetIterator(instances);
    while((de = dictNext(di)) != NULL) {

        // 取出 instance 对应的 instance 结构
        sentinelRedisInstance *ri = dictGetVal(de);

        // 执行调度操作
        sentinelHandleRedisInstance(ri);

        // 如果被遍历的是 master ，那么递归地遍历该 master 的所有 slave 
        // 以及所有 sentinel
        // 实际的检查顺序: 先检查 slave 的 sentinel, 然后是所有的 slave, 最后才是旧 master
        if (ri->flags & SRI_MASTER) {

            // 所有 slave 
            sentinelHandleDictOfRedisInstances(ri->slaves);

            // 所有 sentinel
            sentinelHandleDictOfRedisInstances(ri->sentinels);

            // 对已下线 master （ri）的故障迁移已经完成
            // ri 的所有 slave 都已经同步到新 master 
            // 切记, 整个 failover 过程, 依旧是采用旧 master 作为状态机的核心 instance
            // 相应的, 这个旧的 master 也会将 promoted_slave 的信息记录下来的
            if (ri->failover_state == SENTINEL_FAILOVER_STATE_UPDATE_CONFIG) {
                // 已选出新的 master 
                switch_to_promoted = ri;    // 这个 ri 还是旧的 master
            }
        }
    }

    // 将原 master （已下线）从 master 表格中移除，并使用新 master 代替它
    if (switch_to_promoted)
        sentinelFailoverSwitchToPromotedSlave(switch_to_promoted);

    dictReleaseIterator(di);
}

/* This function checks if we need to enter the TITL mode.
 *
 * 这个函数检查 sentinel 是否需要进入 TITL 模式。
 *
 * The TILT mode is entered if we detect that between two invocations of the
 * timer interrupt, a negative amount of time, or too much time has passed.
 *
 * 当程序发现两次执行 sentinel 之间的时间差为负值，或者过大时，
 * 就会进入 TILT 模式。
 *
 * Note that we expect that more or less just 100 milliseconds will pass
 * if everything is fine. However we'll see a negative number or a
 * difference bigger than SENTINEL_TILT_TRIGGER milliseconds if one of the
 * following conditions happen:
 *
 * 通常来说，两次执行 sentinel 之间的差会在 100 毫秒左右，
 * 但当出现以下内容时，这个差就可能会出现异常：
 *
 * 1) The Sentinel process for some time is blocked, for every kind of
 * random reason: the load is huge, the computer was frozen for some time
 * in I/O or alike, the process was stopped by a signal. Everything.
 *    sentinel 进程因为某些原因而被阻塞，比如载入的数据太大，计算机 I/O 任务繁重，
 *    进程被信号停止，诸如此类。
 *
 * 2) The system clock was altered significantly.
 *    系统的时钟产生了非常明显的变化。
 *
 * Under both this conditions we'll see everything as timed out and failing
 * without good reasons. Instead we enter the TILT mode and wait
 * for SENTINEL_TILT_PERIOD to elapse before starting to act again.
 *
 * 当出现以上两种情况时， sentinel 可能会将任何 instance 都视为掉线，并无原因地判断 instance 为失效。
 * 为了避免这种情况，我们让 sentinel 进入 TILT 模式，
 * 停止进行任何动作，并等待 SENTINEL_TILT_PERIOD 秒钟。 
 *
 * During TILT time we still collect information, we just do not act. 
 *
 * TILT 模式下的 sentinel 仍然会进行监控并收集信息，
 * 它只是不执行诸如故障转移、下线判断之类的操作而已。
 */
// TODO: 为什么不通过开始时间来解决这个问题呢？本质上，sentinel 并不依赖于实现的 UNIX 时间呀？
// 因为哨兵判断监控的 redis-server UP or DOWN 是通过超时来进行判断的
// 也就意味着，一旦 host 机的时间发生了变化，将会导致判断失误，所以要进入 TILT 模式
// 让这个 sentinel 不参与实质性决策，直到时间波动恢复正常，退出 TILT 模式
void sentinelCheckTiltCondition(void) {

    // 计算当前时间（1970 to now），将会受到 time-server 的影响
    mstime_t now = mstime();

    // 计算上次运行 sentinel 和当前时间的差
    mstime_t delta = now - sentinel.previous_time;

    // 如果差为负数，或者大于 2 秒钟，那么进入 TILT 模式
    // delta < 0，负数则是 host 的时间被设置为过去的时间了
    // delta > SENTINEL_TILT_TRIGGER，当前 sentinel 进行了一个很长时间的阻塞任务
    // 这个阻塞可能会导致这个 sentinel 判断：当前监控的所有 redis-server 都 donw 了
    // 然而，这是这个 sentinel 自己阻塞太长时间的锅
    if (delta < 0 || delta > SENTINEL_TILT_TRIGGER) {
        // 打开标记
        sentinel.tilt = 1;
        // 记录进入 TILT 模式的开始时间
        sentinel.tilt_start_time = mstime();
        // 打印事件
        sentinelEvent(REDIS_WARNING,"+tilt",NULL,"#tilt mode entered");
    }

    // 更新最后一次 sentinel 运行时间，以便判断当前 sentinel 的判断是否可信
    sentinel.previous_time = mstime();
}

// sentinel 模式的主函数，由 redis.c/serverCron() 函数调用
void sentinelTimer(void) {

    // 记录本次 sentinel 调用的事件，
    // 并判断是否需要进入 TILT 模式
    sentinelCheckTiltCondition();

    // 执行定期操作
    // 比如 PING  instance 、分析 master 和 slave 的 INFO 命令
    // 向其他监视相同 master 的 sentinel 发送问候信息
    // 并接收其他 sentinel 发来的问候信息
    // 执行故障转移操作，等等
    sentinelHandleDictOfRedisInstances(sentinel.masters);

    // 运行等待执行的脚本
    sentinelRunPendingScripts();

    // 清理已执行完毕的脚本，并重试出错的脚本
    sentinelCollectTerminatedScripts();

    // 杀死运行超时的脚本
    sentinelKillTimedoutScripts();

    /* We continuously change the frequency of the Redis "timer interrupt"
     * in order to desynchronize every Sentinel from every other.
     * This non-determinism avoids that Sentinels started at the same time
     * exactly continue to stay synchronized asking to be voted at the
     * same time again and again (resulting in nobody likely winning the
     * election because of split brain voting). */
    // 修改server.hz，增加其随机性，以避免投票选举时发生冲突；
    server.hz = REDIS_DEFAULT_HZ + rand() % REDIS_DEFAULT_HZ;
}
