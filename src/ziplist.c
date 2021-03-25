/* The ziplist is a specially encoded dually linked list that is designed
 * to be very memory efficient. 
 *
 * Ziplist 是为了尽可能地节约内存而设计的特殊编码 双端 链表。
 *
 * It stores both strings and integer values,
 * where integers are encoded as actual integers instead of a series of
 * characters. 
 *
 * Ziplist 可以储存字符串值和整数值，
 * 其中，整数值被保存为实际的整数，而不是字符数组。
 *
 * It allows push and pop operations on either side of the list
 * in O(1) time. However, because every operation requires a reallocation of
 * the memory used by the ziplist, the actual complexity is related to the
 * amount of memory used by the ziplist.
 *
 * Ziplist 允许在列表的两端进行 O(1) 复杂度的 push 和 pop 操作。
 * 但是，因为这些操作都需要对整个 ziplist 进行内存重分配，
 * 所以实际的复杂度和 ziplist 占用的内存大小有关。(T = O(N))
 * 
 * ziplist 的出现，是为了尽可能节省内存，用时间换取空间的
 * 因此，像 STL 的双向链表那样，采用组的形式来进行预分配、转跳，都是违背初衷的
 * 而且，STL 那样的管理方式，未免耗费过多比例的内存在管理部分上，对于小对象来说，相当不利
 *
 * ----------------------------------------------------------------------------
 *
 * ZIPLIST OVERALL LAYOUT:
 * Ziplist 的整体布局：
 *
 * The general layout of the ziplist is as follows:
 * 以下是 ziplist 的一般布局：
 * 
* 
 * 
 * |<----   ziplist header    ---->|
 * |  4 byte  |  4 byte  |  2 byte |  ..............  |  1 byte |
 * +----------+----------+---------+------------------+---------+
 * | zlbytes  |  zltail  |  zllen  |  ... entrys ...  |  zlend  |
 * +----------+----------+---------+------------------+---------+
 * 
 * 完整布局
 * |<----   ziplist header    ---->|<---                  entry-0                 -->|<---                  entry-1                  -->|
 *                                 |<----    meta-data\header   ---->|<-- optional ->|
 * |  4 byte  |  4 byte  |  2 byte |   1 or 5 byte   |                                                                                         |  1 byte |
 * +----------+----------+---------+-----------------+--------------+----------------+-----------------+--------------+----------------+.......+---------+
 * | zlbytes  |  zltail  |  zllen  |    prevlen-0    |  encoding-0  |  entry-data-0  |    prevlen-1    |  encoding-1  |  entry-data-1  |.......|  zlend  |
 * +----------+----------+---------+-----------------+--------------+----------------+-----------------+--------------+----------------+.......+---------+
 * 
 * 
 * 
 * zlbytes(4 byte, uint32_t): the number of bytes of the whole ziplist
 * zltail (4 byte, uint32_t): the offset to the last entry in this ziplist(最开头是 offset 0)
 * zllen  (2 byte, uint16_t): all the number of entries in this ziplist
 * zlend  (1 byte,  uint8_t): a single byte special value, 0xFF
 * 
 * 
 * <zlbytes><zltail><zllen><entry>...<entry><zl-end>
 *
 * zl --> zip-list
 * <zlbytes> is an unsigned integer to hold " the number of bytes " that the
 * ziplist occupies. This value needs to be stored to be able to resize the
 * entire structure without the need to traverse it first.
 *
 * <zlbytes> 是一个无符号整数，保存着 ziplist 使用的内存数量。
 *
 * 通过这个值，程序可以直接对 ziplist 的内存大小进行调整，
 * 而无须为了计算 ziplist 的内存大小而遍历整个列表。（原本要知道这个链表的大小，只能遍历，才知道有多少个节点）
 *
 * <uint32_t zltail> is " the offset to the last entry " in the list. This allows a pop
 * operation on the far side of the list without the need for full traversal.
 *
 * <uint32_t zltail> 保存着到达列表中最后一个节点的偏移量。四舍五入就是尾指针，只不过 ziplist 的内存都是分配在一起的
 * 所以才能通过 offset 来访问尾指针；（这样可以一定程度上减少内存碎片）
 *
 * 这个偏移量使得对表尾的 pop 操作可以在无须遍历整个列表的情况下进行。
 *
 * <uint16_t zllen> is " the number of entries." When this value is larger than 2**16-2,
 * we need to traverse the entire list to know how many items it holds.
 *
 * <zllen> 保存着列表中的节点数量。
 * 
 * 当 zllen 保存的值大于 2**16-2 () 时，TODO: 怎么算的？ziplist 怎么直到自己当前的状态是需要进行遍历的？
 * 程序需要遍历整个列表才能知道列表实际包含了多少个节点。
 * 这个遍历操作一旦需要触发，将会是一个很耗时的阻塞操作
 *
 * <zlend> is a single byte special value, equal to 255, which indicates the
 * end of the list.
 *
 * <zlend> 的长度为 1 字节，值为 255 ，标识列表的末尾。
 */

/**
 * the follow comment from latest version ziplist.c(https://github.com/redis/redis/blob/unstable/src/ziplist.c), after redis 6.0
 * 
 * 
 * ZIPLIST ENTRIES
 * ===============
 *
 * Every entry in the ziplist is prefixed by metadata that contains two pieces
 * of information. First, the length of the previous entry is stored to be
 * able to traverse the list from back to front. Second, the entry encoding is
 * provided. It represents the entry type, integer or string, and in the case
 * of strings it also represents the length of the string payload.
 * So a complete entry is stored like this:
 *
 * ZIPLIST ENTRIES:
 * |<---                  entry-0                 -->|<---                  entry-1                  -->|
 * |<----    meta-data\header   ---->|<-- optional ->|
 * |   1 or 5 byte   |                               
 * +-----------------+--------------+----------------+-----------------+--------------+----------------+.......+---------+
 * |    prevlen-0    |  encoding-0  |  entry-data-0  |    prevlen-1    |  encoding-1  |  entry-data-1  |.......|  zlend  |
 * +-----------------+--------------+----------------+-----------------+--------------+----------------+.......+---------+
 *
 *                                                        prevlen-1 = sizeof(prevlen-0) 
 *                                                                    + sizeof(encoding-0) 
 *                                                                    + sizeof(entry-data-0)
 * 
 * <prevlen> <encoding> <entry-data>
 *
 * record for:
 * 1) prevlen: 用来 traverse the list from back to front.
 *             NOTE: 这是用来从后往前遍历的，并不是用来记录当前 entry 长度的
 *             TODO: 最开头的那一个 entry 怎么设置 prevlen ？
 * 2) encoding: 标识后续数据的编码方式：integer or string；(entry-data 的种类 + 本 entry-data 的长度)
 *              string 的话，还包含 string 的长度
 *              integer 且数值足够小的话，将会直接在这里记录 entry-data，而不再需要 entry-date 这一部分了
 * 
 * FORMAT:
 * prevlen:
 * 1) prevlen < 254 bytes, | 8 bits counter | (total: 1 byte)
 *    上一个 entry 全部内容加起来小于 254 byte 的情况下
 *    1 byte（max is 253, 0xfd, 1111 1101）, as a uint8_t counter;
 *    
 * 2) prevlen >= 254 bytes, | 0xFE as flag | + | 4 byte as counter | (total: 5 byte)
 *    上一个 entry 全部内容加起来大于等于 254 byte 的情况下, 5 byte,
 *    第一个 byte 必然是 0xFE, 254; 剩下的 4 byte 都用来进行计数
 *    
 * 
 * <prevlen> 
 * ============
 * Sometimes the encoding represents the entry itself, like for small integers
 * as we'll see later. In such a case the <entry-data> part is missing, and we
 * could have just:
 *
 * <prevlen> <encoding>
 *
 * The length of the previous entry, <prevlen>, is encoded in the following way:
 * 1) If this length is smaller than 254 bytes, it will only consume a single
 * byte representing the length as an unsinged 8 bit integer. 
 * 2) When the length
 * is greater than or equal to 254, it will consume 5 bytes. The first byte is
 * set to 254 (FE) to indicate a larger value is following. The remaining 4
 * bytes take the length of the previous entry as value.
 *
 * So practically an entry is encoded in the following way:
 *
 * <prevlen from 0 to 253> <encoding> <entry>
 *
 * Or alternatively if the previous entry length is greater than 253 bytes
 * the following encoding is used:
 *
 * 0xFE <4 bytes unsigned little endian prevlen> <encoding> <entry>
 *
 * 
 * <encoding> 
 * ============
 * 参考函数：zipEncodeLength()
 * 
 * The encoding field of the entry depends on the content of the
 * entry. 
 * 1) When the entry is a string, the first 2 bits of the encoding first
 * byte will hold the type of encoding used to store the length of the string,
 * followed by the actual length of the string. 
 * 2) When the entry is an integer
 * the first 2 bits are both set to 1. The following 2 bits are used to specify
 * what kind of integer will be stored after this header. 
 * 
 * encoding 的前两个 bits 用来表征这个 entry 记录的 entry-data 数据类型：
 * 00\01\10 string; (total 1\2\5 bytes)
 * 11 integer;      (total 1\2\3\4\5\9 bytes)
 * 
 * An overview of the
 * different types and encodings is as follows. The first byte is always enough
 * to determine the kind of entry.
 * 
 * |00pppppp| - encoding total: 1 byte, (+ <= 63 bytes string-entry-data)
 *      String value with length less than or equal to 63 bytes (6 bits).
 *      "pppppp" represents the unsigned 6 bit length.
 * |01pppppp|qqqqqqqq| - encoding total: 2 bytes, (+ <= 16383 bytes string-entry-data)
 *      String value with length less than or equal to 16383 bytes (14 bits).
 *      IMPORTANT: The 14 bit number is stored in big endian.
 * |10000000|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt| - encoding total: 5 bytes, (+ 16384 ~ 2^32-1 string-entry-data)
 *      String value with length greater than or equal to 16384 bytes.
 *      Only the 4 bytes following the first byte represents the length
 *      up to 2^32-1. The 6 lower bits of the first byte are not used and
 *      are set to zero.|10 000000|
 *      IMPORTANT: The 32 bit number(|q qqqqqqq|) is stored in big endian.（第 32 个 bit 存储在大端上，而不是 32bits 的数字采用大端字节序来保存）
 * 
 * |11000000| + int16_t - encoding + integer-entry-data = 3 bytes
 *      Integer encoded as int16_t (2 bytes).
 * |11010000| + int32_t - encoding + integer-entry-data = 5 bytes
 *      Integer encoded as int32_t (4 bytes).
 * |11100000| + int64_t - encoding + integer-entry-data = 9 bytes
 *      Integer encoded as int64_t (8 bytes).
 * |11110000| + int24_t - encoding + integer-entry-data = 4 bytes
 *      Integer encoded as 24 bit signed (3 bytes).
 * |11111110| +  int8_t - encoding + integer-entry-data = 2 bytes
 *      Integer encoded as 8 bit signed (1 byte).
 * |1111xxxx| - (with xxxx between 0001 and 1101) immediate 4 bit integer.(后面不用再附带 entry-data 了，integer-entry-data 直接记在 encoding 里面)
 *      Unsigned integer from 0 to 12. The encoded value is actually from
 *      1 to 13 because 0000 and 1111 can not be used, so 1 should be
 *      subtracted from the encoded 4 bit value to obtain the right value.
 * 
 * |11111111| - End of ZIPLIST special entry.（整个 ziplist 只会有一个，而不是每一个 entry 后面都加 zlend）
 *
 * Like for the ziplist header, all the integers are represented in little
 * endian byte order, even when this code is compiled in big endian systems.
 * 
*/

