#include "CLanServer.h"
#include <cassert>
#include <process.h>
#include <fstream>

CLanServer::CLanServer(const char* serverIP, uint16 serverPort, 
	DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections,
	bool tlsMemPoolReferenceFlag, bool tlsMemPoolPlacementNewFlag,
	size_t tlsMemPoolDefaultUnitCnt, size_t tlsMemPoolDefaultCapacity,
	uint32 sessionSendBuffSize, uint32 sessionRecvBuffSize,
	bool beNagle
)
#if defined(ALLOC_BY_TLS_MEM_POOL)
	: m_MaxOfSessions(maxOfConnections), m_Incremental(0), 
	m_NumOfWorkerThreads(numOfWorkerThreads), m_StopFlag(false), 
	m_SerialBuffPoolMgr(tlsMemPoolDefaultUnitCnt, tlsMemPoolDefaultCapacity, tlsMemPoolReferenceFlag, tlsMemPoolPlacementNewFlag)
#else
	: m_MaxOfSessions(maxOfConnections), m_Incremental(0), m_NumOfWorkerThreads(numOfWorkerThreads), m_StopFlag(false)
#endif
{
#if defined(SESSION_RELEASE_LOG)
	m_ReleaseLog.resize(USHRT_MAX + 1);
	m_ReleaseLogIndex = -1;
#endif

	//////////////////////////////////////////////////
	// 네트워크 초기화
	//////////////////////////////////////////////////
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


	//////////////////////////////////////////////////
	// 세션 관리 초기화
	//////////////////////////////////////////////////
	for (uint16 idx = 1; idx <= m_MaxOfSessions; idx++) {		// sessionID의 인덱스 부 값이 0이라는 것은 존재하지 않은 세션이라는 보초값
		m_SessionAllocIdQueue.push(idx);
	}
	m_Sessions.resize(m_MaxOfSessions + 1, NULL);
	for (uint16 idx = 1; idx <= m_MaxOfSessions; idx++) {
		m_Sessions[idx] = new stCLanSession;
	}
	InitializeCriticalSection(&m_SessionAllocIdQueueCS);
	InitializeCriticalSection(&m_SessionCS);

	//////////////////////////////////////////////////
	// IOCP 객체 초기화
	//////////////////////////////////////////////////
	m_IOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, numOfIocpConcurrentThrd);
	assert(m_IOCP != NULL);


	//////////////////////////////////////////////////
	// 스레드 관리 초기화
	//////////////////////////////////////////////////
	//m_ExitEvent = CreateEvent(0, TRUE, FALSE, NULL);

	if (m_NumOfWorkerThreads == 0) {
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		m_NumOfWorkerThreads = si.dwNumberOfProcessors;
	}
	m_WorkerThreads.resize(m_NumOfWorkerThreads, NULL);
	
}
CLanServer::~CLanServer()
{
	if (!m_StopFlag) {
		Stop();
	}
}

bool CLanServer::Start()
{
	if (BindSocket(m_ListenSock, m_ListenSockAddr) == SOCKET_ERROR) {
		return false;
	}
	if (ListenSocket(m_ListenSock, SOMAXCONN) == SOCKET_ERROR) {
		return false;
	}

	m_AcceptThread = (HANDLE)_beginthreadex(NULL, 0, CLanServer::AcceptThreadFunc, this, 0, NULL);
	cout << "[Start Thread] Accept Thread" << endl;
	for (uint16 idx = 0; idx < m_NumOfWorkerThreads; idx++) {
		m_WorkerThreads[idx] = (HANDLE)_beginthreadex(NULL, 0, CLanServer::WorkerThreadFunc, this, CREATE_SUSPENDED, NULL);
		DWORD thID = GetThreadId(m_WorkerThreads[idx]);
		m_WorkerThreadStartFlag.insert({thID, true});
		if (!OnWorkerThreadCreate(m_WorkerThreads[idx])) {
			m_WorkerThreadStartFlag[thID] = false;
			cout << "[Cant't Start Thread] Worker Thread (thID: " << GetThreadId(m_WorkerThreads[idx]) << ")" << endl;
		}
		else {
			cout << "[Start Thread] Worker Thread (thID: " << GetThreadId(m_WorkerThreads[idx]) << ")" << endl;
		}
		ResumeThread(m_WorkerThreads[idx]);
	}

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
	WaitForMultipleObjects(m_NumOfWorkerThreads, m_WorkerThreads.data(), TRUE, INFINITE);
	cout << "Exit Worker Threads..." << endl;

	for (size_t i = 0; i < m_Sessions.size(); i++) {
		if (m_Sessions[i]) {
			closesocket(m_Sessions[i]->sock);
		}
	}

	DeleteCriticalSection(&m_SessionAllocIdQueueCS);
	DeleteCriticalSection(&m_SessionCS);
	CloseHandle(m_IOCP);

	WSACleanup();
}


bool CLanServer::Disconnect(uint64 sessionID)
{
	//stCLanSession* session = AcquireSession(sessionID);

	return true;
}

