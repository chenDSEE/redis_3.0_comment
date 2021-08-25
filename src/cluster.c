/* Redis Cluster implementation.
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

#include "redis.h"
#include "cluster.h"
#include "endianconv.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>

/**
 * Main properties and rationales of the design
*/
/**
 * node 与 node 之间的联系：
 * This second high port(port 10000+) is used for the **Redis Cluster Bus**, that is a node-to-node communication channel using a binary protocol.
 * The Redis Cluster Bus is used by nodes for failure detection, configuration update, failover authorization and so forth. 
 * Clients should never try to communicate with the cluster bus port, but always with the normal Redis command port
 * The command port and cluster bus port offset is fixed and is always 10000.(cluster port 一般跟普通 port 相差 10000)
 * 
 * Nodes use a gossip protocol(via the Redis Cluster Bus) to propagate information about the cluster in order to discover new nodes,
 * to send ping packets to make sure all the other nodes are working properly,
 * and to send cluster messages needed to signal specific conditions.
 * The cluster bus is also used in order to propagate Pub/Sub messages across the cluster
 * 
 * 当整个 cluster 的 bus 建立起来之后，将会是网状的结构！
 * 
 * In a cluster of N nodes, every node has N-1 outgoing TCP connections, and N-1 incoming connections.
 * 所以两个 node 之间，是会建立两条 socket 的，一条 incoming，一条 outgoing
 */
/**
 * Redis Cluster is not able to guarantee strong consistency.
 * beacuse redis uses asynchronous replication
*/
/**
 * Availability
 * redis cluster 模式下的 availability 是建议在这样的一个前提下的：
 * 只有少数 node 网络异常失效了；当有大量 node 网络分区了，整个 redis cluster 也将会停住，无法提供服务
 * 所谓的少数是指：少数 master node down 了，但是这些 down 的 node 都有 slave 依旧存活；
 * 也就是说：有效的网络分区中，所有分区都存在（既可以是 master node，也可以是 slave node）
 * 
 * This means that Redis Cluster is designed to survive failures of a few nodes in the cluster,
 * but it is not a suitable solution for applications that require availability in the event of large net splits.
 * redis 是没有办法应对大面积 node 失效的，这个时候，整个 redis cluster 都将会停止服务
*/
/**
 * Gossip 与 redis-cluster
 * 首先，redis-cluster 是采用 Gossip 的方式，实现分散式的数据最终一致性同步。
 * 主要同步 node 的配置信息、运行状态信息、分片的管理信息等一系列的源数据
 * 这种分散式的数据同步方式，必然是会有一定同步延时的
*/
// TODO:(DONE) node 之间是怎么建立联系的？
// TODO:(DONE) 超时怎么重试？
/**
 * 约定：
 * 1. node A 主动连接 node B
 * 2. link AB(node A 主动连接 node B 的 link)
 * 3. link BA(node B 主动连接 node A 的 link)
 * 4. 大部分 node flag 都是记录在主动发起连接的 link->node.flag 上面
 * 5. 被动的连接，只是一个无情的回 PONG 机器
 * 6. 两个 node 之间会有两条 TCP socket 连接
 * 
 *                node A                                     |                 node B                                                                        
 *                                                           | 
 * 1. node A 收到 CLI 发来的 MEET                            |                                                                          
 * 2. node A 创建 link AB，信息节点 node B                   |                                                                                    
 * 3. 等到 clusterCron 的时候，node A 主动                   |                                                                     
 *    conneect node B，                                     |                                                   
 * 4. Cron 设置相应事项（相当于完成当前状态动作）:            |                                                                            
 *    1) link AB->node = node B；                           |                                                            
 *    2) 将一个 MEET 消息加入 link AB->sndbuf 中            |                                                                           
 *    3) 注册相应的事件，发起 TCP 三次握手                   |                                                                     
 *                                                          |                                 
 *    此时 node B flag 为：                                 |                                                      
 *    REDIS_NODE_MEET + REDIS_NODE_HANDSHAKE                |                                                                       
 *                                                          |
 *                                               ------- TCP SYN -------->
 *                                              <----- TCP SYN, ACK -----
 *                                               --------- ACK ---------->
 *                                                          |
 * 5. 三次握手完成，write 事件可写，                         |     5. accept，并建立相应的 link AB，由于是被动连接  
 *    将 link AB->sndbuf 中的 MEET 发送出去                 |         所以 link AB->node == NULL;       
 *                                                          |         注册 read 事件监听          
 *                                                          |
 *                                               -------- MEET --------->
 *                                                         |        
 *                                                         |      6. read 事件被触发，开始处理 MEET 消息（不含 Gossip）
 *                                                         |         clusterProcessPacket()
 *                                                         |      7. 实际上这个函数不会有很多分支被 hit 中
 *                                                         |         因为 sender == NULL；
 *                                                         |         link AB->node == NULL
 *                                                         |         type == CLUSTERMSG_TYPE_MEET;
 *                                                         |         最后也就 createNode，加入 server.cluster->nodes; 
 *                                                         |         但是不被 link AB->node 记录
 *                                                         |      8. 向 node A 返回 PONG（包含 node B 的 node-id），作为 MEET 的回应
 *                                              <------- PONG --------
 * 9. read 事件触发，接收到 PONG                           |      9. 因为 step 8 中加入了一个新的 node A；但是 node A->link == NULL;
 *    进入 clusterProcessPacket()                          |         所以在 clusterCron 里面，将会主动发起 link BA 的连接；
 *    处理 PONG 消息（不含 Gossip）                        |         ... 重复 TCP 过程、node handshake 过程（4、5、6、7、8）                                                                
 *10. 同样，sender 是 NULL 的；因为                        |         
 *    最开始并不知道 node B 的 node-id；                   |              
 *    所以 node A 是随机填的，然后放进哈希表里             |                    
 *    直到现在才正式 rename，重新设置哈希表                |                                                                                 
 *    MEET 标志位取消，HANDSHAK 取消                       |                                          
 *                                                                  
 * 
 * 后续 node C 怎么加入这个 AB 集群？
 * 很简单，通过 redis-cli 向 node A 或 node B 发送 cluster meet C_IP C_port 就可以了
 * node A 会主动 connect node C；connect、handshake 完成之后，将会由 node A 向其他 node 发送 PING\PONG 的时候，
 * 带上 node C 的信息，Gossip 给 node B（也有可能是 node A 先把 node B 的信息 Gossip 给 node C）
 * 然后剩下的 node 跟上面一样，相会建立 socket 连接，并记录新的 node 就好了
*/
/**
 * 你可能回很好奇：为什么之前的 sentinel 可以单个连接，但是 cluster node 之间却要两个连接。这是为什么呢？
 * 1. sentinel 模式下，只需要 sentinel 观察 redis-server 的状态就可以了，并不需要 redis-server 观察 sentinel 的状态
 * 2. sentinel 之间的通信，是通过 channel broadcast 的方式实现的
 * 
 * 3. 但是 cluster node 需要进行相互观察，要是观察跟被观察都用同一个 socket 将会产生很多问题：
 *    1) 其中一方觉得 link down 了，直接 close 掉 socket，对端也回被动关闭 socket；那谁负责重连？
 *       谁知道 link down 究竟是 node down 还是网络波动呢？网络波动双方都可以重连，但是 node down 不行
 *       node 都 down 了，重连个啥？但是 node down 跟 网路波动，node 是没有办法区分的
 *    2) 拥有控制权的 link 才是某个 node 用来观察另一个 node 的方法；被动的 link 就不应该被做出任何操作
 *       被动触发 PONG 动作就好
*/
/**
 * 一个 node 怎么才算在一个集群里面呢？
 * 能够被这个集群中其他的 node Gossip，传播到其他的 node 中；
 * 又或者说：集群中的 node 是会相会建立起 cluster bus 的（双向 socket link）
*/
// TODO:(DONE) resharde 过程
/**
 * 注意事项：
 * 1. resharde 必然是一个管理员手动发起的过程，全程是需要管理员自己手动操作的（脚本只是方便管理员操作罢了）
 * 2. redis 的 resharde 过程，是可以进行热操作的，虽然会一定程度影响服务，但是不会彻底 block 住两边的 redis instance
 * 3. 其实整个 resharde 过程中，无论是无论是在 noda A 还是 node B 里面，被标记为 migrate\import 的 slot 都不应该相应任何的读写操作了
 *    标记要被移动的 slot，尽可能避免访问，是 step 1 跟 step 2 最大的作用
 *    读操作：需要 redis-cli 支持 ASK 的 error，因为 CMD 里面指定的 key 既可能在 node A 也可能在 node B。而且 ASK 之后，也可能会发生 node 变化
 *            所以读操作中的 ASK 跟后续的读操作，应当是一个原子性的事务操作
 *    写操作：那就更明显了，你不停的向 slot 写入 key，那就得不停的迁移，而且这时候你会很疑惑：究竟应该向 node A 写入，还是 node B？
 *            （必然是先问 node B 的，因为整个 cluster 里面，slot 100 仍然是归属于 node B）；
 *            在不在 node B + 在不在 node A 组合情况：
 *            在 node B，不在 node A：可以直接修改。因为 redis 是单线程的，一定会把修改、新增、删除、查询执行完，然后再进行新的迁移
 *            不再 node B，可能在 node A：1) node B 返回 ASK 错误，让 cli 去问一下 node A，在 node A 的话，那就向 node A 修改、读取、删除
 *            不再 node B，可能在 node A：2) 要是想新写入一个新的，那是不行的，必然会是失败的：getNodeByQuery() 里面，missing_key >= 1; 而且 node A 的 slot 1000
 *                                           在 import_slot，所以会再次返回一个 ASK（node B 也是返回 ASK），基本上可以视为不能够写入的
 * 
 *            这样一来，就可以避免新的写入跟 import 的写入相互覆盖的问题
 * 4. step 1 跟 step 2 可以颠倒，没区别的
 * 
 * 过程（把 node B 的 100 slot 向 node A 迁移）：
 * 1. 向 node A 发送 CLUSTER SETSLOT 100 IMPORTING node-B-ID, 告知 node A：即将从 node B 中迁移 100 slot 给你，
 *    你做好准备；
 *    这时候，node A 并不会修改自己的 server.cluster->slots[100] 所指向的 node；
 *    将 importing_slots_from[100] 登记为 node B（备查，当由 slot 100 的 key CMD 到了 node A 的话，getNodeByQuery() 有用）
 * 2. 向 node B 发送 CLUSTER SETSLOT 100 MIGRATE node-A-ID, 告知 node B：即将从你这里，迁出 slot 100 给 node A
 *    这时候，node B 并不会修改自己的 server.cluster->slots[100] 所指向的 node；（当访问这个 slot 的时候，仍然算 node B 的）
 *    将 migrating_slots_to[100] 等级为 node A（备查，当由 slot 100 的 key CMD 到了 node B 的话，getNodeByQuery() 有用）
 * 
 * 开始正式迁移：
 * while(count(key in slot 100) > 0)
 * {
 *     3. 向 node B 询问：CLUSTER GETKEYSINSLOT 100 <count>
 *        <count> 参数可以控制 step 4 的迁移数量，避免长时间 block
 * 
 *     for(key in step-3-count)
 *     {
 *         4. 向 node B 发送：MIGRATE node-A-ip node-A-port key-in-slot100 0 10；
 *            一个个 key 进行迁移
 *            因为单线程的缘故，每一个 key 的 migrate 都是原子的。
 *            注意：step 4 是同步阻塞的，node B 一定会同步等待 node A 返回结果才能够结束 step 4
 *            只有确认了这个 key 成功写入 node A 之后，才能够删除掉 key 在 node B 的副本，向 AOF 写入记录，向 salve 发送相应的 DEL
 *            超时的话，就要再次执行本命令。
 *            为了避免 bigkey block 住 node A，node B 一次最多发送 64K 的内容
 *     }
 * }
 * 
 * 5. slot 100 里面的全部 key 迁移完毕。向 node A 发送 CLUSTER SETSLOT 100 node node-A-ID
 *    让 node A 正式拥有 slot 100；
 * 6. node A 在后续的连接中，将会通过 PING\PONG\MEET 等形式的消息，传播自己拥有 slot 100；
 *    一段时间后，整个 cluster 将会达成一致的共识
 *    注意：在没有达成一致共识之前也没有关系，可以正常用的：
 *          1) 其他 node：会让 redis-cli 去访问 node B（MOVE），仍然觉得 slot 100 是 node B 的
 *          2) node B：自己还在 migrate 状态，让你去 ASK node A
 *          3) node A：已经拥有了 slot 了，直接接受这个 CMD
*/
// TODO:(DONE) cluster master-slave 建立过程
/**
 * 前提：
 * 1. 即将成为 slave 的 node 不能有任何的 slot
 * 2. 每一个 slave 以前都是一个 cluster 模式的 master
 * 3. 首先所有被配置的 node，都要已经在同一个 cluster 里面（meet 所有 node 了）
 * 4. slave 可以随意的成为其他 master 的 slave，完成了复制同步就好了
 * 
 * 过程（node B 成为 node A 的 slave）：
 * 1. node A, node B 分别采用 cluster 启动；
 * 2. node A，node B 通过 MEET 加入同一个 cluster；
 * 3. （node A 有没有 slot 都可以，但是 node B 一定不能有 slot）
 * 4. 向 node B 发送 CLUSTER REPLICATE node-A-ID
 * 
 * node B:
 * 5. 设置 node_B.slaveof = node_A;（毕竟 cluster 追求的是：通过 Gossip，
 *    让整个 cluster 里面的每一个 node 都拥有相同的网络拓补。知晓整个 cluster 的 node 状态，互联情况）
 * 6. 通过 replication.c 里面的函数，完成异步 connect、SYNC\PSYNC
 *    slot 的分布情况则是通过 Gossip 来完成同步的。key-value DB 则是通过 replication.c 的函数来完成的
 * 
 * node A:
 * 7. 通过 replication.c 里面的代码，完成相应的 accept、SYNC\PSYNC。是通过 redis 端口建立的 replication socket。
 *    而不是 cluster 的端口（10000 偏移）建立的
 * 8. 通过 B --PONG--> A 或者 B --PING--> 的方式，传播 node B slaveof node A 的事情。
 *  （其他 node 也是一样的，通过 cluster bus Gossip 完成）
 * 
 * 再有其他 node 连上来也是一样的
*/
/**
 * REDIS_NODE_PFAIL 的标记位点：
 * 1. 自己对于某个 node 的看法：在 clusterCron() 里面被标记上（因为超时不回 PONG 才算 PFAIL 嘛）
 * 2. 其他 node 对于某个 node 的看法：在 Gossip 里面获得
 * 
 * REDIS_NODE_FAIL 的标记位点：
 * 1. 自己对于某个 node 的看法：
 * 2. 其他 node 对于某个 node 的看法：
 *    1) 获得 CLUSTERMSG_TYPE_FAIL 类型消息，直接标记这个 node 是 REDIS_NODE_FAIL
 *    2) 在 Gossip 里面获得 REDIS_NODE_FAIL 状态
*/
/**
 * High Availability 的另一种途径：
 * Using replicas migration, masters no longer replicated by any slave will 
 * receive one from a master which is covered by multiple slaves.
 * 能够更有效的利用 cluster 里面的所有 slave，自动调整 master--->slave 的 layout，
 * 从而自动、持续不断的应对可能到来的 failure 故障
 * 
 * 重点：在整个 cluster 范围内，从拥有最多 slave 的 master 中，
 * 把最末尾(name 的字符排序，避免 master 的所有 slave 都想转向)的那个 salve 
 * 转向集群中没有任何 slave 的 master node。
 * 只需要让每个 slave node 在 clusterCron() 里面进行判断就好了，剩下的代码都在 replication.c 里面
*/
// TODO:(DONE) cluster 集群 UP\DOWN 的判断 + automatic failover 过程
/**
 * node FAIL 的判断过程：
 * node A、node B PING 不通 node C
 * 1. node A, node B 在 clusterCron 中遍历所有 node，发现 node C 很长时间都是没有回复 PONG 了
 * 2. node A 先将 node C 视为 PFAIL，等待下一 Gossip 的时候，想其他 node 发送：node A 觉得 node C PFAIL 的附加 Gossip 信息
 * 3. node B 觉得 node C PFAIL，同样也会等待下次 Gossip 的时候，告知其他 node
 * 4. node B 收到了 node A 发过来的 Gossip：node A 觉得 node C PFAIL
 *    4.1 clusterNodeAddFailureReport() 在这里会不断统计认为 node C 是 PFAIL 的 master node 数量，方便 step 6.1 干活
 * 5. node B 进入了 markNodeAsFailingIfNeeded() 函数，准备进行 node C PFAIL-->FAIL 的判断
 * 6. 当大多数的 node 都觉得 node C 是 PFAIL 的，且 node B 自己也认为 node C 是 PFAIL 的，那么 node B 就正式将 node C 视为 FAIL
 *    6.1 只有 master node 能够参与投票；slave 可以把 master 的看法 Gossip 出去，但是 slave 对于 node C 的看法是不会被 node B 采纳的
 *        needed_quorum = (server.cluster->size / 2) + 1；显然 server.cluster->size
 *    6.2 node B 都不觉得 node C 的 PFAIL 了，自然是不会把 node C 设置为 FAIL 啦
 *        不然会扩大网络波动的影响范围
 * 7. node B 成功确认 node C 的 FAIL，向其他 node 传播 CLUSTERMSG_TYPE_FAIL 消息（广播出去的），直接让其他 node 将 node C 视为 FAIL
 *    7.1 既然在 step 6 中确认了：大多数 node 都认为 node C 是 FAIL 的。那么其他 node 应当直接接受 node B 的判断结果，
 *        让 cluster 中的所有 node 更快达成一致的看法（而不是每一个 node 都自己判断一次，这样很难达成一致的）
 *        node B 做出了判断，剩下的理应是让整个 cluster 知晓并认同这个判断
 * 
 * PS: step 3，step 4 顺序可以互换，没有关系
 * 
 * 
 * failover 过程：
 * 假设：node C1\C2\C3 是 node C 的 slave
 * 1. node C DOWN，并且被其他 node 发现，标识为 FAIL，经过一定时间后，FAIL 被某一个 slave 收到了
 * 2. 假设是 node C1 先收到，然后是 C2, C3.
 * 
 * clusterCron()
 * {
 *     clusterHandleSlaveFailover()
 *     {
 *         3. C1\C2\C3 分别在 clusterCron() 里面的 clusterHandleSlaveFailover() 
 *            确认自己的 master 是 DONE 的，就开始进行 automatic failover 的过程
 *         4. automatic failover 将会执行很多轮才能够完成整个过程
 *         automatic round 1: 计算出即将发起 failover author 的投票开始时间
 *                            留出一些时间，让同一个 master 的 slave 也知晓 master DONW 的事实
 *                            并开始准备自己的 automatic failover
 *                            (automatic failover 超时的后备军)
 *         automatic round 2 to n: 等待同一个 master 的其他 slave，知晓并达成一致：master DOWN，
 *                                 开始相应的 failover_auth_time 设置
 *         automatic round n + 1: 本 node 估摸着自己可以开始 automatic failover 的 new master 竞选了；
 *                                发起 author 请求 CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST
 *         等待其他具有投票权的 master 返回投票。并不断检查是否达到了票数要求
 * 
 *         automatic round n + 2: 通过 CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST，
 *                                本 node 已经收集到了足够多的选票，可以成为 master
 * 
 *         5. 接管相应的 slot，当完成所有准备工作之后，clusterBroadcastPong(CLUSTER_BROADCAST_ALL);
 *            让其他 node 知晓自己完成 failover 的事实
 *         
 *         其他没有成功当选的 slave，将会收到新 master 发过来的告知，并且利用这个告知，恢复 myself->slaveof
 *         从而终止自己还没有完成的选举
 *     }
 * }
 * 
 * 
 * 具体的，直接看 clusterHandleSlaveFailover() 的 automatic round 吧
 * 
 * 
*/
// TODO:(DONE) automatic failover 是怎么应对 error case 的？
/**
 * error case 1: 某个 slave node 发起的选举超时
 * 实际上所有的 slave 在发现自己的 master DOWN 之后，都会琢磨着发起选举，自己成为 master
 * 但是不同的 slave 发起的时间是会有差距的，跟原 master 数据差异越大的，越晚发起属于自己的投票选举。
 * 如此一来，当新 master 还没选出来的时候，所有 slave 就会不断尝试发起选举，成为 master
 * 
 * error case 2: 多个 slave node 发起的投票选举间隔过小，不会导致 cluster 出现多个相同的 master 吗？
 * 不会，因为只有 server.cluster->size - 1 个 master node 才有投票权的；
 * 而且每一个拥有投票权的 master node 又会受到如下的投票限制：
 * 1. 短时间内不可以进行多次投票（哪怕是不同的投票选举，避免不同的投票选举相互覆盖）
 * 2. 同一轮投票，只能投一次
*/
// TODO:(DONE) Manual Failover 过程
/**
 * 1. 通过 redis-cli，手动向一个 slave node 发送 CLUSTER FAILOVER
 * 2. slave 发送 CLUSTERMSG_TYPE_MFSTART 给 master，并且设置 mf_end，防止超时
 * 
 * clusterCron()
 * {
 *     3. master 收到 CLUSTERMSG_TYPE_MFSTART 之后，暂停 client，并且对比 offset（不然你的 offset 一直在变，玩啥呢）
 *        3.1 因为 master 已经暂停服务了，所以 offset 即使不太一样，也会慢慢同步到一致的（别超过 mf_end 就好了）
 *     4. slave 定期在 clusterHandleManualFailover 检查是否就绪，可以开始切换（master 跟 slave 的 repl_offset 同步了）
 *     5. slave 发起投票（CLUSTERMSG_FLAG0_FORCEACK 带上这个 flag），向其他 master 收集意见（一般都会成功的，除非这个 slave 状态不满足 author 的要求)
 *     6. 收集到足够的投票了，开始 failover
 * 
 *     检测 manual 是否操作超时
 *     可能发生 clusterHandleSlaveMigration()
 * }
*/
// TODO:(DONE) automatic failover  VS  manual automatic
/**
 * manual failover 的起点是：手动发送的 CLUSTER FAILOVER 命令，然后在 clusterCron 里面推进状态
 * automatic failover 的起点是：clusterHandleSlaveFailover() 检测到可以发起 automatic failover
 * 
 * 无论是 manual 还是 automatic，都是需要进行 vote 的。
*/
// TODO:(DONE) replica migration
/**
 * clusterHandleSlaveMigration() 函数
 */
// TODO:(DONE) Gossip 里面的 node flag VS CLUSTERMSG_TYPE_FAIL 消息，有什么区别？
/**
 * 未必可信 ---> 大概率可信
 * Gossip 里面的 node flag 是传播：某个 node 对于另一个 node 的看法，仅仅是自己 ping 不通，就可以认为他 PFAIL 了
 * 所以 Gossip 仅仅是传播片面的看法。需要所有 node 自己收集不同 node 的看法，然后进行统计判断，才能形成决策。
 * 
 *  CLUSTERMSG_TYPE_FAIL
 * 当 node 在 Gossip 收集了其他 node 的看法之后，自己做出统计，进而判断某个 node 是不是 FAIL。
 * 是的话，就会直接向 cluster 的其他 node 广播 CLUSTERMSG_TYPE_FAIL 消息。其他 node 服从这个决策判断就好了
 * 这样才能快速达成一致
 * 
 * 所以 Gossip 是个人偏见（未必可信），个别偏见不断积累，然后达到临界值（过半数），那不再是偏见，而是大概率的事实。
 * 这时候，做出了这个大概率事实判断的 node，只需要把判断的结果快速传播出去就好了（可信）
 * */
// TODO: FAIL 的 node 再次上来时，cluster 时怎么知晓的？这个 node 主动从 config 读，然后 connect 呢？还是其他 node 定期 re-connect ？
/** 
 * freeClusterLink() 然后 clusterCron() 会尝试重连
 * 是其他仍然 UP 的 node，不断尝试重连被 down 掉的 node
 */

// 阻塞同步 bind and listen；非阻塞异步完成 node 与 node 之间的 handshake


/* A global reference to myself is handy to make code more clear.
 * Myself always points to server.cluster->myself, that is, the clusterNode
 * that represents this node. */
// 为了方便起见，维持一个 myself 全局变量，让它总是指向 server.cluster->myself 。
clusterNode *myself = NULL;

clusterNode *createClusterNode(char *nodename, int flags);
int clusterAddNode(clusterNode *node);
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterSendPing(clusterLink *link, int type);
void clusterSendFail(char *nodename);
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request);
void clusterUpdateState(void);
int clusterNodeGetSlotBit(clusterNode *n, int slot);
sds clusterGenNodesDescription(int filter);
clusterNode *clusterLookupNode(char *name);
int clusterNodeAddSlave(clusterNode *master, clusterNode *slave);
int clusterAddSlot(clusterNode *n, int slot);
int clusterDelSlot(int slot);
int clusterDelNodeSlots(clusterNode *node);
int clusterNodeSetSlotBit(clusterNode *n, int slot);
void clusterSetMaster(clusterNode *n);
void clusterHandleSlaveFailover(void);
void clusterHandleSlaveMigration(int max_slaves);
int bitmapTestBit(unsigned char *bitmap, int pos);
void clusterDoBeforeSleep(int flags);
void clusterSendUpdate(clusterLink *link, clusterNode *node);
void resetManualFailover(void);
void clusterCloseAllSlots(void);
void clusterSetNodeAsMaster(clusterNode *n);
void clusterDelNode(clusterNode *delnode);

/* -----------------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------------- */

/* Return the greatest configEpoch found in the cluster. */
uint64_t clusterGetMaxEpoch(void) {
    uint64_t max = 0;
    dictIterator *di;
    dictEntry *de;

    // 选出节点中的最大纪元
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->configEpoch > max) max = node->configEpoch;
    }
    dictReleaseIterator(di);
    if (max < server.cluster->currentEpoch) max = server.cluster->currentEpoch;
    return max;
}

// 载入集群配置
/* Load the cluster config from 'filename'.
 *
 * If the file does not exist or is zero-length (this may happen because
 * when we lock the nodes.conf file, we create a zero-length one for the
 * sake of locking if it does not already exist), REDIS_ERR is returned.
 * If the configuration was loaded from the file, REDIS_OK is returned. */
int clusterLoadConfig(char *filename) {
    FILE *fp = fopen(filename,"r");
    struct stat sb;
    char *line;
    int maxline, j;

    if (fp == NULL) {
        if (errno == ENOENT) {
            return REDIS_ERR;
        } else {
            redisLog(REDIS_WARNING,
                "Loading the cluster node config from %s: %s",
                filename, strerror(errno));
            exit(1);
        }
    }

    /* Check if the file is zero-length: if so return REDIS_ERR to signal
     * we have to write the config. */
    if (fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        fclose(fp);
        return REDIS_ERR;
    }

    /* Parse the file. Note that single liens of the cluster config file can
     * be really long as they include all the hash slots of the node.
     * 集群配置文件中的行可能会非常长，
     * 因为它会在行里面记录所有哈希槽的节点。
     *
     * This means in the worst possible case, half of the Redis slots will be
     * present in a single line, possibly in importing or migrating state, so
     * together with the node ID of the sender/receiver.
     *
     * 在最坏情况下，一个行可能保存了半数的哈希槽数据，
     * 并且可能带有导入或导出状态，以及发送者和接受者的 ID 。
     *
     * To simplify we allocate 1024+REDIS_CLUSTER_SLOTS*128 bytes per line. 
     *
     * 为了简单起见，我们为每行分配 1024+REDIS_CLUSTER_SLOTS*128 字节的空间
     */
    maxline = 1024+REDIS_CLUSTER_SLOTS*128;
    line = zmalloc(maxline);
    while(fgets(line,maxline,fp) != NULL) {
        int argc;
        sds *argv;
        clusterNode *n, *master;
        char *p, *s;

        /* Skip blank lines, they can be created either by users manually
         * editing nodes.conf or by the config writing process if stopped
         * before the truncate() call. */
        if (line[0] == '\n') continue;

        /* Split the line into arguments for processing. */
        argv = sdssplitargs(line,&argc);
        if (argv == NULL) goto fmterr;

        /* Handle the special "vars" line. Don't pretend it is the last
         * line even if it actually is when generated by Redis. */
        if (strcasecmp(argv[0],"vars") == 0) {
            for (j = 1; j < argc; j += 2) {
                if (strcasecmp(argv[j],"currentEpoch") == 0) {
                    server.cluster->currentEpoch =
                            strtoull(argv[j+1],NULL,10);
                } else if (strcasecmp(argv[j],"lastVoteEpoch") == 0) {
                    server.cluster->lastVoteEpoch =
                            strtoull(argv[j+1],NULL,10);
                } else {
                    redisLog(REDIS_WARNING,
                        "Skipping unknown cluster config variable '%s'",
                        argv[j]);
                }
            }
            continue;
        }

        /* Create this node if it does not exist */
        // 检查节点是否已经存在
        n = clusterLookupNode(argv[0]);
        if (!n) {
            // 未存在则创建这个节点
            n = createClusterNode(argv[0],0);
            clusterAddNode(n);
        }
        /* Address and port */
        // 设置节点的 ip 和 port
        if ((p = strchr(argv[1],':')) == NULL) goto fmterr;
        *p = '\0';
        memcpy(n->ip,argv[1],strlen(argv[1])+1);
        n->port = atoi(p+1);

        /* Parse flags */
        // 分析节点的 flag
        p = s = argv[2];
        while(p) {
            p = strchr(s,',');
            if (p) *p = '\0';
            // 这是节点本身
            if (!strcasecmp(s,"myself")) {
                redisAssert(server.cluster->myself == NULL);
                myself = server.cluster->myself = n;
                n->flags |= REDIS_NODE_MYSELF;
            // 这是一个主节点
            } else if (!strcasecmp(s,"master")) {
                n->flags |= REDIS_NODE_MASTER;
            // 这是一个从节点
            } else if (!strcasecmp(s,"slave")) {
                n->flags |= REDIS_NODE_SLAVE;
            // 这是一个疑似下线节点
            } else if (!strcasecmp(s,"fail?")) {
                n->flags |= REDIS_NODE_PFAIL;
            // 这是一个已下线节点
            } else if (!strcasecmp(s,"fail")) {
                n->flags |= REDIS_NODE_FAIL;
                n->fail_time = mstime();
            // 等待向节点发送 PING
            } else if (!strcasecmp(s,"handshake")) {
                n->flags |= REDIS_NODE_HANDSHAKE;
            // 尚未获得这个节点的地址
            } else if (!strcasecmp(s,"noaddr")) {
                n->flags |= REDIS_NODE_NOADDR;
            // 无 flag
            } else if (!strcasecmp(s,"noflags")) {
                /* nothing to do */
            } else {
                redisPanic("Unknown flag in redis cluster config file");
            }
            if (p) s = p+1;
        }

        /* Get master if any. Set the master and populate master's
         * slave list. */
        // 如果有主节点的话，那么设置主节点
        if (argv[3][0] != '-') {
            master = clusterLookupNode(argv[3]);
            // 如果主节点不存在，那么添加它
            if (!master) {
                master = createClusterNode(argv[3],0);
                clusterAddNode(master);
            }
            // 设置主节点
            n->slaveof = master;
            // 将节点 n 加入到主节点 master 的从节点名单中
            clusterNodeAddSlave(master,n);
        }

        /* Set ping sent / pong received timestamps */
        // 设置最近一次发送 PING 命令以及接收 PING 命令回复的时间戳
        if (atoi(argv[4])) n->ping_sent = mstime();
        if (atoi(argv[5])) n->pong_received = mstime();

        /* Set configEpoch for this node. */
        // 设置配置纪元
        n->configEpoch = strtoull(argv[6],NULL,10);

        /* Populate hash slots served by this instance. */
        // 取出节点服务的槽
        for (j = 8; j < argc; j++) {
            int start, stop;

            // 正在导入或导出槽
            if (argv[j][0] == '[') {
                /* Here we handle migrating / importing slots */
                int slot;
                char direction;
                clusterNode *cn;

                p = strchr(argv[j],'-');
                redisAssert(p != NULL);
                *p = '\0';
                // 导入 or 导出？
                direction = p[1]; /* Either '>' or '<' */
                // 槽
                slot = atoi(argv[j]+1);
                p += 3;
                // 目标节点
                cn = clusterLookupNode(p);
                // 如果目标不存在，那么创建
                if (!cn) {
                    cn = createClusterNode(p,0);
                    clusterAddNode(cn);
                }
                // 根据方向，设定本节点要导入或者导出的槽的目标
                if (direction == '>') {
                    server.cluster->migrating_slots_to[slot] = cn;
                } else {
                    server.cluster->importing_slots_from[slot] = cn;
                }
                continue;

            // 没有导入或导出，这是一个区间范围的槽
            // 比如 0 - 10086
            } else if ((p = strchr(argv[j],'-')) != NULL) {
                *p = '\0';
                start = atoi(argv[j]);
                stop = atoi(p+1);

            // 没有导入或导出，这是单一个槽
            // 比如 10086
            } else {
                start = stop = atoi(argv[j]);
            }

            // 将槽载入节点
            while(start <= stop) clusterAddSlot(n, start++);
        }

        sdsfreesplitres(argv,argc);
    }
    zfree(line);
    fclose(fp);

    /* Config sanity check */
    redisAssert(server.cluster->myself != NULL);
    redisLog(REDIS_NOTICE,"Node configuration loaded, I'm %.40s", myself->name);

    /* Something that should never happen: currentEpoch smaller than
     * the max epoch found in the nodes configuration. However we handle this
     * as some form of protection against manual editing of critical files. */
    if (clusterGetMaxEpoch() > server.cluster->currentEpoch) {
        server.cluster->currentEpoch = clusterGetMaxEpoch();
    }
    return REDIS_OK;

fmterr:
    redisLog(REDIS_WARNING,
        "Unrecoverable error: corrupted cluster config file.");
    fclose(fp);
    exit(1);
}

