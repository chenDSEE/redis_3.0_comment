/* endinconv.c -- Endian conversions utilities.
 *
 * This functions are never called directly, but always using the macros
 * defined into endianconv.h, this way we define everything is a non-operation
 * if the arch is already little endian.
 *
 * Redis tries to encode everything as little endian (but a few things that need
 * to be backward compatible are still in big endian) because most of the
 * production environments are little endian, and we have a lot of conversions
 * in a few places because ziplists, intsets, zipmaps, need to be endian-neutral
 * even in memory, since they are serialied on RDB files directly with a single
 * write(2) without other additional steps.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2011-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

// NOTE: 仅仅作为字节序翻转，并不是说只能够 Toggle the 16 bit unsigned integer
// pointed by *p from little endian to big endian 的！！！
// 下面的英文注释不太正确


/* Toggle the 16 bit unsigned integer pointed by *p from little endian to
 * big endian */
/**
 * binary data(in memory): 1111 1111 1111 1111  (0xFF FF)
 *                         [  char ] [  char ]
 *                            x[0]      x[1]               
*/
void memrev16(void *p) {
    unsigned char *x = p, t;

    t = x[0];
    x[0] = x[1];
    x[1] = t;
}

/**
 * binary data(in memory): 1111 1111 1111 1111 1111 1111 1111 1111 (0xFF FF FF FF)
 *                         [  char ] [  char ] [  char ] [  char ]
 *                            x[0]      x[1]      x[2]      x[3]              
*/
/* Toggle the 32 bit unsigned integer pointed by *p from little endian to
 * big endian */
void memrev32(void *p) {
    unsigned char *x = p, t;

    t = x[0];
    x[0] = x[3];
    x[3] = t;
    t = x[1];
    x[1] = x[2];
    x[2] = t;
}

/* Toggle the 64 bit unsigned integer pointed by *p from little endian to
 * big endian */
void memrev64(void *p) {
    unsigned char *x = p, t;

    t = x[0];
    x[0] = x[7];
    x[7] = t;
    t = x[1];
    x[1] = x[6];
    x[6] = t;
    t = x[2];
    x[2] = x[5];
    x[5] = t;
    t = x[3];
    x[3] = x[4];
    x[4] = t;
}

uint16_t intrev16(uint16_t v) {
    memrev16(&v);
    return v;
}

uint32_t intrev32(uint32_t v) {
    memrev32(&v);
    return v;
}

uint64_t intrev64(uint64_t v) {
    memrev64(&v);
    return v;
}

// gcc -g endianconv.c -D TESTMAIN
#ifdef TESTMAIN
#include <stdio.h>

int main(void) {
    char buf[32];

    sprintf(buf,"ciaoroma");
    memrev16(buf);
    printf("%s\n", buf);    // ic aoroma

    sprintf(buf,"ciaoroma");
    memrev32(buf);
    printf("%s\n", buf);    // oaic roma

    sprintf(buf,"ciaoroma");
    memrev64(buf);
    printf("%s\n", buf);    // amoroaic

    return 0;
}
#endif
