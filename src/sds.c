/* SDSLib, A C dynamic strings library
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "sds.h"
#include "zmalloc.h"

/*
 * 根据给定的初始化字符串 init 和字符串长度 initlen
 * 创建一个新的 sds
 *
 * 参数
 *  init ：初始化字符串指针
 *  initlen ：初始化字符串的长度
 *
 * 返回值
 *  sds ：创建成功返回 sdshdr 相对应的 sds
 *        创建失败返回 NULL
 *
 * 复杂度
 *  T = O(N)
 */
/* Create a new sds string with the content specified by the 'init' pointer
 * and 'initlen'.
 * If NULL is used for 'init' the string is initialized with zero bytes.
 *
 * The string is always null-termined (all the sds strings are, always) so
 * even if you create an sds string with:
 *
 * mystring = sdsnewlen("abc",3");
 *
 * You can print the string with printf() as there is an implicit \0 at the
 * end of the string. However the string is binary safe and can contain
 * \0 characters in the middle, as the length is stored in the sds header. */
sds sdsnewlen(const void *init, size_t initlen) {

    struct sdshdr *sh;

    // 根据是否有初始化内容，选择适当的内存分配方式
    // 因为 sds 本身具有管理节点，而且是由 len 这一个「 已使用内存长度 」 的信息节点
    // 所以是可以不预先 memset() 清空的
    // T = O(N)
    if (init) {
        // zmalloc 不初始化所分配的内存
        sh = zmalloc(sizeof(struct sdshdr)+initlen+1);
    } else {
        // zcalloc 将分配的内存全部初始化为 0
        sh = zcalloc(sizeof(struct sdshdr)+initlen+1);
    }

    // 内存分配失败，返回
    if (sh == NULL) return NULL;

    // 设置初始化长度（不包含 '\0'）
    sh->len = initlen;
    // 新 sds 不预留任何空间
    sh->free = 0;
    // 如果有指定初始化内容，将它们复制到 sdshdr 的 buf 中
    // T = O(N)
    if (initlen && init)
        memcpy(sh->buf, init, initlen);
    // 以 \0 结尾
    sh->buf[initlen] = '\0';

    // 返回 buf 部分，而不是整个 sdshdr，毕竟调用者不关心管理部分
    return (char*)sh->buf;
}

/*
 * 创建并返回一个只保存了空字符串 "" 的 sds
 *
 * 返回值
 *  sds ：创建成功返回 sdshdr 相对应的 sds
 *        创建失败返回 NULL
 *
 * 复杂度
 *  T = O(1)
 */
/* Create an empty (zero length) sds string. Even in this case the string
 * always has an implicit null term. */
sds sdsempty(void) {
    return sdsnewlen("",0); // NOTE: 完全不碍事，会自动分配 '\0' 的空间
}

/*
 * 根据给定字符串 init ，创建一个包含同样字符串的 sds
 *
 * 参数
 *  init ：如果输入为 NULL ，那么创建一个空白 sds
 *         否则，新创建的 sds 中包含和 init 内容相同字符串
 *
 * 返回值
 *  sds ：创建成功返回 sdshdr 相对应的 sds
 *        创建失败返回 NULL
 *
 * 复杂度
 *  T = O(N)
 */
/* Create a new sds string starting from a null termined C string. */
sds sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

/*
 * 复制给定 sds 的副本（sds 的拷贝构造）
 *
 * 返回值
 *  sds ：创建成功返回输入 sds 的副本
 *        创建失败返回 NULL
 *
 * 复杂度
 *  T = O(N)
 */
/* Duplicate an sds string. */
sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

/*
 * 释放给定的 sds
 *
 * 复杂度
 *  T = O(N)
 */
/* Free an sds string. No operation is performed if 's' is NULL. */
void sdsfree(sds s) {
    if (s == NULL) return;
    zfree(s-sizeof(struct sdshdr)); // 不然 alloc 用到错误的 cookie 来 free 这一块内存
}

// 未使用函数，可能已废弃
/* Set the sds string length to the length as obtained with strlen(), so
 * considering as content only up to the first null term character.
 *
 * This function is useful when the sds string is hacked manually in some
 * way, like in the following example:
 *
 * s = sdsnew("foobar");
 * s[2] = '\0';
 * sdsupdatelen(s);
 * printf("%d\n", sdslen(s));
 *
 * The output will be "2", but if we comment out the call to sdsupdatelen()
 * the output will be "6" as the string was modified but the logical length
 * remains 6 bytes. */
void sdsupdatelen(sds s) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    int reallen = strlen(s);
    sh->free += (sh->len-reallen);
    sh->len = reallen;
}

/*
 * 在不释放 SDS 的字符串空间的情况下，
 * 重置 SDS 所保存的字符串为空字符串。（避免反复分配内存，浪费时间）
 *
 * 复杂度
 *  T = O(1)
 */
/* Modify an sds string on-place（原地、就地） to make it empty (zero length).
 * However all the existing buffer is not discarded but set as free space
 * so that next append operations will not require allocations up to the
 * number of bytes previously available. */
void sdsclear(sds s) {

    // 取出 sdshdr
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));

    // 重新计算属性
    sh->free += sh->len;
    sh->len = 0;

    // 将结束符放到最前面（相当于惰性地删除 buf 中的内容）
    // 看到了不！这就是 sdshdr 拥有 len 节点的好处，压根就不用再调用一次 memset
    // NOTE: len 永远记录有效数据的使用长度，废弃数据永远不用管（前提：这是一堆二进制数据，永远不需要在 redis-server 里面进行解析）
    // 这个前提其实也可以不要（printf 的时候比较麻烦罢了，要是你已经明确不会有 '\0' 出现的话，'\0' 作为结束 flag 不也挺好的嘛）
    sh->buf[0] = '\0';
}

/* Enlarge the free space at the end of the sds string so that the caller
 * is sure that after calling this function can overwrite up to addlen
 * bytes after the end of the string, plus one more byte for nul term.
 * 
 * Note: this does not change the *length* of the sds string as returned
 * by sdslen(), but only the free buffer space we have. */
/*
 * 对 sds 中 buf 的长度进行扩展，确保在函数执行之后，
 * buf 至少会有 addlen + 1 长度的空余空间
 * （额外的 1 字节是为 \0 准备的）
 *
 * 返回值
 *  sds ：扩展成功返回扩展后的 sds
 *        扩展失败返回 NULL
 *
 * 复杂度
 *  T = O(N)
 * 
 * 这个函数一定要在调用完之后，自己接新的 sds，因为扩容可能发生数据迁移
 */
