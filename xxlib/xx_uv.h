﻿#pragma once
#include "uv.h"
#include "xx_bbuffer.h"
#include "xx_dict.h"
#include "ikcp.h"

namespace xx {
	struct UvKcp;
	struct Uv {
		uv_loop_t uvLoop;
		BBuffer recvBB;								// shared deserialization for package receive. direct replace buf when using
		BBuffer sendBB;								// shared serialization for package send
		int autoId = 0;								// udps key, udp dialer port gen: --autoId
		Dict<int, std::weak_ptr<UvKcp>> udps;		// key: port( dialer peer port = autoId )
		char* recvBuf = nullptr;					// shared receive buf for kcp
		size_t recvBufLen = 65535;					// shared receive buf's len
		uv_run_mode runMode = UV_RUN_DEFAULT;		// reduce frame client update kcp delay

		Uv() {
			if (int r = uv_loop_init(&uvLoop)) throw r;
			recvBuf = new char[recvBufLen];
		}
		Uv(Uv const&) = delete;
		Uv& operator=(Uv const&) = delete;

		~Uv() {
			recvBB.Reset();					// clear replaced buf.
			if (recvBuf) {
				delete[] recvBuf;
				recvBuf = nullptr;
			}

			int r = uv_run(&uvLoop, UV_RUN_DEFAULT);
			assert(!r);
			r = uv_loop_close(&uvLoop);
			assert(!r);
		}

		int Run(uv_run_mode const& mode = UV_RUN_DEFAULT) noexcept {
			runMode = mode;
			return uv_run(&uvLoop, mode);
		}

		inline void Stop() {
			uv_stop(&uvLoop);
		}

		template<typename T>
		static T* Alloc(void* const& ud) noexcept {
			auto p = (void**)::malloc(sizeof(void*) + sizeof(T));
			if (!p) return nullptr;
			p[0] = ud;
			return (T*)& p[1];
		}
		inline static void Free(void* const& p) noexcept {
			::free((void**)p - 1);
		}
		template<typename T>
		static T* GetSelf(void* const& p) noexcept {
			return (T*)* ((void**)p - 1);
		}
		template<typename T>
		static void HandleCloseAndFree(T * &tar) noexcept {
			if (!tar) return;
			auto h = (uv_handle_t*)tar;
			tar = nullptr;
			assert(!uv_is_closing(h));
			uv_close(h, [](uv_handle_t * handle) {
				Uv::Free(handle);
				});
		}
		inline static void AllocCB(uv_handle_t * h, size_t suggested_size, uv_buf_t * buf) noexcept {
			buf->base = (char*)::malloc(suggested_size);
			buf->len = decltype(uv_buf_t::len)(suggested_size);
		}

		inline static int FillIP(sockaddr_in6 & saddr, std::string & ip, bool includePort = true) noexcept {
			ip.resize(64);
			if (saddr.sin6_family == AF_INET6) {
				if (int r = uv_ip6_name(&saddr, (char*)ip.data(), ip.size())) return r;
				ip.resize(strlen(ip.data()));
				if (includePort) {
					ip.append(":");
					ip.append(std::to_string(ntohs(saddr.sin6_port)));
				}
			}
			else {
				if (int r = uv_ip4_name((sockaddr_in*)& saddr, (char*)ip.data(), ip.size())) return r;
				ip.resize(strlen(ip.data()));
				if (includePort) {
					ip.append(":");
					ip.append(std::to_string(ntohs(((sockaddr_in*)& saddr)->sin_port)));
				}
			}
			return 0;
		}
		inline static int FillIP(uv_tcp_t * stream, std::string & ip, bool includePort = true) noexcept {
			sockaddr_in6 saddr;
			int len = sizeof(saddr);
			int r = 0;
			if ((r = uv_tcp_getpeername(stream, (sockaddr*)& saddr, &len))) return r;
			return FillIP(saddr, ip, includePort);
		}
		inline static std::string ToIpPortString(sockaddr const* const& addr, bool includePort = true) noexcept {
			sockaddr_in6 a;
			memcpy(&a, addr, sizeof(a));
			std::string ipAndPort;
			Uv::FillIP(a, ipAndPort, includePort);
			return ipAndPort;
		}
		inline static std::string ToIpPortString(sockaddr_in6 const& addr, bool includePort = true) noexcept {
			return ToIpPortString((sockaddr*)& addr, includePort);
		}
	};

	struct UvItem : std::enable_shared_from_this<UvItem> {
		Uv& uv;
		UvItem(Uv& uv) : uv(uv) {}
		virtual ~UvItem() {}
		// must be call Dispose() at all inherit class if override following funcs
		/*
			~TTTTTTTT() { this->Dispose(0); }
		*/
		virtual bool Disposed() const noexcept = 0;
		virtual void Dispose(int const& flag) noexcept = 0;	// flag == 0 : call by ~T()

		// user data
		void* userData = nullptr;
	};
	using UvItem_s = std::shared_ptr<UvItem>;
	using UvItem_w = std::weak_ptr<UvItem>;

	struct UvAsync : UvItem {
		uv_async_t* uvAsync = nullptr;
		std::mutex mtx;
		std::deque<std::function<void()>> actions;
		std::function<void()> action;	// for pop store

		UvAsync(Uv& uv)
			: UvItem(uv) {
			uvAsync = Uv::Alloc<uv_async_t>(this);
			if (!uvAsync) throw - 1;
			if (int r = uv_async_init(&uv.uvLoop, uvAsync, [](uv_async_t * handle) {
				Uv::GetSelf<UvAsync>(handle)->Execute();
				})) {
				uvAsync = nullptr;
				throw r;
			}
		}
		UvAsync(UvAsync const&) = delete;
		UvAsync& operator=(UvAsync const&) = delete;
		~UvAsync() { this->Dispose(0); }

		inline virtual bool Disposed() const noexcept override {
			return !uvAsync;
		}
		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (!uvAsync) return;
			Uv::HandleCloseAndFree(uvAsync);
			if (flag) {
				auto holder = shared_from_this();
				actions.clear();
			}
		}
		inline int Dispatch(std::function<void()>&& action) noexcept {
			if (!uvAsync) return -1;
			{
				std::scoped_lock<std::mutex> g(mtx);
				actions.push_back(std::move(action));
			}
			return uv_async_send(uvAsync);
		}

