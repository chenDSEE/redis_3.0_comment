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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "intset.h"
#include "zmalloc.h"
#include "endianconv.h"

/* Note that these encodings are ordered, so:
 * INTSET_ENC_INT16 < INTSET_ENC_INT32 < INTSET_ENC_INT64. */
/*
 * intset 的编码方式
 */
#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

/* Return the required encoding for the provided value. 
 *
 * 返回适用于传入值 v 的编码方式
 * 实际返回的是：保存 value 所需要的内存大小
 *
 * T = O(1)
 */
/**
 * 由于这个是 int 类型的，所以范围区间长这样（v < INT32_MIN   v < INT16_MIN 的原因）
 * INT64_MIN < INT32_MIN < INT16_MIN < 0 < INT16_MAX < INT32_MAX < INT64_MAX
*/
// 先用最大的 int64_t 接收参数（尽可能避免丢失精度）
// 然后在根据不同的数据范围区间，选择尽可能小的那种编码方式（节省空间？）
// TODO: 之所以要采用不同的编码方式是为了什么？节省内存空间？会不会引入内存对齐问题，导致速度下降
static uint8_t _intsetValueEncoding(int64_t v) {
    if (v < INT32_MIN || v > INT32_MAX)
        return INTSET_ENC_INT64;
    else if (v < INT16_MIN || v > INT16_MAX)
        return INTSET_ENC_INT32;
    else
        return INTSET_ENC_INT16;
}

/* Return the value at pos, given an encoding. 
 *
 * 根据给定的编码方式 enc ，返回集合的底层数组在 pos 索引上的元素。
 *
 * 根据跟定的解码方式 uint8_t enc；
 * 获得指定 intset* is 上，index 为 int pos 的数字，并通过指定的 enc 编码方式来返回值
 * T = O(1)
 */
// 返回的应该是什么字节序？当前操作系统使用的字节序
// 编码 跟 字节序 是两回事；
//     编码的 redis 为了节省内存而进行的数字压缩；
//     字节序是操作系统怎么理解 int、long 这些变量在内存里面的值；
static int64_t _intsetGetEncoded(intset *is, int pos, uint8_t enc) {
    int64_t v64;
    int32_t v32;
    int16_t v16;

    // ((ENCODING*)is->contents) 首先将数组转换回被编码的类型
    // 然后 ((ENCODING*)is->contents)+pos 计算出元素在数组中的正确位置
    // 之后 member(&vEnc, ..., sizeof(vEnc)) 再从数组中拷贝出正确数量的字节
    // 如果有需要的话， memrevEncifbe(&vEnc) 会对拷贝出的字节进行大小端转换，转换出操作系统的字节序
    // 最后将值返回
    if (enc == INTSET_ENC_INT64) {
        memcpy(&v64,((int64_t*)is->contents)+pos,sizeof(v64));  // 目的变量，源地址，memcpy 多长
        memrev64ifbe(&v64); // 由于 redis 内部采用小端编码，if is big-end system，则执行转换，将小端换成大端，再传出去
        return v64;
    } else if (enc == INTSET_ENC_INT32) {
        memcpy(&v32,((int32_t*)is->contents)+pos,sizeof(v32));
        memrev32ifbe(&v32);
        return v32;
    } else {
        memcpy(&v16,((int16_t*)is->contents)+pos,sizeof(v16));
        memrev16ifbe(&v16);
        return v16;
    }
}

/* Return the value at pos, using the configured encoding. 
 *
 * 根据集合的编码方式，返回底层数组在 pos 索引上的值
 * 
 * 不自己强行指定编码、解码的方式，采用 intset* is 当前的编解码方式，避免吃下自己强行指定的恶果
 *
 * T = O(1)
 */
static int64_t _intsetGet(intset *is, int pos) {
    return _intsetGetEncoded(is,pos,intrev32ifbe(is->encoding));
    // 之所以要 intrev32ifbe，是因为 is->encoding 采用小端字节序保存
}

/* Set the value at pos, using the configured encoding. 
 *
 * 根据集合的编码方式，将底层数组在 pos 位置上的值设为 value 。
 *
 * T = O(1)
 */
