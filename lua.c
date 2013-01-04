/*
 * uhttpd - Tiny single-threaded httpd
 *
 *   Copyright (C) 2010-2012 Jo-Philipp Wich <xm@subsignal.org>
 *   Copyright (C) 2012 Felix Fietkau <nbd@openwrt.org>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */


#include <libubox/blobmsg.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <poll.h>

#include "uhttpd.h"
#include "plugin.h"

#define UH_LUA_CB	"handle_request"

static const struct uhttpd_ops *ops;
static struct config *_conf;
#define conf (*_conf)

static lua_State *_L;

static int uh_lua_recv(lua_State *L)
{
	static struct pollfd pfd = {
		.fd = STDIN_FILENO,
		.events = POLLIN,
	};
	luaL_Buffer B;
	int data_len = 0;
	int len;
	int r;

	len = luaL_checknumber(L, 1);
	luaL_buffinit(L, &B);
	while(len > 0) {
		char *buf;

		buf = luaL_prepbuffer(&B);
		r = read(STDIN_FILENO, buf, LUAL_BUFFERSIZE);
		if (r < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				pfd.revents = 0;
				poll(&pfd, 1, 1000);
				if (pfd.revents & POLLIN)
					continue;
			}
			if (errno == EINTR)
				continue;

			if (!data_len)
				data_len = -1;
			break;
		}
		if (!r)
			break;

		luaL_addsize(&B, r);
		data_len += r;
		if (r != LUAL_BUFFERSIZE)
			break;
	}

	luaL_pushresult(&B);
	lua_pushnumber(L, data_len);
	if (data_len > 0) {
		lua_pushvalue(L, -2);
		lua_remove(L, -3);
		return 2;
	} else {
		lua_remove(L, -2);
		return 1;
	}
}

static int
uh_lua_strconvert(lua_State *L, int (*convert)(char *, int, const char *, int))
{
	const char *in_buf;
	static char out_buf[4096];
	size_t in_len;
	int out_len;

	in_buf = luaL_checklstring(L, 1, &in_len);
	out_len = convert(out_buf, sizeof(out_buf), in_buf, in_len);

	if (out_len < 0) {
		const char *error;

		if (out_len == -1)
			error = "buffer overflow";
		else
			error = "malformed string";

		luaL_error(L, "%s on URL conversion\n", error);
	}

	lua_pushlstring(L, out_buf, out_len);
	return 1;
}

static int uh_lua_urldecode(lua_State *L)
{
	return uh_lua_strconvert(L, ops->urldecode);
}

static int uh_lua_urlencode(lua_State *L)
{
	return uh_lua_strconvert(L, ops->urlencode);
}

static lua_State *uh_lua_state_init(void)
{
	const char *msg = "(unknown error)";
	const char *status;
	lua_State *L;
	int ret;

	L = lua_open();
	luaL_openlibs(L);

	/* build uhttpd api table */
	lua_newtable(L);

	/* 
	 * use print as send and sendc implementation,
	 * chunked transfer is handled in the main server
	 */
	lua_getglobal(L, "print");
	lua_pushvalue(L, -1);
	lua_setfield(L, -3, "send");
	lua_setfield(L, -2, "sendc");

	lua_pushcfunction(L, uh_lua_recv);
	lua_setfield(L, -2, "recv");

	lua_pushcfunction(L, uh_lua_urldecode);
	lua_setfield(L, -2, "urldecode");

	lua_pushcfunction(L, uh_lua_urlencode);
	lua_setfield(L, -2, "urlencode");

	lua_pushstring(L, conf.docroot);
	lua_setfield(L, -2, "docroot");

	lua_setglobal(L, "uhttpd");

	ret = luaL_loadfile(L, conf.lua_handler);
	if (ret) {
		status = "loading";
		goto error;
	}

	ret = lua_pcall(L, 0, 0, 0);
	if (ret) {
		status = "initializing";
		goto error;
	}

	lua_getglobal(L, UH_LUA_CB);
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "Error: Lua handler provides no " UH_LUA_CB "() callback.\n");
		exit(1);
	}

	return L;

error:
	if (!lua_isnil(L, -1))
		msg = lua_tostring(L, -1);

	fprintf(stderr, "Error %s Lua handler: %s\n", status, msg);
	exit(1);
	return NULL;
}

static void lua_main(struct client *cl, struct path_info *pi, const char *url)
{
	const char *error;
	struct env_var *var;
	lua_State *L = _L;
	int path_len, prefix_len;
	char *str;

	lua_getglobal(L, UH_LUA_CB);

	/* new env table for this request */
	lua_newtable(L);

	prefix_len = strlen(conf.lua_prefix);
	path_len = strlen(url);
	str = strchr(url, '?');
	if (str) {
		pi->query = str;
		path_len = str - url;
	}
	if (path_len > prefix_len) {
		lua_pushlstring(L, url + prefix_len,
				path_len - prefix_len);
		lua_setfield(L, -2, "PATH_INFO");
	}

	for (var = ops->get_process_vars(cl, pi); var->name; var++) {
		if (!var->value)
			continue;

		lua_pushstring(L, var->value);
		lua_setfield(L, -2, var->name);
	}

	lua_pushnumber(L, 0.9 + (cl->request.version / 10.0));
	lua_setfield(L, -2, "HTTP_VERSION");

	switch(lua_pcall(L, 1, 0, 0)) {
	case LUA_ERRMEM:
	case LUA_ERRRUN:
		error = luaL_checkstring(L, -1);
		if (!error)
			error = "(unknown error)";

		printf("Status: 500 Internal Server Error\r\n\r\n"
	       "Unable to launch the requested Lua program:\n"
	       "  %s: %s\n", pi->phys, strerror(errno));
	}

	exit(0);
}

static void lua_handle_request(struct client *cl, const char *url, struct path_info *pi)
{
	static struct path_info _pi;

	pi = &_pi;
	pi->name = conf.lua_prefix;
	pi->phys = conf.lua_handler;

	if (!ops->create_process(cl, pi, url, lua_main)) {
		ops->client_error(cl, 500, "Internal Server Error",
				  "Failed to create CGI process: %s", strerror(errno));
	}
}

static bool check_lua_url(const char *url)
{
	return ops->path_match(conf.lua_prefix, url);
}

static struct dispatch_handler lua_dispatch = {
	.check_url = check_lua_url,
	.handle_request = lua_handle_request,
};

static int lua_plugin_init(const struct uhttpd_ops *o, struct config *c)
{
	ops = o;
	_conf = c;
	_L = uh_lua_state_init();
	ops->dispatch_add(&lua_dispatch);
	return 0;
}

const struct uhttpd_plugin uhttpd_plugin = {
	.init = lua_plugin_init,
};