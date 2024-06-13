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
	////////////////////////////////////////////////////////////////////////
	// CLanSession
	////////////////////////////////////////////////////////////////////////
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
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
		LockFreeQueue<JBuffer*>	sendBufferQueue;
		//std::queue<JBuffer*>	sendPostedQueue;
		JBuffer* sendPostedQueue[WSABUF_ARRAY_DEFAULT_SIZE];
#else
		JBuffer sendRingBuffer;
		SRWLOCK sendBuffSRWLock;
#endif
#else
		std::vector<std::shared_ptr<JBuffer>> sendBufferVector;
#endif
		stSessionRef sessionRef;
		uint32 sendFlag;


#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
		stCLanSession(uint32 recvBuffSize) : recvRingBuffer(recvBuffSize)
#else
		stCLanSession(uint32 recvBuffSize, uint32 sendBuffSize) : recvRingBuffer(recvBuffSize), sendRingBuffer(sendBuffSize)
#endif
#else
		stCLanSession() : recvRingBuffer(SESSION_RECV_BUFFER_DEFAULT_SIZE)
#endif
		{
#if !defined(ALLOC_BY_TLS_MEM_POOL)
			// ���� ���Ҵ����� ���� ���� ����
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
			// IOCnt�� ������Ű��, releaseFlag�� 0���� �ʱ�ȭ�ϴ� ������ �߿��ϴ�.
			// (IOCnt�� 0���� 1�� �ʱ�ȭ�ϴ� ����� �ƴ� ������Ű�� �������)
			InterlockedIncrement((uint32*)&sessionRef);
			
			stSessionRef releaseFlagOffRef;
			releaseFlagOffRef.ioCnt = -1;
			releaseFlagOffRef.releaseFlag = 0;
			uint32 releaseFlagOff = *(uint32*)&releaseFlagOffRef;
			InterlockedAnd((long*)&sessionRef, releaseFlagOff);

			sock = _sock;
			memset(&recvOverlapped, 0, sizeof(WSAOVERLAPPED));
			memset(&sendOverlapped, 0, sizeof(WSAOVERLAPPED));
			recvRingBuffer.ClearBuffer();
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
			if (sendBufferQueue.GetSize() > 0) {
				DebugBreak();
			}
			memset(sendPostedQueue, NULL, sizeof(sendPostedQueue));
#else
			if (sendRingBuffer.GetUseSize() > 0) {
				DebugBreak();
			}
			sendRingBuffer.ClearBuffer();
			InitializeSRWLock(&sendBuffSRWLock);
#endif
#else
			if (sendBufferVector.size() > 0) {
				DebugBreak();
			}
#endif
			sendFlag = false;
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


	////////////////////////////////////////////////////////////////////////
	// CLanServer
	////////////////////////////////////////////////////////////////////////
private:

	/////////////////////////////////
	// Networking
	/////////////////////////////////
	SOCKET			m_ListenSock;			// Listen Sock
	SOCKADDR_IN		m_ListenSockAddr;
	UINT64			m_TotalAccept = 0;
	USHORT			m_AcceptTransaction = 0;

	bool m_Nagle;
	bool m_ZeroCopySend;

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

#if defined(ALLOC_BY_TLS_MEM_POOL)
	/////////////////////////////////
	// Memory Pool
	/////////////////////////////////
public:
	TlsMemPoolManager<JBuffer> m_SerialBuffPoolMgr;
	size_t			m_TlsMemPoolDefaultUnitCnt;
	size_t			m_TlsMemPoolDefaultUnitCapacity;
	UINT			m_SerialBufferSize;

	DWORD m_SerialBuffPoolIdx;
	inline JBuffer* AllocSerialBuff() {
		JBuffer* msg = m_SerialBuffPoolMgr.GetTlsMemPool().AllocMem(1, m_SerialBufferSize);
		msg->ClearBuffer();
		return msg;
	}
	inline JBuffer* AllocSerialSendBuff(USHORT length) {
		JBuffer* msg = m_SerialBuffPoolMgr.GetTlsMemPool().AllocMem(1, m_SerialBufferSize);
		msg->ClearBuffer();

		stMSG_HDR* hdr = msg->DirectReserve<stMSG_HDR>();
		hdr->code = dfPACKET_CODE;
		hdr->len = length;
		hdr->randKey = (BYTE)(-1);	// Encode �� �۽� ����ȭ ���� �ĺ�

		return msg;
	}
	inline void FreeSerialBuff(JBuffer* buff) {
		m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(buff);
	}
	inline void AddRefSerialBuff(JBuffer* buff) {
		m_SerialBuffPoolMgr.GetTlsMemPool().IncrementRefCnt(buff, 1);
	}
	inline size_t GetAllocMemPoolUsageUnitCnt() {
		//return (m_SerialBuffPoolMgr.GetTotalAllocMemCnt() - m_SerialBuffPoolMgr.GetTotalFreeMemCnt()) * sizeof(stMemPoolNode<JBuffer>);
		return m_SerialBuffPoolMgr.GetAllocatedMemUnitCnt();
	}
	inline size_t GetAllocMemPoolUsageSize() {
		//return (m_SerialBuffPoolMgr.GetTotalAllocMemCnt() - m_SerialBuffPoolMgr.GetTotalFreeMemCnt()) * sizeof(stMemPoolNode<JBuffer>);
		return m_SerialBuffPoolMgr.GetAllocatedMemUnitCnt() * (sizeof(stMemPoolNode<JBuffer>) + m_SerialBufferSize);
	}
	inline DWORD AllocTlsMemPool() {
		return m_SerialBuffPoolMgr.AllocTlsMemPool(m_TlsMemPoolDefaultUnitCnt, m_TlsMemPoolDefaultUnitCapacity, m_SerialBufferSize);
	}

#endif

public:
#if defined(ALLOC_BY_TLS_MEM_POOL)
	CLanServer(const char* serverIP, uint16 serverPort,
		DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections, 
		size_t tlsMemPoolDefaultUnitCnt = 0, size_t tlsMemPoolDefaultUnitCapacity = 0,
		bool tlsMemPoolReferenceFlag = false, bool tlsMemPoolPlacementNewFlag = false,
		UINT serialBufferSize = DEFAULT_SERIAL_BUFFER_SIZE,
#if defined(LOCKFREE_SEND_QUEUE)
		uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
#else
		uint32 sessionSendBuffSize = SESSION_SEND_BUFFER_DEFAULT_SIZE, uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
#endif
		bool beNagle = true, bool zeroCopySend = false
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

	/////////////////////////////////////////////////////////////////
	// SendPacket
	// - encoded: false ��, ����ȭ ���� ���ڵ� ����(������ ���� ����� ���� �޽������� �ִ� ����)
	// - postToWorker: true ��, SendPost�� �ƴ� SendPostRequestuest �Լ� ȣ��
	/////////////////////////////////////////////////////////////////
#if defined(ALLOC_BY_TLS_MEM_POOL)
	bool SendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded = false, bool postToWorker = false);
#else
	bool SendPacket(uint64 sessionID, std::shared_ptr<JBuffer> sendDataPtr, bool reqToWorkerTh = false);
#endif
	////////////////////////////////////////////////////////////////////////////////////////////////////////
	// BufferSendPacket: �۽� ���� ť(�Ǵ� ������) ���Ը� ����, ���� SendBufferedPacket�� ���ٸ� �۽� ���� x
	////////////////////////////////////////////////////////////////////////////////////////////////////////
	bool BufferSendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded = false);
	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// SendBufferedPacket: �۽� ���� ť(�Ǵ� ������)�� �۽� ��� ��Ŷ�� �����Ѵٸ� SendPost �Ǵ� SendPostRequset ȣ��
	//					   AcquireSession�� ���� ���� ��� �ʿ�
	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	void SendBufferedPacket(uint64 sessionID, bool postToWorker = false);