sds sdsMakeRoomFor(sds s, size_t addlen) {  // 本函数的核心目的是：预留、预分配空间

    struct sdshdr *sh, *newsh;

    // 获取 s 目前的空余空间长度
    size_t free = sdsavail(s);

    size_t len, newlen;

    // s 目前的空余空间已经足够，无须再进行扩展，直接返回，而且 zrealloc 也不能接受拓展长度比原本的小
    if (free >= addlen) return s;

    // 获取 s 目前已占用空间的长度
    len = sdslen(s);
    sh = (void*) (s-(sizeof(struct sdshdr)));

    // 整个 sds 的数据部分，最少需要的总长度
    newlen = (len+addlen);

    // 根据新长度，为 s 分配新空间所需的大小
    if (newlen < SDS_MAX_PREALLOC)
        // 如果新长度小于 SDS_MAX_PREALLOC 
        // 那么为它分配两倍于所需长度的空间
        newlen *= 2;
    else
        // 否则，分配长度为目前长度加上 SDS_MAX_PREALLOC
        newlen += SDS_MAX_PREALLOC;
    // T = O(N)，因为全部内存要逐一搬运
    newsh = zrealloc(sh, sizeof(struct sdshdr)+newlen+1);// 预留 '\0' 的空位，并且完成源数据的迁移工作，旧的部分自己会 free 掉
    // 在这里是自己管理内存，直接跟 alloc 打交道，所以要自己决定要不要预留 +1 给 '\0', 
    // 跟 sdsnewlen() 这样的 “用字符串进行操作指挥” 的方式不一样（sdsnewlen 是更上层，更 OO 的操作）
    // 而这里的扩容，则是更面向过程的内部细节操作
    // 由于内部是会自己把 old_sh 给 free 掉的，所以一定不能写成：sh = zrealloc(sh, sizeof(struct sdshdr)+newlen+1)
    // 这样的话，一旦 zrealloc 失败了，将会导致旧的 sh 发生内存泄漏（也算是异常不安全，破坏了原本的数据）
    // 而且采用末尾 +1 的方式，天然的排除了 size == 0 的坑

    // 内存不足，分配失败，返回
    if (newsh == NULL) return NULL;

    // 更新 sds 的空余长度
    newsh->free = newlen - len;

    // 返回 sds
    // 因为是预留、预分配空间，所以并不需要更新 '\0'
    return newsh->buf;
}

/*
 * 回收 sds 中的空闲空间，
 * 回收不会对 sds 中保存的字符串内容做任何修改。
 *
 * 返回值
 *  sds ：内存调整后的 sds
 *
 * 使得 sds 更为紧凑，但是这个压缩的过程耗时可能比较久
 * 复杂度
 *  T = O(N)
 */
/* Reallocate the sds string so that it has no free space at the end. The
 * contained string remains not altered, but next concatenation operations
 * will require a reallocation.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. 
 * NOTE: 一旦调用了这个函数之后，旧的 sds 指针统统都失效了（调用成功的话）！
 * */
sds sdsRemoveFreeSpace(sds s) {
    struct sdshdr *sh;

    sh = (void*) (s-(sizeof(struct sdshdr)));

    // 进行内存重分配，让 buf 的长度仅仅足够保存字符串内容
    // T = O(N)
    sh = zrealloc(sh, sizeof(struct sdshdr)+sh->len+1);

    // 空余空间为 0
    sh->free = 0;

    return sh->buf;
}

/*
 * 返回给定 sds 分配的内存字节数
 *
 * 复杂度
 *  T = O(1)
 */
/* Return the total size of the allocation of the specifed sds string,
 * including:
 * 1) The sds header before the pointer.
 * 2) The string.
 * 3) The free buffer at the end if any.
 * 4) The implicit null term.
 */
size_t sdsAllocSize(sds s) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));

    return sizeof(*sh)+sh->len+sh->free+1;
}

/* Increment the sds length and decrements the left free space at the
 * end of the string according to 'incr'. Also set the null term
 * in the new end of the string.
 *
 * 根据 incr 参数，增加 sds 的长度，缩减空余空间，
 * 并将 \0 放到新字符串的尾端
 *
 * This function is used in order to fix the string length after the
 * user calls sdsMakeRoomFor(), writes something after the end of
 * the current string, and finally needs to set the new length.
 *
 * 这个函数是在调用 sdsMakeRoomFor() 对字符串进行扩展，
 * 然后用户在字符串尾部写入了某些内容之后，
 * 用来正确更新 free 和 len 属性的。
 *
 * Note: it is possible to use a negative increment in order to
 * right-trim the string.
 *
 * 如果 incr 参数为负数，那么对字符串进行右截断操作。
 *
 * Usage example:
 *
 * Using sdsIncrLen() and sdsMakeRoomFor() it is possible to mount the
 * following schema, to cat bytes coming from the kernel to the end of an
 * sds string without copying into an intermediate buffer:
 *
 * 以下是 sdsIncrLen 的用例：
 *
 * oldlen = sdslen(s);
 * s = sdsMakeRoomFor(s, BUFFER_SIZE);
 * nread = read(fd, s+oldlen, BUFFER_SIZE);
 * ... check for nread <= 0 and handle it ...
 * sdsIncrLen(s, nread);
 *
 * 复杂度
 *  T = O(1)
 */
void sdsIncrLen(sds s, int incr) {  // 这个函数的存在，是为了能够更好的适配 linux 系统本身体统的 read 操作（update sds 的管理节点信息）
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));

    // 确保 sds 空间足够
    assert(sh->free >= incr);

    // 更新属性
    sh->len += incr;
    sh->free -= incr;

    // 这个 assert 其实可以忽略
    // 因为前一个 assert 已经确保 sh->free - incr >= 0 了
    assert(sh->free >= 0);

    // 放置新的结尾符号
    // 因为 make_room_for 并不意味着真的成功填充内容
    // 而且 make_room_for 算是一种预分配空间的操作
    // 所以只能拖延到这里才添加 '\0'
    s[sh->len] = '\0';
}

/* Grow the sds to have the specified length. Bytes that were not part of
 * the original length of the sds will be set to zero.（新扩展的内容将会统一设置为 '\0'）
 *
 * if the specified length is smaller than the current length, no operation
 * is performed. */
/*
 * 将 sds 扩充至指定长度，未使用的空间以 0 字节填充。
 *
 * 返回值
 *  sds ：扩充成功返回新 sds ，失败返回 NULL
 *
 * 复杂度：
 *  T = O(N)
 */
