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
	/*****************************************************************************************************************************
	* CLanSession
	*****************************************************************************************************************************/
	struct stSessionID {
		uint64 idx			: 16;		// 0 ~ 65535 ���� �ε���
		uint64 incremental	: 48;		// ���� ID ���� part, ���� �Ҵ� �� ���� ���е�
										// => �ε���, ���� �� ���� 64��Ʈ ���� ID
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
		LockFreeQueue<JBuffer*>	sendBufferQueue;				// �۽� ��Ŷ�� ��� ���� �۽� (��-����)ť
		JBuffer* sendPostedQueue[WSABUF_ARRAY_DEFAULT_SIZE];	// WSASend ȣ�� �� wsabuf�� ���޵� ����ȭ �۽� ���۸� ��� ����
																// �۽� �Ϸ� ���� �� ����ȭ ���� �޸� Ǯ�� ��ȯ
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

			clearRecvOverlapped();
			clearSendOverlapped();
			recvRingBuffer.ClearBuffer();
			memset(sendPostedQueue, NULL, sizeof(sendPostedQueue));
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

			// releaseFlagOffRef == 0xFFFF'FFFF'FFFF'0000
			// ���������� releaseFlag�� �ʱ�ȭ�Ѵ�.
			InterlockedAnd((long*)&sessionRef, releaseFlagOff);

			sock = _sock;
			clearRecvOverlapped();
			clearSendOverlapped();
			recvRingBuffer.ClearBuffer();
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
			if (sendBufferQueue.GetSize() > 0) {
#if defined(CLANSERVER_ASSERT)
				DebugBreak();
#endif
				while (sendBufferQueue.GetSize() > 0) {
					JBuffer* resBuff;
					sendBufferQueue.Dequeue(resBuff);

					// sendBufferQueue.GetSize() > 0�� ���������� ��Ȳ
					// �ӽ� �������� Dequeue�� ���� �۽� ť�� ���. ���� �۽� ������ ���� �� �޸� ���� �߻�(�޸� Ǯ ��ȯx)
				}
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

			// ������ �� �� ����
			// ioCnt == 0, releaseFlag == 0 �� ���¸� Ȯ���ϰ�, releaseFlag�� On �Ѵ�.
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


	/*****************************************************************************************************************************
	* CLanServer
	*****************************************************************************************************************************/
private:

	/////////////////////////////////
	// Networking
	/////////////////////////////////
	SOCKET			m_ListenSock;			// Listen Sock
	SOCKADDR_IN		m_ListenSockAddr;
	UINT64			m_TotalAccept;
	USHORT			m_AcceptTransaction;
	UINT			m_MaxOfRecvBufferSize;
	LONG			m_MaxOfBufferedSerialSendBufferCnt;

	/////////////////////////////////
	// [TO DO]
	bool m_Nagle;				// ���̱� �۵� ���� �̹ݿ�
	bool m_ZeroCopySend;		// ����-ī�� �̹ݿ�
	/////////////////////////////////

	/////////////////////////////////
	// Session
	/////////////////////////////////
	UINT16					m_MaxOfSessions;
	queue<UINT16>			m_SessionAllocIdQueue;		// 1 ~ m_MaxOfSessions ���� �Ҵ� �ε��� ť
														// (�ε��� 0�� ����)
	CRITICAL_SECTION		m_SessionAllocIdQueueCS;	// AcceptThread'Pop(���� �ε��� �Ҵ�), WorkerThreads'Push(���� ���� �� �ε��� ��ȯ)
														// => ����ȭ ��ü �ʿ�

	uint64					m_Incremental;				// ���� ID ���� ��Ʈ �Ҵ� ��, ���� ���� �ø��� ���� ���� (���� Accept �����尡 ����, ������ ���� ���ʿ�)

	vector<stCLanSession*>	m_Sessions;					// ���� �ε��� == ���� ID �ε����� ���� ����ü ���� ����
#if !defined(LOCKFREE_SEND_QUEUE)
	uint32 m_SessionSendBufferSize;
#endif
	uint32 m_SessionRecvBufferSize;

	/////////////////////////////////
	// IOCP
	/////////////////////////////////
	HANDLE m_IOCP;

	/////////////////////////////////
	// Exit
	/////////////////////////////////
	bool m_StopFlag;	// Stop(���� �� ���� �Լ�) ��ȣ�� ��,
						// �Ҹ��ڿ��� ȣ��ǵ��� �ϱ� ���� ���� ����

	/////////////////////////////////
	// Threads
	/////////////////////////////////
	// 1. Accept Thread(single)
	HANDLE				m_AcceptThread;

	// 2. IOCP Worker Thread(multi)
	UINT16				m_NumOfWorkerThreads;		// �����ڸ� ���� ����, 0 ���� �� GetSystemInfo ȣ���� ���� ���� CPU ������ ����
	vector<HANDLE>		m_WorkerThreads;
	vector<DWORD>		m_WorkerThreadIDs;
	map<DWORD, bool>	m_WorkerThreadStartFlag;	// �۾��� ������ ���� �� OnWorkerThreadCreate ȣ��
													// TRUE ��ȯ �� { �۾��� ������ ID, TRUE } insert
													// FALSE ��ȯ ��  { �۾��� ������ ID, FALSE } insert
													// �۾��� ������ �Լ� ���Ժο��� TRUE/FALSE ���� Ȯ��, FALSE �� �۾� ���� ���� RETURN

#if defined(ALLOC_BY_TLS_MEM_POOL)
	/////////////////////////////////
	// Memory Pool
	/////////////////////////////////
public:
	TlsMemPoolManager<JBuffer>	m_SerialBuffPoolMgr;				// CLanServer ����ȭ ���� �޸� Ǯ ������

	size_t						m_TlsMemPoolDefaultUnitCnt;			// tlsMemPool �⺻ �Ҵ� �޸� ��ü ����
	size_t						m_TlsMemPoolDefaultUnitCapacity;	// tlsMemPool �ִ� �޸� ��ü ���� ����
	UINT						m_SerialBufferSize;					// m_SerialBuffPoolMgr�� ���� ������ tlsMemPool�� ���� �Ҵ�޴� ����ȭ ���� ũ��
																	// (�������� �Ҵ�޾� ���Ǵ� ����ȭ ���� ���� ������ �� ���� ū ������ ���� �ʿ�)
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

	/***************************************************************
	* ������ ȣ�� API
	* *************************************************************/
	/////////////////////////////////////////////////////////////////
	// Disconnect: IOCP_COMPLTED_LPOVERLAPPED_DISCONNECT �ĺ��ڷ� PostQueuedCompletionStatus ȣ��
	//			   ICOP �۾��� �����忡 ���� ���� ��û
	/////////////////////////////////////////////////////////////////
	void Disconnect(uint64 sessionID);

	/////////////////////////////////////////////////////////////////
	// SendPacket
	// - encoded:		false ��, ����ȭ ���� ���ڵ� ����(������ ���� ����� ���� �޽������� �ִ� ����)
	// - postToWorker:	true ��, SendPost�� �ƴ� SendPostRequestuest �Լ� ȣ��
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
	stCLanSession* AcquireSession(uint64 sessionID);			// called by SendPacket, BufferedSendPacket, SendBufferedPacket
	void ReturnSession(stCLanSession* session);					//						"   "

	void SendPost(uint64 sessionID, bool onSendFlag = false);	// called by SendPacket, SendBufferedPacket
																// IOCP �۽� �Ϸ� ���� �� IOCP_COMPLTED_LPOVERLAPPED_SENDPOST_REQ ��û ��

	void SendPostRequest(uint64 sessionID);		// IOCP_COMPLTED_LPOVERLAPPED_SENDPOST_REQ �ĺ��ڷ� PostQueuedCompletionStatus ȣ��
												// IOCP �۾��� �����忡 SendPost ȣ�� å�� ����

	stCLanSession* CreateNewSession(SOCKET sock);
	bool DeleteSession(uint64 sessionID);

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

	virtual bool OnConnectionRequest(/*IP, Port*/) {
		bool ret = true;
		EnterCriticalSection(&m_SessionAllocIdQueueCS);
		if (m_SessionAllocIdQueue.empty()) {
			// �Ҵ� ���� ���� �ε����� ���ٸ� FALSE ����
			ret = false;
		}
		LeaveCriticalSection(&m_SessionAllocIdQueueCS);

		return ret;
	}

	virtual void OnClientJoin(UINT64 sessionID) = 0;

	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// OnDeleteSendPacket
	// ȣ�� ����: DeleteSession 
	// Desc: ���� ���� �� �۽� ���ۿ� �۽� ��û�� ���۸� �޸� Ǯ�� ��ȯ, override�� ���� �α� �۾� ����
	//////////////////////////////////////////////////////////////////////////////////////////////////////
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
	virtual void OnDeleteSendPacket(UINT64 sessionID, LockFreeQueue<JBuffer*>& sendBufferQueue, JBuffer** sendPostedQueue) {
		while (sendBufferQueue.GetSize() > 0) {
			JBuffer* sendPacket;
			sendBufferQueue.Dequeue(sendPacket);
			m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendPacket);
		}
		for (int i = 0; i < WSABUF_ARRAY_DEFAULT_SIZE; i++) {
			if (sendPostedQueue[i] == NULL) { break; }
			JBuffer* sendPacket = sendPostedQueue[i];
			m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendPacket);
		}
	}
