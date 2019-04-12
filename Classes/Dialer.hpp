inline int Dialer::Update() noexcept {
	lineNumber = UpdateCore(lineNumber);
	return lineNumber ? 0 : -1;
}

inline int Dialer::UpdateCore(int const& lineNumber) noexcept {
	// todo: client network logic here
	COR_BEGIN
		LabDial :
	// clear flag
	finished = false;

	// event: set flag
	OnAccept = [this](auto peer) {
		finished = true;
	};

	// try connect to server
	Dial(catchFish->serverIp, catchFish->serverPort, 2000);

	// cleanup context data
	Reset();

	xx::CoutN("step 1");

	// wait connected or timeout
	while (!finished) {
		COR_YIELD
	}

	// timeout ?
	if (!peer) {
		// todo: sleep?
		goto LabDial;
	}

	xx::CoutN("step 2");

	// send enter package
	xx::MakeTo(pkgEnter);
	if (r = peer->SendPush(pkgEnter)) {
		// todo: log?
		goto LabDial;
	}

	// wait recv data
	waitMS = xx::NowSteadyEpochMS() + 5000;	// 5 sec timeout
	while (!recvs.size()) {
		COR_YIELD
			if (xx::NowSteadyEpochMS() > waitMS) goto LabDial;
	}

	// first package handle
	if (r = HandleFirstPackage()) {
		// todo: show error?
		goto LabDial;
	}

	xx::CoutN("step 3");

	// peer keeper
	while (!peer->Disposed()) {
		if (r = HandlePackagesOrUpdateScene()) {
			// todo: show error?
			goto LabDial;
		}

		// ping test
		if (::catchFish->scene->frameNumber % 60 == 0) {
			pkgPing->ticks = xx::NowSteadyEpochMS();
			peer->SendRequest(pkgPing, [](xx::Object_s && msg) {
				std::string s;
				if (auto&& pong = xx::As<PKG::Generic::Pong>(msg)) {
					xx::Append(s, "ping: ",xx::NowSteadyEpochMS() - pong->ticks, "ms");
				}
				else {
					s = "ping: timeout";	// todo: 已断开, 重连
				}
				catchFish->SetLabelPingText(s);
				return 0;
			}, 2000);
			peer->Flush();
		}
		COR_YIELD
	}
	COR_END
}

inline int Dialer::HandleFirstPackage() noexcept {
	switch (recvs.front()->GetTypeId()) {
	case xx::TypeId_v<PKG::CatchFish_Client::EnterSuccess>: {
		auto&& es = xx::As<PKG::CatchFish_Client::EnterSuccess>(recvs.front());

		// store players
		for (auto&& p : *es->players) {
			::catchFish->players.Add(xx::As<Player>(p));
		}

		// store scene
		::catchFish->scene = xx::As<Scene>(es->scene);

		// store current player
		player = xx::As<Player>(es->self.lock());

		// set current player's flag
		player->isSelf = true;

		// restore scene
		::catchFish->scene->cfg = &*::catchFish->cfg;
		if (int r = ::catchFish->scene->InitCascade()) return r;

		// restore player
		for (auto&& p : ::catchFish->players) {
			if (int r = p->InitCascade(&*::catchFish->scene)) return r;
		}

		// handle finish, pop / drop.
		recvs.pop_front();
		return 0;
	}
	case xx::TypeId_v<PKG::Generic::Error>: {
		// todo: show error msg?
		return -1;
	}
	default: {
		// todo: log?
		return -1;
	}
	}
	assert(false);
}