sds sdsgrowzero(sds s, size_t len) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    size_t totlen, curlen = sh->len;

    // 如果 len 比字符串的现有长度小，
    // 那么直接返回，不做动作
    if (len <= curlen) return s;

    // 扩展 sds
    // T = O(N)
    // 这里有可能导致 sds 指针发生了变动
    s = sdsMakeRoomFor(s,len-curlen);   // 里面的 realloc 实际上就实现了异常安全性，所以这里接参数的时候，也要确保异常安全性

    // 如果内存不足，直接返回
    if (s == NULL) return NULL; 
    // FIXME: 这里会不会导致内存泄漏？不会！因为通过 sds 传进来的是一个地址，而不是 sds 这个指针变量本身
    // 所以这里 s = sdsMakeRoomFor(s,len-curlen); 是不会有内存泄漏的，因为 sdsgrowzero 的 sds s（这个函数拿到的仅仅是一个副本罢了） 是一个拷贝过来的指针，而不是传递了一个二级指针
    // 正因为没有传递二级指针进来，所以在 sdsgrowzero 这个函数里面是没有办法操作调用者的 sds s 指针的

    /* Make sure added region doesn't contain garbage */
    // 将新分配的空间用 0 填充，防止出现垃圾内容
    // T = O(N)
    sh = (void*)(s-(sizeof(struct sdshdr)));
    memset(s+curlen,0,(len-curlen+1)); /* also set trailing \0 byte */

    // 更新属性
    totlen = sh->len+sh->free;
    sh->len = len;
    sh->free = totlen-sh->len;

    // 返回新的 sds
    return s;
}

/*
 * 将长度为 len 的字符串 t 追加到 sds 的字符串末尾
 *
 * 返回值
 *  sds ：追加成功返回新 sds ，失败返回 NULL
 *
 * 复杂度
 *  T = O(N)
 */
/* Append the specified binary-safe string pointed by 't' of 'len' bytes to the
 * end of the specified sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. 
 * s = sdscatlen(s,"\\n",2); 像这样的操作，将会对紧凑型的 sds 带来比较大的麻烦
 * */
sds sdscatlen(sds s, const void *t, size_t len) {
    
    struct sdshdr *sh;
    
    // 原有字符串长度
    size_t curlen = sdslen(s);

    // 扩展 sds 空间
    // T = O(N)
    s = sdsMakeRoomFor(s,len);

    // 内存不足？直接返回
    if (s == NULL) return NULL;

    // 复制 t 中的内容到字符串后部
    // T = O(N)
    sh = (void*) (s-(sizeof(struct sdshdr)));
    memcpy(s+curlen, t, len);   // 二进制安全的拷贝

    // 更新属性
    sh->len = curlen+len;
    sh->free = sh->free-len;

    // 添加新结尾符号
    s[curlen+len] = '\0';

    // 返回新 sds
    return s;
}

/*
 * 将给定字符串 t 追加到 sds 的末尾
 * 
 * 返回值
 *  sds ：追加成功返回新 sds ，失败返回 NULL
 *
 * 复杂度
 *  T = O(N)
 */
/* Append the specified null termianted C string to the sds string 's'.
 * 字符串版本（作为来源的 const char *t 非二进制安全）
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscat(sds s, const char *t) {
    return sdscatlen(s, t, strlen(t));
}

/*
 * 将另一个 sds 追加到一个 sds 的末尾
 * 
 * 返回值
 *  sds ：追加成功返回新 sds ，失败返回 NULL
 *
 * 复杂度
 *  T = O(N)
 */
/* Append the specified sds 't' to the existing sds 's'.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscatsds(sds s, const sds t) {
    return sdscatlen(s, t, sdslen(t));
}

/*
 * 将字符串 t 的前 len 个字符复制到 sds s 当中，
 * 并在字符串的最后添加终结符。(覆盖写)
 *
 * 如果 sds 的长度少于 len 个字符，那么扩展 sds
 *
 * 复杂度
 *  T = O(N)
 *
 * 返回值
 *  sds ：复制成功返回新的 sds ，否则返回 NULL
 */
/* Destructively modify the sds string 's' to hold the specified binary
 * safe string pointed by 't' of length 'len' bytes. */
sds sdscpylen(sds s, const char *t, size_t len) {

    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));

    // sds 现有 buf 的长度
    size_t totlen = sh->free+sh->len;

    // 如果 s 的 buf 长度不满足 len ，那么扩展它
    if (totlen < len) {
        // T = O(N)
        s = sdsMakeRoomFor(s,len-sh->len);
        if (s == NULL) return NULL;
        sh = (void*) (s-(sizeof(struct sdshdr)));
        totlen = sh->free+sh->len;
    }

    // 复制内容
    // T = O(N)
    memcpy(s, t, len);

    // 添加终结符号
    s[len] = '\0';

    // 更新属性
    sh->len = len;
    sh->free = totlen-len;

    // 返回新的 sds
    return s;
}

/*
 * 将字符串复制到 sds 当中，
 * 覆盖原有的字符。
 *
 * 如果 sds 的长度少于字符串的长度，那么扩展 sds 。
 *
 * 复杂度
 *  T = O(N)
 *
 * 返回值
 *  sds ：复制成功返回新的 sds ，否则返回 NULL
 */
/* Like sdscpylen() but 't' must be a null-termined string so that the length
 * of the string is obtained with strlen(). */
sds sdscpy(sds s, const char *t) {
    return sdscpylen(s, t, strlen(t));
}

/* Helper for sdscatlonglong() doing the actual number -> string
 * conversion. 's' must point to a string with room for at least
 * SDS_LLSTR_SIZE bytes.
 *
 * long long 的数字向 string 转换
 * T = O(N + 0.5 * N)
 * 
 * The function returns the lenght of the null-terminated string
 * representation stored at 's'. */
#define SDS_LLSTR_SIZE 21   // 9,223,372,036,854,775,807 加上 '\0' 刚好 20； 可能还有符号位，所以是 21
int sdsll2str(char *s, long long value) {
    char *p, aux;
    unsigned long long v;
    size_t l;

    /* Generate the string representation, this method produces
     * an reversed string. */

    // step 1：统一符号位
    v = (value < 0) ? -value : value;

    // step 2：从最后面开始，逐位取出，并转化为对应的单字符，存到 sds.buf 里面
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);

    // step 3：符号位存在末尾，因为待会还要反转的 string 的
    if (value < 0) *p++ = '-';

    /* Compute length and add null term. */
    l = p-s;    // 返回，便于后续更新 sds.len 的值
    *p = '\0';  // 末尾的惯例

    /* Reverse the string.（交换开头和末尾） */
    p--;    // '\0' 不参与交换
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }

    return l;
}

/* Identical sdsll2str(), but for unsigned long long type. */
int sdsull2str(char *s, unsigned long long v) {
    char *p, aux;
    size_t l;

    /* Generate the string representation, this method produces
     * an reversed string. */
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);

    /* Compute length and add null term. */
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Create an sds string from a long long value. It is much faster than:
 *
 * sdscatprintf(sdsempty(),"%lld\n", value);
 */
// 根据输入的 long long 值 value ，创建一个 SDS
sds sdsfromlonglong(long long value) {
    char buf[SDS_LLSTR_SIZE];   // long long 的极限是 9,223,372,036,854,775,807 加上 '\0' 刚好 20； 可能还有符号位，所以是 21
    int len = sdsll2str(buf,value);

    return sdsnewlen(buf,len);
}

/* 
 * 打印函数，被 sdscatprintf 所调用
 * 1）完成 fmt 的格式化输出
 * 2）将 fmt 格式化后的字符串，加入 sds s 里面
 * 
 * T = O(N^2)
 */