#else
	virtual void OnDeleteSendPacket(UINT64 sessionID, JBuffer& sendRingBuffer) {
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		// �۽� ���۷κ��� �۽� ����ȭ ��Ŷ ������ ��ť�� -> AcquireSRWLockExclusive
		stSessionID stID = *(stSessionID*)&sessionID;
		stCLanSession* session = m_Sessions[stID.idx];
		AcquireSRWLockExclusive(&session->sendBuffSRWLock);
#endif
		// ���� �۽� ť�� �����ϴ� �۽� ����ȭ ���� �޸� ��ȯ
		while (sendRingBuffer.GetUseSize() >= sizeof(JBuffer*)) {
			JBuffer* sendPacekt;
			sendRingBuffer >> sendPacekt;
			m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendPacekt);
		}
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		ReleaseSRWLockExclusive(&session->sendBuffSRWLock);
#endif
	}
#endif
#else
	virtual void OnDeleteSendPacket(UINT64 sessionID, std::vector<std::shared_ptr<JBuffer>>& sendBufferVec) {
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		// �۽� ���۷κ��� �۽� ����ȭ ��Ŷ ������ ��ť�� -> AcquireSRWLockExclusive
		stSessionID stID = *(stSessionID*)&sessionID;
		stCLanSession* session = m_Sessions[stID.idx];
		AcquireSRWLockExclusive(&session->sendBuffSRWLock);
#endif
		sendBufferVec.clear();
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		ReleaseSRWLockExclusive(&session->sendBuffSRWLock);
#endif
}
#endif

	virtual void OnClientLeave(UINT64 sessionID) = 0;