/* Cluster node configuration is exactly the same as CLUSTER NODES output.
 *
 * This function writes the node config and returns 0, on error -1
 * is returned.
 *
 * Note: we need to write the file in an atomic way from the point of view
 * of the POSIX filesystem semantics, so that if the server is stopped
 * or crashes during the write, we'll end with either the old file or the
 * new one. Since we have the full payload to write available we can use
 * a single write to write the whole file. If the pre-existing file was
 * bigger we pad our payload with newlines that are anyway ignored and truncate
 * the file afterward. */
// 写入 nodes.conf 文件
int clusterSaveConfig(int do_fsync) {
    sds ci;
    size_t content_size;
    struct stat sb;
    int fd;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_SAVE_CONFIG;

    /* Get the nodes description and concatenate our "vars" directive to
     * save currentEpoch and lastVoteEpoch. */
    ci = clusterGenNodesDescription(REDIS_NODE_HANDSHAKE);
    ci = sdscatprintf(ci,"vars currentEpoch %llu lastVoteEpoch %llu\n",
        (unsigned long long) server.cluster->currentEpoch,
        (unsigned long long) server.cluster->lastVoteEpoch);
    content_size = sdslen(ci);

    if ((fd = open(server.cluster_configfile,O_WRONLY|O_CREAT,0644))
        == -1) goto err;

    /* Pad the new payload if the existing file length is greater. */
    if (fstat(fd,&sb) != -1) {
        if (sb.st_size > content_size) {
            ci = sdsgrowzero(ci,sb.st_size);
            memset(ci+content_size,'\n',sb.st_size-content_size);
        }
    }
    if (write(fd,ci,sdslen(ci)) != (ssize_t)sdslen(ci)) goto err;
    if (do_fsync) {
        server.cluster->todo_before_sleep &= ~CLUSTER_TODO_FSYNC_CONFIG;
        fsync(fd);
    }

    /* Truncate the file if needed to remove the final \n padding that
     * is just garbage. */
    if (content_size != sdslen(ci) && ftruncate(fd,content_size) == -1) {
        /* ftruncate() failing is not a critical error. */
    }
    close(fd);
    sdsfree(ci);
    return 0;

err:
    if (fd != -1) close(fd);
    sdsfree(ci);
    return -1;
}

// 尝试写入 nodes.conf 文件，失败则退出
void clusterSaveConfigOrDie(int do_fsync) {
    if (clusterSaveConfig(do_fsync) == -1) {
        redisLog(REDIS_WARNING,"Fatal: can't update cluster config file.");
        exit(1);
    }
}

/* Lock the cluster config using flock(), and leaks the file descritor used to
 * acquire the lock so that the file will be locked forever.
 *
 * This works because we always update nodes.conf with a new version
 * in-place, reopening the file, and writing to it in place (later adjusting
 * the length with ftruncate()).
 *
 * On success REDIS_OK is returned, otherwise an error is logged and
 * the function returns REDIS_ERR to signal a lock was not acquired. */
int clusterLockConfig(char *filename) {
    /* To lock it, we need to open the file in a way it is created if
     * it does not exist, otherwise there is a race condition with other
     * processes. */
    int fd = open(filename,O_WRONLY|O_CREAT,0644);
    if (fd == -1) {
        redisLog(REDIS_WARNING,
            "Can't open %s in order to acquire a lock: %s",
            filename, strerror(errno));
        return REDIS_ERR;
    }

    if (flock(fd,LOCK_EX|LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            redisLog(REDIS_WARNING,
                 "Sorry, the cluster configuration file %s is already used "
                 "by a different Redis Cluster node. Please make sure that "
                 "different nodes use different cluster configuration "
                 "files.", filename);
        } else {
            redisLog(REDIS_WARNING,
                "Impossible to lock %s: %s", filename, strerror(errno));
        }
        close(fd);
        return REDIS_ERR;
    }
    /* Lock acquired: leak the 'fd' by not closing it, so that we'll retain the
     * lock to the file as long as the process exists. */
    return REDIS_OK;
}

// 初始化集群
void clusterInit(void) {
    int saveconf = 0;

    // 初始化配置
    server.cluster = zmalloc(sizeof(clusterState));
    server.cluster->myself = NULL;
    server.cluster->currentEpoch = 0;
    server.cluster->state = REDIS_CLUSTER_FAIL;
    server.cluster->size = 1;
    server.cluster->todo_before_sleep = 0;
    server.cluster->nodes = dictCreate(&clusterNodesDictType,NULL);
    server.cluster->nodes_black_list =
        dictCreate(&clusterNodesBlackListDictType,NULL);
    server.cluster->failover_auth_time = 0;
    server.cluster->failover_auth_count = 0;
    server.cluster->failover_auth_rank = 0;
    server.cluster->failover_auth_epoch = 0;
    server.cluster->lastVoteEpoch = 0;
    server.cluster->stats_bus_messages_sent = 0;
    server.cluster->stats_bus_messages_received = 0;
    memset(server.cluster->slots,0, sizeof(server.cluster->slots));
    clusterCloseAllSlots();

    /* Lock the cluster config file to make sure every node uses
     * its own nodes.conf. */
    if (clusterLockConfig(server.cluster_configfile) == REDIS_ERR)
        exit(1);

    /* Load or create a new nodes configuration. */
    if (clusterLoadConfig(server.cluster_configfile) == REDIS_ERR) {
        /* No configuration found. We will just use the random name provided
         * by the createClusterNode() function. */
        myself = server.cluster->myself =
            createClusterNode(NULL,REDIS_NODE_MYSELF|REDIS_NODE_MASTER);
        redisLog(REDIS_NOTICE,"No cluster configuration found, I'm %.40s",
            myself->name);
        clusterAddNode(myself);
        saveconf = 1;
    }

    // 保存 nodes.conf 文件
    if (saveconf) clusterSaveConfigOrDie(1);

    /* We need a listening TCP port for our cluster messaging needs. */
    // 监听 TCP 端口
    server.cfd_count = 0;

    /* Port sanity check II
     * The other handshake port check is triggered too late to stop
     * us from trying to use a too-high cluster port number. */
    if (server.port > (65535-REDIS_CLUSTER_PORT_INCR)) {
        redisLog(REDIS_WARNING, "Redis port number too high. "
                   "Cluster communication port is 10,000 port "
                   "numbers higher than your Redis port. "
                   "Your Redis port number must be "
                   "lower than 55535.");
        exit(1);
    }

    // 同步 bind
    if (listenToPort(server.port+REDIS_CLUSTER_PORT_INCR,
        server.cfd,&server.cfd_count) == REDIS_ERR)
    {
        exit(1);
    } else {
        int j;

        for (j = 0; j < server.cfd_count; j++) {
            // 关联监听事件处理器
            if (aeCreateFileEvent(server.el, server.cfd[j], AE_READABLE,
                clusterAcceptHandler, NULL) == AE_ERR)
                    redisPanic("Unrecoverable error creating Redis Cluster "
                                "file event.");
        }
    }

    /* The slots -> keys map is a sorted set. Init it. */
    // slots -> keys 映射是一个有序集合
    server.cluster->slots_to_keys = zslCreate();
    resetManualFailover();
}

/* Reset a node performing a soft or hard reset:
 *
 * 1) All other nodes are forget.
 * 2) All the assigned / open slots are released.
 * 3) If the node is a slave, it turns into a master.
 * 5) Only for hard reset: a new Node ID is generated.
 * 6) Only for hard reset: currentEpoch and configEpoch are set to 0.
 * 7) The new configuration is saved and the cluster state updated.  */
void clusterReset(int hard) {
    dictIterator *di;
    dictEntry *de;
    int j;

    /* Turn into master. */
    if (nodeIsSlave(myself)) {
        clusterSetNodeAsMaster(myself);
        replicationUnsetMaster();
    }

    /* Close slots, reset manual failover state. */
    clusterCloseAllSlots();
    resetManualFailover();

    /* Unassign all the slots. */
    for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) clusterDelSlot(j);

    /* Forget all the nodes, but myself. */
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node == myself) continue;
        clusterDelNode(node);
    }
    dictReleaseIterator(di);

    /* Hard reset only: set epochs to 0, change node ID. */
    if (hard) {
        sds oldname;

        server.cluster->currentEpoch = 0;
        server.cluster->lastVoteEpoch = 0;
        myself->configEpoch = 0;

        /* To change the Node ID we need to remove the old name from the
         * nodes table, change the ID, and re-add back with new name. */
        oldname = sdsnewlen(myself->name, REDIS_CLUSTER_NAMELEN);
        dictDelete(server.cluster->nodes,oldname);
        sdsfree(oldname);
        getRandomHexChars(myself->name, REDIS_CLUSTER_NAMELEN);
        clusterAddNode(myself);
    }

    /* Make sure to persist the new config and update the state. */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE|
                         CLUSTER_TODO_FSYNC_CONFIG);
}

/* -----------------------------------------------------------------------------
 * CLUSTER communication link
 * -------------------------------------------------------------------------- */

// 创建节点连接，分配相应 buf 缓冲区
clusterLink *createClusterLink(clusterNode *node) {
    clusterLink *link = zmalloc(sizeof(*link));
    link->ctime = mstime();
    link->sndbuf = sdsempty();
    link->rcvbuf = sdsempty();
    link->node = node;
    link->fd = -1;
    return link;
}

/* Free a cluster link, but does not free the associated node of course.
 * This function will just make sure that the original node associated
 * with this link will have the 'link' field set to NULL. */
// 将给定的连接清空
// 并将包含这个连接的节点的 link 属性设为 NULL
void freeClusterLink(clusterLink *link) {

    // 删除事件处理器
    if (link->fd != -1) {
        aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
        aeDeleteFileEvent(server.el, link->fd, AE_READABLE);
    }

    // 释放输入缓冲区和输出缓冲区
    sdsfree(link->sndbuf);
    sdsfree(link->rcvbuf);

    // 将节点的 link 属性设为 NULL
    if (link->node)
        link->node->link = NULL;

    // 关闭连接
    close(link->fd);

    // 释放连接结构
    zfree(link);
}

// 监听事件处理器, 这个函数是被连接的 node 调用的
// 一次性最多增减 1000 个 node，避免长时间阻塞
#define MAX_CLUSTER_ACCEPTS_PER_CALL 1000
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = MAX_CLUSTER_ACCEPTS_PER_CALL;
    char cip[REDIS_IP_STR_LEN];
    clusterLink *link;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    /* If the server is starting up, don't accept cluster connections:
     * UPDATE messages may interact with the database content. */
    if (server.masterhost == NULL && server.loading) return;

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                redisLog(REDIS_VERBOSE,
                    "Accepting cluster node: %s", server.neterr);
            return;
        }
        anetNonBlock(NULL,cfd); // accept 返回来的 cfd 是新的 fd，所以是要重新设置 non-block 的
        anetEnableTcpNoDelay(NULL,cfd);

        /* Create a link object we use to handle the connection.
         * It gets passed to the readable handler(clusterReadHandler) when data is available for cfd.
         * Initiallly the link->node pointer is set to NULL as we don't know
         * which node is, but the right node is references once we know the
         * node identity.
         * 这时候，我们仅仅是知道了：有一个 socket 向本 node 的 cluster bus 监听端口发起了连接
         * 但是，对面是什么？是不是 cluster node？node 里面的信息是什么？
         * 统统不知道，因此，现在既没有能力为这个 socket-link 创建 node，更不可能知晓对面 node 的信息
         * 所以 link->node 也只好暂且搁置
         * 实话说，到后面，这个 link->node 一直都是 NULL 的，因为 accept 的必然是被动连接的 socket。
         * 至于为什么要这么做，不知道
         * */
        link = createClusterLink(NULL);
        link->fd = cfd;
        /* Use non-blocking I/O for cluster messages. */
        redisLog(REDIS_VERBOSE,"Accepted cluster node %s:%d; [link:%p]", cip, cport, (void*)link);
        // connect 完成，等待对象发送数据，以便知晓对端的信息，判断对方是不是可以被接受的 node
        aeCreateFileEvent(server.el,cfd,AE_READABLE,clusterReadHandler,link);
    }
}

/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
// 计算给定键应该被分配到那个槽
unsigned int keyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. */
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing betweeen {} ? Hash the whole key. */
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}

/* -----------------------------------------------------------------------------
 * CLUSTER node API
 * -------------------------------------------------------------------------- */

/* Create a new cluster node, with the specified flags.
 *
 * 创建一个带有指定 flag 的集群节点。
 *
 * If "nodename" is NULL this is considered a first handshake and a random
 * node name is assigned to this node (it will be fixed later when we'll
 * receive the first pong).
 *
 * 如果 nodename 参数为 NULL ，那么表示我们尚未向节点发送 PING ，
 * 集群会为节点设置一个随机的命令，
 * 这个命令在之后接收到节点的 PONG 回复之后就会被更新。
 *
 * The node is created and returned to the user, but it is not automatically
 * added to the nodes hash table. 
 *
 * 函数会返回被创建的节点，但不会自动将它添加到当前节点的节点哈希表中
 * （nodes hash table）。
 */
clusterNode *createClusterNode(char *nodename, int flags) {
    clusterNode *node = zmalloc(sizeof(*node));

    // 设置名字
    if (nodename)
        memcpy(node->name, nodename, REDIS_CLUSTER_NAMELEN);
    else
        getRandomHexChars(node->name, REDIS_CLUSTER_NAMELEN);

    // 初始化属性
    node->ctime = mstime();
    node->configEpoch = 0;
    node->flags = flags;
    memset(node->slots,0,sizeof(node->slots));
    node->numslots = 0;
    node->numslaves = 0;
    node->slaves = NULL;
    node->slaveof = NULL;
    node->ping_sent = node->pong_received = 0;
    node->fail_time = 0;
    node->link = NULL;
    memset(node->ip,0,sizeof(node->ip));
    node->port = 0;
    node->fail_reports = listCreate();
    node->voted_time = 0;
    node->repl_offset_time = 0;
    node->repl_offset = 0;
    listSetFreeMethod(node->fail_reports,zfree);

    return node;
}

/* This function is called every time we get a failure report from a node.
 *
 * 这个函数会在当前节点接到某个节点的下线报告时调用。
 *
 * The side effect is to populate the fail_reports list (or to update
 * the timestamp of an existing report).
 *
 * 函数的作用就是将下线节点的下线报告添加到 fail_reports 列表，
 * 如果这个下线节点的下线报告已经存在，
 * 那么更新该报告的时间戳。
 *
 * 'failing' is the node that is in failure state according to the
 * 'sender' node.
 *
 * failing 参数指向下线节点，而 sender 参数则指向报告 failing 已下线的节点。
 *
 * The function returns 0 if it just updates a timestamp of an existing
 * failure report from the same sender. 1 is returned if a new failure
 * report is created. 
 *
 * 函数返回 0 表示对已存在的报告进行了更新，
 * 返回 1 则表示创建了一条新的下线报告。
 */
// clusterNodeAddFailureReport(be_gossip_node, gossip_sender)
// 添加 fail_report 是为了让 FAIL 的标记，拥有统计数据
int clusterNodeAddFailureReport(clusterNode *failing, clusterNode *sender) {

    // 指向保存下线报告的链表
    list *l = failing->fail_reports;

    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* If a failure report from the same sender already exists, just update
     * the timestamp. */
    // 查找 sender 节点的下线报告是否已经存在
    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        // 如果存在的话，那么只更新该报告的时间戳
        if (fr->node == sender) {
            fr->time = mstime();
            return 0;
        }
    }

    /* Otherwise create a new report. */
    // 否则的话，就创建一个新的报告
    fr = zmalloc(sizeof(*fr));
    fr->node = sender;
    fr->time = mstime();

    // 将报告添加到列表
    listAddNodeTail(l,fr);

    return 1;
}

/* Remove failure reports that are too old, where too old means reasonably
 * older than the global node timeout. Note that anyway for a node to be
 * flagged as FAIL we need to have a local PFAIL state that is at least
 * older than the global node timeout, so we don't just trust the number
 * of failure reports from other nodes. 
 *
 * 移除对 node 节点的过期的下线报告，
 * 多长时间为过期是根据 node timeout 选项的值来决定的。
 *
 * 注意，
 * 要将一个节点标记为 FAIL 状态，
 * 当前节点将 node 标记为 PFAIL 状态的时间至少应该超过 node timeout ，
 * 所以报告 node 已下线的节点数量并不是当前节点将 node 标记为 FAIL 的唯一条件。
 */
void clusterNodeCleanupFailureReports(clusterNode *node) {

    // 指向该节点的所有下线报告
    list *l = node->fail_reports;

    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    // 下线报告的最大保质期（超过这个时间的报告会被删除）
    mstime_t maxtime = server.cluster_node_timeout *
                     REDIS_CLUSTER_FAIL_REPORT_VALIDITY_MULT;
    mstime_t now = mstime();

    // 遍历所有下线报告
    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        // 删除过期报告
        if (now - fr->time > maxtime) listDelNode(l,ln);
    }
}

/* Remove the failing report for 'node' if it was previously considered
 * failing by 'sender'. This function is called when a node informs us via
 * gossip that a node is OK from its point of view (no FAIL or PFAIL flags).
 *
 * 从 node 节点的下线报告中移除 sender 对 node 的下线报告。
 *
 * 这个函数在以下情况使用：当前节点认为 node 已下线（FAIL 或者 PFAIL），
 * 但 sender 却向当前节点发来报告，说它认为 node 节点没有下线，
 * 那么当前节点就要移除 sender 对 node 的下线报告 
 * —— 如果 sender 曾经报告过 node 下线的话。
 *
 * Note that this function is called relatively often as it gets called even
 * when there are no nodes failing, and is O(N), however when the cluster is
 * fine the failure reports list is empty so the function runs in constant
 * time.
 *
 * 即使在节点没有下线的情况下，这个函数也会被调用，并且调用的次数还比较频繁。
 * 在一般情况下，这个函数的复杂度为 O(N) ，
 * 不过在不存在下线报告的情况下，这个函数的复杂度仅为常数时间。
 *
 * The function returns 1 if the failure report was found and removed.
 * Otherwise 0 is returned. 
 *
 * 函数返回 1 表示下线报告已经被成功移除，
 * 0 表示 sender 没有发送过 node 的下线报告，删除失败。
 */
int clusterNodeDelFailureReport(clusterNode *node, clusterNode *sender) {
    list *l = node->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* Search for a failure report from this sender. */
    // 查找 sender 对 node 的下线报告
    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (fr->node == sender) break;
    }
    // sender 没有报告过 node 下线，直接返回
    if (!ln) return 0; /* No failure report from this sender. */

    /* Remove the failure report. */
    // 删除 sender 对 node 的下线报告
    listDelNode(l,ln);
    // 删除对 node 的下线报告中，过期的报告
    clusterNodeCleanupFailureReports(node);

    return 1;
}

/* Return the number of external nodes that believe 'node' is failing,
 * not including this node, that may have a PFAIL or FAIL state for this
 * node as well. 
 *
 * 计算不包括本节点在内的，
 * 将 node 标记为 PFAIL 或者 FAIL 的节点的数量。
 */
int clusterNodeFailureReportsCount(clusterNode *node) {

    // 移除过期的下线报告
    clusterNodeCleanupFailureReports(node);

    // 统计下线报告的数量
    return listLength(node->fail_reports);
}

// 移除主节点 master 的从节点 slave
int clusterNodeRemoveSlave(clusterNode *master, clusterNode *slave) {
    int j;

    // 在 slaves 数组中找到从节点 slave 所属的主节点，
    // 将主节点中的 slave 信息移除
    for (j = 0; j < master->numslaves; j++) {
        if (master->slaves[j] == slave) {
            memmove(master->slaves+j,master->slaves+(j+1),
                (master->numslaves-1)-j);   // 最后一个是横竖删不掉的，所以只能通过计数来确认使用到那个坑位
            master->numslaves--;    // 这个才是唯一准确的计数，直接看 master->slaves[master->numslaves] == NULL 是不准确的
            return REDIS_OK;
        }
    }
    return REDIS_ERR;
}

// 将 slave 加入到 master 的从节点名单中
// 在 master、slave node 上面都可以执行
int clusterNodeAddSlave(clusterNode *master, clusterNode *slave) {
    int j;

    /* If it's already a slave, don't add it again. */
    // 如果 slave 已经存在，那么不做操作（去重）
    for (j = 0; j < master->numslaves; j++)
        if (master->slaves[j] == slave) return REDIS_ERR;

    // 将 slave 添加到 slaves 数组里面
    master->slaves = zrealloc(master->slaves,
        sizeof(clusterNode*)*(master->numslaves+1));
    master->slaves[master->numslaves] = slave;
    master->numslaves++;

    return REDIS_OK;
}

// 重置给定节点的从节点名单
void clusterNodeResetSlaves(clusterNode *n) {
    zfree(n->slaves);
    n->numslaves = 0;
    n->slaves = NULL;
}

// 耗时不会很长的，因为一个 master 搞太多 slave 没啥意义
int clusterCountNonFailingSlaves(clusterNode *n) {
    int j, okslaves = 0;

    for (j = 0; j < n->numslaves; j++)
        if (!nodeFailed(n->slaves[j])) okslaves++;
    return okslaves;
}

// 释放节点
void freeClusterNode(clusterNode *n) {
    sds nodename;

    nodename = sdsnewlen(n->name, REDIS_CLUSTER_NAMELEN);

    // 从 nodes 表中删除节点
    redisAssert(dictDelete(server.cluster->nodes,nodename) == DICT_OK);
    sdsfree(nodename);

    // 移除从节点
    if (n->slaveof) clusterNodeRemoveSlave(n->slaveof, n);

    // 释放连接
    if (n->link) freeClusterLink(n->link);
    
    // 释放失败报告
    listRelease(n->fail_reports);

    // 释放节点结构
    zfree(n);
}

/* Add a node to the nodes hash table */
// 将给定 node 添加到节点表里面
int clusterAddNode(clusterNode *node) {
    int retval;
    // 将 node 添加到当前节点的 nodes 表中
    // 这样接下来当前节点就会创建连向 node 的节点
    retval = dictAdd(server.cluster->nodes,
            sdsnewlen(node->name,REDIS_CLUSTER_NAMELEN), node);
    return (retval == DICT_OK) ? REDIS_OK : REDIS_ERR;
}

/* Remove a node from the cluster:
 *
 * 从集群中移除一个节点：
 *
 * 1) Mark all the nodes handled by it as unassigned.
 *    将所有由该节点负责的槽全部设置为未分配
 * 2) Remove all the failure reports sent by this node.
 *    移除所有由这个节点发送的下线报告（failure report）
 * 3) Free the node, that will in turn remove it from the hash table
 *    and from the list of slaves of its master, if it is a slave node.
 *    释放这个节点，
 *    清除它在各个节点的 nodes 表中的数据，
 *    如果它是一个从节点的话，
 *    还要在它的主节点的 slaves 表中清除关于这个节点的数据。
 */
void clusterDelNode(clusterNode *delnode) {
    int j;
    dictIterator *di;
    dictEntry *de;

    /* 1) Mark slots as unassigned. */
    for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
        // 取消向该节点接收槽的计划
        if (server.cluster->importing_slots_from[j] == delnode)
            server.cluster->importing_slots_from[j] = NULL;
        // 取消向该节点移交槽的计划
        if (server.cluster->migrating_slots_to[j] == delnode)
            server.cluster->migrating_slots_to[j] = NULL;
        // 将所有由该节点负责的槽设置为未分配
        if (server.cluster->slots[j] == delnode)
            clusterDelSlot(j);
    }

    /* 2) Remove failure reports. */
    // 移除所有由该节点发送的下线报告
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node == delnode) continue;
        clusterNodeDelFailureReport(node,delnode);
    }
    dictReleaseIterator(di);

    /* 3) Remove this node from its master's slaves if needed. */
    // 将节点从它的主节点的从节点列表中移除
    if (nodeIsSlave(delnode) && delnode->slaveof)
        clusterNodeRemoveSlave(delnode->slaveof,delnode);

    /* 4) Free the node, unlinking it from the cluster. */
    // 释放节点
    freeClusterNode(delnode);
}

/* Node lookup by name */
// 根据名字，查找给定的节点
clusterNode *clusterLookupNode(char *name) {
    sds s = sdsnewlen(name, REDIS_CLUSTER_NAMELEN);
    dictEntry *de;

    de = dictFind(server.cluster->nodes,s);
    sdsfree(s);
    if (de == NULL) return NULL;
    return dictGetVal(de);
}

/* This is only used after the handshake. When we connect a given IP/PORT
 * as a result of CLUSTER MEET we don't have the node name yet, so we
 * pick a random one, and will fix it when we receive the PONG request using
 * this function. */
// 在第一次向节点发送 CLUSTER MEET 命令的时候
// 因为发送命令的节点还不知道目标节点的名字
// 所以它会给目标节点分配一个随机的名字
// 当目标节点向发送节点返回 PONG 回复时
// 发送节点就知道了目标节点的 IP 和 port
// 这时发送节点就可以通过调用这个函数
// 为目标节点改名
void clusterRenameNode(clusterNode *node, char *newname) {
    int retval;
    sds s = sdsnewlen(node->name, REDIS_CLUSTER_NAMELEN);

    redisLog(REDIS_DEBUG,"Renaming node %.40s into %.40s",
        node->name, newname);
    retval = dictDelete(server.cluster->nodes, s);
    sdsfree(s);
    redisAssert(retval == DICT_OK);
    memcpy(node->name, newname, REDIS_CLUSTER_NAMELEN);
    clusterAddNode(node);
}

/* -----------------------------------------------------------------------------
 * CLUSTER nodes blacklist
 *
 * 集群节点黑名单
 *
 * The nodes blacklist is just a way to ensure that a given node with a given
 * Node ID is not readded before some time elapsed (this time is specified
 * in seconds in REDIS_CLUSTER_BLACKLIST_TTL).
 *
 * 黑名单用于禁止一个给定的节点在 REDIS_CLUSTER_BLACKLIST_TTL 指定的时间内，
 * 被重新添加到集群中。
 *
 * This is useful when we want to remove a node from the cluster completely:
 * when CLUSTER FORGET is called, it also puts the node into the blacklist so
 * that even if we receive gossip messages from other nodes that still remember
 * about the node we want to remove, we don't re-add it before some time.
 *
 * 当我们需要从集群中彻底移除一个节点时，就需要用到黑名单：
 * 在执行 CLUSTER FORGET 命令时，节点会被添加进黑名单里面，
 * 这样即使我们从仍然记得被移除节点的其他节点那里收到关于被移除节点的消息，
 * 我们也不会重新将被移除节点添加至集群。
 *
 * Currently the REDIS_CLUSTER_BLACKLIST_TTL is set to 1 minute, this means
 * that redis-trib has 60 seconds to send CLUSTER FORGET messages to nodes
 * in the cluster without dealing with the problem of other nodes re-adding
 * back the node to nodes we already sent the FORGET command to.
 *
 * REDIS_CLUSTER_BLACKLIST_TTL 当前的值为 1 分钟，
 * 这意味着 redis-trib 有 60 秒的时间，可以向集群中的所有节点发送 CLUSTER FORGET
 * 命令，而不必担心有其他节点会将被 CLUSTER FORGET 移除的节点重新添加到集群里面。
 *
 * The data structure used is a hash table with an sds string representing
 * the node ID as key, and the time when it is ok to re-add the node as
 * value.
 *
 * 黑名单的底层实现是一个字典，
 * 字典的键为 SDS 表示的节点 id ，字典的值为可以重新添加节点的时间戳。
 * -------------------------------------------------------------------------- */

#define REDIS_CLUSTER_BLACKLIST_TTL 60      /* 1 minute. */


/* Before of the addNode() or Exists() operations we always remove expired
 * entries from the black list. This is an O(N) operation but it is not a
 * problem since add / exists operations are called very infrequently and
 * the hash table is supposed to contain very little elements at max.
 *
 * 在执行 addNode() 操作或者 Exists() 操作之前，
 * 我们总是会先执行这个函数，移除黑名单中的过期节点。
 *
 * 这个函数的复杂度为 O(N) ，不过它不会对效率产生影响，
 * 因为这个函数执行的次数并不频繁，并且字典的链表里面包含的节点数量也非常少。
 *
 * However without the cleanup during long uptimes and with some automated
 * node add/removal procedures, entries could accumulate. 
 *
 * 定期清理过期节点是为了防止字典中的节点堆积过多。
 */
void clusterBlacklistCleanup(void) {
    dictIterator *di;
    dictEntry *de;

    // 遍历黑名单中的所有节点
    di = dictGetSafeIterator(server.cluster->nodes_black_list);
    while((de = dictNext(di)) != NULL) {
        int64_t expire = dictGetUnsignedIntegerVal(de);

        // 删除过期节点
        if (expire < server.unixtime)
            dictDelete(server.cluster->nodes_black_list,dictGetKey(de));
    }
    dictReleaseIterator(di);
}

/* Cleanup the blacklist and add a new node ID to the black list. */
// 清除黑名单中的过期节点，然后将新的节点添加到黑名单中
void clusterBlacklistAddNode(clusterNode *node) {
    dictEntry *de;
    sds id = sdsnewlen(node->name,REDIS_CLUSTER_NAMELEN);

    // 先清理过期名单
    clusterBlacklistCleanup();

    // 添加节点
    if (dictAdd(server.cluster->nodes_black_list,id,NULL) == DICT_OK) {
        /* If the key was added, duplicate the sds string representation of
         * the key for the next lookup. We'll free it at the end. */
        id = sdsdup(id);
    }
    // 设置过期时间
    de = dictFind(server.cluster->nodes_black_list,id);
    dictSetUnsignedIntegerVal(de,time(NULL)+REDIS_CLUSTER_BLACKLIST_TTL);
    sdsfree(id);
}

/* Return non-zero if the specified node ID exists in the blacklist.
 * You don't need to pass an sds string here, any pointer to 40 bytes
 * will work. */