inline int Dialer::HandlePackagesOrUpdateScene() noexcept {
	int r = 0;
	bool needUpdateScene = true;
	while (!recvs.empty()) {
		switch (recvs.front()->GetTypeId()) {
		case xx::TypeId_v<PKG::CatchFish_Client::FrameEvents>: {
			auto&& fe = xx::As<PKG::CatchFish_Client::FrameEvents>(recvs.front());
			xx::CoutN(fe->frameNumber - ::catchFish->scene->frameNumber);
			// 如果本地帧编号慢于 server 则追帧
			if (fe->frameNumber > ::catchFish->scene->frameNumber) {
				while (fe->frameNumber > ::catchFish->scene->frameNumber) {
					if (r = ::catchFish->scene->Update()) return r;
				}
				needUpdateScene = false;
			}
			// 依次处理事件集合
			for (auto&& e : *fe->events) {
				switch (e->GetTypeId()) {
				case xx::TypeId_v<PKG::CatchFish::Events::Enter>:
					r = Handle(xx::As<PKG::CatchFish::Events::Enter>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::Leave>:
					r = Handle(xx::As<PKG::CatchFish::Events::Leave>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::NoMoney>:
					r = Handle(xx::As<PKG::CatchFish::Events::NoMoney>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::Refund>:
					r = Handle(xx::As<PKG::CatchFish::Events::Refund>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::FishDead>:
					r = Handle(xx::As<PKG::CatchFish::Events::FishDead>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::PushWeapon>:
					r = Handle(xx::As<PKG::CatchFish::Events::PushWeapon>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::PushFish>:
					r = Handle(xx::As<PKG::CatchFish::Events::PushFish>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::OpenAutoLock>:
					r = Handle(xx::As<PKG::CatchFish::Events::OpenAutoLock>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::Aim>:
					r = Handle(xx::As<PKG::CatchFish::Events::Aim>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::CloseAutoLock>:
					r = Handle(xx::As<PKG::CatchFish::Events::CloseAutoLock>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::OpenAutoFire>:
					r = Handle(xx::As<PKG::CatchFish::Events::OpenAutoFire>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::CloseAutoFire>:
					r = Handle(xx::As<PKG::CatchFish::Events::CloseAutoFire>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::Fire>:
					r = Handle(xx::As<PKG::CatchFish::Events::Fire>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::CannonSwitch>:
					r = Handle(xx::As<PKG::CatchFish::Events::CannonSwitch>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::CannonCoinChange>:
					r = Handle(xx::As<PKG::CatchFish::Events::CannonCoinChange>(e)); break;
				case xx::TypeId_v<PKG::CatchFish::Events::DebugInfo>:
					r = Handle(xx::As<PKG::CatchFish::Events::DebugInfo>(e), fe->frameNumber); break;
				default:
					// todo: log?
					assert(false);	// 不该执行到这里
				}
				if (r) return r;
			}
			break;
		}
		default: {
			// todo: log?
			return -1;
		}
		}
		recvs.pop_front();
	}

	return needUpdateScene ? ::catchFish->scene->Update() : 0;
}

inline void Dialer::Reset() noexcept {
	recvs.clear();
	player.reset();
	::catchFish->players.Clear();
	::catchFish->scene.reset();
}


inline int Dialer::Handle(PKG::CatchFish::Events::Enter_s o) noexcept {
	if (o->playerId == player->id) return 0;		// 忽略自己进入游戏的消息

	// 构建玩家上下文( 模拟收到的数据以方便调用 InitCascade )
	auto&& player = xx::Make<Player>();
	player->autoFire = false;
	player->autoIncId = 0;
	player->autoLock = false;
	player->avatar_id = o->avatar_id;
	xx::MakeTo(player->cannons);
	player->coin = o->coin;
	player->id = o->playerId;
	if (o->nickname) {
		xx::MakeTo(player->nickname, *o->nickname);
	}
	player->noMoney = o->noMoney;
	//player->scene = &*catchFish->scene;
	player->sit = o->sit;
	xx::MakeTo(player->weapons);

	// 构建初始炮台
	switch (o->cannonCfgId) {
	case 0: {
		auto&& cannonCfg = catchFish->cfg->cannons->At(o->cannonCfgId);
		auto&& cannon = xx::Make<Cannon>();
		cannon->angle = float(cannonCfg->angle);
		xx::MakeTo(cannon->bullets);
		cannon->cfgId = o->cannonCfgId;
		cannon->coin = o->cannonCoin;
		cannon->id = (int)player->cannons->len;
		//cannon->player = &*player;
		//cannon->cfg = &*cannonCfg;
		//cannon->pos = catchFish->cfg->sitPositons->At((int)o->sit);
		cannon->quantity = cannonCfg->quantity;
		cannon->scene = &*catchFish->scene;
		cannon->fireCD = 0;
		player->cannons->Add(cannon);
		break;
	}
			// todo: more cannon types here
	default:
		return -2;
	}

	// 将玩家放入相应容器
	catchFish->players.Add(player);
	catchFish->scene->players->Add(player);

	// 进一步初始化
	return player->InitCascade(&*catchFish->scene);
}
inline int Dialer::Handle(PKG::CatchFish::Events::Leave_s o) noexcept {
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::NoMoney_s o) noexcept {
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::Refund_s o) noexcept {
	player->coin += o->coin;
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::FishDead_s o) noexcept {
	auto&& fs = *player->scene->fishs;
	for (auto&& f : fs) {
		if (f->id == o->fishId) {
			fs[fs.len - 1]->indexAtContainer = f->indexAtContainer;
			fs.SwapRemoveAt(f->indexAtContainer);
			// todo: 加钱
			// todo: 鱼死特效
			return 0;
		}
	}
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::PushWeapon_s o) noexcept {
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::PushFish_s o) noexcept {
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::OpenAutoLock_s o) noexcept {
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::Aim_s o) noexcept {
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::CloseAutoLock_s o) noexcept {
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::OpenAutoFire_s o) noexcept {
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::CloseAutoFire_s o) noexcept {
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::Fire_s o) noexcept {
	// 如果是自己发射的就忽略绘制
	if (o->playerId == player->id) return 0;
	for (auto&& p : catchFish->players) {
		if (p->id == o->playerId) {
			for (auto&& c : *p->cannons) {
				if (c->id == o->cannonId) {
					auto&& cannon = xx::As<Cannon>(c);
					cannon->coin = o->coin;					// todo: 理论上如果做完了币值切换通知就不需要这个赋值了
					cannon->angle = o->tarAngle;
					(void)cannon->Fire(o->frameNumber);
				}
			}
		}
	}
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::CannonSwitch_s o) noexcept {
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::CannonCoinChange_s o) noexcept {
	return 0;
}
inline int Dialer::Handle(PKG::CatchFish::Events::DebugInfo_s o, int const& frameNumber) noexcept {
	auto&& fishIds = ::catchFish->scene->fishIdss[frameNumber];
	assert(fishIds.len == o->fishIds->len);
	std::sort(fishIds.buf, fishIds.buf + fishIds.len);
	std::sort(o->fishIds->buf, o->fishIds->buf + o->fishIds->len);
	assert(memcmp(fishIds.buf, o->fishIds->buf, fishIds.len * sizeof(int)) == 0);
	return 0;
}
