#pragma once
#include "SocketUtil.h"
#include <assert.h>

#define JBUFF_DIRPTR_MANUAL_RESET
#include "JBuffer.h"

#include "CLanServerConfig.h"
#if defined(ALLOC_BY_TLS_MEM_POOL)
#include "TlsMemPool.h"
#endif
#include "CommonProtocol.h"


class CLanServer
{
	struct stSessionID {
		uint64 idx			: 16;
		uint64 incremental	: 48;
	};
	struct stSessionRef {
		int32 ioCnt			: 24;		// 비동기 I/O가 요청된 상황 뿐 아니라 세션을 참조하는 상황에서도 해당 카운트를 증가시킨다.
		int32 releaseFlag	: 8;		// 세션 삭제에 대한 제어를 위해서이다. 
	};
	struct stCLanSession {
		SOCKET sock;
		stSessionID Id;
		uint64 uiId;
		WSAOVERLAPPED recvOverlapped;
		WSAOVERLAPPED sendOverlapped;
		JBuffer recvRingBuffer;
#if defined(ALLOC_BY_TLS_MEM_POOL)

#if defined(LOCKFREE_SEND_QUEUE)
		LockFreeQueue<JBuffer*>	sendBufferQueue;
		std::queue<JBuffer*>	sendPostedQueue;
#else
		JBuffer sendRingBuffer;
#endif

#else
		//std::queue<std::shared_ptr<JBuffer>> sendQueueBuffer;
		std::vector<std::shared_ptr<JBuffer>> sendBufferVector;
#endif
		
		stSessionRef sessionRef;
		uint32 sendFlag;
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		SRWLOCK sendBuffSRWLock;
		// => 추후 송신 버퍼를 락-프리 큐로 변경, 송신 버퍼를 위한 동기화 객체 생략
#endif

#if defined(TRACKING_CLIENT_PORT)
		USHORT	clientPort;
#endif

#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
		stCLanSession() : recvRingBuffer(SESSION_RECV_BUFFER_DEFAULT_SIZE)
#else
		stCLanSession() : recvRingBuffer(SESSION_RECV_BUFFER_DEFAULT_SIZE), sendRingBuffer(SESSION_SEND_BUFFER_DEFAULT_SIZE)
#endif
#else
		stCLanSession() : recvRingBuffer(SESSION_RECV_BUFFER_DEFAULT_SIZE)
#endif
		{
#if !defined(ALLOC_BY_TLS_MEM_POOL)
			// 동적 재할당으로 인한 결함 방지
			sendBufferVector.reserve(SESSION_SEND_BUFFER_DEFAULT_SIZE);
#endif
			Id.idx = 0;
			Id.incremental = 0;
			uiId = 0;
			sessionRef.ioCnt = 0;
			sessionRef.releaseFlag = 0;
		}

		void Init(SOCKET _sock, stSessionID _Id) {
			Id = _Id;
			memcpy(&uiId, &Id, sizeof(Id));			

			// IOCnt를 증가시키고, releaseFlag를 0으로 초기화하는 순서가 중요하다.
			// (IOCnt를 0으로 1로 초기화하는 방식이 아닌 증가시키는 방식으로)
			InterlockedIncrement((uint32*)&sessionRef);

			//sessionRef.releaseFlag = 0;						
			stSessionRef releaseFlagOffRef;
			releaseFlagOffRef.ioCnt = -1;
			releaseFlagOffRef.releaseFlag = 0;
			uint32 releaseFlagOff = *(uint32*)&releaseFlagOffRef;
			InterlockedAnd((long*)&sessionRef, releaseFlagOff);

			sock = _sock;
			memset(&recvOverlapped, 0, sizeof(WSAOVERLAPPED));
			memset(&sendOverlapped, 0, sizeof(WSAOVERLAPPED));
			//if (recvRingBuffer.GetUseSize() > 0) {
			//	DebugBreak();
			//}
			// => 채팅 서버 테스트 용, 채팅 서버에서는 더미 클라이언트의 송신->수신->종료 순
			recvRingBuffer.ClearBuffer();
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
			if (sendBufferQueue.GetSize() > 0) {
				DebugBreak();
			}
			if (!sendPostedQueue.empty()) {
				DebugBreak();
			}
#else
			if (sendRingBuffer.GetUseSize() > 0) {
				DebugBreak();
			}
			sendRingBuffer.ClearBuffer();
#endif
#else
			if (sendBufferVector.size() > 0) {
				DebugBreak();
			}
#endif
			sendFlag = false;
#if defined(SESSION_SENDBUFF_SYNC_TEST)
			InitializeSRWLock(&sendBuffSRWLock);
#endif

		}
		