// 检查给定 id 所指定的节点是否存在于黑名单中。
// nodeid 参数不必是一个 SDS 值，只要一个 40 字节长的字符串即可
int clusterBlacklistExists(char *nodeid) {

    // 构建 SDS 表示的节点名
    sds id = sdsnewlen(nodeid,REDIS_CLUSTER_NAMELEN);
    int retval;

    // 清除过期黑名单
    clusterBlacklistCleanup();

    // 检查节点是否存在
    retval = dictFind(server.cluster->nodes_black_list,id) != NULL;
    sdsfree(id);

    return retval;
}

/* -----------------------------------------------------------------------------
 * CLUSTER messages exchange - PING/PONG and gossip
 * -------------------------------------------------------------------------- */

/* This function checks if a given node should be marked as FAIL.
 * It happens if the following conditions are met:
 *
 * 此函数用于判断是否需要将 node 标记为 FAIL 。
 *
 * 将 node 标记为 FAIL 需要满足以下两个条件：
 *
 * 1) We received enough failure reports from **other master nodes** via gossip.
 *    Enough means that the majority of the masters signaled the node is
 *    down recently.
 *    有半数以上的主节点将 node 标记为 PFAIL 状态。
 * 2) We believe this node is in PFAIL state.
 *    当前节点也将 node 标记为 PFAIL 状态。
 *
 * If a failure is detected we also inform the whole cluster about this
 * event trying to force every other node to set the FAIL flag for the node.
 *
 * 如果确认 node 已经进入了 FAIL 状态，
 * 那么节点还会向其他节点发送 FAIL 消息，让其他节点也将 node 标记为 FAIL 。
 *
 * Note that the form of agreement used here is weak, as we collect the majority
 * of masters state during some time, and even if we force agreement by
 * propagating the FAIL message, because of partitions we may not reach every
 * node. However:
 *
 * 注意，集群判断一个 node 进入 FAIL 所需的条件是弱（weak）的，
 * 因为节点们对 node 的状态报告并不是实时的，而是有一段时间间隔
 * （这段时间内 node 的状态可能已经发生了改变），
 * 并且尽管当前节点会向其他节点发送 FAIL 消息，
 * 但因为网络分裂（network partition）的问题，
 * 有一部分节点可能还是会不知道将 node 标记为 FAIL 。
 *
 * 不过：
 *
 * 1) Either we reach the majority and eventually the FAIL state will propagate
 *    to all the cluster.
 *    只要我们成功将 node 标记为 FAIL ，
 *    那么这个 FAIL 状态最终（eventually）总会传播至整个集群的所有节点。
 * 2) Or there is no majority so no slave promotion will be authorized and the
 *    FAIL flag will be cleared after some time.
 *    又或者，因为没有半数的节点支持，当前节点不能将 node 标记为 FAIL ，
 *    所以对 FAIL 节点的故障转移将无法进行， FAIL 标识可能会在之后被移除。
 *    
 */
// node 是 Gossip 中，sender node 对于某个 node 的看法
void markNodeAsFailingIfNeeded(clusterNode *node) {
    int failures;

    // 标记为 FAIL 所需的节点数量，需要超过集群节点数量的一半
    int needed_quorum = (server.cluster->size / 2) + 1;

    // 首先要我们自己觉得这个 node 是 REDIS_NODE_PFAIL 的（timeout）
    if (!nodeTimedOut(node)) return; /* We can reach it. */
    if (nodeFailed(node)) return; /* Already FAILing. 不需要往下走了 */

    // 统计将 node 标记为 PFAIL 或者 FAIL 的节点数量（不包括当前节点）
    failures = clusterNodeFailureReportsCount(node);

    /* Also count myself as a voter if I'm a master. */
    // 如果当前节点是主节点，那么将当前节点也算在 failures 之内
    if (nodeIsMaster(myself)) failures++;
    // 报告下线节点的数量不足节点总数的一半，不能将节点判断为 FAIL ，返回
    if (failures < needed_quorum) return; /* No weak agreement from masters. */

    redisLog(REDIS_WARNING,
        "Marking node %.40s as failing (quorum reached).", node->name);

    /* Mark the node as failing. */
    // 将 node 标记为 FAIL
    node->flags &= ~REDIS_NODE_PFAIL;
    node->flags |= REDIS_NODE_FAIL;
    node->fail_time = mstime();

    /* Broadcast the failing node name to everybody, forcing all the other
     * reachable nodes to flag the node as FAIL. */
    // 如果当前节点是主节点的话，那么向其他节点发送报告 node 的 FAIL 信息
    // 让其他节点也将 node 标记为 FAIL
    // slave 默默的改变自己的看法就好了，不可以通知任何人（权限上，slave 不应该有 DOWN 别人的权立）
    // 本质上 cluster 并不算很好的网络、redis 状态检测方案，因为只有在所有 client 的角度看 redis-server 才是最完美的
    if (nodeIsMaster(myself)) clusterSendFail(node->name); 
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
}

/* This function is called only if a node is marked as FAIL, but we are able
 * to reach it again. It checks if there are the conditions to undo the FAIL
 * state. 
 *
 * 这个函数在当前节点接收到一个被标记为 FAIL 的节点那里收到消息时使用，
 * 它可以检查是否应该将节点的 FAIL 状态移除。
 */
void clearNodeFailureIfNeeded(clusterNode *node) {
    mstime_t now = mstime();

    redisAssert(nodeFailed(node));

    /* For slaves we always clear the FAIL flag if we can contact the
     * node again. */
    // 如果 FAIL 的是从节点，那么当前节点会直接移除该节点的 FAIL
    if (nodeIsSlave(node) || node->numslots == 0) {
        redisLog(REDIS_NOTICE,
            "Clear FAIL state for node %.40s: %s is reachable again.",
                node->name,
                nodeIsSlave(node) ? "slave" : "master without slots");
        // 移除
        node->flags &= ~REDIS_NODE_FAIL;

        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }

    /* If it is a master and...
     *
     * 如果 FAIL 的是一个主节点，并且：
     *
     * 1) The FAIL state is old enough.
     *    节点被标记为 FAIL 状态已经有一段时间了
     *
     * 2) It is yet serving slots from our point of view (not failed over).
     *    从当前节点的视角来看，这个节点还有负责处理的槽
     *
     * Apparently no one is going to fix these slots, clear the FAIL flag. 
     *
     * 那么说明 FAIL 节点仍然有槽没有迁移完，那么当前节点移除该节点的 FAIL 标识。
     */
    if (nodeIsMaster(node) && node->numslots > 0 &&
        (now - node->fail_time) >
        (server.cluster_node_timeout * REDIS_CLUSTER_FAIL_UNDO_TIME_MULT))
    {
        redisLog(REDIS_NOTICE,
            "Clear FAIL state for node %.40s: is reachable again and nobody is serving its slots after some time.",
                node->name);

        // 撤销 FAIL 状态
        node->flags &= ~REDIS_NODE_FAIL;

        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }
}

/* Return true if we already have a node in HANDSHAKE state matching the
 * specified ip address and port number. This function is used in order to
 * avoid adding a new handshake node for the same address multiple times. 
 *
 * 如果当前节点已经向 ip 和 port 所指定的节点进行了握手，
 * 那么返回 1 。
 *
 * 这个函数用于防止对同一个节点进行多次握手。
 */
int clusterHandshakeInProgress(char *ip, int port) {
    dictIterator *di;
    dictEntry *de;

    // 遍历所有已知节点
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        // 跳过非握手状态的节点，之后剩下的都是正在握手的节点
        if (!nodeInHandshake(node)) continue;

        // 给定 ip 和 port 的节点正在进行握手
        if (!strcasecmp(node->ip,ip) && node->port == port) break;
    }
    dictReleaseIterator(di);

    // 检查节点是否正在握手
    return de != NULL;
}

/* Start an handshake with the specified address if there is not one
 * already in progress. Returns non-zero if the handshake was actually
 * started. On error zero is returned and errno is set to one of the
 * following values:
 *
 * 如果还没有与指定的地址进行过握手，那么进行握手。
 * 返回 1 表示握手已经开始，
 * 返回 0 并将 errno 设置为以下值来表示意外情况：
 *
 * EAGAIN - There is already an handshake in progress for this address.
 *          已经有握手在进行中了。
 * EINVAL - IP or port are not valid. 
 *          ip 或者 port 参数不合法。
 */
int clusterStartHandshake(char *ip, int port) {
    clusterNode *n;
    char norm_ip[REDIS_IP_STR_LEN];
    struct sockaddr_storage sa;

    /* IP sanity check */
    // ip 合法性检查
    if (inet_pton(AF_INET,ip,
            &(((struct sockaddr_in *)&sa)->sin_addr)))
    {
        sa.ss_family = AF_INET;
    } else if (inet_pton(AF_INET6,ip,
            &(((struct sockaddr_in6 *)&sa)->sin6_addr)))
    {
        sa.ss_family = AF_INET6;
    } else {
        errno = EINVAL;
        return 0;
    }

    /* Port sanity check */
    // port 合法性检查
    if (port <= 0 || port > (65535-REDIS_CLUSTER_PORT_INCR)) {
        errno = EINVAL;
        return 0;
    }

    /* Set norm_ip as the normalized string representation of the node
     * IP address. */
    if (sa.ss_family == AF_INET)
        inet_ntop(AF_INET,
            (void*)&(((struct sockaddr_in *)&sa)->sin_addr),
            norm_ip,REDIS_IP_STR_LEN);
    else
        inet_ntop(AF_INET6,
            (void*)&(((struct sockaddr_in6 *)&sa)->sin6_addr),
            norm_ip,REDIS_IP_STR_LEN);

    // 检查节点是否已经发送握手请求，如果是的话，那么直接返回，防止出现重复握手
    if (clusterHandshakeInProgress(norm_ip,port)) {
        errno = EAGAIN;
        return 0;
    }

    /* Add the node with a random address (NULL as first argument to
     * createClusterNode()). Everything will be fixed during the
     * handskake. */
    // 对给定地址的节点设置一个随机名字
    // 当 HANDSHAKE 完成时，当前节点会取得给定地址节点的真正名字
    // 到时会用真名替换随机名
    n = createClusterNode(NULL,REDIS_NODE_HANDSHAKE|REDIS_NODE_MEET);
    memcpy(n->ip,norm_ip,sizeof(n->ip));
    n->port = port;

    // 把新创建的 node，出于等待 connect + handshake 的 node 放入 server.cluster->nodes 里面
    // 晚点在 clusterCron() 里面才会正式开始剩下的工作（类似于 sentinel 的 cron 异步 job）
    // 针对同一个 ip:port 进行重复添加，是不会被放进这个 dict 里面的
    // TODO: 要是重复操作一个 cluster meet ip port，会发生三次握手，但是后面会 close 掉，看看这个过程在哪里
    clusterAddNode(n);

    return 1;
}

/* Process the gossip section of PING or PONG packets.
 *
 * 解释 MEET 、 PING 或 PONG 消息中和 gossip 协议有关的信息。
 *
 * Note that this function assumes that the packet is already sanity-checked
 * by the caller, not in the content of the gossip section, but in the
 * length. 
 *
 * 注意，这个函数假设调用者已经根据消息的长度，对消息进行过合法性检查。
 */
void clusterProcessGossipSection(clusterMsg *hdr, clusterLink *link) {

    // 记录这条消息中包含了多少个节点的信息
    uint16_t count = ntohs(hdr->count);

    // 指向第一个节点的信息
    clusterMsgDataGossip *g = (clusterMsgDataGossip*) hdr->data.ping.gossip;

    // 取出发送者(link 里面有的话，就省的再找了。在没有握手完成之前，link->node 都很有可能是 NULL 的)
    // link->node == NULL; 意味着这是一个被动创立的连接，会从这个 link 收到 PING\FAIL 等等；发送 PONG
    clusterNode *sender = link->node ? link->node : clusterLookupNode(hdr->sender);

    // 遍历所有节点的信息
    while(count--) {
        sds ci = sdsempty();

        // 分析节点的 flag
        uint16_t flags = ntohs(g->flags);

        // 信息节点
        clusterNode *node;

        // 取出节点的 flag
        if (flags == 0) ci = sdscat(ci,"noflags,");
        if (flags & REDIS_NODE_MYSELF) ci = sdscat(ci,"myself,");
        if (flags & REDIS_NODE_MASTER) ci = sdscat(ci,"master,");
        if (flags & REDIS_NODE_SLAVE) ci = sdscat(ci,"slave,");
        if (flags & REDIS_NODE_PFAIL) ci = sdscat(ci,"fail?,");
        if (flags & REDIS_NODE_FAIL) ci = sdscat(ci,"fail,");
        if (flags & REDIS_NODE_HANDSHAKE) ci = sdscat(ci,"handshake,");
        if (flags & REDIS_NODE_NOADDR) ci = sdscat(ci,"noaddr,");
        if (ci[sdslen(ci)-1] == ',') ci[sdslen(ci)-1] = ' ';

        redisLog(REDIS_DEBUG,"GOSSIP %.40s %s:%d %s",
            g->nodename,
            g->ip,
            ntohs(g->port),
            ci);
        sdsfree(ci);

        /* Update our state accordingly to the gossip sections */
        // 使用消息中的信息对节点进行更新
        node = clusterLookupNode(g->nodename);
        // 当前 node 已经认识 Gossip 里面说的这个 node
        if (node) {
            /* We already know this node.
               Handle failure reports, only when the sender is a master. */
            // 如果 sender 是一个主节点，那么我们需要处理下线报告
            // TODO:(DONE) 为什么 sender 必须是一个 master ？
            // 在 markNodeAsFailingIfNeeded() 里面：needed_quorum = (server.cluster->size / 2) + 1
            if (sender && nodeIsMaster(sender) && node != myself) {
                // 节点处于 FAIL 或者 PFAIL 状态
                if (flags & (REDIS_NODE_FAIL|REDIS_NODE_PFAIL)) {

                    // 添加 sender 对 node 的下线报告
                    if (clusterNodeAddFailureReport(node,sender)) {
                        redisLog(REDIS_WARNING,
                            "Node %.40s reported node %.40s as not reachable.",
                            sender->name, node->name);
                    }

                    // 尝试将 node 标记为 FAIL
                    markNodeAsFailingIfNeeded(node);

                // 节点处于正常状态
                } else {

                    // 如果 sender 曾经发送过对 node 的下线报告
                    // 那么清除该报告
                    if (clusterNodeDelFailureReport(node,sender)) {
                        redisLog(REDIS_VERBOSE,
                            "Node %.40s reported node %.40s is back online.",
                            sender->name, node->name);
                    }
                }
            }

            /* If we already know this node, but it is not reachable, and
             * we see a different address in the gossip section, start an
             * handshake with the (possibly) new address: this will result
             * into a node address update if the handshake will be
             * successful. */
            // 如果节点之前处于 PFAIL 或者 FAIL 状态
            // 并且该节点的 IP 或者端口号已经发生变化
            // 那么可能是节点换了新地址，尝试对它进行握手
            if (node->flags & (REDIS_NODE_FAIL|REDIS_NODE_PFAIL) &&
                (strcasecmp(node->ip,g->ip) || node->port != ntohs(g->port)))
            {
                clusterStartHandshake(g->ip,ntohs(g->port));
            }

        // 当前 node 并不认识 Gossip 里面说的这个 node，所以尝试去跟他 handshake
        } else {
            /* If it's not in NOADDR state and we don't have it, we
             * start a handshake process against this IP/PORT pairs.
             *
             * 如果 node 不在 NOADDR 状态，并且当前节点不认识 node 
             * 那么向 node 发送 HANDSHAKE 消息。
             *
             * Note that we require that the sender of this gossip message
             * is a well known node in our cluster, otherwise we risk
             * joining another cluster.
             *
             * 注意，当前节点必须保证 sender 是本集群的节点，
             * 否则我们将有加入了另一个集群的风险。
             */
            if (sender &&
                !(flags & REDIS_NODE_NOADDR) &&
                !clusterBlacklistExists(g->nodename))
            {
                clusterStartHandshake(g->ip,ntohs(g->port));
                // 后续的 connect 会在 clusterCron 完成
            }
        }

        /* Next node */
        // 处理下个节点的信息
        g++;
    }
}

/* IP -> string conversion. 'buf' is supposed to at least be 46 bytes. */
// 将 ip 转换为字符串
void nodeIp2String(char *buf, clusterLink *link) {
    anetPeerToString(link->fd, buf, REDIS_IP_STR_LEN, NULL);
}

/* Update the node address to the IP address that can be extracted
 * from link->fd, and at the specified port.
 *
 * 更新节点的地址， IP 和端口可以从 link->fd 获得。
 *
 * Also disconnect the node link so that we'll connect again to the new
 * address.
 *
 * 并且断开当前的节点连接，并根据新地址创建新连接。
 *
 * If the ip/port pair are already correct no operation is performed at
 * all.
 *
 * 如果 ip 和端口和现在的连接相同，那么不执行任何动作。
 *
 * The function returns 0 if the node address is still the same,
 * otherwise 1 is returned. 
 *
 * 函数返回 0 表示地址不变，地址已被更新则返回 1 。
 */
int nodeUpdateAddressIfNeeded(clusterNode *node, clusterLink *link, int port) {
    char ip[REDIS_IP_STR_LEN];

    /* We don't proceed if the link is the same as the sender link, as this
     * function is designed to see if the node link is consistent with the
     * symmetric link that is used to receive PINGs from the node.
     *
     * As a side effect this function never frees the passed 'link', so
     * it is safe to call during packet processing. */
    // 连接不变，直接返回
    if (link == node->link) return 0;

    // 获取字符串格式的 ip 地址
    nodeIp2String(ip,link);
    // 获取端口号
    if (node->port == port && strcmp(ip,node->ip) == 0) return 0;

    /* IP / port is different, update it. */
    memcpy(node->ip,ip,sizeof(ip));
    node->port = port;

    // 释放旧连接（新连接会在之后自动创建）
    if (node->link) freeClusterLink(node->link);

    redisLog(REDIS_WARNING,"Address updated for node %.40s, now %s:%d",
        node->name, node->ip, node->port);

    /* Check if this is our master and we have to change the
     * replication target as well. */
    // 如果连接来自当前节点（从节点）的主节点，那么根据新地址设置复制对象
    if (nodeIsSlave(myself) && myself->slaveof == node)
        replicationSetMaster(node->ip, node->port);
    return 1;
}

/* Reconfigure the specified node 'n' as a master. This function is called when
 * a node that we believed to be a slave is now acting as master in order to
 * update the state of the node. 
 *
 * 将节点 n 设置为主节点。
 */
// 即可能是其他 node 对于 n 的看法；n 也可以是 myself node
void clusterSetNodeAsMaster(clusterNode *n) {

    // 已经是主节点了。
    if (nodeIsMaster(n)) return;

    // 移除 slaveof
    if (n->slaveof) clusterNodeRemoveSlave(n->slaveof,n);

    // 关闭 SLAVE 标识
    n->flags &= ~REDIS_NODE_SLAVE;

    // 打开 MASTER 标识
    n->flags |= REDIS_NODE_MASTER;

    // 清零 slaveof 属性
    n->slaveof = NULL;

    /* Update config and state. */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE);
}

/* This function is called when we receive a master configuration via a
 * PING, PONG or UPDATE packet. What we receive is a node, a configEpoch of the
 * node, and the set of slots claimed under this configEpoch.
 *
 * 这个函数在节点通过 PING 、 PONG 、 UPDATE 消息接收到一个 master 的配置时调用，
 * 函数以一个节点，节点的 configEpoch ，
 * 以及节点在 configEpoch 纪元下的槽配置作为参数。
 *
 * What we do is to rebind the slots with newer configuration compared to our
 * local configuration, and if needed, we turn ourself into a replica of the
 * node (see the function comments for more info).
 *
 * 这个函数要做的就是在 slots 参数的新配置和本节点的当前配置进行对比，
 * 并更新本节点对槽的布局，
 * 如果有需要的话，函数还会将本节点转换为 sender 的从节点，
 * 更多信息请参考函数中的注释。
 *
 * The 'sender' is the node for which we received a configuration update.
 * Sometimes it is not actaully the "Sender" of the information, like in the case
 * we receive the info via an UPDATE packet. 
 *
 * 根据情况， sender 参数可以是消息的发送者，也可以是消息发送者的主节点。
 */
// TODO: update 消息是什么？
// TODO: configEpoch 的工作过程与原理
void clusterUpdateSlotsConfigWith(clusterNode *sender, uint64_t senderConfigEpoch, unsigned char *slots) {
    int j;
    clusterNode *curmaster, *newmaster = NULL;
    /* The dirty slots list is a list of slots for which we lose the ownership
     * while having still keys inside. This usually happens after a failover
     * or after a manual cluster reconfiguration operated by the admin.
     * 一旦新的 slots bitmap 跟原本的不一样（手动重分配，failover），但是我们不可能简简单单的更新一下我们的
     * slots bitmap 就完事，因为不同的 slots 下面是有着不同 key 的，我们要考虑到对 key 的影响
     *
     * If the update message is not able to demote a master to slave (in this
     * case we'll resync with the master updating the whole key space), we
     * need to delete all the keys in the slots we lost ownership. 
     * TODO: 为什么 update message 能够把一个 master 降级为 slave，就可以不用删除？ */
    uint16_t dirty_slots[REDIS_CLUSTER_SLOTS];
    int dirty_slots_count = 0;

    /* Here we set curmaster(current master) to this node or the node this node
     * replicates to if it's a slave. In the for loop we are
     * interested to check if slots are taken away from curmaster. */
    // 1）如果当前节点是主节点，那么将 curmaster 设置为当前节点
    // 2）如果当前节点是从节点，那么将 curmaster 设置为当前节点正在复制的主节点
    // 稍后在 for 循环中我们将使用 curmaster 检查与当前节点有关的槽是否发生了变动
    curmaster = nodeIsMaster(myself) ? myself : myself->slaveof;

    if (sender == myself) {
        redisLog(REDIS_WARNING,"Discarding UPDATE message about myself.");
        return;
    }

    // 更新槽布局（以 PING 中的 slot 为核心进行遍历）
    for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {

        // 如果 slots 中的槽 j 已经被指派，那么我们就要看看这个 node 又是怎么记录这个 slot 的
        if (bitmapTestBit(slots,j)) {
            /* The slot is already bound to the sender of this message. */
            if (server.cluster->slots[j] == sender) continue;   // 本地记录跟新来的记录

            /* The slot is in importing state, it should be modified only
             * manually via redis-trib (example: a resharding is in progress
             * and the migrating side slot was already closed and is advertising
             * a new config. We still want the slot to be closed manually). */
            // TODO:(DONE) 迁移操作必然是管理员手动发起的，在管理员操作中的
            // 既然 migrate 过程必须全程管理员手动操作，那么所有自动 update、登记，都不应该被允许。直接 skip 掉
            if (server.cluster->importing_slots_from[j]) continue;

            /* We rebind the slot to the new node claiming it if:
             * 1) The slot was unassigned or the new node claims it with a
             *    greater configEpoch.
             * 2) We are not currently importing the slot. */
            // TODO: 为什么要对比 configEpoch ？有什么含义？
            if (server.cluster->slots[j] == NULL ||     // PING 中的 slots 新占领一个本地没有使用过的 slot
                server.cluster->slots[j]->configEpoch < senderConfigEpoch)
            {
                /* Was this slot mine, and still contains keys? Mark it as
                 * a dirty slot. */
                if (server.cluster->slots[j] == myself &&
                    countKeysInSlot(j) &&
                    sender != myself)
                {
                    // 其他 sender 想要占领我现在拥有的 slot，且这个 slot 现在有 key 在用着
                    // 暂存起来，后续再进行处理
                    dirty_slots[dirty_slots_count] = j;
                    dirty_slots_count++;
                }

                // 负责槽 j 的原节点是当前节点的主节点？
                // 如果是的话，说明故障转移发生了，将当前节点的复制对象设置为新的主节点
                if (server.cluster->slots[j] == curmaster)
                    newmaster = sender;

                // 将槽 j 设为未指派
                clusterDelSlot(j);

                // 将槽 j 指派给 sender
                clusterAddSlot(sender,j);

                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE|
                                     CLUSTER_TODO_FSYNC_CONFIG);
            }
        }
    }

    /* If at least one slot was reassigned from a node to another node
     * with a greater configEpoch, it is possible that:
     *
     * 如果当前节点（或者当前节点的主节点）有至少一个槽被指派到了 sender
     * 并且 sender 的 configEpoch 比当前节点的纪元要大，
     * 那么可能发生了：
     *
     * 1) We are a master left without slots. This means that we were
     *    failed over and we should turn into a replica of the new
     *    master.
     *    当前节点是一个不再处理任何槽的主节点，
     *    这时应该将当前节点设置为新主节点的从节点。
     * 2) We are a slave and our master is left without slots. We need
     *    to replicate to the new slots owner. 
     *    当前节点是一个从节点，
     *    并且当前节点的主节点已经不再处理任何槽，
     *    这时应该将当前节点设置为新主节点的从节点。
     */
    if (newmaster && curmaster->numslots == 0) {
        // TODO: 看完 cluster master and slave 之后再回来看
        // TODO: master 在这里被降级为 slave 的触发条件是什么？
        redisLog(REDIS_WARNING,
            "Configuration change detected. Reconfiguring myself "
            "as a replica of %.40s", sender->name);
        // 将 sender 设置为当前节点的主节点
        clusterSetMaster(sender);

        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
    } else if (dirty_slots_count) {
        /* If we are here, we received an update message which removed
         * ownership for certain slots we still have keys about, but still
         * we are serving some slots, so this master node was not demoted to
         * a slave.
         *
         * In order to maintain a consistent state between keys and slots
         * we need to remove all the keys from the slots we lost. */
        for (j = 0; j < dirty_slots_count; j++)
            delKeysInSlot(dirty_slots[j]);
    }
}

/* This function is called when this node is a master, and we receive from
 * another master a configuration epoch that is equal to our configuration
 * epoch.
 *
 * BACKGROUND
 *
 * It is not possible that different slaves get the same config
 * epoch during a failover election, because the slaves need to get voted
 * by a majority. However when we perform a manual resharding of the cluster
 * the node will assign a configuration epoch to itself without to ask
 * for agreement. Usually resharding happens when the cluster is working well
 * and is supervised by the sysadmin, however it is possible for a failover
 * to happen exactly while the node we are resharding a slot to assigns itself
 * a new configuration epoch, but before it is able to propagate it.
 *
 * So technically it is possible in this condition that two nodes end with
 * the same configuration epoch.
 *
 * Another possibility is that there are bugs in the implementation causing
 * this to happen.
 *
 * Moreover when a new cluster is created, all the nodes start with the same
 * configEpoch. This collision resolution code allows nodes to automatically
 * end with a different configEpoch at startup automatically.
 *
 * In all the cases, we want a mechanism that resolves this issue automatically
 * as a safeguard. The same configuration epoch for masters serving different
 * set of slots is not harmful, but it is if the nodes end serving the same
 * slots for some reason (manual errors or software bugs) without a proper
 * failover procedure.
 *
 * In general we want a system that eventually always ends with different
 * masters having different configuration epochs whatever happened, since
 * nothign is worse than a split-brain condition in a distributed system.
 *
 * BEHAVIOR
 *
 * When this function gets called, what happens is that if this node
 * has the lexicographically smaller Node ID compared to the other node
 * with the conflicting epoch (the 'sender' node), it will assign itself
 * the greatest configuration epoch currently detected among nodes plus 1.
 *
 * This means that even if there are multiple nodes colliding, the node
 * with the greatest Node ID never moves forward, so eventually all the nodes
 * end with a different configuration epoch.
 */
void clusterHandleConfigEpochCollision(clusterNode *sender) {
    /* Prerequisites: nodes have the same configEpoch and are both masters. */
    if (sender->configEpoch != myself->configEpoch ||
        !nodeIsMaster(sender) || !nodeIsMaster(myself)) return;
    /* Don't act if the colliding node has a smaller Node ID. */
    if (memcmp(sender->name,myself->name,REDIS_CLUSTER_NAMELEN) <= 0) return;
    /* Get the next ID available at the best of this node knowledge. */
    server.cluster->currentEpoch++;
    myself->configEpoch = server.cluster->currentEpoch;
    clusterSaveConfigOrDie(1);
    redisLog(REDIS_VERBOSE,
        "WARNING: configEpoch collision with node %.40s."
        " Updating my configEpoch to %llu",
        sender->name,
        (unsigned long long) myself->configEpoch);
}

/* When this function is called, there is a packet to process starting
 * at node->rcvbuf. Releasing the buffer is up to the caller, so this
 * function should just handle the higher level stuff of processing the
 * packet, modifying the cluster state if needed.
 *
 * 当这个函数被调用时，说明 node->rcvbuf 中有一条待处理的信息。（而且是恰好只有一条消息，一个 byte 都不多）
 * 信息处理完毕之后的释放工作由调用者处理，所以这个函数只需负责处理信息就可以了。
 *
 * The function returns 1 if the link is still valid after the packet
 * was processed, otherwise 0 if the link was freed since the packet
 * processing lead to some inconsistency error (for instance a PONG
 * received from the wrong sender ID). 
 *
 * 如果函数返回 1 ，那么说明处理信息时没有遇到问题，连接依然可用。(但是有可能会因为消息格式有问题，直接 drop 掉本条消息)
 * 如果函数返回 0 ，那么说明信息处理时遇到了不一致问题
 * （比如接收到的 PONG 是发送自不正确的发送者 ID 的），连接已经被释放。
 */
// 解析并处理其他 node 发送过来的消息（八卦消息）
// 一些已知的 node 可能回重复发来 MEET 消息
// 当本 node 第一次处理新的 MEET 时，很快就退出这个函数了，基本上就是 createNode、send PONG 就完事了
// Nodes always accept connections on the cluster bus port, and even reply to pings when received, 
// even if the pinging node is not trusted. (也就是说，只要是 link 发来 PING，就会无情的回 PONG，也不管这个 node 是不是已知的   )
// However, all other packets will be discarded by the receiving node if the sending node is not considered part of the cluster.
// 既然每一个 node 都是无差别的回包，那么检查 PONG 的有效性，则需要所有主动发送者来进行检查
// 这也就是为什么这个函数里面经常检查 sender、link->node，因为不被认识 node 发来的 PONG 将无法 hit 任何 if-branch，被默默的 drop 掉
// 被动连接中，link->node 将会是 NULL
// 除了 MEET，MEET 会强迫接收者接受一个新的 node；

