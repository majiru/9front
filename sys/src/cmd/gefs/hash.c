// metrohash64.cpp
//
// The MIT License (MIT)
//
// Copyright (c) 2015 J. Andrew Rogers
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"

#define rotate_right(v, k)\
	(((v) >> (k)) | ((v) << (64 - (k))))
#define read_u64(ptr) \
	(*(u64int*)ptr)
#define read_u32(ptr) \
	(*(u32int*)ptr)
#define read_u16(ptr) \
	(*(u16int*)ptr)
#define read_u8(ptr) \
	(*(u8int*)ptr)

uvlong
metrohash64_1(void * key, u64int len, u32int seed)
{
	static const u64int k0 = 0xC83A91E1;
	static const u64int k1 = 0x8648DBDB;
	static const u64int k2 = 0x7BDEC03B;
	static const u64int k3 = 0x2F5870A5;

	const uchar * ptr = key;
	const uchar * const end = ptr + len;
	
	u64int hash = ((((u64int) seed) + k2) * k0) + len;
	
	if(len >= 32){
		u64int v[4];
		v[0] = hash;
		v[1] = hash;
		v[2] = hash;
		v[3] = hash;
		
		do{
			v[0] += read_u64(ptr) * k0; ptr += 8; v[0] = rotate_right(v[0],29) + v[2];
			v[1] += read_u64(ptr) * k1; ptr += 8; v[1] = rotate_right(v[1],29) + v[3];
			v[2] += read_u64(ptr) * k2; ptr += 8; v[2] = rotate_right(v[2],29) + v[0];
			v[3] += read_u64(ptr) * k3; ptr += 8; v[3] = rotate_right(v[3],29) + v[1];
		}
		while(ptr <= (end - 32));

		v[2] ^= rotate_right(((v[0] + v[3]) * k0) + v[1], 33) * k1;
		v[3] ^= rotate_right(((v[1] + v[2]) * k1) + v[0], 33) * k0;
		v[0] ^= rotate_right(((v[0] + v[2]) * k0) + v[3], 33) * k1;
		v[1] ^= rotate_right(((v[1] + v[3]) * k1) + v[2], 33) * k0;
		hash += v[0] ^ v[1];
	}
	
	if((end - ptr) >= 16){
		u64int v0 = hash + (read_u64(ptr) * k0); ptr += 8; v0 = rotate_right(v0,33) * k1;
		u64int v1 = hash + (read_u64(ptr) * k1); ptr += 8; v1 = rotate_right(v1,33) * k2;
		v0 ^= rotate_right(v0 * k0, 35) + v1;
		v1 ^= rotate_right(v1 * k3, 35) + v0;
		hash += v1;
	}
	
	if((end - ptr) >= 8){
		hash += read_u64(ptr) * k3; ptr += 8;
		hash ^= rotate_right(hash, 33) * k1;
		
	}
	
	if((end - ptr) >= 4){
		hash += read_u32(ptr) * k3; ptr += 4;
		hash ^= rotate_right(hash, 15) * k1;
	}
	
	if((end - ptr) >= 2){
		hash += read_u16(ptr) * k3; ptr += 2;
		hash ^= rotate_right(hash, 13) * k1;
	}
	
	if((end - ptr) >= 1){
		hash += read_u8 (ptr) * k3;
		hash ^= rotate_right(hash, 25) * k1;
	}
	
	hash ^= rotate_right(hash, 33);
	hash *= k0;
	hash ^= rotate_right(hash, 33);

	return hash;
}

uvlong
bufhash(void *src, usize len)
{
	return metrohash64_1(src, len, 0x6765);
}

uvlong
blkhash(Blk *b)
{
	return metrohash64_1(b->buf, Blksz, 0x6765);
}

u32int
ihash(uvlong x)
{
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	x = x ^ (x >> 31);
	return x;
}