/**
 * EXAMPLES OF ACTUAL ZIPLISTS
 * ===========================
 *
 * 初始化状态
 *    [0b 00 00 00] [0a 00 00 00] [00 00] [ff]
 *          |             |          |      |
 *       zlbytes        zltail    zllen    end
 * 
 * 单字符 "2" "5" 将会被转化为更高效的编码方式进行存储（integer，|1111xxxx|）
 * The following is a ziplist containing the two elements representing
 * the strings "2" and "5". It is composed of 15 bytes, that we visually
 * split into sections:
 *
 *  [0f 00 00 00] [0c 00 00 00] [02 00] [00 f3] [02 f6] [ff]  （十六进制）
 *        |             |          |       |       |     |
 *     zlbytes        zltail    zllen     "2"     "5"   end
 *
 * zlbytes: The first 4 bytes represent the number 15, that is the number of bytes
 * the whole ziplist is composed of. 
 * 
 * zltail: The second 4 bytes are the offset
 * at which the last ziplist entry is found, that is 12, in fact the
 * last entry, that is "5", is at offset 12 inside the ziplist.
 * 
 * zllen： The next 16 bit integer represents the number of elements inside the
 * ziplist, its value is 2 since there are just two elements inside.
 * 
 * entry(<prevlen> <encoding> <entry-data>):
 * "2" foramt |1111xxxx|:
 * |0000 0000|1111 1101|
 *      |         |
 *  prevlen    encoding
 * 
 * Finally "00 f3" is the first entry representing the number 2. It is
 * composed of the previous entry length, which is zero because this is
 * our first entry, and the byte F3 which corresponds to the encoding
 * |1111xxxx| with xxxx between 0001 and 1101. We need to remove the "F"
 * higher order bits 1111, and subtract 1 from the "3", so the entry value
 * is "2". （暂时无视标志位的 1111，然后看后面的 4 bits）
 * 
 * 
 * "5" foramt |1111xxxx|:
 * |0000 0010|1111 0110|
 *      |         |
 *  prevlen    encoding
 * 
 * The next entry has a prevlen of 02, since the first entry is
 * composed of exactly two bytes. The entry itself, F6, is encoded exactly
 * like the first entry, and 6-1 = 5, so the value of the entry is 5.
 * .
 * Finally the special entry FF signals the end of the ziplist.
 *
 * Adding another element to the above string with the value "Hello World"
 * allows us to show how the ziplist encodes small strings. We'll just show
 * the hex dump of the entry itself. Imagine the bytes as following the
 * entry that stores "5" in the ziplist above:
 *
 * [02]        [0b] [48 65 6c 6c 6f 20 57 6f 72 6c 64]
 *  |           |
 * prevlen   encoding
 * 
 * The first byte, 02, is the length of the previous entry. The next
 * byte represents the encoding in the pattern |00pppppp| that means
 * that the entry is a string of length <pppppp>, so 0B means that
 * an 11 bytes string follows. From the third byte (48) to the last (64)
 * there are just the ASCII characters for "Hello World".
*/


/* ZIPLIST ENTRIES:
 * ZIPLIST 节点：
 *
 * Every entry in the ziplist is prefixed by a header that contains two pieces
 * of information. First, the length of the previous entry is stored to be
 * able to traverse the list from back to front. Second, the encoding with an
 * optional string length of the entry itself is stored.
 *
 * 每个 ziplist 节点的前面都带有一个 header ，这个 header 包含两部分信息：
 *
 * 1)前置节点的长度，在程序从后向前遍历时使用。
 *
 * 2)当前节点所保存的值的类型和长度。
 *
 * The length of the previous entry is encoded in the following way:
 * If this length is smaller than 254 bytes, it will only consume a single
 * byte that takes the length as value. When the length is greater than or
 * equal to 254, it will consume 5 bytes. The first byte is set to 254 to
 * indicate a larger value is following. The remaining 4 bytes take the
 * length of the previous entry as value.
 *
 * 编码前置节点的长度的方法如下：
 *
 * 1) 如果前置节点的长度小于 254 字节，那么程序将使用 1 个字节（0xFE, uint8_t - 1）来保存这个长度值。
 *
 * 2) 如果前置节点的长度大于等于 254 字节，那么程序将使用 5 个字节来保存这个长度值：
 *    a) 第 1 个字节的值将被设为 254(0xFE)，用于标识这是一个 5 字节长的长度值。
 *    b) 之后的 4 个字节则用于保存前置节点的实际长度。(0xFF FF FF FF)
 *
 * The other header field of the entry itself depends on the contents of the
 * entry. When the entry is a string, the first 2 bits of this header will hold
 * the type of encoding which used to store the length of the string, followed by the
 * actual length of the string. When the entry is an integer the first 2 bits
 * are both set to 1. The following 2 bits are used to specify what kind of
 * integer will be stored after this header. An overview of the different
 * types and encodings is as follows:
 *
 * header 另一部分的内容和节点所保存的值有关。
 *
 * 1) 如果节点保存的是字符串值，
 *    那么这部分 header 的头 2 个位将保存编码字符串长度所使用的类型，
 *    而之后跟着的内容则是字符串的实际长度。
 *
 * |00pppppp| - 1 byte
 *      String value with length less than or equal to 63 bytes (6 bits).
 *      字符串的长度小于或等于 63 字节。(6 bits 的最大值)
 * |01pppppp|qqqqqqqq| - 2 bytes
 *      String value with length less than or equal to 16383 bytes (14 bits).
 *      字符串的长度小于或等于 16383 字节。
 * |10______|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt| - 5 bytes
 *      String value with length greater than or equal to 16384 bytes.
 *      字符串的长度大于或等于 16384 字节。
 *
 * 2) 如果节点保存的是整数值，
 *    那么这部分 header 的头 2 位都将被设置为 1 ，
 *    而之后跟着的 2 位则用于标识节点所保存的整数的类型。
 *
 * |11000000| - 1 byte
 *      Integer encoded as int16_t (2 bytes).
 *      节点的值为 int16_t 类型的整数，长度为 2 字节。
 * |11010000| - 1 byte
 *      Integer encoded as int32_t (4 bytes).
 *      节点的值为 int32_t 类型的整数，长度为 4 字节。
 * |11100000| - 1 byte
 *      Integer encoded as int64_t (8 bytes).
 *      节点的值为 int64_t 类型的整数，长度为 8 字节。
 * |11110000| - 1 byte
 *      Integer encoded as 24 bit signed (3 bytes).
 *      节点的值为 24 位（3 字节）长的整数。
 * |11111110| - 1 byte
 *      Integer encoded as 8 bit signed (1 byte).
 *      节点的值为 8 位（1 字节）长的整数。
 * |1111xxxx| - (with xxxx between 0000 and 1101) immediate 4 bit integer.
 *      Unsigned integer from 0 to 12. The encoded value is actually from
 *      1 to 13 because 0000 and 1111 can not be used, so 1 should be
 *      subtracted from the encoded 4 bit value to obtain the right value.
 *      这种情况是：直接用 |1111xxxx| 记录晚全部数据，不需要任何后续的内存
 *      节点的值为介于 0 至 12 之间的无符号整数。
 *      因为 0000 和 1111 都不能使用，所以位的实际值将是 1 至 13 。
 *      程序在取得这 4 个位的值之后，还需要减去 1 ，才能计算出正确的值。
 *      比如说，如果位的值为 0001 = 1 ，那么程序返回的值将是 1 - 1 = 0 。
 * |11111111| - End of ziplist.
 *      ziplist 的结尾标识
 *
 * All the integers are represented in little endian byte order.
 *
 * 所有整数都表示为小端字节序。
 *
 * ----------------------------------------------------------------------------
 *
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
#include <stdint.h>
#include <limits.h>
#include "zmalloc.h"
#include "util.h"
#include "ziplist.h"
#include "endianconv.h"
#include "redisassert.h"

/*
 * ziplist 末端标识符，以及 5 字节长长度标识符
 */
#define ZIP_END 255     // 0xFF, |11111111| - End of ziplist.
#define ZIP_BIGLEN 254  // 0xFE, 意味着：一个 byte 并不能表达全部 size，必须再用一些 bit 才能表示需要的数值

/* Different encoding/length possibilities */
/*
 * 字符串编码和整数编码的掩码
 */
// |00pppppp|   |01pppppp|qqqqqqqq|   |10______|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|  三种类型
#define ZIP_STR_MASK 0xc0   // string 类型仅仅采用前 2 bits 作为标识，0xc0 = 1100 0000
// |11000000|   |11010000|   |11100000|   |11110000|   |11111110|   |1111xxxx|
#define ZIP_INT_MASK 0x30   // 整数类型则采用第 1 + 2 bits 来标识当前是整数，3 + 4 bit 才是这个整数的类型，0x30 = 0011 0000

/*
 * 字符串编码类型
 */
#define ZIP_STR_06B (0 << 6)    // |00pppppp|
#define ZIP_STR_14B (1 << 6)    // |01pppppp|qqqqqqqq|
#define ZIP_STR_32B (2 << 6)    // |10______|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|

/*
 * 整数编码类型
 * |11xx xxxx| 是每一个 integer 类型 encoding 的必备前缀
 */
#define ZIP_INT_16B (0xc0 | 0<<4)   // 1100 0000 | 0000 0000 ==> 1100 0000, |1100 0000|(1 byte) + 2 byte(16 bits, uint16_t)
#define ZIP_INT_32B (0xc0 | 1<<4)   // 1100 0000 | 0001 0000 ==> 1101 0000, |1101 0000|(1 byte) + 4 byte(32 bits, uint32_t)
#define ZIP_INT_64B (0xc0 | 2<<4)   // 1100 0000 | 0010 0000 ==> 1110 0000, |1110 0000|(1 byte) + 8 byte(64 bits, uint32_t)
#define ZIP_INT_24B (0xc0 | 3<<4)   // 1100 0000 | 0011 0000 ==> 1111 0000, |1111 0000|(1 byte) + 3 byte(24 bits, uint24_t)
#define ZIP_INT_8B 0xfe             //                           1111 1110, |1111 1110|(1 byte) + 1 byte( 8 bits, uint8_t)

/* 4 bit integer immediate encoding 
 *
 * 4 位整数编码的掩码和类型, 直接在这 8 个 bits 里面记录数值，不需要额外的内存
 */
#define ZIP_INT_IMM_MASK 0x0f   /* 00001111 */  // 因为基础格式是 |1111xxxx|
#define ZIP_INT_IMM_MIN 0xf1    /* 11110001 */  // 1111 0000 是留给 ZIP_INT_24B 的
#define ZIP_INT_IMM_MAX 0xfd    /* 11111101 */  // 1111 1111 是留给 ZIP_END 的
#define ZIP_INT_IMM_VAL(v) (v & ZIP_INT_IMM_MASK)   // 实际存储数值的只有 ZIP_INT_IMM_MASK 里面的 bits

/*
 * 24 位整数的最大值和最小值(8388607 ~ -8388608)
 * 
 * 只要你打印出 整型 内存里面的样子（十六进制），那必然是采用 补码 的形式的！
 * 0x10 00 00 是负数的最小值 -8388608;
 * 0x10 00 01 是 -8388607;
 * 0xff ff ff 就是 -1;
 * 0x00 00 00 就是  0;
 * 0x00 00 01 就是  1;
 * 0x7f ff ff 就是整数的最大值，8388607;
 */
#define INT24_MAX 0x7fffff          // 0111 1111 | 1111 1111 | 1111 1111 |
#define INT24_MIN (-INT24_MAX - 1)  // 1000 0000 | 0000 0000 | 0000 0001 |(-0x7ffff 的补码) - 1 = 1000 0000 | 0000 0000 | 0000 0000 |

/* Macro to determine type 
 *
 * 查看给定编码 enc 是否字符串编码
 */
#define ZIP_IS_STR(enc) (((enc) & ZIP_STR_MASK) < ZIP_STR_MASK)

/* Utility macros */
/*
 * ziplist 属性宏
 * 因为 ziplist 并没有采用任何的 struct 来进行构建，直接通过全部内存操作来完成紧凑的内存操作
 * 用 struct 会产生一定程度上的内存对齐，导致内存空洞（尤其是跨平台，内存空洞完全不可控，所以只好直接进行内存操作了）
 */
/*

area        |<---- ziplist header ---->|<----------- entries ------------->|<-end->|

size          4 bytes  4 bytes  2 bytes    ?        ?        ?        ?     1 byte
            +---------+--------+-------+--------+--------+--------+--------+-------+
component   | zlbytes | zltail | zllen | entry1 | entry2 |  ...   | entryN | zlend |
            +---------+--------+-------+--------+--------+--------+--------+-------+
                                       ^                          ^        ^
address                                |                          |        |
                                ZIPLIST_ENTRY_HEAD                |   ZIPLIST_ENTRY_END
                                                                  |
                                                        ZIPLIST_ENTRY_TAIL

*/