bool CLanServer::SendPacket(uint64 sessionID, JBuffer& sendDataRef)
{
	//uint16 idx = (uint16)sessionID;
	//stCLanSession* session = m_Sessions[idx];
	stCLanSession* session = AcquireSession(sessionID);
	if (session != nullptr) {
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		//session->sendBuffMtx.lock();
		AcquireSRWLockExclusive(&session->sendBuffSRWLock);
#endif

#if defined(SEND_RECV_RING_BUFF_COPY_MODE)
		if (session->sendRingBuffer.GetFreeSize() < sendData.GetUseSize()) {
			// 송신 링-버퍼에 송신 데이터를 Enqueue(복사)할 여유 분이 없음(송신 버퍼 초과)
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터를 Enqueue할 여유 사이즈 없음" << endl;
			DebugBreak();
		}
		uint32 enqSize = session->sendRingBuffer.Enqueue(sendData.GetDequeueBufferPtr(), sendData.GetUseSize());
		if (enqSize < sendData.GetUseSize()) {
			// 송신 링-버퍼에 송신 데이터를 복사할 수 있음을 확인했음에도 불구하고,
			// Enqueue 사이즈가 송신 데이터의 크기보다 작은 상황 발생
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터 전체 Enqueue 실패" << endl;
			DebugBreak();
		}
#elif defined(SEND_RECV_RING_BUFF_SERIALIZATION_MODE)
		if (session->sendRingBuffer.GetFreeSize() < sizeof(UINT_PTR)) {
			// 송신 링-버퍼에 송신 데이터를 Enqueue(복사)할 여유 분이 없음(송신 버퍼 초과)
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터를 Enqueue할 여유 사이즈 없음" << endl;
			DebugBreak();
		}
		UINT_PTR sendDataPtr = (UINT_PTR)&sendDataRef;
		uint32 enqSize = session->sendRingBuffer.Enqueue((BYTE*)(&sendDataPtr), sizeof(UINT_PTR));
		if (enqSize < sizeof(UINT_PTR)) {
			// 송신 링-버퍼에 송신 데이터를 복사할 수 있음을 확인했음에도 불구하고,
			// Enqueue 사이즈가 송신 데이터의 크기보다 작은 상황 발생
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터 전체 Enqueue 실패" << endl;
			DebugBreak();
		}
#endif
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		//session->sendBuffMtx.unlock();
		ReleaseSRWLockExclusive(&session->sendBuffSRWLock);
#endif

		SendPost(sessionID);
	}
	else {
		return false;
	}

	ReturnSession(session);
	return true;
}
bool CLanServer::SendPacket(uint64 sessionID, JBuffer* sendDataPtr) {
#if defined(MT_FILE_LOG)
	USHORT logIdx = mtFileLogger.AllocLogIndex();
#endif

	//uint16 idx = (uint16)sessionID;
	//stCLanSession* session = m_Sessions[idx];
	stCLanSession* session = AcquireSession(sessionID);
	if (session != nullptr) {				// 인덱스가 동일한 다른 세션이거나 제거된(제거중인) 세션
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		//session->sendBuffMtx.lock();
		AcquireSRWLockExclusive(&session->sendBuffSRWLock);
#endif

#if defined(SEND_RECV_RING_BUFF_COPY_MODE)
		if (session->sendRingBuffer.GetFreeSize() < sendData->GetUseSize()) {
			// 송신 링-버퍼에 송신 데이터를 Enqueue(복사)할 여유 분이 없음(송신 버퍼 초과)
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터를 Enqueue할 여유 사이즈 없음" << endl;
			DebugBreak();
		}
		uint32 enqSize = session->sendRingBuffer.Enqueue(sendData->GetDequeueBufferPtr(), sendData->GetUseSize());
		if (enqSize < sendData->GetUseSize()) {
			// 송신 링-버퍼에 송신 데이터를 복사할 수 있음을 확인했음에도 불구하고,
			// Enqueue 사이즈가 송신 데이터의 크기보다 작은 상황 발생
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터 전체 Enqueue 실패" << endl;
			DebugBreak();
		}
#elif defined(SEND_RECV_RING_BUFF_SERIALIZATION_MODE)
		if (session->sendRingBuffer.GetFreeSize() < sizeof(UINT_PTR)) {
			// 송신 링-버퍼에 송신 데이터를 Enqueue(복사)할 여유 분이 없음(송신 버퍼 초과)
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터를 Enqueue할 여유 사이즈 없음" << endl;
			DebugBreak();
		}
		uint32 enqSize = session->sendRingBuffer.Enqueue((BYTE*)&sendDataPtr, sizeof(UINT_PTR));
#if defined(MT_FILE_LOG)
		{
			mtFileLogger.GetLogStruct(logIdx).ptr0 = 4;				// SendPacekt
			mtFileLogger.GetLogStruct(logIdx).ptr1 = (UINT_PTR)sendData;
			mtFileLogger.GetLogStruct(logIdx).ptr2 = sendData->GetUseSize();
			mtFileLogger.GetLogStruct(logIdx).ptr4 = session->sendRingBuffer.GetUseSize();
			mtFileLogger.GetLogStruct(logIdx).ptr5 = session->sendRingBuffer.GetEnqOffset();
			mtFileLogger.GetLogStruct(logIdx).ptr6 = session->sendRingBuffer.GetDeqOffset();
		}
#endif
		if (enqSize < sizeof(UINT_PTR)) {
			// 송신 링-버퍼에 송신 데이터를 복사할 수 있음을 확인했음에도 불구하고,
			// Enqueue 사이즈가 송신 데이터의 크기보다 작은 상황 발생
			cout << "[ERROR, SendPacket] 송신 링-버퍼에 송신 데이터 전체 Enqueue 실패" << endl;
			DebugBreak();
		}
#endif
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		//session->sendBuffMtx.unlock();
		ReleaseSRWLockExclusive(&session->sendBuffSRWLock);
#endif

		SendPost(sessionID);
	}
	else {
		return false;
	}

	ReturnSession(session);

	return true;
}

