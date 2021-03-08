/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

#ifndef __INTSET_H
#define __INTSET_H
#include <stdint.h>


/**
 * |<--  struct intset  -->|
 * +------------+----------+----------------
 * |  encoding  |  length  |  .........
 * +------------+----------+----------------
 *                         ^
 *                         |
 *                      contents(柔性数值)
*/


// TODO: 画一下整个结构的示意图
// 存进来的一定是小端
// 里面的 value 一定是唯一的，这不是一个 multi-set
typedef struct intset {
    
    // 编码方式（TODO: 出于字节对齐的目的才设置为 uint32_t ?）
    // 直接记录了每一个 整数 的大小（ sizeof(uint64_t) ）
    uint32_t encoding;

    // 集合包含的元素数量（可以保存很多个整型数字的）
    // contents[length - 1] 是最后一个 element
    uint32_t length;

    // 保存元素的数组(按照大小，有序的进行存储，从小到大)
    // TODO: 在不同的编码方式中，int8_t 会造成什么影响？我们又要做什么措施进行补救？
    // 参见 _intsetGetEncoded() 函数，我们总是在 stack 上创建一个变量，然后操作这个变量的内存（执行大小端统一转换）
    // 实际上这里的 int8_t 没有任何表征意义，因为 contents 的类型会影响指针移动的步长，
    // 在实际使用的时候，是要根据 encoding 再次强制转换的
    int8_t contents[];  // 柔性数组

} intset;

intset *intsetNew(void);
intset *intsetAdd(intset *is, int64_t value, uint8_t *success);
intset *intsetRemove(intset *is, int64_t value, int *success);
uint8_t intsetFind(intset *is, int64_t value);
int64_t intsetRandom(intset *is);
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value);
uint32_t intsetLen(intset *is);
size_t intsetBlobLen(intset *is);

#endif // __INTSET_H