/* 返回 ziplist 的属性数值，存进来的数值，必然是采用小端 */
// 所有宏在使用之前，都需要根据这个 section 占用的位数，进行指针的强制转换，以确保指针的步长、解引用的正确

// 定位到 ziplist 的 bytes 属性，该属性记录了整个 ziplist 所占用的内存字节数（就在开头那 4 byte 上）
// 用于取出 bytes 属性的现有值，或者为 bytes 属性赋予新值
#define ZIPLIST_BYTES(zl)       (*((uint32_t*)(zl)))

// 第二个 4 byte
// 定位到 ziplist 的 offset 属性，该属性记录了到达表尾节点的偏移量
// 用于取出 offset 属性的现有值，或者为 offset 属性赋予新值
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)((zl)+sizeof(uint32_t))))

// 返回的是 uint16_t 指针就很心机，通过指针的类型，充分指导 length 的数值格式化方式（只会读取 16 bits 的内存来进行数值构成） 
// 定位到 ziplist 的 length 属性，该属性记录了 ziplist 包含的节点数量
// 用于取出 length 属性的现有值，或者为 length 属性赋予新值
#define ZIPLIST_LENGTH(zl)      (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))

// 返回 ziplist 表头的大小：ziplist header = zlbytes + zltail + zllen
#define ZIPLIST_HEADER_SIZE     (sizeof(uint32_t)*2+sizeof(uint16_t))

/* 直接从 ziplist 根据 ziplist 的 handler 指针跳到相应的位置, 返回 entry 的指针 */
// 返回指向 ziplist 第一个节点（的起始位置）的指针
#define ZIPLIST_ENTRY_HEAD(zl)  ((zl)+ZIPLIST_HEADER_SIZE)

// 返回指向 ziplist 最后一个节点（的起始位置）的指针
#define ZIPLIST_ENTRY_TAIL(zl)  ((zl)+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))

// 返回指向 ziplist 末端 ZIP_END （的起始位置）的指针。-1，倒退一个 byte
#define ZIPLIST_ENTRY_END(zl)   ((zl)+intrev32ifbe(ZIPLIST_BYTES(zl))-1)

/* 
空白 ziplist 示例图

area        |<---- ziplist header ---->|<-- end -->|

size          4 bytes   4 bytes 2 bytes  1 byte
            +---------+--------+-------+-----------+
component   | zlbytes | zltail | zllen | zlend     |
            |         |        |       |           |
value       |  1011   |  1010  |   0   | 1111 1111 |
            +---------+--------+-------+-----------+
                                       ^
                                       |
                               ZIPLIST_ENTRY_HEAD
                                       &
address                        ZIPLIST_ENTRY_TAIL
                                       &
                               ZIPLIST_ENTRY_END

非空 ziplist 示例图

area        |<---- ziplist header ---->|<----------- entries ------------->|<-end->|

size          4 bytes  4 bytes  2 bytes    ?        ?        ?        ?     1 byte
            +---------+--------+-------+--------+--------+--------+--------+-------+
component   | zlbytes | zltail | zllen | entry1 | entry2 |  ...   | entryN | zlend |
            +---------+--------+-------+--------+--------+--------+--------+-------+
                                       ^                          ^        ^
address                                |                          |        |
                                ZIPLIST_ENTRY_HEAD                |   ZIPLIST_ENTRY_END
                                                                  |
                                                        ZIPLIST_ENTRY_TAIL
*/

/* We know a positive increment can only be 1 because entries can only be
 * pushed one at a time. */
/*
 * 增加 ziplist 的节点数
 *
 * T = O(1)
 */
/* Increment the number of items field in the ziplist header. Note that this
 * macro should never overflow the unsigned 16 bit integer, since entries are
 * always pushed one at a time.(but can delete more than one entry at a time) When UINT16_MAX is reached we want the count
 * to stay there to signal that a full scan is needed to get the number of
 * items inside the ziplist. 
 * 
 * TODO: 要是 -1 怎么办？要是超出了 UINT16_MAX 则么办？（incr 不可能是 UINT16_MIN，看上面的注释）
 * 当 zllen == UINT16_MAX，也就意味着已经 max 了，现在要知道这个 zl 究竟有多长，你只能够遍历 zl
 * 详见 ziplistLen() 函数
 * */
#define ZIPLIST_INCR_LENGTH(zl,incr) { \
    if (ZIPLIST_LENGTH(zl) < UINT16_MAX) \
        ZIPLIST_LENGTH(zl) = intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl))+incr); \
}

/*
 * 保存 ziplist 节点信息的结构
 * struct zlentry 并不是 zlentry 实际保存时的样子（编码方式）
 * 这个结构体仅仅是为了方便库作者写代码罢了。。。。。也正是因为这样，所以不用写进 .h 头文件里面
 */
/* We use this function to receive information about a ziplist entry.
 * Note that this is not how the data is actually encoded, is just what we
 * get filled by a function in order to operate more easily. */
typedef struct zlentry {

    // prevrawlen ：前置节点的长度
    // prevrawlensize ：编码 prevrawlen 所需的字节大小（1 或 5）
    unsigned int prevrawlensize, prevrawlen;

    // len ：当前节点 entry-data 的长度
    // lensize ：编码 <encoding> field 所需的字节大小
    unsigned int lensize, len;

    // 当前节点 header 的大小
    // 等于 prevrawlensize + lensize
    unsigned int headersize;

    // 当前节点值所使用的编码类型
    // 只保留 encoding 的 encode 部分，只用来区分 string、integer，以及计数器的长度
    // 不保留计数器的数值部分
    unsigned char encoding;

    // 指向当前节点的指针
    // 跳过 prevlen 部分
    unsigned char *p;

} zlentry;

/* Extract the encoding from the byte pointed by 'ptr' and set it into
 * 'encoding'. 
 *
 * 此时 ptr 已经指向了 ptr 的开头
 * 从 ptr 中取出节点值的编码类型，并将它保存到 encoding 变量中。
 * 需要自己保证 encoding 的类型正确性
 * TODO: ZIP_INT_MASK 直接忽略了？（DONE）
 * (encoding) < ZIP_STR_MASK 的含义是：当前这个 entry 必然是一个 string
 * 否则，这个 entry 必然是个 integer；
 * 
 * 之所以，(encoding) &= ZIP_STR_MASK; 是因为 encoding 里面压根旧不打算保存 计数 部分的内容
 * T = O(1)
 */
#define ZIP_ENTRY_ENCODING(ptr, encoding) do {  \
    (encoding) = (ptr[0]); \
    if ((encoding) < ZIP_STR_MASK) (encoding) &= ZIP_STR_MASK; \
} while(0)

/* Return bytes needed to store integer encoded by 'encoding' 
 *
 * 返回保存 encoding 编码的值所需的字节数量
 * 给定 encoding 表示的 entry-data 大小
 *
 * T = O(1)
 */
static unsigned int zipIntSize(unsigned char encoding) {

    switch(encoding) {
    case ZIP_INT_8B:  return 1;
    case ZIP_INT_16B: return 2;
    case ZIP_INT_24B: return 3;
    case ZIP_INT_32B: return 4;
    case ZIP_INT_64B: return 8;
    default: return 0; /* 4 bit immediate */
    }

    assert(NULL);
    return 0;
}

/* Encode the length 'l' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. 
 *
 * 编码节点长度值 l ，并将它写入到 p 中，然后返回编码 l 所需的字节数量。
 *
 * 如果 p 为 NULL ，那么仅返回编码 l 所需的字节数量，不进行写入。
 *
 * T = O(1)
 */
// 因为 <encoding> field 是包含 本 entry-data 的长度 的，所以需要 rawlen 这个参数
// 返回的是 <encoding> field 需要的内存大小（1\2\5）
static unsigned int zipEncodeLength(unsigned char *p, unsigned char encoding, unsigned int rawlen) {
    unsigned char len = 1, buf[5];

    // 编码字符串
    if (ZIP_IS_STR(encoding)) {
        /* Although encoding is given it may not be set for strings,
         * so we determine it here using the raw length. */
        if (rawlen <= 0x3f) {
            if (!p) return len; // string 的 1 byte encoding 格式
            buf[0] = ZIP_STR_06B | rawlen;
        } else if (rawlen <= 0x3fff) {
            len += 1;
            if (!p) return len;
            buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);  // 处理 rawlen 的高 6 bits
            buf[1] = rawlen & 0xff; // 处理 rawlen 的低 8 bits
        } else {
            len += 4;
            if (!p) return len;
            buf[0] = ZIP_STR_32B;
            buf[1] = (rawlen >> 24) & 0xff;
            buf[2] = (rawlen >> 16) & 0xff;
            buf[3] = (rawlen >> 8) & 0xff;
            buf[4] = rawlen & 0xff;
        }

    // 编码整数
    } else {
        /* Implies integer encoding, so length is always 1. */
        if (!p) return len;
        buf[0] = encoding;
    }

    /* Store this length at p */
    // 将编码后的长度写入 p 
    memcpy(p,buf,len);

    // 返回编码所需的字节数
    return len;
}

/* Decode the length encoded in 'ptr'. The 'encoding' variable will hold the
 * entries encoding, the 'lensize' variable will hold the number of bytes
 * required to encode the entries length, and the 'len' variable will hold the
 * entries length. 
 *
 * ptr 当前指向了 encoding 的开头
 * 解码 ptr 指针，取出列表节点的相关信息，并将它们保存在以下变量中：
 *
 * - encoding 保存节点值的编码类型。（不保留长度的）
 *
 * - lensize 保存编码节点长度所需的字节数。
 *
 * - len 保存节点的长度。
 *
 * T = O(1)
 */
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len) do {                    \
                                                                               \
    /* 取出值的编码类型 */                                                     \
    ZIP_ENTRY_ENCODING((ptr), (encoding));                                     \
                                                                               \
    /* 字符串编码 */                                                           \
    if ((encoding) < ZIP_STR_MASK) {                                           \
        if ((encoding) == ZIP_STR_06B) {                                       \
            (lensize) = 1;                                                     \
            (len) = (ptr)[0] & 0x3f;                                           \
        } else if ((encoding) == ZIP_STR_14B) {                                \
            (lensize) = 2;                                                     \
            (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1];                       \
        } else if (encoding == ZIP_STR_32B) {                                  \
            (lensize) = 5;                                                     \
            (len) = ((ptr)[1] << 24) |                                         \
                    ((ptr)[2] << 16) |                                         \
                    ((ptr)[3] <<  8) |                                         \
                    ((ptr)[4]);                                                \
        } else {                                                               \
            assert(NULL);                                                      \
        }                                                                      \
                                                                               \
    /* 整数编码 */                                                             \
    } else {                                                                   \
        (lensize) = 1;                                                         \
        (len) = zipIntSize(encoding);                                          \
    }                                                                          \
} while(0);

/* Encode the length of the previous entry and write it to "p". Return the
 * number of bytes needed to encode this length if "p" is NULL. 
 *
 * 对前置节点的长度 len 进行编码，并将它写入到 p 中，
 * 然后返回编码 len 所需的字节数量。
 *
 * 如果 p 为 NULL ，那么不进行写入，仅返回编码 len 所需的字节数量。
 *
 * T = O(1)
 */