	protected:
		inline void Execute() noexcept {
			{
				std::scoped_lock<std::mutex> g(mtx);
				action = std::move(actions.front());
				actions.pop_front();
			}
			action();
		}
	};
	using UvAsync_s = std::shared_ptr<UvAsync>;
	using UvAsync_w = std::weak_ptr<UvAsync>;

	struct UvTimer : UvItem {
		uv_timer_t* uvTimer = nullptr;
		uint64_t timeoutMS = 0;
		uint64_t repeatIntervalMS = 0;
		std::function<void()> onFire;

		UvTimer(Uv& uv)
			: UvItem(uv) {
			uvTimer = Uv::Alloc<uv_timer_t>(this);
			if (!uvTimer) throw - 1;
			if (int r = uv_timer_init(&uv.uvLoop, uvTimer)) {
				uvTimer = nullptr;
				throw r;
			}
		}
		UvTimer(Uv& uv, uint64_t const& timeoutMS, uint64_t const& repeatIntervalMS, std::function<void()>&& onFire = nullptr)
			: UvTimer(uv) {
			if (int r = Start(timeoutMS, repeatIntervalMS, std::move(onFire))) throw r;
		}
		UvTimer(UvTimer const&) = delete;
		UvTimer& operator=(UvTimer const&) = delete;
		~UvTimer() { this->Dispose(0); }

		inline virtual bool Disposed() const noexcept override {
			return !uvTimer;
		}
		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (!uvTimer) return;
			Uv::HandleCloseAndFree(uvTimer);
			if (flag) {
				auto holder = shared_from_this();
				onFire = nullptr;
			}
		}

		inline int Start(uint64_t const& timeoutMS, uint64_t const& repeatIntervalMS, std::function<void()> && onFire = nullptr) noexcept {
			if (!uvTimer) return -1;
			this->timeoutMS = timeoutMS;
			this->repeatIntervalMS = repeatIntervalMS;
			this->onFire = std::move(onFire);
			return uv_timer_start(uvTimer, Fire, timeoutMS, repeatIntervalMS);
		}
		inline int Restart() noexcept {
			if (!uvTimer) return -1;
			return uv_timer_start(uvTimer, Fire, timeoutMS, repeatIntervalMS);
		}
		inline int Stop() noexcept {
			if (!uvTimer) return -1;
			return uv_timer_stop(uvTimer);
		}
		inline int Again() noexcept {
			if (!uvTimer) return -1;
			return uv_timer_again(uvTimer);
		}

		// force the loop to exit early by unreferencing handles which are active
		inline void Unref() noexcept {
			uv_unref((uv_handle_t*)uvTimer);
		}

	protected:
		inline static void Fire(uv_timer_t * t) {
			auto self = Uv::GetSelf<UvTimer>(t);
			if (self->onFire) {
				self->onFire();
			}
		}
	};
	using UvTimer_s = std::shared_ptr<UvTimer>;
	using UvTimer_w = std::weak_ptr<UvTimer>;

	struct UvResolver;
	using UvResolver_s = std::shared_ptr<UvResolver>;
	using UvResolver_w = std::weak_ptr<UvResolver>;
	struct uv_getaddrinfo_t_ex {
		uv_getaddrinfo_t req;
		UvResolver_w resolver_w;
	};

	struct UvResolver : UvItem {
		uv_getaddrinfo_t_ex* req = nullptr;
		UvTimer_s timeouter;
		bool disposed = false;
		std::vector<std::string> ips;
		std::function<void()> onFinish;
#ifdef __IPHONE_OS_VERSION_MIN_REQUIRED
		addrinfo hints;
#endif

		UvResolver(Uv& uv) noexcept
			: UvItem(uv) {
#ifdef __IPHONE_OS_VERSION_MIN_REQUIRED
			hints.ai_family = PF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_protocol = 0;// IPPROTO_TCP;
			hints.ai_flags = AI_DEFAULT;
#endif
		}

		UvResolver(UvResolver const&) = delete;
		UvResolver& operator=(UvResolver const&) = delete;
		~UvResolver() { this->Dispose(0); }

		inline virtual bool Disposed() const noexcept override {
			return disposed;
		}
		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (disposed) return;
			Cancel();
			if (flag) {
				auto holder = shared_from_this();
				onFinish = nullptr;
			}
		}

		inline void Cancel() {
			if (disposed) return;
			ips.clear();
			if (req) {
				uv_cancel((uv_req_t*)req);
				req = nullptr;
			}
			timeouter.reset();
		}

		inline int Resolve(std::string const& domainName, uint64_t const& timeoutMS = 0) noexcept {
			if (disposed) return -1;
			Cancel();
			if (timeoutMS) {
				TryMakeTo(timeouter, uv, timeoutMS, 0, [this] {
					Cancel();
					if (onFinish) {
						onFinish();
					}
					});
				if (!timeouter) return -2;
			}
			auto req = std::make_unique<uv_getaddrinfo_t_ex>();
			req->resolver_w = As<UvResolver>(shared_from_this());
			if (int r = uv_getaddrinfo((uv_loop_t*)& uv.uvLoop, (uv_getaddrinfo_t*)& req->req, [](uv_getaddrinfo_t * req_, int status, struct addrinfo* ai) {
				auto req = std::unique_ptr<uv_getaddrinfo_t_ex>(container_of(req_, uv_getaddrinfo_t_ex, req));
				if (status) return;													// error or -4081 canceled
				auto resolver = req->resolver_w.lock();
				if (!resolver) return;
				resolver->req = nullptr;
				assert(ai);

				auto& ips = resolver->ips;
				std::string s;
				do {
					s.resize(64);
					if (ai->ai_addr->sa_family == AF_INET6) {
						uv_ip6_name((sockaddr_in6*)ai->ai_addr, (char*)s.data(), s.size());
					}
					else {
						uv_ip4_name((sockaddr_in*)ai->ai_addr, (char*)s.data(), s.size());
					}
					s.resize(strlen(s.data()));

					if (std::find(ips.begin(), ips.end(), s) == ips.end()) {
						ips.push_back(std::move(s));								// known issue: ios || android will receive duplicate result
					}
					ai = ai->ai_next;
				} while (ai);
				uv_freeaddrinfo(ai);

				resolver->timeouter.reset();
				if (resolver->onFinish) {
					resolver->onFinish();
				}

				}, domainName.c_str(), nullptr,
#ifdef __IPHONE_OS_VERSION_MIN_REQUIRED
				(const addrinfo*) & hints
#else
					nullptr
#endif
					)) return r;
			this->req = req.release();
			return 0;
		}
	};

	struct UvPeer;
	using UvPeer_s = std::shared_ptr<UvPeer>;
	using UvPeer_w = std::weak_ptr<UvPeer>;

	struct UvPeerBase : UvItem {
		using UvItem::UvItem;
		UvPeer* peer = nullptr;
		virtual std::string GetIP() noexcept = 0;
		virtual int SendPackage(Object_s const& data, int32_t const& serial = 0) noexcept = 0;
		virtual int SendPackage(BBuffer const& data, int32_t const& serial = 0) noexcept = 0;		// for lua
		virtual void Flush() noexcept = 0;
		virtual int Update(int64_t const& nowMS) noexcept = 0;
		virtual bool IsKcp() noexcept = 0;
	};
	using UvPeerBase_s = std::shared_ptr<UvPeerBase>;

	struct UvCreateAcceptBase : UvItem {
		using UvItem::UvItem;
		std::function<UvPeer_s(Uv& uv)> onCreatePeer;
		std::function<void(UvPeer_s peer)> onAccept;

		virtual UvPeer_s CreatePeer() noexcept;
		virtual void Accept(UvPeer_s peer) noexcept;
	};

	struct UvKcpListener;
	struct UvTcpListener;
	struct UvListener : UvCreateAcceptBase {
		std::shared_ptr<UvTcpListener> tcpListener;
		std::shared_ptr<UvKcpListener> kcpListener;

		UvListener(Uv& uv, std::string const& ip, int const& port, int const& tcpKcpOpt = 2);
		~UvListener() {
			Dispose(0);
		}
		inline virtual bool Disposed() const noexcept override {
			return !tcpListener && !kcpListener;
		}
		virtual void Dispose(int const& flag) noexcept override;
	};
	using UvListener_s = std::shared_ptr<UvListener>;
	using UvListener_w = std::weak_ptr<UvListener>;


	struct UvListenerBase : UvItem {
		using UvItem::UvItem;
		UvListener* listener = nullptr;
		virtual void Accept(UvPeerBase_s peer) noexcept;
	};

	struct UvDialer;
	struct UvDialerBase : UvItem {
		using UvItem::UvItem;
		UvDialer* dialer = nullptr;
		bool disposed = false;
		inline virtual bool Disposed() const noexcept override {
			return disposed;
		}
		virtual int Dial(std::string const& ip, int const& port, uint64_t const& timeoutMS = 0, bool cleanup = true) noexcept = 0;
		virtual int Dial(std::vector<std::string> const& ips, int const& port, uint64_t const& timeoutMS) noexcept;
		virtual int Dial(std::vector<std::pair<std::string, int>> const& ipports, uint64_t const& timeoutMS) noexcept;
		virtual void Accept(UvPeerBase_s pb) noexcept;
		virtual void Cancel() noexcept = 0;
	};

	struct UvPeer : UvItem {
		UvPeerBase_s peerBase;		// fill after make
		Dict<int, std::pair<std::function<int(Object_s&& msg)>, int64_t>> callbacks;
		int serial = 0;
		int64_t timeoutMS = 0;
		UvTimer_s timer;
		std::function<void()> onDisconnect;
		std::function<int(Object_s&& msg)> onReceivePush;
		std::function<int(int const& serial, Object_s&& msg)> onReceiveRequest;
		std::string ip;			// cache

		UvPeer(Uv& uv)
			: UvItem(uv) {
			MakeTo(timer, uv, 10, 10, [this] {
				Update(NowSteadyEpochMS());
				});
		}

		inline std::string GetIP() noexcept {
			if (!peerBase || ip.size()) return ip;
			return ip = peerBase->GetIP();
		}

		inline bool IsKcp() noexcept {
			if (!peerBase) return false;
			return peerBase->IsKcp();
		}

		inline void ResetTimeoutMS(int64_t const& ms) noexcept {
			timeoutMS = ms ? NowSteadyEpochMS() + ms : 0;
		}

		inline void Flush() noexcept {
			peerBase->Flush();
		};

		inline virtual void Disconnect() noexcept {
			if (onDisconnect) {
				onDisconnect();
			}
		}

		inline virtual int ReceivePush(Object_s&& msg) noexcept {
			return onReceivePush ? onReceivePush(std::move(msg)) : 0;
		}

		inline virtual int ReceiveRequest(int const& serial, Object_s&& msg) noexcept {
			return onReceiveRequest ? onReceiveRequest(serial, std::move(msg)) : 0;
		}

		inline int SendPush(Object_s const& data) noexcept {
			return peerBase->SendPackage(data);
		}

		inline int SendResponse(int32_t const& serial, Object_s const& data) noexcept {
			return peerBase->SendPackage(data, serial);
		}

		inline int SendRequest(Object_s const& data, std::function<int(Object_s&& msg)>&& cb, uint64_t const& timeoutMS) noexcept {
			if (!peerBase) return -1;
			std::pair<std::function<int(Object_s && msg)>, int64_t> v;
			serial = (serial + 1) & 0x7FFFFFFF;			// uint circle use
			if (timeoutMS) {
				v.second = NowSteadyEpochMS() + timeoutMS;
			}
			if (int r = peerBase->SendPackage(data, -serial)) return r;
			v.first = std::move(cb);
			callbacks[serial] = std::move(v);
			return 0;
		}

		inline virtual int HandlePack(uint8_t * const& recvBuf, uint32_t const& recvLen) noexcept {
			// for kcp listener accept
			if (recvLen == 1 && *recvBuf == 0) {
				ip = peerBase->GetIP();
				return 0;
			}

			auto & recvBB = uv.recvBB;
			recvBB.Reset((uint8_t*)recvBuf, recvLen);

			int serial = 0;
			if (int r = recvBB.Read(serial)) return r;
			Object_s msg;
			if (int r = recvBB.ReadRoot(msg)) return r;

			if (serial == 0) {
				return ReceivePush(std::move(msg));
			}
			else if (serial < 0) {
				return ReceiveRequest(-serial, std::move(msg));
			}
			else {
				auto&& idx = callbacks.Find(serial);
				if (idx == -1) return 0;
				auto a = std::move(callbacks.ValueAt(idx).first);
				callbacks.RemoveAt(idx);
				return a(std::move(msg));
			}
		}

		// call by timer
		inline virtual int Update(int64_t const& nowMS) noexcept {
			assert(peerBase);
			if (timeoutMS && timeoutMS < nowMS) {
				Dispose(1);
				return -1;
			}

			if (int r = peerBase->Update(nowMS)) return r;

			for (auto&& iter = callbacks.begin(); iter != callbacks.end(); ++iter) {
				auto&& v = iter->value;
				if (v.second < nowMS) {
					auto a = std::move(v.first);
					iter.Remove();
					a(nullptr);
				}
			}

			return 0;
		}

		inline virtual bool Disposed() const noexcept override {
			return !peerBase;
		}

		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (!peerBase) return;
			peerBase.reset();
			timer.reset();
			for (auto&& kv : callbacks) {
				kv.value.first(nullptr);
			}
			callbacks.Clear();
			if (flag) {
				auto holder = shared_from_this();
				Disconnect();
				onDisconnect = nullptr;
				onReceivePush = nullptr;
				onReceiveRequest = nullptr;
			}
		}
	};

	inline UvPeer_s UvCreateAcceptBase::CreatePeer() noexcept {
		return onCreatePeer ? onCreatePeer(uv) : TryMake<UvPeer>(uv);
	}

	inline void UvCreateAcceptBase::Accept(UvPeer_s peer) noexcept {
		if (onAccept) {
			assert(!peer || peer->peerBase);
			onAccept(peer);
		}
	}

	inline void UvListenerBase::Accept(UvPeerBase_s pb) noexcept {
		assert(pb);
		auto&& p = listener->CreatePeer();
		p->peerBase = pb;
		pb->peer = &*p;
		listener->Accept(p);
	}

	struct uv_write_t_ex : uv_write_t {
		uv_buf_t buf;
	};

	struct UvTcpPeerBase : UvPeerBase {
		uv_tcp_t* uvTcp = nullptr;
		std::string ip;
		Buffer buf;

		UvTcpPeerBase(Uv& uv) : UvPeerBase(uv) {
			uvTcp = Uv::Alloc<uv_tcp_t>(this);
			if (!uvTcp) throw - 1;
			if (int r = uv_tcp_init(&uv.uvLoop, uvTcp)) {
				uvTcp = nullptr;
				throw r;
			}
		}

		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (!uvTcp) return;
			Uv::HandleCloseAndFree(uvTcp);
			if (flag) {
				peer->Dispose(flag);
			}
		}

		~UvTcpPeerBase() {
			Dispose(0);
		}

		inline virtual bool Disposed() const noexcept override {
			return !uvTcp;
		}

		inline virtual bool IsKcp() noexcept override {
			return false;
		}

		inline virtual std::string GetIP() noexcept override {
			return ip;
		}

		// serial == 0: push    > 0: response    < 0: request
		template<typename Data>
		inline int SendPackageCore(Data const& data, int32_t const& serial = 0) noexcept {
			if (!uvTcp) return -1;
			auto& sendBB = uv.sendBB;
			static_assert(sizeof(uv_write_t_ex) + 4 <= 1024);
			sendBB.Reserve(1024);
			sendBB.len = sizeof(uv_write_t_ex) + 4;		// skip uv_write_t_ex + header space
			sendBB.Write(serial);
			if constexpr (std::is_same_v<BBuffer, Data>) {
				sendBB.AddRange(data.buf, data.len);
			}
			else {
				sendBB.WriteRoot(data);
			}
			auto buf = sendBB.buf;						// cut buf memory for send
			auto len = sendBB.len - sizeof(uv_write_t_ex) - 4;
			sendBB.buf = nullptr;
			sendBB.len = 0;
			sendBB.cap = 0;
			return SendReqAndData(buf, (uint32_t)len);
		}

		inline virtual int SendPackage(Object_s const& data, int32_t const& serial = 0) noexcept override {
			return SendPackageCore(data, serial);
		}
		inline virtual int SendPackage(BBuffer const& data, int32_t const& serial = 0) noexcept override {
			return SendPackageCore(data, serial);
		}

		inline virtual void Flush() noexcept override {}

		inline virtual int Update(int64_t const& nowMS) noexcept override { return 0; }


		// called by dialer or listener
		inline int ReadStart() noexcept {
			if (!uvTcp) return -1;
			return uv_read_start((uv_stream_t*)uvTcp, Uv::AllocCB, [](uv_stream_t * stream, ssize_t nread, const uv_buf_t * buf) {
				auto self = Uv::GetSelf<UvTcpPeerBase>(stream);
				auto holder = self->shared_from_this();	// hold for callback Dispose
				if (nread > 0) {
					nread = self->Unpack((uint8_t*)buf->base, (uint32_t)nread);
				}
				if (buf) ::free(buf->base);
				if (nread < 0) {
					if (!self->Disposed()) {
						self->Dispose(1);
					}
				}
				});
		}

		// 4 byte len header. can override for write custom header format
		virtual int Unpack(uint8_t * const& recvBuf, uint32_t const& recvLen) noexcept {
			buf.AddRange(recvBuf, recvLen);
			size_t offset = 0;
			while (offset + 4 <= buf.len) {							// ensure header len( 4 bytes )
				auto len = buf[offset + 0] + (buf[offset + 1] << 8) + (buf[offset + 2] << 16) + (buf[offset + 3] << 24);
				if (len <= 0 /* || len > maxLimit */) return -1;	// invalid length
				if (offset + 4 + len > buf.len) break;				// not enough data

				offset += 4;
				if (int r = peer->HandlePack(buf.buf + offset, len)) return r;
				offset += len;
			}
			buf.RemoveFront(offset);
			return 0;
		}

		inline int Send(uint8_t const* const& buf, ssize_t const& dataLen) noexcept {
			if (!uvTcp) return -1;
			auto req = (uv_write_t_ex*)::malloc(sizeof(uv_write_t_ex) + dataLen);
			if (!req) return -2;
			memcpy(req + 1, buf, dataLen);
			req->buf.base = (char*)(req + 1);
			req->buf.len = decltype(uv_buf_t::len)(dataLen);
			return SendReq(req);
		}

		// launch a send request
		inline int SendReq(uv_write_t_ex * const& req) noexcept {
			if (!uvTcp) return -1;
			// todo: check send queue len ? protect?  uv_stream_get_write_queue_size((uv_stream_t*)uvTcp);
			int r = uv_write(req, (uv_stream_t*)uvTcp, &req->buf, 1, [](uv_write_t * req, int status) {
				::free(req);
				});
			if (r) Dispose(1);
			return r;
		}

		// fast mode. req + data 2N1, reduce malloc times.
		// reqbuf = uv_write_t_ex space + len space + data, len = data's len
		inline int SendReqAndData(uint8_t * const& reqbuf, uint32_t const& len) {
			reqbuf[sizeof(uv_write_t_ex) + 0] = uint8_t(len);		// fill package len
			reqbuf[sizeof(uv_write_t_ex) + 1] = uint8_t(len >> 8);
			reqbuf[sizeof(uv_write_t_ex) + 2] = uint8_t(len >> 16);
			reqbuf[sizeof(uv_write_t_ex) + 3] = uint8_t(len >> 24);

			auto req = (uv_write_t_ex*)reqbuf;						// fill req args
			req->buf.base = (char*)(req + 1);
			req->buf.len = decltype(uv_buf_t::len)(len + 4);
			return SendReq(req);
		}
	};

	struct uv_udp_send_t_ex : uv_udp_send_t {
		uv_buf_t buf;
	};

	struct UvKcp : UvItem {
		uv_udp_t* uvUdp = nullptr;
		sockaddr_in6 addr;
		Buffer buf;
		int port = 0;								// fill by owner. dict's key. port > 0: listener  < 0: dialer fill by --uv.udpId
		virtual void Remove(uint32_t const& conv) noexcept = 0;

		UvKcp(Uv& uv, std::string const& ip, int const& port, bool const& isListener)
			: UvItem(uv) {
			if (ip.size()) {
				if (ip.find(':') == std::string::npos) {
					if (int r = uv_ip4_addr(ip.c_str(), port, (sockaddr_in*)& addr)) throw r;
				}
				else {
					if (int r = uv_ip6_addr(ip.c_str(), port, &addr)) throw r;
				}
			}
			uvUdp = Uv::Alloc<uv_udp_t>(this);
			if (!uvUdp) throw - 2;
			if (int r = uv_udp_init(&uv.uvLoop, uvUdp)) {
				uvUdp = nullptr;
				throw r;
			}
			ScopeGuard sgUdp([this] { Uv::HandleCloseAndFree(uvUdp); });
			if (isListener) {
				if (int r = uv_udp_bind(uvUdp, (sockaddr*)& addr, UV_UDP_REUSEADDR)) throw r;
			}
			if (int r = uv_udp_recv_start(uvUdp, Uv::AllocCB, [](uv_udp_t * handle, ssize_t nread, const uv_buf_t * buf, const struct sockaddr* addr, unsigned flags) {
				auto self = Uv::GetSelf<UvKcp>(handle);
				auto holder = self->shared_from_this();	// hold for callback Dispose
				if (nread > 0) {
					nread = self->Unpack((uint8_t*)buf->base, (uint32_t)nread, addr);
				}
				if (buf) ::free(buf->base);
				if (nread < 0) {
					if (!self->Disposed()) {
						self->Dispose(1);
					}
				}
				})) throw r;
			sgUdp.Cancel();
		}
		UvKcp(UvKcp const&) = delete;
		UvKcp& operator=(UvKcp const&) = delete;
		~UvKcp() { Dispose(0); }

		inline virtual bool Disposed() const noexcept override {
			return !uvUdp;
		}

		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (!uvUdp) return;
			Uv::HandleCloseAndFree(uvUdp);
		}

		// send target: addr or this->addr
		inline virtual int Send(uint8_t const* const& buf, ssize_t const& dataLen, sockaddr const* const& addr = nullptr) noexcept {
			if (!uvUdp) return -1;
			auto req = (uv_udp_send_t_ex*)::malloc(sizeof(uv_udp_send_t_ex) + dataLen);
			if (!req) return -2;
			memcpy(req + 1, buf, dataLen);
			req->buf.base = (char*)(req + 1);
			req->buf.len = decltype(uv_buf_t::len)(dataLen);
			return Send(req, addr);
		}

	protected:
		virtual int Unpack(uint8_t * const& recvBuf, uint32_t const& recvLen, sockaddr const* const& addr) noexcept = 0;

		// reqbuf = uv_udp_send_t_ex space + len space + data
		// len = data's len
		inline int SendReqAndData(uint8_t * const& reqbuf, uint32_t const& len, sockaddr const* const& addr = nullptr) {
			reqbuf[sizeof(uv_udp_send_t_ex) + 0] = uint8_t(len);		// fill package len
			reqbuf[sizeof(uv_udp_send_t_ex) + 1] = uint8_t(len >> 8);
			reqbuf[sizeof(uv_udp_send_t_ex) + 2] = uint8_t(len >> 16);
			reqbuf[sizeof(uv_udp_send_t_ex) + 3] = uint8_t(len >> 24);

			auto req = (uv_udp_send_t_ex*)reqbuf;						// fill req args
			req->buf.base = (char*)(req + 1);
			req->buf.len = decltype(uv_buf_t::len)(len + 4);
			return Send(req, addr);
		}

		inline int Send(uv_udp_send_t_ex * const& req, sockaddr const* const& addr = nullptr) noexcept {
			if (!uvUdp) return -1;
			// todo: check send queue len ? protect?
			int r = uv_udp_send(req, uvUdp, &req->buf, 1, addr ? addr : (sockaddr*)& this->addr, [](uv_udp_send_t * req, int status) {
				::free(req);
				});
			if (r) Dispose(1);
			return r;
		}
	};

	struct UvKcpPeerBase : UvPeerBase {
		using UvPeerBase::UvPeerBase;
		std::shared_ptr<UvKcp> udp;					// fill by creater
		uint32_t conv = 0;							// fill by creater
		int64_t createMS = 0;						// fill by creater
		ikcpcb* kcp = nullptr;
		uint32_t nextUpdateMS = 0;					// for kcp update interval control. reduce cpu usage
		Buffer buf;
		sockaddr_in6 addr;							// for Send. fill by owner Unpack

		// require: fill udp, conv, createMS, addr
		inline int InitKcp() {
			if (kcp) return -1;
			assert(conv);
			kcp = ikcp_create(conv, this);
			if (!kcp) return -1;
			ScopeGuard sgKcp([&] { ikcp_release(kcp); kcp = nullptr; });
			if (int r = ikcp_wndsize(kcp, 1024, 1024)) return r;
			if (int r = ikcp_nodelay(kcp, 1, 10, 2, 1)) return r;
			kcp->rx_minrto = 10;
			kcp->stream = 1;
			ikcp_setoutput(kcp, [](const char* inBuf, int len, ikcpcb * kcp, void* user)->int {
				auto self = ((UvKcpPeerBase*)user);
				return self->udp->Send((uint8_t*)inBuf, len, (sockaddr*)& self->addr);
				});
			sgKcp.Cancel();
			return 0;
		}

		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (!kcp) return;
			ikcp_release(kcp);
			kcp = nullptr;
			udp->Remove(conv);						// remove self from container
			udp.reset();							// unbind
			if (flag) {
				peer->Dispose(flag);
			}
		}

		~UvKcpPeerBase() {
			Dispose(0);
		}

		inline virtual bool IsKcp() noexcept override {
			return true;
		}

		inline virtual std::string GetIP() noexcept override {
			std::string ip;
			Uv::FillIP(addr, ip);
			return ip;
		}

		virtual bool Disposed() const noexcept override {
			return !kcp;
		}

		// called by ext class
		inline virtual int Update(int64_t const& nowMS) noexcept override {
			if (!kcp) return -1;

			auto&& currentMS = uint32_t(nowMS - createMS);				// known issue: uint32 limit. connect only alive 50+ days
			if (uv.runMode == UV_RUN_DEFAULT && nextUpdateMS > currentMS) return 0;						// reduce cpu usage
			ikcp_update(kcp, currentMS);
			if (!kcp) return -1;
			if (uv.runMode == UV_RUN_DEFAULT) {
				nextUpdateMS = ikcp_check(kcp, currentMS);
			}

			do {
				int recvLen = ikcp_recv(kcp, uv.recvBuf, (int)uv.recvBufLen);
				if (recvLen <= 0) break;
				if (int r = Unpack((uint8_t*)uv.recvBuf, recvLen)) {
					Dispose(1);
					return -1;
				}
			} while (true);

			return 0;
		}


		// serial == 0: push    > 0: response    < 0: request
		template<typename Data>
		inline int SendPackageCore(Data const& data, int32_t const& serial = 0) noexcept {
			if (!kcp) return -1;
			auto& sendBB = uv.sendBB;
			static_assert(sizeof(uv_write_t_ex) + 4 <= 1024);
			sendBB.Reserve(1024);
			sendBB.len = 4;		// skip header space
			sendBB.Write(serial);
			if constexpr (std::is_same_v<BBuffer, Data>) {
				sendBB.AddRange(data.buf, data.len);
			}
			else {
				sendBB.WriteRoot(data);
			}
			auto buf = sendBB.buf;
			auto len = sendBB.len - 4;
			buf[0] = uint8_t(len);					// fill package len
			buf[1] = uint8_t(len >> 8);
			buf[2] = uint8_t(len >> 16);
			buf[3] = uint8_t(len >> 24);
			return Send(buf, sendBB.len);
		}

		inline virtual int SendPackage(Object_s const& data, int32_t const& serial = 0) noexcept override {
			return SendPackageCore(data, serial);
		}
		inline virtual int SendPackage(BBuffer const& data, int32_t const& serial = 0) noexcept override {
			return SendPackageCore(data, serial);
		}

		// send data immediately ( no wait for more data combine send )
		inline virtual void Flush() noexcept override {
			if (!kcp) return;
			ikcp_flush(kcp);
		}

		// called by udp class. put data to kcp when udp receive 
		inline int Input(uint8_t * const& recvBuf, uint32_t const& recvLen) noexcept {
			if (!kcp) return -1;
			return ikcp_input(kcp, (char*)recvBuf, recvLen);
		}

		// 4 bytes len header. can override for custom header format.
		inline virtual int Unpack(uint8_t * const& recvBuf, uint32_t const& recvLen) noexcept {
			buf.AddRange(recvBuf, recvLen);
			size_t offset = 0;
			while (offset + 4 <= buf.len) {							// ensure header len( 4 bytes )
				auto len = buf[offset + 0] + (buf[offset + 1] << 8) + (buf[offset + 2] << 16) + (buf[offset + 3] << 24);
				if (len <= 0 /* || len > maxLimit */) return -1;	// invalid length
				if (offset + 4 + len > buf.len) break;				// not enough data

				offset += 4;
				if (int r = peer->HandlePack(buf.buf + offset, len)) return r;
				offset += len;
			}
			buf.RemoveFront(offset);
			return 0;
		}

		// push send data to kcp. though ikcp_setoutput func send.
		inline int Send(uint8_t const* const& buf, ssize_t const& dataLen) noexcept {
			if (!kcp) return -1;
			return ikcp_send(kcp, (char*)buf, (int)dataLen);
		}
	};

	struct UvListenerKcp : UvKcp {
		using UvKcp::UvKcp;
		UvListenerBase* owner = nullptr;			// fill by owner
		Dict<uint32_t, std::weak_ptr<UvKcpPeerBase>> peers;
		Dict<std::string, std::pair<uint32_t, int64_t>> shakes;	// key: ip:port   value: conv, nowMS
		uint32_t convId = 0;
		int handShakeTimeoutMS = 3000;
		UvTimer_s timer;

		UvListenerKcp(Uv& uv, std::string const& ip, int const& port, bool const& isListener)
			: UvKcp(uv, ip, port, isListener) {
			MakeTo(timer, uv, 10, 10, [this] {
				this->Update(NowSteadyEpochMS());
				});
		}

		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (!this->uvUdp) return;
			this->UvKcp::Dispose(flag);
			for (auto&& kv : peers) {
				if (auto && peer = kv.value.lock()) {
					peer->Dispose(flag);
				}
			}
			peers.Clear();
			uv.udps.Remove(port);
		}

		~UvListenerKcp() {
			this->Dispose(0);
		}

		inline virtual void Update(int64_t const& nowMS) noexcept {
			for (auto&& kv : peers) {
				assert(kv.value.lock());
				kv.value.lock()->Update(nowMS);
			}
			for (auto&& iter = shakes.begin(); iter != shakes.end(); ++iter) {
				if (iter->value.second < nowMS) {
					iter.Remove();
				}
			}
		}

		inline virtual void Remove(uint32_t const& conv) noexcept override {
			peers.Remove(conv);
		}

	protected:
		inline virtual int Unpack(uint8_t* const& recvBuf, uint32_t const& recvLen, sockaddr const* const& addr) noexcept override {
			assert(port);
			// ensure hand shake, and udp peer owner still alive
			if (recvLen == 4 && owner) {						// hand shake contain 4 bytes auto inc serial
				auto&& ipAndPort = Uv::ToIpPortString(addr);
				// ip_port : <conv, createMS>
				auto&& idx = shakes.Find(ipAndPort);
				if (idx == -1) {
					idx = shakes.Add(ipAndPort, std::make_pair(++convId, NowSteadyEpochMS() + handShakeTimeoutMS)).index;
				}
				memcpy(recvBuf + 4, &shakes.ValueAt(idx).first, 4);	// return serial + conv( temp write to recvBuf is safe )
				return this->Send(recvBuf, 8, addr);
			}

			// header size limit IKCP_OVERHEAD ( kcp header ) check. ignore non kcp or hand shake pkgs.
			if (recvLen < 24) {
				return 0;
			}

			// read conv header
			uint32_t conv;
			memcpy(&conv, recvBuf, sizeof(conv));
			std::shared_ptr<UvKcpPeerBase> peer;

			// find at peers. if does not exists, find addr at shakes. if exists, create peer
			auto&& peerIter = peers.Find(conv);
			if (peerIter == -1) {								// conv not found: scan shakes
				if (!owner || owner->Disposed()) return 0;		// listener disposed: ignore
				auto && ipAndPort = Uv::ToIpPortString(addr);
				auto && idx = shakes.Find(ipAndPort);			// find by addr
				if (idx == -1 || shakes.ValueAt(idx).first != conv) return 0;	// not found or bad conv: ignore
				shakes.RemoveAt(idx);							// remove from shakes
				peer = xx::TryMake<UvKcpPeerBase>(uv);			// create kcp peer
				if (!peer) return 0;
				peer->udp = As<UvKcp>(shared_from_this());
				peer->conv = conv;
				peer->createMS = NowSteadyEpochMS();
				memcpy(&peer->addr, addr, sizeof(sockaddr_in6));// upgrade peer's tar addr( maybe accept callback need it )
				if (peer->InitKcp()) return 0;					// init failed: ignore
				peers[conv] = peer;								// store
				owner->Accept(peer);							// accept callback
			}
			else {
				peer = peers.ValueAt(peerIter).lock();
				if (!peer || peer->Disposed()) return 0;		// disposed: ignore
			}

			memcpy(&peer->addr, addr, sizeof(sockaddr_in6));	// upgrade peer's tar addr
			if (peer->Input(recvBuf, recvLen)) {
				peer->Dispose(1);								// peer will remove self from peers
			}
			return 0;
		}
	};

	struct UvDialerKcp : UvKcp {
		int i = 0;
		bool connected = false;
		std::weak_ptr<UvKcpPeerBase> peer_w;
		UvTimer_s timer;
		UvDialerBase* owner = nullptr;			// fill by owner

		UvDialerKcp(Uv& uv, std::string const& ip, int const& port, bool const& isListener)
			: UvKcp(uv, ip, port, isListener) {
			MakeTo(timer, uv, 10, 10, [this] {
				this->Update(NowSteadyEpochMS());
				});
		}
		~UvDialerKcp() {
			this->Dispose(0);
		}

		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (!this->uvUdp) return;
			this->UvKcp::Dispose(flag);
			if (auto && peer = peer_w.lock()) {
				peer->Dispose(flag);
			}
			uv.udps.Remove(port);
		}

		inline virtual void Update(int64_t const& nowMS) noexcept {
			if (connected) {
				if (auto && peer = peer_w.lock()) {
					peer->Update(nowMS);
				}
				else {
					Dispose();
				}
			}
			else {
				++i;
				if ((i & 0xFu) == 0) {		// 每 16 帧发送一次
					if (int r = Send((uint8_t*)& port, sizeof(port))) {
						Dispose();
					}
				}
			}
		}

		inline virtual void Remove(uint32_t const& conv) noexcept override {
			Dispose();
		}

	protected:
		virtual int Unpack(uint8_t* const& recvBuf, uint32_t const& recvLen, sockaddr const* const& addr) noexcept override {
			assert(owner || peer_w.lock());

			// ensure hand shake result data
			if (recvLen == 8 && owner) {						// 4 bytes serial + 4 bytes conv
				if (memcmp(recvBuf, &port, 4)) return 0;		// bad serial: ignore
				auto&& p = xx::TryMake<UvKcpPeerBase>(uv);
				if (!p) return -1;								// not enough memory
				peer_w = p;										// bind
				auto&& self = As<UvKcp>(shared_from_this());
				p->udp = self;
				memcpy(&p->conv, recvBuf + 4, 4);
				memcpy(&p->addr, addr, sizeof(sockaddr_in6));	// upgrade peer's tar addr
				p->createMS = NowSteadyEpochMS();
				if (p->InitKcp()) return 0;						// init kcp fail: ignore
				int r = p->Send((uint8_t*)"\x1\0\0\0\0", 5);	// for server accept
				assert(!r);
				p->Flush();
				connected = true;								// set flag
				owner->Accept(p);								// cleanup all reqs
				owner = nullptr;
				return 0;
			}

			if (recvLen < 24) {									// ignore non kcp data
				return 0;
			}

			auto&& peer = peer_w.lock();
			if (!peer) return 0;								// hand shake not finish: ignore

			uint32_t conv;
			memcpy(&conv, recvBuf, sizeof(conv));
			if (peer->conv != conv) return 0;					// bad conv: ignore

			memcpy(&peer->addr, addr, sizeof(sockaddr_in6));	// upgrade peer's tar addr
			if (peer->Input(recvBuf, recvLen)) {				// input data to kcp
				peer->Dispose(1);								// if fail: sucide
			}
			return 0;
		}
	};

	struct UvKcpListener : UvListenerBase {
		std::shared_ptr<UvListenerKcp> udp;

		UvKcpListener(Uv& uv, std::string const& ip, int const& port)
			: UvListenerBase(uv) {
			auto&& udps = uv.udps;
			auto&& idx = udps.Find(port);
			if (idx != -1) {
				udp = As<UvListenerKcp>(udps.ValueAt(idx).lock());
				if (udp->owner) throw - 1;			// same port listener already exists?
			}
			else {
				MakeTo(udp, uv, ip, port, true);
				udp->port = port;
				udp->owner = this;
				udps[port] = udp;
			}
			udp->owner = this;
		}
		~UvKcpListener() { this->Dispose(0); }

		virtual bool Disposed() const noexcept override {
			return !udp;
		}

		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (!udp) return;
			udp->owner = nullptr;								// unbind
			udp.reset();
			if (flag) {
				listener->Dispose(1);
			}
		}
	};

	struct UvTcpListener : UvListenerBase {
		uv_tcp_t* uvTcp = nullptr;
		sockaddr_in6 addr;

		UvTcpListener(Uv& uv, std::string const& ip, int const& port, int const& backlog = 128)
			: UvListenerBase(uv) {
			uvTcp = Uv::Alloc<uv_tcp_t>(this);
			if (!uvTcp) throw - 4;
			if (int r = uv_tcp_init(&uv.uvLoop, uvTcp)) {
				uvTcp = nullptr;
				throw r;
			}

			if (ip.find(':') == std::string::npos) {
				if (uv_ip4_addr(ip.c_str(), port, (sockaddr_in*)& addr)) throw - 1;
			}
			else {
				if (uv_ip6_addr(ip.c_str(), port, &addr)) throw - 2;
			}
			if (uv_tcp_bind(uvTcp, (sockaddr*)& addr, 0)) throw - 3;

			if (uv_listen((uv_stream_t*)uvTcp, backlog, [](uv_stream_t * server, int status) {
				if (status) return;
				auto&& self = Uv::GetSelf<UvTcpListener>(server);
				auto&& peer = xx::TryMake<UvTcpPeerBase>(self->uv);
				if (!peer) return;
				if (uv_accept(server, (uv_stream_t*)peer->uvTcp)) return;
				if (peer->ReadStart()) return;
				Uv::FillIP(peer->uvTcp, peer->ip);
				self->Accept(peer);
			})) throw - 4;
		};

		UvTcpListener(UvTcpListener const&) = delete;
		UvTcpListener& operator=(UvTcpListener const&) = delete;
		~UvTcpListener() { this->Dispose(0); }

		inline virtual bool Disposed() const noexcept override {
			return !uvTcp;
		}

		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (!uvTcp) return;
			Uv::HandleCloseAndFree(uvTcp);
			if (flag) {
				listener->Dispose(1);
			}
		}
	};

	inline UvListener::UvListener(Uv& uv, std::string const& ip, int const& port, int const& tcpKcpOpt)
		: UvCreateAcceptBase(uv) {
		if (tcpKcpOpt == 0 || tcpKcpOpt == 2) {
			xx::MakeTo(tcpListener, uv, ip, port);
			tcpListener->listener = this;
		}
		if (tcpKcpOpt == 1 || tcpKcpOpt == 2) {
			xx::MakeTo(kcpListener, uv, ip, port);
			kcpListener->listener = this;
		}
	}

	inline void UvListener::Dispose(int const& flag) noexcept {
		tcpListener.reset();
		kcpListener.reset();
		if (flag) {
			auto&& holder = shared_from_this();
			onCreatePeer = nullptr;
			onAccept = nullptr;
		}
	}

	struct UvDialer : UvCreateAcceptBase {
		std::shared_ptr<UvDialerBase> kcpDialer;
		std::shared_ptr<UvDialerBase> tcpDialer;
		UvTimer_s timeouter;
		bool disposed = false;

		UvDialer(Uv& uv);
		UvDialer(UvDialer const&) = delete;
		UvDialer& operator=(UvDialer const&) = delete;

		~UvDialer() { this->Dispose(0); }
		virtual bool Disposed() const noexcept override {
			return disposed;
		}
		virtual void Dispose(int const& flag = 1) noexcept override;
		virtual int Dial(std::string const& ip, int const& port, uint64_t const& timeoutMS = 2000) noexcept;
		virtual int Dial(std::vector<std::string> const& ips, int const& port, uint64_t const& timeoutMS = 2000) noexcept;
		virtual int Dial(std::vector<std::pair<std::string, int>> const& ipports, uint64_t const& timeoutMS = 2000) noexcept;
		virtual void Cancel() noexcept;

		inline int SetTimeout(uint64_t const& timeoutMS = 0) noexcept {
			if (disposed) return -1;
			timeouter.reset();
			if (!timeoutMS) return 0;
			xx::TryMakeTo(timeouter, uv, timeoutMS, 0, [self_w = AsWeak<UvDialer>(shared_from_this())]{
				if (auto && self = self_w.lock()) {
					self->Cancel();
					self->Accept(UvPeer_s());
				}
			});
			return timeouter ? 0 : -2;
		}
	};

	inline int UvDialerBase::Dial(std::vector<std::string> const& ips, int const& port, uint64_t const& timeoutMS) noexcept {
		if (Disposed()) return -1;
		Cancel();
		for (auto&& ip : ips) {
			if (int r = Dial(ip, port, 0, false)) return r;
		}
		return 0;
	}
	inline int UvDialerBase::Dial(std::vector<std::pair<std::string, int>> const& ipports, uint64_t const& timeoutMS) noexcept {
		if (Disposed()) return -1;
		Cancel();
		for (auto&& ipport : ipports) {
			if (int r = Dial(ipport.first, ipport.second, 0, false)) return r;
		}
		return 0;
	}
	inline void UvDialerBase::Accept(UvPeerBase_s pb) noexcept {
		assert(pb);
		auto&& p = dialer->CreatePeer();
		if (!p) return;
		p->peerBase = pb;
		pb->peer = &*p;
		dialer->Cancel();
		dialer->Accept(p);
	}

	struct UvKcpDialer : UvDialerBase {
		using UvDialerBase::UvDialerBase;
		Dict<int, std::shared_ptr<UvDialerKcp>> reqs;		// key: port

		inline virtual UvPeer_s CreatePeer() noexcept {
			return dialer->CreatePeer();
		}

		~UvKcpDialer() { this->Dispose(0); }
		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (disposed) return;
			disposed = true;
			Cancel();
			if (flag) {
				dialer->Dispose(1);
			}
		}

		inline virtual int Dial(std::string const& ip, int const& port, uint64_t const& timeoutMS = 0, bool cleanup = true) noexcept override {
			if (disposed) return -1;
			if (cleanup) {
				Cancel();
			}
			auto&& req = TryMake<UvDialerKcp>(uv, ip, port, false);
			if (!req) return -2;
			req->owner = this;
			req->port = --uv.autoId;
			uv.udps[req->port] = req;
			reqs[req->port] = std::move(req);
			return 0;
		}

		inline virtual void Cancel() noexcept override {
			if (disposed) return;
			reqs.Clear();
		}
	};

	struct UvTcpDialer;
	struct uv_connect_t_ex {
		uv_connect_t req;
		std::shared_ptr<UvTcpPeerBase> peer;
		std::weak_ptr<UvTcpDialer> dialer_w;
		~uv_connect_t_ex();
	};

	struct UvTcpDialer : UvDialerBase {
		using UvDialerBase::UvDialerBase;
		List<uv_connect_t_ex*> reqs;

		~UvTcpDialer() { this->Dispose(0); }
		inline virtual void Dispose(int const& flag = 1) noexcept override {
			if (disposed) return;
			disposed = true;
			Cancel();
			if (flag) {
				dialer->Dispose(1);
			}
		}

		inline virtual int Dial(std::string const& ip, int const& port, uint64_t const& timeoutMS = 0, bool cleanup = true) noexcept override {
			if (disposed) return -1;
			if (cleanup) {
				Cancel();
			}

			sockaddr_in6 addr;
			if (ip.find(':') == std::string::npos) {								// ipv4
				if (int r = uv_ip4_addr(ip.c_str(), port, (sockaddr_in*)& addr)) return r;
			}
			else {																	// ipv6
				if (int r = uv_ip6_addr(ip.c_str(), port, &addr)) return r;
			}

			auto&& req = new (std::nothrow) uv_connect_t_ex();
			if (!req) return -1;
			xx::ScopeGuard sgReq([&req] { delete req; });

			req->peer = xx::TryMake<UvTcpPeerBase>(uv);
			if (!req->peer) return -2;

			req->dialer_w = As<UvTcpDialer>(shared_from_this());

			if (uv_tcp_connect(&req->req, req->peer->uvTcp, (sockaddr*)& addr, [](uv_connect_t * conn, int status) {
				std::shared_ptr<UvTcpDialer> dialer;
				std::shared_ptr<UvTcpPeerBase> peer;
				{
					// auto delete when exit scope
					auto&& req = std::unique_ptr<uv_connect_t_ex>(container_of(conn, uv_connect_t_ex, req));
					if (status) return;													// error or -4081 canceled
					if (!req->peer) return;												// canceled
					dialer = req->dialer_w.lock();
					if (!dialer) return;												// container disposed
					peer = std::move(req->peer);										// remove peer to outside, avoid cancel
				}
				if (peer->ReadStart()) return;											// read error
				Uv::FillIP(peer->uvTcp, peer->ip);
				dialer->Accept(peer);													// callback
			})) return -3;

			reqs.Add(req);
			sgReq.Cancel();
			return 0;
		}

		inline virtual void Cancel() noexcept override {
			if (disposed) return;
			if (reqs.len) {
				for (auto i = reqs.len - 1; i != (size_t)-1; --i) {
					auto req = reqs[i];
					assert(req->peer);
					uv_cancel((uv_req_t*)& req->req);				// ios call this do nothing
					if (reqs[i] == req) {							// check req is alive
						req->peer.reset();							// ios need this to fire cancel progress
					}
				}
			}
			reqs.Clear();
		}
	};

	inline uv_connect_t_ex::~uv_connect_t_ex() {
		auto&& dialer = dialer_w.lock();
		if (!dialer) return;
		auto idx = dialer->reqs.Find(this);
		if (idx == -1) return;
		dialer->reqs.SwapRemoveAt(idx);
	}

	inline UvDialer::UvDialer(Uv& uv)
		: UvCreateAcceptBase(uv) {
		tcpDialer = xx::Make<UvTcpDialer>(uv);
		tcpDialer->dialer = this;
		kcpDialer = xx::Make<UvKcpDialer>(uv);
		kcpDialer->dialer = this;
	}
	inline int UvDialer::Dial(std::string const& ip, int const& port, uint64_t const& timeoutMS) noexcept {
		if (int r = SetTimeout(timeoutMS)) return r;
		if (int r = tcpDialer->Dial(ip, port, timeoutMS)) return r;
		return kcpDialer->Dial(ip, port, timeoutMS);
	}
	inline int UvDialer::Dial(std::vector<std::string> const& ips, int const& port, uint64_t const& timeoutMS) noexcept {
		if (int r = SetTimeout(timeoutMS)) return r;
		if (int r = tcpDialer->Dial(ips, port, timeoutMS)) return r;
		return kcpDialer->Dial(ips, port, timeoutMS);
	}
	inline int UvDialer::Dial(std::vector<std::pair<std::string, int>> const& ipports, uint64_t const& timeoutMS) noexcept {
		if (int r = SetTimeout(timeoutMS)) return r;
		if (int r = tcpDialer->Dial(ipports, timeoutMS)) return r;
		return kcpDialer->Dial(ipports, timeoutMS);
	}
	inline void UvDialer::Cancel() noexcept {
		timeouter.reset();
		tcpDialer->Cancel();
		kcpDialer->Cancel();
	}

	inline void UvDialer::Dispose(int const& flag) noexcept {
		if (disposed) return;
		disposed = true;
		Cancel();
		if (flag) {
			kcpDialer->Dispose(1);
			tcpDialer->Dispose(1);
		}
	}

	using UvDialer_s = std::shared_ptr<UvDialer>;
	using UvDialer_w = std::weak_ptr<UvDialer>;
}