// 这是一个相当重要的函数，因为每个 node 都是从这个函数中获取其他 node 的状态信息、想要传递的信息
// 所以从这个函数获取的不同信息，将会触发相应的 node 本身、对于其他 node 的看法、对于整个 cluster 的看法**转变**；
// 进而引发不同的过程
int clusterProcessPacket(clusterLink *link) {

    // 指向消息头
    clusterMsg *hdr = (clusterMsg*) link->rcvbuf;

    // 消息的长度
    uint32_t totlen = ntohl(hdr->totlen);

    // 消息的类型
    uint16_t type = ntohs(hdr->type);

    // 消息发送者的标识
    uint16_t flags = ntohs(hdr->flags);

    uint64_t senderCurrentEpoch = 0, senderConfigEpoch = 0;

    clusterNode *sender;

    // 更新接受消息计数器
    server.cluster->stats_bus_messages_received++;

// #define CLUSTERMSG_TYPE_PING 0          /* Ping */
// #define CLUSTERMSG_TYPE_PONG 1          /* Pong (reply to Ping) */
// #define CLUSTERMSG_TYPE_MEET 2          /* Meet "let's join" message */
    redisLog(REDIS_DEBUG,"--- Processing packet of type %d, %lu bytes",
        type, (unsigned long) totlen);

    /* Perform sanity checks */
    // 合法性检查，不合法的直接 drop 掉
    if (totlen < 16) return 1; /* At least signature, version, totlen, count. */
    if (ntohs(hdr->ver) != 0) return 1; /* Can't handle versions other than 0.*/
    if (totlen > sdslen(link->rcvbuf)) return 1;

    // 检查不同种类的消息，长度是否合法（有没有用错 union）
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        uint16_t count = ntohs(hdr->count);
        uint32_t explen; /* expected length of this packet */

        //  len(clusterMsg) = sizeof(header) + ((union clusterMsgDataGossip) * count)
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += (sizeof(clusterMsgDataGossip)*count);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataFail);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataPublish) +
                ntohl(hdr->data.publish.msg.channel_len) +
                ntohl(hdr->data.publish.msg.message_len);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST ||
               type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK ||
               type == CLUSTERMSG_TYPE_MFSTART)
    {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataUpdate);
        if (totlen != explen) return 1;
    }

    /* Check if the sender is a known node. */
    // 查找发送者节点
    // node A 第一次收到 node B 回应 MEET 的 PONG 时，sender == NULL，因为之前 add 进哈希表里面的 node-id 是 node A 随机生成的的
    // 现在 node B 发过来的 PONG 里面的 node-id 是真实的，但并不存在与哈希表里面
    sender = clusterLookupNode(hdr->sender);
    
    // 节点存在，并且不是 HANDSHAKE 节点
    // 那么个更新节点的配置纪元信息
    // TODO:(DONE) 这是啥 case？更新 currentEpoch
    if (sender && !nodeInHandshake(sender)) {
        /* Update our curretEpoch if we see a newer epoch in the cluster. */
        senderCurrentEpoch = ntohu64(hdr->currentEpoch);
        senderConfigEpoch = ntohu64(hdr->configEpoch);

        // 无论是 slave 还是 master，当收到了 CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 
        // 都会更新 server.cluster->currentEpoch，
        // 对于 slave 来说：这样才能正确的发起属于自己的投票
        // 对于 master 来说：这样才能避免再同一轮投票中，重复投票
        if (senderCurrentEpoch > server.cluster->currentEpoch)
            server.cluster->currentEpoch = senderCurrentEpoch;

        /* Update the sender configEpoch if it is publishing a newer one. */
        if (senderConfigEpoch > sender->configEpoch) {
            sender->configEpoch = senderConfigEpoch;
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_FSYNC_CONFIG);
        }
        /* Update the replication offset info for this node. */
        sender->repl_offset = ntohu64(hdr->offset);
        sender->repl_offset_time = mstime();

        /* If we are a slave performing **a manual failover** and our master
         * sent its offset while already paused, populate the MF state. */
        if (server.cluster->mf_end &&   // 开始了 failover 过程
            nodeIsSlave(myself) &&      // slave 才能主动发起 failover
            myself->slaveof == sender &&    // slave 的 master 回复了
            hdr->mflags[0] & CLUSTERMSG_FLAG0_PAUSED &&
            server.cluster->mf_master_offset == 0)
        {
            // 这时候记录的 repl_offset 自然是固定不变的，因为 master 已经暂停了服务了，暂时不相应 redis-cli
            server.cluster->mf_master_offset = sender->repl_offset;
            redisLog(REDIS_WARNING,
                "Received replication offset for paused "
                "master manual failover: %lld",
                server.cluster->mf_master_offset);
        }
    }

    /* Process packets by type. */
    // 根据消息的类型，处理节点

    // 在 ping 中进行 re-connect 或者是 meet 中进行 connect
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_MEET) {
        redisLog(REDIS_DEBUG,"[node create] Ping packet received: [link:%p, node:%p]", 
                 (void*)link, (void*)link->node);

        /* Add this node if it is new for us and the msg type is MEET.
         *
         * 如果当前节点是第一次遇见这个节点，并且对方发来的是 MEET 信息，
         * 那么将这个节点添加到集群的节点列表里面。
         *
         * In this stage we don't try to add the node with the right
         * flags, slaveof pointer, and so forth, as this details will be
         * resolved when we'll receive PONGs from the node. 
         *
         * TODO: 为什么现在不设置呢？是因为还不知道吗？MEET 不带这些信息的吗？还是说这些信息还不可信？
         * 节点目前的 flag 、 slaveof 等属性的值都是未设置的，
         * 等当前节点向对方发送 PING 命令之后，
         * 这些信息可以从对方回复的 PONG 信息中取得。
         */
        if (!sender && type == CLUSTERMSG_TYPE_MEET) {
            clusterNode *node;

            // 创建 HANDSHAKE 状态的新节点
            node = createClusterNode(NULL,REDIS_NODE_HANDSHAKE);

            // 设置 IP 和端口
            nodeIp2String(node->ip,link);
            node->port = ntohs(hdr->port);

            // 将新节点添加到集群
            clusterAddNode(node);

            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
        }

        /* Get info from the gossip section */
        // 分析并取出消息中的 gossip 节点信息
        // TODO:(DONE) 为什么两个 node 的时候，没有 gossip 呢？（没有发的必要，一个是你，一个是对方）
        clusterProcessGossipSection(hdr,link);

        /* Anyway reply with a PONG */
        // 向目标节点返回一个 PONG（表示 meet、ping 已经收到，并且 gossip 一下自己的 info）
        clusterSendPing(link,CLUSTERMSG_TYPE_PONG); // 正常情况下的 PONG 回包

        // 即使进入了这个 if-brance，link->node 也依旧是 NULL 的
        // 新的 MEET 的话，基本上走完这个 if-brance，这个函数就结束了
    }

    /* PING or PONG: process config information. */
    // 这是一条 PING 、 PONG 或者 MEET 消息
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        redisLog(REDIS_DEBUG,"[parse config-info %s%s%s] packet received: [link:%p, node:%p]",
            type == CLUSTERMSG_TYPE_PING ? "PING" : "",
            type == CLUSTERMSG_TYPE_PONG ? "PONG" : "",
            type == CLUSTERMSG_TYPE_MEET ? "MEET" : "",
            (void*)link,
            (void*)link->node);

        if (link->node) {
            // 连接的 clusterNode 结构存在; 暗示了这个 link 是由本 redis-server 主动创建的
            // 节点处于 HANDSHAKE 状态
            if (nodeInHandshake(link->node)) {
                // 通常是本 node 向其他 node 发送 MEET，然后其他 node 收到之后，返回了 PONG。
                // 说明 node A --> node B 的通路构建完成
                /* If we already have this node, try to change the
                 * IP/port of the node with the new one. 
                 * 有些 node 可能回重复发送 MEET
                 */
                if (sender) {
                    // 因为一开始加入 server.clusters->nodes 里面的 node-id 是 node A 随机生成的
                    // 所以 node B 回的第一个 PONG 一定进不来这里
                    redisLog(REDIS_VERBOSE,
                        "Handshake: we already know node %.40s, "
                        "updating the address if needed.", sender->name);
                    // 如果有需要的话，更新节点的地址
                    if (nodeUpdateAddressIfNeeded(sender,link,ntohs(hdr->port)))
                    {
                        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                             CLUSTER_TODO_UPDATE_STATE);
                    }
                    /* Free this node as we alrady have it. This will
                     * cause the link to be freed as well. */
                    // 释放节点
                    freeClusterNode(link->node);
                    return 0;
                }

                /* First thing to do is replacing the random name with the
                 * right node name if this was a handshake stage. */
                // 用节点的真名替换在 HANDSHAKE 时创建的随机名字，同时别忘了 update server.cluster->nodes 
                clusterRenameNode(link->node, hdr->sender);
                redisLog(REDIS_DEBUG,"Handshake with node %.40s completed.",
                    link->node->name);

                // 关闭 HANDSHAKE 状态
                link->node->flags &= ~REDIS_NODE_HANDSHAKE;

                // 设置节点的角色(只关心 slave、master 两个)
                link->node->flags |= flags&(REDIS_NODE_MASTER|REDIS_NODE_SLAVE);

                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);

            // 节点已存在，但它的 id 和当前节点保存的 id 不同
            } else if (memcmp(link->node->name,hdr->sender,
                        REDIS_CLUSTER_NAMELEN) != 0)
            {
                /* If the reply has a non matching node ID we
                 * disconnect this node and set it as not having an associated
                 * address. */
                // 那么将这个节点设为 NOADDR 
                // 并断开连接
                redisLog(REDIS_DEBUG,"PONG contains mismatching sender ID");
                link->node->flags |= REDIS_NODE_NOADDR;
                link->node->ip[0] = '\0';
                link->node->port = 0;

                // 断开连接
                freeClusterLink(link);

                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                return 0;
            }
        }

        /* Update the node address if it changed. */
        // 如果收到的消息为 PING 
        // 并且发送者不在 HANDSHAKE 状态
        // 那么更新发送者的 addr 信息
        if (sender && type == CLUSTERMSG_TYPE_PING &&
            !nodeInHandshake(sender) &&
            nodeUpdateAddressIfNeeded(sender,link,ntohs(hdr->port)))
        {
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_UPDATE_STATE);
        }

        /* Update our info about the node */
        // 如果这是一条 PONG 消息，那么更新我们关于 node 节点的认识
        if (link->node && type == CLUSTERMSG_TYPE_PONG) {

            // 最后一次接到该节点的 PONG 的时间
            link->node->pong_received = mstime();

            // 清零最近一次等待 PING 命令的时间
            link->node->ping_sent = 0;

            /* The PFAIL condition can be reversed without external
             * help if it is momentary (that is, if it does not
             * turn into a FAIL state).
             *
             * 接到节点的 PONG 回复，我们可以移除节点的 PFAIL 状态。
             *
             * The FAIL condition is also reversible under specific
             * conditions detected by clearNodeFailureIfNeeded(). 
             *
             * 如果节点的状态为 FAIL ，
             * 那么是否撤销该状态要根据 clearNodeFailureIfNeeded() 函数来决定。
             */
            if (nodeTimedOut(link->node)) {
                // 撤销 PFAIL
                link->node->flags &= ~REDIS_NODE_PFAIL;

                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            } else if (nodeFailed(link->node)) {
                // 看是否可以撤销 FAIL
                clearNodeFailureIfNeeded(link->node);
            }
        }

        /* Check for role switch: slave -> master or master -> slave. */
        // 感知 sender 节点的身份信息变化，并在需要时进行更新
        if (sender) {

            // 发送消息的节点的 slaveof 为 REDIS_NODE_NULL_NAME
            // 那么 sender 就是一个主节点
            if (!memcmp(hdr->slaveof,REDIS_NODE_NULL_NAME,
                sizeof(hdr->slaveof)))
            {
                /* Node is a master. */
                // 设置 sender 为主节点
                clusterSetNodeAsMaster(sender);

            // sender 的 slaveof 不为空，那么这是一个从节点
            } else {    // 从 PONG\PING 中取出 slaveof 信息，并更新

                /* Node is a slave. */
                // 取出 sender 的主节点
                clusterNode *master = clusterLookupNode(hdr->slaveof);

                // sender 由主节点变成了从节点，重新配置 sender
                if (nodeIsMaster(sender)) {
                    /* Master turned into a slave! Reconfigure the node. */

                    // 删除所有由该节点负责的槽
                    clusterDelNodeSlots(sender);

                    // 更新标识
                    sender->flags &= ~REDIS_NODE_MASTER;
                    sender->flags |= REDIS_NODE_SLAVE;

                    /* Remove the list of slaves from the node. */
                    // 移除 sender 的从节点名单
                    if (sender->numslaves) clusterNodeResetSlaves(sender);

                    /* Update config and state. */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                         CLUSTER_TODO_UPDATE_STATE);
                }

                /* Master node changed for this slave? */

                // 检查 sender 的主节点是否变更
                if (master && sender->slaveof != master) {
                    // 如果 sender 之前的主节点不是现在的主节点
                    // 那么在旧主节点的从节点列表中移除 sender
                    if (sender->slaveof)
                        clusterNodeRemoveSlave(sender->slaveof,sender);

                    // 并在新主节点的从节点列表中添加 sender
                    clusterNodeAddSlave(master,sender);

                    // 更新 sender 的主节点
                    sender->slaveof = master;

                    /* Update config. */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                }
            }
        }

        /* Update our info about served slots.
         *
         * 更新当前节点对 sender 所处理槽的认识。
         *
         * Note: this MUST happen after we update the master/slave state
         * so that REDIS_NODE_MASTER flag will be set. 
         *
         * 这部分的更新 *必须* 在更新 sender 的主/从节点信息之后，
         * 因为这里需要用到 REDIS_NODE_MASTER 标识。
         */

        /* Many checks are only needed if the set of served slots this
         * instance claims is different compared to the set of slots we have
         * for it. Check this ASAP to avoid other computational expansive
         * checks later. */
        clusterNode *sender_master = NULL; /* Sender or its master if slave. */
        int dirty_slots = 0; /* Sender claimed slots don't match my view? */

        if (sender) {
            sender_master = nodeIsMaster(sender) ? sender : sender->slaveof;
            if (sender_master) {
                dirty_slots = memcmp(sender_master->slots,
                        hdr->myslots,sizeof(hdr->myslots)) != 0;
            }
        }

        /* 1) If the sender of the message is a master, and we detected that
         *    the set of slots it claims changed, scan the slots to see if we
         *    need to update our configuration. */
        // 如果 sender 是主节点，并且 sender 的槽布局出现了变动
        // 那么检查当前节点对 sender 的槽布局设置，看是否需要进行更新
        if (sender && nodeIsMaster(sender) && dirty_slots)
            clusterUpdateSlotsConfigWith(sender,senderConfigEpoch,hdr->myslots);

        /* 2) We also check for the reverse condition, that is, the sender
         *    claims to serve slots we know are served by a master with a
         *    greater configEpoch. If this happens we inform the sender.
         *
         *    检测和条件 1 的相反条件，也即是，
         *    sender 处理的槽的配置纪元比当前节点已知的某个节点的配置纪元要低，
         *    如果是这样的话，通知 sender 。
         *
         * This is useful because sometimes after a partition heals, a
         * reappearing master may be the last one to claim a given set of
         * hash slots, but with a configuration that other instances know to
         * be deprecated. Example:
         *
         * 这种情况可能会出现在网络分裂中，
         * 一个重新上线的主节点可能会带有已经过时的槽布局。
         *
         * 比如说：
         *
         * A and B are master and slave for slots 1,2,3.
         * A 负责槽 1 、 2 、 3 ，而 B 是 A 的从节点。
         *
         * A is partitioned away, B gets promoted.
         * A 从网络中分裂出去，B 被提升为主节点。
         *
         * B is partitioned away, and A returns available.
         * B 从网络中分裂出去， A 重新上线（但是它所使用的槽布局是旧的）。
         *
         * Usually B would PING A publishing its set of served slots and its
         * configEpoch, but because of the partition B can't inform A of the
         * new configuration, so other nodes that have an updated table must
         * do it. In this way A will stop to act as a master (or can try to
         * failover if there are the conditions to win the election).
         *
         * 在正常情况下， B 应该向 A 发送 PING 消息，告知 A ，自己（B）已经接替了
         * 槽 1、 2、 3 ，并且带有更更的配置纪元，但因为网络分裂的缘故，
         * 节点 B 没办法通知节点 A ，
         * 所以通知节点 A 它带有的槽布局已经更新的工作就交给其他知道 B 带有更高配置纪元的节点来做。
         * 当 A 接到其他节点关于节点 B 的消息时，
         * 节点 A 就会停止自己的主节点工作，又或者重新进行故障转移。
         */
        if (sender && dirty_slots) {
            int j;

            for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {

                // 检测 slots 中的槽 j 是否已经被指派
                if (bitmapTestBit(hdr->myslots,j)) {

                    // 当前节点认为槽 j 由 sender 负责处理，
                    // 或者当前节点认为该槽未指派，那么跳过该槽
                    if (server.cluster->slots[j] == sender ||
                        server.cluster->slots[j] == NULL) continue;

                    // 当前节点槽 j 的配置纪元比 sender 的配置纪元要大
                    if (server.cluster->slots[j]->configEpoch >
                        senderConfigEpoch)
                    {
                        redisLog(REDIS_VERBOSE,
                            "Node %.40s has old slots configuration, sending "
                            "an UPDATE message about %.40s",
                                sender->name, server.cluster->slots[j]->name);

                        // 向 sender 发送关于槽 j 的更新信息
                        clusterSendUpdate(sender->link,
                            server.cluster->slots[j]);

                        /* TODO: instead of exiting the loop send every other
                         * UPDATE packet for other nodes that are the new owner
                         * of sender's slots. */
                        break;
                    }
                }
            }
        }

        /* If our config epoch collides with the sender's try to fix
         * the problem. */
        if (sender &&
            nodeIsMaster(myself) && nodeIsMaster(sender) &&
            senderConfigEpoch == myself->configEpoch)
        {
            clusterHandleConfigEpochCollision(sender);
        }

        /* Get info from the gossip section */
        // 分析并提取出消息 gossip 协议部分的信息
        clusterProcessGossipSection(hdr,link);

    // 这是一条 FAIL 消息： sender 告知当前节点，某个节点已经进入 FAIL 状态。
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        clusterNode *failing;

        if (sender) {

            // 获取下线节点的消息
            failing = clusterLookupNode(hdr->data.fail.about.nodename);
            // 下线的节点既不是当前节点，在当前 node 看来：failing 有没有处于 FAIL 状态
            if (failing &&
                !(failing->flags & (REDIS_NODE_FAIL|REDIS_NODE_MYSELF)))
            {
                redisLog(REDIS_NOTICE,
                    "FAIL message received from %.40s about %.40s",
                    hdr->sender, hdr->data.fail.about.nodename);

                // 打开 FAIL 状态
                failing->flags |= REDIS_NODE_FAIL;
                failing->fail_time = mstime();
                // 关闭 PFAIL 状态
                failing->flags &= ~REDIS_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            }
        } else {
            redisLog(REDIS_NOTICE,
                "Ignoring FAIL message from unknonw node %.40s about %.40s",
                hdr->sender, hdr->data.fail.about.nodename);
        }

    // 这是一条 PUBLISH 消息
    } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
        robj *channel, *message;
        uint32_t channel_len, message_len;

        /* Don't bother creating useless objects if there are no
         * Pub/Sub subscribers. */
        // 只在有订阅者时创建消息对象
        if (dictSize(server.pubsub_channels) ||
           listLength(server.pubsub_patterns))
        {
            // 频道长度
            channel_len = ntohl(hdr->data.publish.msg.channel_len);

            // 消息长度
            message_len = ntohl(hdr->data.publish.msg.message_len);

            // 频道
            channel = createStringObject(
                        (char*)hdr->data.publish.msg.bulk_data,channel_len);

            // 消息
            message = createStringObject(
                        (char*)hdr->data.publish.msg.bulk_data+channel_len,
                        message_len);
            // 发送消息
            pubsubPublishMessage(channel,message);

            decrRefCount(channel);
            decrRefCount(message);
        }

    // 这是其他 node 请求本 node 投票故障迁移授权的消息： sender 请求当前节点为它进行故障转移投票
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST) {
        if (!sender) return 1;  /* We don't know that node. */
        // 如果条件允许的话，向 sender 投票，支持它进行故障转移
        // 只有 master 身份的 node 拥有投票权
        clusterSendFailoverAuthIfNeeded(sender,hdr);

    // 这是其他 node 的故障迁移投票： sender 支持当前节点执行故障转移操作
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK) {
        if (!sender) return 1;  /* We don't know that node. */

        /* We consider this vote only if the sender is a master serving
         * a non zero number of slots, and its currentEpoch is greater or
         * equal to epoch where this node started the election. */
        // 只有正在处理至少一个槽的主节点的投票会被视为是有效投票
        // 只有符合以下条件， sender 的投票才算有效：
        // 1） sender 是主节点
        // 2） sender 正在处理至少一个槽
        // 3） sender 的配置纪元大于等于当前节点的配置纪元
        if (nodeIsMaster(sender) && sender->numslots > 0 &&
            senderCurrentEpoch >= server.cluster->failover_auth_epoch)
        {
            // 增加支持票数
            server.cluster->failover_auth_count++;

            /* Maybe we reached a quorum here, set a flag to make sure
             * we check ASAP. */
            // 尽快检查是不是够票了，当然，还是会等到 clusterCron() 再检查的
            clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        }

    } else if (type == CLUSTERMSG_TYPE_MFSTART) {
        /* This message is acceptable only if I'm a master and the sender
         * is one of my slaves. */
        if (!sender || sender->slaveof != myself) return 1;
        /* Manual failover requested from slaves. Initialize the state
         * accordingly. */
        resetManualFailover();
        server.cluster->mf_end = mstime() + REDIS_CLUSTER_MF_TIMEOUT;
        server.cluster->mf_slave = sender;
        pauseClients(mstime()+(REDIS_CLUSTER_MF_TIMEOUT*2));
        redisLog(REDIS_WARNING,"Manual failover requested by slave %.40s.",
            sender->name);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        redisLog(REDIS_DEBUG,"[parse config-info UPDATE] packet received: [link:%p, node:%p]",
            (void*)link,
            (void*)link->node);

        clusterNode *n; /* The node the update is about. */
        uint64_t reportedConfigEpoch =
                    ntohu64(hdr->data.update.nodecfg.configEpoch);

        if (!sender) return 1;  /* We don't know the sender. */

        // 获取需要更新的节点
        n = clusterLookupNode(hdr->data.update.nodecfg.nodename);
        if (!n) return 1;   /* We don't know the reported node. */

        // 消息的纪元并不大于节点 n 所处的配置纪元
        // 无须更新
        if (n->configEpoch >= reportedConfigEpoch) return 1; /* Nothing new. */

        /* If in our current config the node is a slave, set it as a master. */
        // 如果节点 n 为从节点，但它的槽配置更新了
        // 那么说明这个节点已经变为主节点，将它设置为主节点
        if (nodeIsSlave(n)) clusterSetNodeAsMaster(n);

        /* Update the node's configEpoch. */
        n->configEpoch = reportedConfigEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_FSYNC_CONFIG);

        /* Check the bitmap of served slots and udpate our
         * config accordingly. */
        // 将消息中对 n 的槽布局与当前节点对 n 的槽布局进行对比
        // 在有需要时更新当前节点对 n 的槽布局的认识
        clusterUpdateSlotsConfigWith(n,reportedConfigEpoch,
            hdr->data.update.nodecfg.slots);
    } else {
        redisLog(REDIS_WARNING,"Received unknown packet type: %d", type);
    }
    return 1;
}

/* This function is called when we detect the link with this node is lost.

   这个函数在发现节点的连接已经丢失时使用。

   We set the node as no longer connected. The Cluster Cron will detect
   this connection and will try to get it connected again.

   我们将节点的状态设置为断开状态，Cluster Cron 会根据该状态尝试重新连接节点。

   Instead if the node is a temporary node used to accept a query, we
   completely free the node on error. 

   如果连接是一个临时连接的话，那么它就会被永久释放，不再进行重连。

   */
// 没有被 node 接管的 link 一旦被释放，就是永久释放（因为这是一个被动连接，不需要本 node 关心重连的问题）
// 被 node 接管的 link，虽然被释放了，但是在 clusterCron 里面发现有 node->link == NULL, 那就重连
void handleLinkIOError(clusterLink *link) {
    freeClusterLink(link);
}

/* Send data. This is handled using a trivial send buffer that gets
 * consumed by write(). We don't try to optimize this for speed too much
 * as this is a very low traffic channel. 
 *
 * 写事件处理器，用于向集群节点发送信息。
 */
void clusterWriteHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    clusterLink *link = (clusterLink*) privdata;
    ssize_t nwritten;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    // 写入信息
    nwritten = write(fd, link->sndbuf, sdslen(link->sndbuf));

    // 写入错误
    if (nwritten <= 0) {
        redisLog(REDIS_DEBUG,"I/O error writing to node link: %s",
            strerror(errno));
        handleLinkIOError(link);
        return;
    }

    // 删除已写入的部分
    sdsrange(link->sndbuf,nwritten,-1);

    // 如果所有当前节点输出缓冲区里面的所有内容都已经写入完毕
    // （缓冲区为空）
    // 那么删除写事件处理器
    if (sdslen(link->sndbuf) == 0)
        aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
}

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. */
// 读事件处理器
// 首先读入内容的头，以判断读入内容的长度
// 如果内容是一个 whole packet ，那么调用函数来处理这个 packet 。
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    // 这个 buf 仅仅是为了编程方便罢了，完全是可以优化掉，转而使用 link->rcvbuf 替代的
    // 当然，这个中间 buf 的引入，也能够有效地避免异常问题，确保异常安全性
    char buf[sizeof(clusterMsg)];
    ssize_t nread;
    clusterMsg *hdr;
    clusterLink *link = (clusterLink*) privdata;
    int readlen, rcvbuflen;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    // 尽可能地多读数据
    // 在正常情况下：
    // 每一次执行到 while 的末尾，只会有两种情况：
    // 1. sdslen(link->rcvlen) == totlen, 进入 clusterProcessPacket()；
    // 2. 整个 clusterMsg 还没有读完整，要再来一次 read
    // 不可能出现，rcvbuf 里面同时包含一条以上的信息 rcvbuf 里面的信息只可能 <= 1
    // 只因为 read 的时候，read 多少被 readlen 卡死了，都是算的刚刚好的
    while(1) { /* Read as long as there is data to read. */

        // 检查输入缓冲区的长度（看看作为最重要的消息头是不是到了，8 bytes）
        rcvbuflen = sdslen(link->rcvbuf);
        if (rcvbuflen < 8) {
            // 完整的头信息（8 字节，sig + totlen）还没有到
            /* First, obtain the first 8 bytes to get the full message
             * length. */
            readlen = 8 - rcvbuflen;
        } else {
            //  完成的消息头都已经在 buf 里面了，可以直接读了
            /* Finally read the full message. */
            hdr = (clusterMsg*) link->rcvbuf;   // 很灵性吧，是从 rcvbuf 里面 peek 一眼
            if (rcvbuflen == 8) {
                /* Perform some sanity check on the message signature
                 * and length. */
                if (memcmp(hdr->sig,"RCmb",4) != 0 ||
                    ntohl(hdr->totlen) < CLUSTERMSG_MIN_LEN)
                {
                    redisLog(REDIS_WARNING,
                        "Bad message length or signature received "
                        "from Cluster bus.");
                    handleLinkIOError(link);
                    return;
                }
            }
            // 记录已读入内容长度
            readlen = ntohl(hdr->totlen) - rcvbuflen;
            if (readlen > sizeof(buf)) readlen = sizeof(buf);
        }

        // 读入内容
        // case 1: sig + totlen 并没有完整从内核的 socket 中拿到
        // case 2: sig + totlen 已经完整，且刚好等于 totlen 地从内核的 socket 里面拿到了
        nread = read(fd,buf,readlen);

        // 没有内容可读，非阻塞嘛，里面返回
        if (nread == -1 && errno == EAGAIN) return; /* No more data ready. */

        // 处理读入错误
        if (nread <= 0) {
            /* I/O error... */
            // 异步 connect() 失败的话，将会返回 Connection refused 的 errno （三次握手直接失败了）
            redisLog(REDIS_DEBUG,"I/O error reading from node link: %s",
                (nread == 0) ? "connection closed" : strerror(errno));
            handleLinkIOError(link);
            return;
        } else {
            /* Read data and recast the pointer to the new buffer. */
            // 将读入的内容追加进 rcvbuf 里面，备用（read 可能会导致这个 read-while-loop 异常退出，所以要 buf 起来）
            // 其实可以考虑去掉 buf 这个多余的缓冲
            link->rcvbuf = sdscatlen(link->rcvbuf,buf,nread);
            hdr = (clusterMsg*) link->rcvbuf;
            rcvbuflen += nread;
        }

        /* Total length obtained? Process this packet. */
        // 检查已读入内容的长度，看是否整条信息已经被读入了
        if (rcvbuflen >= 8 && rcvbuflen == ntohl(hdr->totlen)) {
            // 如果是的话，执行处理信息的函数
            if (clusterProcessPacket(link)) {
                // 处理成功，立马清掉，因为这个 buf 一个消息
                sdsfree(link->rcvbuf);
                link->rcvbuf = sdsempty();
            } else {
                // 当 clusterProcessPacket() return 0 的时候，在里面就已经 断开了 link 了
                return; /* Link no longer valid. */
            }
        }
    }
}

/* Put stuff into the send buffer.
 *
 * 发送信息
 *
 * It is guaranteed that this function will never have as a side effect
 * the link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with the same link later. 
 *
 * 因为发送不会对连接本身造成不良的副作用，
 * 所以可以在发送信息的处理器上做一些针对连接本身的动作。
 */
void clusterSendMessage(clusterLink *link, unsigned char *msg, size_t msglen) {

    // 安装写事件处理器
    if (sdslen(link->sndbuf) == 0 && msglen != 0)
        aeCreateFileEvent(server.el,link->fd,AE_WRITABLE,
                    clusterWriteHandler,link);

    // 将信息追加到输出缓冲区
    link->sndbuf = sdscatlen(link->sndbuf, msg, msglen);

    // 增一发送信息计数
    server.cluster->stats_bus_messages_sent++;
}

/* Send a message to all the nodes that are part of the cluster having
 * a connected link.
 *
 * 向节点连接的所有其他节点发送信息。
 *
 * It is guaranteed that this function will never have as a side effect
 * some node->link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with node links later. */
void clusterBroadcastMessage(void *buf, size_t len) {
    dictIterator *di;
    dictEntry *de;

    // 遍历所有已知节点
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        // 不向未连接节点发送信息
        if (!node->link) continue;

        // 不向节点自身或者 HANDSHAKE 状态的节点发送信息
        if (node->flags & (REDIS_NODE_MYSELF|REDIS_NODE_HANDSHAKE))
            continue;

        // 发送信息
        clusterSendMessage(node->link,buf,len);
    }
    dictReleaseIterator(di);
}

/* Build the message header */
// 构建 header，header 总是自己这个 node 的信息，是发送者的信息
/**
 * 之所以没有公布 node 之间的 message 协议，是因为压根没有精心设计的协议
 * 收发双发直接通过 clusterMsg 这个结构体来序列化消息。这个结构体随时可以变
 * 只要 send 跟 read 两边协同好就可以了
 * 怎么防止 half-read\half-write ？ 缓冲区呀，只有当解析了 message-length 且 缓冲区有 message-length 长度的数据时
 * 才正式从缓冲区里面拿出数据，开始处理
*/
// TODO: 最后再看吧，里面装的都是基础信息
void clusterBuildMessageHdr(clusterMsg *hdr, int type) {
    int totlen = 0;
    uint64_t offset;
    clusterNode *master;

    /* If this node is a master, we send its slots bitmap and configEpoch.
     *
     * 如果这是一个主节点，那么发送该节点的槽 bitmap 和配置纪元。
     *
     * If this node is a slave we send the master's information instead (the
     * node is flagged as slave so the receiver knows that it is NOT really
     * in charge for this slots.
     * 如果这是一个从节点，
     * 那么发送这个节点的主节点的槽 bitmap 和配置纪元。
     *
     * 因为接收信息的节点通过标识可以知道这个节点是一个从节点，
     * 所以接收信息的节点不会将从节点错认作是主节点。
     */
    // TODO: 为什么 master 的信息还需要 slave 去公布一下？
    master = (nodeIsSlave(myself) && myself->slaveof) ?
              myself->slaveof : myself;

    // 清零信息头
    memset(hdr,0,sizeof(*hdr));

    // Siganture "RCmb" (Redis Cluster message bus)
    hdr->sig[0] = 'R';
    hdr->sig[1] = 'C';
    hdr->sig[2] = 'm';
    hdr->sig[3] = 'b';

    // 设置信息类型
    hdr->type = htons(type);

    // 设置信息发送者
    memcpy(hdr->sender,myself->name,REDIS_CLUSTER_NAMELEN);

    // 设置当前节点负责的槽（直接把本 node->slots bitmap 拷贝一下就好了）
    memcpy(hdr->myslots,master->slots,sizeof(hdr->myslots));

    // 清零 slaveof 域
    memset(hdr->slaveof,0,REDIS_CLUSTER_NAMELEN);

    // 如果节点是从节点的话，那么设置 slaveof 域
    if (myself->slaveof != NULL)
        memcpy(hdr->slaveof,myself->slaveof->name, REDIS_CLUSTER_NAMELEN);

    // 设置端口号
    hdr->port = htons(server.port);

    // 设置标识
    hdr->flags = htons(myself->flags);

    // 设置状态（这个 node 觉得整个 cluster 集群是 UP\DOWN）
    hdr->state = server.cluster->state;

    /* Set the currentEpoch and configEpochs. */
    // 设置集群当前配置纪元
    hdr->currentEpoch = htonu64(server.cluster->currentEpoch);
    // 设置主节点当前配置纪元
    hdr->configEpoch = htonu64(master->configEpoch);

    /* Set the replication offset. */
    // 设置复制偏移量
    if (nodeIsSlave(myself))
        offset = replicationGetSlaveOffset();
    else
        offset = server.master_repl_offset;
    hdr->offset = htonu64(offset);

    /* Set the message flags. */
    if (nodeIsMaster(myself) && server.cluster->mf_end)
        hdr->mflags[0] |= CLUSTERMSG_FLAG0_PAUSED;

    /* Compute the message length for certain messages. For other messages
     * this is up to the caller. */
    // 计算信息的长度
    if (type == CLUSTERMSG_TYPE_FAIL) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataFail);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataUpdate);
    }

    // 设置信息的长度
    hdr->totlen = htonl(totlen);
    /* For PING, PONG, and MEET, fixing the totlen field is up to the caller. */
}