static void _intsetSet(intset *is, int pos, int64_t value) {

    // 取出集合的编码方式
    uint32_t encoding = intrev32ifbe(is->encoding);

    // 根据编码 ((Enc_t*)is->contents) 将数组转换回正确的类型
    // 然后 ((Enc_t*)is->contents)[pos] 定位到数组索引上
    // 接着 ((Enc_t*)is->contents)[pos] = value 将值赋给数组
    // 最后， ((Enc_t*)is->contents)+pos 定位到刚刚设置的新值上 
    // 如果有需要的话， memrevEncifbe 将对值进行大小端转换
    if (encoding == INTSET_ENC_INT64) {
        ((int64_t*)is->contents)[pos] = value;
        memrev64ifbe(((int64_t*)is->contents)+pos); // 保存的永远是小端，所以大端系统要转换之后才能存进去；拿出来的时候，也要转换之后才能用(转成大端，大端系统才能够看懂)
    } else if (encoding == INTSET_ENC_INT32) {
        ((int32_t*)is->contents)[pos] = value;
        memrev32ifbe(((int32_t*)is->contents)+pos);
    } else {
        ((int16_t*)is->contents)[pos] = value;
        memrev16ifbe(((int16_t*)is->contents)+pos);
    }
}

/* Create an empty intset. 
 *
 * 创建并返回一个新的空整数集合
 *
 * T = O(1)
 */
intset *intsetNew(void) {

    // 为整数集合结构分配空间
    // sizeof(intset) = 8 = 4 + 4 = uint32_t + uint32_t = sizeof(encoding) + sizeof(length)
    // new 的时候并不直接分配 数据部分的 内存
    intset *is = zmalloc(sizeof(intset));   // 不包含柔性数字的 int8_t contents[]; 指针的！很省内存

    // 设置初始编码
    is->encoding = intrev32ifbe(INTSET_ENC_INT16);  // 永远都采用小端的方式进行保存

    // 初始化元素数量
    is->length = 0; // 0 大端、小端都是 0

    return is;
}

/* Resize the intset 
 *
 * 调整整数集合的内存空间大小（扩大、缩小都可以）
 *
 * 如果调整后的大小要比集合原来的大小要大，
 * 那么集合中原有元素的值不会被改变。
 *
 * 返回值：调整大小后的整数集合
 *
 * T = O(N)
 */
// TODO: 要是调整的大小变小了，会发生什么？里面的内容会被截断？
// TODO: 不感觉很慢吗? 每次来新的数字，就要扩容一次，搬运一次，为什么不预分配？
//       这是一个有序的 intset，所以即使是预分配，但是每次都会有重排序的问题，即使预分配，实际效果也不会好太多
static intset *intsetResize(intset *is, uint32_t len) {

    // 计算数组的空间大小
    uint32_t size = len*intrev32ifbe(is->encoding);

    // 根据空间大小，重新分配空间
    // 注意这里使用的是 zrealloc ，
    // 所以如果新空间大小比原来的空间大小要大，
    // 那么数组原有的数据会被保留
    is = zrealloc(is,sizeof(intset)+size);  // 别忘了管理部分也要分配内存的

    return is;
}

/* Search for the position of "value".
 * 
 * 在集合 is 的底层数组中查找值 value 所在的索引。
 *
 * Return 1 when the value was found and 
 * sets "pos" to the position of the value within the intset. 
 *
 * 成功找到 value 时，函数返回 1 ，并将 *pos 的值设为 value 所在的索引。
 *
 * Return 0 when the value is not present in the intset 
 * and sets "pos" to the position where "value" can be inserted. 
 *
 * 当在数组中没找到 value 时，返回 0 。
 * 并将 *pos 的值设为 value 可以插入到数组中的位置。(pos 表示：你这个 value 日后要顶替掉现在 pos 上的元素，自己成为 pos 上的元素)
 *
 * T = O(log N)
 */
