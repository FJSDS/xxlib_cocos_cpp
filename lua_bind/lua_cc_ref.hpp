﻿#pragma once

inline void Lua_Register_Ref(lua_State* const& L)
{
	Lua_NewMT(L, TypeNames<cocos2d::Ref*>::value);					// cc, Ref

	Lua_NewFunc(L, "retain", [](lua_State* L)
	{
		auto&& t = Lua_ToTuple<cocos2d::Ref*>(L, "retain error! need 1 args: self");
		std::get<0>(t)->retain();
		return 0;
	});

	Lua_NewFunc(L, "release", [](lua_State* L)
	{
		auto&& t = Lua_ToTuple<cocos2d::Ref*>(L, "release error! need 1 args: self");
		std::get<0>(t)->release();
		return 0;
	});

	Lua_NewFunc(L, "autorelease", [](lua_State* L)
	{
		auto&& t = Lua_ToTuple<cocos2d::Ref*>(L, "autorelease error! need 1 args: self");
		std::get<0>(t)->autorelease();
		return 0;
	});

	Lua_NewFunc(L, "getReferenceCount", [](lua_State* L)
	{
		auto&& t = Lua_ToTuple<cocos2d::Ref*>(L, "getReferenceCount error! need 1 args: self");
		auto&& r = std::get<0>(t)->getReferenceCount();
		return Lua_Push(L, r);
	});

	lua_pop(L, 1);													// cc
}