		bool TryRelease() {
			bool ret = false;

			stSessionRef exgRef;
			exgRef.ioCnt = 0;
			exgRef.releaseFlag = 1;
			uint32 exg = *((uint32*)&exgRef);

			uint32 org = InterlockedCompareExchange((uint32*)&sessionRef, exg, 0);
			if (org == 0) {
				ret = true;
			}
			
			return ret;
		}

		void clearRecvOverlapped() {
			memset(&recvOverlapped, 0, sizeof(WSAOVERLAPPED));
		}
		void clearSendOverlapped() {
			memset(&sendOverlapped, 0, sizeof(WSAOVERLAPPED));
		}
	};

private:

	/////////////////////////////////
	// Networking
	/////////////////////////////////
	SOCKET m_ListenSock;			// Listen Sock
	SOCKADDR_IN m_ListenSockAddr;

	/////////////////////////////////
	// Session
	/////////////////////////////////
	UINT16 m_MaxOfSessions;
	queue<UINT16> m_SessionAllocIdQueue;
	CRITICAL_SECTION m_SessionAllocIdQueueCS;

	uint64 m_Incremental;
	vector<stCLanSession*> m_Sessions;
	
	uint32 m_SessionSendBufferSize;
	uint32 m_SessionRecvBufferSize;

	/////////////////////////////////
	// IOCP
	/////////////////////////////////
	HANDLE m_IOCP;

	/////////////////////////////////
	// Exit
	/////////////////////////////////
	bool m_StopFlag;

	/////////////////////////////////
	// Threads
	/////////////////////////////////
	// 1. Accept Thread(single)
	HANDLE				m_AcceptThread;
	// 2. IOCP Worker Thread(multi)
	UINT16				m_NumOfWorkerThreads;
	vector<HANDLE>		m_WorkerThreads;
	vector<DWORD>		m_WorkerThreadIDs;
	map<DWORD, bool>	m_WorkerThreadStartFlag;

#if defined(CALCULATE_TRANSACTION_PER_SECOND)
	HANDLE				m_CalcTpsThread;
protected:
	LONG				m_CalcTpsItems[NUM_OF_TPS_ITEM];
	LONG				m_TpsItems[NUM_OF_TPS_ITEM];
	LONG				m_TotalTransaction[NUM_OF_TPS_ITEM];
public:
	inline void IncrementRecvTransaction(LONG cnt = 1) {
		InterlockedAdd(&m_CalcTpsItems[RECV_TRANSACTION], cnt);
		InterlockedAdd(&m_TotalTransaction[RECV_TRANSACTION], cnt);
	}
	inline void IncrementRecvTransactionNoGuard(LONG cnt = 1) {
		m_CalcTpsItems[RECV_TRANSACTION] += cnt;
		m_TotalTransaction[RECV_TRANSACTION] += cnt;
	}
	inline void IncrementSendTransaction(LONG cnt = 1) {
		InterlockedAdd(&m_CalcTpsItems[SEND_TRANSACTION], cnt);
		InterlockedAdd(&m_TotalTransaction[SEND_TRANSACTION], cnt);
	}
	inline void IncrementSendTransactionNoGuard(LONG cnt = 1) {
		m_CalcTpsItems[SEND_TRANSACTION] += cnt;
		m_TotalTransaction[SEND_TRANSACTION] += cnt;
	}
#endif

#if defined(ALLOC_BY_TLS_MEM_POOL)
	/////////////////////////////////
	// Memory Pool
	/////////////////////////////////
public:
	TlsMemPoolManager<JBuffer> m_SerialBuffPoolMgr;
	DWORD m_SerialBuffPoolIdx;
	inline JBuffer* AllocSerialBuff() {
		JBuffer* msg = m_SerialBuffPoolMgr.GetTlsMemPool().AllocMem();
		msg->ClearBuffer();
		return msg;
	}
	inline JBuffer* AllocSerialBuff(const std::string& log) {
		JBuffer* msg = m_SerialBuffPoolMgr.GetTlsMemPool().AllocMem(1, log);
		msg->ClearBuffer();
		return msg;
	}
	inline JBuffer* AllocSerialSendBuff(USHORT length) {
		JBuffer* msg = m_SerialBuffPoolMgr.GetTlsMemPool().AllocMem();
		msg->ClearBuffer();

		stMSG_HDR* hdr = msg->DirectReserve<stMSG_HDR>();
		hdr->code = dfPACKET_CODE;
		hdr->len = length;
		hdr->randKey = (BYTE)(-1);	// Encode 전 송신 직렬화 버퍼 식별

		return msg;
	}
	inline JBuffer* AllocSerialSendBuff(USHORT length, const std::string& log) {
		JBuffer* msg = m_SerialBuffPoolMgr.GetTlsMemPool().AllocMem(1, log);
		msg->ClearBuffer();

		stMSG_HDR* hdr = msg->DirectReserve<stMSG_HDR>();
		hdr->code = dfPACKET_CODE;
		hdr->len = length;
		hdr->randKey = (BYTE)(-1);

		return msg;
	}
	inline void FreeSerialBuff(JBuffer* buff) {
		m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(buff);
	}
	inline void FreeSerialBuff(JBuffer* buff, const std::string& log) {
		m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(buff, log);
	}
	inline void AddRefSerialBuff(JBuffer* buff) {
		m_SerialBuffPoolMgr.GetTlsMemPool().IncrementRefCnt(buff, 1);
	}
	inline void AddRefSerialBuff(JBuffer* buff, const std::string& log) {
		m_SerialBuffPoolMgr.GetTlsMemPool().IncrementRefCnt(buff, 1, log);
	}

