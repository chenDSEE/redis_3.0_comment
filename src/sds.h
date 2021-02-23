/* SDSLib, A C dynamic strings library
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __SDS_H
#define __SDS_H

/*
 * 最大预分配长度
 * 初始长度
 */
#define SDS_MAX_PREALLOC (1024*1024)

#include <sys/types.h>
#include <stdarg.h>

/*
 * 类型别名，用于指向 sdshdr 的 buf 属性
 * 对于调用者来说：只有 buf 部分是可见的，sdshdr 部分是调用者所哦不必关心的内部管理部分，
 * 所以，外界拿着的 sds 部分可以直接拿来进行数据操作，
 * 但是 sds 库作者在进行相应的管理操作时，需要自己手动从 buf 部分，回溯找到 sdshdr 管理部分
 * sds 这个 typedef 一定是一个指针！！！
 * 
 * 另外，char * 作为本体，方便了 sds + 1 这样的数据部分偏移操作
 */
typedef char *sds;  // 作为 动态字符串 的 handler 实在是方便，体积小，拿来直接打印数据，对管理数据没有感知（封装）

/*
 * 保存字符串对象的结构
 */
struct sdshdr {
    
    // buf 中已占用空间的长度（不包含 '\0'）
    // 因为这个节点的存在，让每一次的新内容直接覆盖旧内容成为可能，甚至都不用预先 memset 清空
    // 本质上 '\0' 的添加是不得已而为之，既然如此，这个 \0 就不应该交由调用者去操心
    // 而是交由 sds 库的作者来操心
    // 最好不要用 uint32_t, 不然的话，向下溢出，不好处理
    int len; // （-2,147,483,648 到 2,147,483,647），理论上可以容纳 2047 MB 的 string

    // buf 中剩余可用空间的长度
    int free;

    // 数据空间（柔性数组）
    char buf[]; // typedef char *sds; 总是指向这里，而这里也是外界 handle 的通常操作对象
    // 常见手动，不定长的数据部分，而且是让数据部分直接跟在这里后面，不用跟管理节点分离
    // 不分离，也就意味着可以直接一次性 malloc，而不需要多次 malloc\free
    // NOTE: 上层 sds 操作完全不关系 '\0' 的坑位，全部交由跟 alloc 打交道的底层函数去添加 '\0' 的坑位
};
/**
 * sds 通常长这样
 * | len + free | + buf
 * |  管理部分  | + 实际数据开头(柔性数组作为 data_buf)
 * | 对外界隐藏 | + typedef char *sds；
 * 
 * 这样的操作有一个好处：进行 sds 数据操作的时候，可以直接进行操作，而不用每次都进行结构体转跳计算（经常发生）
 * 只有当进行 sds 的管理（扩容、get_len）等操作的时候，才需要计算偏移，获得管理部分（不经常）
 * 
 * 另外，从封装的角度上来讲，只暴露数据内容部分，而不暴露管理部分是良好的封装，毕竟让每一个人都了解管理部分时不现实的
 * 所以尽可能让专业人士才能拿到管理部分是合理的
*/



/*
 * 返回 sds 实际保存的字符串的长度
 *
 * T = O(1)
 */
static inline size_t sdslen(const sds s) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr))); // 回溯找到 sdshdr 部分，获得长度
    return sh->len;
}

/*
 * 返回 sds 可用空间的长度
 *
 * T = O(1)
 */
static inline size_t sdsavail(const sds s) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr))); // 回溯找到 sdshdr 部分，获得 free 的余量
    return sh->free;
}

sds sdsnewlen(const void *init, size_t initlen);
sds sdsnew(const char *init);
sds sdsempty(void);
size_t sdslen(const sds s);
sds sdsdup(const sds s);
void sdsfree(sds s);
size_t sdsavail(const sds s);
sds sdsgrowzero(sds s, size_t len);
sds sdscatlen(sds s, const void *t, size_t len);
sds sdscat(sds s, const char *t);
sds sdscatsds(sds s, const sds t);
sds sdscpylen(sds s, const char *t, size_t len);
sds sdscpy(sds s, const char *t);

sds sdscatvprintf(sds s, const char *fmt, va_list ap);
#ifdef __GNUC__
sds sdscatprintf(sds s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...);
#endif

sds sdscatfmt(sds s, char const *fmt, ...);
sds sdstrim(sds s, const char *cset);
void sdsrange(sds s, int start, int end);
void sdsupdatelen(sds s);
void sdsclear(sds s);
int sdscmp(const sds s1, const sds s2);
sds *sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count);
void sdsfreesplitres(sds *tokens, int count);
void sdstolower(sds s);
void sdstoupper(sds s);
sds sdsfromlonglong(long long value);
sds sdscatrepr(sds s, const char *p, size_t len);
sds *sdssplitargs(const char *line, int *argc);
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen);
sds sdsjoin(char **argv, int argc, char *sep);

/* Low level functions exposed to the user API */
sds sdsMakeRoomFor(sds s, size_t addlen);
void sdsIncrLen(sds s, int incr);
sds sdsRemoveFreeSpace(sds s);
size_t sdsAllocSize(sds s);

#endif
