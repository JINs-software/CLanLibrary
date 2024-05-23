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
		int32 ioCnt			: 24;		// �񵿱� I/O�� ��û�� ��Ȳ �� �ƴ϶� ������ �����ϴ� ��Ȳ������ �ش� ī��Ʈ�� ������Ų��.
		int32 releaseFlag	: 8;		// ���� ������ ���� ��� ���ؼ��̴�. 
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
		// => ���� �۽� ���۸� ��-���� ť�� ����, �۽� ���۸� ���� ����ȭ ��ü ����
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

			// IOCnt�� ������Ű��, releaseFlag�� 0���� �ʱ�ȭ�ϴ� ������ �߿��ϴ�.
			// (IOCnt�� 0���� 1�� �ʱ�ȭ�ϴ� ����� �ƴ� ������Ű�� �������)
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

	// �� Accept Ƚ��
	INT64 m_TotalAcceptCnt = 0;
	INT64 m_TotalDeleteCnt = 0;
	INT64 m_TotalLoginCnt = 0;
#endif

#if defined(SENDBUFF_MONT_LOG)
	// �ִ� �۽� ����
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
	// ȣ�� ����: CLanServer::Start���� �����带 ����(CREATE_SUSPENDED) ��
	// ��ȯ: True ��ȯ �� ������ �������� ���� ����
	//		 False ��ȯ �� ������ ������ ������ ����	
	// Desc: �����ڿ� ������ IOCP �۾��� ������ ���� ������ ������ �ش� �Լ��� �����ο��� IOCP �۾��� ������ ���� ����
	//		���� �۾��� �������� ���� �� �ʿ��� �ʱ�ȭ �۾� ����(���� �� OnWorkerThreadStart ȣ�� ��)
	virtual bool OnWorkerThreadCreate(HANDLE thHnd) { return true; };		

	/////////////////////////////////////////////////////////////////
	// OnWorkerThreadCreateDone
	/////////////////////////////////////////////////////////////////
	// ȣ�� ����: CLanServer::Start���� ��� IOCP �۾��� ������ ������ ��ģ ��
	virtual void OnWorkerThreadCreateDone() {};
		
	/////////////////////////////////////////////////////////////////
	// OnWorkerThreadStart
	/////////////////////////////////////////////////////////////////
	// ȣ�� ����: ���� IOCP �۾��� �����尡 �����ϴ� WorkerThreadFunc�� �Լ� ���Ժ�(GQCS�� ���Ե� �۾� �ݺ��� ����)
	// Desc: ���� �����尡 �ʱ⿡ �����ϴ� �۾� ����(ex, Thread Local Storage ���� �ʱ�ȭ)
	virtual void OnWorkerThreadStart() {};									
	/////////////////////////////////////////////////////////////////
	// OnWorkerThreadEnd
	/////////////////////////////////////////////////////////////////
	// ȣ�� ����: ���� IOCP �۾��� �����尡 ����(�۾��� �Լ� return) ��
	// Desc: ������ �� �ʿ��� ���� �۾� ����
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
	// ����͸� �׸�
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

