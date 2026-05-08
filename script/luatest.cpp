#include <iostream>
#include "include/lua.hpp"

#pragma comment (lib, "lua55.lib")
using namespace std;

int main()
{
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);
	luaL_loadfile(L, "dragon.lua");
	int error = lua_pcall(L, 0, 0, 0);
	if (error) {
		cout << "Error:" << lua_tostring(L, -1);
		lua_pop(L, 1);
	}

	//lua_getglobal(L, "pos_x");
	//lua_getglobal(L, "pos_y");
	//int pos_x = (int)lua_tonumber(L, -2);
	//int pos_y = (int)lua_tonumber(L, -1);

	//printf("Pos_x %d, Pos_Y %d\n", pos_x, pos_y);

	//lua_pop(L, 2);
	
	lua_getglobal(L, "addtwo");
	lua_pushnumber(L, 5);
	error = lua_pcall(L, 1, 1, 0);
	if (error) {
		cout << "Error:" << lua_tostring(L, -1);
		lua_pop(L, 1);
	}
	int result = (int)lua_tonumber(L, -1);
	printf("Result %d\n", result);
	lua_pop(L, 1);

	lua_close(L);
}