/* Like sdscatpritf() but gets va_list instead of being variadic. */
sds sdscatvprintf(sds s, const char *fmt, va_list ap) {
    va_list cpy;    // 避免在 vsnprintf 尝试的时候，破坏了传进来的 ap
    char staticbuf[1024], *buf = staticbuf, *t; // 创建三个变量，且用 buf 指向 staticbuf，出参 t 暂时不处理
    size_t buflen = strlen(fmt)*2; // TODO: 为什么至少要两倍的长度？

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    if (buflen > sizeof(staticbuf)) {
        // fmt 太长，不得不用 heap 上面的内存（防止 stack overflow 炸了）
        buf = zmalloc(buflen);
        if (buf == NULL) return NULL;
    } else {
        // strlen(fmt) * 2 <= 1024
        // staticbuf 能够处理两倍的 fmt
        buflen = sizeof(staticbuf);
    }

    /* Try with buffers two times bigger every time we fail to
     * fit the string in the current buffer size. */
    while(1) {
        // 最后一个坑位：buf[buflen-1]; 为什么还要在向前挪一个位呢？
        // 因为 vsnprintf 自身会在最末尾追加 '\0'(不超过 buflen 的前提下)
        // 所以，当 printf 之后，消耗的 len 长于 buflen 的话，就会发生截断
        //（vsnprinf 会在 buf[buflen-1] 的位置写上 '\0'，而这时候，我们自己加入了 buf[buflen] 就不会再是 '\0' 了）
        // 你说要是刚好怎么办？ignore，依旧扩大两倍（因为 strlen 相当耗时间）
        // TODO: 为什么不利用 vsnprinf 返回来的实际写入数量呢？你可以自己测试一下，速度差距有多少？
        buf[buflen-2] = '\0';
        va_copy(cpy,ap);
        // T = O(N)
        vsnprintf(buf, buflen, fmt, cpy);

        if (buf[buflen-2] != '\0') {    // buf 太小了，扩大后重试
            if (buf != staticbuf) zfree(buf);
            buflen *= 2;
            buf = zmalloc(buflen);
            if (buf == NULL) return NULL;
            continue;
        }
        break;
    }

    /* Finally concat the obtained string to the SDS string and return it. */
    t = sdscat(s, buf);
    if (buf != staticbuf) zfree(buf);
    return t;
}

/*
 * 打印任意数量个字符串，并将这些字符串追加到给定 sds 的末尾
 *
 * T = O(N^2)
 */
/* Append to the sds string 's' a string obtained using printf-alike format
 * specifier.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsempty("Sum is: ");
 * s = sdscatprintf(s,"%d+%d = %d",a,b,a+b).
 *
 * Often you need to create a string from scratch with the printf-alike
 * format. When this is the need, just use sdsempty() as the target string:
 *
 * s = sdscatprintf(sdsempty(), "... your format ...", args);
 */
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap; // 实际上底层是指针
    char *t;
    va_start(ap, fmt);
    // T = O(N^2)
    t = sdscatvprintf(s,fmt,ap);    // 将格式 fmt、可变参数列进一步传递
    va_end(ap);
    return t;
}

/* This function is similar to sdscatprintf, but much faster as it does
 * not rely on sprintf() family functions implemented by the libc that
 * are often very slow. Moreover directly handling the sds string as
 * new data is concatenated provides a performance improvement.
 *
 * However this function only handles an incompatible subset of printf-alike
 * format specifiers:
 *
 * %s - C String
 * %S - SDS string
 * %i - signed int
 * %I - 64 bit signed integer (long long, int64_t)
 * %u - unsigned int
 * %U - 64 bit unsigned integer (unsigned long long, uint64_t)
 * %% - Verbatim "%" character.
 */
sds sdscatfmt(sds s, char const *fmt, ...) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t initlen = sdslen(s);
    const char *f = fmt;
    int i;
    va_list ap;

    va_start(ap,fmt);
    f = fmt;    /* Next format specifier byte to process. */
    i = initlen; /* Position of the next byte to write to dest str. */
    while(*f) {
        char next, *str;
        size_t l;
        long long num;
        unsigned long long unum;

        /* Make sure there is always space for at least 1 char. */
        if (sh->free == 0) {
            s = sdsMakeRoomFor(s,1);
            sh = (void*) (s-(sizeof(struct sdshdr)));
        }

        switch(*f) {    // 轮询扫描，以 % 作为触发点
        case '%':
            next = *(f+1);  // 实际格式字符，本次要处理的是哪一种格式？
            f++;    // 跳过 %
            switch(next) {
            case 's':
            case 'S':
                str = va_arg(ap,char*); // 拿到本次要处理的变量
                l = (next == 's') ? strlen(str) : sdslen(str);
                if (sh->free < l) {
                    s = sdsMakeRoomFor(s,l);
                    sh = (void*) (s-(sizeof(struct sdshdr)));
                }
                memcpy(s+i,str,l);  // s+i 当前用到了哪里？
                sh->len += l;
                sh->free -= l;
                i += l; // 总是等于 sh->len
                break;
            case 'i':
            case 'I':
                if (next == 'i')
                    num = va_arg(ap,int);
                else
                    num = va_arg(ap,long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsll2str(buf,num);
                    if (sh->free < l) {
                        s = sdsMakeRoomFor(s,l);
                        sh = (void*) (s-(sizeof(struct sdshdr)));
                    }
                    memcpy(s+i,buf,l);
                    sh->len += l;
                    sh->free -= l;
                    i += l;
                }
                break;
            case 'u':
            case 'U':
                if (next == 'u')
                    unum = va_arg(ap,unsigned int);
                else
                    unum = va_arg(ap,unsigned long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsull2str(buf,unum);
                    if (sh->free < l) {
                        s = sdsMakeRoomFor(s,l);
                        sh = (void*) (s-(sizeof(struct sdshdr)));
                    }
                    memcpy(s+i,buf,l);
                    sh->len += l;
                    sh->free -= l;
                    i += l;
                }
                break;
            default: /* Handle %% and generally %<unknown>. */
                s[i++] = next;
                sh->len += 1;
                sh->free -= 1;
                break;
            }
            break;
        default:
            s[i++] = *f;    // 原样照抄这一个字符
            sh->len += 1;
            sh->free -= 1;
            break;
        }
        f++;    // 下一个待检查字符
    }
    va_end(ap);

    /* Add null-term */
    s[i] = '\0';
    return s;
}

/*
 * 对 sds 左右两端进行修剪，清除其中 cset 指定的所有字符
 *
 * 比如 sdsstrim(xxyyabcyyxy, "xy") 将返回 "abc"（x、y 这两个 单字符，只要出现了，都会被删除）
 *
 * TODO: 试一下这个 case 会发生什么
 * s = sdsnew("AA...AA.a.aa.aHelloAAAWorld     :::");
 * s = sdstrim(s,"A. :");
 * printf("%s\n", s);
 * 
 * 
 * 复杂性：
 *  T = O(M*N)，M 为 SDS 长度， N 为 cset 长度。
 */