private:
	stCLanSession* AcquireSession(uint64 sessionID);
	void ReturnSession(stCLanSession* session);

	void SendPost(uint64 sessionID, bool onSendFlag = false);
	void SendPostRequest(uint64 sessionID);

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
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
	//virtual void OnDeleteSendPacket(UINT64 sessionID, LockFreeQueue<JBuffer*>& sendBufferQueue, std::queue<JBuffer*>& sendPostedQueue);
	virtual void OnDeleteSendPacket(UINT64 sessionID, LockFreeQueue<JBuffer*>& sendBufferQueue, JBuffer** sendPostedQueue);
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
		return rand() % UINT8_MAX;	// 0b0000'0000 ~ 0b0111'1110 (0b1111'1111�� ���ڵ��� �̷������ ���� ���̷ε� �ĺ� ��)
	}
	
	/////////////////////////////////
	// ����͸� �׸�
	// - Accept TPS
	// - Recv Message TPS
	// - Send Message TPS
	/////////////////////////////////
	inline USHORT GetAndResetAcceptTransaction() {
		static clock_t m_LastClock = clock();

		USHORT ret = m_AcceptTransaction;
		clock_t now = clock();
		clock_t interval = now - m_LastClock;
		if (interval >= 1000) {
			ret = m_AcceptTransaction / (interval / 1000);
			m_AcceptTransaction = 0;
			m_LastClock = now;
		}
		return ret;
	}
	USHORT getRecvMessageTPS();
	USHORT getSendMessageTPS();

public:
	void ConsoleLog();
	virtual void ServerConsoleLog() {}
	
#if defined(CALCULATE_TRANSACTION_PER_SECOND)
private:
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

#if defined(ALLOC_MEM_LOG)
public:
	inline JBuffer* AllocSerialBuff(const std::string& log) {
		JBuffer* msg = m_SerialBuffPoolMgr.GetTlsMemPool().AllocMem(1, log);
		msg->ClearBuffer();
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
	inline void FreeSerialBuff(JBuffer* buff, const std::string& log) {
		m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(buff, log);
	}

	inline void AddRefSerialBuff(JBuffer* buff, const std::string& log) {
		m_SerialBuffPoolMgr.GetTlsMemPool().IncrementRefCnt(buff, 1, log);
	}
#endif

/////////////////////////////////
// Log
/////////////////////////////////
#if defined(ALLOC_MEM_LOG)
public:
	void MemAllocLog();
#endif
#if defined(SESSION_LOG)
public:
	void SessionReleaseLog();
#endif

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
};

