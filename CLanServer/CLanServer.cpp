#include "CLanServer.h"
#include <cassert>
#include <process.h>
#include <fstream>
#include <cstdlib> // system 함수를 사용하기 위해 필요

#if defined(ALLOC_BY_TLS_MEM_POOL)
CLanServer::CLanServer(const char* serverIP, uint16 serverPort,
	DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections,
	size_t tlsMemPoolDefaultUnitCnt, size_t tlsMemPoolDefaultUnitCapacity, bool tlsMemPoolReferenceFlag, bool tlsMemPoolPlacementNewFlag,
	UINT serialBufferSize,
#if defined(LOCKFREE_SEND_QUEUE)
	uint32 sessionRecvBuffSize,
#else
	uint32 sessionSendBuffSize, uint32 sessionRecvBuffSize,
#endif
	bool beNagle, bool zeroCopySend
)
	: m_TotalAccept(0), m_AcceptThread(0), m_MaxOfRecvBufferSize(0), m_MaxOfBufferedSerialSendBufferCnt(0),
	m_MaxOfSessions(maxOfConnections), m_Incremental(0),
#if defined(LOCKFREE_SEND_QUEUE)
	m_SessionRecvBufferSize(sessionRecvBuffSize),
#else
	m_SessionSendBufferSize(sessionSendBuffSize), m_SessionRecvBufferSize(sessionRecvBuffSize),
#endif
	m_StopFlag(false), m_NumOfWorkerThreads(numOfWorkerThreads),
	m_SerialBuffPoolMgr(tlsMemPoolDefaultUnitCnt, tlsMemPoolDefaultUnitCapacity, tlsMemPoolReferenceFlag, tlsMemPoolPlacementNewFlag),
	m_TlsMemPoolDefaultUnitCnt(tlsMemPoolDefaultUnitCnt), m_TlsMemPoolDefaultUnitCapacity(tlsMemPoolDefaultUnitCapacity),
	m_SerialBufferSize(serialBufferSize),
	m_Nagle(beNagle), m_ZeroCopySend(zeroCopySend)
#else
CLanServer::CLanServer(const char* serverIP, uint16 serverPort,
	DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections,
#if defined(LOCKFREE_SEND_QUEUE)
	uint32 sessionRecvBuffSize,
#else
	uint32 sessionSendBuffSize, uint32 sessionRecvBuffSize,
#endif
	bool beNagle
)
	: m_MaxOfSessions(maxOfConnections), m_Incremental(0), m_NumOfWorkerThreads(numOfWorkerThreads), m_StopFlag(false)
#endif
{
	////////////////////////////////////////////////////////////////////////////////////////////////////
	// 네트워크 초기화
	////////////////////////////////////////////////////////////////////////////////////////////////////
	WSAData wsadata;
	InitWindowSocketLib(&wsadata);
	m_ListenSock = CreateWindowSocket_IPv4(true);
	if (serverIP == nullptr) {
		m_ListenSockAddr = CreateServerADDR(serverPort);
	}
	else {
		m_ListenSockAddr = CreateServerADDR(serverIP, serverPort);
	}
	int optval = 1;
	setsockopt(m_ListenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval));


	////////////////////////////////////////////////////////////////////////////////////////////////////
	// 세션 관리 초기화
	////////////////////////////////////////////////////////////////////////////////////////////////////
	for (uint16 idx = 1; idx <= m_MaxOfSessions; idx++) {		// sessionID의 인덱스 부 값이 0이라는 것은 존재하지 않은 세션이라는 보초값
		m_SessionAllocIdQueue.push(idx);
	}
	m_Sessions.resize(m_MaxOfSessions + 1, NULL);
	for (uint16 idx = 1; idx <= m_MaxOfSessions; idx++) {
		m_Sessions[idx] = new stCLanSession(m_SessionRecvBufferSize);
	}
	InitializeCriticalSection(&m_SessionAllocIdQueueCS);

	////////////////////////////////////////////////////////////////////////////////////////////////////
	// IOCP 객체 초기화
	////////////////////////////////////////////////////////////////////////////////////////////////////
	m_IOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, numOfIocpConcurrentThrd);
#if defined(CLANSERVER_ASSERT)
	if (m_IOCP == NULL) {
		DebugBreak();
	}
#endif

	////////////////////////////////////////////////////////////////////////////////////////////////////
	// 스레드 관리 초기화
	////////////////////////////////////////////////////////////////////////////////////////////////////
	if (m_NumOfWorkerThreads == 0) {
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		m_NumOfWorkerThreads = si.dwNumberOfProcessors;
	}
	m_WorkerThreads.resize(m_NumOfWorkerThreads, NULL);
	m_WorkerThreadIDs.resize(m_NumOfWorkerThreads, NULL);
	
}
CLanServer::~CLanServer()
{
	if (!m_StopFlag) {
		Stop();
	}
}

bool CLanServer::Start()
{
	// 리슨 소켓 바인딩
	if (BindSocket(m_ListenSock, m_ListenSockAddr) == SOCKET_ERROR) {
		return false;
	}
	// 리슨 소켓 Accept 준비
	if (ListenSocket(m_ListenSock, SOMAXCONN) == SOCKET_ERROR) {
		return false;
	}

	// 링거 옵션 추가 (강제 종료 유도)
	struct linger linger_option;
	linger_option.l_onoff = 1;  // SO_LINGER 사용
	linger_option.l_linger = 0; // 소켓이 닫힐 때 최대 대기 시간 (초)
	setsockopt(m_ListenSock, SOL_SOCKET, SO_LINGER, (const char*)&linger_option, sizeof(linger_option));

	////////////////////////////////////////////////////////////////////////////////////////////////////
	// 스레드 생성
	////////////////////////////////////////////////////////////////////////////////////////////////////
	// Accept 스레드 생성
	m_AcceptThread = (HANDLE)_beginthreadex(NULL, 0, CLanServer::AcceptThreadFunc, this, 0, NULL);
	cout << "[Start Thread] Accept Thread" << endl;

	// IOCP 작업자 스레드 생성
	for (uint16 idx = 0; idx < m_NumOfWorkerThreads; idx++) {
		uintptr_t ret = _beginthreadex(NULL, 0, CLanServer::WorkerThreadFunc, this, CREATE_SUSPENDED, NULL);
		m_WorkerThreads[idx] = (HANDLE)ret;
		if (m_WorkerThreads[idx] == INVALID_HANDLE_VALUE) {
			DebugBreak();
		}
		DWORD thID = GetThreadId(m_WorkerThreads[idx]);
		m_WorkerThreadIDs[idx] = thID;
		m_WorkerThreadStartFlag.insert({thID, true});

		if (!OnWorkerThreadCreate(m_WorkerThreads[idx])) {
			m_WorkerThreadStartFlag[thID] = false;
			cout << "[Cant't Start Thread] Worker Thread (thID: " << GetThreadId(m_WorkerThreads[idx]) << ")" << endl;
			_endthreadex(ret);
		}
		else {
			cout << "[Start Thread] Worker Thread (thID: " << GetThreadId(m_WorkerThreads[idx]) << ")" << endl;
			ResumeThread(m_WorkerThreads[idx]);
		}
	}
#if defined(CALCULATE_TRANSACTION_PER_SECOND)
	m_CalcTpsThread = (HANDLE)_beginthreadex(NULL, 0, CLanServer::CalcTpsThreadFunc, this, 0, NULL);
#endif

	// 모든 스레드가 생성된 후 호출, 생성된 스레드는 작업 중...
	OnWorkerThreadCreateDone();
}

