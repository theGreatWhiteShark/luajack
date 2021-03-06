/* The MIT License (MIT)
 *
 * Copyright (c) 2015 Stefano Trettel
 *
 * Software repository: LuaJack, https://github.com/stetre/luajack
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/****************************************************************************
 * Lock-free ringbuffers wrapper											*
 ****************************************************************************/

#include "internal.h"

/* This module implements a simple protocol to carry data over jack ringbuffers.
 * Data is transferred over the ringbuffers in form of messages, each consisting
 * of a tag-len header optionally followed by data.
 * - 'tag' is a a user defined type value;
 * - 'len' is the length of the string that follow (may be 0)
 * - 'data' (optional) may be any Lua string (i.e. it may be an ASCII string as well
 *         as a binary string containing 0s).
 */

typedef struct {
	/* use fixed size types, so to ease the dimensioning of the ringbuffer ... */
	int32_t	tag;
	uint32_t len;	/* length of data that follow */
} hdr_t;

size_t ringbuffer_header_len(void)
/* if someone needs it to dimension the buffer... */
	{
	return  sizeof(hdr_t);
	}

jack_ringbuffer_t* ringbuffer_new(size_t sz, int mlock)
/* Create a ringbuffer. mlock!=0: lock the buffer in memory. */
	{
	jack_ringbuffer_t *rbuf;
	if((rbuf = jack_ringbuffer_create(sz)) == NULL)
		return NULL;
	if(mlock)
		jack_ringbuffer_mlock(rbuf);
	return rbuf;
	}

void ringbuffer_free(jack_ringbuffer_t *rbuf)
	{
	jack_ringbuffer_free(rbuf);
	}

int ringbuffer_creset(jack_ringbuffer_t *rbuf)
	{
	jack_ringbuffer_reset(rbuf);
	return 0;
	}

int ringbuffer_luawrite(jack_ringbuffer_t *rbuf, lua_State *L, int arg)
/* bool, errmsg = write(..., tag, data)
 * expects 
 * tag (integer) at index 'arg' of the stack, and 
 * data (string) at index 'arg+1' (optional)
 *
 * if there is not enough space available, it returns 'false, "no space"';
 * data may be an empty string ("") or nil, in which case it defaults
 * to the empty string (i.e., the message has only the header).
 */
	{
	jack_ringbuffer_data_t vec[2];
	hdr_t hdr;
	int isnum;
	size_t space, cnt;
	size_t len;
	const char *data;
	hdr.tag = (uint32_t)lua_tointegerx(L, arg, &isnum);
	if(!isnum)
		luaL_error(L, "invalid tag");

	data = luaL_optlstring(L, arg + 1, NULL, &len);
	if(!data)
		hdr.len = 0;
	else
		hdr.len = len; /*@@*/

	space = jack_ringbuffer_write_space(rbuf);
	if((sizeof(hdr) + hdr.len) > space)
		{ lua_pushboolean(L, 0); return 1; }

	/* write header first (this automatically advances) */
	cnt = jack_ringbuffer_write(rbuf, (const char *)&hdr, sizeof(hdr));
	if(cnt != sizeof(hdr))
		return luaL_error(L, UNEXPECTED_ERROR);

	if(hdr.len)
		{
		/* write data */
		jack_ringbuffer_get_write_vector(rbuf, vec);
		if((vec[0].len+vec[1].len) < hdr.len)
			return luaL_error(L, UNEXPECTED_ERROR);
		if(vec[0].len >= hdr.len)
			memcpy(vec[0].buf, data, hdr.len);
		else
			{
			memcpy(vec[0].buf, data, vec[0].len);
			memcpy(vec[1].buf, data + vec[0].len, hdr.len - vec[0].len);
			}
		jack_ringbuffer_write_advance(rbuf, hdr.len);
		}
	lua_pushboolean(L, 1);
	return 1;
	}

int ringbuffer_cwrite(jack_ringbuffer_t *rbuf, uint32_t tag, const void *data, size_t len)
/* C version: returns 1 on success and 0 on error */
	{
	jack_ringbuffer_data_t vec[2];
	hdr_t hdr;
	size_t space, cnt;
	hdr.tag = tag;

	hdr.len = data!=NULL ? len : 0;

	space = jack_ringbuffer_write_space(rbuf);
	if((sizeof(hdr) + hdr.len) > space)
		return 0;

	/* write header first (this automatically advances) */
	cnt = jack_ringbuffer_write(rbuf, (const char *)&hdr, sizeof(hdr));
	if(cnt != sizeof(hdr))
		return luajack_error(UNEXPECTED_ERROR);

	if(hdr.len)
		{
		/* write data */
		jack_ringbuffer_get_write_vector(rbuf, vec);
		if((vec[0].len+vec[1].len) < hdr.len)
			return luajack_error(UNEXPECTED_ERROR);
		if(vec[0].len >= hdr.len)
			memcpy(vec[0].buf, data, hdr.len);
		else
			{
			memcpy(vec[0].buf, data, vec[0].len);
			memcpy(vec[1].buf, (char*)data + vec[0].len, hdr.len - vec[0].len);
			}
		jack_ringbuffer_write_advance(rbuf, hdr.len);
		}
	return 1;
	}