// 将编写好的 <prevlen> section 放进 p 指针去（这个 field 的内容根据 len 来生成）
static unsigned int zipPrevEncodeLength(unsigned char *p, unsigned int len) {

    // 仅返回编码 len 所需的字节数量
    if (p == NULL) {    // 超出了 1 byte 的表达能力，就要启用 5 byte 的计数方式
        return (len < ZIP_BIGLEN) ? 1 : sizeof(len)+1;
    // sizeof(len) + 1; 1 是 0xFE 的 5 byte 方案标识，sizeof(len) 则是因为：5 byte 方案本质上也就是用 4 byte 来计数，既然是计数那自然只会有正数
    // 所以 sizeof(len) 是更为通用的方案，因为你改了 uint32_t 都好，sizeof 的结果是不变的

    } else {
        // 写入并返回编码 len 所需的字节数量

        // 1 字节
        if (len < ZIP_BIGLEN) {
            p[0] = len;
            return 1;

        // 5 字节
        } else {
            // 添加 5 字节长度标识
            p[0] = ZIP_BIGLEN;
            // 写入编码
            memcpy(p+1,&len,sizeof(len));
            // 如果有必要的话，进行大小端转换
            // 因为 32 bits 的数字是需要进行大小端转换的，所以就不采用指针强制转换的方式了
            memrev32ifbe(p+1);
            // 返回编码长度
            return 1+sizeof(len);
        }
    }
}

/* Encode the length of the previous entry and write it to "p". This only
 * uses the larger encoding (required in __ziplistCascadeUpdate). 
 *
 * 将原本只需要 1 个字节来保存的前置节点长度 len 编码至一个 5 字节长的 header 中。
 * （避免导致后续节点的级联更改）
 * T = O(1)
 */
static void zipPrevEncodeLengthForceLarge(unsigned char *p, unsigned int len) {

    if (p == NULL) return;

    // 设置 5 字节长度标识
    p[0] = ZIP_BIGLEN;

    // 写入 len
    memcpy(p+1,&len,sizeof(len));
    memrev32ifbe(p+1);
}

/* Decode the number of bytes required to store the length of the previous
 * element, from the perspective of the entry pointed to by 'ptr'. 
 *
 * 解码 ptr 指针，
 * 取出编码前置节点长度所需的字节数，并将它保存到 prevlensize 变量中。
 * 根据 <prevlen> field 的第一个 byte 来分辨 <prevlen> 的编码方式
 *
 * T = O(1)，参数 ptr 需要是 指向一个 entry 开头的指针
 */
#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize) do {                          \
    if ((ptr)[0] < ZIP_BIGLEN) {                                               \
        (prevlensize) = 1;                                                     \
    } else {                                                                   \
        (prevlensize) = 5;                                                     \
    }                                                                          \
} while(0);

/* Decode the length of the previous element, from the perspective of the entry
 * pointed to by 'ptr'. 
 *
 * 解码 ptr 指针，
 * 取出编码前置节点长度所需的字节数，
 * 并将这个字节数保存到 prevlensize 中。
 *
 * 然后根据 prevlensize ，从 ptr 中取出前置节点的长度值，
 * 并将这个长度值保存到 prevlen 变量中。
 *
 * T = O(1)
 * assert(sizeof((prevlensize)) == 4); 这一个 statement 是为了确保下面的 memcpy 的正确性
 */
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen) do {                     \
                                                                               \
    /* 先计算被编码长度值的字节数 */                                           \
    ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);                                  \
                                                                               \
    /* 再根据编码字节数来取出长度值 */                                         \
    if ((prevlensize) == 1) {                                                  \
        (prevlen) = (ptr)[0];                                                  \
    } else if ((prevlensize) == 5) {                                           \
        assert(sizeof((prevlensize)) == 4);                                    \
        memcpy(&(prevlen), ((char*)(ptr)) + 1, 4);                             \
        memrev32ifbe(&prevlen);                                                \
    }                                                                          \
} while(0);

/* Return the difference in number of bytes needed to store the length of the
 * previous element 'len', in the entry pointed to by 'p'. 
 *
 * 计算编码 新的前置节点 长度 len 所需的字节数，（也就是我现在想要向 ziplist 插入的那个新节点）
 * 减去编码 p 原来的前置节点长度所需的字节数之差。
 *
 * T = O(1)
 */
// 看看现有的旧节点，当中的 <prevlen> field，能不能表达 len ？
// len 是即将创建的新节点长度（全部 header + entry-data），将会被保存在旧节点的 <prevlen> field
// 0 意味着旧节点不需要变动；> 0 意味着旧节点需要扩容；< 0 旧节点可以考虑缩小
static int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
    unsigned int prevlensize;

    // 取出编码原来的前置节点长度所需的字节数，现在 p 指向的这个 <entry> 中 <prevlen> field 的 size(1 or 5)
    // T = O(1)
    ZIP_DECODE_PREVLENSIZE(p, prevlensize);

    // 计算编码 len 所需的字节数，然后进行减法运算
    // T = O(1)   看看新的 entry 会不会导致 旧节点的 <prevlen> field 需要扩容
    return zipPrevEncodeLength(NULL, len) - prevlensize;
}

/* Return the total number of bytes used by the entry pointed to by 'p'. 
 *
 * 返回指针 p 所指向的节点占用的字节数总和。
 *
 * T = O(1)
 */
// 没有直接在本节点中记录本 entry 的总长度，只能够算出来
static unsigned int zipRawEntryLength(unsigned char *p) {
    unsigned int prevlensize, encoding, lensize, len;

    // 取出编码前置节点的长度所需的字节数
    // T = O(1)
    ZIP_DECODE_PREVLENSIZE(p, prevlensize);

    // 取出当前节点值的编码类型，编码节点值长度所需的字节数，以及节点值的长度
    // T = O(1)
    ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);

    // 计算节点占用的字节数总和
    return prevlensize + lensize + len;
}

/* Check if string pointed to by 'entry' can be encoded as an integer.
 * Stores the integer value in 'v' and its encoding in 'encoding'. 
 *
 * 检查 entry 中指向的字符串能否被编码为整数。
 *
 * 如果可以的话，
 * 将编码后的整数保存在指针 v 的值中，并将编码的方式保存在指针 encoding 的值中。
 *
 * 注意，这里的 entry 和前面代表节点的 entry 不是一个意思。
 *
 * 尝试将 string(里面记录的全是数字，如："12345") 转换为 integer 来保存（更省内存空间）
 * 
 * T = O(N)
 */
// unsigned char *entry，是尝试进行 encode 的 string-data-buf
// 作为出参数的 long long *v, 采用最大容纳能力的 long long 才是正确的
// unsigned char *encoding，也是出参数，告诉调用者，这个 value 的范围所对应的 encoding 方式，然后调用者在进行 encode
static int zipTryEncoding(unsigned char *entry, unsigned int entrylen, long long *v, unsigned char *encoding) {
    long long value;

    // 忽略太长或太短的字符串
    // 超过 32，基本也就超过 int64_t 的范围了
    if (entrylen >= 32 || entrylen == 0) return 0;

    // 尝试转换
    // T = O(N)
    if (string2ll((char*)entry,entrylen,&value)) {

        /* Great, the string can be encoded. Check what's the smallest
         * of our encoding types that can hold this value. */
        // 转换成功，以从小到大的顺序检查适合值 value 的编码方式
        if (value >= 0 && value <= 12) {    // 这种方式只存正数
            *encoding = ZIP_INT_IMM_MIN+value;  // 最小是 1111 0001, 所以直接加上 value
        } else if (value >= INT8_MIN && value <= INT8_MAX) {
            *encoding = ZIP_INT_8B;
        } else if (value >= INT16_MIN && value <= INT16_MAX) {
            *encoding = ZIP_INT_16B;
        } else if (value >= INT24_MIN && value <= INT24_MAX) {
            *encoding = ZIP_INT_24B;
        } else if (value >= INT32_MIN && value <= INT32_MAX) {
            *encoding = ZIP_INT_32B;
        } else {
            *encoding = ZIP_INT_64B;
        }

        // 记录值到指针
        *v = value;

        // 返回转换成功标识
        return 1;
    }

    // 转换失败
    return 0;
}

/* Store integer 'value' at 'p', encoded as 'encoding' 
 * 
 * 以 encoding 指定的编码方式，将整数值 value 写入到 p->entry-data 。
 *
 * T = O(1)
 */
// 此时，p 已经指向了 <entry-data> field 的开头
static void zipSaveInteger(unsigned char *p, int64_t value, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64;

    if (encoding == ZIP_INT_8B) {
        ((int8_t*)p)[0] = (int8_t)value;
    } else if (encoding == ZIP_INT_16B) {
        i16 = value;
        memcpy(p,&i16,sizeof(i16));
        memrev16ifbe(p);
    } else if (encoding == ZIP_INT_24B) {
        i32 = value<<8;
        memrev32ifbe(&i32);
        memcpy(p,((uint8_t*)&i32)+1,sizeof(i32)-sizeof(uint8_t));
    } else if (encoding == ZIP_INT_32B) {
        i32 = value;
        memcpy(p,&i32,sizeof(i32));
        memrev32ifbe(p);
    } else if (encoding == ZIP_INT_64B) {
        i64 = value;
        memcpy(p,&i64,sizeof(i64));
        memrev64ifbe(p);
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        /* Nothing to do, the value is stored in the encoding itself. */
    } else {
        assert(NULL);
    }
}

/* Read integer encoded as 'encoding' from 'p'
 * 
 * 以 encoding 指定的编码方式，读取并返回指针 p 中的整数值。（此时 p 要指向 entry-data 的开头）
 *
 * T = O(1)
 */
static int64_t zipLoadInteger(unsigned char *p, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64, ret = 0;

    if (encoding == ZIP_INT_8B) {
        ret = ((int8_t*)p)[0];
    } else if (encoding == ZIP_INT_16B) {
        memcpy(&i16,p,sizeof(i16));
        memrev16ifbe(&i16);
        ret = i16;
    } else if (encoding == ZIP_INT_32B) {
        memcpy(&i32,p,sizeof(i32));
        memrev32ifbe(&i32);
        ret = i32;
    } else if (encoding == ZIP_INT_24B) {
        i32 = 0;
        memcpy(((uint8_t*)&i32)+1,p,sizeof(i32)-sizeof(uint8_t));
        memrev32ifbe(&i32);
        ret = i32>>8;
    } else if (encoding == ZIP_INT_64B) {
        memcpy(&i64,p,sizeof(i64));
        memrev64ifbe(&i64);
        ret = i64;
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        ret = (encoding & ZIP_INT_IMM_MASK)-1;
    } else {
        assert(NULL);
    }

    return ret;
}

/* Return a struct with all information about an entry. 
 *
 * 将 p 所指向的列表节点的信息全部保存到 zlentry 中，并返回该 zlentry 。
 *
 * T = O(1)
 */
// 为了简化操作而存在的函数
static zlentry zipEntry(unsigned char *p) {
    zlentry e;  // 理论上是可以进行返回值优化的

    // e.prevrawlensize 保存着编码前一个节点的长度所需的字节数
    // e.prevrawlen 保存着前一个节点的长度
    // T = O(1)
    ZIP_DECODE_PREVLEN(p, e.prevrawlensize, e.prevrawlen);

    // p + e.prevrawlensize 将指针移动到列表节点本身
    // e.encoding 保存着节点值的编码类型
    // e.lensize 保存 <encoding> field 所需的字节数
    // e.len 保存着节点值的长度
    // T = O(1)
    ZIP_DECODE_LENGTH(p + e.prevrawlensize, e.encoding, e.lensize, e.len);

    // 计算头结点的字节数 header = <prevlen> + <encoding>
    e.headersize = e.prevrawlensize + e.lensize;

    // 记录指针
    e.p = p;

    return e;
}

/* Create a new empty ziplist. 
 *
 * 创建并返回一个新的 ziplist 
 *
 * T = O(1)
 */
unsigned char *ziplistNew(void) {

    // ZIPLIST_HEADER_SIZE 是 ziplist 表头的大小
    // 1 字节是表末端 ZIP_END 的大小
    unsigned int bytes = ZIPLIST_HEADER_SIZE+1;

    // 为表头和表末端分配空间
    unsigned char *zl = zmalloc(bytes);

    // 初始化表属性
    ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);
    ZIPLIST_LENGTH(zl) = 0;

    // 设置表末端
    zl[bytes-1] = ZIP_END;

    return zl;
}

/* Resize the ziplist. 
 *
 * 调整 ziplist 的大小为 len 字节。
 *
 * 扩容的话，并不会影响 zl 原本的内容
 *
 * T = O(N)
 */