CLanServer::stCLanSession* CLanServer::AcquireSession(uint64 sessionID)
{
	uint16 idx = (uint16)sessionID;								
	stCLanSession* session = m_Sessions[idx];					// 세션 ID의 인덱스 파트를 통해 세션 획득
	if (session == nullptr) {									// AcquireSession을 호출하는 시점에서 찾고자 하였던 세션을 획득하였다는 보장은 할 수 없음
		DebugBreak();											// (이미 삭제된 세션이거나, 삭제된 후 같은 인덱스 자리에 재활용된 세션일 수 있음)
	}

	InterlockedIncrement((uint32*)&session->sessionRef);		// 세션 IOCnt 증가
																// 세션 IOCnt를 증가한 시점 이후에는 AcquireSession을 호출하면서 찾고자 하였던 세션이든,
																// 같은 인덱스 자리에 재활용된 세션이든 삭제되지 않는 보장을 할 수 있음
	
	if (session->uiId != sessionID) {							// 찾고자 하였던 세션인지 확인, 아닌 경우 증가 시켰던 IOCnt를 감소 시키고 nullptr 반환
		//ReturnSession(session);

		InterlockedDecrement((uint32*)&session->sessionRef);
		assert(session->sessionRef.ioCnt >= 0);
		if (session->sessionRef.ioCnt == 0) {
			DeleteSession(session->uiId, "AcquireSession" + to_string(sessionID) + " -(삭제)-> " + to_string(session->uiId));
		}
		return nullptr;
	}

	if (session->sessionRef.releaseFlag == 1) {					// 삭제가 진행 중인 세션이거나 이미 삭제된 세션이라면 nullptr 반환
		return nullptr;
	}

	return session;
}

void CLanServer::ReturnSession(stCLanSession* session)
{
	InterlockedDecrement((uint32*)&session->sessionRef);
	assert(session->sessionRef.ioCnt >= 0);
	if (session->sessionRef.ioCnt == 0) {					// 찾고자 하던 세션이 아닌 다른 세션이 ioCnt가 0이 된다면?!
#if defined(SESSION_RELEASE_LOG)
		DeleteSession(session->uiId, "ReturnSession");
#else
		DeleteSession(session->uiId);
#endif
	}
}