// 根据 value 采用二分法找 pos
static uint8_t intsetSearch(intset *is, int64_t value, uint32_t *pos) {
    int min = 0, max = intrev32ifbe(is->length)-1, mid = -1;    // 二分后的搜索范围 flag
    int64_t cur = -1;

    /* The value can never be found when the set is empty */
    // 处理 is 为空时的情况
    if (intrev32ifbe(is->length) == 0) {
    /*  */
        if (pos) *pos = 0;
        return 0;
    } else {
        /* Check for the case where we know we cannot find the value,
         * but do know the insert position. */
        if (value > _intsetGet(is,intrev32ifbe(is->length)-1)) {
            // 因为底层数组是有序的，如果 value 比数组中最后一个值（contents[length - 1]）都要大
            // 那么 value 肯定不存在于集合中，
            // 并且应该将 value 添加到底层数组的最末端, 也就是 contents[length]
            if (pos) *pos = intrev32ifbe(is->length);
            return 0;

        } else if (value < _intsetGet(is,0)) {
            // 因为底层数组是有序的，如果 value 比数组中最前一个值都要小
            // 那么 value 肯定不存在于集合中，
            // 并且应该将它添加到底层数组的最前端
            if (pos) *pos = 0;
            return 0;
        }
    }

    // 在有序数组中进行二分查找
    // T = O(log N)
    while(max >= min) {
        mid = (min+max)/2;
        cur = _intsetGet(is,mid);
        if (value > cur) {
            min = mid+1;
        } else if (value < cur) {
            max = mid-1;
        } else {
            break;  // matched
        }
    }

    // 检查是否已经找到了 value
    if (value == cur) {
        if (pos) *pos = mid;
        return 1;
    } else {
        if (pos) *pos = min;
        return 0;
    }
}

/* Upgrades the intset to a larger encoding and inserts the given integer. 
 *
 * 根据值 value 所使用的编码方式，对整数集合的编码进行升级，
 * 并将值 value 添加到升级后的整数集合中。
 *
 * 返回值：添加新元素之后的整数集合
 *
 * T = O(N)
 */
// NOTE: 这个函数的调用，就意味着：要存储的数值 value，必然超出了当前 is->encoding 的限制
// 当前版本并不支持降级操作
static intset *intsetUpgradeAndAdd(intset *is, int64_t value) {
    
    // 当前的编码方式
    uint8_t curenc = intrev32ifbe(is->encoding);

    // 新值所需的编码方式
    uint8_t newenc = _intsetValueEncoding(value);

    // 当前集合的元素数量
    int length = intrev32ifbe(is->length);

    // 根据 value 的值，决定是将它添加到底层数组的最前端还是最后端
    // 注意，因为 value 的编码比集合原有的其他元素的编码都要大
    // 也就暗示着： value 要么大于集合中的所有元素，要么小于集合中的所有元素
    // 因此，value 只能添加到底层数组的最前端或最后端
    // value > 0, 那就是放在最后面，intset 原来的数据不用动
    // value < 0, 那就是放在最前面，intset 原来的所有数据往后挪一位
    int prepend = value < 0 ? 1 : 0;

    /* First set new encoding and resize */
    // 更新集合的编码方式
    is->encoding = intrev32ifbe(newenc);
    // 根据新编码对集合（的底层数组）进行空间调整
    // 加多一个坑位
    // T = O(N)
    // TODO: 为什么这里不进行预分配？
    //       intset 是有序的，预分配效果不会会明显（N --> k * N, 实际上只优化了 N - k 的时间）
    is = intsetResize(is,intrev32ifbe(is->length)+1);

    /* 采用新编码方式，迁移 intset（倒序拷贝） */
    /* Upgrade back-to-front so we don't overwrite values.（从后往前进行更新，不会覆盖原有的值）
     * Note that the "prepend" variable is used to make sure we have an empty
     * space at either the beginning or the end of the intset. */
    // 根据集合原来的编码方式，从底层数组中取出集合元素
    // 然后再将元素以新编码的方式添加到集合中
    // 当完成了这个步骤之后，集合中所有原有的元素就完成了从旧编码到新编码的转换
    // 因为新分配的空间都放在数组的后端，所以程序先从后端向前端移动元素
    // 举个例子，假设原来有 curenc 编码的三个元素，它们在数组中排列如下：
    // | x | y | z | 
    // 当程序对数组进行重分配之后，数组就被扩容了（符号 ？ 表示未使用的内存）：
    // | x | y | z | ? |   ?   |   ?   |
    // 这时程序从数组后端开始，重新插入元素：
    // | x | y | z | ? |   z   |   ?   |
    // | x | y |   y   |   z   |   ?   |
    // |   x   |   y   |   z   |   ?   |
    // 最后，程序可以将新元素添加到最后 ？ 号标示的位置中：
    // |   x   |   y   |   z   |  new  |
    // 上面演示的是新元素比原来的所有元素都大的情况，也即是 prepend == 0
    // 当新元素比原来的所有元素都小时（prepend == 1），调整的过程如下：
    // | x | y | z | ? |   ?   |   ?   |
    // | x | y | z | ? |   ?   |   z   |
    // | x | y | z | ? |   y   |   z   |
    // | x | y |   x   |   y   |   z   |
    // 当添加新值时，原本的 | x | y | 的数据将被新值代替
    // |  new  |   x   |   y   |   z   |
    // T = O(N)
    // 这个 while-loop 是一定要走完的，因为要更新 intset 里面的每一个 element 的 encode
    // FIXME: 确实是存在一次多余的 复制操作：1. 创建新 intset 的时候，复制了一次；2. 重排序的时候，又复制了一次
    while(length--)
        _intsetSet(is,length+prepend,_intsetGetEncoded(is,length,curenc));
        // _intsetSet(新的 intset, 原本 intset[length] 应该在的新位置, 原本 intset[length] 的新 encode 数值)
        // prepend 实际上是预留空位给 value < 0 情况的

    /* Set the NEW value at the beginning or the end. */
    // 设置新值，根据 prepend 的值来决定是添加到数组头还是数组尾
    if (prepend)
        _intsetSet(is,0,value);
    else
        _intsetSet(is,intrev32ifbe(is->length),value);

    // 更新整数集合的元素数量
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);

    return is;
}

