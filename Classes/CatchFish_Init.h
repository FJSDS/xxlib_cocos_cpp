﻿inline int CatchFish::Init(std::string cfgName) {
#ifdef CC_TARGET_PLATFORM
	::cc_scene = cocos2d::Director::getInstance()->getRunningScene();
#endif
	// 从文件加载 cfg. 出问题返回非 0
	{
		auto&& data = cocos2d::FileUtils::getInstance()->getDataFromFile(cfgName);
		xx::BBuffer bb;
		bb.Reset(data.getBytes(), data.getSize());
		auto&& r = bb.ReadRoot(cfg);
		bb.Reset();
		if (r) return r;
	}

	// 初始化派生类的东西
	if (int r = cfg->InitCascade()) return r;
	::cfg = &*cfg;									// 存到全局方便访问( 单线程适用 )

	// 模拟收到 sync( 含 players & scene )
	xx::MakeTo(scene);
	::scene = &*scene;								// 存到全局方便访问( 单线程适用 )
	xx::MakeTo(scene->borns);
	xx::MakeTo(scene->fishs);
	xx::MakeTo(scene->freeSits);
	xx::MakeTo(scene->items);
	xx::MakeTo(scene->players);
	xx::MakeTo(scene->rnd, 123);
	xx::MakeTo(scene->stage);

	auto&& plr = xx::Make<Player>();
	selfPlayer = plr;
	plr->scene = ::scene;
	players.Add(std::move(plr));

	scene->players->Add(plr);

	auto&& fish = MakeRandomFish();
	scene->fishs->Add(fish);

	return 0;
}
