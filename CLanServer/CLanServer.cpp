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
	// ��Ʈ��ũ �ʱ�ȭ
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
	// ���� ���� �ʱ�ȭ
	//////////////////////////////////////////////////
	for (uint16 idx = 1; idx <= m_MaxOfSessions; idx++) {		// sessionID�� �ε��� �� ���� 0�̶�� ���� �������� ���� �����̶�� ���ʰ�
		m_SessionAllocIdQueue.push(idx);
	}
	m_Sessions.resize(m_MaxOfSessions + 1, NULL);
	for (uint16 idx = 1; idx <= m_MaxOfSessions; idx++) {
		m_Sessions[idx] = new stCLanSession;
	}
	InitializeCriticalSection(&m_SessionAllocIdQueueCS);
	InitializeCriticalSection(&m_SessionCS);

	//////////////////////////////////////////////////
	// IOCP ��ü �ʱ�ȭ
	//////////////////////////////////////////////////
	m_IOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, numOfIocpConcurrentThrd);
	assert(m_IOCP != NULL);


	//////////////////////////////////////////////////
	// ������ ���� �ʱ�ȭ
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

	// 1. ���� ���� ���� �ݱ� -> Accept �������� accept �Լ����� INVALID_SOCKET ��ȯ
	closesocket(m_ListenSock);

	// 2. Accept �����尡 �۾��� �������� ���Ḧ ���� PostQueuedCompletionStatus �Լ��� ���� ���Ḧ ���� �Ϸ� ������ �߻���Ŵ. �� �� Accept ������ ����
	// 2-1. �۾��� ������ �� ��ŭ Post
	// 2-2. �ϳ��� Post �� �� ��������� �ٽ� Post�� ����
	// => �ϴ� 2-1 ������� ����
	for (int i = 0; i < m_NumOfWorkerThreads; i++) {
		PostQueuedCompletionStatus(m_IOCP, 0, 0, NULL);
	}

	// 3. Accept ������ �� �۾��� ������ ���Ḧ ���
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
			// �۽� ��-���ۿ� �۽� �����͸� Enqueue(����)�� ���� ���� ����(�۽� ���� �ʰ�)
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� �����͸� Enqueue�� ���� ������ ����" << endl;
			DebugBreak();
		}
		uint32 enqSize = session->sendRingBuffer.Enqueue(sendData.GetDequeueBufferPtr(), sendData.GetUseSize());
		if (enqSize < sendData.GetUseSize()) {
			// �۽� ��-���ۿ� �۽� �����͸� ������ �� ������ Ȯ���������� �ұ��ϰ�,
			// Enqueue ����� �۽� �������� ũ�⺸�� ���� ��Ȳ �߻�
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� ������ ��ü Enqueue ����" << endl;
			DebugBreak();
		}