/*
 * 向前或先后移动指定索引范围内的数组元素
 *
 * 函数名中的 MoveTail 其实是一个有误导性的名字，
 * 这个函数可以向前或向后移动元素，
 * 而不仅仅是向后
 *
 * 在添加新元素到数组时，就需要进行向后移动，
 * 如果数组表示如下（？表示一个未设置新值的空间）：
 * | x | y | z | ? |
 *     |<----->|
 * 而新元素 n 的 pos 为 1 ，那么数组将移动 y 和 z 两个元素
 * | x | y | y | z |
 *         |<----->|
 * 接着就可以将新元素 n 设置到 pos 上了：
 * | x | n | y | z |
 *
 * 当从数组中删除元素时，就需要进行向前移动，
 * 如果数组表示如下，并且 b 为要删除的目标：
 * | a | b | c | d |
 *         |<----->|
 * 那么程序就会移动 b 后的所有元素向前一个元素的位置，
 * 从而覆盖 b 的数据：
 * | a | c | d | d |
 *     |<----->|
 * 最后，程序再从数组末尾删除一个元素的空间：
 * | a | c | d |
 * 这样就完成了删除操作。
 *
 * T = O(N)
 */
// intset->length 也就 uint32_t, 所以 from  to 的类型没有问题
// from to 实际上是下标，index、pos
// 内部使用的函数，基本会保证：from to 之间的差只会是 1
// 既可向前移动（删掉一个 element），也可以向后移动（加一个 element）
static void intsetMoveTail(intset *is, uint32_t from, uint32_t to) {

    void *src, *dst;

    // 要移动的元素个数
    uint32_t bytes = intrev32ifbe(is->length)-from;

    // 集合的编码方式
    uint32_t encoding = intrev32ifbe(is->encoding);

    // 根据不同的编码
    // src = (Enc_t*)is->contents+from 记录移动开始的位置
    // dst = (Enc_t*)is->contents+to 记录移动结束的位置
    // bytes *= sizeof(Enc_t) 计算一共要移动多少字节
    if (encoding == INTSET_ENC_INT64) {
        src = (int64_t*)is->contents+from;  // 指针一定要进行(int64_t*)强制转换（完成指针加法的步长设定），否则后面的 +from 无法指向想要的内存
        dst = (int64_t*)is->contents+to;
        bytes *= sizeof(int64_t);   // 计算需要移动的内存总量
    } else if (encoding == INTSET_ENC_INT32) {
        src = (int32_t*)is->contents+from;
        dst = (int32_t*)is->contents+to;
        bytes *= sizeof(int32_t);
    } else {
        src = (int16_t*)is->contents+from;
        dst = (int16_t*)is->contents+to;
        bytes *= sizeof(int16_t);
    }

    // 进行移动
    // T = O(N)
    // 当发生内存重叠时，memmove 可以保证重叠内存的拷贝结果正确；但是 memcpy 并不能保证
    memmove(dst,src,bytes);
}