void CLanServer::SendPost(uint64 sessionID)
{
	uint16 idx = (uint16)sessionID;
	stCLanSession* session = m_Sessions[idx];
	if (session == nullptr) {
		DebugBreak();
	}

	if (InterlockedExchange(&session->sendFlag, 1) == 0) {
#if defined(MT_FILE_LOG)
		USHORT logIdx = mtFileLogger.AllocLogIndex();
#endif
		session->clearSendOverlapped();	// 송신용 overlapped 구조체 초기화

#if defined(SEND_RECV_RING_BUFF_COPY_MODE)
		WSABUF wsabuf;
		wsabuf.buf = (CHAR*)session->sendRingBuffer.GetDequeueBufferPtr();
		wsabuf.len = session->sendRingBuffer.GetDirectDequeueSize();
		if (wsabuf.len > 0) {
			InterlockedIncrement(&session->ioCnt);
			if (WSASend(session->sock, &wsabuf, 1, NULL, 0, &session->sendOverlapped, NULL) == SOCKET_ERROR) {
				int errcode = WSAGetLastError();
				if (errcode != WSA_IO_PENDING) {
					InterlockedDecrement(&session->ioCnt);
					if (session->ioCnt == 0) {
						DeleteSession(session);
						OnClientLeave(session->uiId);
					}
				}
			}
		}
		else {
			InterlockedExchange(&session->sendFlag, 0);
		}
#elif defined(SEND_RECV_RING_BUFF_SERIALIZATION_MODE)


#if defined(SESSION_SENDBUFF_SYNC_TEST)
		AcquireSRWLockShared(&session->sendBuffSRWLock);
#endif
		DWORD numOfMessages = session->sendRingBuffer.GetUseSize() / sizeof(UINT_PTR);
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		ReleaseSRWLockShared(&session->sendBuffSRWLock);
#endif

		WSABUF wsabuffs[WSABUF_ARRAY_DEFAULT_SIZE];

		if (numOfMessages > 0) {
			//InterlockedIncrement(&session->ioCnt);
			InterlockedIncrement((uint32*)&session->sessionRef);

			int sendLimit = min(numOfMessages, WSABUF_ARRAY_DEFAULT_SIZE);
			for (int idx = 0; idx < sendLimit; idx++) {
				JBuffer* msgPtr;
				//session->sendRingBuffer.Dequeue((BYTE*)&msgPtr, sizeof(UINT_PTR));
				session->sendRingBuffer.Peek(sizeof(UINT_PTR) * idx, (BYTE*)&msgPtr, sizeof(UINT_PTR));
				wsabuffs[idx].buf = (CHAR*)msgPtr->GetBeginBufferPtr();
				wsabuffs[idx].len = msgPtr->GetUseSize();
				if (wsabuffs[idx].buf == NULL || wsabuffs[idx].len == 0) {
					DebugBreak();
				}

#if defined(MT_FILE_LOG)
				{
					mtFileLogger.GetLogStruct(logIdx).ptr0 = 3;						// SendPost
					mtFileLogger.GetLogStruct(logIdx).ptr1 = (UINT_PTR)msgPtr;
					mtFileLogger.GetLogStruct(logIdx).ptr2 = wsabuffs[idx].len;
					mtFileLogger.GetLogStruct(logIdx).ptr3 = sendLimit;
					mtFileLogger.GetLogStruct(logIdx).ptr4 = session->sendRingBuffer.GetUseSize();
					mtFileLogger.GetLogStruct(logIdx).ptr5 = session->sendRingBuffer.GetEnqOffset();
					mtFileLogger.GetLogStruct(logIdx).ptr6 = session->sendRingBuffer.GetDeqOffset();
				}
#endif
			}

			session->sendOverlapped.Offset = sendLimit;	// Offset 멤버를 활용해보면 어떨까?
															// 송신한 메시지 갯수를 담도록 한다.

			if (WSASend(session->sock, wsabuffs, sendLimit, NULL, 0, &session->sendOverlapped, NULL) == SOCKET_ERROR) {
				int errcode = WSAGetLastError();
				if (errcode != WSA_IO_PENDING) {
					//InterlockedDecrement(&session->ioCnt);
					InterlockedDecrement((uint32*)&session->sessionRef);

					assert(session->sessionRef.ioCnt >= 0);
					if(session->sessionRef.ioCnt == 0) {
#if defined(SESSION_RELEASE_LOG)
						DeleteSession(sessionID, "WSASend 실패");
#else
						DeleteSession(sessionID);
#endif
					}
				}
			}
		}
		else {
			InterlockedExchange(&session->sendFlag, 0);
		}
#endif
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

#if defined(SESSION_RELEASE_LOG)
		m_CreatedSessionMtx.lock();
		m_CreatedSession.insert(newSession->uiId);
		m_CreatedSessionMtx.unlock();
#endif

	}
	LeaveCriticalSection(&m_SessionAllocIdQueueCS);

	return newSession;
}

//void CLanServer::DeleteSession(stCLanSession* delSession)
//{
//	// 세션 삭제
//	uint16 allocatedIdx = delSession->Id.idx;
//	closesocket(m_Sessions[allocatedIdx]->sock);
//	//delete delSession;
//	//m_Sessions[allocatedIdx] = NULL;
//
//	EnterCriticalSection(&m_SessionAllocIdQueueCS);
//	m_SessionAllocIdQueue.push(allocatedIdx);
//	LeaveCriticalSection(&m_SessionAllocIdQueueCS);
//}

