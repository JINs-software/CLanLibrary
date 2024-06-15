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
		uint64 idx			: 16;		// 0 ~ 65535 세션 인덱스
		uint64 incremental	: 48;		// 세션 ID 증분 part, 세션 할당 시 마다 증분됨
										// => 인덱스, 증분 값 조합 64비트 세션 ID
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
		LockFreeQueue<JBuffer*>	sendBufferQueue;				// 송신 패킷을 담는 세션 송신 (락-프리)큐
		JBuffer* sendPostedQueue[WSABUF_ARRAY_DEFAULT_SIZE];	// WSASend 호출 시 wsabuf에 전달된 직렬화 송신 버퍼를 담는 벡터
																// 송신 완료 통지 시 직렬화 버퍼 메모리 풀에 반환
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
			// 동적 재할당으로 인한 결함 방지
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
			// IOCnt를 증가시키고, releaseFlag를 0으로 초기화하는 순서가 중요하다.
			// (IOCnt를 0으로 1로 초기화하는 방식이 아닌 증가시키는 방식으로)
			InterlockedIncrement((uint32*)&sessionRef);
			
			stSessionRef releaseFlagOffRef;
			releaseFlagOffRef.ioCnt = -1;
			releaseFlagOffRef.releaseFlag = 0;
			uint32 releaseFlagOff = *(uint32*)&releaseFlagOffRef;

			// releaseFlagOffRef == 0xFFFF'FFFF'FFFF'0000
			// 원자적으로 releaseFlag를 초기화한다.
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

					// sendBufferQueue.GetSize() > 0는 비정상적인 상황
					// 임시 방편으로 Dequeue를 통해 송신 큐를 비움. 만약 송신 데이터 존재 시 메모리 누수 발생(메모리 풀 반환x)
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

			// 원자적 비교 및 변경
			// ioCnt == 0, releaseFlag == 0 인 상태를 확인하고, releaseFlag를 On 한다.
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
	bool m_Nagle;				// 네이글 작동 변경 미반영
	bool m_ZeroCopySend;		// 제로-카피 미반영
	/////////////////////////////////

	/////////////////////////////////
	// Session
	/////////////////////////////////
	UINT16					m_MaxOfSessions;
	queue<UINT16>			m_SessionAllocIdQueue;		// 1 ~ m_MaxOfSessions 세션 할당 인덱스 큐
														// (인덱스 0은 없음)
	CRITICAL_SECTION		m_SessionAllocIdQueueCS;	// AcceptThread'Pop(세션 인덱스 할당), WorkerThreads'Push(세션 삭제 시 인덱스 반환)
														// => 동기화 객체 필요

	uint64					m_Incremental;				// 세션 ID 증분 파트 할당 값, 세션 생성 시마다 증가 수행 (단일 Accept 스레드가 증분, 원자적 여난 불필요)

	vector<stCLanSession*>	m_Sessions;					// 벡터 인덱스 == 세션 ID 인덱스인 세션 구조체 관리 벡터
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
	bool m_StopFlag;	// Stop(정지 및 정리 함수) 미호출 시,
						// 소멸자에서 호출되도록 하기 위한 제어 변수

	/////////////////////////////////
	// Threads
	/////////////////////////////////
	// 1. Accept Thread(single)
	HANDLE				m_AcceptThread;

	// 2. IOCP Worker Thread(multi)
	UINT16				m_NumOfWorkerThreads;		// 생성자를 통해 설정, 0 전달 시 GetSystemInfo 호출을 통해 가용 CPU 갯수로 설정
	vector<HANDLE>		m_WorkerThreads;
	vector<DWORD>		m_WorkerThreadIDs;
	map<DWORD, bool>	m_WorkerThreadStartFlag;	// 작업자 스레드 생성 시 OnWorkerThreadCreate 호출
													// TRUE 반환 시 { 작업자 스레드 ID, TRUE } insert
													// FALSE 반환 시  { 작업자 스레드 ID, FALSE } insert
													// 작업자 스레드 함수 도입부에서 TRUE/FALSE 여부 확인, FALSE 시 작업 수행 없이 RETURN

#if defined(ALLOC_BY_TLS_MEM_POOL)
	/////////////////////////////////
	// Memory Pool
	/////////////////////////////////
public:
	TlsMemPoolManager<JBuffer>	m_SerialBuffPoolMgr;				// CLanServer 직렬화 버퍼 메모리 풀 관리자

	size_t						m_TlsMemPoolDefaultUnitCnt;			// tlsMemPool 기본 할당 메모리 객체 갯수
	size_t						m_TlsMemPoolDefaultUnitCapacity;	// tlsMemPool 최대 메모리 객체 관리 갯수
	UINT						m_SerialBufferSize;					// m_SerialBuffPoolMgr을 통해 생성된 tlsMemPool로 부터 할당받는 직렬화 버퍼 크기
																	// (서버에서 할당받아 사용되는 직렬화 버퍼 저장 데이터 중 가장 큰 값으로 지정 필요)
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
	* 컨텐츠 호출 API
	* *************************************************************/
	/////////////////////////////////////////////////////////////////
	// Disconnect: IOCP_COMPLTED_LPOVERLAPPED_DISCONNECT 식별자로 PostQueuedCompletionStatus 호출
	//			   ICOP 작업자 스레드에 세션 삭제 요청
	/////////////////////////////////////////////////////////////////
	void Disconnect(uint64 sessionID);

	/////////////////////////////////////////////////////////////////
	// SendPacket
	// - encoded:		false 시, 직렬화 버퍼 인코딩 수행(복수의 공용 헤더가 붙은 메시지들이 있다 가정)
	// - postToWorker:	true 시, SendPost가 아닌 SendPostRequestuest 함수 호출
	/////////////////////////////////////////////////////////////////