static unsigned char *ziplistResize(unsigned char *zl, unsigned int len) {

    // 用 zrealloc ，扩展时不改变现有元素
    zl = zrealloc(zl,len);

    // 更新 bytes 属性
    ZIPLIST_BYTES(zl) = intrev32ifbe(len);

    // 重新设置表末端
    zl[len-1] = ZIP_END;

    return zl;
}

/* When an entry is inserted, we need to set the prevlen field of the next
 * entry(旧节点) to equal the length of the inserted entry(新节点). It can occur that this
 * length cannot be encoded in 1 byte and the next entry needs to be grow
 * a bit larger to hold the 5-byte encoded prevlen. This can be done for free,
 * because this only happens when an entry is already being inserted (which
 * causes a realloc and memmove). However, encoding the prevlen may require
 * that this entry is grown as well. This effect may cascade throughout
 * the ziplist when there are consecutive entries with a size close to
 * ZIP_BIGLEN, so we need to check that the prevlen can be encoded in every
 * consecutive entry.
 *
 * 当将一个新节点添加到某个节点之前的时候，
 * 如果原节点的 header 空间不足以保存新节点的长度，
 * 那么就需要对原节点的 header 空间进行扩展（从 1 字节扩展到 5 字节）。
 *
 * 但是，当对原节点进行扩展之后，原节点的下一个节点的 prevlen 可能出现空间不足，
 * 这种情况在多个连续节点的长度都接近 ZIP_BIGLEN 时可能发生。
 *
 * 这个函数就用于检查并修复后续节点的空间问题。
 *
 * Note that this effect can also happen in reverse, where the bytes required
 * to encode the prevlen field can shrink. This effect is deliberately ignored,
 * because it can cause a "flapping" effect where a chain prevlen fields is
 * first grown and then shrunk again after consecutive inserts. Rather, the
 * field is allowed to stay larger than necessary, because a large prevlen
 * field implies the ziplist is holding large entries anyway.
 *
 * 反过来说，
 * 因为节点的长度变小而引起的连续缩小也是可能出现的，
 * 不过，为了避免扩展-缩小-扩展-缩小这样的情况反复出现（flapping，抖动），
 * 我们不处理这种情况，而是任由 prevlen 比所需的长度更长。
 * 因为只有当上一个 entry 的长度十分接近 ZIP_BIGLEN 的情况下才会发生 拓展，直接 ignore 也是考虑到这一点
 * 既然这个节点会发生拓展，也就意味着可能会经常发生拓展、压缩，所以还不如直接留空，不压缩
 * TODO: 不再次压缩的话，这个一明明可以采用 1 byte 标识的 <prevlen> 怎么用 5 byte 标识？直接用 uint32_t 的 counter
 *
 * The pointer "p" points to the first entry that does NOT need to be
 * updated, i.e. consecutive fields MAY need an update. 
 *
 * 注意，程序的检查是针对 p 的后续节点，而不是 p 所指向的节点。
 * 因为节点 p 在传入之前已经完成了所需的空间扩展工作。
 *
 * T = O(N^2)
 */
// p = p + reqlen（现在的 p 指向完成迁移、更新的旧节点）
static unsigned char *__ziplistCascadeUpdate(unsigned char *zl, unsigned char *p) {
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), rawlen/* 一个 entry 的完整长度 */, rawlensize;
    size_t offset, noffset, extra;  // noffset: next-entry-offset
    unsigned char *np;  // next-entry-ptr
    zlentry cur, next;

    // T = O(N^2)
    // 每一轮总是在检查：当前节点的当前总长度，需不需要引发下一个节点的扩容
    // 需要的话，扩容，并更新 zl，然后更新下一节点的 <prevlen> ；不需要则更新下一节点的 <prevlen>, 然后退出就好
    // 每一轮，当前节点总是完成的，不需要动的，而下一个节点的 <prevlen> 总是没有被更新到最新值
    while (p[0] != ZIP_END) {   // 直到检查到 ZIP_END 为止（中途是可以提前退出的）

        // 将 p 所指向的节点的信息保存到 cur 结构中
        cur = zipEntry(p);
        // 当前节点的长度
        rawlen = cur.headersize + cur.len;
        // 计算编码当前节点的长度所需的字节数
        // T = O(1)
        rawlensize = zipPrevEncodeLength(NULL,rawlen);

        /* Abort if there is no next entry. */
        // 如果已经没有后续空间需要更新了，跳出
        if (p[rawlen] == ZIP_END) break;

        // 取出后续节点的信息，保存到 next 结构中
        // T = O(1)
        next = zipEntry(p+rawlen);

        /* Abort when "prevlen" has not changed. */
        // 后续节点编码当前节点的空间已经足够，无须再进行任何处理，跳出
        // 可以证明，只要遇到一个空间足够的节点，
        // 那么这个节点之后的所有节点的空间都是足够的
        // NOTE: 怎么看下一节点是否需要进行适配？
        // 我们重新取出了：当前节点、下一节点，并分别拿出来当前节点总长度，下一节点的 <prevlen>
        // 这样一来，下一节点的 <prevlen> 跟 当前节点总长度 不一致的时候（当前节点发生了扩容，级联影响了下一节点），才需要继续
        if (next.prevrawlen == rawlen) break;

        if (next.prevrawlensize < rawlensize) { // 下一节点的 <prevlen> 编码位数，不足以表达 当前节点的总长度

            /* The "prevlen" field of "next" needs more bytes to hold
             * the raw length of "cur". */
            // 执行到这里，表示 next 空间的大小不足以编码 cur 的长度
            // 所以程序需要对 next 节点的（header 部分）空间进行扩展

            // 记录 p 的偏移量（不然更新 zl 之后，你都忘了 while-loop 走到那一个 entry 了）
            offset = p-zl;
            // extra = 这个 zl 还需要额外增加多少 byte，以满足 next-entry 的扩展需求
            extra = rawlensize-next.prevrawlensize;
            // 扩展 zl 的大小
            // T = O(N)
            zl = ziplistResize(zl,curlen+extra);
            // 还原指针 p
            p = zl+offset;

            /* Current pointer and offset for next element. */
            // 记录下一节点的偏移量
            np = p+rawlen;
            noffset = np-zl;

            /* Update tail offset when next element is not the tail element. */
            // 当 next 节点不是表尾节点时，更新列表到表尾节点的偏移量
            // 
            // 不用更新的情况（next 为表尾节点）：
            //
            // |     | next |      ==>    |     | new next          |
            //       ^                          ^
            //       |                          |
            //     tail                        tail
            //
            // 需要更新的情况（next 不是表尾节点）：
            //
            // | next |     |   ==>     | new next          |     |
            //        ^                        ^
            //        |                        |
            //    old tail                 old tail
            // 
            // 更新之后：
            //
            // | new next          |     |
            //                     ^
            //                     |
            //                  new tail
            // T = O(1)
            if ((zl+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))) != np) { // 除非下一节点是 tail，否则都需要用 extra 修正 tail-offset
                ZIPLIST_TAIL_OFFSET(zl) =
                    intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+extra);
            }

            /* Move the tail to the back. */
            // 向后移动 cur 节点之后的数据，为 cur 的新 header 腾出空间
            //
            // 示例：
            //
            // | header | value |  ==>  | header |    | value |  ==>  | header      | value |
            //                                   |<-->|
            //                            为新 header 腾出的空间
            // T = O(N)
            memmove(np+rawlensize,  // np + 当前节点需要的总长度（扩容后） = np 的真实起始地址
                np+next.prevrawlensize, // 反正下一个 entry 的 <prevlen> 都是要重做的，那还不如不拷贝，留出空间就好
                curlen-noffset-next.prevrawlensize-1);
            // 将新的前一节点长度值编码进新的 next 节点的 header
            // T = O(1)
            zipPrevEncodeLength(np,rawlen);

            /* Advance the cursor */
            // 移动指针，继续处理下个节点
            p += rawlen;
            curlen += extra;
        } else {
            if (next.prevrawlensize > rawlensize) {
                /* This would result in shrinking, which we want to avoid.
                 * So, set "rawlen" in the available bytes. */
                // 执行到这里，说明 next 节点编码前置节点的 header 空间有 5 字节
                // 而编码 rawlen 只需要 1 字节
                // 但是程序不会对 next 进行缩小，
                // 所以这里只将 rawlen 写入 5 字节的 header 中就算了。
                // T = O(1)
                zipPrevEncodeLengthForceLarge(p+rawlen,rawlen);
            } else {
                // 运行到这里，
                // 说明 cur 节点的长度正好可以编码到 next 节点的 header 中
                // T = O(1)
                zipPrevEncodeLength(p+rawlen,rawlen);
            }

            /* Stop here, as the raw length of "next" has not changed. */
            break;
        }
    }

    return zl;
}

/* Delete "num" entries, starting at "p". Returns pointer to the ziplist. 
 *
 * 从位置 p 开始，连续删除 num 个节点。
 *
 * 函数的返回值为处理删除操作之后的 ziplist 。
 *
 * T = O(N^2)
 */
// 即使是删除操作，也是通过 ziplistResize() 函数来重新整一个新的 ziplist（底层再使用 zrealloc）
// 1. 现在原地进行删减，构建出删减后的 zl
// 2. 然后 zrealloc，释放不用的内存（所以基本上 zl 的指针一定会变化）
static unsigned char *__ziplistDelete(unsigned char *zl, unsigned char *p, unsigned int num) {
    unsigned int i, totlen, deleted = 0;
    size_t offset;
    int nextdiff = 0;
    zlentry first, tail;

    // T = O(N)
    first = zipEntry(p);
    for (i = 0; p[0] != ZIP_END && i < num; i++) {
        p += zipRawEntryLength(p);  // 移动指针，最后将指向 ZIP_END 或 指定的节点的下一个节点
        deleted++;                  // 以及被删除节点的总个数
    }

    // totlen 是所有被删除节点总共占用的内存字节数
    totlen = p-first.p; // 计算被删除节点总共占用的内存字节数
    if (totlen > 0) {   // 有效性检查
        if (p[0] != ZIP_END) {  // range 会直接删到最末尾

            // 执行这里，表示被删除节点之后仍然有节点存在

            /* Storing `prevrawlen` in this entry may increase or decrease the
             * number of bytes required compare to the current `prevrawlen`.
             * There always is room to store this, because it was previously
             * stored by an entry that is now being deleted. */
            // 因为位于被删除范围之后的第一个节点的 header 部分的大小
            // 可能容纳不了新的前置节点，所以需要计算新旧前置节点之间的字节数差
            // T = O(1)
            // p 此时指向了 range 范围的最末尾那个 entry 的下一个 entry
            // | ... | ... | ... | ... |
            // |<---- range ---->|
            // ^                 ^
            // first             p
            nextdiff = zipPrevLenByteDiff(p,first.prevrawlen);
            // 如果有需要的话，将指针 p 后退 nextdiff 字节，为新 header 空出空间
            // 因为将会根据 p 的位置来进行 zipPrevEncodeLength()
            p -= nextdiff;
            // 将 first 的前置节点的长度编码至 p 中
            // T = O(1)
            zipPrevEncodeLength(p,first.prevrawlen);

            /* Update offset for tail */
            // 更新到达表尾的偏移量，后续还要根据 nextdiff 来进行微调
            // T = O(1)
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))-totlen);

            /* 进一步微调 zl.tail_offset */
            /* When the tail contains more than one entry, we need to take
             * "nextdiff" in account as well. Otherwise, a change in the
             * size of prevlen doesn't have an effect on the *tail* offset. */
            // 如果被删除节点之后，有多于一个节点
            // 那么程序需要将 nextdiff 记录的字节数也计算到表尾偏移量中
            // 这样才能让表尾偏移量正确对齐表尾节点
            // T = O(1)
            tail = zipEntry(p);
            if (p[tail.headersize+tail.len] != ZIP_END) {
                ZIPLIST_TAIL_OFFSET(zl) =
                   intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
            }

            /* Move tail to the front of the ziplist */
            // 从表尾向表头移动数据（包含刷新后的 next-entry），覆盖被删除节点的数据
            // T = O(N)
            memmove(first.p,p,
                intrev32ifbe(ZIPLIST_BYTES(zl))-(p-zl)-1);
        } else {

            // 执行这里，表示被删除节点之后已经没有其他节点了
            // 这时候，更新 offset 就好，因为后面的尾部统统不要得了，也就没什么东西是需要维护的

            /* The entire tail was deleted. No need to move memory. */
            // T = O(1)
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe((first.p-zl)-first.prevrawlen);
        }

        /* Resize and update length */
        // 缩小并更新 ziplist 的长度
        offset = first.p-zl;    // 免得 realloc 之后找不到了
        zl = ziplistResize(zl, intrev32ifbe(ZIPLIST_BYTES(zl))-totlen+nextdiff);
        ZIPLIST_INCR_LENGTH(zl,-deleted);
        p = zl+offset;

        /* When nextdiff != 0, the raw length of the next entry has changed, so
         * we need to cascade the update throughout the ziplist */
        // 如果 p 所指向的节点的大小已经变更，那么进行级联更新
        // 检查 p 之后的所有节点是否符合 ziplist 的编码要求
        // T = O(N^2)
        if (nextdiff != 0)
            zl = __ziplistCascadeUpdate(zl,p);
    }

    return zl;
}