void CLanServer::Stop()
{
	m_StopFlag = true;

	// 1. 서버 리슨 소켓 닫기 -> Accept 스레드의 accept 함수에서 INVALID_SOCKET 반환
	closesocket(m_ListenSock);

	// 2. Accept 스레드가 작업자 스레드의 종료를 위해 PostQueuedCompletionStatus 함수를 통해 종료를 위한 완료 통지를 발생시킴. 이 후 Accept 스레드 종료
	// 2-1. 작업자 스레드 수 만큼 Post
	// 2-2. 하나의 Post 후 각 스레드들이 다시 Post로 전파
	// => 일단 2-1 방식으로 진행
	for (int i = 0; i < m_NumOfWorkerThreads; i++) {
		PostQueuedCompletionStatus(m_IOCP, 0, 0, NULL);
	}

	// 3. Accept 스레드 및 작업자 스레드 종료를 대기
	WaitForSingleObject(m_AcceptThread, INFINITE);
	cout << "Exit Accept Thread..." << endl;

	// 4. 잔여 클라이언트 통신 소켓 정리
	WaitForMultipleObjects(m_NumOfWorkerThreads, m_WorkerThreads.data(), TRUE, INFINITE);
	cout << "Exit Worker Threads..." << endl;

	for (size_t i = 0; i < m_Sessions.size(); i++) {
		if (m_Sessions[i]) {
			shutdown(m_Sessions[i]->sock, SD_BOTH);
			closesocket(m_Sessions[i]->sock);
		}
	}

	DeleteCriticalSection(&m_SessionAllocIdQueueCS);
	CloseHandle(m_IOCP);

	WSACleanup();
}


void CLanServer::Disconnect(uint64 sessionID)
{
	stSessionID orgID = *((stSessionID*)&sessionID);
	stCLanSession* delSession = m_Sessions[orgID.idx];
#if defined(CLANSERVER_ASSERT)
	if (delSession->sessionRef.ioCnt < 1) {
		DebugBreak();
	}
#endif
	PostQueuedCompletionStatus(m_IOCP, 0, (ULONG_PTR)delSession, (LPOVERLAPPED)IOCP_COMPLTED_LPOVERLAPPED_DISCONNECT);
}

#if defined(ALLOC_BY_TLS_MEM_POOL)
bool CLanServer::SendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded, bool postToWorker) {	// sendDataPtr의 deqOffset은 0으로 가정

	stCLanSession* session = AcquireSession(sessionID);
	if (session != nullptr) {				// 인덱스가 동일한 다른 세션이거나 제거된(제거중인) 세션

		if (!encoded) {
			///////////////////////////////////////////// 인코딩 수행 //////////////////////////////////////////////////
			if (sendDataPtr->GetUseSize() < sizeof(stMSG_HDR)) {
#if defined(CLANSERVER_ASSERT)
				DebugBreak();
#endif
				return false;
			}
			else {
				UINT offset = 0;
				stMSG_HDR* hdr;// = (stMSG_HDR*)sendDataPtr->GetDequeueBufferPtr();
				while (offset < sendDataPtr->GetUseSize()) {
					hdr = (stMSG_HDR*)sendDataPtr->GetBufferPtr(offset);
					offset += sizeof(stMSG_HDR);
					if (hdr->randKey == (BYTE)-1) {
						hdr->randKey = GetRandomKey();
						Encode(hdr->randKey, hdr->len, hdr->checkSum, sendDataPtr->GetBufferPtr(offset));
					}
					offset += hdr->len;
				}
			}
			///////////////////////////////////////////////////////////////////////////////////////////////////////////
		}

#if defined(LOCKFREE_SEND_QUEUE)
		session->sendBufferQueue.Enqueue(sendDataPtr);
		LONG serialSendBufferCnt = session->sendBufferQueue.GetSize();
		if (serialSendBufferCnt > m_MaxOfBufferedSerialSendBufferCnt) {
			m_MaxOfBufferedSerialSendBufferCnt = serialSendBufferCnt;
		}
#else
		AcquireSRWLockExclusive(&session->sendBuffSRWLock);

		if (session->sendRingBuffer.GetFreeSize() < sizeof(UINT_PTR)) {
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터를 Enqueue할 여유 사이즈 없음" << endl;
			DebugBreak();
		}
		uint32 enqSize = session->sendRingBuffer.Enqueue((BYTE*)&sendDataPtr, sizeof(UINT_PTR));

#if defined(SENDBUFF_MONT_LOG)
		uint32 sendBuffSize = session->sendRingBuffer.GetUseSize();
		if (m_SessionOfMaxSendBuff == sessionID || m_SendBuffOfMaxSize < sendBuffSize) {
			m_SendBuffOfMaxSize = sendBuffSize;
			m_SessionOfMaxSendBuff = sessionID;
		}
#endif
		if (enqSize < sizeof(UINT_PTR)) {
			// 송신 링-버퍼에 송신 데이터를 복사할 수 있음을 확인했음에도 불구하고,
			// Enqueue 사이즈가 송신 데이터의 크기보다 작은 상황 발생
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터 전체 Enqueue 실패" << endl;
			DebugBreak();
		}

		ReleaseSRWLockExclusive(&session->sendBuffSRWLock);
#endif

		if (postToWorker) {
			SendPostRequest(sessionID);
		}
		else {
			SendPost(sessionID);
		}
	}
	else {
		return false;
	}

	ReturnSession(session);

	return true;
}
#else
bool CLanServer::SendPacket(uint64 sessionID, std::shared_ptr<JBuffer> sendDataPtr, bool reqToWorkerTh)
{
	stCLanSession* session = AcquireSession(sessionID);
	if (session != nullptr) {				// 인덱스가 동일한 다른 세션이거나 제거된(제거중인) 세션

		AcquireSRWLockExclusive(&session->sendBuffSRWLock);

		session->sendBufferVector.push_back(sendDataPtr);

		ReleaseSRWLockExclusive(&session->sendBuffSRWLock);

		if (reqToWorkerTh) {
			SendPostRequest(sessionID);
		}
		else {
			SendPost(sessionID);
		}
	}
	else {
		return false;
	}

	ReturnSession(session);

	return true;
}
#endif