/* Remove the part of the string from left and from right composed just of
 * contiguous characters found in 'cset', that is a null terminted C string.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("AA...AA.a.aa.aHelloWorld     :::");
 * s = sdstrim(s,"A. :");
 * printf("%s\n", s);
 *
 * Output will be just "Hello World".
 */
sds sdstrim(sds s, const char *cset) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    char *start, *end, *sp, *ep;    // start\end 是固定不动的上界与下界，sp\ep 则是浮动的指针
    size_t len;

    // 设置和记录指针
    sp = start = s;
    ep = end = s+sdslen(s)-1;

    // 修剪, T = O(N^2)
    // 只会恰头，或者去尾，不会动中间的
    while(sp <= end && strchr(cset, *sp)) sp++;     // 从左到右，在 sds 上面找到以一个不在 cset 出现过的字符，并用 sp 记录这个位置
    while(ep > start && strchr(cset, *ep)) ep--;    // 从右到左，在 sds 上面找到以一个不在 cset 出现过的字符，并用 ep 记录这个位置

    // 计算 trim 完毕之后剩余的字符串长度
    len = (sp > ep) ? 0 : ((ep-sp)+1);
    
    // 如果有需要，前移字符串内容
    // T = O(N)
    if (sh->buf != sp) memmove(sh->buf, sp, len);   // sh->buf == sp 的话，更新 free\len 和 '\0' 就好了

    // 添加终结符
    sh->buf[len] = '\0';

    // 更新属性
    sh->free = sh->free+(sh->len-len);  // 新释放了 sh->len - len 长度
    sh->len = len;

    // 返回修剪后的 sds
    return s;
}

/*
 * 按索引对截取 sds 字符串的其中一段
 * start 和 end 都是闭区间（包含在内）
 *
 * 索引从 0 开始，最大为 sdslen(s) - 1
 * 索引可以是负数， sdslen(s) - 1 == -1
 *
 * 复杂度
 *  T = O(N)
 */
/* Turn the string into a smaller (or equal) string containing only the
 * substring specified by the 'start' and 'end' indexes.
 *
 * start and end can be negative, where -1 means the last character of the
 * string, -2 the penultimate character, and so forth.
 *
 * The interval is inclusive, so the start and end characters will be part
 * of the resulting string.
 *
 * The string is modified in-place.
 *
 * Example:
 *
 * s = sdsnew("Hello World");
 * sdsrange(s,1,-1); => "ello World"
 */
void sdsrange(sds s, int start, int end) {  // 截取后的结果直接放在原本的 sds 里面（截取操作只会缩减，不会增加）
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t newlen, len = sdslen(s);

    if (len == 0) return;
    if (start < 0) {
        start = len+start;  // 负数，直接反向计算
        if (start < 0) start = 0;
    }
    if (end < 0) {
        end = len+end;
        if (end < 0) end = 0;
    }
    newlen = (start > end) ? 0 : (end-start)+1; // 别忘了留 '\0'
    if (newlen != 0) {
        if (start >= (signed)len) {
            newlen = 0;
        } else if (end >= (signed)len) {
            end = len-1;
            newlen = (start > end) ? 0 : (end-start)+1;
        }
    } else {
        start = 0;  // 这样就不会移动字符串了
    }

    // 如果有需要，对字符串进行移动
    // T = O(N)
    if (start && newlen) memmove(sh->buf, sh->buf+start, newlen);

    // 添加终结符，当发生 start > end, start 在 end 的右边时，整个 sds 将会被截断为 len = 1（'\0'）
    sh->buf[newlen] = 0;

    // 更新属性
    sh->free = sh->free+(sh->len-newlen);
    sh->len = newlen;
}

/*
 * 将 sds 字符串中的所有字符转换为小写
 *
 * T = O(N)
 */
/* Apply tolower() to every character of the sds string 's'. */
void sdstolower(sds s) {
    int len = sdslen(s), j;
    // len 直接拿出来就好，省的每 for-loop 一次都要算一次
    for (j = 0; j < len; j++) s[j] = tolower(s[j]);
}

/*
 * 将 sds 字符串中的所有字符转换为大写
 *
 * T = O(N)
 */
/* Apply toupper() to every character of the sds string 's'. */
void sdstoupper(sds s) {
    int len = sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = toupper(s[j]);
}

/*
 * 对比两个 sds ， strcmp 的 sds 版本
 *
 * 返回值
 *  int ：相等返回 0 ，s1 较大返回正数， s2 较大返回负数
 *
 * T = O(N)
 */
/* Compare two sds strings s1 and s2 with memcmp().
 *
 * Return value:
 *
 *     1 if s1 > s2.
 *    -1 if s1 < s2.
 *     0 if s1 and s2 are exactly the same binary string.
 *
 * If two strings share exactly the same prefix, but one of the two has
 * additional characters, the longer string is considered to be greater than
 * the smaller one. */
int sdscmp(const sds s1, const sds s2) {
    size_t l1, l2, minlen;
    int cmp;

    l1 = sdslen(s1);
    l2 = sdslen(s2);
    minlen = (l1 < l2) ? l1 : l2;
    cmp = memcmp(s1,s2,minlen);

    // 当 s1 比 s2 长度不一时，且 s1 s2 的前缀都一样的时候，就会发生 cmp == 0, 但是 l1 != l2 的情况
    // 因为 cmp = memcmp(s1,s2,minlen)， 只对比了 minlen
    if (cmp == 0) return l1-l2;

    return cmp;
}

/* Split 's' with separator in 'sep'. An array
 * of sds strings is returned. *count will be set
 * by reference to the number of tokens returned.
 *
 * 使用分隔符 sep 对 s 进行分割，返回一个 sds 字符串的数组。
 * *count 会被设置为返回数组元素的数量。
 *
 * On out of memory, zero length string, zero length
 * separator, NULL is returned.
 *
 * 如果出现内存不足、字符串长度为 0 或分隔符长度为 0
 * 的情况，返回 NULL
 *
 * Note that 'sep' is able to split a string using
 * a multi-character separator. For example
 * sdssplit("foo_-_bar","_-_"); will return two
 * elements "foo" and "bar".
 *
 * 注意分隔符可以的是包含多个字符的字符串
 *
 * This version of the function is binary-safe but
 * requires length arguments. sdssplit() is just the
 * same function but for zero-terminated strings.
 *
 * // TODO: 这个放出去的内存，日后怎么回收回来？调用 sdsfreesplitres() 回收内存
 * 
 * 这个函数接受 len 参数，因此它是二进制安全的。
 * （文档中提到的 sdssplit() 已废弃）
 *
 * T = O(N^2)
 */