/* Insert item at "p". */
/*
 * 根据指针 p 所指定的位置，将长度为 slen 的字符串 s 插入到 zl 中。
 * （哪怕是数字，上层函数也会通过 string 的方式传递进来的）
 * 函数的返回值为完成插入操作之后的 ziplist（可能发生内存迁移的操作，所以 zl 的 handler 指针也要更新）
 * 
 * 1. 尽可能压缩新的 entry；
 * 2. 扩容，并完善下一节点的 <prevlen>
 * 3. 解决级联的连锁修改
 * 4. 完成新节点的数据写入
 *
 * T = O(N^2)
 */
// unsigned char *p, 要插入新 entry 的地方；
// unsigned char *s，待插入的内容，slen，待插入内容的长度
static unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    // 记录当前 ziplist 的长度
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), reqlen/* 本次插入行为，需要分配多少内存 */, prevlen = 0;/* 务必设置为 0，否则第一次 insert entry 进 zl 的话，将会出错 */
    size_t offset;
    int nextdiff = 0;
    unsigned char encoding = 0;

    // int64_t，因为 integer-entry-date 的极限就是 int64_t
    long long value = 123456789; /* initialized to avoid warning. Using a value
                                    that is easy to see if for some reason
                                    we use it uninitialized. */
    zlentry entry/* p 指向的位置的那一个 entry（如果有的话） */, tail;

    /* Find out prevlen for the entry that is inserted. */
    if (p[0] != ZIP_END) {
        // 如果 p[0] 不指向列表末端，说明列表非空，并且 p 正指向列表的其中一个节点
        // 那么取出 p 所指向节点的信息，并将它保存到 entry 结构中
        // 然后用 prevlen 变量记录前置节点的长度
        // （当插入新节点之后 p 所指向的节点就成了新节点的前置节点）
        // T = O(1)
        entry = zipEntry(p);
        prevlen = entry.prevrawlen;
    } else {
        // 如果 p 指向表尾末端，那么程序需要检查列表是否为空：
        // 1)如果 ptail 也指向 ZIP_END ，那么列表为空；
        // 2)如果列表不为空，那么 ptail 将指向列表的最后一个节点。
        unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl);
        if (ptail[0] != ZIP_END) {  // ziplist 非空
            // 表尾节点为新节点的前置节点

            // 取出表尾节点的长度
            // T = O(1)
            prevlen = zipRawEntryLength(ptail);
        }
    }

    /* See if the entry can be encoded */
    // 尝试看能否将输入字符串转换为整数，如果成功的话：
    // 1)value 将保存转换后的整数值
    // 2)encoding 则保存适用于 value 的编码方式
    // 无论使用什么编码， reqlen 都保存节点值的长度
    // T = O(N)
    if (zipTryEncoding(s,slen,&value,&encoding)) {
        // value 可以进行数字编码压缩
        /* 'encoding' is set to the appropriate integer encoding */
        reqlen = zipIntSize(encoding);  // 将来 <entry-data> 需要多少内存
    } else {
        // 这是一个字符串，无法进行数字编码的压缩
        /* 'encoding' is untouched, however zipEncodeLength will use the
         * string length to figure out how to encode it. */
        reqlen = slen;
    }

    /* We need space for both the length of the previous entry and
     * the length of the payload. */
    // 计算编码前置节点的长度所需的大小
    // T = O(1)
    reqlen += zipPrevEncodeLength(NULL,prevlen);    // 记录 <prevlen> field 需要的内存大小
    // 计算编码当前节点值所需的大小
    // T = O(1)
    reqlen += zipEncodeLength(NULL,encoding,slen);  // 记录 <encoding> field 需要的内存大小

    /* When the insert position is not equal to the tail, we need to
     * make sure that the next entry can hold this entry's length in
     * its prevlen field. */
    // 只要新节点不是被添加到列表末端，
    // 那么程序就需要检查看 p 所指向的节点（的 header）能否编码新节点的长度。
    // nextdiff 保存了新旧编码之间的字节大小差，如果这个值大于 0 
    // 那么说明需要对 p 所指向的节点（的 header ）进行扩展
    // T = O(1)
    // 旧节点的 <prevlen> field 能够表达即将创建的新 entry 的大小？ 在 list 尾部的话，那就无所谓了，不用考虑这个问题
    // 0 意味着旧节点不需要变动；> 0 意味着旧节点需要扩容；< 0 旧节点可以考虑缩小
    // TODO: 难道旧节点的扩大，不会引起连锁反应？为什么后续的节点不用改 prevlen
    // 有的，下面的 __ziplistCascadeUpdate() 会处理的
    nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p,reqlen) : 0;

    /* Store offset because a realloc may change the address of zl. */
    // 因为重分配空间可能会改变 zl 的地址
    // 所以在分配之前，需要记录 zl 到 p 的偏移量，然后在分配之后依靠偏移量还原 p（知道要往哪里插入新节点）
    offset = p-zl;
    // curlen 是 ziplist 原来的长度
    // reqlen 是整个新节点的长度
    // nextdiff 是新节点的后继节点扩展 header 的长度（要么 0 字节，要么 4 个字节，还可能是负数）
    // T = O(N)
    zl = ziplistResize(zl,curlen+reqlen+nextdiff);
    p = zl+offset; // 找回要实行插入操作的位置

    /* Apply memory move when necessary and update tail offset. */
    if (p[0] != ZIP_END) {  // 插入位置不是 ZIP_END
        // 新元素之后还有节点，因为新元素的加入，需要对这些原有节点进行调整

        /* Subtract one because of the ZIP_END bytes */
        // 移动现有元素（旧节点不移动），为新节点 + 重构后的旧节点 腾出位置
        // T = O(N)
        // p+reqlen 为新节点之后的地方
        // nextdiff: 为什么要多这一部分？这一部分的数据明明是没有用的
        // 只是为了预留空间罢了！因为迁移后的旧节点 <prevlen> 这个 field 是要重新些内容的，所以要提前预留 diff 的空间来扩容
        // 只后移需要移动的 entry 部分
        memmove(p+reqlen,p-nextdiff,curlen-offset-1+nextdiff);

        /* Encode this entry's raw length in the next entry. */
        // 将新节点的长度编码至后置节点
        // p+reqlen 定位到后置节点的 <prevlen> field
        // reqlen 是新节点的长度
        // T = O(1)
        zipPrevEncodeLength(p+reqlen,reqlen);   // 更新旧节点的 <prevlen> field

        /* Update offset for tail */
        // 更新到达表尾的偏移量，将新节点的长度也算上
        // TODO: 扩容后的长度 diff 不管了？
        // 再下面一点会处理
        ZIPLIST_TAIL_OFFSET(zl) =
            intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+reqlen);

        /* When the tail contains more than one entry, we need to take
         * "nextdiff" in account as well. Otherwise, a change in the
         * size of prevlen doesn't have an effect on the *tail* offset. */
        // 如果新节点的后面有多于一个节点
        // 那么程序需要将 nextdiff 记录的字节数也计算到表尾偏移量中
        // 这样才能让表尾偏移量正确对齐表尾节点
        // T = O(1)
        // 补充移动 nextdiff 的距离
        tail = zipEntry(p+reqlen);  // 迁移并更新后的旧节点
        if (p[reqlen+tail.headersize+tail.len] != ZIP_END) {    // 看看旧节点后面还有没有节点
            // 如果旧节点是最后一个 entry，也就没有必要更新 zltail 了（可以i不进来这个 if 分支）
            // 如果旧节点并不是最后一个 entry，那么旧节点的 <prevlen> field 扩展，必然导致真正的 ZIPLIST_TAIL_OFFSET(zl) 后移了 nextdiff 个 byte
            // 这时候，我们要把它补回去；
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
        }
        /*
        else {
            // a change in the size of prevlen doesn't have an effect on the *tail* offset.
        }
        */

    } else {
        /* This element will be the new tail. */
        // 新元素是新的表尾节点(p-zl ===> 插入的 entry 首地址 - zl 的首地址 = offset)
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(p-zl);
    }

    /* When nextdiff != 0, the raw length of the next entry has changed, so
     * we need to cascade(级联) the update throughout the ziplist */
    // 当 nextdiff != 0 时，完成迁移、更新后，旧节点的（header 部分）长度已经被改变，
    // 所以需要级联地更新旧节点的后续节点
    if (nextdiff != 0) {
        offset = p-zl;  // 保存当前节点在 zl 上的 offset，下面的操作可能导致 zl 的整体迁移
        // T  = O(N^2)
        zl = __ziplistCascadeUpdate(zl,p+reqlen);
        p = zl+offset;
    }

    /* Write the entry（写完之后，顺手推进 p 指向的位置） */
    // 一切搞定，将前置节点的长度写入新节点的 header
    // 当第一次 insert 时，要确保 头 entry 的 <prevlen> 为 0，这是一个判断 zl back-forward 遍历是否到头的重要依据
    p += zipPrevEncodeLength(p,prevlen);
    // 将节点值的长度写入新节点的 header
    p += zipEncodeLength(p,encoding,slen);
    // 写入节点值
    if (ZIP_IS_STR(encoding)) {
        // T = O(N)
        memcpy(p,s,slen);
    } else {
        // T = O(1)
        zipSaveInteger(p,value,encoding);
    }

    // 更新列表的节点数量计数器
    // T = O(1)
    ZIPLIST_INCR_LENGTH(zl,1);

    return zl;
}

/*
 * 将长度为 slen 的字符串 s 推入到 zl 中。
 *
 * where 参数的值决定了推入的方向：
 * - 值为 ZIPLIST_HEAD 时，将新值推入到表头。
 * - 否则，将新值推入到表末端。（ZIPLIST_TAIL）
 *
 * 函数的返回值为添加新值后的 ziplist 。
 *
 *  sprintf(buf, "100");
 *  zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
 * 
 * T = O(N^2)
 */
// unsigned char *zl，ziplist 的本体，也是 handler
// unsigned char *s， 即使是数字，也是采用 string 的方式传递过来的
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {

    // 根据 where 参数的值，决定将值推入到表头还是表尾
    unsigned char *p;   // 指向的是：要插入的地方，而不是要被替换掉的节点
    p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);

    // 返回添加新值后的 ziplist
    // T = O(N^2)
    return __ziplistInsert(zl,p,s,slen);
}

/* Returns an offset to use for iterating with ziplistNext. When the given
 * index is negative, the list is traversed back to front. When the list
 * doesn't contain an element at the provided index, NULL is returned. */
/*
 * 根据给定索引，遍历列表，并返回索引指定节点的指针。
 *
 * 如果索引为正，那么从表头向表尾遍历。
 * 如果索引为负，那么从表尾向表头遍历。
 * 正数索引从 0 开始，负数索引从 -1 开始。
 *
 * 如果索引超过列表的节点数量，或者列表为空，那么返回 NULL 。
 *
 * T = O(N)
 */