bool CLanServer::BufferSendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded) {
	stCLanSession* session = AcquireSession(sessionID);
	if (session != nullptr) {				// 인덱스가 동일한 다른 세션이거나 제거된(제거중인) 세션

		if (!encoded) {
			///////////////////////////////////////////// 인코딩 수행 //////////////////////////////////////////////////
			if (sendDataPtr->GetUseSize() < sizeof(stMSG_HDR)) {
#if defined(CLANSERVER_ASSERT)
				DebugBreak();
#endif
				return false;
			}
			else {
				UINT offset = 0;
				stMSG_HDR* hdr;// = (stMSG_HDR*)sendDataPtr->GetDequeueBufferPtr();
				while (offset < sendDataPtr->GetUseSize()) {
					hdr = (stMSG_HDR*)sendDataPtr->GetBufferPtr(offset);
					offset += sizeof(stMSG_HDR);
					if (hdr->randKey == (BYTE)-1) {
						hdr->randKey = GetRandomKey();
						Encode(hdr->randKey, hdr->len, hdr->checkSum, sendDataPtr->GetBufferPtr(offset));
					}
					offset += hdr->len;
				}
			}
			///////////////////////////////////////////////////////////////////////////////////////////////////////////
		}

#if defined(LOCKFREE_SEND_QUEUE)
		session->sendBufferQueue.Enqueue(sendDataPtr);
		LONG serialSendBufferCnt = session->sendBufferQueue.GetSize();
		if (serialSendBufferCnt > m_MaxOfBufferedSerialSendBufferCnt) {
			m_MaxOfBufferedSerialSendBufferCnt = serialSendBufferCnt;
		}
#else
		AcquireSRWLockExclusive(&session->sendBuffSRWLock);

		if (session->sendRingBuffer.GetFreeSize() < sizeof(UINT_PTR)) {
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터를 Enqueue할 여유 사이즈 없음" << endl;
			DebugBreak();
		}
		uint32 enqSize = session->sendRingBuffer.Enqueue((BYTE*)&sendDataPtr, sizeof(UINT_PTR));
		if (enqSize < sizeof(UINT_PTR)) {
			// 송신 링-버퍼에 송신 데이터를 복사할 수 있음을 확인했음에도 불구하고,
			// Enqueue 사이즈가 송신 데이터의 크기보다 작은 상황 발생
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터 전체 Enqueue 실패" << endl;
			DebugBreak();
		}

		ReleaseSRWLockExclusive(&session->sendBuffSRWLock);
#endif
	}
	else {
		return false;
	}

	ReturnSession(session);

	return true;
}

void CLanServer::SendBufferedPacket(uint64 sessionID, bool postToWorker)
{
	// AcquireSession 호출 간소화
	uint16 idx = (uint16)sessionID;
	stCLanSession* session = m_Sessions[idx];
	if (session->sendBufferQueue.GetSize() == 0) {
		return;
	}

	session = AcquireSession(sessionID);
	if (session != NULL) {
		if (postToWorker) {
			SendPostRequest(sessionID);
		}
		else {
			SendPost(sessionID);
		}
		ReturnSession(session);
	}
}

CLanServer::stCLanSession* CLanServer::AcquireSession(uint64 sessionID)
{
	uint16 idx = (uint16)sessionID;								
	stCLanSession* session = m_Sessions[idx];					// 세션 ID의 인덱스 파트를 통해 세션 획득
	if (session == nullptr) {									// AcquireSession을 호출하는 시점에서 찾고자 하였던 세션을 획득하였다는 보장은 할 수 없음
																// (이미 삭제된 세션이거나, 삭제된 후 같은 인덱스 자리에 재활용된 세션일 수 있음)
#if defined(CLANSERVER_ASSERT)
		DebugBreak();
#endif
		return nullptr;
	}
	else {
		///////////////////////////////////////////////////////////////////
		// 세션 참조 카운트(ioCnt) 증가!
		uint32 uiRef = InterlockedIncrement((uint32*)&session->sessionRef);
		///////////////////////////////////////////////////////////////////
		stSessionRef sessionRef = *((stSessionRef*)&uiRef);
#if defined(CLANSERVER_ASSERT)
		if (sessionRef.ioCnt < 1) {
			DebugBreak();
		}
#endif

		// 세션 IOCnt를 증가한 시점 이후,
		// 참조하고자 하였던 본래 세션이든, 또는 같은 인덱스 자리에 재활용된 세션이든 삭제되지 않는 보장을 할 수 있음

		// (1) if(session->sessionRef.releaseFlag == 1)
		//		=> 본래 참조하고자 하였던 세션 또는 새로운 세션이 삭제(중)
		// (2) if(session->sessionRef.releaseFlag == 0 && sessionID != session->uiId)
		//		=> 참조하고자 하였던 세션은 이미 삭제되고, 새로운 세션이 동일 인덱스에 생성
		if (session->sessionRef.releaseFlag == 1 || sessionID != session->uiId) {
			// 세션 참조 카운트(ioCnt) 감소
			InterlockedDecrement((uint32*)&session->sessionRef);
			
			// 세션 삭제 및 변경 가능 구역
			// ....

			// case0) ioCnt >= 1, 세션 유효
			// case1) releaseFlag == 1 유지, 세션 삭제(중)
			// caas2) releaseFlag == 0, ioCnt == 0
			//			=> Disconnect 책임

			stSessionRef exgRef;
			exgRef.ioCnt = 1;
			exgRef.releaseFlag = 0;
			uint32 exg = *((uint32*)&exgRef);
			uiRef = InterlockedCompareExchange((uint32*)&session->sessionRef, exg, 0);
			sessionRef = *((stSessionRef*)&uiRef);

			// releaseFlag == 0이 된 상태에서 ioCnt == 0이 된 상황
			// => Disconnect 호출 책임
			// CAS를 진행하였기에 CAS 이 후부터 세션 삭제는 없음이 보장됨
			if (sessionRef.ioCnt == 0 && sessionRef.releaseFlag == 0) {
				Disconnect(session->uiId);
			}
			else if (sessionRef.ioCnt == 0 && sessionRef.releaseFlag == 1) {
				// 새로운 세션 생성 전
				// nothing to do..
			}
#if defined(CLANSERVER_ASSERT)
			else if (sessionRef.ioCnt < 0) {
				DebugBreak();
			}
#endif
			return nullptr;
		}
		else {
			// 기존 세션 확정 -> 반환 (ioCnt == 1이라면 ReturnSession에서 처리할 것)
			return session;			
		}
	}
}

void CLanServer::ReturnSession(stCLanSession* session)
{
	//assert(session->sessionRef.ioCnt >= 1);
	//if (session->sessionRef.ioCnt == 1) {
	//	Disconnect(session->uiId, "ReturnSession Disconnect");
	//}
	//else {
	//	InterlockedDecrement((uint32*)&session->sessionRef);
	//}
	// => 문제 식별
	// ex) 비동기 수신이 걸려있는 상황에서 AcquireSession을 통해 ioCnt == 2인 상황,
	// (1) 업데이트 스레드, ioCnt == 2인 상황에서 if (session->sessionRef.ioCnt == 1) 조건 통과
	// (2) 작업자 스레드, GQCS 실패 또는 추가적인 WSARecv 실패 -> ioCnt 감소 -> ioCnt == 1 이기에 세션 삭제를 시도하지 못함(DeleteSession 호출 x)
	// (3) 업데이트 스레드, else 문에서 ioCnt를 감소시킴 -> ioCnt == 0
	// =>  ioCnt == 0, release == 0인 상황에서 세션이 삭제되지 않고 남아있는 상황 발생됨.

	if (session->sessionRef.ioCnt < 1) {
#if defined(CLANSERVER_ASSERT)
		DebugBreak();
#endif
		return;
	}

	uint64 sessionID = session->uiId;

	uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
	stSessionRef sessionRef = *((stSessionRef*)&uiRef);
#if defined(CLANSERVER_ASSERT)
	if (sessionRef.ioCnt < 0) {
		DebugBreak();
	}
#endif

	stSessionRef exgRef;
	exgRef.ioCnt = 1;
	exgRef.releaseFlag = 0;
	uint32 exg = *((uint32*)&exgRef);
	uiRef = InterlockedCompareExchange((uint32*)&session->sessionRef, exg, 0);
	sessionRef = *((stSessionRef*)&uiRef);
	if (sessionRef.ioCnt == 0 && sessionRef.releaseFlag == 0) {
		Disconnect(sessionID);
	}
	else if (sessionRef.ioCnt == 0 && sessionRef.releaseFlag == 1) {
		// 새로운 세션 생성 전
		// nothing to do..
	}
#if defined(CLANSERVER_ASSERT)
	else if (sessionRef.ioCnt < 0) {
		DebugBreak();
	}
#endif

	// (1) ioCnt == 1인 상황, 이 뜻은 AcquireSession에 의해 증가된 ioCnt만 남았다는 뜻
	// (2) ioCnt == 1인 상황에서 InterlockedDecrement에 의해 0으로 변경되었을 때,
	//     다른 iocp 작업자 스레드에 의해 감소될 상황은 없어보임. 이미 ioCnt == 0인 상황에서 AcquireSession으로 1이 된 상황이기에
}

