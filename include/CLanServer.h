#pragma once
#include "SocketUtil.h"

#include "CLanServerConfig.h"

#define JBUFF_DIRPTR_MANUAL_RESET
#include "JBuffer.h"

//#define MT_FILE_LOG
#if defined(MT_FILE_LOG)
#include "MTFileLogger.h"
#endif

#include "TlsMemPool.h"

#define SESSION_SENDBUFF_SYNC_TEST
#if defined(SESSION_SENDBUFF_SYNC_TEST)
#include <mutex>
#endif

class CLanServer
{
	struct stSessionID {
		uint64 idx			: 16;
		uint64 incremental	: 48;
	};
	struct stSessionRef {
		int32 ioCnt			: 24;
		int32 releaseFlag	: 8;
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
		mutex	sendBuffMtx;
		SRWLOCK sendBuffSRWLock;
#endif

		stCLanSession() 
			: recvRingBuffer(SESSION_RECV_BUFFER_DEFAULT_SIZE), sendRingBuffer(SESSION_SEND_BUFFER_DEFAULT_SIZE)
		{}
		stCLanSession(SOCKET _sock, stSessionID _Id)
			: sock(_sock), Id(_Id), recvRingBuffer(SESSION_RECV_BUFFER_DEFAULT_SIZE), sendRingBuffer(SESSION_SEND_BUFFER_DEFAULT_SIZE), sendFlag(0)//, ioCnt(0)
		{
			sessionRef.ioCnt = 0;
			sessionRef.releaseFlag = 0;
			memcpy(&uiId, &Id, sizeof(Id));
			memset(&recvOverlapped, 0, sizeof(WSAOVERLAPPED));
			memset(&sendOverlapped, 0, sizeof(WSAOVERLAPPED));

#if defined(SESSION_SENDBUFF_SYNC_TEST)
			InitializeSRWLock(&sendBuffSRWLock);
#endif
		}

		// 내부 버퍼 외부 주입 방식
		//stCLanSession(BYTE* recvBuff, BYTE* sendBuff)
		//	: recvRingBuffer(SESSION_RECV_BUFFER_SIZE, recvBuff), sendRingBuffer(SESSION_SEND_BUFFER_SIZE, sendBuff)
		//{}

		void Init(SOCKET _sock, stSessionID _Id) {
			sock = _sock;
			Id = _Id;

			memcpy(&uiId, &Id, sizeof(Id));
			memset(&recvOverlapped, 0, sizeof(WSAOVERLAPPED));
			memset(&sendOverlapped, 0, sizeof(WSAOVERLAPPED));

			recvRingBuffer.ClearBuffer();
			sendRingBuffer.ClearBuffer();

			//ioCnt = 0;
			sessionRef.ioCnt = 0;
			sessionRef.releaseFlag = 0;
			sendFlag = false;

#if defined(SESSION_SENDBUFF_SYNC_TEST)
			InitializeSRWLock(&sendBuffSRWLock);
#endif
		}

		void clearRecvOverlapped() {
			memset(&recvOverlapped, 0, sizeof(WSAOVERLAPPED));
		}
		void clearSendOverlapped() {
			memset(&sendOverlapped, 0, sizeof(WSAOVERLAPPED));
		}
	};

	// IOCP 모델을 사용하고, 내부에 작업자 스레드들을 사용
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
	CRITICAL_SECTION m_SessionCS;
	
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

#if defined(ALLOC_BY_TLS_MEM_POOL)
protected:
	TlsMemPoolManager<JBuffer> m_SerialBuffPoolMgr;
	DWORD m_SerialBuffPoolIdx;
#endif

#if defined(MT_FILE_LOG)
	/////////////////////////////////
	// MTFileLogger
	/////////////////////////////////
protected:
	MTFileLogger mtFileLogger;
public:
	void PrintMTFileLog();
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
	bool Disconnect(uint64 sessionID);
	bool SendPacket(uint64 sessionID, JBuffer& sendDataRef);
	bool SendPacket(uint64 sessionID, JBuffer* sendDataPtr);

private:
	stCLanSession* AcquireSession(uint64 sessionID);
	void ReturnSession(stCLanSession* session);

	void SendPost(uint64 sessionID);

	stCLanSession* CreateNewSession(SOCKET sock);
	void DeleteSession(stCLanSession* delSession);
	void DeleteSession(uint64 sessionID);

	static UINT __stdcall AcceptThreadFunc(void* arg);
	static UINT __stdcall WorkerThreadFunc(void* arg);
		
public:
	virtual bool OnWorkerThreadCreate(HANDLE thHnd) { return true; };		// IOCP 작업자 스레드의 생성 갯수가 Start 함수에서만 이루어지는 것인지,
																			// 런타임 중 추가적으로 생성되고, 소멸될 수 있는지는 Start 함수 flag에서 선택하도록...
																			// (컨텐츠 쪽에 결정권을 줌)
	virtual void OnWorkerThreadCreateDone() {};
		
	virtual void OnWorkerThreadStart() {};									// TLS 관련 초기화 작업 가능

	virtual bool OnConnectionRequest(/*IP, Port*/) = 0;
	virtual void OnClientJoin(uint64 sessionID) = 0;
	virtual void OnClientLeave(uint64 sessionID) = 0;
	virtual void OnRecv(uint64 sessionID, JBuffer& recvBuff) = 0;
	//virtual void OnSend() = 0;
	//virtual void OnWorkerThreadBegin() = 0;
	//virtual void OnWorkerThreadEnd() = 0;
	virtual void OnError() = 0;

	
	/////////////////////////////////
	// 모니터링 항목
	// - Accept TPS
	// - Recv Message TPS
	// - Send Message TPS
	/////////////////////////////////
	int getAcceptTPS();
	int getRecvMessageTPS();
	int getSendMessageTPS();

	void ConsoleLog();
};