// 通过指定任意的 index 来访问对应的 entry
// 因为每一个 entry 的大小都不固定的关系，所以实际上只能像普通链表那样，一步步地走到指定的 index 那里，才能取出 entry
unsigned char *ziplistIndex(unsigned char *zl, int index) {

    unsigned char *p;

    zlentry entry;

    // 处理负数索引
    if (index < 0) {

        // 将索引转换为正数
        index = (-index)-1; // 实际上算出来的是：从末尾开始的偏移量
        
        // 定位到表尾节点
        p = ZIPLIST_ENTRY_TAIL(zl);

        // 如果列表不为空，那么。。。
        if (p[0] != ZIP_END) {

            // 从表尾向表头遍历
            entry = zipEntry(p);
            // T = O(N)
            while (entry.prevrawlen > 0 && index--) {   // entry.prevrawlen > 0, ziplist 里面的第一个 entry 的 <prevlen> 是 0
                // 前移指针
                p -= entry.prevrawlen;
                // T = O(1)
                entry = zipEntry(p);
            }   // 直到 index = 0; 的时候，才会结束 while-loop
        }

    // 处理正数索引
    } else {

        // 定位到表头节点
        p = ZIPLIST_ENTRY_HEAD(zl);

        // T = O(N)
        while (p[0] != ZIP_END && index--) {
            // 后移指针
            // T = O(1)
            p += zipRawEntryLength(p);
        }
    }

    // 返回结果
    return (p[0] == ZIP_END || index > 0) ? NULL : p;
}

/* Return pointer to next entry in ziplist.
 *
 * zl is the pointer to the ziplist
 * p is the pointer to the current element
 *
 * The element after 'p' is returned, otherwise NULL if we are at the end. */
/*
 * 返回 p 所指向节点的后置节点。
 *
 * 如果 p 为表末端，或者 p 已经是表尾节点，那么返回 NULL 。
 *
 * T = O(1)
 */
// 这个操作很有用的，尤其是采用 ZIPLIST 编码方式的 REDIS_HT，从 field 取出 value，hashTypeGetFromZiplist()
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p) {
    ((void) zl);

    /* "p" could be equal to ZIP_END, caused by ziplistDelete,
     * and we should return NULL. Otherwise, we should return NULL
     * when the *next* element is ZIP_END (there is no next entry). */
    // p 已经指向列表末端
    if (p[0] == ZIP_END) {
        return NULL;
    }

    // 指向后一节点
    p += zipRawEntryLength(p);
    if (p[0] == ZIP_END) {
        // p 已经是表尾节点，没有后置节点
        return NULL;
    }

    return p;
}

/* Return pointer to previous entry in ziplist. */
/*
 * 返回 p 所指向节点的前置节点。
 *
 * 如果 p 所指向为空列表，或者 p 已经指向表头节点，那么返回 NULL 。
 *
 * T = O(1)
 */
// 向前移动迭代器
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p) {
    zlentry entry;

    /* Iterating backwards from ZIP_END should return the tail. When "p" is
     * equal to the first element of the list, we're already at the head,
     * and should return NULL. */
    
    // 如果 p 指向列表末端（列表为空，或者刚开始从表尾向表头迭代）
    // 那么尝试取出列表尾端节点
    if (p[0] == ZIP_END) {
        p = ZIPLIST_ENTRY_TAIL(zl);
        // 尾端节点也指向列表末端，那么列表为空
        return (p[0] == ZIP_END) ? NULL : p;    // p[0] == ZIP_END 意味着：这个 zl 是空的
    
    // 如果 p 指向列表头，那么说明迭代已经完成
    } else if (p == ZIPLIST_ENTRY_HEAD(zl)) {
        return NULL;

    // 既不是表头也不是表尾，从表尾向表头移动指针
    } else {
        // 计算前一个节点的节点数
        entry = zipEntry(p);
        assert(entry.prevrawlen > 0);
        // 移动指针，指向前一个节点
        return p-entry.prevrawlen;
    }
}

/* Get entry pointed to by 'p' and store in either 'e' or 'v' depending
 * on the encoding of the entry. 'e' is always set to NULL to be able
 * to find out whether the string pointer or the integer value was set.
 * Return 0 if 'p' points to the end of the ziplist, 1 otherwise. */
/*
 * 取出 p 所指向节点的值：
 *
 * - 如果节点保存的是字符串，那么将字符串值指针保存到 *sstr 中，字符串长度保存到 *slen
 *
 * - 如果节点保存的是整数，那么将整数保存到 *sval
 *
 * 程序可以通过检查 *sstr 是否为 NULL 来检查值是字符串还是整数。
 *
 * 提取值成功返回 1 ，
 * 如果 p 为空，或者 p 指向的是列表末端，那么返回 0 ，提取值失败。
 *
 * T = O(1)
 */
unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *sval) {

    zlentry entry;
    if (p == NULL || p[0] == ZIP_END) return 0;
    if (sstr) *sstr = NULL;

    // 取出 p 所指向的节点的各项信息，并保存到结构 entry 中
    // T = O(1)
    entry = zipEntry(p);

    // 节点的值为字符串，将字符串长度保存到 *slen ，字符串保存到 *sstr
    // T = O(1)
    if (ZIP_IS_STR(entry.encoding)) {
        if (sstr) {
            *slen = entry.len;
            *sstr = p+entry.headersize; // 一旦在外面通过 sstr 指针修改 string，ziplist 里面的内容也会被修改掉！
        }
    
    // 节点的值为整数，解码值，并将值保存到 *sval
    // T = O(1)
    } else {
        if (sval) {
            *sval = zipLoadInteger(p+entry.headersize,entry.encoding);
        }
    }

    return 1;
}

/* Insert an entry at "p". 
 *
 * 将包含给定值 s 的新节点插入到给定的位置 p 中。
 *
 * 如果 p 指向一个节点，那么新节点将放在原有节点的前面。
 *
 * T = O(N^2)
 */
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    return __ziplistInsert(zl,p,s,slen);
}

/* Delete a single entry from the ziplist, pointed to by *p.
 * Also update *p in place, to be able to iterate over the
 * ziplist, while deleting entries. 
 *
 * 从 zl 中删除 *p 所指向的节点，
 * 并且原地更新 *p 所指向的位置，使得可以在迭代列表的过程中对节点进行删除。
 *
 * T = O(N^2)
 */
// unsigned char **p 之所以要传二维指针进来，是因为这个 p 在外用实际上是当成 iterator 来用的
// 当 ziplist 发生了内存重分配之后，将会引起完全的迭代器失效，这时候，你最好更新一下外面的这个迭代器
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {

    // 因为 __ziplistDelete 时会对 zl 进行内存重分配
    // 而内存重分配可能会改变 zl 的内存地址
    // 所以这里需要记录到达 *p 的偏移量
    // 这样在删除节点之后就可以通过偏移量来将 *p 还原到正确的位置
    size_t offset = *p-zl;
    zl = __ziplistDelete(zl,*p,1);

    /* Store pointer to current element in p, because ziplistDelete will
     * do a realloc which might result in a different "zl"-pointer.
     * When the delete direction is back to front, we might delete the last
     * entry and end up with "p" pointing to ZIP_END, so check this. */
    *p = zl+offset;

    return zl;
}

/* Delete a range of entries from the ziplist. 
 *
 * 从 index 索引指定的节点开始，连续地从 zl 中删除 num 个节点。
 *
 * T = O(N^2)
 */
unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index, unsigned int num) {

    // 根据索引定位到节点
    // T = O(N)
    unsigned char *p = ziplistIndex(zl,index);

    // 连续删除 num 个节点
    // T = O(N^2)
    return (p == NULL) ? zl : __ziplistDelete(zl,p,num);
}

/* Compare entry pointer to by 'p' with 'entry'. Return 1 if equal. 
 *
 * 将 p 所指向的节点的值和 sstr 进行对比。
 *
 * 如果节点值和 sstr 的值相等，返回 1 ，不相等则返回 0 。
 *
 * T = O(N)
 */
// 比较的标准，统统会是 string 的形式，哪怕全是数字
unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen) {
    zlentry entry;
    unsigned char sencoding;
    long long zval, sval;   // 避免溢出
    if (p[0] == ZIP_END) return 0;

    // 取出节点
    entry = zipEntry(p);
    if (ZIP_IS_STR(entry.encoding)) {

        // 节点值为字符串，进行字符串对比

        /* Raw compare */
        if (entry.len == slen) {
            // T = O(N)
            return memcmp(p+entry.headersize,sstr,slen) == 0;
        } else {
            return 0;
        }
    } else {
        
        // 节点值为整数，进行整数对比

        /* Try to compare encoded values. Don't compare encoding because
         * different implementations may encoded integers differently. */
        if (zipTryEncoding(sstr,slen,&sval,&sencoding)) {
          // T = O(1)
          zval = zipLoadInteger(p+entry.headersize,entry.encoding);
          return zval == sval;
        }
    }

    return 0;
}

/* Find pointer to the entry equal to the specified entry. 
 * 
 * 寻找节点值和 vstr 相等的列表节点，并返回该节点的指针。
 * 
 * Skip 'skip' entries between every comparison. 
 *
 * 每次比对之前都跳过 skip 个节点。
 *
 * Returns NULL when the field could not be found. 
 *
 * 如果找不到相应的节点，则返回 NULL 。
 *
 * T = O(N^2)
 */
// unsigned char *p; 从 p 指向的那个 entry 开始整个 find 的流程
// 注意，每次比较之前都会跳过 skip 个 entry。TODO: why? 这岂不是当成跳表来用了？暂时无视，目前版本 skip 都是 1
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip) {
    int skipcnt = 0;
    unsigned char vencoding = 0;    // 只是用来记录 encoding 类型罢了，不需要很长的
    long long vll = 0;

    // 只要未到达列表末端，就一直迭代，直到遍历完毕
    // T = O(N^2)
    while (p[0] != ZIP_END) {
        // 解析当前 entry 的各个 field 参数
        unsigned int prevlensize, encoding, lensize/* encoding field 的大小 */, len;
        unsigned char *q;   // 指向当前 entry 的 entry-data

        ZIP_DECODE_PREVLENSIZE(p, prevlensize);
        ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
        q = p + prevlensize + lensize;

        if (skipcnt == 0) {

            /* Compare current entry with specified entry */
            // 对比字符串值
            // T = O(N)
            if (ZIP_IS_STR(encoding)) {
                if (len == vlen && memcmp(q, vstr, vlen) == 0) {
                    return p;
                }
            } else {
                /* Find out if the searched field can be encoded. Note that
                 * we do it only the first time, once done vencoding is set
                 * to non-zero and vll is set to the integer value. */
                // 因为传入值有可能被编码了，
                // 所以当第一次进行值对比时，程序会对传入值进行解码
                // 这个解码操作只会进行一次
                if (vencoding == 0) {
                    if (!zipTryEncoding(vstr, vlen, &vll, &vencoding)) {
                        /* If the entry can't be encoded we set it to
                         * UCHAR_MAX so that we don't retry again the next
                         * time. */
                        vencoding = UCHAR_MAX;
                    }
                    /* Must be non-zero by now */
                    assert(vencoding);
                }

                /* Compare current entry with specified entry, do it only
                 * if vencoding != UCHAR_MAX because if there is no encoding
                 * possible for the field it can't be a valid integer. */
                // 对比整数值
                if (vencoding != UCHAR_MAX) {   // 这个 if 判断是用来确保 zipLoadInteger() 的安全执行的
                    // T = O(1)
                    long long ll = zipLoadInteger(q, encoding);
                    if (ll == vll) {
                        return p;
                    }
                }
            }

            /* Reset skip count */
            skipcnt = skip;
        } else {
            /* Skip entry */
            skipcnt--;
        }

        /* Move to next entry */
        // 后移指针，指向后置节点
        p = q + len;
    }

    // 没有找到指定的节点
    return NULL;
}

/* Return length of ziplist. 
 * 
 * 返回 ziplist 中的节点个数
 *
 * T = O(N)
 */