void CLanServer::SendPost(uint64 sessionID, bool onSendFlag)
{
	uint16 idx = (uint16)sessionID;
	stCLanSession* session = m_Sessions[idx];

	if (session == nullptr) {
#if defined(CLANSERVER_ASSERT)
		DebugBreak();
#endif
		return;
	}

	if (onSendFlag || InterlockedExchange(&session->sendFlag, 1) == 0) {
		session->clearSendOverlapped();	// 송신용 overlapped 구조체 초기화

		
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
		DWORD numOfMessages = session->sendBufferQueue.GetSize();
#else
		AcquireSRWLockShared(&session->sendBuffSRWLock);
		DWORD numOfMessages = session->sendRingBuffer.GetUseSize() / sizeof(UINT_PTR);
		ReleaseSRWLockShared(&session->sendBuffSRWLock);
#endif
#else
		AcquireSRWLockShared(&session->sendBuffSRWLock);
		DWORD numOfMessages = session->sendBufferVector.size();
		ReleaseSRWLockShared(&session->sendBuffSRWLock);
#endif

		WSABUF wsabuffs[WSABUF_ARRAY_DEFAULT_SIZE];

		if (numOfMessages > 0) {
			InterlockedIncrement((uint32*)&session->sessionRef);

			int sendLimit = min(numOfMessages, WSABUF_ARRAY_DEFAULT_SIZE);
			for (int idx = 0; idx < sendLimit; idx++) {
#if defined(ALLOC_BY_TLS_MEM_POOL)
				JBuffer* msgPtr;
#if defined(LOCKFREE_SEND_QUEUE)
				//session->sendBufferQueue.Dequeue(msgPtr);
				// => single reader 보장
				session->sendBufferQueue.Dequeue(msgPtr, true);

				//session->sendPostedQueue.push(msgPtr);
				// => push 오버헤드 제거
				session->sendPostedQueue[idx] = msgPtr;

				wsabuffs[idx].buf = (CHAR*)msgPtr->GetBeginBufferPtr();
				wsabuffs[idx].len = msgPtr->GetUseSize();

				if (wsabuffs[idx].buf == NULL || wsabuffs[idx].len == 0) {
#if defined(CLANSERVER_ASSERT)
					DebugBreak();
#endif
					return;
				}
				
#else
				session->sendRingBuffer.Peek(sizeof(UINT_PTR) * idx, (BYTE*)&msgPtr, sizeof(UINT_PTR));
				wsabuffs[idx].buf = (CHAR*)msgPtr->GetBeginBufferPtr();
				wsabuffs[idx].len = msgPtr->GetUseSize();
				if (wsabuffs[idx].buf == NULL || wsabuffs[idx].len == 0) {
					DebugBreak();
				}
#endif
#else
				shared_ptr<JBuffer> msgPtr = session->sendBufferVector[idx];
				wsabuffs[idx].buf = (CHAR*)msgPtr->GetBeginBufferPtr();
				wsabuffs[idx].len = msgPtr->GetUseSize();
#endif
				
			}
			session->sendOverlapped.Offset = sendLimit;		// Offset 멤버를 활용하여 송신한 메시지 갯수를 담도록 한다.

			if (WSASend(session->sock, wsabuffs, sendLimit, NULL, 0, &session->sendOverlapped, NULL) == SOCKET_ERROR) {
				int errcode = WSAGetLastError();
				if (errcode != WSA_IO_PENDING) {
					uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
					stSessionRef sessionRef = *((stSessionRef*)&uiRef);

					assert(sessionRef.ioCnt >= 0);
					if (sessionRef.ioCnt == 0) {
						for (int i = 0; i < m_NumOfWorkerThreads; i++) {
							if (DeleteSession(sessionID)) {
								OnClientLeave(sessionID);
							}
							break;
							
						}
					}
				}
			}
		}
		else {
			InterlockedExchange(&session->sendFlag, 0);
		}
	}
}

void CLanServer::SendPostRequest(uint64 sessionID)
{
	// SendPost와 마찬가지로 세션 ioCnt >= 1이 보장된 상태에서 호출이 가정됨.
	stSessionID orgID = *((stSessionID*)&sessionID);
	stCLanSession* session = m_Sessions[orgID.idx];

	// 세션 송신 플래그 off 상태에서만 Post를 수행하여 iocp 큐잉 부하를 감소시킨다. 
	// iocp 작업자 스레드에서 해당 요청을 받으면, 
	// onSendFlag on 상태로 SendPost 호출, 그리고 ioCnt를 감소시키고 세션 종료 판단을 수행
	if(InterlockedCompareExchange(&session->sendFlag, 1, 0) == 0) {
		InterlockedIncrement((uint32*)&session->sessionRef);
		PostQueuedCompletionStatus(m_IOCP, 0, (ULONG_PTR)session, (LPOVERLAPPED)IOCP_COMPLTED_LPOVERLAPPED_SENDPOST_REQ);
	}
}

CLanServer::stCLanSession* CLanServer::CreateNewSession(SOCKET sock)
{
	stCLanSession* newSession = nullptr;
	stSessionID newSessionID;

	EnterCriticalSection(&m_SessionAllocIdQueueCS);
	if (!m_SessionAllocIdQueue.empty() /* && (m_Incremental & 0xFFFF'0000'0000'0000) == 0 */) {
		uint16 allocIdx = m_SessionAllocIdQueue.front();
		m_SessionAllocIdQueue.pop();
		
		newSession = m_Sessions[allocIdx];

		newSessionID.idx = allocIdx;
		newSessionID.incremental = m_Incremental++;
		newSession->Init(sock, newSessionID);
	}
	LeaveCriticalSection(&m_SessionAllocIdQueueCS);

	return newSession;
}