// const char *s 是 C 风格的字符串，可以接受 sdshdr->buf，所以采用补上 len
// 而且这个函数能够将 client 发过来的命令，转换为 sds："SET KEY-string VALUE-string" 转化为 3 个 sds 存起来
sds *sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count) { // const char *sep 意味着可以是一个字符串；int *count 也是一个出参数
    int elements = 0, slots = 5, start = 0, j;
    sds *tokens;    // 二级指针数组，而且因为要传递出去的缘故，只能在 heap

    if (seplen < 1 || len < 0) return NULL; // 无效输入

    tokens = zmalloc(sizeof(sds)*slots);    // 预留 5 个数字作为 slot 待填充
    if (tokens == NULL) return NULL;

    if (len == 0) {
        *count = 0;
        return tokens;  // TODO: 这个放出去的内存，日后怎么回收回来？调用 sdsfreesplitres() 回收内存
    }
    
    // T = O(N^2), s = 11--1--11; sep = --
    // 11--1--11 |     (len = 9)
    // --        | j = 0; start = 0; (seplen = 2, sep 可以在 len 中挪动7次 + 原地不动还有一次， len - seplen + 1)
    //  --       | j = 1; start = 0; 
    //   --      | j = 2; start = 0 ==> ;  
    //    --     | j = 3; start = ;   
    //     --    | j = 4; start = ;   
    //      --   | j = 5; start = ;   
    //       --  | j = 6; start = ;   
    //        -- | j = 7; start = ;   
    for (j = 0; j < (len-(seplen-1)); j++) {
        /* make sure there is room for the next element and the final one */
        if (slots < elements+2) {   // 2 = 1 + 1; 一个是为了当前可能的坑位，另一个只是为了末尾的字符串
            sds *newtokens;

            slots *= 2; // 直接翻倍，而不是一个个来拓展
            newtokens = zrealloc(tokens,sizeof(sds)*slots); // 扩容的同时，将旧的管理节点信息也搬进去，旧节点的内存会在里面顺手 free 掉
            if (newtokens == NULL) goto cleanup;
            tokens = newtokens; // 新的管理节点
        }
        /* search the separator */
        // T = O(N)
        // （sep 只有一个 char 且 s 当前的 char 就是 sep） || （从 s 的当前位置开始，匹配 seplen 长度，能够 match 到 sep 字符串）
        // sep 为单个字符的时候，因为短路原则的关系，不会落入 memcmp 里面
        if ((seplen == 1 && *(s+j) == sep[0]) || (memcmp(s+j,sep,seplen) == 0)) {   // 成功匹配 sep
            //  这个范围就是 j - start
            // |  ^  |
            // 1111111--1--11
            // ^
            // |
            // start
            tokens[elements] = sdsnewlen(s+start,j-start);  // 新的一部分 sds 从 s + start 开始，长度为 j - start(start 是上次 match 完毕之后的位置，j 是当前位置，一减，就是需要创建为 sds 的长度了)
            if (tokens[elements] == NULL) goto cleanup;
            elements++;
            start = j+seplen;   // 接下来的 seplen 都是 sep，跳过
            j = j+seplen-1; /* skip the separator */    // NOTE: -1 是为了配合 for 循环的 j++
        }
    }
    /* Add the final element. We are sure there is room in the tokens array. */
    tokens[elements] = sdsnewlen(s+start,len-start);    // 剩下的全放在这个 sds，有可能只剩下 '\0', 但是 sdsnewlen 对于字符串 "" 是安全的
    if (tokens[elements] == NULL) goto cleanup;
    elements++;
    *count = elements;
    return tokens;  // TODO: 这个放出去的内存，日后怎么回收回来？调用 sdsfreesplitres() 回收内存

cleanup:    // 仅仅作为统一的失败处理，算是个不错的工程实践
    {
        int i;
        for (i = 0; i < elements; i++) sdsfree(tokens[i]);  // free 每一个实际的
        zfree(tokens);  // tokens 也要 free 掉，这是最开始申请的管理内存，二级指针数组，指向所有实际的 sds 内存块
        *count = 0;
        return NULL;
    }
}

/*
 * 释放 tokens 数组中 count 个 sds
 * 回收 sdssplitlen() 放出去的内存
 * 对于 NULL 输入，安全
 * T = O(N^2)
 */
/* Free the result returned by sdssplitlen(), or do nothing if 'tokens' is NULL. */
void sdsfreesplitres(sds *tokens, int count) {
    if (!tokens) return;
    while(count--)
        sdsfree(tokens[count]); // 内部 sds
    zfree(tokens);  // 管理节点
}

/*
 * 将长度为 len 的字符串 p 以带引号（quoted）的格式
 * 追加到给定 sds 的末尾
 *
 * T = O(N)
 * 
 * 输出结果形如：「 "0104069000\x00\x01\x113\x00\x01" 」, \x00 就是 '\0' 的肉眼可见表示
 *              「 "aimer\r\n" 」
 */
/* Append to the sds string "s" an escaped string representation where
 * all the non-printable characters (tested with isprint()) are turned into
 * escapes in the form "\n\r\a...." or "\x<hex-number>".
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
// TODO: 看看这个函数究竟用来干嘛？转换了什么？写一下案例? 
// 为什么这个函数出去的 sds 并不需要在末尾添加 '\0' ? return sdscatlen(s,"\"",1); 会加上；
sds sdscatrepr(sds s, const char *p, size_t len) {  // len 是转移字符串 p 的长度，所以 p 里面哪怕有 '\0' 都是安全的，依旧完成转换任务，输出 \x00

    s = sdscatlen(s,"\"",1);    // 将长度为 len 的字符串 p 以带引号（quoted）的格式，开头的 "

    while(len--) {
        switch(*p) { 
        case '\\':
        case '"':
            s = sdscatprintf(s,"\\%c",*p);  // 最后变成字符串 「 \" 」
            break;
        case '\n': s = sdscatlen(s,"\\n",2); break; // 将转移字符换成字符串：1 Byte 的 char 变量 ===> 2 Byte 的字符串
        case '\r': s = sdscatlen(s,"\\r",2); break;
        case '\t': s = sdscatlen(s,"\\t",2); break;
        case '\a': s = sdscatlen(s,"\\a",2); break;
        case '\b': s = sdscatlen(s,"\\b",2); break;
        default:
            if (isprint(*p))    // 当前这个 char 可以 printf（返回值不为 0）
                s = sdscatprintf(s,"%c",*p);
            else
                // snprintf 的封装，\\ 就是 \, 为了表示、让我们肉眼可见一个编号为 0x01 的 ASCII 字符
                // 不得不从一个 byte，扩增成 \xff 的格式（4 Byte）来让我们看见这个 byte 究竟是什么
                s = sdscatprintf(s,"\\x%02x",(unsigned char)*p);

            break;
        }
        p++;
    }

    return sdscatlen(s,"\"",1); // 将长度为 len 的字符串 p 以带引号（quoted）的格式，末尾的 "
    // 而且这个函数会在末尾自己添加 '\0'，而且还会 update sdshdr->len
}

/* Helper function for sdssplitargs() that returns non zero if 'c'
 * is a valid hex digit. */