unsigned int ziplistLen(unsigned char *zl) {

    unsigned int len = 0;

    // 节点数小于 UINT16_MAX
    // T = O(1)
    if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX) {
        len = intrev16ifbe(ZIPLIST_LENGTH(zl));

    // 节点数大于 UINT16_MAX 时，需要遍历整个列表才能计算出节点数
    // T = O(N)
    } else {
        unsigned char *p = zl+ZIPLIST_HEADER_SIZE;
        while (*p != ZIP_END) { // 从开始开始遍历计数到 ZIP_END
            p += zipRawEntryLength(p);
            len++;
        }

        /* Re-store length if small enough */
        // 出现的可能是：这个 ziplist 曾经超过 UINT16_MAX, 但是发生过删除，进而降了 num
        // 因为缩小的时候，调用了 ZIPLIST_INCR_LENGTH(), 这个宏在 超过 UINT16_MAX 之后，就不会动 length 了
        // 不然的话，总是遍历整个 ziplist，卡都卡死你
        if (len < UINT16_MAX) ZIPLIST_LENGTH(zl) = intrev16ifbe(len);
    }

    return len;
}

/* Return ziplist blob size in bytes. 
 *
 * 返回整个 ziplist 占用的内存字节数
 *
 * T = O(1)
 */
size_t ziplistBlobLen(unsigned char *zl) {
    return intrev32ifbe(ZIPLIST_BYTES(zl));
}

// debug 用，其实这个函数也可以放到下面的 ZIPLIST_TEST_MAIN 里面的
void ziplistRepr(unsigned char *zl) {
    unsigned char *p;
    int index = 0;
    zlentry entry;

    printf(
        "{total bytes %d} "
        "{length %u}\n"
        "{tail offset %u}\n",
        intrev32ifbe(ZIPLIST_BYTES(zl)),
        intrev16ifbe(ZIPLIST_LENGTH(zl)),
        intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)));
    p = ZIPLIST_ENTRY_HEAD(zl);
    while(*p != ZIP_END) {
        entry = zipEntry(p);
        printf(
            "{"
                "addr 0x%08lx, "
                "index %2d, "
                "offset %5ld, "
                "rl: %5u, "
                "hs %2u, "
                "pl: %5u, "
                "pls: %2u, "
                "payload %5u"
            "} ",
            (long unsigned)p,
            index,
            (unsigned long) (p-zl),
            entry.headersize+entry.len,
            entry.headersize,
            entry.prevrawlen,
            entry.prevrawlensize,
            entry.len);
        p += entry.headersize;
        if (ZIP_IS_STR(entry.encoding)) {
            if (entry.len > 40) {
                if (fwrite(p,40,1,stdout) == 0) perror("fwrite");
                printf("...");
            } else {
                if (entry.len &&
                    fwrite(p,entry.len,1,stdout) == 0) perror("fwrite");
            }
        } else {
            printf("%lld", (long long) zipLoadInteger(p,entry.encoding));
        }
        printf("\n");
        p += entry.len;
        index++;
    }
    printf("{end}\n\n");
}

#ifdef ZIPLIST_TEST_MAIN
#include <sys/time.h>
#include <stdlib.h>
#include "adlist.h"
#include "sds.h"

#define debug(f, ...) { if (DEBUG) printf(f, __VA_ARGS__); }

unsigned char *createList() {
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char*)"foo", 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"hello", 5, ZIPLIST_HEAD);
    zl = ziplistPush(zl, (unsigned char*)"1024", 4, ZIPLIST_TAIL);
    return zl;
}

unsigned char *createIntList() {
    unsigned char *zl = ziplistNew();
    char buf[32];

    sprintf(buf, "100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "128000");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "-100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "4294967296");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "much much longer non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    return zl;
}

long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

void stress(int pos, int num, int maxsize, int dnum) {
    int i,j,k;
    unsigned char *zl;
    char posstr[2][5] = { "HEAD", "TAIL" };
    long long start;
    for (i = 0; i < maxsize; i+=dnum) {
        zl = ziplistNew();
        for (j = 0; j < i; j++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,ZIPLIST_TAIL);
        }

        /* Do num times a push+pop from pos */
        start = usec();
        for (k = 0; k < num; k++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,pos);
            zl = ziplistDeleteRange(zl,0,1);
        }
        printf("List size: %8d, bytes: %8d, %dx push+pop (%s): %6lld usec\n",
            i,intrev32ifbe(ZIPLIST_BYTES(zl)),num,posstr[pos],usec()-start);
        zfree(zl);
    }
}

void pop(unsigned char *zl, int where) {
    unsigned char *p, *vstr;
    unsigned int vlen;
    long long vlong;

    p = ziplistIndex(zl,where == ZIPLIST_HEAD ? 0 : -1);
    if (ziplistGet(p,&vstr,&vlen,&vlong)) {
        if (where == ZIPLIST_HEAD)
            printf("Pop head: ");
        else
            printf("Pop tail: ");

        if (vstr)
            if (vlen && fwrite(vstr,vlen,1,stdout) == 0) perror("fwrite");
        else
            printf("%lld", vlong);

        printf("\n");
        ziplistDeleteRange(zl,-1,1);
    } else {
        printf("ERROR: Could not pop\n");
        exit(1);
    }
}

int randstring(char *target, unsigned int min, unsigned int max) {
    int p = 0;
    int len = min+rand()%(max-min+1);
    int minval, maxval;
    switch(rand() % 3) {
    case 0:
        minval = 0;
        maxval = 255;
    break;
    case 1:
        minval = 48;
        maxval = 122;
    break;
    case 2:
        minval = 48;
        maxval = 52;
    break;
    default:
        assert(NULL);
    }

    while(p < len)
        target[p++] = minval+rand()%(maxval-minval+1);
    return len;
}

void verify(unsigned char *zl, zlentry *e) {
    int i;
    int len = ziplistLen(zl);
    zlentry _e;

    for (i = 0; i < len; i++) {
        memset(&e[i], 0, sizeof(zlentry));
        e[i] = zipEntry(ziplistIndex(zl, i));

        memset(&_e, 0, sizeof(zlentry));
        _e = zipEntry(ziplistIndex(zl, -len+i));

        assert(memcmp(&e[i], &_e, sizeof(zlentry)) == 0);
    }
}

int main(int argc, char **argv) {
    unsigned char *zl, *p;
    unsigned char *entry;
    unsigned int elen;
    long long value;

    /* If an argument is given, use it as the random seed. */
    if (argc == 2)
        srand(atoi(argv[1]));

    zl = createIntList();
    ziplistRepr(zl);

    zl = createList();
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_HEAD);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    printf("Get element at index 3:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 3);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index 3\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index 4 (out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
    }

    printf("Get element at index -1 (last element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -1\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index -4 (first element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -4\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index -5 (reverse out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -5);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
    }

    printf("Iterate list from 0 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 1 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 2 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 2);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate starting out of range:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("No entry\n");
        } else {
            printf("ERROR\n");
        }
        printf("\n");
    }

    printf("Iterate from back to front:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate from back to front, deleting all items:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            zl = ziplistDelete(zl,&p);
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Delete inclusive range 0,0:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 1);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 0,1:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 2);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 1,2:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 2);
        ziplistRepr(zl);
    }

    printf("Delete with start index out of range:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 5, 1);
        ziplistRepr(zl);
    }

    printf("Delete with num overflow:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 5);
        ziplistRepr(zl);
    }

    printf("Delete foo while iterating:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        while (ziplistGet(p,&entry,&elen,&value)) {
            if (entry && strncmp("foo",(char*)entry,elen) == 0) {
                printf("Delete foo\n");
                zl = ziplistDelete(zl,&p);
            } else {
                printf("Entry: ");
                if (entry) {
                    if (elen && fwrite(entry,elen,1,stdout) == 0)
                        perror("fwrite");
                } else {
                    printf("%lld",value);
                }
                p = ziplistNext(zl,p);
                printf("\n");
            }
        }
        printf("\n");
        ziplistRepr(zl);
    }

    printf("Regression test for >255 byte strings:\n");
    {
        char v1[257],v2[257];
        memset(v1,'x',256);
        memset(v2,'y',256);
        zl = ziplistNew();
        zl = ziplistPush(zl,(unsigned char*)v1,strlen(v1),ZIPLIST_TAIL);
        zl = ziplistPush(zl,(unsigned char*)v2,strlen(v2),ZIPLIST_TAIL);

        /* Pop values again and compare their value. */
        p = ziplistIndex(zl,0);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v1,(char*)entry,elen) == 0);
        p = ziplistIndex(zl,1);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v2,(char*)entry,elen) == 0);
        printf("SUCCESS\n\n");
    }

    printf("Regression test deleting next to last entries:\n");
    {
        char v[3][257];
        zlentry e[3];
        int i;

        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++) {
            memset(v[i], 'a' + i, sizeof(v[0]));
        }

        v[0][256] = '\0';
        v[1][  1] = '\0';
        v[2][256] = '\0';

        zl = ziplistNew();
        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++) {
            zl = ziplistPush(zl, (unsigned char *) v[i], strlen(v[i]), ZIPLIST_TAIL);
        }

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);
        assert(e[2].prevrawlensize == 1);

        /* Deleting entry 1 will increase `prevrawlensize` for entry 2 */
        unsigned char *p = e[1].p;
        zl = ziplistDelete(zl, &p);

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);

        printf("SUCCESS\n\n");
    }

    printf("Create long list and check indices:\n");
    {
        zl = ziplistNew();
        char buf[32];
        int i,len;
        for (i = 0; i < 1000; i++) {
            len = sprintf(buf,"%d",i);
            zl = ziplistPush(zl,(unsigned char*)buf,len,ZIPLIST_TAIL);
        }
        for (i = 0; i < 1000; i++) {
            p = ziplistIndex(zl,i);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(i == value);

            p = ziplistIndex(zl,-i-1);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(999-i == value);
        }
        printf("SUCCESS\n\n");
    }

    printf("Compare strings with ziplist entries:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        if (!ziplistCompare(p,(unsigned char*)"hello",5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"hella",5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl,3);
        if (!ziplistCompare(p,(unsigned char*)"1024",4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"1025",4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }
        printf("SUCCESS\n\n");
    }

    printf("Stress with random payloads of different encoding:\n");
    {
        int i,j,len,where;
        unsigned char *p;
        char buf[1024];
        int buflen;
        list *ref;
        listNode *refnode;

        /* Hold temp vars from ziplist */
        unsigned char *sstr;
        unsigned int slen;
        long long sval;

        for (i = 0; i < 20000; i++) {
            zl = ziplistNew();
            ref = listCreate();
            listSetFreeMethod(ref,sdsfree);
            len = rand() % 256;

            /* Create lists */
            for (j = 0; j < len; j++) {
                where = (rand() & 1) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
                if (rand() % 2) {
                    buflen = randstring(buf,1,sizeof(buf)-1);
                } else {
                    switch(rand() % 3) {
                    case 0:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) >> 20);
                        break;
                    case 1:
                        buflen = sprintf(buf,"%lld",(0LL + rand()));
                        break;
                    case 2:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) << 20);
                        break;
                    default:
                        assert(NULL);
                    }
                }

                /* Add to ziplist */
                zl = ziplistPush(zl, (unsigned char*)buf, buflen, where);

                /* Add to reference list */
                if (where == ZIPLIST_HEAD) {
                    listAddNodeHead(ref,sdsnewlen(buf, buflen));
                } else if (where == ZIPLIST_TAIL) {
                    listAddNodeTail(ref,sdsnewlen(buf, buflen));
                } else {
                    assert(NULL);
                }
            }

            assert(listLength(ref) == ziplistLen(zl));
            for (j = 0; j < len; j++) {
                /* Naive way to get elements, but similar to the stresser
                 * executed from the Tcl test suite. */
                p = ziplistIndex(zl,j);
                refnode = listIndex(ref,j);

                assert(ziplistGet(p,&sstr,&slen,&sval));
                if (sstr == NULL) {
                    buflen = sprintf(buf,"%lld",sval);
                } else {
                    buflen = slen;
                    memcpy(buf,sstr,buflen);
                    buf[buflen] = '\0';
                }
                assert(memcmp(buf,listNodeValue(refnode),buflen) == 0);
            }
            zfree(zl);
            listRelease(ref);
        }
        printf("SUCCESS\n\n");
    }

    printf("Stress with variable ziplist size:\n");
    {
        stress(ZIPLIST_HEAD,100000,16384,256);
        stress(ZIPLIST_TAIL,100000,16384,256);
    }

    return 0;
}

#endif
