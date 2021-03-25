/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * 这个文件实现了一个内存哈希表，
 * 它支持插入、删除、替换、查找和获取随机元素等操作。
 *
 * 哈希表会自动在表的大小的二次方之间进行调整。
 *
 * 键的冲突通过链表来解决。
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

#include <stdint.h>

#ifndef __DICT_H
#define __DICT_H

/*
 * 字典的操作状态
 */
// 操作成功
#define DICT_OK 0
// 操作失败（或出错）
#define DICT_ERR 1

/* Unused arguments generate annoying warnings... */
// 如果字典的私有数据不使用时
// 用这个宏来避免编译器错误
#define DICT_NOTUSED(V) ((void) V)

/*
 * 哈希表节点
 * entry 的管理信息，实际数据部分通常采用指针（u64\s64 除外）
 */
typedef struct dictEntry {
    
    // 键
    void *key;  // 仅仅是指向某一个 robj，也没有必要知晓这个 robj 的大小、编码方式、引用计数

    // 值
    union {
        void *val;  // 仅仅是指向某一个 robj，也没有必要知晓这个 robj 的大小、编码方式、引用计数
        uint64_t u64;
        int64_t s64;
    } v;    // 实际上这里是 8 byte

    // 指向下个哈希表节点，形成单向链表
    struct dictEntry *next;

} dictEntry;


/*
 * 字典类型特定函数
 * TODO: 为什么要传递 privdata 进去，用来做什么？
 * 
 * struct dictType 存在的意义？
 * 因为 REDIS_SET\REDIS_HASH 都会用到 ENCODING_HT 的编码方式，但是 REDIS_SET 只会使用 key 而不使用 value
 * 为了能够实现多态，或者说 模板方法（dictSetKey() 这个宏，作为模板方法） 来统一创建、销毁、复制的 api 函数，所以采用 callback 的方案，在创建的时候就指定好
 */
// 相应 callback 请参考 redis.c
typedef struct dictType {

    // 计算哈希值的函数
    // REDIS_HASH 配置为 dictEncObjHash(), REDIS_SET 配置为 dictEncObjHash()
    unsigned int (*hashFunction)(const void *key);

    // 复制键的函数(以便提供深拷贝函数)
    // REDIS_HASH 配置为 NULL, REDIS_SET 配置为 NULL
    void *(*keyDup)(void *privdata, const void *key);

    // 复制值的函数(以便提供深拷贝函数)
    // REDIS_HASH 配置为 NULL, REDIS_SET 配置为 NULL
    void *(*valDup)(void *privdata, const void *obj);

    // 对比键的函数(重载 operator= 操作符)
    // REDIS_HASH 配置为 dictEncObjKeyCompare(), REDIS_SET 配置为 dictEncObjKeyCompare()
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);

    // 销毁键的函数
    // REDIS_HASH 配置为 dictRedisObjectDestructor(), REDIS_SET 配置为 dictRedisObjectDestructor()
    void (*keyDestructor)(void *privdata, void *key);
    
    // 销毁值的函数
    // REDIS_HASH 配置为 NULL, REDIS_SET 配置为 NULL
    void (*valDestructor)(void *privdata, void *obj);

} dictType;

/**
 * 
 *                                          dictEntry array
 *         +-->  ht[0] ----- table_1  --->  dictEntry_1(dictEntry*)   --->  dictEntry_1(dictEntry node, will make a single link-list)
 *         |   (dictht)   (dictEntry**)     dictEntry_2(dictEntry*)   --->  dictEntry_2(dictEntry node, will make a single link-list)
 *         |                                dictEntry_3(dictEntry*)   --->  dictEntry_3(dictEntry node, will make a single link-list)
 *  dict --+                                ....                      --->  ...
 *         |         
 *         |
 *         +-->  ht[1] ----- table_2  --->  dictEntry_1(dictEntry*)   --->  dictEntry_1(dictEntry node, will make a single link-list)
 *             (dictht)   (dictEntry**)     dictEntry_2(dictEntry*)   --->  dictEntry_2(dictEntry node, will make a single link-list)
 *                                          dictEntry_3(dictEntry*)   --->  dictEntry_3(dictEntry node, will make a single link-list)
 *                                          ....                      --->  ...
*/

/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table. */
/*
 * 哈希表
 *
 * 每个字典都使用两个哈希表，从而实现渐进式 rehash 。
 * 由于渐进式 rehash 的关系，确实是有可能同时再使用两个 hash-table 的
 * 同时也会采用 timer 来定时 rehash，而不是彻底靠 惰性 rehash
 * rehash 的标志放在 dict，因为管理 hash table 的是 dict，单个 hash table 知道自己正在 rehash 又怎样？
 * 两个 hash table 之间并不能相互访问，所以把 rehash 的标志放在更上层的 dict 是合理的
 */
typedef struct dictht { // dict hash table
    
    // 哈希表数组
    dictEntry **table;

    // 哈希表大小
    unsigned long size;
    
    // 哈希表大小掩码，用于计算索引值
    // 总是等于 size - 1
    // 为了加速 hash code --> hash table array index(下标) 的映射计算速度
    unsigned long sizemask;

    // 该哈希表已有节点的数量
    unsigned long used;

} dictht;

/*
 * 字典
 */