	inline size_t GetAllocMemPoolUsageSize() {
		return (m_SerialBuffPoolMgr.GetTotalAllocMemCnt() - m_SerialBuffPoolMgr.GetTotalFreeMemCnt()) * sizeof(stMemPoolNode<JBuffer>);
	}
#endif

	/////////////////////////////////
	// Log
	/////////////////////////////////
#if defined(SESSION_LOG)
protected:
	enum enSessionWorkType {
		SESSION_CREATE = 1,
		SESSION_RELEASE,
		SESSION_DISCONNECT,
		SESSION_ACCEPT_WSARECV,
		SESSION_RETURN_GQCS_Disconn,
		SESSION_RETURN_GQCS,
		SESSION_COMPLETE_RECV,
		SESSION_COMPLETE_SEND,
		SESSION_WSARECV,
		SESSION_WSASEND,
		SESSION_ACQUIRE,
		SESSION_RETURN,
		SESSION_AFTER_SENDPOST
	};
	struct stSessionLog {
		enSessionWorkType sessionWork;
		bool workDone;
		bool ioPending;

		uint64 sessionID = 0;
		uint64 sessionIndex;

		int32 iocnt;
		int32 releaseFlag;

		string log = "";

		void Init() {
			memset(this, 0, sizeof(stSessionLog) - sizeof(string));
			log = "";
		}
	};
	std::vector<stSessionLog> m_SessionLog;
	USHORT m_SessionLogIndex;
	std::set<uint64> m_CreatedSession;
	std::mutex m_CreatedSessionMtx;

	stSessionLog& GetSessionLog() {
		USHORT releaseLogIdx = InterlockedIncrement16((short*)&m_SessionLogIndex);
		m_SessionLog[releaseLogIdx].Init();
		return m_SessionLog[releaseLogIdx];
	}

	// 총 Accept 횟수
	INT64 m_TotalAcceptCnt = 0;
	INT64 m_TotalDeleteCnt = 0;
	INT64 m_TotalLoginCnt = 0;
#endif

#if defined(SENDBUFF_MONT_LOG)
	// 최대 송신 버퍼
	size_t m_SendBuffOfMaxSize;
	UINT64 m_SessionOfMaxSendBuff;
#endif

public:
#if defined(ALLOC_BY_TLS_MEM_POOL)
	CLanServer(const char* serverIP, uint16 serverPort,
		DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections, 
		bool tlsMemPoolReferenceFlag = false, bool tlsMemPoolPlacementNewFlag = false,
		size_t tlsMemPoolDefaultUnitCnt = TLS_MEM_POOL_DEFAULT_UNIT_CNT, size_t tlsMemPoolDefaultCapacity = TLS_MEM_POOL_DEFAULT_CAPACITY,
#if defined(LOCKFREE_SEND_QUEUE)
		uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
#else
		uint32 sessionSendBuffSize = SESSION_SEND_BUFFER_DEFAULT_SIZE, uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
#endif
		bool beNagle = true
	);
#else
	CLanServer(const char* serverIP, UINT16 serverPort,
		DWORD numOfIocpConcurrentThrd, UINT16 numOfWorkerThreads, UINT16 maxOfConnections,
#if defined(LOCKFREE_SEND_QUEUE)
		uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
#else
		uint32 sessionSendBuffSize = SESSION_SEND_BUFFER_DEFAULT_SIZE, uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
#endif
		bool beNagle = true
	);
#endif
	~CLanServer();
	bool Start();
	void Stop();
	inline int GetSessionCount() {
		return m_MaxOfSessions - m_SessionAllocIdQueue.size();
	}

#if defined(SESSION_LOG)
	void Disconnect(uint64 sessionID, string log = "");
#else
	void Disconnect(uint64 sessionID);
#endif

#if defined(ALLOC_BY_TLS_MEM_POOL)
	bool SendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded = false);
#else
	bool SendPacket(uint64 sessionID, std::shared_ptr<JBuffer> sendDataPtr);
#endif
	
private:
	stCLanSession* AcquireSession(uint64 sessionID);
	void ReturnSession(stCLanSession* session);

	void SendPost(uint64 sessionID);

	stCLanSession* CreateNewSession(SOCKET sock);
