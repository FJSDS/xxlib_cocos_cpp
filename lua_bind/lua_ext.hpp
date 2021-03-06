﻿#pragma once

inline void Lua_Register_ext(lua_State* const& L)
{
	Lua_NewMT(L, TypeNames<CatchFish_s*>::value, nullptr, true);							// CatchFish

	Lua_NewFunc(L, "Create", [](lua_State* L)
	{
		return Lua_Pushs(L, std::make_shared<CatchFish>());
	});

	Lua_NewFunc(L, "__gc", [](lua_State* L)
	{
		auto&& t = Lua_ToTuple<CatchFish_s*>(L, "__gc error! need 1 args: self");
		(*std::get<0>(t)).~shared_ptr();
		return 0;
	});

	Lua_NewFunc(L, "Init", [](lua_State* L)
	{
		auto&& t = Lua_ToTuple<CatchFish_s*, std::string, int, std::string>(L, "Init error! need 4 args: self, string ip, int port, string cfgName");
		assert(*std::get<0>(t));
		auto&& r = (*std::get<0>(t))->Init(std::move(std::get<1>(t)), std::move(std::get<2>(t)), std::move(std::get<3>(t)));
		return Lua_Pushs(L, r);
	});

	Lua_NewFunc(L, "Update", [](lua_State* L)
	{
		auto&& t = Lua_ToTuple<CatchFish_s*>(L, "Update error! need 1 args: self");
		assert(*std::get<0>(t));
		auto&& r = (*std::get<0>(t))->Update();
		return Lua_Pushs(L, r);
	});

	Lua_NewFunc(L, "Reset", [](lua_State* L)
	{
		auto&& t = Lua_ToTuple<CatchFish_s*>(L, "Reset error! need 1 args: self");
		assert(*std::get<0>(t));
		(*std::get<0>(t)).reset();
		return 0;
	});

	Lua_NewFunc(L, "IsNull", [](lua_State* L)
	{
		auto&& t = Lua_ToTuple<CatchFish_s*>(L, "IsNull error! need 1 args: self");
		return Lua_Pushs(L, !(*std::get<0>(t)));
	});


	lua_pop(L, 1);																		//
	assert(lua_gettop(L) == 0);
}
