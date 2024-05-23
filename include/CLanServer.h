#pragma once
#include "SocketUtil.h"

#include "CLanServerConfig.h"

#define JBUFF_DIRPTR_MANUAL_RESET
#include "JBuffer.h"

#define SESSION_LOG

#include "TlsMemPool.h"

#include <assert.h>

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
		JBuffer sendRingBuffer;
		//uint32 ioCnt;
		stSessionRef sessionRef;
		uint32 sendFlag;
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		SRWLOCK sendBuffSRWLock;
		// => 추후 송신 버퍼를 락-프리 큐로 변경, 송신 버퍼를 위한 동기화 객체 생략
#endif

#if defined(TRACKING_CLIENT_PORT)
		USHORT	clientPort;
#endif

		stCLanSession() : recvRingBuffer(SESSION_RECV_BUFFER_DEFAULT_SIZE), sendRingBuffer(SESSION_SEND_BUFFER_DEFAULT_SIZE)
		{
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
			if (recvRingBuffer.GetUseSize() > 0) {
				DebugBreak();
			}
			recvRingBuffer.ClearBuffer();
			if (sendRingBuffer.GetUseSize() > 0) {
				DebugBreak();
			}
			sendRingBuffer.ClearBuffer();
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
	uint16 m_MaxOfSessions;
	queue<uint16> m_SessionAllocIdQueue;
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
	// Threads
	/////////////////////////////////
	//HANDLE m_ExitEvent;
	HANDLE m_AcceptThread;
	uint16 m_NumOfWorkerThreads;
	vector<HANDLE> m_WorkerThreads;
	map<DWORD, bool> m_WorkerThreadStartFlag;

	/////////////////////////////////
	// Exit
	/////////////////////////////////
	bool m_StopFlag;


	/////////////////////////////////
	// Memory Pool
	/////////////////////////////////
#if defined(ALLOC_BY_TLS_MEM_POOL)
protected:
	TlsMemPoolManager<JBuffer> m_SerialBuffPoolMgr;
	DWORD m_SerialBuffPoolIdx;
#endif

	/////////////////////////////////
	// Log
	/////////////////////////////////
#if defined(SESSION_LOG)
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
	CLanServer(const char* serverIP, uint16 serverPort,
		DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections, 
		bool tlsMemPoolReferenceFlag = false, bool tlsMemPoolPlacementNewFlag = false,
		size_t tlsMemPoolDefaultUnitCnt = TLS_MEM_POOL_DEFAULT_UNIT_CNT, size_t tlsMemPoolDefaultCapacity = TLS_MEM_POOL_DEFAULT_CAPACITY,
		uint32 sessionSendBuffSize = SESSION_SEND_BUFFER_DEFAULT_SIZE, uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
		bool beNagle = true
	);
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
	bool SendPacket(uint64 sessionID, JBuffer* sendDataPtr);
	
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
	virtual void OnDeleteSendPacket(UINT64 sessionID, JBuffer& sendRingBuffer);
	virtual void OnClientLeave(UINT64 sessionID) = 0;
	virtual void OnRecv(UINT64 sessionID, JBuffer& recvBuff) = 0;
	//virtual void OnSend() = 0;
	//virtual void OnWorkerThreadBegin() = 0;
	//virtual void OnWorkerThreadEnd() = 0;
	virtual void OnError() = 0;

	/////////////////////////////////////////////////////////////////
	// Encode, Decode
	/////////////////////////////////////////////////////////////////
	void Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads);
	bool Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, BYTE* payloads);

	
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
	void MemAllocLog();
#if defined(SESSION_LOG)
	void SessionReleaseLog();
#endif
};