bool CLanServer::DeleteSession(uint64 sessionID)
{
	bool ret = false;
	uint16 idx = (uint16)sessionID;
	stCLanSession* delSession = m_Sessions[idx];

	if (delSession == nullptr) {
#if defined(CLANSERVER_ASSERT)
		DebugBreak();
#endif
		return false;
	}

	stSessionID orgID = *((stSessionID*)&sessionID);

	if (delSession->TryRelease()) {
		ret = true;
#if defined(CLANSERVER_ASSERT)
		if (delSession->uiId != sessionID) {
			DebugBreak();
		}
#endif

		// 세션 삭제
		uint16 allocatedIdx = delSession->Id.idx;
		SOCKET delSock = m_Sessions[allocatedIdx]->sock;
		shutdown(delSock, SD_BOTH);
		closesocket(delSock);

#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
		OnDeleteSendPacket(sessionID, delSession->sendBufferQueue, delSession->sendPostedQueue);
#else
		OnDeleteSendPacket(sessionID, delSession->sendRingBuffer);
#endif
#else
		OnDeleteSendPacket(sessionID, delSession->sendBufferVector);
#endif

		// 세션 ID 인덱스 반환
		EnterCriticalSection(&m_SessionAllocIdQueueCS);
		m_SessionAllocIdQueue.push(allocatedIdx);
		LeaveCriticalSection(&m_SessionAllocIdQueueCS);
	}

	return ret;
}

UINT __stdcall CLanServer::AcceptThreadFunc(void* arg)
{
	CLanServer* clanserver = (CLanServer*)arg;

#if defined(ALLOC_BY_TLS_MEM_POOL)
	clanserver->m_SerialBuffPoolMgr.AllocTlsMemPool(clanserver->m_TlsMemPoolDefaultUnitCnt, clanserver->m_TlsMemPoolDefaultUnitCapacity, clanserver->m_SerialBufferSize);	// 생성자에서 설정한 Default 값을 따름
#endif

	while (true) {
		SOCKADDR_IN clientAddr;
		int addrLen = sizeof(clientAddr);
		SOCKET clientSock = ::accept(clanserver->m_ListenSock, (sockaddr*)&clientAddr, &addrLen);
		clanserver->m_TotalAccept++;
		clanserver->m_AcceptTransaction++;
		if (clientSock != INVALID_SOCKET) {
			if (!clanserver->OnConnectionRequest()) {
				shutdown(clientSock, SD_BOTH);
				closesocket(clientSock);
			}
			else {
				// 세션 생성
				stCLanSession* newSession = clanserver->CreateNewSession(clientSock);
				if (newSession != nullptr) {
#if defined(CALCULATE_TRANSACTION_PER_SECOND)
					clanserver->m_CalcTpsItems[ACCEPT_TRANSACTION]++;
					clanserver->m_TotalTransaction[ACCEPT_TRANSACTION]++;
#endif

					// 세션 생성 이벤트
					clanserver->OnClientJoin(newSession->uiId);

					if (CreateIoCompletionPort((HANDLE)clientSock, clanserver->m_IOCP, (ULONG_PTR)newSession, 0) == NULL) {
#if defined(CLANSERVER_ASSERT)
						DebugBreak();
#else
						shutdown(clientSock, SD_BOTH);
						closesocket(clientSock);
						continue;
#endif
					}

					// WSARecv 
					WSABUF wsabuf;
					wsabuf.buf = (CHAR*)newSession->recvRingBuffer.GetEnqueueBufferPtr();
					wsabuf.len = newSession->recvRingBuffer.GetFreeSize();
					// Zero byte recv 테스트
					//wsabuf.len = 0;
					DWORD dwFlag = 0;

					//newSession->ioCnt = 1;
					// => 세션 Release 관련 수업(24.04.08) 참고
					// 세션 Init 함수에서 IOCnt를 1로 초기화하는 것이 맞는듯..
					if (WSARecv(newSession->sock, &wsabuf, 1, NULL, &dwFlag, &newSession->recvOverlapped, NULL) == SOCKET_ERROR) {
						int errcode = WSAGetLastError();
						if (errcode != WSA_IO_PENDING) {
							clanserver->Disconnect(newSession->uiId);
						}
					}
				}
				else {
#if defined(CLANSERVER_ASSERT)
					DebugBreak();
#else
					shutdown(clientSock, SD_BOTH);
					closesocket(clientSock);
					continue;
#endif
				}
			}
		}
		else {
			break;
		}
	}
	return 0;
}