/* Insert an integer in the intset 
 * 
 * 尝试将元素 value 添加到 指定的整数集合 中。
 *
 * *success 的值指示添加是否成功：
 * - 如果添加成功，那么将 *success 的值设为 1 。
 * - 因为元素已存在而造成添加失败时，将 *success 的值设为 0 。
 *
 * T = O(N)
 * 
 * TODO: 为什么要 return intset* is ?
 * 因为一旦发生了 intset 的扩容，你是必然要更换掉外面正在用的 intset* is 指针的
 */
intset *intsetAdd(intset *is, int64_t value, uint8_t *success) {

    // 计算编码 value 所需的长度
    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;

    // 默认设置插入为成功
    if (success) *success = 1;

    /* Upgrade encoding if necessary. If we need to upgrade, we know that
     * this value should be either appended (if > 0) or prepended (if < 0),
     * because it lies outside the range of existing values. */
    // 如果 value 的编码比整数集合现在的编码要大
    // 那么表示 value 必然可以添加到整数集合中
    // 并且整数集合需要对自身进行升级，才能满足 value 所需的编码

    // 当前的 intset 未必能够满足 valuec 的要求，不满足的话，要扩大 intset
    // 一个 intset 里面能够装很多的整型数组，但是每一个都是长短不一的，为了能够快速、统一的进行读取
    // 避免记录每一个 intset 中 entry 的大小，直接采用统一的大小，并取决于这个 intset 里面最大的那个整型数字
    // 这样才能够实现 intset 的随机访问，T = O(1);
    // 毕竟一个整型数组，最多也就 8 Byte 的大小，uint64_t，你在每一个整型数组前面添加一个 uint8_t 来记录当前数字长度是不实际得的
    // 管理节点占比 1/8，浪费得很；而且还不利于随机读取
    if (valenc > intrev32ifbe(is->encoding)) {
        // 需要扩大编码
        /* This always succeeds, so we don't need to curry *success. */
        // T = O(N)
        return intsetUpgradeAndAdd(is,value);
    } else {
        // 不需要扩大编码
        // 运行到这里，表示整数集合现有的编码方式适用于 value

        /* Abort if the value is already present in the set.
         * This call will populate "pos" with the right position to insert
         * the value when it cannot be found. */
        // 在整数集合中查找 value ，看他是否存在：
        // - 如果存在，那么将 *success 设置为 0 ，并返回未经改动的整数集合
        // - 如果不存在，那么可以插入 value 的位置将被保存到 pos 指针中
        //   等待后续程序使用
        if (intsetSearch(is,value,&pos)) {
            // 失败：这个 value 已经存在了
            if (success) *success = 0;
            return is;
        }

        // 运行到这里，表示 value 不存在于集合中
        // 程序需要将 value 添加到整数集合中
    
        // 为 value 在集合中分配空间
        is = intsetResize(is,intrev32ifbe(is->length)+1);
        // 如果新元素不是被添加到底层数组的末尾
        // 那么需要对现有元素的数据进行移动，空出 pos 上的位置，用于设置新值
        // 举个例子
        // 如果数组为：
        // | x | y | z | ? |
        //     |<----->|
        // 而新元素 n 的 pos 为 1 ，那么数组将移动 y 和 z 两个元素
        // | x | y | y | z |
        //         |<----->|
        // 这样就可以将新元素设置到 pos 上了：
        // | x | n | y | z |
        // T = O(N)
        if (pos < intrev32ifbe(is->length)) intsetMoveTail(is,pos,pos+1);
        // 要是 value 是最大的那个数值，那就可以不需要移动，直接把新的 value 放在最后就可以了
    }

    // 将新值设置到底层数组的指定位置中
    _intsetSet(is,pos,value);

    // 增一集合元素数量的计数器
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);

    // 返回添加新元素后的整数集合
    return is;

    /* p.s. 上面的代码可以重构成以下更简单的形式：
    NOTE: 个人觉得不太好，尤其是 C 这一类需要手动管理大量资源的，很容易因为多分支 return，
    导致没有正确 handle 各个退出分支，增加维护难度
    
    if (valenc > intrev32ifbe(is->encoding)) {
        return intsetUpgradeAndAdd(is,value);
    }
     
    if (intsetSearch(is,value,&pos)) {
        if (success) *success = 0;
        return is;
    } else {
        is = intsetResize(is,intrev32ifbe(is->length)+1);
        if (pos < intrev32ifbe(is->length)) intsetMoveTail(is,pos,pos+1);
        _intsetSet(is,pos,value);

        is->length = intrev32ifbe(intrev32ifbe(is->length)+1);
        return is;
    }
    */
}