int ringbuffer_luaread(jack_ringbuffer_t *rbuf, lua_State *L, int advance)
/* tag, data = read()
 * returns tag=nil if there is not a complete message (header+data) in
 * the ringbuffer
 * if the header.len is 0, data is returned as an empty string ("")
 */
	{
	jack_ringbuffer_data_t vec[2];
	hdr_t hdr;
	uint32_t len;
	size_t cnt;

	/* peek for header */
	cnt = jack_ringbuffer_peek(rbuf, (char*)&hdr, sizeof(hdr));
	if(cnt != sizeof(hdr))
		{ lua_pushnil(L); return 1; }
	
	/* see if there are 'len' bytes of data available */
	cnt = jack_ringbuffer_read_space(rbuf);
	if( cnt < (sizeof(hdr) + hdr.len) )
		{ lua_pushnil(L); return 1; }
	
	lua_pushinteger(L, hdr.tag);

	if(hdr.len == 0) /* header only */
		{ 
		if(advance)
			jack_ringbuffer_read_advance(rbuf, sizeof(hdr));
		lua_pushstring(L, ""); 
		return 2; 
		}
		
	/* get the read vector */
	jack_ringbuffer_get_read_vector(rbuf, vec);

	//printf("vec[0].len=%u, vec[1].len=%u hdr.len=%u\n",vec[0].len,vec[1].len,hdr.len);
	if(vec[0].len >= (sizeof(hdr) + hdr.len)) /* data fits in vec[0] */
		lua_pushlstring(L, vec[0].buf + sizeof(hdr), hdr.len);
	else if(vec[0].len > sizeof(hdr))
		{
		len = vec[0].len - sizeof(hdr);
		lua_pushlstring(L, vec[0].buf + sizeof(hdr), len);
		lua_pushlstring(L, vec[1].buf, hdr.len - len);
		lua_concat(L, 2);
		}
	else /* vec[0] contains only the header or part of it (data is all in vec[1]) */
		{
		len = sizeof(hdr) - vec[0].len; /* the first len bytes in vec1 are part of the header */
		lua_pushlstring(L, vec[1].buf + len, hdr.len);
		}
	if(advance)
		jack_ringbuffer_read_advance(rbuf, sizeof(hdr) + hdr.len);
	return 2;
	}


int ringbuffer_cread(jack_ringbuffer_t *rbuf, void *buf, size_t bufsz, int advance, uint32_t *tag, size_t *len)
/* C version: returns 1 on success and 0 on error */
	{
	jack_ringbuffer_data_t vec[2];
	hdr_t hdr;
	uint32_t len0;
	size_t cnt;

	/* peek for header */
	cnt = jack_ringbuffer_peek(rbuf, (char*)&hdr, sizeof(hdr));
	if(cnt != sizeof(hdr)) return 0;
	
	/* see if there are 'len' bytes of data available */
	cnt = jack_ringbuffer_read_space(rbuf);
	if( cnt < (sizeof(hdr) + hdr.len) ) return 0;

	/* check if data fits in the user provided buffer */
	if(hdr.len > bufsz )
		return luajack_error("not enough space for ringbuffer_read() "
						"(at least %u bytes needed)", hdr.len);
	

	*tag = hdr.tag;
	*len = hdr.len;

	if(hdr.len == 0) /* tag only */
		{ 
		if(advance)
			jack_ringbuffer_read_advance(rbuf, sizeof(hdr));
		if(bufsz>0) ((char*)buf)[0]='\0';
		return 1;
		}
				
	/* get the read vector */
	jack_ringbuffer_get_read_vector(rbuf, vec);

	/* copy the data part in the user provided buffer */
	
	if(vec[0].len >= (sizeof(hdr) + hdr.len)) /* all the data are in vec[0] */
		memcpy(buf, vec[0].buf + sizeof(hdr), hdr.len);
	else if(vec[0].len > sizeof(hdr)) /* part of the data are in vec[0] */
		{
		len0 = vec[0].len - sizeof(hdr);
		memcpy(buf, vec[0].buf + sizeof(hdr), len0);
		memcpy((char*)buf + len0, vec[1].buf, hdr.len - len0);
		}
	else /* part of the header and all of the data are in vec[1] */
		{
		len0 = sizeof(hdr) - vec[0].len; /* bytes oh header in vec[1] */
		memcpy((char*)buf, vec[1].buf + len0, hdr.len);
		}
	if(advance)	
		jack_ringbuffer_read_advance(rbuf, sizeof(hdr) + hdr.len);
	return 1;
	}


int ringbuffer_luaread_advance(jack_ringbuffer_t *rbuf, lua_State *L)
/* call this after a peek to just advance the read pointer */
	{
	hdr_t hdr;
	/* peek for header */
	size_t cnt = jack_ringbuffer_peek(rbuf, (char*)&hdr, sizeof(hdr));

	if(cnt == sizeof(hdr))
		{
		cnt = jack_ringbuffer_read_space(rbuf);

		if( cnt >= (sizeof(hdr) + hdr.len))
			{
			jack_ringbuffer_read_advance(rbuf, sizeof(hdr) + hdr.len);
			lua_pushboolean(L, 1);
			return 1;
			}
		}
			
	lua_pushboolean(L, 0);
	return 1;
	}


int ringbuffer_cread_advance(jack_ringbuffer_t *rbuf)
/* C version: returns 1 or 0 */
	{
	hdr_t hdr;
	/* peek for header */
	size_t cnt = jack_ringbuffer_peek(rbuf, (char*)&hdr, sizeof(hdr));
	if(cnt == sizeof(hdr))
		{
		/* see if there are 'len' bytes of data available */
		cnt = jack_ringbuffer_read_space(rbuf);
		if( cnt >= (sizeof(hdr) + hdr.len) )
			{
			jack_ringbuffer_read_advance(rbuf, sizeof(hdr) + hdr.len);
			return 1;
			}
		}
	return 0;
	}


size_t ringbuffer_write_space(jack_ringbuffer_t *rbuf)
	{
	return jack_ringbuffer_write_space(rbuf);
	}


size_t ringbuffer_read_space(jack_ringbuffer_t *rbuf)
	{
	return jack_ringbuffer_read_space(rbuf);
	}