#if defined(SESSION_RELEASE_LOG)
void CLanServer::DeleteSession(uint64 sessionID, string log) {
	USHORT releaseLogIdx = InterlockedIncrement16((short*)&m_ReleaseLogIndex);

	uint16 idx = (uint16)sessionID;
	stCLanSession* delSession = m_Sessions[idx];
	if (delSession == nullptr) {
		DebugBreak();
		return;
	}

	m_ReleaseLog[releaseLogIdx].sessionID = sessionID;
	m_ReleaseLog[releaseLogIdx].sessionIndex = delSession->Id.idx;
	m_ReleaseLog[releaseLogIdx].sessionIncrement = delSession->Id.incremental;

	uint32 chg = 0;
	((stSessionRef*)(&chg))->releaseFlag = 1;

	uint32 org = InterlockedCompareExchange((uint32*)&delSession->sessionRef, chg, 0);
	if (org == 0) {
		// 세션 삭제 t성공
		m_ReleaseLog[releaseLogIdx].releaseFlag = true;
		m_CreatedSessionMtx.lock();
		m_CreatedSession.erase(sessionID);
		m_CreatedSessionMtx.unlock();

		// 세션 삭제
		uint16 allocatedIdx = delSession->Id.idx;
		closesocket(m_Sessions[allocatedIdx]->sock);

		// 세션 송신 큐에 존재하는 송신 직렬화 버퍼 메모리 반환
		while (delSession->sendRingBuffer.GetUseSize() >= sizeof(JBuffer*)) {
			JBuffer* sendPacekt;
			delSession->sendRingBuffer >> sendPacekt;
#if defined(ALLOC_MEM_LOG)
			m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendPacekt, to_string(sessionID) + ", FreeMem (DeleteSession)");
#endif
		}

		EnterCriticalSection(&m_SessionAllocIdQueueCS);
		m_SessionAllocIdQueue.push(allocatedIdx);
		LeaveCriticalSection(&m_SessionAllocIdQueueCS);

		delSession->Id.idx = 0;
	}
	else {
		// 세션 삭제 실패
		m_ReleaseLog[releaseLogIdx].releaseFlag = false;
	}

	m_ReleaseLog[releaseLogIdx].iocnt = ((stSessionRef*)(&org))->ioCnt;
	m_ReleaseLog[releaseLogIdx].releaseFlag = ((stSessionRef*)(&org))->releaseFlag;
	m_ReleaseLog[releaseLogIdx].log = log;

	OnClientLeave(sessionID);
}
#else
void CLanServer::DeleteSession(uint64 sessionID)
{
	uint16 idx = (uint16)sessionID;
	stCLanSession* delSession = m_Sessions[idx];
	if (delSession == nullptr) {
		return;
	}

	uint32 chg = 0;
	((stSessionRef*)(&chg))->releaseFlag = 1;

	uint32 org = InterlockedCompareExchange((uint32*)&delSession->sessionRef, chg, 0);
	if (org == 0) {
		// 세션 삭제
		uint16 allocatedIdx = delSession->Id.idx;
		closesocket(m_Sessions[allocatedIdx]->sock);

		// 세션 송신 큐에 존재하는 송신 직렬화 버퍼 메모리 반환
		while (delSession->sendRingBuffer.GetUseSize() >= sizeof(JBuffer*)) {
			JBuffer* sendPacekt;
			delSession->sendRingBuffer >> sendPacekt;
#if defined(ALLOC_MEM_LOG)
			m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendPacekt, to_string(sessionID) + ", FreeMem (DeleteSession)");
#endif
		}

		EnterCriticalSection(&m_SessionAllocIdQueueCS);
		m_SessionAllocIdQueue.push(allocatedIdx);
		LeaveCriticalSection(&m_SessionAllocIdQueueCS);

		delSession->Id.idx = 0;
	}

	OnClientLeave(sessionID);
}
#endif