UINT __stdcall CLanServer::WorkerThreadFunc(void* arg)
{
	CLanServer* clanserver = (CLanServer*)arg;
	if (!clanserver->m_WorkerThreadStartFlag[GetThreadId(GetCurrentThread())]) {
		return 0;
	}

	clanserver->OnWorkerThreadStart();

#if defined(ALLOC_BY_TLS_MEM_POOL)
	clanserver->m_SerialBuffPoolMgr.AllocTlsMemPool(clanserver->m_TlsMemPoolDefaultUnitCnt, clanserver->m_TlsMemPoolDefaultUnitCapacity, clanserver->m_SerialBufferSize);	// 생성자에서 설정한 Default 값을 따름
#endif

	while (true) {
		DWORD transferred = 0;
		ULONG_PTR completionKey;
		WSAOVERLAPPED* overlappedPtr;
		GetQueuedCompletionStatus(clanserver->m_IOCP, &transferred, (PULONG_PTR)&completionKey, &overlappedPtr, INFINITE);
		// transffered == 0으로 먼저 분기를 나누지 않은 이유는? transferred와 session(lpCompletionKey)에 대한 초기화를 매번 진행하고 싶지 않아서 
		if (overlappedPtr != NULL) {
			/////////////////////////////////////////////////////////////////////////////////////////////
			// [완료 통지] IOCP_COMPLTED_LPOVERLAPPED_DISCONNECT: iocp 작업자 스레드 외 스레드의 Disconnect 요청 수행
			/////////////////////////////////////////////////////////////////////////////////////////////
			if (overlappedPtr == (LPOVERLAPPED)IOCP_COMPLTED_LPOVERLAPPED_DISCONNECT) {
				// 연결 종료 판단
				stCLanSession* session = (stCLanSession*)completionKey;

				// 연결 종료 판단
				uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
				stSessionRef sessionRef = *((stSessionRef*)&uiRef);

				if (sessionRef.ioCnt < 0) {
#if defined(CLANSERVER_ASSERT)
					DebugBreak();
#else
					InterlockedIncrement((uint32*)&session->sessionRef);
					continue;
#endif
				}
				if (sessionRef.ioCnt == 0) {
					// 세션 제거...
					uint64 sessionID = session->uiId;
					if (clanserver->DeleteSession(sessionID)) {
						clanserver->OnClientLeave(sessionID);
					}
				}
			}

			/////////////////////////////////////////////////////////////////////////////////////////////
			// [완료 통지] IOCP_COMPLTED_LPOVERLAPPED_SENDPOST_REQ: SendPostRequest를 통해 큐잉된 송신 요청
			/////////////////////////////////////////////////////////////////////////////////////////////
			else if (overlappedPtr == (LPOVERLAPPED)IOCP_COMPLTED_LPOVERLAPPED_SENDPOST_REQ) {
				stCLanSession* session = (stCLanSession*)completionKey;
				// IOCP 작업자 스레드 측에서 SendPost 호출
				clanserver->SendPost(session->uiId, true);
				
				uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
				stSessionRef sessionRef = *((stSessionRef*)&uiRef);
				if (sessionRef.ioCnt < 0) {
#if defined(CLANSERVER_ASSERT)
					DebugBreak();
#else
					InterlockedIncrement((uint32*)&session->sessionRef);
					continue;
#endif
				}

				if (sessionRef.ioCnt == 0) {
					// 세션 연결 종료 판단
					uint64 sessionID = session->uiId;
					if (clanserver->DeleteSession(sessionID)) {
						clanserver->OnClientLeave(sessionID);
					}
				}

			}
			else if (transferred == 0) {
				// 연결 종료 판단
				stCLanSession* session = (stCLanSession*)completionKey;

				uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
				stSessionRef sessionRef = *((stSessionRef*)&uiRef);
				if (sessionRef.ioCnt < 0) {
#if defined(CLANSERVER_ASSERT)
					DebugBreak();
#else
					InterlockedIncrement((uint32*)&session->sessionRef);
					continue;
#endif
				}

				if(sessionRef.ioCnt == 0) {
					// 세션 제거...
					uint64 sessionID = session->uiId;
					if (clanserver->DeleteSession(sessionID)) {
						clanserver->OnClientLeave(sessionID);
					}
				}
			}
			else {
				//////////////////////////////////////////////////////////////////////////////
				// [완료 통지] 수신 완료
				//////////////////////////////////////////////////////////////////////////////
				stCLanSession* session = (stCLanSession*)completionKey;
				if (&(session->recvOverlapped) == overlappedPtr) {
					session->recvRingBuffer.DirectMoveEnqueueOffset(transferred);
					UINT recvBuffSize = session->recvRingBuffer.GetUseSize();
					if (recvBuffSize > clanserver->m_MaxOfRecvBufferSize) {
						clanserver->m_MaxOfRecvBufferSize = recvBuffSize;
					}

					//clanserver->OnRecv(session->uiId, session->recvRingBuffer);	// OnRecv 함수에서는 에코 송신을 수행한다. 
					// => 공통 헤더 라이브러리 단으로 이동, 디코딩 수행
					clanserver->ProcessReceiveMessage(session->uiId, session->recvRingBuffer);

					session->clearRecvOverlapped();
					WSABUF wsabuf;
					wsabuf.buf = (CHAR*)session->recvRingBuffer.GetEnqueueBufferPtr();
					wsabuf.len = session->recvRingBuffer.GetDirectEnqueueSize();
					DWORD dwflag = 0;

					if (wsabuf.len == 0) {
#if defined(CLANSERVER_ASSERT)
						// 0 바이트 수신 요청이 발생하는지 확인
						DebugBreak();
#else
						// 임시 조치 (불완전)
						session->recvRingBuffer.ClearBuffer();
						wsabuf.buf = (CHAR*)session->recvRingBuffer.GetEnqueueBufferPtr();
						wsabuf.len = session->recvRingBuffer.GetDirectEnqueueSize();
#endif
					}

					if (WSARecv(session->sock, &wsabuf, 1, NULL, &dwflag, &session->recvOverlapped, NULL) == SOCKET_ERROR) {
						int errcode = WSAGetLastError();
						if (errcode != WSA_IO_PENDING) {
							uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
							stSessionRef sessionRef = *((stSessionRef*)&uiRef);

							assert(sessionRef.ioCnt >= 0);
							if(sessionRef.ioCnt == 0) {
								// 세션 삭제
								uint64 sessionID = session->uiId;
								if (clanserver->DeleteSession(sessionID)) {
									clanserver->OnClientLeave(sessionID);
								}
							}
						}
					}
				}
				//////////////////////////////////////////////////////////////////////////////
				// [완료 통지] 송신 완료
				//////////////////////////////////////////////////////////////////////////////
				else if (&(session->sendOverlapped) == overlappedPtr) {
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
					for (int i = 0; i < session->sendOverlapped.Offset; i++) {
						JBuffer* sendBuff = session->sendPostedQueue[i];
						session->sendPostedQueue[i] = NULL;
						
						clanserver->m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendBuff);
					}
#else
					// 송신 완료된 직렬화 버퍼 디큐잉 및 메모리 반환
					AcquireSRWLockExclusive(&session->sendBuffSRWLock);
					for (int i = 0; i < session->sendOverlapped.Offset; i++) {
						JBuffer* sendBuff;
						session->sendRingBuffer >> sendBuff;
						//clanserver->m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendBuff, to_string(session->uiId) + ", FreeMem (송신 완료)");
						clanserver->m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendBuff);
					}
					ReleaseSRWLockExclusive(&session->sendBuffSRWLock);
#endif
#else 
					// 송신 완료된 직렬화 버퍼 디큐잉 및 메모리 반환
					AcquireSRWLockExclusive(&session->sendBuffSRWLock);
					for (int i = 0; i < session->sendOverlapped.Offset; i++) {
						session->sendBufferVector.erase(session->sendBufferVector.begin());
					}
					ReleaseSRWLockExclusive(&session->sendBuffSRWLock);
#endif
					
					//uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
					//stSessionRef sessionRef = *((stSessionRef*)&uiRef);
					//assert(sessionRef.ioCnt >= 0);
					//if (sessionRef.ioCnt == 0) {
					//	// 세션 연결 종료 판단
					//	uint64 sessionID = session->uiId;
					//	if (clanserver->DeleteSession(sessionID)) {
					//		clanserver->OnClientLeave(sessionID);
					//	}
					//}
					//else {
					//	// 세션 연결 유지 -> 추가적인 송신 데이터 존재 시 SendPost 호출
					//	if (session->sendBufferQueue.GetSize() > 0) {
					//		clanserver->SendPost(session->uiId, true);
					//	}
					//	else {
					//		InterlockedExchange(&session->sendFlag, 0);
					//	}
					//}
					// => 송신 버퍼 큐의 사이즈 확인과 sendFlag 변경이 원자적으로 수행되지 않는 문제 식별
					// ex) 송신 버퍼 큐 사이즈 == 0 임을 확인한 시점 이후, 다른 스레드에서 송신 버퍼 인큐. 하지만 sendFlag는 아직 On이기에 PostQueue 수행 x
					//     송신 완료 처리 로직이 복귀되면 송신 버퍼 큐에 송신 패킷이 존재함에도 불구하고 송신하지 못하는 상황 발생
					// => 기존 방식대로 sendFlag를 먼저 off하는 방식이 필요

					// sendFlag Off
					InterlockedExchange(&session->sendFlag, 0);

					// 세션 연결 유지 -> 추가적인 송신 데이터 존재 시 SendPost 호출
					bool sendAgainFlag = false;
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_SEND_QUEUE)
					if (session->sendBufferQueue.GetSize() > 0) {
						sendAgainFlag = true;
					}
#else
					AcquireSRWLockShared(&session->sendBuffSRWLock);
					if (session->sendRingBuffer.GetUseSize() >= sizeof(UINT_PTR)) {
						sendAgainFlag = true;
					}
					ReleaseSRWLockShared(&session->sendBuffSRWLock);
#endif
#else
					AcquireSRWLockShared(&session->sendBuffSRWLock);
					if (session->sendBufferVector.size() > 0) {
						sendAgainFlag = true;
					}
					ReleaseSRWLockShared(&session->sendBuffSRWLock);
#endif

					if (sendAgainFlag) {
						clanserver->SendPost(session->uiId);

					}

					// SendPost에서는 세션 유효성 판단을 하지 않는다. 따라서 ioCnt >= 1을 보장한 상태에서 호출한 후 송신 완료 후반부에 ioCnt를 감소시켜야 함.
					uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
					stSessionRef sessionRef = *((stSessionRef*)&uiRef);
					assert(sessionRef.ioCnt >= 0);
					if (sessionRef.ioCnt == 0) {
						// 세션 연결 종료 판단
						uint64 sessionID = session->uiId;
						if (clanserver->DeleteSession(sessionID)) {
							clanserver->OnClientLeave(sessionID);
						}
					}
				}
#if defined(CLANSERVER_ASSERT)
				else {
					// 수신 완료도 송신 완료도 아닌 비정상적 상황
					DebugBreak();
				}
#endif
			}
		}