#if defined(ALLOC_BY_TLS_MEM_POOL)
	bool SendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded = false, bool postToWorker = false);
#else
	bool SendPacket(uint64 sessionID, std::shared_ptr<JBuffer> sendDataPtr, bool reqToWorkerTh = false);
#endif
	////////////////////////////////////////////////////////////////////////////////////////////////////////
	// BufferSendPacket: 송신 버퍼 큐(또는 링버퍼) 삽입만 수행, 추후 SendBufferedPacket이 없다면 송신 보장 x
	////////////////////////////////////////////////////////////////////////////////////////////////////////
	bool BufferSendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded = false);
	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// SendBufferedPacket: 송신 버퍼 큐(또는 링버퍼)에 송신 대기 패킷이 존재한다면 SendPost 또는 SendPostRequset 호출
	//					   AcquireSession에 대한 부하 고려 필요
	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	void SendBufferedPacket(uint64 sessionID, bool postToWorker = false);

private:
	stCLanSession* AcquireSession(uint64 sessionID);			// called by SendPacket, BufferedSendPacket, SendBufferedPacket
	void ReturnSession(stCLanSession* session);					//						"   "

	void SendPost(uint64 sessionID, bool onSendFlag = false);	// called by SendPacket, SendBufferedPacket
																// IOCP 송신 완료 통지 및 IOCP_COMPLTED_LPOVERLAPPED_SENDPOST_REQ 요청 시

	void SendPostRequest(uint64 sessionID);		// IOCP_COMPLTED_LPOVERLAPPED_SENDPOST_REQ 식별자로 PostQueuedCompletionStatus 호출
												// IOCP 작업자 스레드에 SendPost 호출 책임 전가

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

	virtual bool OnConnectionRequest(/*IP, Port*/) {
		bool ret = true;
		EnterCriticalSection(&m_SessionAllocIdQueueCS);
		if (m_SessionAllocIdQueue.empty()) {
			// 할당 가능 세션 인덱스가 없다면 FALSE 리턴
			ret = false;
		}
		LeaveCriticalSection(&m_SessionAllocIdQueueCS);

		return ret;
	}

	virtual void OnClientJoin(UINT64 sessionID) = 0;

	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// OnDeleteSendPacket
	// 호출 시점: DeleteSession 
	// Desc: 세션 삭제 시 송신 버퍼와 송신 요청된 버퍼를 메모리 풀에 반환, override를 통해 로그 작업 가능
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
		// 송신 버퍼로부터 송신 직렬화 패킷 포인터 디큐잉 -> AcquireSRWLockExclusive
		stSessionID stID = *(stSessionID*)&sessionID;
		stCLanSession* session = m_Sessions[stID.idx];
		AcquireSRWLockExclusive(&session->sendBuffSRWLock);
#endif
		// 세션 송신 큐에 존재하는 송신 직렬화 버퍼 메모리 반환
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
		// 송신 버퍼로부터 송신 직렬화 패킷 포인터 디큐잉 -> AcquireSRWLockExclusive
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
	// 수신 패킷을 버퍼링하여 bufferedQueue를 통해 전달
	// OnRecv 호출 횟수를 줄이기 위한 시도
	virtual void OnRecv(UINT64 sessionID, std::queue<JBuffer>& bufferedQueue, size_t recvDataLen) = 0;
#else
	virtual void OnRecv(UINT64 sessionID, JBuffer& recvBuff) = 0;
#endif

	virtual void OnError() {};


private:
	/////////////////////////////////////////////////////////////////
	// Process Recv/Send message, Encode, Decode
	/////////////////////////////////////////////////////////////////
	bool ProcessReceiveMessage(UINT64 sessionID, JBuffer& recvRingBuffer);				// 수신 완료 통지 시 호출
																						// 공통 프로토콜 작업 수행 및 헤더 제거 후 페이로드로 OnRecv 호출
public:
	void Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads);
	bool Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, BYTE* payloads);
	bool Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, JBuffer& ringPayloads);
	inline BYTE GetRandomKey() {
		return rand() % UINT8_MAX;	// 0b0000'0000 ~ 0b0111'1110 (0b1111'1111은 디코딩이 이루어지지 않은 페이로드 식별 값)
	}

#if defined(ALLOC_BY_TLS_MEM_POOL)
	////////////////////////////////////////////////////////////////////////
	// TLS 메모리 풀 할당 요청 함수
	////////////////////////////////////////////////////////////////////////
	inline DWORD AllocTlsMemPool() {
		return m_SerialBuffPoolMgr.AllocTlsMemPool(m_TlsMemPoolDefaultUnitCnt, m_TlsMemPoolDefaultUnitCapacity, m_SerialBufferSize);
	}

	////////////////////////////////////////////////////////////////////////
	// TLS 메모리 풀 직렬화 버퍼 할당 및 반환 그리고 참조 카운트 증가 함수
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
		hdr->randKey = (BYTE)(-1);	// Encode 전 송신 직렬화 버퍼 식별

		return msg;
	}
	inline void FreeSerialBuff(JBuffer* buff) {
		m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(buff);
	}
	inline void AddRefSerialBuff(JBuffer* buff) {
		m_SerialBuffPoolMgr.GetTlsMemPool().IncrementRefCnt(buff, 1);
	}

	////////////////////////////////////////////////////////////////////////
	// 전체 TLS 메모리 풀 할당 메모리 객체 갯수 및 크기 반환 함수
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
	// 모니터링 항목
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
	// CLanServer 관련 정보 콘솔 창 로그 함수
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