/* Send a PING or PONG packet to the specified node, making sure to add enough
 * gossip informations. */
// 向指定节点发送一条 MEET 、 PING 或者 PONG 消息
// Ping and pong packets also contain a gossip section. 
// This section offers to the receiver a view of what the sender node thinks about other nodes in the cluster.
void clusterSendPing(clusterLink *link, int type) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    int gossipcount = 0/*已经发送了多少个 Gossip*/, totlen;
    /* freshnodes is the number of nodes we can still use to populate the
     * gossip section of the ping packet. Basically we start with the nodes
     * we have in memory minus two (ourself and the node we are sending the
     * message to). Every time we add a node we decrement the counter, so when
     * it will drop to <= zero we know there is no more gossip info we can
     * send. */
    // freshnodes 是用于发送 gossip 信息的计数器
    // 每次发送一条信息时，程序将 freshnodes 的值减一
    // 当 freshnodes 的数值小于等于 0 时，程序停止发送 gossip 信息
    // freshnodes 的数量是节点目前的 nodes 表中的节点数量减去 2 
    // 这里的 2 指两个节点，一个是 myself 节点（也即是发送信息的这个节点）
    // 另一个是接受 gossip 信息的节点
    // TODO: 为什么要减去结构 gossip 信息的节点？
    // TODO: 这个 gossip 发送的对象是什么？为什么要给他们发送？
    int freshnodes = dictSize(server.cluster->nodes)-2; // 还有多少个已知 node 可以发送出去（减 2，一个是自己，一个是对端）

    // 如果发送的信息是 PING ，那么更新最后一次发送 PING 命令的时间戳
    if (link->node && type == CLUSTERMSG_TYPE_PING)
        link->node->ping_sent = mstime();

    // 将当前节点的信息（比如名字、地址、端口号、负责处理的槽）记录到消息里面
    // 无论是 salve 还是 master 都会把自己的 node 信息在这里发送出去
    clusterBuildMessageHdr(hdr,type);

    /* Populate the gossip fields */
    // 从当前节点已知的节点中**随机**选出两个节点
    // 并通过这条消息捎带给目标节点，从而实现 gossip 协议

    // 每个节点有 freshnodes 次发送 gossip 信息的机会
    // 每次向目标节点发送 2 个被选中节点的 gossip 信息（gossipcount 计数）
    while(freshnodes > 0 && gossipcount < 3) {
        // 从 nodes 字典中随机选出一个节点（被选中节点），可能会选中自己，甚至是对方
        dictEntry *de = dictGetRandomKey(server.cluster->nodes);
        clusterNode *this = dictGetVal(de);

        clusterMsgDataGossip *gossip;
        int j;

        /* In the gossip section don't include:
         * 以下节点不能作为被选中节点：
         * 1) Myself.
         *    节点本身。
         * 2) Nodes in HANDSHAKE state.
         *    处于 HANDSHAKE 状态的节点。
         * 3) Nodes with the NOADDR flag set.
         *    带有 NOADDR 标识的节点
         * 4) Disconnected nodes if they don't have configured slots.
         *    因为不处理任何槽而被断开连接的节点 
         */
        if (this == myself ||
            this->flags & (REDIS_NODE_HANDSHAKE|REDIS_NODE_NOADDR) ||
            (this->link == NULL/*链接暂时断开了*/ && this->numslots == 0))
        {
                freshnodes--; /* otherwise we may loop forever. */
                continue;
        }

        /* Check if we already added this node */
        // 检查被选中节点是否已经在 hdr->data.ping.gossip 数组里面
        // 如果是的话说明这个节点之前已经被选中了
        // 不要再选中它（否则就会出现重复）
        for (j = 0; j < gossipcount; j++) {
            if (memcmp(hdr->data.ping.gossip[j].nodename,this->name,
                    REDIS_CLUSTER_NAMELEN) == 0) break;
        }
        if (j != gossipcount) continue;

        /* Add it */

        // 这个被选中节点有效，计数器减一
        freshnodes--;

        // 指向 gossip 信息结构
        gossip = &(hdr->data.ping.gossip[gossipcount]);

        // 将被选中节点的名字记录到 gossip 信息
        memcpy(gossip->nodename,this->name,REDIS_CLUSTER_NAMELEN);
        // 将被选中节点的 PING 命令发送时间戳记录到 gossip 信息
        gossip->ping_sent = htonl(this->ping_sent);
        // 将被选中节点的 PING 命令回复的时间戳记录到 gossip 信息
        gossip->pong_received = htonl(this->pong_received);
        // 将被选中节点的 IP 记录到 gossip 信息
        memcpy(gossip->ip,this->ip,sizeof(this->ip));
        // 将被选中节点的端口号记录到 gossip 信息
        gossip->port = htons(this->port);
        // 将被选中节点的标识值记录到 gossip 信息
        gossip->flags = htons(this->flags);

        // 这个被选中节点有效，计数器增一
        gossipcount++;
    }

    // 计算信息长度
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip)*gossipcount);
    // 将被选中节点的数量（gossip 信息中包含了多少个节点的信息）
    // 记录在 count 属性里面
    hdr->count = htons(gossipcount);
    // 将信息的长度记录到信息里面
    hdr->totlen = htonl(totlen);

    // 发送信息
    clusterSendMessage(link,buf,totlen);
}

/* Send a PONG packet to every connected node that's not in handshake state
 * and for which we have a valid link.
 *
 * 向所有未在 HANDSHAKE 状态，并且连接正常的节点发送 PONG 回复。
 *
 * In Redis Cluster pongs are not used just for failure detection, but also
 * to carry important configuration information. So broadcasting a pong is
 * useful when something changes in the configuration and we want to make
 * the cluster aware ASAP (for instance after a slave promotion). *
 * 在集群中， PONG 不仅可以用来检测节点状态，
 * 还可以携带一些重要的信息。
 *
 * 因此广播 PONG 回复在配置发生变化（比如从节点转变为主节点），
 * 并且当前节点想让其他节点尽快知悉这一变化的时候，
 * 就会广播 PONG 回复。
 *
 * The 'target' argument specifies the receiving instances using the
 * defines below:
 *
 * CLUSTER_BROADCAST_ALL -> All known instances.
 * CLUSTER_BROADCAST_LOCAL_SLAVES -> All slaves in my master-slaves ring.
 */
#define CLUSTER_BROADCAST_ALL 0
#define CLUSTER_BROADCAST_LOCAL_SLAVES 1
void clusterBroadcastPong(int target) {
    dictIterator *di;
    dictEntry *de;

    // 遍历所有节点
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        // 不向未建立连接的节点发送
        if (!node->link) continue;
        if (node == myself || nodeInHandshake(node)) continue;
        if (target == CLUSTER_BROADCAST_LOCAL_SLAVES) {
            int local_slave =
                nodeIsSlave(node) && node->slaveof &&
                (node->slaveof == myself || node->slaveof == myself->slaveof);
            if (!local_slave) continue;
        }
        // 发送 PONG 信息
        clusterSendPing(node->link,CLUSTERMSG_TYPE_PONG);
    }
    dictReleaseIterator(di);
}

/* Send a PUBLISH message.
 *
 * 发送一条 PUBLISH 消息。
 *
 * If link is NULL, then the message is broadcasted to the whole cluster. 
 *
 * 如果 link 参数为 NULL ，那么将消息广播给整个集群。
 */
void clusterSendPublish(clusterLink *link, robj *channel, robj *message) {
    unsigned char buf[sizeof(clusterMsg)], *payload;
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;
    uint32_t channel_len, message_len;

    // 频道
    channel = getDecodedObject(channel);

    // 消息
    message = getDecodedObject(message);

    // 频道和消息的长度
    channel_len = sdslen(channel->ptr);
    message_len = sdslen(message->ptr);

    // 构建消息
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_PUBLISH);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += sizeof(clusterMsgDataPublish) + channel_len + message_len;

    hdr->data.publish.msg.channel_len = htonl(channel_len);
    hdr->data.publish.msg.message_len = htonl(message_len);
    hdr->totlen = htonl(totlen);

    /* Try to use the local buffer if possible */
    if (totlen < sizeof(buf)) {
        payload = buf;
    } else {
        payload = zmalloc(totlen);
        memcpy(payload,hdr,sizeof(*hdr));
        hdr = (clusterMsg*) payload;
    }

    // 保存频道和消息到消息结构中
    memcpy(hdr->data.publish.msg.bulk_data,channel->ptr,sdslen(channel->ptr));
    memcpy(hdr->data.publish.msg.bulk_data+sdslen(channel->ptr),
        message->ptr,sdslen(message->ptr));

    // 选择发送到节点还是广播至整个集群
    if (link)
        clusterSendMessage(link,payload,totlen);
    else
        clusterBroadcastMessage(payload,totlen);

    decrRefCount(channel);
    decrRefCount(message);
    if (payload != buf) zfree(payload);
}

/* Send a FAIL message to all the nodes we are able to contact.
 *
 * 向当前节点已知的所有节点发送 FAIL 信息。
 *
 * The FAIL message is sent when we detect that a node is failing
 * (REDIS_NODE_PFAIL) and we also receive a gossip confirmation of this:
 * we switch the node state to REDIS_NODE_FAIL and ask all the other
 * nodes to do the same ASAP. 
 *
 * 如果当前节点将 node 标记为 PFAIL 状态，
 * 并且通过 gossip 协议，
 * 从足够数量的节点那些得到了 node 已经下线的支持， 
 * 那么当前节点会将 node 标记为 FAIL ，
 * 并执行这个函数，向其他 node 发送 FAIL 消息，
 * 要求它们也将 node 标记为 FAIL 。
 */
void clusterSendFail(char *nodename) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;

    // 创建下线消息
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAIL);

    // 记录命令
    memcpy(hdr->data.fail.about.nodename,nodename,REDIS_CLUSTER_NAMELEN);

    // 广播消息
    clusterBroadcastMessage(buf,ntohl(hdr->totlen));
}

/* Send an UPDATE message to the specified link carrying the specified 'node'
 * slots configuration. The node name, slots bitmap, and configEpoch info
 * are included. 
 *
 * 向连接 link 发送包含给定 node 槽配置的 UPDATE 消息，
 * 包括节点名称，槽位图，以及配置纪元。
 */
void clusterSendUpdate(clusterLink *link, clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;

    if (link == NULL) return;

    // 创建消息
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_UPDATE);

    // 设置节点名
    memcpy(hdr->data.update.nodecfg.nodename,node->name,REDIS_CLUSTER_NAMELEN);

    // 设置配置纪元
    hdr->data.update.nodecfg.configEpoch = htonu64(node->configEpoch);

    // 更新节点的槽位图
    memcpy(hdr->data.update.nodecfg.slots,node->slots,sizeof(node->slots));

    // 发送信息
    clusterSendMessage(link,buf,ntohl(hdr->totlen));
}

/* -----------------------------------------------------------------------------
 * CLUSTER Pub/Sub support
 *
 * For now we do very little, just propagating PUBLISH messages across the whole
 * cluster. In the future we'll try to get smarter and avoiding propagating those
 * messages to hosts without receives for a given channel.
 * -------------------------------------------------------------------------- */
// 向整个集群的 channel 频道中广播消息 messages
void clusterPropagatePublish(robj *channel, robj *message) {
    clusterSendPublish(NULL, channel, message);
}

/* -----------------------------------------------------------------------------
 * SLAVE node specific functions
 * -------------------------------------------------------------------------- */

/* This function sends a FAILOVE_AUTH_REQUEST message to every node in order to
 * see if there is the quorum for this slave instance to failover its failing
 * master.
 *
 * 向其他所有节点发送 FAILOVE_AUTH_REQUEST 信息，
 * 看它们是否同意由这个从节点来对下线的主节点进行故障转移。
 *
 * Note that we send the failover request to everybody, master and slave nodes,
 * but only the masters are supposed to reply to our query. 
 *
 * 信息会被发送给所有节点，包括主节点和从节点，但只有主节点会回复这条信息。 
 */
// 只会在 automatic failover 中被使用
void clusterRequestFailoverAuth(void) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    // 设置信息头（包含当前节点的信息）
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST);
    /* If this is a manual failover, set the CLUSTERMSG_FLAG0_FORCEACK bit
     * in the header to communicate the nodes receiving the message that
     * they should authorized the failover even if the master is working. */
    if (server.cluster->mf_end) hdr->mflags[0] |= CLUSTERMSG_FLAG0_FORCEACK;
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);

    // 发送信息
    clusterBroadcastMessage(buf,totlen);
}

/* Send a FAILOVER_AUTH_ACK message to the specified node. */
// 向节点 node 投票，支持它进行故障迁移
// 拥有投票权的 master 向相应的 slave 发出
void clusterSendFailoverAuth(clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->link) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    clusterSendMessage(node->link,buf,totlen);
}

/* Send a MFSTART message to the specified node. */
// 向给定的节点发送一条 MFSTART 消息
// slave 通过这个函数，向自己的 master node 发送 CLUSTERMSG_TYPE_MFSTART
void clusterSendMFStart(clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->link) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_MFSTART);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    clusterSendMessage(node->link,buf,totlen);
}

/* Vote for the node asking for our vote if there are the conditions. */
// 在条件满足的情况下，为请求进行故障转移的节点 node 进行投票，支持它进行故障转移
// 只有 master 身份的 node 拥有投票权
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request) {

    // 请求节点的主节点
    clusterNode *master = node->slaveof;

    // 请求节点的当前配置纪元
    uint64_t requestCurrentEpoch = ntohu64(request->currentEpoch);

    // 请求节点想要获得投票的纪元
    uint64_t requestConfigEpoch = ntohu64(request->configEpoch);

    // 请求节点的槽布局
    unsigned char *claimed_slots = request->myslots;
    int force_ack = request->mflags[0] & CLUSTERMSG_FLAG0_FORCEACK;
    int j;

    /* IF we are not a master serving at least 1 slot, we don't have the
     * right to vote, as the cluster size in Redis Cluster is the number
     * of masters serving at least one slot, and quorum is the cluster
     * size + 1 */

    // 如果节点为从节点，或者是一个没有处理任何槽的主节点，
    // 那么它没有投票权
    if (nodeIsSlave(myself) || myself->numslots == 0) return;

    // server.cluster->currentEpoch 在 clusterProcessPacket() 的时候，已经更新过了
    /* Request epoch must be >= our currentEpoch. */
    // 请求的配置纪元必须大于等于当前节点的配置纪元
    if (requestCurrentEpoch < server.cluster->currentEpoch) return;

    /* I already voted for this epoch? Return ASAP. */
    // 已经投过票了
    if (server.cluster->lastVoteEpoch == server.cluster->currentEpoch) return;

    /* Node must be a slave and its master down.
     * The master can be non failing if the request is flagged
     * with CLUSTERMSG_FLAG0_FORCEACK (manual failover). */
    if (nodeIsMaster(node) || master == NULL ||
        (!nodeFailed(master) && !force_ack)) return;

    /* We did not voted for a slave about this master for two
     * times the node timeout. This is not strictly needed for correctness
     * of the algorithm but makes the base case more linear. */
    // 如果之前一段时间已经对请求节点进行过投票，那么不进行投票
    // 避免弄出两个 master。加速收敛（不然短时间内有多个 slave 发起投票，你要到啥时候才收敛出一个 master 呀）
    if (mstime() - node->slaveof->voted_time < server.cluster_node_timeout * 2)
        return;

    /* The slave requesting the vote must have a configEpoch for the claimed
     * slots that is >= the one of the masters currently serving the same
     * slots in the current configuration. */
    for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {

        // 跳过未指派节点
        if (bitmapTestBit(claimed_slots, j) == 0) continue;

        // 查找是否有某个槽的配置纪元大于节点请求的纪元
        if (server.cluster->slots[j] == NULL || // 这个 slots 没有被 assign（通常不会的）
            server.cluster->slots[j]->configEpoch <= requestConfigEpoch)
        {
            continue;
        }
        /*
        else
        {
            通常不会的，毕竟 slot 一般是不会动的，动了之后，基本会整个集群达成一致的
            case1: 作为 sender 的 salve，保存了过于陈旧的 slot 信息，不能选
            case2: 这个 master 对于 slot 的信息太旧了，出现了误判
        }
        */

        // 如果有的话，说明节点请求的纪元已经过期，没有必要进行投票
        /* If we reached this point we found a slot that in our current slots
         * is served by a master with a greater configEpoch than the one claimed
         * by the slave requesting our vote. Refuse to vote for this slave. */
        return;
    }

    /* We can vote for this slave. */
    // 为节点投票
    clusterSendFailoverAuth(node);
    // 更新时间值
    server.cluster->lastVoteEpoch = server.cluster->currentEpoch;
    node->slaveof->voted_time = mstime();
}

/* This function returns the "rank" of this instance, a slave, in the context
 * of its master-slaves ring. The rank of the slave is given by the number of
 * other slaves for the same master that have a better replication offset
 * compared to the local one (better means, greater, so they claim more data).
 *
 * A slave with rank 0 is the one with the greatest (most up to date)
 * replication offset, and so forth. Note that because how the rank is computed
 * multiple slaves may have the same rank, in case they have the same offset.
 *
 * The slave rank is used to add a delay to start an election in order to
 * get voted and replace a failing master. Slaves with better replication
 * offsets are more likely to win. */
// 根据 repl 的 offset 来得出 slave 的 rank（尽可能选取 data 跟原 master 相同的 slave）
// 这个排名自然是有点误差的，因为是利用本 node 中，通过 Gossip 获取的其他 slave node repl_offset
int clusterGetSlaveRank(void) {
    long long myoffset;
    int j, rank = 0;
    clusterNode *master;

    redisAssert(nodeIsSlave(myself));
    master = myself->slaveof;
    if (master == NULL) return 0; /* Never called by slaves without master. */

    myoffset = replicationGetSlaveOffset();
    for (j = 0; j < master->numslaves; j++)
        if (master->slaves[j] != myself &&
            master->slaves[j]->repl_offset > myoffset) rank++;
    return rank;
}

/* This function is called if we are a slave node and our master serving
 * a non-zero amount of hash slots is in FAIL state.
 *
 * 如果当前节点是一个从节点，并且它正在复制的一个负责非零个槽的主节点处于 FAIL 状态，
 * 那么执行这个函数。
 *
 * The gaol of this function is:
 *
 * 这个函数有三个目标：
 *
 * 1) To check if we are able to perform a failover, is our data updated?
 *    检查是否可以对主节点执行一次故障转移，节点的关于主节点的信息是否准确和最新（updated）？
 * 2) Try to get elected by masters.
 *    在这么多个 slave 中，选出新的 master
 * 3) Perform the failover informing all the other nodes.
 *    执行故障转移，并通知其他节点
 */
// automatic 的过程逻辑跟 manual 的逻辑都混在这里了，不太好
// 无论是 manual failover，还是自动的 failover，都是走这个函数的
void clusterHandleSlaveFailover(void) {
    mstime_t data_age;  // data_agr = the number of seconds we are disconnected from the master.
    mstime_t auth_age = mstime() - server.cluster->failover_auth_time;  // 一开始是 0
    int needed_quorum = (server.cluster->size / 2) + 1;
    int manual_failover = server.cluster->mf_end != 0 &&
                          server.cluster->mf_can_start;
    int j;
    mstime_t auth_timeout, auth_retry_time;

    // TODO:(DONE) 这个 CLUSTER_TODO_HANDLE_FAILOVER 跟 manual failover 有关系吗？
    // 没有关系，CLUSTER_TODO_HANDLE_FAILOVER 的设置，就是想要在 sleep 之前调用这个函数，所以这里去掉相应的标志位
    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_HANDLE_FAILOVER;

    /* Compute the failover timeout (the max time we have to send votes
     * and wait for replies), and the failover retry time (the time to wait
     * before waiting again.
     *
     * Timeout is MIN(NODE_TIMEOUT*2,2000) milliseconds.
     * Retry is two times the Timeout.
     */
    auth_timeout = server.cluster_node_timeout*2;
    if (auth_timeout < 2000) auth_timeout = 2000;
    auth_retry_time = auth_timeout*2;

    /* Pre conditions to run the function, that must be met both in case
     * of an automatic or manual failover:
     * 1) We are a slave.
     * 2) Our master is flagged as FAIL, or this is a manual failover.
     * 3) It is serving slots. */
    // NOTE: 通常情况下，在这里进行常规检查，没啥事的话，直接这里就 return 了
    // 其他 slave 成功升级为 master 之后，myself->slaveof 将会恢复，而且是 UP 的状态，
    // 所以其他 slave 也是通过这里来停掉自己的 slave 选举发动过程的
    if (nodeIsMaster(myself) ||
        myself->slaveof == NULL ||
        (!nodeFailed(myself->slaveof) && !manual_failover) ||
        myself->slaveof->numslots == 0) return;

    /* Set data_age to the number of seconds we are disconnected from
     * the master. */
    // 将 data_age 设置为从节点与主节点的断开秒数
    if (server.repl_state == REDIS_REPL_CONNECTED) {
        data_age = (mstime_t)(server.unixtime - server.master->lastinteraction)
                   * 1000;
    } else {
        data_age = (mstime_t)(server.unixtime - server.repl_down_since) * 1000;
    }

    /* Remove the node timeout from the data_age as it is fine that we are
     * disconnected from our master at least for the time it was down to be
     * flagged as FAIL, that's the baseline. */
    // node timeout 的时间不计入断线时间之内 TODO:(DONE) why? timeout 之后，才被认为是 disconnect
    if (data_age > server.cluster_node_timeout)
        data_age -= server.cluster_node_timeout;

    /* Check if our data is recent enough. For now we just use a fixed
     * constant of ten times the node timeout since the cluster should
     * react much faster to a master down.
     *
     * Check bypassed for manual failovers. */
    // 检查这个从节点的数据是否较新：
    // 目前的检测办法是断线时间不能超过 node timeout 的十倍
    if (data_age >
        ((mstime_t)server.repl_ping_slave_period * 1000) +
        (server.cluster_node_timeout * REDIS_CLUSTER_SLAVE_VALIDITY_MULT))
    {
        if (!manual_failover) {
            redisLog(REDIS_WARNING, "====>  data too old, return");
            return;   // data 有点旧了，所以不采用这个 slave TODO: 要是全部 slave 都太旧了，怎么办
        }
    }

    /* If the previous failover attempt timedout and the retry time has
     * elapsed, we can setup a new one. */
    // NOTE: automatic round 1: 计算出即将发起 failover author 的投票开始时间
    if (auth_age > auth_retry_time) {
        // 基本 delay，留出时间，让其他 slave node 也知晓 master DONW 的事情
        // 这样其他 slave 才能够各自准备发起 automatic failover
        // 必须每个 slave 都会发起（但是需要错峰），这样可以有效的缩短 new master 的选举，
        // 加快 automatic failover 的过程
        server.cluster->failover_auth_time = mstime() +
            500 + /* Fixed delay of 500 milliseconds, let FAIL msg propagate. */
            random() % 500; /* Random delay between 0 and 500 milliseconds. */

        // 根据不同的 slave repl_offset 情况，叠加不同的 delay，鼓励跟原 master 更接近的 slave 成为新 master
        server.cluster->failover_auth_count = 0;
        server.cluster->failover_auth_sent = 0;
        server.cluster->failover_auth_rank = clusterGetSlaveRank();
        /* We add another delay that is proportional to the slave rank.
         * Specifically 1 second * rank. This way slaves that have a probably
         * less updated replication offset, are penalized. */
        server.cluster->failover_auth_time +=
            server.cluster->failover_auth_rank * 1000;

        /* （manual failover 不需要这些花里胡巧的 delay）However if this is a manual failover, no delay is needed. */
        if (server.cluster->mf_end) {
            server.cluster->failover_auth_time = mstime();
            server.cluster->failover_auth_rank = 0;
        }

        redisLog(REDIS_WARNING,
            "Start of election delayed for %lld milliseconds "
            "(rank #%d, offset %lld).",
            server.cluster->failover_auth_time - mstime(),
            server.cluster->failover_auth_rank,
            replicationGetSlaveOffset());

        /* Now that we have a scheduled election, broadcast our offset
         * to all the other slaves so that they'll updated their offsets
         * if our offset is better. */
        clusterBroadcastPong(CLUSTER_BROADCAST_LOCAL_SLAVES);
        redisLog(REDIS_WARNING, "====>  issue setup a new failover attempt, return");
        return;
    }

    /* It is possible that we received more updated offsets from other
     * slaves for the same master since we computed our election delay.
     * Update the delay if our rank changed.
     *
     * Not performed if this is a manual failover. */
    if (server.cluster->failover_auth_sent == 0 &&  // 还没开始 failover 之前，还可以变更
        server.cluster->mf_end == 0)                // manual failover 不需要计算 delay
    {
        // just a check and update
        int newrank = clusterGetSlaveRank();
        if (newrank > server.cluster->failover_auth_rank) {
            long long added_delay =
                (newrank - server.cluster->failover_auth_rank) * 1000;
            server.cluster->failover_auth_time += added_delay;
            server.cluster->failover_auth_rank = newrank;
            redisLog(REDIS_WARNING,
                "Slave rank updated to #%d, added %lld milliseconds of delay.",
                newrank, added_delay);
        }
    }

    /* Return ASAP if we can't still start the election. */
    // 如果执行故障转移的时间未到，先返回
    // NOTE: automatic round 2 to n: 等待同一个 master 的其他 slave，知晓并达成一致：master DOWN，开始相应的 failover_auth_time 设置
    if (mstime() < server.cluster->failover_auth_time) {
        redisLog(REDIS_WARNING, "====>  we can't still start the election, return");
        return;
    }

    /* Return ASAP if the election is too old to be valid. */
    // 如果距离应该执行故障转移的时间已经过了很久
    // 那么不应该再执行故障转移了（因为可能已经没有需要了）
    // 直接返回
    if (auth_age > auth_timeout) {
        redisLog(REDIS_WARNING, "====>  the election is too old to be valid., return");
        return;
    }

    /* Ask for votes if needed. */
    // NOTE: automatic round n + 1: 本 node 估摸着自己可以开始 automatic failover 的 new master 竞选了；发起 author 请求
    // 向其他节点发送故障转移请求
    if (server.cluster->failover_auth_sent == 0) {

        // 增加配置纪元
        server.cluster->currentEpoch++;

        // 记录发起故障转移的配置纪元
        server.cluster->failover_auth_epoch = server.cluster->currentEpoch;

        redisLog(REDIS_WARNING,"Starting a failover election for epoch %llu.",
            (unsigned long long) server.cluster->currentEpoch);

        // 向其他所有节点发送信息，看它们是否支持由本节点来对下线主节点进行故障转移
        // 只有 master 身份的 node 拥有投票权。其他所有的 slave，都不能投票。
        // needed_quorum 的计算，是根据 server.cluster->size 来得出的！！！
        // 再说了，slave 觉得你正常有啥用？其他即将跟你一起干活的 master 觉得你正常，才是对整个 cluster 更有用的
        clusterRequestFailoverAuth();

        // 打开标识，表示已发送信息
        server.cluster->failover_auth_sent = 1;

        // TODO:
        // 在进入下个事件循环之前，执行：
        // 1）保存配置文件
        // 2）更新节点状态
        // 3）同步配置
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
        redisLog(REDIS_WARNING, "====>  Ask for votes if needed, return");
        return; /* Wait for replies. */
    }

    /* Check if we reached the quorum. */
    // 第一次是进上面的 if-branch 然后 return。第二次是进来这个 if-branch，看看是否收集到足够多的投票
    // 如果当前节点获得了足够多的投票，那么对下线主节点进行故障转移
    // NOTE: automatic round n + 2: 通过 CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST，本 node 已经收集到了足够多的选票，可以成为 master
    if (server.cluster->failover_auth_count >= needed_quorum) {
        // 旧主节点
        clusterNode *oldmaster = myself->slaveof;

        redisLog(REDIS_WARNING,
            "Failover election won: I'm the new master.");

        /* We have the quorum, perform all the steps to correctly promote
         * this slave to a master.
         *
         * 1) Turn this node into a master. 
         *    将当前节点的身份由从节点改为主节点
         */
        clusterSetNodeAsMaster(myself);
        // 让从节点取消复制，成为新的主节点
        replicationUnsetMaster();

        /* 2) Claim all the slots assigned to our master. */
        // 接收所有主节点负责处理的槽
        for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
            if (clusterNodeGetSlotBit(oldmaster,j)) {
                // 将槽设置为未分配的
                clusterDelSlot(j);
                // 将槽的负责人设置为当前节点
                clusterAddSlot(myself,j);
            }
        }

        /* 3) Update my configEpoch to the epoch of the election. */
        // 更新集群配置纪元
        myself->configEpoch = server.cluster->failover_auth_epoch;

        /* 4) Update state and save config. */
        // 更新节点状态
        clusterUpdateState();
        // 并保存配置文件
        clusterSaveConfigOrDie(1);

        /* 5) Pong all the other nodes so that they can update the state
         *    accordingly and detect that we switched to master role. */
        // 向所有节点发送 PONG 信息
        // 让它们可以知道当前节点已经升级为主节点了
        clusterBroadcastPong(CLUSTER_BROADCAST_ALL);

        /* 6) If there was a manual failover in progress, clear the state. */
        // 如果有手动故障转移正在执行，那么清理和它有关的状态
        resetManualFailover();
        // NOTE: automatic round n + 3: automatic failover 结束
    }
    redisLog(REDIS_WARNING, "====>  just return");
}

/* -----------------------------------------------------------------------------
 * CLUSTER slave migration（replicas migration，通过调整 slave 的 layout，能够持续应对接连发生的 failover ）
 * 
 * Slave migration is the process that allows a slave of a master that is
 * already covered by at least another slave, to "migrate" to a master that
 * is orpaned, that is, left with no working slaves.
 * 
 * High Availability 的另一种途径：
 * Moreover using replicas migration, masters no longer replicated by any slave will 
 * receive one from a master which is covered by multiple slaves.
 * -------------------------------------------------------------------------- */

/**
 * Thanks to a Redis Cluster feature called replicas migration 
 * the Cluster availability is improved in many real world scenarios by the fact that 
 * replicas migrate to orphaned masters (masters no longer having replicas). 
 * So at every successful failure event, the cluster may reconfigure the slaves layout 
 * in order to better resist the next failure.
 * 
 * 每一个 every successful failure event 之后，都会看看要不要重新调整 slave 的分布
 * in order to better resist the next failure.
*/