/* Delete integer from intset 
 *
 * 从整数集合中删除值 value 。
 *
 * *success 的值指示删除是否成功：
 * - 因值不存在而造成删除失败时该值为 0 。
 * - 删除成功时该值为 1 。
 *
 * T = O(N)
 */
intset *intsetRemove(intset *is, int64_t value, int *success) {

    // 计算 value 的编码方式
    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;

    // 默认设置标识值为删除失败
    if (success) *success = 0;

    // 当 value 的编码大小小于或等于集合的当前编码方式（说明 value 有可能存在于集合）
    // 并且 intsetSearch 的结果为真，那么执行删除
    // T = O(log N)
    if (valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,&pos)) {

        // 取出集合当前的元素数量
        uint32_t len = intrev32ifbe(is->length);

        /* We know we can delete */
        // 设置标识值为删除成功
        if (success) *success = 1;

        /* Overwrite value with tail and update length */
        // 如果 value 不是位于数组的末尾
        // 那么需要对原本位于 value 之后的元素进行移动
        //
        // 举个例子，如果数组表示如下，而 b 为删除的目标
        // | a | b | c | d |
        // 那么 intsetMoveTail 将 b 之后的所有数据向前移动一个元素的空间，
        // 覆盖 b 原来的数据
        // | a | c | d | d |
        // 之后 intsetResize 缩小内存大小时，
        // 数组末尾多出来的一个元素的空间将被移除
        // | a | c | d |
        if (pos < (len-1)) intsetMoveTail(is,pos+1,pos);
        // 缩小数组的大小，移除被删除元素占用的空间
        // T = O(N)
        is = intsetResize(is,len-1);
        // 更新集合的元素数量
        is->length = intrev32ifbe(len-1);
    }

#if 0
    else
    {
        // 这个分支实际上就是: valenc > intrev32ifbe(is->encoding)
        // 这时候，实际上是不存在 intset 的，所以直接返回 success = 0;
    }
#endif

    return is;
}

/* Determine whether a value belongs to this set 
 *
 * 检查给定值 value 是否集合中的元素。
 *
 * 是返回 1 ，不是返回 0 。
 *
 * T = O(log N)，二分查找
 */
uint8_t intsetFind(intset *is, int64_t value) {

    // 计算 value 的编码
    uint8_t valenc = _intsetValueEncoding(value);

    // 如果 value 的编码大于集合的当前编码，那么 value 一定不存在于集合
    // 当 value 的编码小于等于集合的当前编码时，
    // 才再使用 intsetSearch 进行查找
    return valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,NULL);
}

/* Return random member 
 *
 * 从整数集合中随机返回一个元素
 *
 * 只能在集合非空时使用
 *
 * T = O(1)
 */
int64_t intsetRandom(intset *is) {
    // intrev32ifbe(is->length) 取出集合的元素数量
    // 而 rand() % intrev32ifbe(is->length) 根据元素数量计算一个随机索引（取余，防止越界）
    // 然后 _intsetGet 负责根据随机索引来查找值
    return _intsetGet(is,rand()%intrev32ifbe(is->length));
}

/* gets the value to the value at the given position. When this position is
 * out of range the function returns 0, when in range it returns 1. */
/* 
 * 取出集合底层数组指定位置中的值，并将它保存到 value 指针中。
 *
 * 如果 pos 没超出数组的索引范围，那么返回 1 ，如果超出索引，那么返回 0 。
 *
 * p.s. 上面原文的文档说这个函数用于设置值，这是错误的。
 *
 * T = O(1)
 */
// intset[pos] 的结果将会放在 int64_t *value，返回去
// 至于 value 会不会越界这一点，只能由调用者来保证了
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value) {

    // pos < intrev32ifbe(is->length) 
    // 检查 pos 是否符合数组的范围
    if (pos < intrev32ifbe(is->length)) {

        // 保存值到指针
        *value = _intsetGet(is,pos);

        // 返回成功指示值
        return 1;
    }

    // 超出索引范围
    return 0;
}

/* Return intset length 
 *
 * 返回整数集合现有的元素个数
 *
 * T = O(1)
 */