UINT __stdcall CLanServer::AcceptThreadFunc(void* arg)
{
	CLanServer* clanserver = (CLanServer*)arg;
	while (true) {
		SOCKADDR_IN clientAddr;
		int addrLen = sizeof(clientAddr);
		SOCKET clientSock = ::accept(clanserver->m_ListenSock, (sockaddr*)&clientAddr, &addrLen);
		if (clientSock != INVALID_SOCKET) {
			if (!clanserver->OnConnectionRequest()) {
				closesocket(clientSock);
			}
			else {
				// 세션 생성
				stCLanSession* newSession = clanserver->CreateNewSession(clientSock);
				if (newSession != nullptr) {

					// 세션 생성 이벤트
					clanserver->OnClientJoin(newSession->uiId);

					if (CreateIoCompletionPort((HANDLE)clientSock, clanserver->m_IOCP, (ULONG_PTR)newSession, 0) == NULL) {
						__debugbreak();
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
							// 세션을 즉시 제거한다. 
							// ...
							// WSA_IO_PENDING 외 에러 발생 시 IO Completion Queuue에 삽입되는가...?
							DebugBreak();
						}
					}
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
	clanserver->m_SerialBuffPoolIdx = clanserver->m_SerialBuffPoolMgr.AllocTlsMemPool();	// 생성자에서 설정한 Default 값을 따름
#endif

	while (true) {
		DWORD transferred = 0;
		stCLanSession* session;
		WSAOVERLAPPED* overlappedPtr;
		GetQueuedCompletionStatus(clanserver->m_IOCP, &transferred, (PULONG_PTR)&session, &overlappedPtr, INFINITE);
		// transffered == 0으로 먼저 분기를 나누지 않은 이유는? transferred와 session(lpCompletionKey)에 대한 초기화를 매번 진행하고 싶지 않아서 
		if (overlappedPtr != NULL) {
			if (transferred == 0) {
				// 연결 종료 판단
				InterlockedDecrement((uint32*)&session->sessionRef);
				//if (session->ioCnt == 0) {
				assert(session->sessionRef.ioCnt >= 0);
				if(session->sessionRef.ioCnt == 0) {
					// 세션 제거...
#if defined(SESSION_RELEASE_LOG)
					clanserver->DeleteSession(session->uiId, "GQCS 실패 리턴");
#else
					clanserver->DeleteSession(session->uiId);
#endif
				}
			}
			else {
				// 수신 완료 통지
				if (&(session->recvOverlapped) == overlappedPtr) {
#if defined(MT_FILE_LOG)
					// Log
					{	
						USHORT logIdx = clanserver->mtFileLogger.AllocLogIndex();
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr0 = 0;				// 수신 완료 
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr1 = transferred;	
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr4 = session->sendRingBuffer.GetUseSize();
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr5 = session->sendRingBuffer.GetEnqOffset();
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr6 = session->sendRingBuffer.GetDeqOffset();
					}
#endif

					session->recvRingBuffer.DirectMoveEnqueueOffset(transferred);
					clanserver->OnRecv(session->uiId, session->recvRingBuffer);	// OnRecv 함수에서는 에코 송신을 수행한다. 

					session->clearRecvOverlapped();
					WSABUF wsabuf;
					wsabuf.buf = (CHAR*)session->recvRingBuffer.GetEnqueueBufferPtr();
					wsabuf.len = session->recvRingBuffer.GetDirectEnqueueSize();
					DWORD dwflag = 0;
					if (wsabuf.len == 0) {
						// 0 바이트 수신 요청이 발생하는지 확인
						DebugBreak();
					}

					if (WSARecv(session->sock, &wsabuf, 1, NULL, &dwflag, &session->recvOverlapped, NULL) == SOCKET_ERROR) {
						int errcode = WSAGetLastError();
						if (errcode != WSA_IO_PENDING) {
							//InterlockedDecrement(&session->ioCnt);
							InterlockedDecrement((uint32*)&session->sessionRef);
							//if (session->ioCnt == 0) {
							assert(session->sessionRef.ioCnt >= 0);
							if(session->sessionRef.ioCnt == 0) {
								// 세션 삭제
#if defined(SESSION_RELEASE_LOG)
								clanserver->DeleteSession(session->uiId, "WSARecv 실패");
#else
								clanserver->DeleteSession(session->uiId);
#endif
							}
						}
					}
				}
				// 송신 완료 통지
				else if (&(session->sendOverlapped) == overlappedPtr) {
#if defined(MT_FILE_LOG)
					// Log
					{
						USHORT logIdx = clanserver->mtFileLogger.AllocLogIndex();
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr0 = 5;				// 송신 완료 
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr4 = session->sendRingBuffer.GetUseSize();
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr5 = session->sendRingBuffer.GetEnqOffset();
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr6 = session->sendRingBuffer.GetDeqOffset();
					}
#endif
					//InterlockedDecrement(&session->ioCnt);
					InterlockedDecrement((uint32*)&session->sessionRef);

#if defined(SESSION_SENDBUFF_SYNC_TEST)
					//session->sendBuffMtx.lock();
					AcquireSRWLockExclusive(&session->sendBuffSRWLock);
#endif

#if defined(SEND_RECV_RING_BUFF_COPY_MODE)
					session->sendRingBuffer.DirectMoveDequeueOffset(transferred);
#elif defined(SEND_RECV_RING_BUFF_SERIALIZATION_MODE)
					for (int i = 0; i < session->sendOverlapped.Offset; i++) {
						JBuffer* sendBuff;
						session->sendRingBuffer >> sendBuff;
						//cout << "[Free] " << sendBuff << endl;

#if defined(MT_FILE_LOG)
// Log
						{
							USHORT logIdx = clanserver->mtFileLogger.AllocLogIndex();
							clanserver->mtFileLogger.GetLogStruct(logIdx).ptr0 = 1;				// 송신 완료 
							clanserver->mtFileLogger.GetLogStruct(logIdx).ptr1 = (UINT_PTR)sendBuff;
							clanserver->mtFileLogger.GetLogStruct(logIdx).ptr2 = (UINT_PTR)sendBuff->GetUseSize();
							clanserver->mtFileLogger.GetLogStruct(logIdx).ptr3 = session->sendOverlapped.Offset;
							clanserver->mtFileLogger.GetLogStruct(logIdx).ptr4 = session->sendRingBuffer.GetUseSize();
							clanserver->mtFileLogger.GetLogStruct(logIdx).ptr5 = session->sendRingBuffer.GetEnqOffset();
							clanserver->mtFileLogger.GetLogStruct(logIdx).ptr6 = session->sendRingBuffer.GetDeqOffset();
						}
#endif

#if defined(ALLOC_BY_TLS_MEM_POOL)

#if defined(ALLOC_MEM_LOG)
						clanserver->m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendBuff, to_string(session->uiId) + ", FreeMem (송신 완료)");
#else
						clanserver->m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendBuff);
#endif
#else
						delete sendBuff;
#endif
					}
					//session->sendRingBuffer.DirectMoveDequeueOffset(session->sendOverlapped.Offset * sizeof(UINT_PTR));
#endif


#if defined(SESSION_SENDBUFF_SYNC_TEST)
					//session->sendBuffMtx.unlock();
					ReleaseSRWLockExclusive(&session->sendBuffSRWLock);
#endif

					InterlockedExchange(&session->sendFlag, 0);

					if (session->sendRingBuffer.GetUseSize() > 0) {
						clanserver->SendPost(session->uiId);
					}
				}
				else {
					DebugBreak();
				}
			}
		}
		else {
			// 1. IOCP 객체 자체의 문제
			// 2. GQCS 호출 시 INIFINTE 외 대기 시간을 걸어 놓은 경우, 대기 시간 내 I/O가 완료되지 않은 경우
			break;
		}
	}

	return 0;
}


#if defined(MT_FILE_LOG)
void CLanServer::PrintMTFileLog() {
	mtFileLogger.LockMTLogger();

	time_t now = time(0);
	struct tm timeinfo;
	char buffer[80];
	localtime_s(&timeinfo, &now);
	strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", &timeinfo);
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
	outputFile << "LastIndex   : " << mtFileLogger.GetNowIndex() << std::endl;
	outputFile << "IndexTurnCnt: " << mtFileLogger.GetIndexTurnCnt() << std::endl;

	USHORT logIdxLimit;
	if (mtFileLogger.GetIndexTurnCnt() == 0) {
		logIdxLimit = mtFileLogger.GetNowIndex();
	}
	else {
		logIdxLimit = USHRT_MAX;
	}
	for (size_t idx = 0; idx <= logIdxLimit; idx++) {
		outputFile << "----------------------------------------------------" << endl;
		outputFile << "thread ID: " << mtFileLogger.GetLogStruct(idx).threadID << endl;
		switch (mtFileLogger.GetLogStruct(idx).ptr0) {
		case 0:
			outputFile << "[RECV COMPLETION]" << endl;
			outputFile << "Recv Bytes: " << mtFileLogger.GetLogStruct(idx).ptr1 << endl;
			outputFile << "Send Buff Size: " << mtFileLogger.GetLogStruct(idx).ptr4 << endl;
			outputFile << "Send Buff Enq Offset: " << mtFileLogger.GetLogStruct(idx).ptr5 << endl;
			outputFile << "Send Buff Deq Offset: " << mtFileLogger.GetLogStruct(idx).ptr6 << endl;
			break;
		case 1:
			outputFile << "[SEND COMPLETION]" << endl;
			outputFile << "Send Buff: " << mtFileLogger.GetLogStruct(idx).ptr1 << endl;
			outputFile << "Send Bytes: " << mtFileLogger.GetLogStruct(idx).ptr2 << endl;
			outputFile << "Send Offset: " << mtFileLogger.GetLogStruct(idx).ptr3 << endl;
			outputFile << "Send Buff Size: " << mtFileLogger.GetLogStruct(idx).ptr4 << endl;
			outputFile << "Send Buff Enq Offset: " << mtFileLogger.GetLogStruct(idx).ptr5 << endl;
			outputFile << "Send Buff Deq Offset: " << mtFileLogger.GetLogStruct(idx).ptr6 << endl;
			break;
		case 2:
			outputFile << "[OnRecv]" << endl;
			outputFile << "Send Buff(new): " << mtFileLogger.GetLogStruct(idx).ptr1 << endl;
			outputFile << "RecvBytes: " << mtFileLogger.GetLogStruct(idx).ptr2 << endl;
			break;
		case 3:
			outputFile << "[SendPost]" << endl;
			outputFile << "Send Buff: " << mtFileLogger.GetLogStruct(idx).ptr1 << endl;
			outputFile << "Send Bytes: " << mtFileLogger.GetLogStruct(idx).ptr2 << endl;
			outputFile << "Send Limits: " << mtFileLogger.GetLogStruct(idx).ptr3 << endl;
			outputFile << "Send Buff Size: " << mtFileLogger.GetLogStruct(idx).ptr4 << endl;
			outputFile << "Send Buff Enq Offset: " << mtFileLogger.GetLogStruct(idx).ptr5 << endl;
			outputFile << "Send Buff Deq Offset: " << mtFileLogger.GetLogStruct(idx).ptr6 << endl;
			break;
		case 4:
			outputFile << "[SendPacekt]" << endl;
			outputFile << "Send Buff: " << mtFileLogger.GetLogStruct(idx).ptr1 << endl;
			outputFile << "Send Bytes: " << mtFileLogger.GetLogStruct(idx).ptr2 << endl;
			outputFile << "Send Buff Size: " << mtFileLogger.GetLogStruct(idx).ptr4 << endl;
			outputFile << "Send Buff Enq Offset: " << mtFileLogger.GetLogStruct(idx).ptr5 << endl;
			outputFile << "Send Buff Deq Offset: " << mtFileLogger.GetLogStruct(idx).ptr6 << endl;
			break;
		case 5:
			outputFile << "[SEND COMPLETION (Intro)]" << endl;
			outputFile << "Send Buff Size: " << mtFileLogger.GetLogStruct(idx).ptr4 << endl;
			outputFile << "Send Buff Enq Offset: " << mtFileLogger.GetLogStruct(idx).ptr5 << endl;
			outputFile << "Send Buff Deq Offset: " << mtFileLogger.GetLogStruct(idx).ptr6 << endl;
		}
	}

	// 파일 닫기
	outputFile.close();

	std::cout << "파일이 생성되었습니다: " << filePath << std::endl;
}
#endif



void CLanServer::ConsoleLog()
{
	static size_t logCnt = 0;

	size_t totalAllocMemCnt = m_SerialBuffPoolMgr.GetTotalAllocMemCnt();
	size_t totalFreeMemCnt = m_SerialBuffPoolMgr.GetTotalFreeMemCnt();
	size_t totalIncrementRefCnt = m_SerialBuffPoolMgr.GetTotalIncrementRefCnt();
	size_t totalDecrementRefCnt = m_SerialBuffPoolMgr.GetTotalDecrementRefCnt();
	std::unordered_map<DWORD, stMemoryPoolUseInfo> memInfos = m_SerialBuffPoolMgr.GetMemInfo();
	static COORD coord;
	coord.X = 0;
	coord.Y = 0;
	SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
	
	std::cout << "[m_SessionAllocIdQueue size] "  << m_SessionAllocIdQueue.size() << "                            " << std::endl;


	std::cout << "[Log Count] " << logCnt++ << "                                                " << std::endl;
	std::cout << "Total Alloc Mem Count : " << totalAllocMemCnt << "                            " << std::endl;
	std::cout << "Total Free Mem Count  : " << totalFreeMemCnt << "                             " << std::endl;
	std::cout << "Total Increment RefCnt: " << totalIncrementRefCnt << "                        " << std::endl;
	std::cout << "Total Decrement RefCnt: " << totalDecrementRefCnt << "                        " << std::endl;
	std::cout << "------------------------------------------                                    " << std::endl;
	size_t totalUnitCnt = 0;
	for (auto iter = memInfos.begin(); iter != memInfos.end(); iter++) {
		std::cout << "[Thread: " << iter->first << "]                                           " << std::endl;
		std::cout << "TlsMemPoolUnitCnt : " << iter->second.tlsMemPoolUnitCnt << "              " << std::endl;
		std::cout << "LFMemPoolUnitCnt  : " << iter->second.lfMemPoolFreeCnt << "               " << std::endl;
		std::cout << "MallocCnt         : " << iter->second.mallocCnt << "                      " << std::endl;

		totalUnitCnt += iter->second.tlsMemPoolUnitCnt;
		totalUnitCnt += iter->second.lfMemPoolFreeCnt;
	}
	std::cout << "------------------------------------------                                    " << std::endl;
	std::cout << "Total Unit Cnt: " << totalUnitCnt << "                                        " <<std::endl;
	std::cout << "==========================================                                    " << std::endl;
}

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

void CLanServer::SessionReleaseLog() {
	time_t now = time(0);
	struct tm timeinfo;
	char buffer[80];
	localtime_s(&timeinfo, &now);
	strftime(buffer, sizeof(buffer), "SessionReleaseLog-%Y-%m-%d_%H-%M-%S", &timeinfo);
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

	//////////////// 로깅 ////////////////////
	for (USHORT i = 0; i <= m_ReleaseLogIndex; i++) {
		//m_ReleaseLog[i].sessionID
		
		if (m_ReleaseLog[i].sessionID == 0) {
			break;
		}
		if (m_CreatedSession.find(m_ReleaseLog[i].sessionID) == m_CreatedSession.end()) {
			continue;
		}

		std::cout << "-------------------------------------------------" << std::endl;
		if (m_ReleaseLog[i].createFlag) {
			std::cout << "[Create Session] sessionID: " << to_string(m_ReleaseLog[i].sessionID) << std::endl;
			std::cout << "Index:        " << m_ReleaseLog[i].sessionIndex << std::endl;
			std::cout << "Increment:    " << m_ReleaseLog[i].sessionIncrement << std::endl;
			std::cout << "Release Flag: " << m_ReleaseLog[i].releaseFlag << std::endl;
			std::cout << "IO Cnt:       " << m_ReleaseLog[i].iocnt << std::endl;
			
		}
		else {
			if (m_ReleaseLog[i].releaseSuccess) {
				std::cout << "[Release Success] sessionID: " << to_string(m_ReleaseLog[i].sessionID) << std::endl;
			}
			else {
				std::cout << "[Release Fail] sessionID: " << to_string(m_ReleaseLog[i].sessionID) << std::endl;
			}
			
			std::cout << "Log: " << m_ReleaseLog[i].log << std::endl;
			std::cout << "Index:        " << m_ReleaseLog[i].sessionIndex << std::endl;
			std::cout << "Increment:    " << m_ReleaseLog[i].sessionIncrement << std::endl;
			std::cout << "Release Flag: " << m_ReleaseLog[i].releaseFlag << std::endl;
			std::cout << "IO Cnt:       " << m_ReleaseLog[i].iocnt << std::endl;
		}
	}

	//////////////////////////////////////////

	// 파일 닫기
	outputFile.close();

	std::cout << "파일이 생성되었습니다: " << filePath << std::endl;
}