#if defined(CLANSERVER_ASSERT)
		else {
			// 1. IOCP 객체 자체의 문제
			// 2. GQCS 호출 시 INIFINTE 외 대기 시간을 걸어 놓은 경우, 대기 시간 내 I/O가 완료되지 않은 경우
			DebugBreak();
		}
#endif
	}

	clanserver->OnWorkerThreadEnd();

	return 0;
}

#if defined(CALCULATE_TRANSACTION_PER_SECOND)
UINT __stdcall CLanServer::CalcTpsThreadFunc(void* arg)
{
	CLanServer* clanserver = (CLanServer*)arg;

	for (int i = 0; i < NUM_OF_TPS_ITEM; i++) {
		clanserver->m_CalcTpsItems[i] = 0;
		clanserver->m_TpsItems[i] = 0;
		clanserver->m_TotalTransaction[i] = 0;
	}

	while (true) {
		// TPS 항목 읽기
		for (int i = 0; i < NUM_OF_TPS_ITEM; i++) {
			clanserver->m_TpsItems[i] = clanserver->m_CalcTpsItems[i];
			//clanserver->m_TotalTransaction[i] += clanserver->m_CalcTpsItems[i];
		}

		// TPS 항목 전송

		// TPS 항목 초기화
		for (int i = 0; i < NUM_OF_TPS_ITEM; i++) {
			clanserver->m_CalcTpsItems[i] = 0;
		}
		Sleep(1000);
	}
	return 0;
}
#endif

bool CLanServer::ProcessReceiveMessage(UINT64 sessionID, JBuffer& recvRingBuffer)
{
#if defined(ON_RECV_BUFFERING)
	std::queue<JBuffer> bufferedQueue;
	size_t recvDataLen = 0;
	while (recvRingBuffer.GetUseSize() >= sizeof(stMSG_HDR)) {
		//stMSG_HDR* hdr = (stMSG_HDR*)recvRingBuffer.GetDequeueBufferPtr();
		// => 링버퍼를 참조하기에 헤더가 링버퍼 시작과 끝 지점에 걸쳐 있을 수 있음. Decode시 에러 식별(checkSum 에러)
		//	  따라서 복사 비용이 들긴 하지만 Peek을 하는 것이 안전
		stMSG_HDR hdr;
		recvRingBuffer.Peek(&hdr);
		if (hdr.code != dfPACKET_CODE) {
			DebugBreak();
			return false;
		}
		if (recvRingBuffer.GetUseSize() < sizeof(stMSG_HDR) + hdr.len) {
			break;
		}
		recvRingBuffer.DirectMoveDequeueOffset(sizeof(stMSG_HDR));

		if (!Decode(hdr.randKey, hdr.len, hdr.checkSum, recvRingBuffer)) {
			DebugBreak();
			return false;
		}

		JSerBuffer recvBuff(recvRingBuffer, hdr.len, true);
		bufferedQueue.push(recvBuff);
		recvDataLen += hdr.len;
		recvRingBuffer.DirectMoveDequeueOffset(hdr.len);
	}
	OnRecv(sessionID, bufferedQueue, recvDataLen);
#else
	while (recvRingBuffer.GetUseSize() >= sizeof(stMSG_HDR)) {
		//stMSG_HDR* hdr = (stMSG_HDR*)recvRingBuffer.GetDequeueBufferPtr();
		// => 링버퍼를 참조하기에 헤더가 링버퍼 시작과 끝 지점에 걸쳐 있을 수 있음. Decode시 에러 식별(checkSum 에러)
		//	  따라서 복사 비용이 들긴 하지만 Peek을 하는 것이 안전
		stMSG_HDR hdr;
		recvRingBuffer.Peek(&hdr);
		if (hdr.code != dfPACKET_CODE) {
#if defined(CLANSERVER_ASSERT)
			DebugBreak();
#endif
			return false;
		}
		if (recvRingBuffer.GetUseSize() < sizeof(stMSG_HDR) + hdr.len) {
			break;
		}
		recvRingBuffer.DirectMoveDequeueOffset(sizeof(stMSG_HDR));

		if (!Decode(hdr.randKey, hdr.len, hdr.checkSum, recvRingBuffer)) {
#if defined(CLANSERVER_ASSERT)
			DebugBreak();
#endif
			return false;
		}

		JSerBuffer recvBuff(recvRingBuffer, hdr.len, true);
		OnRecv(sessionID, recvBuff);

		recvRingBuffer.DirectMoveDequeueOffset(hdr.len);
	}
#endif
	if (recvRingBuffer.GetUseSize() == 0) {
		recvRingBuffer.ClearBuffer();
	}

	return true;
}