#if defined(ON_RECV_BUFFERING)
	// ���� ��Ŷ�� ���۸��Ͽ� bufferedQueue�� ���� ����
	// OnRecv ȣ�� Ƚ���� ���̱� ���� �õ�
	virtual void OnRecv(UINT64 sessionID, std::queue<JBuffer>& bufferedQueue, size_t recvDataLen) = 0;
#else
	virtual void OnRecv(UINT64 sessionID, JBuffer& recvBuff) = 0;
#endif

	virtual void OnError() {};


private:
	/////////////////////////////////////////////////////////////////
	// Process Recv/Send message, Encode, Decode
	/////////////////////////////////////////////////////////////////
	bool ProcessReceiveMessage(UINT64 sessionID, JBuffer& recvRingBuffer);				// ���� �Ϸ� ���� �� ȣ��
																						// ���� �������� �۾� ���� �� ��� ���� �� ���̷ε�� OnRecv ȣ��
public:
	void Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads);
	bool Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, BYTE* payloads);
	bool Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, JBuffer& ringPayloads);
	inline BYTE GetRandomKey() {
		return rand() % UINT8_MAX;	// 0b0000'0000 ~ 0b0111'1110 (0b1111'1111�� ���ڵ��� �̷������ ���� ���̷ε� �ĺ� ��)
	}

#if defined(ALLOC_BY_TLS_MEM_POOL)
	////////////////////////////////////////////////////////////////////////
	// TLS �޸� Ǯ �Ҵ� ��û �Լ�
	////////////////////////////////////////////////////////////////////////
	inline DWORD AllocTlsMemPool() {
		return m_SerialBuffPoolMgr.AllocTlsMemPool(m_TlsMemPoolDefaultUnitCnt, m_TlsMemPoolDefaultUnitCapacity, m_SerialBufferSize);
	}

	////////////////////////////////////////////////////////////////////////
	// TLS �޸� Ǯ ����ȭ ���� �Ҵ� �� ��ȯ �׸��� ���� ī��Ʈ ���� �Լ�
	////////////////////////////////////////////////////////////////////////
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

	////////////////////////////////////////////////////////////////////////
	// ��ü TLS �޸� Ǯ �Ҵ� �޸� ��ü ���� �� ũ�� ��ȯ �Լ�
	////////////////////////////////////////////////////////////////////////
	inline size_t GetAllocMemPoolUsageUnitCnt() {
		//return (m_SerialBuffPoolMgr.GetTotalAllocMemCnt() - m_SerialBuffPoolMgr.GetTotalFreeMemCnt()) * sizeof(stMemPoolNode<JBuffer>);
		return m_SerialBuffPoolMgr.GetAllocatedMemUnitCnt();
	}
	inline size_t GetAllocMemPoolUsageSize() {
		//return (m_SerialBuffPoolMgr.GetTotalAllocMemCnt() - m_SerialBuffPoolMgr.GetTotalFreeMemCnt()) * sizeof(stMemPoolNode<JBuffer>);
		return m_SerialBuffPoolMgr.GetAllocatedMemUnitCnt() * (sizeof(stMemPoolNode<JBuffer>) + m_SerialBufferSize);
	}
#endif
	
	////////////////////////////////////////////////////////////////////////
	// ����͸� �׸�
	// - Accept TPS
	// - Recv Message TPS
	// - Send Message TPS
	////////////////////////////////////////////////////////////////////////
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
	inline UINT GetMaxOfSessionRecvBuffSize() {
		return m_MaxOfRecvBufferSize;
	}
	inline LONG GetMaxOfSessionSerialSendBuffCnt() {
		return m_MaxOfBufferedSerialSendBufferCnt;
	}
	USHORT getRecvMessageTPS();
	USHORT getSendMessageTPS();

public:
	// CLanServer ���� ���� �ܼ� â �α� �Լ�
	void ConsoleLog();
	virtual void ServerConsoleLog() {}	// called by ConsoleLog
	
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
};