/* This function is responsible to decide if this replica should be migrated
 * to a different (orphaned) master. It is called by the clusterCron() function
 * only if:
 *
 * 1) We(myself node) are a slave node.
 * 2) It was detected that there is at least one orphaned master in
 *    the cluster.
 * 3) We are a slave of one of the masters with the greatest number of
 *    slaves.
 *
 * This checks are performed by the caller since it requires to iterate
 * the nodes anyway, so we spend time into clusterHandleSlaveMigration()
 * if definitely needed.
 *
 * The fuction is called with a pre-computed max_slaves, that is the max
 * number of working (not in FAIL state) slaves for a single master.
 *
 * Additional conditions for migration are examined inside the function.
 */
// TODO:(DONE) 怎么避免多个 slave 同时转向同一个 orphaned master ？
// 没有任何办法，看 step 3。真发生了这种情况，无伤大雅，cluster_migration_barrier 配置项可以控制这种情况的危害
// 而且，master 是 cluster 里面拥有最多 slave 的 master，危害程度就更低了
void clusterHandleSlaveMigration(int max_slaves) {
    int j, okslaves = 0;
    clusterNode *mymaster = myself->slaveof, *target = NULL, *candidate = NULL;
    dictIterator *di;
    dictEntry *de;

    /* Step 1: Don't migrate if the cluster state is not ok. */
    if (server.cluster->state != REDIS_CLUSTER_OK) return;

    /* Step 2: Don't migrate if my master will not be left with at least
     *         'migration-barrier' slaves after my migration. */
    if (mymaster == NULL) return;
    for (j = 0; j < mymaster->numslaves; j++)
        if (!nodeFailed(mymaster->slaves[j]) &&
            !nodeTimedOut(mymaster->slaves[j])) okslaves++;     // 统计 myself->slaveof 中有多少的 salve 可用
    if (okslaves <= server.cluster_migration_barrier) return;   // slave 少于配置的要求。不进行 slave 迁移

    /* Step 3: Idenitfy a candidate(候选人) for migration, and check if among the
     * masters with the greatest number of ok slaves, I'm the one with the
     * smaller node ID.
     *
     * Note that this means that eventually a replica migration will occurr
     * since slaves that are reachable again always have their FAIL flag
     * cleared. At the same time this does not mean that there are no
     * race conditions possible (two slaves migrating at the same time), but
     * this is extremely unlikely to happen, and harmless. */
    // candidate 究竟是哪个 slave 迁出；target 这个 slave 向哪个 master 迁移；
    candidate = myself;
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        int okslaves;

        /* Only iterate over working masters. */
        if (nodeIsSlave(node) || nodeFailed(node)) continue;
        okslaves = clusterCountNonFailingSlaves(node);

        if (okslaves == 0 && target == NULL && node->numslots > 0)
            target = node;

        if (okslaves == max_slaves) {
            for (j = 0; j < node->numslaves; j++) {
                if (memcmp(node->slaves[j]->name,   // 确保同一个 master 上面，只会有一个 slave 主动转向其他 node
                           candidate->name,
                           REDIS_CLUSTER_NAMELEN) < 0)
                {
                    candidate = node->slaves[j];
                }
            }
        }
    }

    /* Step 4: perform the migration if there is a target, and if I'm the
     * candidate. */
    if (target && candidate == myself) {
        redisLog(REDIS_WARNING,"Migrating to orphaned master %.40s",
            target->name);
        clusterSetMaster(target);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER manual failover
 *
 * This are the important steps performed by slaves during a manual failover:
 * 1) User send CLUSTER FAILOVER command. The failover state is initialized
 *    setting mf_end to the millisecond unix time at which we'll abort the
 *    attempt.
 * 2) Slave sends a MFSTART message to the master requesting to pause clients
 *    for two times the manual failover timeout REDIS_CLUSTER_MF_TIMEOUT.
 *    When master is paused for manual failover, it also starts to flag
 *    packets with CLUSTERMSG_FLAG0_PAUSED.
 * 3) Slave waits for master to send its replication offset flagged as PAUSED.
 * 4) If slave received the offset from the master, and its offset matches,
 *    mf_can_start is set to 1, and clusterHandleSlaveFailover() will perform
 *    the failover as usually, with the difference that the vote request
 *    will be modified to force masters to vote for a slave that has a
 *    working master.
 *
 * From the point of view of the master things are simpler: when a
 * PAUSE_CLIENTS packet is received the master sets mf_end as well and
 * the sender in mf_slave. During the time limit for the manual failover
 * the master will just send PINGs more often to this slave, flagged with
 * the PAUSED flag, so that the slave will set mf_master_offset when receiving
 * a packet from the master with this flag set.
 *
 * The gaol of the manual failover is to perform a fast failover without
 * data loss due to the asynchronous master-slave replication.
 * -------------------------------------------------------------------------- */

/* Reset the manual failover state. This works for both masters and slavesa
 * as all the state about manual failover is cleared.
 *
 * 重置与手动故障转移有关的状态，主节点和从节点都可以使用。
 *
 * The function can be used both to initialize the manual failover state at
 * startup or to abort a manual failover in progress. 
 * 这个函数既可以用于在启动集群时进行初始化，
 * 又可以实际地应用在手动故障转移的情况。
 */
void resetManualFailover(void) {
    if (server.cluster->mf_end && clientsArePaused()) {
        server.clients_pause_end_time = 0;
        clientsArePaused(); /* Just use the side effect of the function. */
    }
    server.cluster->mf_end = 0; /* No manual failover in progress. */
    server.cluster->mf_can_start = 0;
    server.cluster->mf_slave = NULL;
    server.cluster->mf_master_offset = 0;
}

/* If a manual failover timed out, abort it. */
void manualFailoverCheckTimeout(void) {
    if (server.cluster->mf_end && server.cluster->mf_end < mstime()) {
        redisLog(REDIS_WARNING,"Manual failover timed out.");
        resetManualFailover();
    }
}

/* This function is called from the cluster cron function in order to go
 * forward with a manual failover state machine. */
void clusterHandleManualFailover(void) {
    /* Return ASAP if no manual failover is in progress. */
    if (server.cluster->mf_end == 0) return;

    /* If mf_can_start is non-zero, the failover was alrady triggered so the
     * next steps are performed by clusterHandleSlaveFailover(). */
    if (server.cluster->mf_can_start) return;

    if (server.cluster->mf_master_offset == 0) return; /* Wait for offset... */

    if (server.cluster->mf_master_offset == replicationGetSlaveOffset()) {
        // 当不满足的时候，是 slave 的 offset 不断增长，然后 reach master 的 offset，再开始切换
        /* Our replication offset matches the master replication offset
         * announced after clients were paused. We can start the failover. */
        server.cluster->mf_can_start = 1;
        redisLog(REDIS_WARNING,
            "All master replication stream processed, "
            "manual failover can start.");
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER cron job
 * -------------------------------------------------------------------------- */

/* This is executed 10 times every second */
// cluster 常规操作函数，默认每秒执行 10 次（每间隔 100 毫秒执行一次）
void clusterCron(void) {
    dictIterator *di;
    dictEntry *de;
    int update_state = 0;
    int orphaned_masters; /* How many masters there are without ok slaves. */
    int max_slaves; /* Max number of ok slaves for a single master. */
    int this_slaves; /* Number of ok slaves for our master (if we are slave). */
    mstime_t min_pong = 0, now = mstime();
    clusterNode *min_pong_node = NULL;
    // 迭代计数器，一个静态变量
    static unsigned long long iteration = 0;
    mstime_t handshake_timeout;

    // 记录一次迭代
    iteration++; /* Number of times this function was called so far. */

    /* The handshake timeout is the time after which a handshake node that was
     * not turned into a normal node is removed from the nodes. Usually it is
     * just the NODE_TIMEOUT value, but when NODE_TIMEOUT is too small we use
     * the value of 1 second. */
    // 如果一个 handshake 节点没有在 handshake_timeout 内
    // 转换成普通节点（normal node）， TODO:(DONE) handshake node 跟 normal node 有什么区别？
    // 那么节点会从 nodes 表中移除这个 handshake 节点
    // 一般来说 handshake_timeout 的值总是等于 NODE_TIMEOUT
    // 不过如果 NODE_TIMEOUT 太少的话，程序会将值设为 1 秒钟
    handshake_timeout = server.cluster_node_timeout;
    if (handshake_timeout < 1000) handshake_timeout = 1000;

    /* Check if we have disconnected nodes and re-establish the connection. */
    // 向集群中的所有断线或者未连接节点发送消息
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        // 跳过当前节点以及没有地址的节点
        if (node->flags & (REDIS_NODE_MYSELF|REDIS_NODE_NOADDR)) continue;

        /* A Node in HANDSHAKE state has a limited lifespan equal to the
         * configured node timeout. */
        // 如果 handshake 节点已超时，释放它
        if (nodeInHandshake(node) && now - node->ctime > handshake_timeout) {
            freeClusterNode(node);
            continue;
        }

        // 为未创建连接的节点创建连接
        // case 1: re-connect; case 2: init connect
        /**
         * This second high port(port 10000+) is used for the **Cluster bus**, that is a node-to-node communication channel using a binary protocol.
         * The Cluster bus is used by nodes for failure detection, configuration update, failover authorization and so forth. 
         * Clients should never try to communicate with the cluster bus port, but always with the normal Redis command port
         */
        if (node->link == NULL) {
            int fd;
            mstime_t old_ping_sent;
            clusterLink *link;

            fd = anetTcpNonBlockBindConnect(server.neterr, node->ip,
                node->port+REDIS_CLUSTER_PORT_INCR,
                    server.bindaddr_count ? server.bindaddr[0] : NULL);
            if (fd == -1) {
                redisLog(REDIS_DEBUG, "Unable to connect to "
                    "Cluster Node [%s]:%d -> %s", node->ip,
                    node->port+REDIS_CLUSTER_PORT_INCR,
                    server.neterr);
                continue;
            }
            link = createClusterLink(node);
            link->fd = fd;
            node->link = link;
            aeCreateFileEvent(server.el,link->fd,AE_READABLE,
                    clusterReadHandler,link);
            /* 这是在 re-connect 的情况下：
             * Queue a PING in the new connection ASAP: this is crucial
             * to avoid false positives in failure detection.
             * 1. 异步 connect；
             * 2. 创建 buf；
             * 3. 把 PING 放入 buf 里面，并监听 write-able 事件
             * 4. 对端 accept，等待接收第一条信息，并了解发起 connect 的 node 信息
             * 5. 异步 connect 完成，write 就绪，开始从 buf 里面取出内容，发送 PING
             *
             * 第一次连接的情况下：
             * If the node is flagged as MEET, we send a MEET message instead
             * of a PING one, to force the receiver to add us in its node
             * table. 
             * 1. 异步 connect；
             * 2. 创建 buf；
             * 3. 把 MEET 放入 buf 里面，并监听 write-able 事件
             * 4. 对端 accept，等待接收第一条信息，并了解发起 connect 的 node 信息
             * 5. 异步 connect 完成，write 就绪，开始从 buf 里面取出内容，发送 MEET
             * 
             * */
            // 向新连接的节点发送 PING 命令，防止节点被识进入下线
            // 如果节点被标记为 MEET ，那么发送 MEET 命令，否则发送 PING 命令
            old_ping_sent = node->ping_sent;
            clusterSendPing(link, node->flags & REDIS_NODE_MEET ?
                    CLUSTERMSG_TYPE_MEET : CLUSTERMSG_TYPE_PING);
            

            if (old_ping_sent) {
                /* If there was an active ping before the link was
                 * disconnected, we want to restore the ping time, otherwise
                 * replaced by the clusterSendPing() call. */
                // TODO:(DONE) 为什么要 reset node->ping_sent?
                // 因为 re-connect 的时候，clusterSendPing 里面会更新这个值，但是我并不想用 re-connect 的第一个 ping 来更新这个 ping_sent
                // 这是 re-connect 的时候才会进入的 if-branch，
                // ping_sent 想要记录的是: 在 disconnect 之前的 ping_sent 时间点，而不是这个尝试重连的 ping 时间点
                // 因为你压根不知道这个链路通不通，你记录刚刚的 ping 毫无意义
                // 上面之所以发送 ping，是用来检查 linux 觉得 connect 成功之后的 socket，是不是真的能用。
                // 而登记这个 ping_sent 是为了能够大概推算发生 node 失联的时间点
                node->ping_sent = old_ping_sent;
            }

            /* We can clear the flag after the first packet is sent.
             *
             * 在发送 MEET 信息之后，清除节点的 MEET 标识。
             *
             * If we'll never receive a PONG, we'll never send new packets
             * to this node. Instead after the PONG is received and we
             * are no longer in meet/handshake status, we want to send
             * normal PING packets. 
             *
             * 如果当前节点（发送者）没能收到 MEET 信息的回复，
             * 那么它将不再向目标节点发送命令。
             *
             * 如果接收到回复的话，那么节点将不再处于 HANDSHAKE 状态，
             * 并继续向目标节点发送普通 PING 命令。
             */
            // TODO:(DONE) 要是这个 MEET 丢失的话，怎么重传？
            // 丢了就丢了，晚点再 clusterCron 会直接 free 掉这个 link，然后再次 connect，发 MEET
            node->flags &= ~REDIS_NODE_MEET;

            redisLog(REDIS_DEBUG,"Connecting with Node %.40s at %s:%d, [link:%p]",
                    node->name, node->ip, node->port+REDIS_CLUSTER_PORT_INCR, (void*)link);
        }
    }
    dictReleaseIterator(di);

    /* Ping some random node 1 time every 10 iterations, so that we usually ping
     * one random node every second. */
    // clusterCron() 每执行 10 次（至少间隔一秒钟），就向一个随机节点发送 gossip 信息
    if (!(iteration % 10)) {
        int j;

        /* Check a few random nodes and ping the one with the oldest
         * pong_received time. */
        // 随机 5 个节点，选出其中一个
        for (j = 0; j < 5; j++) {

            // 随机在集群中挑选节点
            de = dictGetRandomKey(server.cluster->nodes);
            clusterNode *this = dictGetVal(de);

            /* Don't ping nodes disconnected or with a ping currently active. */
            // 不要 PING 连接断开的节点，也不要 PING 最近已经 PING 过的节点
            if (this->link == NULL || this->ping_sent != 0) continue;

            if (this->flags & (REDIS_NODE_MYSELF|REDIS_NODE_HANDSHAKE))
                continue;

            // 选出 5 个随机节点中最近一次接收 PONG 回复距离现在最旧的节点
            if (min_pong_node == NULL || min_pong > this->pong_received) {
                min_pong_node = this;
                min_pong = this->pong_received;
            }
        }

        // 向最久没有收到 PONG 回复的节点发送 PING 命令
        if (min_pong_node) {
            redisLog(REDIS_DEBUG,"Pinging node %.40s [link:%p, node:%p]", 
                     min_pong_node->name, min_pong_node->link, min_pong_node);
            clusterSendPing(min_pong_node->link, CLUSTERMSG_TYPE_PING);
        }
    }

    // 遍历所有节点，检查是否需要将某个节点标记为下线
    /* node FAIL\PFAIL check and mark */
    /* Iterate nodes to check if we need to flag something as failing.
     * This loop is also responsible to:
     * 1) Check if there are orphaned masters (masters without non failing
     *    slaves). 
     * TODO:(DONE) 有啥用？统计来干嘛？useful only if the current instance is a slave that may migrate to another master.
     * replicas migration
     * 2) Count the max number of non failing slaves for a single master.
     * 3) Count the number of slaves for our master, if we are a slave. */
    orphaned_masters = 0;
    max_slaves = 0;
    this_slaves = 0;
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        now = mstime(); /* Use an updated time at every iteration. */
        mstime_t delay;

        // 跳过节点本身、无地址节点、HANDSHAKE 状态的节点
        if (node->flags &
            (REDIS_NODE_MYSELF|REDIS_NODE_NOADDR|REDIS_NODE_HANDSHAKE))
                continue;

        // TODO:(DONE) 这是什么 case ？
        // TODO:(DONE) max_slaves, orphaned_masters 有什么用？
        /* Orphaned master check, useful only if the current instance
         * is a slave that may migrate to another master. 
         * 为后面的 replicas migration 服务，是一个很强的功能，自适应性很高
         */
        if (nodeIsSlave(myself) && nodeIsMaster(node) && !nodeFailed(node)) {
            // node 是一个 UP 的 master
            int okslaves = clusterCountNonFailingSlaves(node);

            if (okslaves == 0 && node->numslots > 0) orphaned_masters++;    // orphaned_masters 必须是一个还有 slot 但是没有 slave 的 master
            if (okslaves > max_slaves) max_slaves = okslaves;
            if (nodeIsSlave(myself) && myself->slaveof == node)
                this_slaves = okslaves;
        }

        /* If we are waiting for the PONG more than half the cluster
         * timeout, reconnect the link: maybe there is a connection
         * issue even if the node is alive. */
        // 如果等到 PONG 到达的时间超过了 node timeout 一半的连接
        // 因为尽管节点依然正常，但连接可能已经出问题了
        if (node->link   /* is connected(具备发送 PING\MEET 的可能) */
            && now - node->link->ctime > server.cluster_node_timeout /* was not already reconnected */
            && node->ping_sent /* we already sent a ping */
            && node->pong_received < node->ping_sent /* still waiting pong */
            && now - node->ping_sent > server.cluster_node_timeout/2) /* and we are waiting for the pong more than timeout/2 */
        {
            /* Disconnect the link, it will be reconnected automatically. */
            // 释放连接，下次 clusterCron() 会自动重连
            freeClusterLink(node->link);
        }

        /* If we have currently no active ping in this instance, and the
         * received PONG is older than half the cluster timeout, send
         * a new ping now, to ensure all the nodes are pinged without
         * a too big delay. */
        // 如果目前没有在 PING 节点
        // 并且已经有 node timeout 一半的时间没有从节点那里收到 PONG 回复
        // 那么向节点发送一个 PING ，确保节点的信息不会太旧
        // （因为一部分节点可能一直没有被随机中）
        if (node->link &&
            node->ping_sent == 0 &&
            (now - node->pong_received) > server.cluster_node_timeout/2)
        {
            // 避免随机 PING 过于集中，导致部分 node 很久都没有收到 Gossip，在这里进行修正
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* If we are a master and one of the slaves requested a manual
         * failover, ping it continuously. */
        // 如果这是一个主节点，并且有一个 slave 请求进行手动故障转移
        // 那么向 slave 发送 PING 。
        if (server.cluster->mf_end &&
            nodeIsMaster(myself) &&
            server.cluster->mf_slave == node &&
            node->link)
        {
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* Check only if we have an active ping for this instance. */
        // 以下代码只在节点发送了 PING 命令的情况下执行
        if (node->ping_sent == 0) continue;

        /* Compute the delay of the PONG. Note that if we already received
         * the PONG, then node->ping_sent is zero, so can't reach this
         * code at all. */
        // 计算等待 PONG 回复的时长
        delay = now - node->ping_sent;

        // 等待 PONG 回复的时长超过了限制值，将目标节点标记为 PFAIL （Probable Fail, 疑似下线）
        if (delay > server.cluster_node_timeout) {
            /* Timeout reached. Set the node as possibly failing if it is
             * not already in this state. */
            if (!(node->flags & (REDIS_NODE_PFAIL|REDIS_NODE_FAIL))) {
                redisLog(REDIS_DEBUG,"*** NODE %.40s possibly failing",
                    node->name);
                // 打开疑似下线标记
                node->flags |= REDIS_NODE_PFAIL;
                update_state = 1;
            }
        }
    }
    dictReleaseIterator(di);

    /* If we are a slave node but the replication is still turned off,
     * enable it if we know the address of our master and it appears to
     * be up. */
    // 如果从节点没有在复制主节点，那么对从节点进行设置
    if (nodeIsSlave(myself) &&
        server.masterhost == NULL &&
        myself->slaveof &&
        nodeHasAddr(myself->slaveof))
    {
        replicationSetMaster(myself->slaveof->ip, myself->slaveof->port);
    }

    /* Abourt a manual failover if the timeout is reached. */
    manualFailoverCheckTimeout();

    /* slave 开始进行 failover */
    if (nodeIsSlave(myself)) {
        clusterHandleManualFailover();
        clusterHandleSlaveFailover();

        /* 看看是否需要调整 master-slave 的 layout */
        // 因为 manual failover 是我们手动发起的，而且发起的前提是：master 仍然健在，正常提供服务
        // 所以这个即将成为 master 的 slave，即使不接替 master，整个 cluster 还是能够正常运作的
        // 所以会更倾向于把 slave 分配给缺少 slave 的 master
        // manual failover 是预设了这样的一个使用场景（我猜的）：当我们遇见了 cluster 可能会出事，所以才会想着 manual failover
        // 因对可能到来的威胁，所以这里才会悄咪咪的进行 slave migration
        /* If there are orphaned slaves, and we are a slave among the masters
         * with the max number of non-failing slaves, consider migrating to
         * the orphaned masters. Note that it does not make sense to try
         * a migration if there is no master with at least *two* working
         * slaves. */
        // orphaned_masters > 0 说明 node 没有任何的 slave，down 了就是 down 了，
        // 整个 cluster 都 fail 了，不对外提供服务。没有任何恢复的办法
        // 1) We(myself node) are a slave node.
        // 2) It was detected that there is at least one orphaned master in
        //    the cluster.
        // 3) We are a slave of one of the masters with the greatest number of
        //    slaves.
        if (orphaned_masters    // 这个 cluster 里面存在 orphaned_master（不安定因素）
            && max_slaves >= 2  // 整个集群中至少有一个 master 拥有一个以上的 slave（有 master 是拥有多余的 slave 的，有分给别人的余裕）
            && this_slaves == max_slaves)   // 当前 node(myself) 的 master，就是在整个 cluster 中，
                                            // 拥有最多 slave 的那个 master（肯定是让资源最多的那个 master 分点资源给别人）
        {
            clusterHandleSlaveMigration(max_slaves);    // 接下来，自己就要主动连接上那个 orphaned_master 了
        }
    }

    // 更新集群状态，等于 FAIL 的时候，update 会更频繁一些，以期尽快恢复工作状态
    if (update_state || server.cluster->state == REDIS_CLUSTER_FAIL)
        clusterUpdateState();
}

/* This function is called before the event handler returns to sleep for
 * events. It is useful to perform operations that must be done ASAP in
 * reaction to events fired but that are not safe to perform inside event
 * handlers, or to perform potentially expansive tasks that we need to do
 * a single time before replying to clients. 
 *
 * 在进入下个事件循环时调用。
 * 这个函数做的事都是需要尽快执行，但是不能在执行文件事件期间做的事情。
 * 在外面的大循环里面执行，beforeSleep()
 */
// 除非网络发生波动，否则这个函数基本上不会执行太多语句
// 这时候需要考虑的是：全量检查、更新会有多大负担，是否会加剧故障的严重程度
// 采用 Gossip 的 redis-cluster 本身就不适合超大规模集群，所以不用太担心这个问题
void clusterBeforeSleep(void) {

    /* Handle failover, this is needed when it is likely that there is already
     * the quorum from masters in order to react fast. */
    // 执行故障迁移
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_HANDLE_FAILOVER)
        clusterHandleSlaveFailover();

    /* Update the cluster state. */
    // 更新节点的状态
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_UPDATE_STATE)
        clusterUpdateState();

    /* Save the config, possibly using fsync. */
    // 保存 nodes.conf 配置文件
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_SAVE_CONFIG) {
        int fsync = server.cluster->todo_before_sleep &
                    CLUSTER_TODO_FSYNC_CONFIG;
        clusterSaveConfigOrDie(fsync);
    }

    /* Reset our flags (not strictly needed since every single function
     * called for flags set should be able to clear its flag). */
    server.cluster->todo_before_sleep = 0;
}

// 打开 todo_before_sleep 的指定标识
// 每个标识代表了节点在结束一个事件循环时要做的工作
void clusterDoBeforeSleep(int flags) {
    server.cluster->todo_before_sleep |= flags;
}

/* -----------------------------------------------------------------------------
 * Slots management
 * slots 的具体信息将会分别存放在 node 跟 cluster 里面
 * server.cluster->slots[] 里面存的是具体的 slot --> node 映射信息（体积巨大，但是全面）
 * node->slots 采用的是 bitmap 的形式，记录了这一个 node 被分配了那几个 slots（体积小，只能知道哪几个 slot 属于这个 node）
 * 
 * slots 信息是通过 node 主动 PING、MEET 其他 node 的时候，在 header 里面附带自己的 slots 信息，从而公布出去的
 * -------------------------------------------------------------------------- */

/* Test bit 'pos' in a generic bitmap. Return 1 if the bit is set,
 * otherwise 0. */
// 检查位图 bitmap 的 pos 位置是否已经被设置
// 返回 1 表示已被设置，返回 0 表示未被设置。
int bitmapTestBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    return (bitmap[byte] & (1<<bit)) != 0;
}

/* Set the bit at position 'pos' in a bitmap. */
// 设置位图 bitmap 在 pos 位置的值
void bitmapSetBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    bitmap[byte] |= 1<<bit;
}

/* Clear the bit at position 'pos' in a bitmap. */
// 清除位图 bitmap 在 pos 位置的值
void bitmapClearBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    bitmap[byte] &= ~(1<<bit);
}

/* Set the slot bit and return the old value. */
// 为槽二进制位设置新值，并返回旧值
int clusterNodeSetSlotBit(clusterNode *n, int slot) {
    int old = bitmapTestBit(n->slots,slot);
    bitmapSetBit(n->slots,slot);
    if (!old) n->numslots++;
    return old;
}

/* Clear the slot bit and return the old value. */
// 清空槽二进制位，并返回旧值
int clusterNodeClearSlotBit(clusterNode *n, int slot) {
    int old = bitmapTestBit(n->slots,slot);
    bitmapClearBit(n->slots,slot);
    if (old) n->numslots--;
    return old;
}

/* Return the slot bit from the cluster node structure. */
// 返回对应 slot 是否被使用
int clusterNodeGetSlotBit(clusterNode *n, int slot) {
    return bitmapTestBit(n->slots,slot);
}

/* Add the specified slot to the list of slots that node 'n' will
 * serve. Return REDIS_OK if the operation ended with success.
 * If the slot is already assigned to another instance this is considered
 * an error and REDIS_ERR is returned. */
// 将槽 slot 添加到节点 n 需要处理的槽的列表中
// 添加成功返回 REDIS_OK ,如果槽已经由这个节点处理了
// 那么返回 REDIS_ERR 。
// 1. 设置 node 的 slots bitmap; 2. 设置 cluster state 的全局 slot 分配情况; 3. 增加计数
int clusterAddSlot(clusterNode *n, int slot) {

    // 槽 slot 已经是节点 n 处理的了
    if (server.cluster->slots[slot]) return REDIS_ERR;

    // 设置 bitmap
    clusterNodeSetSlotBit(n,slot);

    // 更新集群状态
    server.cluster->slots[slot] = n;

    return REDIS_OK;
}

/* Delete the specified slot marking it as unassigned.
 *
 * 将指定槽标记为未分配（unassigned）。
 *
 * Returns REDIS_OK if the slot was assigned, otherwise if the slot was
 * already unassigned REDIS_ERR is returned. 
 *
 * 标记成功返回 REDIS_OK ，
 * 如果槽已经是未分配的，那么返回 REDIS_ERR 。
 */
int clusterDelSlot(int slot) {

    // 获取当前处理槽 slot 的节点 n
    clusterNode *n = server.cluster->slots[slot];

    if (!n) return REDIS_ERR;

    // 清除位图
    redisAssert(clusterNodeClearSlotBit(n,slot) == 1);

    // 清空负责处理槽的节点
    server.cluster->slots[slot] = NULL;

    return REDIS_OK;
}

/* Delete all the slots associated with the specified node.
 * The number of deleted slots is returned. */
// 删除给定节点所有的槽，并返回被删除槽的数量
int clusterDelNodeSlots(clusterNode *node) {
    int deleted = 0, j;

    for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
        // 如果这个槽由该节点负责，那么删除它
        if (clusterNodeGetSlotBit(node,j)) clusterDelSlot(j);
        deleted++;
    }
    return deleted;
}

/* Clear the migrating / importing state for all the slots.
 * This is useful at initialization and when turning a master into slave. */
// 清理所有槽的迁移和导入状态
// 通常在初始化或者将主节点转为从节点时使用
void clusterCloseAllSlots(void) {
    memset(server.cluster->migrating_slots_to,0,
        sizeof(server.cluster->migrating_slots_to));
    memset(server.cluster->importing_slots_from,0,
        sizeof(server.cluster->importing_slots_from));
}

/* -----------------------------------------------------------------------------
 * Cluster state evaluation(评估) function
 * -------------------------------------------------------------------------- */

/* The following are defines that are only used in the evaluation function
 * and are based on heuristics. Actaully the main point about the rejoin and
 * writable delay is that they should be a few orders of magnitude larger
 * than the network latency. */
// 这些宏都是为了让新 up 起来的 node 有足够的时间重新加载配置之后，再加入集群
#define REDIS_CLUSTER_MAX_REJOIN_DELAY 5000
#define REDIS_CLUSTER_MIN_REJOIN_DELAY 500
#define REDIS_CLUSTER_WRITABLE_DELAY 2000
// TODO: 什么是 rejoin？
// TODO: 什么是 writable delay

// 更新本 node 对于整个 cluster 的看法
// 因为在新一轮的 dispatch 中，可能有些 node 的信息、状态发生了变化，所以本 node 也要根据各个具体信息，刷新对于 cluster 的整体看法
// 当检测到所有 slot 都被分配了之后，本 node 将会认为 cluster 设置完毕，正式开始接受外界的访问
void clusterUpdateState(void) {
    int j, new_state;
    int unreachable_masters = 0;
    static mstime_t among_minority_time;
    static mstime_t first_call_time = 0;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_UPDATE_STATE;

    /* If this is a master node, wait some time before turning the state
     * into OK, since it is not a good idea to rejoin the cluster as a writable
     * master, after a reboot, without giving the cluster a chance to
     * reconfigure this node. Note that the delay is calculated starting from
     * the first call to this function and not since the server start, in order
     * to don't count the DB loading time. */
    if (first_call_time == 0) first_call_time = mstime();
    // giving the cluster a chance to reconfigure this node.
    if (nodeIsMaster(myself) &&
        mstime() - first_call_time < REDIS_CLUSTER_WRITABLE_DELAY) return;

    /* Start assuming the state is OK. We'll turn it into FAIL if there
     * are the right conditions. */

    // 先假设节点状态为 OK ，后面再检测节点是否真的下线
    new_state = REDIS_CLUSTER_OK;

    /* Check if all the slots are covered. */
    // 检查是否所有槽都已经有某个节点在处理（这些信息通过 node 之间的消息更新出来的）
    for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
        if (server.cluster->slots[j] == NULL ||
            server.cluster->slots[j]->flags & (REDIS_NODE_FAIL))
        {
            // 有 node fail，很自然整个 cluster 也是 fail 的，触发它再 up 起来了
            new_state = REDIS_CLUSTER_FAIL;
            break;
        }
    }

    /* Compute the cluster size, that is the number of master nodes
     * serving at least a single slot.
     *
     * At the same time count the number of unreachable masters with
     * at least one node. */
    // 统计在线并且拥有 slot 的 master 的数量，
    // 以及下线 master 的数量
    {
        dictIterator *di;
        dictEntry *de;

        server.cluster->size = 0;
        di = dictGetSafeIterator(server.cluster->nodes);
        while((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);

            if (nodeIsMaster(node) && node->numslots) {
                server.cluster->size++;
                if (node->flags & (REDIS_NODE_FAIL|REDIS_NODE_PFAIL))
                    unreachable_masters++;
            }
        }
        dictReleaseIterator(di);
    }

    /* If we can't reach at least half the masters, change the cluster state
     * to FAIL, as we are not even able to mark nodes as FAIL in this side
     * of the netsplit because of lack of majority.
     *
     * 如果不能连接到半数以上节点，那么将我们自己的状态设置为 FAIL
     * 因为在少于半数节点的情况下，节点是无法将一个节点判断为 FAIL 的。
     */
    {
        // 因为不知道故障的是你，还是别人
        int needed_quorum = (server.cluster->size / 2) + 1;

        if (unreachable_masters >= needed_quorum) {
            new_state = REDIS_CLUSTER_FAIL;
            among_minority_time = mstime();
        }
    }

    /* Log a state change */
    // 记录状态变更
    if (new_state != server.cluster->state) {
        mstime_t rejoin_delay = server.cluster_node_timeout;

        /* If the instance is a master and was partitioned away with the
         * minority, don't let it accept queries for some time after the
         * partition heals, to make sure there is enough time to receive
         * a configuration update. */
        if (rejoin_delay > REDIS_CLUSTER_MAX_REJOIN_DELAY)
            rejoin_delay = REDIS_CLUSTER_MAX_REJOIN_DELAY;
        if (rejoin_delay < REDIS_CLUSTER_MIN_REJOIN_DELAY)
            rejoin_delay = REDIS_CLUSTER_MIN_REJOIN_DELAY;

        if (new_state == REDIS_CLUSTER_OK &&
            nodeIsMaster(myself) &&
            mstime() - among_minority_time < rejoin_delay)
        {
            return;
        }

        /* Change the state and log the event. */
        redisLog(REDIS_WARNING,"Cluster state changed: %s",
            new_state == REDIS_CLUSTER_OK ? "ok" : "fail");

        // 设置新状态
        server.cluster->state = new_state;
    }
}