uint32_t intsetLen(intset *is) {
    return intrev32ifbe(is->length);
}

/* Return intset blob size in bytes. 
 *
 * 返回整数集合现在占用的字节总数量
 * 这个大小包括整数集合的结构大小（头部），以及整数集合所有元素的总大小
 *
 * T = O(1)
 */
size_t intsetBlobLen(intset *is) {
    return sizeof(intset)+intrev32ifbe(is->length)*intrev32ifbe(is->encoding);
}


// usage: 
// 1) gcc -g zmalloc.c testhelp.h endianconv.c intset.c -D INTSET_TEST_MAIN
// 2) ./a.out

#ifdef INTSET_TEST_MAIN
#include <sys/time.h>

void intsetRepr(intset *is) {
    int i;
    for (i = 0; i < intrev32ifbe(is->length); i++) {
        printf("%lld\n", (uint64_t)_intsetGet(is,i));
    }
    printf("\n");
}

void error(char *err) {
    printf("%s\n", err);
    exit(1);
}

void ok(void) {
    printf("OK\n");
}

long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

#define assert(_e) ((_e)?(void)0:(_assert(#_e,__FILE__,__LINE__),exit(1)))
void _assert(char *estr, char *file, int line) {
    printf("\n\n=== ASSERTION FAILED ===\n");
    printf("==> %s:%d '%s' is not true\n",file,line,estr);
}

intset *createSet(int bits, int size) {
    uint64_t mask = (1<<bits)-1;
    uint64_t i, value;
    intset *is = intsetNew();

    for (i = 0; i < size; i++) {
        if (bits > 32) {
            value = (rand()*rand()) & mask;
        } else {
            value = rand() & mask;
        }
        is = intsetAdd(is,value,NULL);
    }
    return is;
}

void checkConsistency(intset *is) {
    int i;

    for (i = 0; i < (intrev32ifbe(is->length)-1); i++) {
        uint32_t encoding = intrev32ifbe(is->encoding);

        if (encoding == INTSET_ENC_INT16) {
            int16_t *i16 = (int16_t*)is->contents;
            assert(i16[i] < i16[i+1]);
        } else if (encoding == INTSET_ENC_INT32) {
            int32_t *i32 = (int32_t*)is->contents;
            assert(i32[i] < i32[i+1]);
        } else {
            int64_t *i64 = (int64_t*)is->contents;
            assert(i64[i] < i64[i+1]);
        }
    }
}

int main(int argc, char **argv) {
    uint8_t success;
    int i;
    intset *is;
    //sranddev(); I can not find this function in anywhere in Centos 8
    printf("Value encodings: "); {
        assert(_intsetValueEncoding(-32768) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(+32767) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(-32769) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+32768) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483648) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+2147483647) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483649) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+2147483648) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(-9223372036854775808ull) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+9223372036854775807ull) == INTSET_ENC_INT64);
        ok();
    }

    printf("Basic adding: "); {
        is = intsetNew();
        is = intsetAdd(is,5,&success); assert(success);
        is = intsetAdd(is,6,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(!success);
        ok();
    }

    printf("Large number of random adds: "); {
        int inserts = 0;
        is = intsetNew();
        for (i = 0; i < 1024; i++) {
            is = intsetAdd(is,rand()%0x800,&success);
            if (success) inserts++;
        }
        assert(intrev32ifbe(is->length) == inserts);
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int32: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,65535));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-65535));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int32 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Stress lookups: "); {
        long num = 100000, size = 10000;
        int i, bits = 20;
        long long start;
        is = createSet(bits,size);
        checkConsistency(is);

        start = usec();
        for (i = 0; i < num; i++) intsetSearch(is,rand() % ((1<<bits)-1),NULL);
        printf("%ld lookups, %ld element set, %lldusec\n",num,size,usec()-start);
    }

    printf("Stress add+delete: "); {
        int i, v1, v2;
        is = intsetNew();
        for (i = 0; i < 0xffff; i++) {
            v1 = rand() % 0xfff;
            is = intsetAdd(is,v1,NULL);
            assert(intsetFind(is,v1));

            v2 = rand() % 0xfff;
            is = intsetRemove(is,v2,NULL);
            assert(!intsetFind(is,v2));
        }
        checkConsistency(is);
        ok();
    }
}
#endif