typedef struct dict {

    // 类型特定函数
    dictType *type;

    // 私有数据, 当前版本中没有用到，都是设置为 NULL
    // 传递数据给上面的 dictType *type 里面的 callback
    void *privdata;

    // 哈希表
    // 并不是指针，而是老老实实的在自己这里记住一个 dictht 里面包含了那些指针，自己做一个完整且独立的数据备份
    // 比起整个 dict，这点管理内存容量实在是小（里面全是指针），单独做一个备份也没关系，而且更换 hash table 本身就已经
    // 是很耗时间的时候，这一点指针的拷贝并不是瓶颈
    // ht[0] 总是主要使用的 hash table
    // ht[1] 总是作为备用的、rehash 时采用的 hash table
    // 在 rehash 完成的时候，ht[1] 会更新成为 ht[0]
    dictht ht[2];   // 为了渐进式 rehash 准备

    // rehash 索引，rehashidx 前面的 hash table entry 都已经从 ht[0] 搬运到了 ht[1] 了
    // 在 rehash 的途中，rehashidx 将会不断增加
    // 当 rehash 不在进行时，值为 -1
    // 先 resize 备用的 ht，然后才会有 rehash
    int rehashidx; /* rehashing not in progress if rehashidx == -1 */

    // 目前正在运行的安全迭代器的数量
    int iterators; /* number of iterators currently running */

} dict;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. 
 * 
 * Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
/*
 * 字典迭代器
 *
 * 如果 safe 属性的值为 1 ，那么在迭代进行的过程中，
 * 程序仍然可以执行 dictAdd 、 dictFind 和其他函数，对字典进行修改。
 *
 * 如果 safe 不为 1 ，那么程序只能调用 dictNext 对字典进行迭代，
 * 而不对字典进行修改。
 */
/**
 * TODO: 安全迭代器 VS 非安全迭代器
*/
typedef struct dictIterator {
        
    // 被迭代的字典
    dict *d;

    // table ：正在被迭代的哈希表号码，值可以是 0 或 1 。
    // index ：迭代器当前所指向的哈希表索引位置。
    // safe ：标识这个迭代器是否安全
    int table, index, safe;

    // entry ：当前迭代到的节点的指针
    // nextEntry ：当前迭代节点的下一个节点
    //             因为在安全迭代器运作时， entry 所指向的节点可能会被修改，
    //             所以需要一个额外的指针来保存下一节点的位置，
    //             从而防止指针丢失
    // 关联式容器是有可能做到只有当前迭代器失效的
    dictEntry *entry, *nextEntry;

    // TODO: fingerprint 是用来？
    // unsafe iterator fingerprint for misuse detection
    // misuse detection：根据当前 dict 的 used + size 来生成某个时间点上，独一无二的 version
    // 对于 dict 来说，value 的改变并不影响 version，而是改变了内部内存块的操作才会改变 version
    long long fingerprint; /* unsafe iterator fingerprint for misuse detection */
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);

/* This is the initial size of every hash table */
/*
 * 哈希表的初始大小
 */
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
// 给每一个变量都加上括号，确保变量能够在展开之后，依然是同一个变量
// NOTE: '\' 后面不能有任何的空格！！！

// 释放给定字典节点的值
// TODO: 要是没有对应的析构函数怎么办？为什么这里不加上 do {...} while(0) ?
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

// 设置给定字典节点的值，浅拷贝\深拷贝
#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        entry->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        entry->v.val = (_val_); \
} while(0)

// 将一个有符号整数设为节点的值
#define dictSetSignedIntegerVal(entry, _val_) \
    do { entry->v.s64 = _val_; } while(0)

// 将一个无符号整数设为节点的值
#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { entry->v.u64 = _val_; } while(0)

// 释放给定字典节点的键
// TODO: 要是没有对应的析构函数怎么办？为什么这里不加上 do {...} while(0) ?
#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

// 设置给定字典节点的键, 浅拷贝\深拷贝
#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        entry->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        entry->key = (_key_); \
} while(0)

// 比对两个键，有那就用重载的 operator==，没有则比较 key1 key2 这两个变量的值
#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

// 计算给定键的哈希值
// 比起直接操作 union 里面的值，这样反而会更好
// 你可以很简单的写 union 里面的变量名字，但是通过这些宏来访问，调用者就能够很清晰的表达自己的意图
#define dictHashKey(d, key) (d)->type->hashFunction(key)
// 返回获取给定节点的键（入参数：dictEntry* he, he --> hash entry）
#define dictGetKey(he) ((he)->key)
// 返回获取给定节点的值
#define dictGetVal(he) ((he)->v.val)
// 返回获取给定节点的有符号整数值
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
// 返回给定节点的无符号整数值
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
// 返回给定字典的大小（rehash 的时候也要加进来的）
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
// 返回字典的已有节点数量
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
// 查看字典是否正在 rehash（如参数：dict *ht，ht ---> hash table. 实际上就是 d 嘛）
#define dictIsRehashing(ht) ((ht)->rehashidx != -1)

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);
int dictExpand(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
dictEntry *dictReplaceRaw(dict *d, void *key);
int dictDelete(dict *d, const void *key);
int dictDeleteNoFree(dict *d, const void *key);
void dictRelease(dict *d);
dictEntry * dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
int dictGetRandomKeys(dict *d, dictEntry **des, int count);
void dictPrintStats(dict *d);
unsigned int dictGenHashFunction(const void *key, int len);
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void*));
void dictEnableResize(void);
void dictDisableResize(void);
int dictRehash(dict *d, int n);
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(unsigned int initval);
unsigned int dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata);

/* Hash table types */
/* The following is code that we don't use for Redis currently, but that is part
of the library. */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