/* This function is called after the node startup in order to verify that data
 * loaded from disk is in agreement with the cluster configuration:
 *
 * 1) If we find keys about hash slots we have no responsibility for, the
 *    following happens:
 *    A) If no other node is in charge according to the current cluster
 *       configuration, we add these slots to our node.
 *    B) If according to our config other nodes are already in charge for
 *       this lots, we set the slots as IMPORTING from our point of view
 *       in order to justify we have those slots, and in order to make
 *       redis-trib aware of the issue, so that it can try to fix it.
 * 2) If we find data in a DB different than DB0 we return REDIS_ERR to
 *    signal the caller it should quit the server with an error message
 *    or take other actions.
 *
 * The function always returns REDIS_OK even if it will try to correct
 * the error described in "1". However if data is found in DB different
 * from DB0, REDIS_ERR is returned.
 *
 * The function also uses the logging facility in order to warn the user
 * about desynchronizations between the data we have in memory and the
 * cluster configuration. */
// 检查当前节点的节点配置是否正确，包含的数据是否正确
// 在启动集群时被调用（看 redis.c ）
// TODO: 看完 failover 之后再看
int verifyClusterConfigWithData(void) {
    int j;
    int update_config = 0;

    /* If this node is a slave, don't perform the check at all as we
     * completely depend on the replication stream. */
    // 不对从节点进行检查
    if (nodeIsSlave(myself)) return REDIS_OK;

    /* Make sure we only have keys in DB0. */
    // 确保只有 0 号数据库有数据
    for (j = 1; j < server.dbnum; j++) {
        if (dictSize(server.db[j].dict)) return REDIS_ERR;
    }

    /* Check that all the slots we see populated memory have a corresponding
     * entry in the cluster table. Otherwise fix the table. */
    // 检查槽表是否都有相应的节点，如果不是的话，进行修复
    for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
        if (!countKeysInSlot(j)) continue; /* No keys in this slot. */
        /* Check if we are assigned to this slot or if we are importing it.
         * In both cases check the next slot as the configuration makes
         * sense. */
        // 跳过正在导入的槽
        if (server.cluster->slots[j] == myself ||
            server.cluster->importing_slots_from[j] != NULL) continue;

        /* If we are here data and cluster config don't agree, and we have
         * slot 'j' populated even if we are not importing it, nor we are
         * assigned to this slot. Fix this condition. */

        update_config++;
        /* Case A: slot is unassigned. Take responsability for it. */
        if (server.cluster->slots[j] == NULL) {
            // 处理未被接受的槽
            redisLog(REDIS_WARNING, "I've keys about slot %d that is "
                                    "unassigned. Taking responsability "
                                    "for it.",j);
            clusterAddSlot(myself,j);
        } else {
            // 如果一个槽已经被其他节点接管
            // 那么将槽中的资料发送给对方
            redisLog(REDIS_WARNING, "I've keys about slot %d that is "
                                    "already assigned to a different node. "
                                    "Setting it in importing state.",j);
            server.cluster->importing_slots_from[j] = server.cluster->slots[j];
        }
    }

    // 保存 nodes.conf 文件
    if (update_config) clusterSaveConfigOrDie(1);

    return REDIS_OK;
}

/* -----------------------------------------------------------------------------
 * SLAVE nodes handling
 * -------------------------------------------------------------------------- */

/* Set the specified node 'n' as master for this node.
 * If this node is currently a master, it is turned into a slave. */
// 将节点 n 设置为当前节点的主节点
// 如果当前节点为主节点，那么将它转换为从节点
void clusterSetMaster(clusterNode *n) {
    redisAssert(n != myself);
    redisAssert(myself->numslots == 0);

    if (nodeIsMaster(myself)) {
        myself->flags &= ~REDIS_NODE_MASTER;
        myself->flags |= REDIS_NODE_SLAVE;
        clusterCloseAllSlots();
    } else {
        if (myself->slaveof)
            clusterNodeRemoveSlave(myself->slaveof,myself);
    }

    // 将 slaveof 属性指向主节点
    myself->slaveof = n;

    // 设置主节点的 IP 和地址，开始对它进行复制
    clusterNodeAddSlave(n,myself);  // 必须要把自己挂在 n->slaves 上面，不然 show 的时候会有问题
    replicationSetMaster(n->ip, n->port);   // 利用 replication.c 里面的 replicationCron() 函数、状态机转换，异步完成 connect, PSYNC\SYNC

    /**
     * NOTE: 即使这里成功设置了 master，但并不意味着这个 node 的 master 一定会是 n
     * 1. 跟 master 的连接，是在 replicateCron 里面进行的
     * 2. clusterCron 比 replicationCron 更为频繁
     * 
     * 所以在 clusterCron 的 clusterHandleSlaveMigration 有可能会重新选择你的 master
     * 但是并不绝对，因为在 redis.c 的 serverCron 里面，replicationCron 在 clusterCron 前面
    */
    resetManualFailover();
}

/* -----------------------------------------------------------------------------
 * CLUSTER command
 * -------------------------------------------------------------------------- */

/* Generate a csv-alike representation of the specified cluster node.
 * See clusterGenNodesDescription() top comment for more information.
 *
 * The function returns the string representation as an SDS string. */
// 生成节点的状态描述信息
sds clusterGenNodeDescription(clusterNode *node) {
    int j, start;
    sds ci;

    /* Node coordinates */
    ci = sdscatprintf(sdsempty(),"%.40s %s:%d ",
        node->name,
        node->ip,
        node->port);

    /* Flags */
    if (node->flags == 0) ci = sdscat(ci,"noflags,");
    if (node->flags & REDIS_NODE_MYSELF) ci = sdscat(ci,"myself,");
    if (node->flags & REDIS_NODE_MASTER) ci = sdscat(ci,"master,");
    if (node->flags & REDIS_NODE_SLAVE) ci = sdscat(ci,"slave,");
    if (node->flags & REDIS_NODE_PFAIL) ci = sdscat(ci,"fail?,");
    if (node->flags & REDIS_NODE_FAIL) ci = sdscat(ci,"fail,");
    if (node->flags & REDIS_NODE_HANDSHAKE) ci =sdscat(ci,"handshake,");
    if (node->flags & REDIS_NODE_NOADDR) ci = sdscat(ci,"noaddr,");
    if (ci[sdslen(ci)-1] == ',') ci[sdslen(ci)-1] = ' ';

    /* Slave of... or just "-" */
    if (node->slaveof)
        ci = sdscatprintf(ci,"%.40s ",node->slaveof->name);
    else
        ci = sdscatprintf(ci,"- ");

    /* Latency from the POV of this node, link status */
    ci = sdscatprintf(ci,"%lld %lld %llu %s",
        (long long) node->ping_sent,
        (long long) node->pong_received,
        (unsigned long long) node->configEpoch,
        (node->link || node->flags & REDIS_NODE_MYSELF) ?
                    "connected" : "disconnected");

    /* Slots served by this instance */
    start = -1;
    for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
        int bit;

        if ((bit = clusterNodeGetSlotBit(node,j)) != 0) {
            if (start == -1) start = j;
        }
        if (start != -1 && (!bit || j == REDIS_CLUSTER_SLOTS-1)) {
            if (bit && j == REDIS_CLUSTER_SLOTS-1) j++;

            if (start == j-1) {
                ci = sdscatprintf(ci," %d",start);
            } else {
                ci = sdscatprintf(ci," %d-%d",start,j-1);
            }
            start = -1;
        }
    }

    /* Just for MYSELF node we also dump info about slots that
     * we are migrating to other instances or importing from other
     * instances. */
    if (node->flags & REDIS_NODE_MYSELF) {
        for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
            if (server.cluster->migrating_slots_to[j]) {
                ci = sdscatprintf(ci," [%d->-%.40s]",j,
                    server.cluster->migrating_slots_to[j]->name);
            } else if (server.cluster->importing_slots_from[j]) {
                ci = sdscatprintf(ci," [%d-<-%.40s]",j,
                    server.cluster->importing_slots_from[j]->name);
            }
        }
    }
    return ci;
}

/* Generate a csv-alike representation of the nodes we are aware of,
 * including the "myself" node, and return an SDS string containing the
 * representation (it is up to the caller to free it).
 *
 * 以 csv 格式记录当前节点已知所有节点的信息（包括当前节点自身），
 * 这些信息被保存到一个 sds 里面，并作为函数值返回。
 *
 * All the nodes matching at least one of the node flags specified in
 * "filter" are excluded from the output, so using zero as a filter will
 * include all the known nodes in the representation, including nodes in
 * the HANDSHAKE state.
 *
 * filter 参数可以用来指定节点的 flag 标识，
 * 带有被指定标识的节点不会被记录在输出结构中，
 * filter 为 0 表示记录所有节点的信息，包括 HANDSHAKE 状态的节点。
 *
 * The representation obtained using this function is used for the output
 * of the CLUSTER NODES function, and as format for the cluster
 * configuration file (nodes.conf) for a given node. 
 *
 * 这个函数生成的结果会被用于 CLUSTER NODES 命令，
 * 以及用于生成 nodes.conf 配置文件。
 */
sds clusterGenNodesDescription(int filter) {
    sds ci = sdsempty(), ni;
    dictIterator *di;
    dictEntry *de;

    // 遍历集群中的所有节点
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        // 不打印包含指定 flag 的节点
        if (node->flags & filter) continue;

        ni = clusterGenNodeDescription(node);
        ci = sdscatsds(ci,ni);
        sdsfree(ni);
        ci = sdscatlen(ci,"\n",1);
    }
    dictReleaseIterator(di);

    return ci;
}

// 取出一个 slot 数值
int getSlotOrReply(redisClient *c, robj *o) {
    long long slot;

    if (getLongLongFromObject(o,&slot) != REDIS_OK ||
        slot < 0 || slot >= REDIS_CLUSTER_SLOTS)
    {
        addReplyError(c,"Invalid or out of range slot");
        return -1;
    }
    return (int) slot;
}

// CLUSTER 命令的实现
void clusterCommand(redisClient *c) {

    // 不能在非集群模式下使用该命令
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    if (!strcasecmp(c->argv[1]->ptr,"meet") && c->argc == 4) {
        /* CLUSTER MEET <ip> <port> */
        // 将给定地址的节点添加到当前节点所处的集群里面（发出 CMD 的 node 主动把 ip:port 上面的 node 招揽进来）
        // 本节点主动做这件事
        /**
         * This second high port(port 10000+) is used for the **Cluster bus**, that is a node-to-node communication channel using a binary protocol.
         * The Cluster bus is used by nodes for failure detection, configuration update, failover authorization and so forth. 
         * Clients should never try to communicate with the cluster bus port, but always with the normal Redis command port
         */
        long long port;

        // 检查 port 参数的合法性
        if (getLongLongFromObject(c->argv[3], &port) != REDIS_OK) {
            addReplyErrorFormat(c,"Invalid TCP port specified: %s",
                                (char*)c->argv[3]->ptr);
            return;
        }

        // 尝试与给定地址的节点进行连接
        if (clusterStartHandshake(c->argv[2]->ptr,port) == 0 &&
            errno == EINVAL)
        {
            // 连接环境创建失败
            addReplyErrorFormat(c,"Invalid node address specified: %s:%s",
                            (char*)c->argv[2]->ptr, (char*)c->argv[3]->ptr);
        } else {
            // 连接环境创建成功
            addReply(c,shared.ok);
        }
        // 到这里为止，并没有真正的发起 connect，要等到 clusterCorn 才会真正开始 connect 的

    } else if (!strcasecmp(c->argv[1]->ptr,"nodes") && c->argc == 2) {
        /* CLUSTER NODES */
        // 列出集群所有节点的信息
        // 例如：97a3a64667477371c4479320d683e4c8db5858b1 :0 myself,master - 0 0 0 connected 0-5959 10922-11422
        // 例如：3c3a0c74aae0b56170ccb03a76b60cfe7dc1912e 127.0.0.1:7005 master - 0 1385503419023 3 connected 11423-16383
        // 例如：3fc783611028b1707fd65345e763befb36454d73 127.0.0.1:7004 slave 3e3a6cb0d9a9a87168e266b0a0b24026c0aae3f0 0 1385503418521 0 connected
        // node-id: 3fc783611028b1707fd65345e763befb36454d73 
        // ip:port 127.0.0.1:7004; flag(node-role\status): slave\master\myself; 
        // slave 节点的话，还会把相应的主节点 id 打印出来：3e3a6cb0d9a9a87168e266b0a0b24026c0aae3f0（没有的话，采用 - 占位）
        // 集群最近一次向节点发送 PING 命令之后， 过去了多长时间还没接到回复。这里的示例都是 0
        // 节点最近一次返回 PONG 回复的时间。1385503419023，1385503418521，myself 必然是 0
        // 节点的配置纪元（configuration epoch）
        // 本节点的网络连接情况：例如 connected 。
        // 节点目前包含的槽; 10922-11422; 11423-16383

        robj *o;
        sds ci = clusterGenNodesDescription(0);

        o = createObject(REDIS_STRING,ci);
        addReplyBulk(c,o);
        decrRefCount(o);

    } else if (!strcasecmp(c->argv[1]->ptr,"flushslots") && c->argc == 2) {
        /* CLUSTER FLUSHSLOTS */
        // 删除当前节点的所有槽，让它变为不处理任何槽

        // 删除槽必须在数据库为空的情况下进行
        if (dictSize(server.db[0].dict) != 0) {
            addReplyError(c,"DB must be empty to perform CLUSTER FLUSHSLOTS.");
            return;
        }
        // 删除所有由该节点处理的槽
        clusterDelNodeSlots(myself);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);

    } else if ((!strcasecmp(c->argv[1]->ptr,"addslots") || !strcasecmp(c->argv[1]->ptr,"delslots")) && c->argc >= 3)
    {
        /* CLUSTER ADDSLOTS <slot> [slot] ... */
        // 将一个或多个 slot 添加到当前节点

        /* CLUSTER DELSLOTS <slot> [slot] ... */
        // 从当前节点中删除一个或多个 slot
    
        int j, slot;

        // 一个数组，记录所有要添加或者删除的槽（去除 CMD 中重复指定的 slot）
        unsigned char *slots = zmalloc(REDIS_CLUSTER_SLOTS);

        // 检查这是 delslots 还是 addslots
        int del = !strcasecmp(c->argv[1]->ptr,"delslots");

        // 将 slots 数组的所有值设置为 0
        memset(slots,0,REDIS_CLUSTER_SLOTS);

        /* Check that all the arguments are parsable and that all the
         * slots are not already busy. */
        // 处理所有输入 slot 参数（去重、去异常参数）
        for (j = 2; j < c->argc; j++) {

            // 获取 slot 数字
            if ((slot = getSlotOrReply(c,c->argv[j])) == -1) {
                zfree(slots);
                return;
            }

            // 如果这是 delslots 命令，并且指定槽为未指定，那么返回一个错误
            if (del && server.cluster->slots[slot] == NULL) {
                addReplyErrorFormat(c,"Slot %d is already unassigned", slot);
                zfree(slots);
                return;
            // 如果这是 addslots 命令，并且槽已经有节点在负责，那么返回一个错误
            } else if (!del && server.cluster->slots[slot]) {
                addReplyErrorFormat(c,"Slot %d is already busy", slot);
                zfree(slots);
                return;
            }

            // 如果某个槽指定了一次以上，那么返回一个错误
            if (slots[slot]++ == 1) {
                addReplyErrorFormat(c,"Slot %d specified multiple times",
                    (int)slot);
                zfree(slots);
                return;
            }
        }

        // 处理所有输入 slot
        for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
            if (slots[j]) {
                int retval;

                /* If this slot was set as importing we can clear this 
                 * state as now we are the real owner of the slot. */
                // 如果指定 slot 之前的状态为载入状态，那么现在可以清除这一状态
                // 因为当前节点现在已经是 slot 的负责人了
                if (server.cluster->importing_slots_from[j])
                    server.cluster->importing_slots_from[j] = NULL;

                // 添加或者删除指定 slot
                retval = del ? clusterDelSlot(j) :
                               clusterAddSlot(myself,j);    // 同时加减 node->numslots 的计数
                redisAssertWithInfo(c,NULL,retval == REDIS_OK);
            }
        }
        zfree(slots);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);

    } else if (!strcasecmp(c->argv[1]->ptr,"setslot") && c->argc >= 4) {
        // 在相应的 node 上面更新：server.cluster->slots
        // 执行完这两个 CMD 之后，slot\key 仍然能够进行读写
        /* send to source node: SETSLOT 10 MIGRATING <target node ID> */
        /* send to target node: SETSLOT 10 IMPORTING <source node ID> */

        // 强行配置 slot 的 node 信息，是 resharde 的最后异步
        // 在 mirgate 过程结束的时候，这个 CMD 必须发送给 target node，由 target node 接受这个 CMD
        // clusterBuildMessageHdr() --> memcpy(hdr->myslots,master->slots,sizeof(hdr->myslots));
        // 这里已经被限制死了，不然这个新的 slot 分配信息，没办法通过 PING、PONG 的 header 传播出去
        // n.r.cluster("setslot",slot,"node",target.info[:name]); 看，自动脚本也是这样的，向 target node-id 发送这个 CMD
        /* send to any node: SETSLOT 10 NODE <target node ID> */

        // 撤销操作
        /* SETSLOT 10 STABLE */
        int slot;
        clusterNode *n;

        // 取出 slot 值
        if ((slot = getSlotOrReply(c,c->argv[2])) == -1) return;

        // CLUSTER SETSLOT <slot> MIGRATING <tarrget node id>
        // 将本节点的槽 slot 迁移至 node id 所指定的节点
        if (!strcasecmp(c->argv[3]->ptr,"migrating") && c->argc == 5) {
            // 被迁移的槽必须属于本节点
            if (server.cluster->slots[slot] != myself) {
                addReplyErrorFormat(c,"I'm not the owner of hash slot %u",slot);
                return;
            }

            // 迁移的目标节点必须是本节点已知的
            if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }

            // 为槽设置迁移目标节点
            server.cluster->migrating_slots_to[slot] = n;

        // CLUSTER SETSLOT <slot> IMPORTING <node id>
        // 从节点 node id 中导入槽 slot 到本节点
        } else if (!strcasecmp(c->argv[3]->ptr,"importing") && c->argc == 5) {

            // 如果 slot 槽本身已经由本节点处理，那么无须进行导入
            if (server.cluster->slots[slot] == myself) {
                addReplyErrorFormat(c,
                    "I'm already the owner of hash slot %u",slot);
                return;
            }

            // node id 指定的节点必须是本节点已知的，这样才能从目标节点导入槽
            if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[3]->ptr);
                return;
            }

            // 为槽设置导入目标节点
            server.cluster->importing_slots_from[slot] = n;

        } else if (!strcasecmp(c->argv[3]->ptr,"stable") && c->argc == 4) {
            /* CLUSTER SETSLOT <SLOT> STABLE */
            // 取消对槽 slot 的迁移或者导入

            server.cluster->importing_slots_from[slot] = NULL;
            server.cluster->migrating_slots_to[slot] = NULL;

        } else if (!strcasecmp(c->argv[3]->ptr,"node") && c->argc == 5) {
            /* CLUSTER SETSLOT <SLOT> NODE <NODE ID> */
            // 将未指派 slot 指派给 node id 指定的节点

            // 查找目标节点
            clusterNode *n = clusterLookupNode(c->argv[4]->ptr);

            // 目标节点必须已存在
            if (!n) {
                addReplyErrorFormat(c,"Unknown node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }

            /* If this hash slot was served by 'myself' before to switch
             * make sure there are no longer local keys for this hash slot. */
            // 如果这个槽之前由当前节点负责处理，那么必须保证槽里面没有键存在
            if (server.cluster->slots[slot] == myself && n != myself) {
                if (countKeysInSlot(slot) != 0) {
                    addReplyErrorFormat(c,
                        "Can't assign hashslot %d to a different node "
                        "while I still hold keys for this hash slot.", slot);
                    return;
                }
            }
            /* If this slot is in migrating status but we have no keys
             * for it assigning the slot to another node will clear
             * the migratig status. */
            if (countKeysInSlot(slot) == 0 &&
                server.cluster->migrating_slots_to[slot])
                server.cluster->migrating_slots_to[slot] = NULL;

            /* If this node was importing this slot, assigning the slot to
             * itself also clears the importing status. */
            // 撤销本节点对 slot 的导入计划
            if (n == myself &&
                server.cluster->importing_slots_from[slot])
            {
                /* This slot was manually migrated, set this node configEpoch
                 * to a new epoch so that the new version can be propagated
                 * by the cluster.
                 *
                 * Note that if this ever results in a collision with another
                 * node getting the same configEpoch, for example because a
                 * failover happens at the same time we close the slot, the
                 * configEpoch collision resolution will fix it assigning
                 * a different epoch to each node. */
                uint64_t maxEpoch = clusterGetMaxEpoch();

                if (myself->configEpoch == 0 ||
                    myself->configEpoch != maxEpoch)
                {
                    server.cluster->currentEpoch++;
                    myself->configEpoch = server.cluster->currentEpoch;
                    clusterDoBeforeSleep(CLUSTER_TODO_FSYNC_CONFIG);
                    redisLog(REDIS_WARNING,
                        "configEpoch set to %llu after importing slot %d",
                        (unsigned long long) myself->configEpoch, slot);
                }
                server.cluster->importing_slots_from[slot] = NULL;
            }

            // 将槽设置为未指派
            clusterDelSlot(slot);

            // 将槽指派给目标节点
            clusterAddSlot(n,slot);

        } else {
            addReplyError(c,
                "Invalid CLUSTER SETSLOT action or number of arguments");
            return;
        }
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|CLUSTER_TODO_UPDATE_STATE);
        addReply(c,shared.ok);

    } else if (!strcasecmp(c->argv[1]->ptr,"info") && c->argc == 2) {
        /* CLUSTER INFO */
        // 打印出集群的当前信息

        char *statestr[] = {"ok","fail","needhelp"};
        int slots_assigned = 0, slots_ok = 0, slots_pfail = 0, slots_fail = 0;
        int j;

        // 统计集群中的已指派节点、已下线节点、疑似下线节点和正常节点的数量
        for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
            clusterNode *n = server.cluster->slots[j];

            // 跳过未指派节点
            if (n == NULL) continue;

            // 统计已指派节点的数量
            slots_assigned++;
            if (nodeFailed(n)) {
                slots_fail++;
            } else if (nodeTimedOut(n)) {
                slots_pfail++;
            } else {
                // 正常节点
                slots_ok++;
            }
        }

        // 打印信息
        sds info = sdscatprintf(sdsempty(),
            "cluster_state:%s\r\n"
            "cluster_slots_assigned:%d\r\n"
            "cluster_slots_ok:%d\r\n"
            "cluster_slots_pfail:%d\r\n"
            "cluster_slots_fail:%d\r\n"
            "cluster_known_nodes:%lu\r\n"
            "cluster_size:%d\r\n"
            "cluster_current_epoch:%llu\r\n"
            "cluster_stats_messages_sent:%lld\r\n"
            "cluster_stats_messages_received:%lld\r\n"
            , statestr[server.cluster->state],
            slots_assigned,
            slots_ok,
            slots_pfail,
            slots_fail,
            dictSize(server.cluster->nodes),
            server.cluster->size,
            (unsigned long long) server.cluster->currentEpoch,
            server.cluster->stats_bus_messages_sent,
            server.cluster->stats_bus_messages_received
        );
        addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
            (unsigned long)sdslen(info)));
        addReplySds(c,info);
        addReply(c,shared.crlf);

    } else if (!strcasecmp(c->argv[1]->ptr,"saveconfig") && c->argc == 2) {
        // CLUSTER SAVECONFIG 命令
        // 将 nodes.conf 文件保存到磁盘里面

        // 保存
        int retval = clusterSaveConfig(1);

        // 检查错误
        if (retval == 0)
            addReply(c,shared.ok);
        else
            addReplyErrorFormat(c,"error saving the cluster node config: %s",
                strerror(errno));

    } else if (!strcasecmp(c->argv[1]->ptr,"keyslot") && c->argc == 3) {
        /* CLUSTER KEYSLOT <key> */
        // 返回 key 应该被 hash 到那个槽上

        sds key = c->argv[2]->ptr;

        addReplyLongLong(c,keyHashSlot(key,sdslen(key)));

    } else if (!strcasecmp(c->argv[1]->ptr,"countkeysinslot") && c->argc == 3) {
        /* CLUSTER COUNTKEYSINSLOT <slot> */
        // 计算指定 slot 上的键数量

        long long slot;

        // 取出 slot 参数
        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != REDIS_OK)
            return;
        if (slot < 0 || slot >= REDIS_CLUSTER_SLOTS) {
            addReplyError(c,"Invalid slot");
            return;
        }

        addReplyLongLong(c,countKeysInSlot(slot));

    } else if (!strcasecmp(c->argv[1]->ptr,"getkeysinslot") && c->argc == 4) {
        /* CLUSTER GETKEYSINSLOT <slot> <count> */
        // 打印 count 个属于 slot 槽的键

        long long maxkeys, slot;
        unsigned int numkeys, j;
        robj **keys;

        // 取出 slot 参数
        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != REDIS_OK)
            return;
        // 取出 count 参数
        if (getLongLongFromObjectOrReply(c,c->argv[3],&maxkeys,NULL)
            != REDIS_OK)
            return;
        // 检查参数的合法性
        if (slot < 0 || slot >= REDIS_CLUSTER_SLOTS || maxkeys < 0) {
            addReplyError(c,"Invalid slot or number of keys");
            return;
        }

        // 分配一个保存键的数组
        keys = zmalloc(sizeof(robj*)*maxkeys);
        // 将键记录到 keys 数组
        numkeys = getKeysInSlot(slot, keys, maxkeys);

        // 打印获得的键
        addReplyMultiBulkLen(c,numkeys);
        for (j = 0; j < numkeys; j++) addReplyBulk(c,keys[j]);
        zfree(keys);

    } else if (!strcasecmp(c->argv[1]->ptr,"forget") && c->argc == 3) {
        /* CLUSTER FORGET <NODE ID> */
        // 从集群中删除 NODE_ID 指定的节点

        // 查找 NODE_ID 指定的节点
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);

        // 该节点不存在于集群中
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        } else if (n == myself) {
            addReplyError(c,"I tried hard but I can't forget myself...");
            return;
        } else if (nodeIsSlave(myself) && myself->slaveof == n) {
            addReplyError(c,"Can't forget my master!");
            return;
        }

        // 将集群添加到黑名单
        clusterBlacklistAddNode(n);
        // 从集群中删除该节点
        clusterDelNode(n);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);

    } else if (!strcasecmp(c->argv[1]->ptr,"replicate") && c->argc == 3) {
        // cluster + master-slave 的命令
        /* CLUSTER REPLICATE <NODE ID> */
        // 将当前节点设置为 NODE_ID 指定的节点的从节点（复制品）

        // 根据名字查找节点
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);

        /* Lookup the specified node in our table. */
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        /* I can't replicate myself. */
        // 指定节点是自己，不能进行复制
        if (n == myself) {
            addReplyError(c,"Can't replicate myself");
            return;
        }

        /* Can't replicate a slave. */
        // 不能复制一个从节点
        if (n->slaveof != NULL) {
            addReplyError(c,"I can only replicate a master, not a slave.");
            return;
        }

        /* If the instance is currently a master, it should have no assigned
         * slots nor keys to accept to replicate some other node.
         * Slaves can switch to another master without issues. */
        // 节点必须没有被指派任何槽，并且数据库必须为空
        if (nodeIsMaster(myself) &&
            (myself->numslots != 0 || dictSize(server.db[0].dict) != 0)) {
            addReplyError(c,
                "To set a master the node must be empty and "
                "without assigned slots.");
            return;
        }

        /* Set the master. */
        // 将节点 n 设为本节点的主节点
        //（本节点的 slaveof node-id 信息将会从跟 node 相关的 PING\PONG 中传播出去,
        // 然后全集群就会知晓相应的 slaveof 信息了（在 process 的时候，解析添加）
        clusterSetMaster(n);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"slaves") && c->argc == 3) {
        /* CLUSTER SLAVES <NODE ID> */
        // 打印给定主节点的所有从节点的信息

        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);
        int j;

        /* Lookup the specified node in our table. */
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        if (nodeIsSlave(n)) {
            addReplyError(c,"The specified node is not a master");
            return;
        }

        addReplyMultiBulkLen(c,n->numslaves);
        for (j = 0; j < n->numslaves; j++) {
            sds ni = clusterGenNodeDescription(n->slaves[j]);
            addReplyBulkCString(c,ni);
            sdsfree(ni);
        }

    } else if (!strcasecmp(c->argv[1]->ptr,"failover") && (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER FAILOVER [FORCE] */
        // 执行手动故障转移

        int force = 0;

        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"force")) {
                force = 1;
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }

        // 命令只能发送给从节点
        if (nodeIsMaster(myself)) {
            addReplyError(c,"You should send CLUSTER FAILOVER to a slave");
            return;
        } else if (!force &&
                   (myself->slaveof == NULL || nodeFailed(myself->slaveof) ||
                   myself->slaveof->link == NULL))
        {
            // 如果主节点已下线或者处于失效状态
            // 并且命令没有给定 force 参数，那么命令执行失败
            addReplyError(c,"Master is down or failed, "
                            "please use CLUSTER FAILOVER FORCE");
            return;
        }

        // 重置手动故障转移的有关属性
        resetManualFailover();
        // 设定手动故障转移的最大执行时限
        server.cluster->mf_end = mstime() + REDIS_CLUSTER_MF_TIMEOUT;

        /* If this is a forced failover, we don't need to talk with our master
         * to agree about the offset. We just failover taking over it without
         * coordination. */
        // 如果这是强制的手动 failover ，那么直接开始 failover ，
        // 无须向其他 master 沟通偏移量。
        if (force) {
            // 如果这是强制的手动故障转移，那么直接开始执行故障转移操作
            server.cluster->mf_can_start = 1;
        } else {
            // 如果不是强制的话，那么需要和主节点比对相互的偏移量是否一致
            clusterSendMFStart(myself->slaveof);
        }
        redisLog(REDIS_WARNING,"Manual failover user request accepted.");
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"set-config-epoch") && c->argc == 3)
    {
        /* CLUSTER SET-CONFIG-EPOCH <epoch>
         *
         * The user is allowed to set the config epoch only when a node is
         * totally fresh: no config epoch, no other known node, and so forth.
         * This happens at cluster creation time to start with a cluster where
         * every node has a different node ID, without to rely on the conflicts
         * resolution system which is too slow when a big cluster is created. */
        long long epoch;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&epoch,NULL) != REDIS_OK)
            return;

        if (epoch < 0) {
            addReplyErrorFormat(c,"Invalid config epoch specified: %lld",epoch);
        } else if (dictSize(server.cluster->nodes) > 1) {
            addReplyError(c,"The user can assign a config epoch only when the "
                            "node does not know any other node.");
        } else if (myself->configEpoch != 0) {
            addReplyError(c,"Node config epoch is already non-zero");
        } else {
            myself->configEpoch = epoch;
            /* No need to fsync the config here since in the unlucky event
             * of a failure to persist the config, the conflict resolution code
             * will assign an unique config to this node. */
            clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                                 CLUSTER_TODO_SAVE_CONFIG);
            addReply(c,shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"reset") && (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER RESET [SOFT|HARD] */
        int hard = 0;

        /* Parse soft/hard argument. Default is soft. */
        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"hard")) {
                hard = 1;
            } else if (!strcasecmp(c->argv[2]->ptr,"soft")) {
                hard = 0;
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }

        /* Slaves can be reset while containing data, but not master nodes
         * that must be empty. */
        if (nodeIsMaster(myself) && dictSize(c->db->dict) != 0) {
            addReplyError(c,"CLUSTER RESET can't be called with "
                            "master nodes containing keys");
            return;
        }
        clusterReset(hard);
        addReply(c,shared.ok);
    } else {
        addReplyError(c,"Wrong CLUSTER subcommand or number of arguments");
    }
}