#if defined(SESSION_LOG)
	bool DeleteSession(uint64 sessionID, string log = "");
#else
	bool DeleteSession(uint64 sessionID);
#endif

	static UINT __stdcall AcceptThreadFunc(void* arg);
	static UINT __stdcall WorkerThreadFunc(void* arg);
#if defined(CALCULATE_TRANSACTION_PER_SECOND)
	static UINT	__stdcall CalcTpsThreadFunc(void* arg);
#endif
		
protected:
	/////////////////////////////////////////////////////////////////
	// OnWorkerThreadCreate
	/////////////////////////////////////////////////////////////////
	// 호출 시점: CLanServer::Start에서 스레드를 생성(CREATE_SUSPENDED) 후
	// 반환: True 반환 시 생성된 스레드의 수행 시작
	//		 False 반환 시 생성된 생성된 스레드 종료	
	// Desc: 생성자에 전달한 IOCP 작업자 스레드 생성 갯수와 별개로 해당 함수의 구현부에서 IOCP 작업자 스레드 갯수 제어
	//		또한 작업자 스레드의 수행 전 필요한 초기화 작업 수행(시점 상 OnWorkerThreadStart 호출 전)
	virtual bool OnWorkerThreadCreate(HANDLE thHnd) { return true; };		

	/////////////////////////////////////////////////////////////////
	// OnWorkerThreadCreateDone
	/////////////////////////////////////////////////////////////////
	// 호출 시점: CLanServer::Start에서 모든 IOCP 작업자 스레드 생성을 마친 후
	virtual void OnWorkerThreadCreateDone() {};
		
	/////////////////////////////////////////////////////////////////
	// OnWorkerThreadStart
	/////////////////////////////////////////////////////////////////
	// 호출 시점: 개별 IOCP 작업자 스레드가 수행하는 WorkerThreadFunc의 함수 초입부(GQCS가 포함된 작업 반복문 이전)
	// Desc: 개별 스레드가 초기에 설정하는 작업 수행(ex, Thread Local Storage 관련 초기화)
	virtual void OnWorkerThreadStart() {};									
	/////////////////////////////////////////////////////////////////
	// OnWorkerThreadEnd
	/////////////////////////////////////////////////////////////////
	// 호출 시점: 개별 IOCP 작업자 스레드가 종료(작업자 함수 return) 전
	// Desc: 스레드 별 필요한 정리 작업 수행
	virtual void OnWorkerThreadEnd() {};

	virtual bool OnConnectionRequest(/*IP, Port*/);
	virtual void OnClientJoin(UINT64 sessionID) = 0;
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
	virtual void OnDeleteSendPacket(UINT64 sessionID, LockFreeQueue<JBuffer*>& sendBufferQueue, std::queue<JBuffer*>& sendPostedQueue);
#else
	virtual void OnDeleteSendPacket(UINT64 sessionID, JBuffer& sendRingBuffer);
#endif
#else
	virtual void OnDeleteSendPacket(UINT64 sessionID, std::vector<std::shared_ptr<JBuffer>>& sendBufferVec);
#endif
	virtual void OnClientLeave(UINT64 sessionID) = 0;
#if defined(ON_RECV_BUFFERING)
	virtual void OnRecv(UINT64 sessionID, std::queue<JBuffer>& bufferedQueue, size_t recvDataLen) = 0;
#else
	virtual void OnRecv(UINT64 sessionID, JBuffer& recvBuff) = 0;
#endif
	//virtual void OnSend() = 0;
	//virtual void OnWorkerThreadBegin() = 0;
	//virtual void OnWorkerThreadEnd() = 0;
	virtual void OnError() {};


private:
	/////////////////////////////////////////////////////////////////
	// Process Recv/Send message, Encode, Decode
	/////////////////////////////////////////////////////////////////
	bool ProcessReceiveMessage(UINT64 sessionID, JBuffer& recvRingBuffer);
public:
	void Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads);
	bool Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, BYTE* payloads);
	bool Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, JBuffer& ringPayloads);
	inline BYTE GetRandomKey() {
		return rand() % UINT8_MAX;	// 0b0000'0000 ~ 0b0111'1110 (0b1111'1111은 디코딩이 이루어지지 않은 페이로드 식별 값)
	}
	
	/////////////////////////////////
	// 모니터링 항목
	// - Accept TPS
	// - Recv Message TPS
	// - Send Message TPS
	/////////////////////////////////
	int getAcceptTPS();
	int getRecvMessageTPS();
	int getSendMessageTPS();

public:
	virtual void ServerConsoleLog() {}
	void ConsoleLog();
#if defined(ALLOC_MEM_LOG)
	void MemAllocLog();
#endif
#if defined(SESSION_LOG)
	void SessionReleaseLog();
#endif
};