/*
 * 如果 c 为十六进制符号的其中一个，返回正数
 *
 * T = O(1)
 */
int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* Helper function for sdssplitargs() that converts a hex digit into an
 * integer from 0 to 15 */
/*
 * 将十六进制符号转换为 10 进制
 *
 * T = O(1)
 */
int hex_digit_to_int(char c) {
    switch(c) {
    case '0': return 0;
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'a': case 'A': return 10;
    case 'b': case 'B': return 11;
    case 'c': case 'C': return 12;
    case 'd': case 'D': return 13;
    case 'e': case 'E': return 14;
    case 'f': case 'F': return 15;
    default: return 0;
    }
}

/* Split a line into arguments, where every argument can be in the
 * following programming-language REPL-alike form:
 *
 * 将一行文本分割成多个参数，每个参数可以有以下的类编程语言 REPL 格式：
 *
 * foo bar "newline are supported\n" and "\xff\x00otherstuff"
 *
 * The number of arguments is stored into *argc, and an array
 * of sds is returned.
 *
 * 参数的个数会保存在 *argc 中，函数返回一个 sds 数组。
 *
 * The caller should free the resulting array of sds strings with
 * sdsfreesplitres().
 *
 * 调用者应该使用 sdsfreesplitres() 来释放函数返回的 sds 数组。
 *
 * Note that sdscatrepr() is able to convert back a string into
 * a quoted string in the same format sdssplitargs() is able to parse.
 *
 * sdscatrepr() 可以将一个字符串转换为一个带引号（quoted）的字符串，
 * 这个带引号的字符串可以被 sdssplitargs() 分析。
 *
 * The function returns the allocated tokens on success, even when the
 * input string is empty, or NULL if the input contains unbalanced
 * quotes or closed quotes followed by non space characters
 * as in: "foo"bar or "foo'
 *
 * 即使输入出现空字符串， NULL ，或者输入带有未对应的括号，
 * 函数都会将已成功处理的字符串先返回。
 *
 * 这个函数主要用于 config.c 中对配置文件进行分析。
 * 还有 redis-server 解析 client-socket 发送过来的 inline 格式协议内容
 * 例子：
 *  sds *arr = sdssplitargs("timeout 10086\r\nport 123321\r\n");
 * "timeout 10086\r\nport 123321\r\n" 这个字符串是已经被 sdscatrepr() 处理过的(将文本文件里面的不可打印字符转换出来)
 * 会得出
 *  arr[0] = "timeout"
 *  arr[1] = "10086"
 *  arr[2] = "port"
 *  arr[3] = "123321"
 *
 * T = O(N^2)
 */
// TODO: 测试一下这个函数
sds *sdssplitargs(const char *line, int *argc) {    // line 里面已经将所有的转移字符转为字面意义了（所有只有最末尾才会有 字符 '\0'；其他的都是字符串 "\0", 字符 '\' 和 '0'）
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while(1) {

        /* skip blanks */
        // 跳过空白，遇上 '\0' 则结束，意味着全是空格
        // T = O(N)
        while(*p && isspace(*p)) p++;

        if (*p) {
            /* get a token */
            int inq=0;  /* set to 1 if we are in "quotes" */
            int insq=0; /* set to 1 if we are in 'single quotes' */
            int done=0;

            if (current == NULL) current = sdsempty();  // 创建一个新的 sds 节点

            // T = O(N)
            while(!done) {  // 
                if (inq) {  // 开始
                    if (*p == '\\' && *(p+1) == 'x' &&
                                             is_hex_digit(*(p+2)) &&
                                             is_hex_digit(*(p+3)))
                    {   // \x<hex_num> 格式
                        unsigned char byte; // 0x00

                        byte = (hex_digit_to_int(*(p+2))*16)+   // 填充高位 0x10
                                hex_digit_to_int(*(p+3));       // 填充低位 0x01
                        current = sdscatlen(current,(char*)&byte,1);    // （四个 char 变成了一个 char）保存的时候，自然是采用更为节省内存空间的 ASCII 格式
                        p += 3; // 下一个可能的格式
                    } else if (*p == '\\' && *(p+1)) {
                        // \n \r \t \b \a    \其他 则原样打印
                        char c;

                        p++;
                        switch(*p) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'b': c = '\b'; break;
                        case 'a': c = '\a'; break;
                        default: c = *p; break;
                        }
                        current = sdscatlen(current,&c,1);  // 两个 char 变成了 一个 char，这不就更节省内存空间了吗！毕竟这时候并不需要理解内部含义。人类也不用读取；所以直接变成 ASCII 来存就好了
                    } else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        // '"' + ' ' 才是唯一正确的格式
                        if (*(p+1) && !isspace(*(p+1))) goto err;

                        done=1; // 末尾了，结束，并退出
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;   // 没有成对的双引号
                    } else {
                        current = sdscatlen(current,p,1);   // 普通字符，往当前的 sds 里面加就完事了
                    }
                } else if (insq) {
                    if (*p == '\\' && *(p+1) == '\'') {
                        // "\'" 这一个字符串
                        p++;
                        current = sdscatlen(current,"'",1);
                    } else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        // 结束的引号，后面一定要有空格
                        if (*(p+1) && !isspace(*(p+1))) goto err;   // TODO: 啥意思？
                        done=1; // 单引号作为结尾
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else {    // 只有在一开始会进来，剩下的基本不会进来，除非是 done 了
                    switch(*p) {
                    case ' ':   // 理论上这些都不存在了，都被转成普通的字符串了，'\0' 始终在最后
                    case '\n':
                    case '\r':
                    case '\t':
                    case '\0':
                        done=1;
                        break;
                    case '"':   // 一开始先来这里
                        inq=1;
                        break;
                    case '\'':
                        insq=1;
                        break;
                    default:
                        current = sdscatlen(current,p,1);
                        break;
                    }
                }
                if (*p) p++;    // 在 while(!done) 中 ++
            }
            /* add the token to the vector */
            // T = O(N)
            vector = zrealloc(vector,((*argc)+1)*sizeof(char*));    // 每一个 vector[] 成员都是一个 char*, 指向散落在 heap 的 sds
            // FIXME: 确实，作为管理节点的 vector 是有可能分配失败的，vector[] 是有可能 crash 的
            vector[*argc] = current;    // vector 本身是 stack 变量
            (*argc)++;  // 因为转换成功多少个，就会返回多少个
            current = NULL; // 存好了，可以悬空了
        } else {
            /* Even on empty input string return something not NULL. */
            // TODO: 为什么 ？
            if (vector == NULL) vector = zmalloc(sizeof(void*));
            return vector;
        }
    }