/* -----------------------------------------------------------------------------
 * DUMP, RESTORE and MIGRATE commands
 * -------------------------------------------------------------------------- */

/* Generates a DUMP-format representation of the object 'o', adding it to the
 * io stream pointed by 'rio'. This function can't fail. 
 *
 * 创建对象 o 的一个 DUMP 格式表示，
 * 并将它添加到 rio 指针指向的 io 流当中。
 */
void createDumpPayload(rio *payload, robj *o) {
    unsigned char buf[2];
    uint64_t crc;

    /* Serialize the object in a RDB-like format. It consist of an object type
     * byte followed by the serialized object. This is understood by RESTORE. */
    // 将对象序列化为一个 RDB 格式对象
    // 序列化对象以对象类型为首，后跟序列化后的对象
    // 如图
    //
    // |<-- RDB payload  -->|
    //      序列化数据
    // +-------------+------+
    // | 1 byte type | obj  |
    // +-------------+------+
    rioInitWithBuffer(payload,sdsempty());
    redisAssert(rdbSaveObjectType(payload,o));
    redisAssert(rdbSaveObject(payload,o));

    /* Write the footer, this is how it looks like:
     * ----------------+---------------------+---------------+
     * ... RDB payload | 2 bytes RDB version | 8 bytes CRC64 |
     * ----------------+---------------------+---------------+
     * RDB version and CRC are both in little endian.
     */

    /* RDB version */
    // 写入 RDB 版本
    buf[0] = REDIS_RDB_VERSION & 0xff;
    buf[1] = (REDIS_RDB_VERSION >> 8) & 0xff;
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,buf,2);

    /* CRC64 */
    // 写入 CRC 校验和
    crc = crc64(0,(unsigned char*)payload->io.buffer.ptr,
                sdslen(payload->io.buffer.ptr));
    memrev64ifbe(&crc);
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,&crc,8);

    // 整个数据的结构:
    //
    // | <--- 序列化数据 -->|
    // +-------------+------+---------------------+---------------+
    // | 1 byte type | obj  | 2 bytes RDB version | 8 bytes CRC64 |
    // +-------------+------+---------------------+---------------+

}

/* Verify that the RDB version of the dump payload matches the one of this Redis
 * instance and that the checksum is ok.
 *
 * 检查输入的 DUMP 数据中， RDB 版本是否和当前 Redis 实例所使用的 RDB 版本相同，
 * 并检查校验和是否正确。
 *
 * If the DUMP payload looks valid REDIS_OK is returned, otherwise REDIS_ERR
 * is returned. 
 *
 * 检查正常返回 REDIS_OK ，否则返回 REDIS_ERR 。
 */
int verifyDumpPayload(unsigned char *p, size_t len) {
    unsigned char *footer;
    uint16_t rdbver;
    uint64_t crc;

    /* At least 2 bytes of RDB version and 8 of CRC64 should be present. */
    // 因为序列化数据至少包含 2 个字节的 RDB 版本
    // 以及 8 个字节的 CRC64 校验和
    // 所以序列化数据不可能少于 10 个字节
    if (len < 10) return REDIS_ERR;

    // 指向数据的最后 10 个字节
    footer = p+(len-10);

    /* Verify RDB version */
    // 检查序列化数据的版本号，看是否和当前实例使用的版本号一致
    rdbver = (footer[1] << 8) | footer[0];
    if (rdbver != REDIS_RDB_VERSION) return REDIS_ERR;

    /* Verify CRC64 */
    // 检查数据的 CRC64 校验和是否正确
    crc = crc64(0,p,len-8);
    memrev64ifbe(&crc);
    return (memcmp(&crc,footer+2,8) == 0) ? REDIS_OK : REDIS_ERR;
}

/* DUMP keyname
 * DUMP is actually not used by Redis Cluster but it is the obvious
 * complement of RESTORE and can be useful for different applications. */
void dumpCommand(redisClient *c) {
    robj *o, *dumpobj;
    rio payload;

    /* Check if the key is here. */
    // 取出给定键的值
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }

    /* Create the DUMP encoded representation. */
    // 创建给定值的一个 DUMP 编码表示
    createDumpPayload(&payload,o);

    /* Transfer to the client */
    // 将编码后的键值对数据返回给客户端
    dumpobj = createObject(REDIS_STRING,payload.io.buffer.ptr);
    addReplyBulk(c,dumpobj);
    decrRefCount(dumpobj);

    return;
}

/* RESTORE key ttl serialized-value [REPLACE] */
// 根据给定的 DUMP 数据，还原出一个键值对数据，并将它保存到数据库里面
void restoreCommand(redisClient *c) {
    long long ttl;
    rio payload;
    int j, type, replace = 0;
    robj *obj;

    /* Parse additional options */
    // 是否使用了 REPLACE 选项？
    for (j = 4; j < c->argc; j++) {
        if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Make sure this key does not already exist here... */
    // 如果没有给定 REPLACE 选项，并且键已经存在，那么返回错误
    if (!replace && lookupKeyWrite(c->db,c->argv[1]) != NULL) {
        addReply(c,shared.busykeyerr);
        return;
    }

    /* Check if the TTL value makes sense */
    // 取出（可能有的） TTL 值
    if (getLongLongFromObjectOrReply(c,c->argv[2],&ttl,NULL) != REDIS_OK) {
        return;
    } else if (ttl < 0) {
        addReplyError(c,"Invalid TTL value, must be >= 0");
        return;
    }

    /* Verify RDB version and data checksum. */
    // 检查 RDB 版本和校验和
    if (verifyDumpPayload(c->argv[3]->ptr,sdslen(c->argv[3]->ptr)) == REDIS_ERR)
    {
        addReplyError(c,"DUMP payload version or checksum are wrong");
        return;
    }

    // 读取 DUMP 数据，并反序列化出键值对的类型和值
    rioInitWithBuffer(&payload,c->argv[3]->ptr);
    if (((type = rdbLoadObjectType(&payload)) == -1) ||
        ((obj = rdbLoadObject(type,&payload)) == NULL))
    {
        addReplyError(c,"Bad data format");
        return;
    }

    /* Remove the old key if needed. */
    // 如果给定了 REPLACE 选项，那么先删除数据库中已存在的同名键
    if (replace) dbDelete(c->db,c->argv[1]);

    /* Create the key and set the TTL if any */
    // 将键值对添加到数据库
    dbAdd(c->db,c->argv[1],obj);

    // 如果键带有 TTL 的话，设置键的 TTL
    if (ttl) setExpire(c->db,c->argv[1],mstime()+ttl);

    signalModifiedKey(c->db,c->argv[1]);

    addReply(c,shared.ok);
    server.dirty++;
}

/* MIGRATE socket cache implementation.
 *
 * MIGRATE 套接字缓存实现
 *
 * We take a map between host:ip and a TCP socket that we used to connect
 * to this instance in recent time.
 *
 * 保存一个字典，字典的键为 host:ip ，值为最近使用的连接向指定地址的 TCP 套接字。
 *
 * This sockets are closed when the max number we cache is reached, and also
 * in serverCron() when they are around for more than a few seconds. 
 *
 * 这个字典在缓存数达到上限时被释放，
 * 并且 serverCron() 也会定期删除字典中的一些过期套接字。
 */
// 最大缓存数
#define MIGRATE_SOCKET_CACHE_ITEMS 64 /* max num of items in the cache. */
// 套接字保质期（超过这个时间的套接字会被删除）
#define MIGRATE_SOCKET_CACHE_TTL 10 /* close cached socekts after 10 sec. */

typedef struct migrateCachedSocket {

    // 套接字描述符
    int fd;

    // 最后一次使用的时间
    time_t last_use_time;

} migrateCachedSocket;

/* Return a TCP scoket connected with the target instance, possibly returning
 * a cached one.
 *
 * 返回一个连接向指定地址的 TCP 套接字，这个套接字可能是一个缓存套接字。
 *
 * This function is responsible of sending errors to the client if a
 * connection can't be established. In this case -1 is returned.
 * Otherwise on success the socket is returned, and the caller should not
 * attempt to free it after usage.
 *
 * 如果连接出错，那么函数返回 -1 。
 * 如果连接正常，那么函数返回 TCP 套接字描述符。
 *
 * If the caller detects an error while using the socket, migrateCloseSocket()
 * should be called so that the connection will be craeted from scratch
 * the next time. 
 *
 * 如果调用者在使用这个函数返回的套接字时遇上错误，
 * 那么调用者会使用 migrateCloseSocket() 来关闭出错的套接字，
 * 这样下次要连接相同地址时，服务器就会创建新的套接字来进行连接。
 */
// migrate 是会单独创建一个新的 socket 进行传输的；不能复用 cluster bus，不然会影响 node 之间的 Gossip，导致错误观测到 down
int migrateGetSocket(redisClient *c, robj *host, robj *port, long timeout) {
    int fd;
    sds name = sdsempty();
    migrateCachedSocket *cs;

    /* Check if we have an already cached socket for this ip:port pair. */
    // 根据 ip 和 port 创建地址名字
    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    
    // 在套接字缓存中查找套接字是否已经存在
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    // 缓存存在，更新最后一次使用时间，以免它被当作过期套接字而被释放
    if (cs) {
        sdsfree(name);
        cs->last_use_time = server.unixtime;
        return cs->fd;
    }

    /* No cached socket, create one. */
    // 没有缓存，创建一个新的缓存
    if (dictSize(server.migrate_cached_sockets) == MIGRATE_SOCKET_CACHE_ITEMS) {

        // 如果缓存数已经达到上线，那么在创建套接字之前，先随机删除一个连接

        /* Too many items, drop one at random. */
        dictEntry *de = dictGetRandomKey(server.migrate_cached_sockets);
        cs = dictGetVal(de);
        close(cs->fd);
        zfree(cs);
        dictDelete(server.migrate_cached_sockets,dictGetKey(de));
    }

    /* Create the socket */
    // 创建连接
    fd = anetTcpNonBlockConnect(server.neterr,c->argv[1]->ptr,
                atoi(c->argv[2]->ptr));
    if (fd == -1) {
        sdsfree(name);
        addReplyErrorFormat(c,"Can't connect to target node: %s",
            server.neterr);
        return -1;
    }
    anetEnableTcpNoDelay(server.neterr,fd);

    /* Check if it connects within the specified timeout. */
    // 检查连接的超时设置
    if ((aeWait(fd,AE_WRITABLE,timeout) & AE_WRITABLE) == 0) {
        sdsfree(name);
        addReplySds(c,
            sdsnew("-IOERR error or timeout connecting to the client\r\n"));
        close(fd);
        return -1;
    }

    /* Add to the cache and return it to the caller. */
    // 将连接添加到缓存
    cs = zmalloc(sizeof(*cs));
    cs->fd = fd;
    cs->last_use_time = server.unixtime;
    dictAdd(server.migrate_cached_sockets,name,cs);

    return fd;
}

/* Free a migrate cached connection. */
// 释放一个缓存连接
void migrateCloseSocket(robj *host, robj *port) {
    sds name = sdsempty();
    migrateCachedSocket *cs;

    // 根据 ip 和 port 创建连接的名字
    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    // 查找连接
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    if (!cs) {
        sdsfree(name);
        return;
    }

    // 关闭连接
    close(cs->fd);
    zfree(cs);

    // 从缓存中删除该连接
    dictDelete(server.migrate_cached_sockets,name);
    sdsfree(name);
}

// 移除过期的连接，由 redis.c/serverCron() 调用
void migrateCloseTimedoutSockets(void) {
    dictIterator *di = dictGetSafeIterator(server.migrate_cached_sockets);
    dictEntry *de;

    while((de = dictNext(di)) != NULL) {
        migrateCachedSocket *cs = dictGetVal(de);

        // 如果套接字最后一次使用的时间已经超过 MIGRATE_SOCKET_CACHE_TTL 
        // 那么表示该套接字过期，释放它！
        if ((server.unixtime - cs->last_use_time) > MIGRATE_SOCKET_CACHE_TTL) {
            close(cs->fd);
            zfree(cs);
            dictDelete(server.migrate_cached_sockets,dictGetKey(de));
        }
    }
    dictReleaseIterator(di);
}

/* in source node: MIGRATE target-host target-port key dbid(MUST 0) timeout [COPY | REPLACE] */
// 这个 CMD 会比较慢，因为是同步操作
// TODO:(DONE) resharde 过程中，怎么锁住这个 slot。通过同步 migrate 操作
// TODO:(DONE) 为什么要同步操作？为了锁住这个 slot，在迁移完毕之前，不再接受任何读写操作
//（不太对，因为 ask 命令，实际上在 resharde 过程中，还是可能进行部分数据读写操作的）具体情况讨论请看开头的注释
void migrateCommand(redisClient *c) {
    int fd, copy, replace, j;
    long timeout;
    long dbid;
    long long ttl, expireat;
    robj *o;
    rio cmd, payload;
    int retry_num = 0;

try_again:
    /* Initialization */
    copy = 0;
    replace = 0;
    ttl = 0;

    /* Parse additional options */
    // 读入 COPY 或者 REPLACE 选项
    for (j = 6; j < c->argc; j++) {
        if (!strcasecmp(c->argv[j]->ptr,"copy")) {
            copy = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Sanity check */
    // 检查输入参数的正确性
    if (getLongFromObjectOrReply(c,c->argv[5],&timeout,NULL) != REDIS_OK)
        return;
    if (getLongFromObjectOrReply(c,c->argv[4],&dbid,NULL) != REDIS_OK)
        return;
    if (timeout <= 0) timeout = 1000;

    /* Check if the key is here. If not we reply with success as there is
     * nothing to migrate (for instance the key expired in the meantime), but
     * we include such information in the reply string. */
    // 取出键的值对象
    if ((o = lookupKeyRead(c->db,c->argv[3])) == NULL) {
        addReplySds(c,sdsnew("+NOKEY\r\n"));
        return;
    }

    /* Connect */
    // 获取套接字连接
    fd = migrateGetSocket(c,c->argv[1],c->argv[2],timeout);
    if (fd == -1) return; /* error sent to the client by migrateGetSocket() */

    /* Create RESTORE payload and generate the protocol to call the command. */
    // 创建用于指定数据库的 SELECT 命令，以免键值对被还原到了错误的地方
    rioInitWithBuffer(&cmd,sdsempty());
    redisAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',2));
    redisAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"SELECT",6));
    redisAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,dbid));    // db-id 基本是 0 的了，cluster mode 下只能用 0

    // 取出键的过期时间戳
    expireat = getExpire(c->db,c->argv[3]);
    if (expireat != -1) {
        ttl = expireat-mstime();
        if (ttl < 1) ttl = 1;
    }
    redisAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',replace ? 5 : 4));

    // 如果运行在集群模式下，那么发送的命令为 RESTORE-ASKING
    // 如果运行在非集群模式下，那么发送的命令为 RESTORE
    if (server.cluster_enabled)
        redisAssertWithInfo(c,NULL,
            rioWriteBulkString(&cmd,"RESTORE-ASKING",14));
    else
        redisAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"RESTORE",7));

    // 写入键名和过期时间
    redisAssertWithInfo(c,NULL,sdsEncodedObject(c->argv[3]));
    redisAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,c->argv[3]->ptr,
            sdslen(c->argv[3]->ptr)));
    redisAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,ttl));

    /* Emit the payload argument, that is the serialized object using
     * the DUMP format. */
    // 将值对象进行序列化
    createDumpPayload(&payload,o);
    // 写入序列化对象
    redisAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,payload.io.buffer.ptr,
                                sdslen(payload.io.buffer.ptr)));
    sdsfree(payload.io.buffer.ptr);

    /* Add the REPLACE option to the RESTORE command if it was specified
     * as a MIGRATE option. */
    // 是否设置了 REPLACE 命令？
    if (replace)
        // 写入 REPLACE 参数
        redisAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"REPLACE",7));

    /* Transfer the query to the other node in 64K chunks. */
    // 以 64 kb 每次的大小向对方发送数据
    errno = 0;
    {
        sds buf = cmd.io.buffer.ptr;
        size_t pos = 0, towrite;
        int nwritten = 0;

        while ((towrite = sdslen(buf)-pos) > 0) {
            towrite = (towrite > (64*1024) ? (64*1024) : towrite);
            nwritten = syncWrite(fd,buf+pos,towrite,timeout);
            if (nwritten != (signed)towrite) goto socket_wr_err;
            pos += nwritten;
        }
    }

    /* Read back the reply. */
    // 读取命令的回复
    {
        char buf1[1024];
        char buf2[1024];

        /* Read the two replies */
        if (syncReadLine(fd, buf1, sizeof(buf1), timeout) <= 0)
            goto socket_rd_err;
        if (syncReadLine(fd, buf2, sizeof(buf2), timeout) <= 0)
            goto socket_rd_err;

        // 检查 RESTORE 命令执行是否成功

        if (buf1[0] == '-' || buf2[0] == '-') {

            // 执行出错。。。

            addReplyErrorFormat(c,"Target instance replied with error: %s",
                (buf1[0] == '-') ? buf1+1 : buf2+1);
        } else {

            // 执行成功。。。

            robj *aux;

            // 如果没有指定 COPY 选项，那么删除本机数据库中的键
            if (!copy) {
                /* No COPY option: remove the local key, signal the change. */
                dbDelete(c->db,c->argv[3]);
                signalModifiedKey(c->db,c->argv[3]);
            }
            addReply(c,shared.ok);
            server.dirty++;

            /* Translate MIGRATE as DEL for replication/AOF. */
            // 如果键被删除了的话，向 AOF 文件和 slave /节点发送一个 DEL 命令
            aux = createStringObject("DEL",3);
            rewriteClientCommandVector(c,2,aux,c->argv[3]);
            decrRefCount(aux);
        }
    }

    sdsfree(cmd.io.buffer.ptr);
    return;

socket_wr_err:
    sdsfree(cmd.io.buffer.ptr);
    migrateCloseSocket(c->argv[1],c->argv[2]);
    if (errno != ETIMEDOUT && retry_num++ == 0) goto try_again;
    addReplySds(c,
        sdsnew("-IOERR error or timeout writing to target instance\r\n"));
    return;

socket_rd_err:
    sdsfree(cmd.io.buffer.ptr);
    migrateCloseSocket(c->argv[1],c->argv[2]);
    if (errno != ETIMEDOUT && retry_num++ == 0) goto try_again;
    addReplySds(c,
        sdsnew("-IOERR error or timeout reading from target node\r\n"));
    return;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to serving / redirecting clients
 * Since cluster nodes are not able to proxy requests, 
 * clients may be redirected to other nodes using redirection errors -MOVED and -ASK.
 * 
 * clients(redis-cli) that are able to cache the map between keys and nodes can improve the performance in a sensible way.
 * 尽管理论上 redis-cli 是不需要 cache 相应的 node 映射关系
 * 
 * 相关 redirect 过程详见 processCommand() 函数
 * -------------------------------------------------------------------------- */
// TODO:(DONE) 在 cluster 模式下，CMD 的执行过程
// 在 dbAdd() 里面会进行 slot 里面的 key 管理（在 db.c 里面搜索 "cluster_enabled" 就好了）
/**
 * 1. CLI 随机向某一个 node 发送带 key 的 CMD；
 * 2. node A 收到的 CMD，解析，然后在 processCommand() 中进行检查：
 *    看看这个 key 属于哪一个 slot，这个 slot 是不是归 node A 管理
 * 3. 根据 key 算出相应的 slot 值，从 server.cluster->state 取出相应的管理 node
 * 4. 比较 myself == node，
 *    1) 不相等，那就向 CLI 返回 MOVE + 管理 node ip:port
 *       CLI 收到 MOVE 之后，将会连接到对应的 redis-server，然后重复 CMD 的发送流程
 *    2) 相等，则继续处理。其他处理流程跟单机版的基本是一样的
 *       区别是，在 server.cluster->slots_to_keys 里面记录 slot-->key 的映射关系
 *       方便 resharde
*/

// TODO:(DONE) key, value, slot, DB 的存储体系
// CLUSTER GETKEYSINSLOT <slot> <count> 这个 CMD 是很好的说明
/**
 * 实际上，cluster 模式下，key-value pair 的保存，跟单机模式下是完全一样的
 * 只不过，当开启 cluster 之后，需要额外增加一个 server.cluster->slots_to_keys 的跳表，
 * 保存 slot-->key 的映射关系。然后每次有关于 key 的 CMD 到了每个不同的 redis-server（node），
 * 每个 node 都会利用这个 server.cluster->slots_to_keys 检查一下：这个 key 对应的 slot，属不属于现在的 node 管理，
 * 在的话，这个 CMD 交由本 node 处理，不在的话，MOVE 去其他 node 处理。
 * 
 * 每次，所谓的 cluster 仅仅是采用 server.cluster->slots_to_keys 来控制每个 CMD 中的 key 能不能在这个 node 上面执行
 * 而不是说，真的在每个不同的 node 上面，建立很多个 hash slot，然后相应的 key-value pair 都放进这个 slot 里面。
 * slot 并不会真正存放对应 key-value pair 的！！！利用 server.cluster->slots_to_keys 让外界用 redis 的时候，感觉不同 node
 * 具有 slot 划分就可以了。
 * 
 * 说实话，采用 cluster 仅仅是想让不同的 key-value pair 能够分散在不同的 redis-server instance 上面罢了，
 * 其实用户不太关心某一个 key-value 是放在哪一个 node 的哪一个 slot 上面的。
 * （你其实并不关心落在同一个 node 上面不同的 key-value pair，究竟在这个 node 的哪一个 slot 上。
 *   你更关心的是：这个 key-value 在哪个 node 上面）
 * 之所以要引入 slot 这么一个概念，是为了更好的 resharde（重新分片），调整不同 instance 上面的负载，
 * 让负载能够更均匀罢了
*/

/* The ASKING command is required after a -ASK redirection.
 *
 * 客户端在接到 -ASK 转向之后，需要发送 ASKING 命令。
 *
 * The client should issue ASKING before to actually send the command to
 * the target instance. See the Redis Cluster specification for more
 * information. 
 *
 * 客户端应该在向目标节点发送命令之前，向节点发送 ASKING 命令。
 * 具体原因请参考 Redis 集群规范。
 */
void askingCommand(redisClient *c) {

    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    // 打开客户端的标识
    c->flags |= REDIS_ASKING;

    addReply(c,shared.ok);
}

/* The READONLY command is uesd by clients to enter the read-only mode.
 * In this mode slaves will not redirect clients as long as clients access
 * with read-only commands to keys that are served by the slave's master. */
void readonlyCommand(redisClient *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->flags |= REDIS_READONLY;
    addReply(c,shared.ok);
}

/* The READWRITE command just clears the READONLY command state. */
void readwriteCommand(redisClient *c) {
    c->flags &= ~REDIS_READONLY;
    addReply(c,shared.ok);
}

/* Return the pointer to the cluster node that is able to serve the command.
 * For the function to succeed the command should only target either:
 *
 * 1) A single key (even multiple times like LPOPRPUSH mylist mylist).
 * 2) Multiple keys in the same hash slot, while the slot is stable (no
 *    resharding in progress).
 *
 * On success the function returns the node that is able to serve the request.
 * If the node is not 'myself' a redirection must be perfomed. The kind of
 * redirection is specified setting the integer passed by reference
 * 'error_code', which will be set to REDIS_CLUSTER_REDIR_ASK or
 * REDIS_CLUSTER_REDIR_MOVED.
 *
 * When the node is 'myself' 'error_code' is set to REDIS_CLUSTER_REDIR_NONE.
 *
 * If the command fails NULL is returned, and the reason of the failure is
 * provided via 'error_code', which will be set to:
 *
 * REDIS_CLUSTER_REDIR_CROSS_SLOT if the request contains multiple keys that
 * don't belong to the same hash slot.
 *
 * REDIS_CLUSTER_REDIR_UNSTABLE if the request contains mutliple keys
 * belonging to the same slot, but the slot is not stable (in migration or
 * importing state, likely because a resharding is in progress). */
// int *hashslot, int *error_code 都是对外输出的参数
// 进来这个函数，检查一下这个 CMD 中，key 属于相应的 slot 属于谁管理
clusterNode *getNodeByQuery(redisClient *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *error_code) {

    // 初始化为 NULL ，
    // 如果输入命令是无参数命令，那么 n 就会继续为 NULL
    clusterNode *n = NULL;

    robj *firstkey = NULL;
    int multiple_keys = 0;
    multiState *ms, _ms;
    multiCmd mc;
    int i, slot = 0, migrating_slot = 0, importing_slot = 0, missing_keys = 0;

    /* Set error code optimistically for the base case. */
    if (error_code) *error_code = REDIS_CLUSTER_REDIR_NONE;

    /* We handle all the cases as if they were EXEC commands, so we have
     * a common code path for everything */
    // 集群可以执行事务，
    // 但必须确保事务中的所有命令都是针对某个相同的键进行的
    // 这个 if 和接下来的 for 进行的就是这一合法性检测
    if (cmd->proc == execCommand) {
        /* If REDIS_MULTI flag is not set EXEC is just going to return an
         * error. */
        if (!(c->flags & REDIS_MULTI)) return myself;
        ms = &c->mstate;
    } else {
        /* In order to have a single codepath create a fake Multi State
         * structure if the client is not in MULTI/EXEC state, this way
         * we have a single codepath below. */
        ms = &_ms;
        _ms.commands = &mc;
        _ms.count = 1;
        mc.argv = argv;
        mc.argc = argc;
        mc.cmd = cmd;
    }

    /* Check that all the keys are in the same hash slot, and obtain this
     * slot and the node associated. */
    for (i = 0; i < ms->count; i++) {
        struct redisCommand *mcmd;
        robj **margv;
        int margc, *keyindex, numkeys, j;

        mcmd = ms->commands[i].cmd;
        margc = ms->commands[i].argc;
        margv = ms->commands[i].argv;

        // 定位命令的键位置
        keyindex = getKeysFromCommand(mcmd,margv,margc,&numkeys);
        // 遍历命令中的所有键
        for (j = 0; j < numkeys; j++) {
            robj *thiskey = margv[keyindex[j]];
            int thisslot = keyHashSlot((char*)thiskey->ptr,
                                       sdslen(thiskey->ptr));

            if (firstkey == NULL) {
                // 只会进来这个 if-branch 一次，firstkey，slot，n 都是第一个 key 的对应数据
                // 这是事务中第一个被处理的键
                // 获取该键的槽和负责处理该槽的节点
                /* This is the first key we see. Check what is the slot
                 * and node. */
                firstkey = thiskey;
                slot = thisslot;
                n = server.cluster->slots[slot];
                redisAssertWithInfo(c,firstkey,n != NULL);
                /* If we are migrating or importing this slot, we need to check
                 * if we have all the keys in the request (the only way we
                 * can safely serve the request, otherwise we return a TRYAGAIN
                 * error). To do so we set the importing/migrating state and
                 * increment a counter for every missing key. */
                if (n == myself &&
                    server.cluster->migrating_slots_to[slot] != NULL)
                {
                    migrating_slot = 1;
                } else if (server.cluster->importing_slots_from[slot] != NULL) {
                    importing_slot = 1;
                }
            } else {
                /* If it is not the first key, make sure it is exactly
                 * the same key as the first we saw. */
                if (!equalStringObjects(firstkey,thiskey)) {    // key 不同，slot 自然也是不同的
                    if (slot != thisslot) {
                        /* Error: multiple keys from different slots. */
                        getKeysFreeResult(keyindex);
                        if (error_code)
                            *error_code = REDIS_CLUSTER_REDIR_CROSS_SLOT;
                        return NULL;
                    } else {
                        /* Flag this request as one with multiple different
                         * keys. */
                        // key 不同，但是 slot 相同，自然也在同一个 node 中
                        multiple_keys = 1;
                    }
                }
            }

            /* Migarting / Improrting slot? Count keys we don't have. */
            // TODO:(DONE) 这两个是啥？
            // 当要访问的这个 key 对应的 slot 正在发生迁移（无论是 import 还是 migrate），则视为找不到这个 key
            // 因为你现在很难确认这个 key 究竟在新的 node 上面，还是旧的 node 上面
            // 你得采用 ASK 才行
            if ((migrating_slot || importing_slot) &&
                lookupKeyRead(&server.db[0],thiskey) == NULL)
            {
                missing_keys++;
            }
        }
        getKeysFreeResult(keyindex);
    }

    /* No key at all in command? then we can serve the request
     * without redirections or errors. */
    if (n == NULL) return myself;

    /* Return the hashslot by reference. */
    if (hashslot) *hashslot = slot;

    /* This request is about a slot we are migrating into another instance?
     * Then if we have all the keys. */

    /* If we don't have all the keys and we are migrating the slot, send
     * an ASK redirection. */
    if (migrating_slot && missing_keys) {
        // 这个 key 呢，属于 slot，但是现在这个 slot 正在迁移，所以这个 key 究竟在哪一个 node 上面不好说
        // 你得 ask 一下
        if (error_code) *error_code = REDIS_CLUSTER_REDIR_ASK;
        return server.cluster->migrating_slots_to[slot];
    }

    /* If we are receiving the slot, and the client correctly flagged the
     * request as "ASKING", we can serve the request. However if the request
     * involves multiple keys and we don't have them all, the only option is
     * to send a TRYAGAIN error. */
    if (importing_slot &&
        (c->flags & REDIS_ASKING || cmd->flags & REDIS_CMD_ASKING))
    {
        if (multiple_keys && missing_keys) {
            if (error_code) *error_code = REDIS_CLUSTER_REDIR_UNSTABLE;
            return NULL;
        } else {
            return myself;
        }
    }

    /* Handle the read-only client case reading from a slave: if this
     * node is a slave and the request is about an hash slot our master
     * is serving, we can reply without redirection. */
    if (c->flags & REDIS_READONLY &&
        cmd->flags & REDIS_CMD_READONLY &&
        nodeIsSlave(myself) &&
        myself->slaveof == n)
    {
        return myself;
    }

    /* Base case: just return the right node. However if this node is not
     * myself, set error_code to MOVED since we need to issue a rediretion. */
    if (n != myself && error_code) *error_code = REDIS_CLUSTER_REDIR_MOVED;

    // 返回负责处理槽 slot 的节点 n
    // 没有法伤迁移时，新加的 key，旧的 key
    return n;
}