#elif defined(SEND_RECV_RING_BUFF_SERIALIZATION_MODE)
		if (session->sendRingBuffer.GetFreeSize() < sizeof(UINT_PTR)) {
			// �۽� ��-���ۿ� �۽� �����͸� Enqueue(����)�� ���� ���� ����(�۽� ���� �ʰ�)
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� �����͸� Enqueue�� ���� ������ ����" << endl;
			DebugBreak();
		}
		UINT_PTR sendDataPtr = (UINT_PTR)&sendDataRef;
		uint32 enqSize = session->sendRingBuffer.Enqueue((BYTE*)(&sendDataPtr), sizeof(UINT_PTR));
		if (enqSize < sizeof(UINT_PTR)) {
			// �۽� ��-���ۿ� �۽� �����͸� ������ �� ������ Ȯ���������� �ұ��ϰ�,
			// Enqueue ����� �۽� �������� ũ�⺸�� ���� ��Ȳ �߻�
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� ������ ��ü Enqueue ����" << endl;
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
	if (session != nullptr) {				// �ε����� ������ �ٸ� �����̰ų� ���ŵ�(��������) ����
#if defined(SESSION_SENDBUFF_SYNC_TEST)
		//session->sendBuffMtx.lock();
		AcquireSRWLockExclusive(&session->sendBuffSRWLock);
#endif

#if defined(SEND_RECV_RING_BUFF_COPY_MODE)
		if (session->sendRingBuffer.GetFreeSize() < sendData->GetUseSize()) {
			// �۽� ��-���ۿ� �۽� �����͸� Enqueue(����)�� ���� ���� ����(�۽� ���� �ʰ�)
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� �����͸� Enqueue�� ���� ������ ����" << endl;
			DebugBreak();
		}
		uint32 enqSize = session->sendRingBuffer.Enqueue(sendData->GetDequeueBufferPtr(), sendData->GetUseSize());
		if (enqSize < sendData->GetUseSize()) {
			// �۽� ��-���ۿ� �۽� �����͸� ������ �� ������ Ȯ���������� �ұ��ϰ�,
			// Enqueue ����� �۽� �������� ũ�⺸�� ���� ��Ȳ �߻�
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� ������ ��ü Enqueue ����" << endl;
			DebugBreak();
		}
#elif defined(SEND_RECV_RING_BUFF_SERIALIZATION_MODE)
		if (session->sendRingBuffer.GetFreeSize() < sizeof(UINT_PTR)) {
			// �۽� ��-���ۿ� �۽� �����͸� Enqueue(����)�� ���� ���� ����(�۽� ���� �ʰ�)
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� �����͸� Enqueue�� ���� ������ ����" << endl;
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
			// �۽� ��-���ۿ� �۽� �����͸� ������ �� ������ Ȯ���������� �ұ��ϰ�,
			// Enqueue ����� �۽� �������� ũ�⺸�� ���� ��Ȳ �߻�
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� ������ ��ü Enqueue ����" << endl;
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
	stCLanSession* session = m_Sessions[idx];					// ���� ID�� �ε��� ��Ʈ�� ���� ���� ȹ��
	if (session == nullptr) {									// AcquireSession�� ȣ���ϴ� �������� ã���� �Ͽ��� ������ ȹ���Ͽ��ٴ� ������ �� �� ����
		DebugBreak();											// (�̹� ������ �����̰ų�, ������ �� ���� �ε��� �ڸ��� ��Ȱ��� ������ �� ����)
	}

	InterlockedIncrement((uint32*)&session->sessionRef);		// ���� IOCnt ����
																// ���� IOCnt�� ������ ���� ���Ŀ��� AcquireSession�� ȣ���ϸ鼭 ã���� �Ͽ��� �����̵�,
																// ���� �ε��� �ڸ��� ��Ȱ��� �����̵� �������� �ʴ� ������ �� �� ����
	
	if (session->uiId != sessionID) {							// ã���� �Ͽ��� �������� Ȯ��, �ƴ� ��� ���� ���״� IOCnt�� ���� ��Ű�� nullptr ��ȯ
		//ReturnSession(session);

		InterlockedDecrement((uint32*)&session->sessionRef);
		assert(session->sessionRef.ioCnt >= 0);
		if (session->sessionRef.ioCnt == 0) {
			DeleteSession(session->uiId, "AcquireSession" + to_string(sessionID) + " -(����)-> " + to_string(session->uiId));
		}
		return nullptr;
	}

	if (session->sessionRef.releaseFlag == 1) {					// ������ ���� ���� �����̰ų� �̹� ������ �����̶�� nullptr ��ȯ
		return nullptr;
	}

	return session;
}

void CLanServer::ReturnSession(stCLanSession* session)
{
	InterlockedDecrement((uint32*)&session->sessionRef);
	assert(session->sessionRef.ioCnt >= 0);
	if (session->sessionRef.ioCnt == 0) {					// ã���� �ϴ� ������ �ƴ� �ٸ� ������ ioCnt�� 0�� �ȴٸ�?!
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
		session->clearSendOverlapped();	// �۽ſ� overlapped ����ü �ʱ�ȭ

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

			session->sendOverlapped.Offset = sendLimit;	// Offset ����� Ȱ���غ��� ���?
															// �۽��� �޽��� ������ �㵵�� �Ѵ�.

			if (WSASend(session->sock, wsabuffs, sendLimit, NULL, 0, &session->sendOverlapped, NULL) == SOCKET_ERROR) {
				int errcode = WSAGetLastError();
				if (errcode != WSA_IO_PENDING) {
					//InterlockedDecrement(&session->ioCnt);
					InterlockedDecrement((uint32*)&session->sessionRef);

					assert(session->sessionRef.ioCnt >= 0);
					if(session->sessionRef.ioCnt == 0) {
#if defined(SESSION_RELEASE_LOG)
						DeleteSession(sessionID, "WSASend ����");
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
//	// ���� ����
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
		// ���� ���� t����
		m_ReleaseLog[releaseLogIdx].releaseFlag = true;
		m_CreatedSessionMtx.lock();
		m_CreatedSession.erase(sessionID);
		m_CreatedSessionMtx.unlock();

		// ���� ����
		uint16 allocatedIdx = delSession->Id.idx;
		closesocket(m_Sessions[allocatedIdx]->sock);

		// ���� �۽� ť�� �����ϴ� �۽� ����ȭ ���� �޸� ��ȯ
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
		// ���� ���� ����
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
		// ���� ����
		uint16 allocatedIdx = delSession->Id.idx;
		closesocket(m_Sessions[allocatedIdx]->sock);

		// ���� �۽� ť�� �����ϴ� �۽� ����ȭ ���� �޸� ��ȯ
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
				// ���� ����
				stCLanSession* newSession = clanserver->CreateNewSession(clientSock);
				if (newSession != nullptr) {

					// ���� ���� �̺�Ʈ
					clanserver->OnClientJoin(newSession->uiId);

					if (CreateIoCompletionPort((HANDLE)clientSock, clanserver->m_IOCP, (ULONG_PTR)newSession, 0) == NULL) {
						__debugbreak();
					}

					// WSARecv 
					WSABUF wsabuf;
					wsabuf.buf = (CHAR*)newSession->recvRingBuffer.GetEnqueueBufferPtr();
					wsabuf.len = newSession->recvRingBuffer.GetFreeSize();
					// Zero byte recv �׽�Ʈ
					//wsabuf.len = 0;
					DWORD dwFlag = 0;

					//newSession->ioCnt = 1;
					// => ���� Release ���� ����(24.04.08) ����
					// ���� Init �Լ����� IOCnt�� 1�� �ʱ�ȭ�ϴ� ���� �´µ�..
					if (WSARecv(newSession->sock, &wsabuf, 1, NULL, &dwFlag, &newSession->recvOverlapped, NULL) == SOCKET_ERROR) {
						int errcode = WSAGetLastError();
						if (errcode != WSA_IO_PENDING) {
							// ������ ��� �����Ѵ�. 
							// ...
							// WSA_IO_PENDING �� ���� �߻� �� IO Completion Queuue�� ���ԵǴ°�...?
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
	clanserver->m_SerialBuffPoolIdx = clanserver->m_SerialBuffPoolMgr.AllocTlsMemPool();	// �����ڿ��� ������ Default ���� ����
#endif

	while (true) {
		DWORD transferred = 0;
		stCLanSession* session;
		WSAOVERLAPPED* overlappedPtr;
		GetQueuedCompletionStatus(clanserver->m_IOCP, &transferred, (PULONG_PTR)&session, &overlappedPtr, INFINITE);
		// transffered == 0���� ���� �б⸦ ������ ���� ������? transferred�� session(lpCompletionKey)�� ���� �ʱ�ȭ�� �Ź� �����ϰ� ���� �ʾƼ� 
		if (overlappedPtr != NULL) {
			if (transferred == 0) {
				// ���� ���� �Ǵ�
				InterlockedDecrement((uint32*)&session->sessionRef);
				//if (session->ioCnt == 0) {
				assert(session->sessionRef.ioCnt >= 0);
				if(session->sessionRef.ioCnt == 0) {
					// ���� ����...
#if defined(SESSION_RELEASE_LOG)
					clanserver->DeleteSession(session->uiId, "GQCS ���� ����");
#else
					clanserver->DeleteSession(session->uiId);
#endif
				}
			}
			else {
				// ���� �Ϸ� ����
				if (&(session->recvOverlapped) == overlappedPtr) {
#if defined(MT_FILE_LOG)
					// Log
					{	
						USHORT logIdx = clanserver->mtFileLogger.AllocLogIndex();
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr0 = 0;				// ���� �Ϸ� 
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr1 = transferred;	
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr4 = session->sendRingBuffer.GetUseSize();
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr5 = session->sendRingBuffer.GetEnqOffset();
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr6 = session->sendRingBuffer.GetDeqOffset();
					}
#endif

					session->recvRingBuffer.DirectMoveEnqueueOffset(transferred);
					clanserver->OnRecv(session->uiId, session->recvRingBuffer);	// OnRecv �Լ������� ���� �۽��� �����Ѵ�. 

					session->clearRecvOverlapped();
					WSABUF wsabuf;
					wsabuf.buf = (CHAR*)session->recvRingBuffer.GetEnqueueBufferPtr();
					wsabuf.len = session->recvRingBuffer.GetDirectEnqueueSize();
					DWORD dwflag = 0;
					if (wsabuf.len == 0) {
						// 0 ����Ʈ ���� ��û�� �߻��ϴ��� Ȯ��
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
								// ���� ����
#if defined(SESSION_RELEASE_LOG)
								clanserver->DeleteSession(session->uiId, "WSARecv ����");
#else
								clanserver->DeleteSession(session->uiId);
#endif
							}
						}
					}
				}
				// �۽� �Ϸ� ����
				else if (&(session->sendOverlapped) == overlappedPtr) {
#if defined(MT_FILE_LOG)
					// Log
					{
						USHORT logIdx = clanserver->mtFileLogger.AllocLogIndex();
						clanserver->mtFileLogger.GetLogStruct(logIdx).ptr0 = 5;				// �۽� �Ϸ� 
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
							clanserver->mtFileLogger.GetLogStruct(logIdx).ptr0 = 1;				// �۽� �Ϸ� 
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
						clanserver->m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendBuff, to_string(session->uiId) + ", FreeMem (�۽� �Ϸ�)");
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
			// 1. IOCP ��ü ��ü�� ����
			// 2. GQCS ȣ�� �� INIFINTE �� ��� �ð��� �ɾ� ���� ���, ��� �ð� �� I/O�� �Ϸ���� ���� ���
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

	// ���� ��� ����
	std::string filePath = "./" + currentDateTime + ".txt";

	// ���� ��Ʈ�� ����
	std::ofstream outputFile(filePath);

	if (!outputFile) {
		std::cerr << "������ �� �� �����ϴ�." << std::endl;
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

	// ���� �ݱ�
	outputFile.close();

	std::cout << "������ �����Ǿ����ϴ�: " << filePath << std::endl;
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

	// ���� ��� ����
	std::string filePath = "./" + currentDateTime + ".txt";

	// ���� ��Ʈ�� ����
	std::ofstream outputFile(filePath);

	if (!outputFile) {
		std::cerr << "������ �� �� �����ϴ�." << std::endl;
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
	

	// ���� �ݱ�
	outputFile.close();

	std::cout << "������ �����Ǿ����ϴ�: " << filePath << std::endl;
}

void CLanServer::SessionReleaseLog() {
	time_t now = time(0);
	struct tm timeinfo;
	char buffer[80];
	localtime_s(&timeinfo, &now);
	strftime(buffer, sizeof(buffer), "SessionReleaseLog-%Y-%m-%d_%H-%M-%S", &timeinfo);
	std::string currentDateTime = std::string(buffer);

	// ���� ��� ����
	std::string filePath = "./" + currentDateTime + ".txt";

	// ���� ��Ʈ�� ����
	std::ofstream outputFile(filePath);

	if (!outputFile) {
		std::cerr << "������ �� �� �����ϴ�." << std::endl;
		return;
	}

	outputFile << currentDateTime << std::endl;

	//////////////// �α� ////////////////////
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

	// ���� �ݱ�
	outputFile.close();

	std::cout << "������ �����Ǿ����ϴ�: " << filePath << std::endl;
}