err:
    while((*argc)--)
        sdsfree(vector[*argc]); // 逐个释放
    zfree(vector);  // 管理节点释放
    if (current) sdsfree(current);  // 未完成的释放
    *argc = 0;
    return NULL;
}

/* Modify the string substituting all the occurrences of the set of
 * characters specified in the 'from' string to the corresponding character
 * in the 'to' array.
 *
 * 将字符串 s 中，
 * 所有在 from 中出现的字符，替换成 to 中的字符
 *
 * For instance: sdsmapchars(mystring, "ho", "01", 2)
 * will have the effect of turning the string "hello" into "0ell1".
 *
 * 比如调用 sdsmapchars(mystring, "ho", "01", 2)
 * 就会将 "hello" 转换为 "0ell1"
 *
 * The function returns the sds string pointer, that is always the same
 * as the input pointer since no resize is needed. 
 * 因为无须对 sds 进行大小调整，
 * 所以返回的 sds 输入的 sds 一样
 *
 * T = O(N^2)
 */
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen) {
    size_t j, i, l = sdslen(s);

    // 遍历输入字符串
    for (j = 0; j < l; j++) {
        // 遍历映射
        for (i = 0; i < setlen; i++) {
            // 替换字符串
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

/* Join an array of C strings using the specified separator (also a C string).
 * Returns the result as an sds string. */
sds sdsjoin(char **argv, int argc, char *sep) {
    sds join = sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscat(join, argv[j]);
        if (j != argc-1) join = sdscat(join,sep);   // 只要不是最后一个，就要加上 sep
    }
    return join;
}

// usage: 
// 1) gcc -g zmalloc.c testhelp.h sds.c -D SDS_TEST_MAIN
// 2) ./a.out
//
// PS: gcc -g zmalloc.c testhelp.h sds.c -D SDS_TEST_MAIN -fsanitize=address -fno-omit-frame-pointer -O1 
// you can use this commond to detecte memory problem. If commond failed, place update your gcc.
#ifdef SDS_TEST_MAIN
#include <stdio.h>
#include "testhelp.h"
#include "limits.h"

int main(void) {
    {
        struct sdshdr *sh;
        sds x = sdsnew("foo"), y;

        test_cond("Create a string and obtain the length",
            sdslen(x) == 3 && memcmp(x,"foo\0",4) == 0) // 用的是 memcmp，所以要把 \0 写一写，确保每一个 sds 后面都是有 \0 的，这样才能把 \0 也加入检查中

        sdsfree(x);
        x = sdsnewlen("foo",2); // 只创建 2 个 byte 大小的 sds
        test_cond("Create a string with specified length",
            sdslen(x) == 2 && memcmp(x,"fo\0",3) == 0)

        x = sdscat(x,"bar");
        test_cond("Strings concatenation",
            sdslen(x) == 5 && memcmp(x,"fobar\0",6) == 0);

        x = sdscpy(x,"a");
        test_cond("sdscpy() against an originally longer string",
            sdslen(x) == 1 && memcmp(x,"a\0",2) == 0)

        x = sdscpy(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        test_cond("sdscpy() against an originally shorter string",
            sdslen(x) == 33 &&
            memcmp(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0",33) == 0)

        sdsfree(x);
        x = sdscatprintf(sdsempty(),"%d",123);
        test_cond("sdscatprintf() seems working in the base case",
            sdslen(x) == 3 && memcmp(x,"123\0",4) == 0)

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN,LLONG_MAX); // cat 是指链接的意思
        test_cond("sdscatfmt() seems working in the base case",
            sdslen(x) == 60 &&
            memcmp(x,"--Hello Hi! World -9223372036854775808,"
                     "9223372036854775807--",60) == 0)

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
        test_cond("sdscatfmt() seems working with unsigned numbers",
            sdslen(x) == 35 &&
            memcmp(x,"--4294967295,18446744073709551615--",35) == 0)

        sdsfree(x);
        x = sdsnew("xxcixyaoyyy");
        sdstrim(x,"xy");
        test_cond("sdstrim() correctly trims characters",
            sdslen(x) == 6 && memcmp(x,"cixyao\0",7) == 0)

        sdsfree(x);
        x = sdsnew("xxciao");
        sdstrim(x,"xy");
        test_cond("sdstrim() correctly trims characters",
            sdslen(x) == 4 && memcmp(x,"ciao\0",5) == 0)

        sdsfree(x);
        x = sdsnew("xxciaoyyy");
        sdstrim(x,"xy");
        test_cond("sdstrim() correctly trims characters",
            sdslen(x) == 4 && memcmp(x,"ciao\0",5) == 0)

        y = sdsdup(x);
        sdsrange(y,1,1);
        test_cond("sdsrange(...,1,1)",
            sdslen(y) == 1 && memcmp(y,"i\0",2) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,-1);
        test_cond("sdsrange(...,1,-1)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,-2,-1);
        test_cond("sdsrange(...,-2,-1)",
            sdslen(y) == 2 && memcmp(y,"ao\0",3) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,2,1);
        test_cond("sdsrange(...,2,1)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,100);
        test_cond("sdsrange(...,1,100)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,100,100);
        test_cond("sdsrange(...,100,100)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("foo");
        y = sdsnew("foa");
        test_cond("sdscmp(foo,foa)", sdscmp(x,y) > 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) == 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("aar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) < 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar");
        y = sdsnew("bar_more");
        test_cond("sdscmp(bar,bar_more)", sdscmp(x,y) < 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar_more");
        y = sdsnew("bar");
        test_cond("sdscmp(bar_more,bar)", sdscmp(x,y) > 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnewlen("\a\n\0foo\r",7);
        y = sdscatrepr(sdsempty(),x,sdslen(x));
        test_cond("sdscatrepr(...data...)",
            memcmp(y,"\"\\a\\n\\x00foo\\r\"\0",16) == 0)  // 为什么后面没有 '\0'
        // y 就是字符串 "\a\n\x00foo\r" 15 个字符
        {
            int oldfree;

            sdsfree(x);
            x = sdsnew("0");
            sh = (void*) (x-(sizeof(struct sdshdr)));
            test_cond("sdsnew() free/len buffers", sh->len == 1 && sh->free == 0);
            x = sdsMakeRoomFor(x,1);
            sh = (void*) (x-(sizeof(struct sdshdr)));
            test_cond("sdsMakeRoomFor()", sh->len == 1 && sh->free > 0);
            oldfree = sh->free;
            x[1] = '1'; // x[2] 在 update 为 '\0' 会更完善
            sdsIncrLen(x,1);    // 手动 update sdshdr->len, 因为手动更新的 x[1] 的值
            test_cond("sdsIncrLen() -- content", x[0] == '0' && x[1] == '1');
            test_cond("sdsIncrLen() -- len", sh->len == 2);
            test_cond("sdsIncrLen() -- free", sh->free == oldfree-1);

        }

        sdsfree(x);
        sdsfree(y);
    }
    test_report()
    return 0;
}
#endif
