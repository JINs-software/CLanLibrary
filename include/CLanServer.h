#pragma once
#include "SocketUtil.h"

#include "CLanServerConfig.h"

#define JBUFF_DIRPTR_MANUAL_RESET
#include "JBuffer.h"

//#define MT_FILE_LOG
#if defined(MT_FILE_LOG)
#include "MTFileLogger.h"
#endif

#define SESSION_RELEASE_LOG

#include "TlsMemPool.h"

#define SESSION_SENDBUFF_SYNC_TEST
#if defined(SESSION_SENDBUFF_SYNC_TEST)
#include <mutex>
#endif

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
		stCLanSession() 
			: recvRingBuffer(SESSION_RECV_BUFFER_DEFAULT_SIZE), sendRingBuffer(SESSION_SEND_BUFFER_DEFAULT_SIZE)
		{
			Id.idx = 0;
			sessionRef.ioCnt = 0;
			sessionRef.releaseFlag = 0;
		}
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

		// ���� ���� �ܺ� ���� ���
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
			//sessionRef.ioCnt = 0;
			InterlockedIncrement((uint32*)&sessionRef);		// AcquireSession���� session IOCnt�� 1 ���� ��Ű�� �Ͱ� ����

			sessionRef.releaseFlag = 0;						// IOCnt�� ������Ű��, releaseFlag�� 0���� �ʱ�ȭ�ϴ� ������ �߿��ϴ�.
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

	// IOCP ���� ����ϰ�, ���ο� �۾��� ��������� ���
private:
#if defined(SESSION_RELEASE_LOG)
	struct stSessionRelaseLog {
		bool createFlag = false;
		bool releaseSuccess;
		bool disconnect = false;
		uint64 sessionID = 0;
		uint64 sessionIndex;
		uint64 sessionIncrement;
		int32 iocnt;
		int32 releaseFlag;
		string log = "";
	};
	std::vector<stSessionRelaseLog> m_ReleaseLog;
	USHORT m_ReleaseLogIndex;
	std::set<uint64> m_CreatedSession;
	std::mutex m_CreatedSessionMtx;
#endif

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
#if defined(SESSION_RELEASE_LOG)
	void Disconnect(uint64 sessionID, string log = "");
#else
	void Disconnect(uint64 sessionID);
#endif
	//bool SendPacket(uint64 sessionID, JBuffer& sendDataRef);
	bool SendPacket(uint64 sessionID, JBuffer* sendDataPtr);

private:
	stCLanSession* AcquireSession(uint64 sessionID);
	void ReturnSession(stCLanSession* session);

	void SendPost(uint64 sessionID);

	stCLanSession* CreateNewSession(SOCKET sock);
#if defined(SESSION_RELEASE_LOG)
	bool DeleteSession(uint64 sessionID, string log = "");
#else
	bool DeleteSession(uint64 sessionID);
#endif

	static UINT __stdcall AcceptThreadFunc(void* arg);
	static UINT __stdcall WorkerThreadFunc(void* arg);
		
public:
	virtual bool OnWorkerThreadCreate(HANDLE thHnd) { return true; };		// IOCP �۾��� �������� ���� ������ Start �Լ������� �̷������ ������,
																			// ��Ÿ�� �� �߰������� �����ǰ�, �Ҹ�� �� �ִ����� Start �Լ� flag���� �����ϵ���...
																			// (������ �ʿ� �������� ��)
	virtual void OnWorkerThreadCreateDone() {};
		
	virtual void OnWorkerThreadStart() {};									// TLS ���� �ʱ�ȭ �۾� ����

	virtual bool OnConnectionRequest(/*IP, Port*/);
	virtual void OnClientJoin(uint64 sessionID) = 0;
	virtual void OnDeleteSendPacket(uint64 sessionID, JBuffer& sendRingBuffer);
	virtual void OnClientLeave(uint64 sessionID) = 0;
	virtual void OnRecv(uint64 sessionID, JBuffer& recvBuff) = 0;
	//virtual void OnSend() = 0;
	//virtual void OnWorkerThreadBegin() = 0;
	//virtual void OnWorkerThreadEnd() = 0;
	virtual void OnError() = 0;

	
	/////////////////////////////////
	// ����͸� �׸�
	// - Accept TPS
	// - Recv Message TPS
	// - Send Message TPS
	/////////////////////////////////
	int getAcceptTPS();
	int getRecvMessageTPS();
	int getSendMessageTPS();

	void ConsoleLog();
	void MemAllocLog();
#if defined(SESSION_RELEASE_LOG)
	void SessionReleaseLog();
#endif
};