void CLanServer::Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads) {
	BYTE payloadSum = 0;
	for (USHORT i = 0; i < payloadLen; i++) {
		payloadSum += payloads[i];
		payloadSum %= 256;
	}
	BYTE Pb = payloadSum ^ (randKey + 1);
	BYTE Eb = Pb ^ (dfPACKET_KEY + 1);
	checkSum = Eb;

	for (USHORT i = 1; i <= payloadLen; i++) {
		//BYTE Pn = payloads[i - 1] ^ (Pb + randKey + (BYTE)(i + 1));
		//BYTE En = Pn ^ (Eb + dfPACKET_KEY + (BYTE)(i + 1));
		BYTE Pn = payloads[i - 1] ^ (Pb + randKey + i + 1);
		BYTE En = Pn ^ (Eb + dfPACKET_KEY + i + 1);

		payloads[i - 1] = En;

		Pb = Pn;
		Eb = En;
	}
}
bool CLanServer::Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, BYTE* payloads) {
	BYTE Pb = checkSum ^ (dfPACKET_KEY + 1);
	BYTE payloadSum = Pb ^ (randKey + 1);
	BYTE Eb = checkSum;
	BYTE Pn;
	BYTE Dn;
	BYTE payloadSumCmp = 0;

	for (USHORT i = 1; i <= payloadLen; i++) {
		//Pn = payloads[i - 1] ^ (Eb + dfPACKET_KEY + (BYTE)(i + 1));
		//Dn = Pn ^ (Pb + randKey + (BYTE)(i + 1));
		Pn = payloads[i - 1] ^ (Eb + dfPACKET_KEY + i + 1);
		Dn = Pn ^ (Pb + randKey + i + 1);

		Pb = Pn;
		Eb = payloads[i - 1];
		payloads[i - 1] = Dn;
		payloadSumCmp += payloads[i - 1];
		payloadSumCmp %= 256;
	}

	if (payloadSum != payloadSumCmp) {
#if defined(CLANSERVER_ASSERT)
		DebugBreak();
#endif
		return false;
	}

	return true;
}
bool CLanServer::Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, JBuffer& ringPayloads) {
	if (ringPayloads.GetDirectDequeueSize() >= payloadLen) {
		return Decode(randKey, payloadLen, checkSum, ringPayloads.GetDequeueBufferPtr());
	}
	else {
		BYTE Pb = checkSum ^ (dfPACKET_KEY + 1);
		BYTE payloadSum = Pb ^ (randKey + 1);
		BYTE Eb = checkSum;
		BYTE Pn, Dn;
		BYTE payloadSumCmp = 0;

		UINT offset = ringPayloads.GetDeqOffset();
		BYTE* bytepayloads = ringPayloads.GetBeginBufferPtr();
		for (USHORT i = 1; i <= payloadLen; i++, offset++) {
			offset = offset % (ringPayloads.GetBufferSize() + 1);
			//Pn = bytepayloads[offset] ^ (Eb + dfPACKET_KEY + (BYTE)(i + 1));
			//Dn = Pn ^ (Pb + randKey + (BYTE)(i + 1));
			Pn = bytepayloads[offset] ^ (Eb + dfPACKET_KEY + i + 1);
			Dn = Pn ^ (Pb + randKey + i + 1);

			Pb = Pn;
			Eb = bytepayloads[offset];
			bytepayloads[offset] = Dn;
			payloadSumCmp += bytepayloads[offset];
			payloadSumCmp %= 256;
		}
		
		if (payloadSum != payloadSumCmp) {
#if defined(CLANSERVER_ASSERT)
			DebugBreak();
#endif
			return false;
		}

		return true;
	}
}

void CLanServer::ConsoleLog()
{
	system("cls");   // 콘솔 창 지우기

	static size_t logCnt = 0;

	static COORD coord;
	coord.X = 0;
	coord.Y = 0;
	SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);

	std::cout << "================ SERVER CORE CONSOLE MONT ================ " << std::endl;
	// Accept TPS
	std::cout << "[Accept] Total Accept Count : " << m_TotalAccept << std::endl;
	std::cout << "[Accept] Accept TPS         : " << GetAndResetAcceptTransaction() << std::endl;
	std::cout << "----------------------------------------------------------" << std::endl;
	std::cout << "[Session] Session acceptance limit               : " << m_MaxOfSessions << std::endl;
	std::cout << "[Session] Current number of sessions             : " << GetSessionCount() << std::endl;
	std::cout << "[Session] Number of Acceptances Available        : " << m_SessionAllocIdQueue.size() << std::endl;
	std::cout << "[Session Buffer] Max of Recv Ring Buffer Size    : " << m_MaxOfRecvBufferSize << " Bytes" << std::endl;
	std::cout << "[Session Buffer] Max of Serial Send Buffer Count : " << m_MaxOfBufferedSerialSendBufferCnt << std::endl;
	std::cout << "----------------------------------------------------------" << std::endl;
#if defined(ALLOC_BY_TLS_MEM_POOL)
	// TLS 메모리 풀
	UINT64 totalAllocMemCnt = m_SerialBuffPoolMgr.GetTotalAllocMemCnt();
	UINT64 totalFreeMemCnt = m_SerialBuffPoolMgr.GetTotalFreeMemCnt();
	INT64 allocatedUnitCnt = m_SerialBuffPoolMgr.GetAllocatedMemUnitCnt();
	UINT64 totalInjectedMemCnt = m_SerialBuffPoolMgr.GetMallocCount();
	std::cout << "[Memory Pool] Total Alloc Count           : " << totalAllocMemCnt << std::endl;
	std::cout << "[Memory Pool] Total Free Count            : " << totalFreeMemCnt << std::endl;
	std::cout << "[Memory Pool] Allocated Mem Count         : " << allocatedUnitCnt << std::endl;
	std::cout << "[Memory Pool] Total Malloc Mem Unit Count : " << totalInjectedMemCnt << std::endl;
	std::cout << "[Memory Pool] Total Malloc Mem Unit Size  : " << totalInjectedMemCnt * m_SerialBufferSize / 1024 << " KBytes" << std::endl;
	std::cout << "----------------------------------------------------------" << std::endl;
#endif

	ServerConsoleLog();
	std::cout << "========================================================== " << std::endl;
}

#if defined(ALLOC_MEM_LOG)
void CLanServer::MemAllocLog()
{
	time_t now = time(0);
	struct tm timeinfo;
	char buffer[80];
	localtime_s(&timeinfo, &now);
	strftime(buffer, sizeof(buffer), "MemAllocLog-%Y-%m-%d_%H-%M-%S", &timeinfo);
	std::string currentDateTime = std::string(buffer);

	// 파일 경로 생성
	std::string filePath = "./" + currentDateTime + ".txt";

	// 파일 스트림 열기
	std::ofstream outputFile(filePath);

	if (!outputFile) {
		std::cerr << "파일을 열 수 없습니다." << std::endl;
		return;
	}

	outputFile << currentDateTime << std::endl;

	std::vector<stAllocMemLog>& allocMemLog = m_SerialBuffPoolMgr.m_AllocLog;
	std::map<UINT_PTR, short>& allocLogMap = m_SerialBuffPoolMgr.m_AllocMap;
	
	for (auto iter = allocLogMap.begin(); iter != allocLogMap.end(); iter++) {
		outputFile << "address: " << iter->first << " | refCnt: " << iter->second << std::endl;
	}
	outputFile << "-----------------------------------------------------------------" << std::endl;

	//for (USHORT i = 0; i < allocMemLog.size(); i++) {
	//for(USHORT i = m_SerialBuffPoolMgr.m_AllocLogIndex; i <= USHRT_MAX; i++) {
	//	if (allocMemLog[i].address == 0) {
	//		break;
	//	}
	//	if (allocLogMap.find(allocMemLog[i].address) == allocLogMap.end()) {
	//		continue;
	//	}
	//
	//	outputFile << "address: " << allocMemLog[i].address << " | refCnt: " << allocMemLog[i].refCnt << std::endl;
	//	outputFile << "log: " << allocMemLog[i].log  << std::endl;
	//}
	for (USHORT i = 0; i < m_SerialBuffPoolMgr.m_AllocLogIndex; i++) {
		if (allocMemLog[i].address == 0) {
			break;
		}
		if (allocLogMap.find(allocMemLog[i].address) == allocLogMap.end()) {
			continue;
		}

		outputFile << "address: " << allocMemLog[i].address << " | refCnt: " << allocMemLog[i].refCnt << std::endl;
		outputFile << "log: " << allocMemLog[i].log << std::endl;
	}
	

	// 파일 닫기
	outputFile.close();

	std::cout << "파일이 생성되었습니다: " << filePath << std::endl;
}
#endif