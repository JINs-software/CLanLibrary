#include "CLanServer.h"
#include <cassert>
#include <process.h>
#include <fstream>

#include <cstdlib> // system �Լ��� ����ϱ� ���� �ʿ�

#if defined(ALLOC_BY_TLS_MEM_POOL)
CLanServer::CLanServer(const char* serverIP, uint16 serverPort,
	DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections,
	bool tlsMemPoolReferenceFlag, bool tlsMemPoolPlacementNewFlag,
	size_t tlsMemPoolDefaultUnitCnt, size_t tlsMemPoolDefaultCapacity,
	uint32 sessionSendBuffSize, uint32 sessionRecvBuffSize,
	bool beNagle
)
	: m_MaxOfSessions(maxOfConnections), m_Incremental(0),
	m_NumOfWorkerThreads(numOfWorkerThreads), m_StopFlag(false),
	m_SerialBuffPoolMgr(tlsMemPoolDefaultUnitCnt, tlsMemPoolDefaultCapacity, tlsMemPoolReferenceFlag, tlsMemPoolPlacementNewFlag)
#else
CLanServer::CLanServer(const char* serverIP, uint16 serverPort,
	DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections,
	bool tlsMemPoolReferenceFlag, bool tlsMemPoolPlacementNewFlag,
	uint32 sessionSendBuffSize, uint32 sessionRecvBuffSize,
	bool beNagle
)
	: m_MaxOfSessions(maxOfConnections), m_Incremental(0), m_NumOfWorkerThreads(numOfWorkerThreads), m_StopFlag(false)
#endif
{
#if defined(SESSION_LOG)
	m_SessionLog.resize(USHRT_MAX + 1);
	m_SessionLogIndex = -1;
#endif

#if defined(SENDBUFF_MONT_LOG)
	m_SendBuffOfMaxSize = 0;
	m_SessionOfMaxSendBuff = 0;
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
		uintptr_t ret = _beginthreadex(NULL, 0, CLanServer::WorkerThreadFunc, this, CREATE_SUSPENDED, NULL);
		m_WorkerThreads[idx] = (HANDLE)ret;
		if (m_WorkerThreads[idx] == INVALID_HANDLE_VALUE) {
			DebugBreak();
		}
		DWORD thID = GetThreadId(m_WorkerThreads[idx]);
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
	CloseHandle(m_IOCP);

	WSACleanup();
}

#if defined(SESSION_LOG)
void CLanServer::Disconnect(uint64 sessionID, string log)
{
	stSessionID orgID = *((stSessionID*)&sessionID);
	stCLanSession* delSession = m_Sessions[orgID.idx];
	if (delSession->sessionRef.ioCnt < 1) {
		DebugBreak();
	}

#if defined(SESSION_LOG)
	stSessionLog& sessionLog = GetSessionLog();
	sessionLog.Init();
	sessionLog.sessionWork = SESSION_DISCONNECT;
	sessionLog.sessionID = sessionID;
	sessionLog.sessionIndex = orgID.idx;
	sessionLog.iocnt = delSession->sessionRef.ioCnt;
	sessionLog.releaseFlag = delSession->sessionRef.releaseFlag;
	sessionLog.log = log;
#endif

	PostQueuedCompletionStatus(m_IOCP, 0, (ULONG_PTR)delSession, (LPOVERLAPPED)-1);
}
#else
void CLanServer::Disconnect(uint64 sessionID)
{
	stSessionID* sessionIdPtr = (stSessionID*)&sessionID;
	stCLanSession* session = m_Sessions[sessionIdPtr->idx];
	PostQueuedCompletionStatus(m_IOCP, 0, (ULONG_PTR)session, NULL);
}
#endif

#if defined(ALLOC_BY_TLS_MEM_POOL)
bool CLanServer::SendPacket(uint64 sessionID, JBuffer* sendDataPtr) {

	stCLanSession* session = AcquireSession(sessionID);
	if (session != nullptr) {				// �ε����� ������ �ٸ� �����̰ų� ���ŵ�(��������) ����

		AcquireSRWLockExclusive(&session->sendBuffSRWLock);

		if (session->sendRingBuffer.GetFreeSize() < sizeof(UINT_PTR)) {
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� �����͸� Enqueue�� ���� ������ ����" << endl;
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
			// �۽� ��-���ۿ� �۽� �����͸� ������ �� ������ Ȯ���������� �ұ��ϰ�,
			// Enqueue ����� �۽� �������� ũ�⺸�� ���� ��Ȳ �߻�
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� ������ ��ü Enqueue ����" << endl;
			DebugBreak();
		}

		ReleaseSRWLockExclusive(&session->sendBuffSRWLock);

		SendPost(sessionID);
	}
	else {
		return false;
	}

	ReturnSession(session);

	return true;
}
#else
bool CLanServer::SendPacket(uint64 sessionID, std::shared_ptr<JBuffer> sendDataPtr)
{
	stCLanSession* session = AcquireSession(sessionID);
	if (session != nullptr) {				// �ε����� ������ �ٸ� �����̰ų� ���ŵ�(��������) ����

		AcquireSRWLockExclusive(&session->sendBuffSRWLock);

		session->sendBufferVector.push_back(sendDataPtr);

		ReleaseSRWLockExclusive(&session->sendBuffSRWLock);

		SendPost(sessionID);
	}
	else {
		return false;
	}

	ReturnSession(session);

	return true;
}
#endif

CLanServer::stCLanSession* CLanServer::AcquireSession(uint64 sessionID)
{
	uint16 idx = (uint16)sessionID;								
	stCLanSession* session = m_Sessions[idx];					// ���� ID�� �ε��� ��Ʈ�� ���� ���� ȹ��
	if (session == nullptr) {									// AcquireSession�� ȣ���ϴ� �������� ã���� �Ͽ��� ������ ȹ���Ͽ��ٴ� ������ �� �� ����
		DebugBreak();											// (�̹� ������ �����̰ų�, ������ �� ���� �ε��� �ڸ��� ��Ȱ��� ������ �� ����)
	}
	else {
#if defined(SESSION_LOG)
		stSessionLog& sessionLog = GetSessionLog();
		sessionLog.sessionWork = SESSION_ACQUIRE;
		sessionLog.sessionID = sessionID;
		sessionLog.sessionIndex = idx;
#endif

		///////////////////////////////////////////////////////////////////
		// ���� ���� ī��Ʈ(ioCnt) ����!
		uint32 uiRef = InterlockedIncrement((uint32*)&session->sessionRef);
		///////////////////////////////////////////////////////////////////
		stSessionRef sessionRef = *((stSessionRef*)&uiRef);
		if (sessionRef.ioCnt < 1) {
			DebugBreak();
		}

#if defined(SESSION_LOG)
		sessionLog.iocnt = sessionRef.ioCnt;
		sessionLog.releaseFlag = sessionRef.releaseFlag;
#endif

		// ���� IOCnt�� ������ ���� ����,
		// �����ϰ��� �Ͽ��� ���� �����̵�, �Ǵ� ���� �ε��� �ڸ��� ��Ȱ��� �����̵� �������� �ʴ� ������ �� �� ����

		// (1) if(session->sessionRef.releaseFlag == 1)
		//		=> ���� �����ϰ��� �Ͽ��� ���� �Ǵ� ���ο� ������ ����(��)
		// (2) if(session->sessionRef.releaseFlag == 0 && sessionID != session->uiId)
		//		=> �����ϰ��� �Ͽ��� ������ �̹� �����ǰ�, ���ο� ������ ���� �ε����� ����
		if (session->sessionRef.releaseFlag == 1 || sessionID != session->uiId) {
			// ���� ���� ī��Ʈ(ioCnt) ����
			InterlockedDecrement((uint32*)&session->sessionRef);
			
			// ���� ���� �� ���� ���� ����
			// ....

			// case0) ioCnt >= 1, ���� ��ȿ
			// case1) releaseFlag == 1 ����, ���� ����(��)
			// caas2) releaseFlag == 0, ioCnt == 0
			//			=> Disconnect å��

			stSessionRef exgRef;
			exgRef.ioCnt = 1;
			exgRef.releaseFlag = 0;
			uint32 exg = *((uint32*)&exgRef);
			uiRef = InterlockedCompareExchange((uint32*)&session->sessionRef, exg, 0);
			sessionRef = *((stSessionRef*)&uiRef);

			// releaseFlag == 0�� �� ���¿��� ioCnt == 0�� �� ��Ȳ
			// => Disconnect ȣ�� å��
			// CAS�� �����Ͽ��⿡ CAS �� �ĺ��� ���� ������ ������ �����
			if (sessionRef.ioCnt == 0 && sessionRef.releaseFlag == 0) {
				Disconnect(session->uiId, "AcquireSession, Disconnect other session..");
			}
			else if (sessionRef.ioCnt == 0 && sessionRef.releaseFlag == 1) {
				// ���ο� ���� ���� ��
				// nothing to do..
			}
			else if (sessionRef.ioCnt < 0) {
				DebugBreak();
			}
			return nullptr;
		}
		else {
#if defined(SESSION_LOG)
			sessionLog.workDone = true;
#endif
			// ���� ���� Ȯ�� -> ��ȯ (ioCnt == 1�̶�� ReturnSession���� ó���� ��)
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
	// => ���� �ĺ�
	// ex) �񵿱� ������ �ɷ��ִ� ��Ȳ���� AcquireSession�� ���� ioCnt == 2�� ��Ȳ,
	// (1) ������Ʈ ������, ioCnt == 2�� ��Ȳ���� if (session->sessionRef.ioCnt == 1) ���� ���
	// (2) �۾��� ������, GQCS ���� �Ǵ� �߰����� WSARecv ���� -> ioCnt ���� -> ioCnt == 1 �̱⿡ ���� ������ �õ����� ����(DeleteSession ȣ�� x)
	// (3) ������Ʈ ������, else ������ ioCnt�� ���ҽ�Ŵ -> ioCnt == 0
	// =>  ioCnt == 0, release == 0�� ��Ȳ���� ������ �������� �ʰ� �����ִ� ��Ȳ �߻���.

	assert(session->sessionRef.ioCnt >= 1);

	uint64 sessionID = session->uiId;

#if defined(SESSION_LOG)
	stSessionLog& sessionLog = GetSessionLog();
	sessionLog.sessionWork = SESSION_RETURN;
	sessionLog.sessionID = sessionID;
	sessionLog.sessionIndex = (uint16)sessionID;
#endif

	uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
	stSessionRef sessionRef = *((stSessionRef*)&uiRef);
	if (sessionRef.ioCnt < 0) {
		DebugBreak();
	}

#if defined(SESSION_LOG)
	sessionLog.iocnt = sessionRef.ioCnt;
	sessionLog.releaseFlag = sessionRef.releaseFlag;
#endif

	stSessionRef exgRef;
	exgRef.ioCnt = 1;
	exgRef.releaseFlag = 0;
	uint32 exg = *((uint32*)&exgRef);
	uiRef = InterlockedCompareExchange((uint32*)&session->sessionRef, exg, 0);
	sessionRef = *((stSessionRef*)&uiRef);
	if (sessionRef.ioCnt == 0 && sessionRef.releaseFlag == 0) {
		Disconnect(sessionID, "ReturnSession Disconnect");
	}
	else if (sessionRef.ioCnt == 0 && sessionRef.releaseFlag == 1) {
		// ���ο� ���� ���� ��
		// nothing to do..
	}
	else if (sessionRef.ioCnt < 0) {
		DebugBreak();
	}
	else {
#if defined(SESSION_LOG)
		sessionLog.workDone = true;
#endif
	}

	// (1) ioCnt == 1�� ��Ȳ, �� ���� AcquireSession�� ���� ������ ioCnt�� ���Ҵٴ� ��
	// (2) ioCnt == 1�� ��Ȳ���� InterlockedDecrement�� ���� 0���� ����Ǿ��� ��,
	//     �ٸ� iocp �۾��� �����忡 ���� ���ҵ� ��Ȳ�� �����. �̹� ioCnt == 0�� ��Ȳ���� AcquireSession���� 1�� �� ��Ȳ�̱⿡
}

void CLanServer::SendPost(uint64 sessionID)
{
	uint16 idx = (uint16)sessionID;
	stCLanSession* session = m_Sessions[idx];
	if (session == nullptr) {
		DebugBreak();
	}

	if (InterlockedExchange(&session->sendFlag, 1) == 0) {
		session->clearSendOverlapped();	// �۽ſ� overlapped ����ü �ʱ�ȭ


		AcquireSRWLockShared(&session->sendBuffSRWLock);
#if defined(ALLOC_BY_TLS_MEM_POOL)
		DWORD numOfMessages = session->sendRingBuffer.GetUseSize() / sizeof(UINT_PTR);
#else
		DWORD numOfMessages = session->sendBufferVector.size();
#endif
		ReleaseSRWLockShared(&session->sendBuffSRWLock);

		WSABUF wsabuffs[WSABUF_ARRAY_DEFAULT_SIZE];

		if (numOfMessages > 0) {
			InterlockedIncrement((uint32*)&session->sessionRef);

			int sendLimit = min(numOfMessages, WSABUF_ARRAY_DEFAULT_SIZE);
			for (int idx = 0; idx < sendLimit; idx++) {
#if defined(ALLOC_BY_TLS_MEM_POOL)
				JBuffer* msgPtr;
				session->sendRingBuffer.Peek(sizeof(UINT_PTR) * idx, (BYTE*)&msgPtr, sizeof(UINT_PTR));
				wsabuffs[idx].buf = (CHAR*)msgPtr->GetBeginBufferPtr();
				wsabuffs[idx].len = msgPtr->GetUseSize();
				if (wsabuffs[idx].buf == NULL || wsabuffs[idx].len == 0) {
					DebugBreak();
				}
#else
				shared_ptr<JBuffer> msgPtr = session->sendBufferVector[idx];
				wsabuffs[idx].buf = (CHAR*)msgPtr->GetBeginBufferPtr();
				wsabuffs[idx].len = msgPtr->GetUseSize();
#endif
				
			}
			session->sendOverlapped.Offset = sendLimit;		// Offset ����� Ȱ���غ��� ���?
															// �۽��� �޽��� ������ �㵵�� �Ѵ�.

#if defined(SESSION_LOG)
			stSessionLog& wsasendLog = GetSessionLog();
			wsasendLog.sessionWork = SESSION_WSASEND;
			wsasendLog.sessionID = session->uiId;
			wsasendLog.sessionIndex = session->Id.idx;
			wsasendLog.iocnt = session->sessionRef.ioCnt;
			wsasendLog.releaseFlag = session->sessionRef.releaseFlag;
#endif

			if (WSASend(session->sock, wsabuffs, sendLimit, NULL, 0, &session->sendOverlapped, NULL) == SOCKET_ERROR) {
#if defined(SESSION_LOG)
				wsasendLog.workDone = false;
#endif

				int errcode = WSAGetLastError();
				if (errcode != WSA_IO_PENDING) {
#if defined(SESSION_LOG)
					wsasendLog.ioPending = false;
#endif
					uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
					stSessionRef sessionRef = *((stSessionRef*)&uiRef);

					assert(sessionRef.ioCnt >= 0);
					if (sessionRef.ioCnt == 0) {
						HANDLE thHnd = GetCurrentThread();

						bool workerFlag = false;
						for (int i = 0; i < m_NumOfWorkerThreads; i++) {
							if (m_WorkerThreads[i] == thHnd) {
								if (DeleteSession(sessionID, "SendPost, WSASendFail")) {
									OnClientLeave(sessionID);
								}
								workerFlag = true;
								break;
							}
						}

						if (!workerFlag) {
							DebugBreak();		// ������Ʈ �����忡���� �׻� AcquireSession ȣ�� �� SendPost ȣ�� �Ǵ�
												// ���� ioCnt >= 1 �� ������� �����ϱ⿡ �ش� �κ� ������ Ÿ�� ���� ���������� ��Ȳ���� ����
							//stCLanSession* disconnectedSession = AcquireSession(sessionID);
							//if (disconnectedSession != nullptr) {
							//	Disconnect(sessionID, "SendPost, WSASendFail");
							//}
						}
					}
				}
				else {
					wsasendLog.ioPending = true;
				}
			}
			else {
				wsasendLog.workDone = true;
			}
		}
		else {
			InterlockedExchange(&session->sendFlag, 0);
		}
	}
}

CLanServer::stCLanSession* CLanServer::CreateNewSession(SOCKET sock)
{
#if defined(SESSION_LOG)
	stSessionLog& sessionLog = GetSessionLog();
#endif

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

#if defined(SESSION_LOG)
		m_CreatedSessionMtx.lock();
		m_CreatedSession.insert(newSession->uiId);
		m_CreatedSessionMtx.unlock();

		sessionLog.sessionWork = SESSION_CREATE;
		sessionLog.sessionID = newSession->uiId;
		sessionLog.sessionIndex = newSession->Id.idx;
		sessionLog.iocnt = newSession->sessionRef.ioCnt;
		sessionLog.releaseFlag = newSession->sessionRef.releaseFlag;
#endif

	}
	LeaveCriticalSection(&m_SessionAllocIdQueueCS);

	return newSession;
}

#if defined(SESSION_LOG)
bool CLanServer::DeleteSession(uint64 sessionID, string log) {
	bool ret = false;
	
	stSessionLog& sessionLog = GetSessionLog();

	uint16 idx = (uint16)sessionID;
	stCLanSession* delSession = m_Sessions[idx];
	if (delSession == nullptr) {
		DebugBreak();
		return false;
	}

	stSessionID orgID = *((stSessionID*)&sessionID);

	sessionLog.sessionWork = SESSION_RELEASE;
	sessionLog.sessionID = sessionID;
	sessionLog.sessionIndex = orgID.idx;
	sessionLog.iocnt = delSession->sessionRef.ioCnt;
	sessionLog.releaseFlag = delSession->sessionRef.releaseFlag;
	sessionLog.log = log;


	if(delSession->TryRelease()) {
		ret = true;

		if (delSession->uiId != sessionID) {
			DebugBreak();
		}

		// ���� ���� ����
		sessionLog.workDone = true;

		m_CreatedSessionMtx.lock();
		m_CreatedSession.erase(sessionID);
		m_CreatedSessionMtx.unlock();

		// ���� ����
		uint16 allocatedIdx = delSession->Id.idx;
		closesocket(m_Sessions[allocatedIdx]->sock);

#if defined(ALLOC_BY_TLS_MEM_POOL)
		OnDeleteSendPacket(sessionID, delSession->sendRingBuffer);
#else
		OnDeleteSendPacket(sessionID, delSession->sendBufferVector);
#endif

#if defined(SESSION_LOG)
		InterlockedIncrement64(&m_TotalDeleteCnt);
#endif

		EnterCriticalSection(&m_SessionAllocIdQueueCS);
		m_SessionAllocIdQueue.push(allocatedIdx);
		LeaveCriticalSection(&m_SessionAllocIdQueueCS);
	}
	else {
		// ���� ���� ����
		sessionLog.workDone = false;
	}

	return ret;
}
#else
bool CLanServer::DeleteSession(uint64 sessionID)
{
	bool ret = false;
	uint16 idx = (uint16)sessionID;
	stCLanSession* delSession = m_Sessions[idx];
	if (delSession == nullptr) {
		DebugBreak();
		return;
	}

	uint32 chg = 0;
	((stSessionRef*)(&chg))->releaseFlag = 1;

	uint32 org = InterlockedCompareExchange((uint32*)&delSession->sessionRef, chg, 0);
	if (org == 0) {
		ret = true;
		// ���� ����
		uint16 allocatedIdx = delSession->Id.idx;
		closesocket(m_Sessions[allocatedIdx]->sock);

		OnDeleteSendPacket(sessionID, delSession->sendRingBuffer);

		EnterCriticalSection(&m_SessionAllocIdQueueCS);
		m_SessionAllocIdQueue.push(allocatedIdx);
		LeaveCriticalSection(&m_SessionAllocIdQueueCS);

		// ���� �ε����� ���� ������ ������ �ε����� ������ ���� ����
		//delSession->Id.idx = 0;
	}

	return ret;
}
#endif

UINT __stdcall CLanServer::AcceptThreadFunc(void* arg)
{
	CLanServer* clanserver = (CLanServer*)arg;
	while (true) {
		SOCKADDR_IN clientAddr;
		int addrLen = sizeof(clientAddr);
		// ���� �ʰ� -> down client ���� �ӽ� ����
		while (true) {
			if (clanserver->OnConnectionRequest()) {
				break;
			}
		}
		//////////////////////////////////////////
		SOCKET clientSock = ::accept(clanserver->m_ListenSock, (sockaddr*)&clientAddr, &addrLen);
		if (clientSock != INVALID_SOCKET) {
			if (!clanserver->OnConnectionRequest()) {
				closesocket(clientSock);
			}
			else {
				// ���� ����
				stCLanSession* newSession = clanserver->CreateNewSession(clientSock);
				if (newSession != nullptr) {
#if defined(SESSION_LOG)
					clanserver->m_TotalAcceptCnt++;
#endif

#if defined(TRACKING_CLIENT_PORT)
					newSession->clientPort = clientAddr.sin_port;
#endif

					// ���� ���� �̺�Ʈ
					clanserver->OnClientJoin(newSession->uiId);

					DWORD errCode;
					if (CreateIoCompletionPort((HANDLE)clientSock, clanserver->m_IOCP, (ULONG_PTR)newSession, 0) == NULL) {
						errCode = GetLastError();
						DebugBreak();
					}

					// WSARecv 
					WSABUF wsabuf;
					wsabuf.buf = (CHAR*)newSession->recvRingBuffer.GetEnqueueBufferPtr();
					wsabuf.len = newSession->recvRingBuffer.GetFreeSize();
					// Zero byte recv �׽�Ʈ
					//wsabuf.len = 0;
					DWORD dwFlag = 0;

#if defined(SESSION_LOG)
					stSessionLog& sessionLog = clanserver->GetSessionLog();
					sessionLog.sessionWork = SESSION_ACCEPT_WSARECV;
					sessionLog.sessionID = newSession->uiId;
					sessionLog.sessionIndex = newSession->Id.idx;
					sessionLog.iocnt = newSession->sessionRef.ioCnt;
					sessionLog.releaseFlag = newSession->sessionRef.releaseFlag;
#endif

					//newSession->ioCnt = 1;
					// => ���� Release ���� ����(24.04.08) ����
					// ���� Init �Լ����� IOCnt�� 1�� �ʱ�ȭ�ϴ� ���� �´µ�..
					if (WSARecv(newSession->sock, &wsabuf, 1, NULL, &dwFlag, &newSession->recvOverlapped, NULL) == SOCKET_ERROR) {
						int errcode = WSAGetLastError();
						if (errcode != WSA_IO_PENDING) {
#if defined(SESSION_LOG)
							sessionLog.workDone = false;
#endif
							clanserver->Disconnect(newSession->uiId, "Accept Thread Disconnect");
						}
					}
					else {
#if defined(SESSION_LOG)
						sessionLog.workDone = true;
#endif
					}
				}
				else {
					DebugBreak();
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
		//stCLanSession* session;
		ULONG_PTR completionKey;
		WSAOVERLAPPED* overlappedPtr;
		GetQueuedCompletionStatus(clanserver->m_IOCP, &transferred, (PULONG_PTR)&completionKey, &overlappedPtr, INFINITE);
		// transffered == 0���� ���� �б⸦ ������ ���� ������? transferred�� session(lpCompletionKey)�� ���� �ʱ�ȭ�� �Ź� �����ϰ� ���� �ʾƼ� 
		if (overlappedPtr != NULL) {
			if (overlappedPtr == (LPOVERLAPPED)-1) {
			
				// ���� ���� �Ǵ�
				stCLanSession* session = (stCLanSession*)completionKey;

#if defined(SESSION_LOG)
				stSessionLog& gqcsReturnLog = clanserver->GetSessionLog();
				gqcsReturnLog.workDone = SESSION_RETURN_GQCS_Disconn;
				gqcsReturnLog.sessionID = session->uiId;
				gqcsReturnLog.sessionIndex = session->Id.idx;
				gqcsReturnLog.iocnt = session->sessionRef.ioCnt;
				gqcsReturnLog.releaseFlag = session->sessionRef.releaseFlag;
#endif

				// ���� ���� �Ǵ�
				uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
				stSessionRef sessionRef = *((stSessionRef*)&uiRef);
				if (sessionRef.ioCnt < 0) {
					DebugBreak();
				}
				if (sessionRef.ioCnt == 0) {
					// ���� ����...
					uint64 sessionID = session->uiId;
#if defined(SESSION_LOG)
					gqcsReturnLog.workDone = false;
					if (clanserver->DeleteSession(sessionID, "GQCS return Fail!")) {
						clanserver->OnClientLeave(sessionID);
					}
#else
					clanserver->DeleteSession(sessinID);
					clanserver->OnClientLeave(sessinID);
#endif
				}
				else {
#if defined(SESSION_LOG)
					gqcsReturnLog.workDone = true;
#endif
				}
			}
			else if (transferred == 0) {
				// ���� ���� �Ǵ�
				stCLanSession* session = (stCLanSession*)completionKey;
#if defined(SESSION_LOG)
				stSessionLog& gqcsReturnLog = clanserver->GetSessionLog();
				gqcsReturnLog.sessionWork = SESSION_RETURN_GQCS;
				gqcsReturnLog.sessionID = session->uiId;
				gqcsReturnLog.sessionIndex = session->Id.idx;
				gqcsReturnLog.iocnt = session->sessionRef.ioCnt;
				gqcsReturnLog.releaseFlag = session->sessionRef.releaseFlag;
#endif

				uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
				stSessionRef sessionRef = *((stSessionRef*)&uiRef);
				if (sessionRef.ioCnt < 0) {
					DebugBreak();
				}
				if(sessionRef.ioCnt == 0) {
					// ���� ����...
					uint64 sessionID = session->uiId;
#if defined(SESSION_LOG)
					gqcsReturnLog.workDone = false;
					if (clanserver->DeleteSession(sessionID, "GQCS return Fail!")) {
						clanserver->OnClientLeave(sessionID);
					}
#else
					clanserver->DeleteSession(sessinID);
					clanserver->OnClientLeave(sessinID);
#endif
				}
				else {
#if defined(SESSION_LOG)
					gqcsReturnLog.workDone = true;
#endif
				}
			}
			else {
				//////////////////////////////////////////////////////////////////////////////
				// ���� �Ϸ� ����
				//////////////////////////////////////////////////////////////////////////////
				stCLanSession* session = (stCLanSession*)completionKey;
				if (&(session->recvOverlapped) == overlappedPtr) {
#if defined(SESSION_LOG)
					stSessionLog& recvLog = clanserver->GetSessionLog();
					recvLog.sessionWork = SESSION_COMPLETE_RECV;
					recvLog.sessionID = session->uiId;
					recvLog.sessionIndex = session->Id.idx;
					recvLog.iocnt = session->sessionRef.ioCnt;
					recvLog.releaseFlag = session->sessionRef.releaseFlag;
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

#if defined(SESSION_LOG)
					stSessionLog& wsaRecvLog = clanserver->GetSessionLog();
					wsaRecvLog.sessionWork = SESSION_WSARECV;
					wsaRecvLog.sessionID = session->uiId;
					wsaRecvLog.sessionIndex = session->Id.idx;
					wsaRecvLog.iocnt = session->sessionRef.ioCnt;
					wsaRecvLog.releaseFlag = session->sessionRef.releaseFlag;
#endif

					if (WSARecv(session->sock, &wsabuf, 1, NULL, &dwflag, &session->recvOverlapped, NULL) == SOCKET_ERROR) {
						int errcode = WSAGetLastError();
						if (errcode != WSA_IO_PENDING) {
#if defined(SESSION_LOG)
							wsaRecvLog.workDone = false;
#endif
							uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
							stSessionRef sessionRef = *((stSessionRef*)&uiRef);

							assert(sessionRef.ioCnt >= 0);
							if(sessionRef.ioCnt == 0) {
								// ���� ����
								uint64 sessionID = session->uiId;
#if defined(SESSION_LOG)
								if (clanserver->DeleteSession(sessionID, "Recv Complete, WSARecv Fail!")) {
									clanserver->OnClientLeave(sessionID);
								}
#else
								clanserver->DeleteSession(sessionID);
								clanserver->OnClientLeave(sessionID);
#endif
							}
						}
						else {
							wsaRecvLog.ioPending = true;
						}
					}
					else {
#if defined(SESSION_LOG)
						wsaRecvLog.workDone = true;
#endif
					}
				}
				//////////////////////////////////////////////////////////////////////////////
				// �۽� �Ϸ� ����
				//////////////////////////////////////////////////////////////////////////////
				else if (&(session->sendOverlapped) == overlappedPtr) {
#if defined(SESSION_LOG)
					stSessionLog& sendLog = clanserver->GetSessionLog();
					sendLog.sessionWork = SESSION_COMPLETE_SEND;
					sendLog.sessionID = session->uiId;
					sendLog.sessionIndex = session->Id.idx;
					sendLog.iocnt = session->sessionRef.ioCnt;
					sendLog.releaseFlag = session->sessionRef.releaseFlag;
#endif

					// �۽� �Ϸ�� ����ȭ ���� ��ť�� �� �޸� ��ȯ
					AcquireSRWLockExclusive(&session->sendBuffSRWLock);
					for (int i = 0; i < session->sendOverlapped.Offset; i++) {
#if defined(ALLOC_BY_TLS_MEM_POOL)
						JBuffer* sendBuff;
						session->sendRingBuffer >> sendBuff;
						clanserver->m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendBuff, to_string(session->uiId) + ", FreeMem (�۽� �Ϸ�)");
#else 
						session->sendBufferVector.erase(session->sendBufferVector.begin());
#endif
					}
					ReleaseSRWLockExclusive(&session->sendBuffSRWLock);

					// sendFlag Off
					InterlockedExchange(&session->sendFlag, 0);

					// ioCnt ����
					uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
					stSessionRef sessionRef = *((stSessionRef*)&uiRef);
					assert(sessionRef.ioCnt >= 0);
					if (sessionRef.ioCnt == 0) {
						// ���� ���� ���� �Ǵ�
						uint64 sessionID = session->uiId;
						if (clanserver->DeleteSession(sessionID, "Send Complete, ioCnt == 0")) {
							clanserver->OnClientLeave(sessionID);
						}
					}
					else {
						// ���� ���� ���� -> �߰����� �۽� ������ ���� �� SendPost ȣ��
						bool sendAgainFlag = false;
						AcquireSRWLockShared(&session->sendBuffSRWLock);
#if defined(ALLOC_BY_TLS_MEM_POOL)
						if (session->sendRingBuffer.GetUseSize() >= sizeof(UINT_PTR)) {
							sendAgainFlag = true;
						}
#else
						if (session->sendBufferVector.size() > 0) {
							sendAgainFlag = true;
						}
#endif
						ReleaseSRWLockShared(&session->sendBuffSRWLock);

						if (sendAgainFlag) {
							clanserver->SendPost(session->uiId);

#if defined(SESSION_LOG)
							stSessionLog& afterSendPostLog = clanserver->GetSessionLog();
							afterSendPostLog.sessionWork = SESSION_AFTER_SENDPOST;
							afterSendPostLog.sessionID = session->uiId;
							afterSendPostLog.sessionIndex = (uint16)session->uiId;
							afterSendPostLog.iocnt = sessionRef.ioCnt;
							afterSendPostLog.releaseFlag = sessionRef.releaseFlag;
#endif
						}
					}
				}
				else {
					// ���� �Ϸᵵ �۽� �Ϸᵵ �ƴ� �������� ��Ȳ
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

	clanserver->OnWorkerThreadEnd();

	return 0;
}

//EnterCriticalSection(&m_SessionAllocIdQueueCS);
//if (!m_SessionAllocIdQueue.empty()
bool CLanServer::OnConnectionRequest(/*IP, Port*/) {
	bool ret = true;
	EnterCriticalSection(&m_SessionAllocIdQueueCS);
	if (m_SessionAllocIdQueue.empty()) {
		ret = false;
	}
	LeaveCriticalSection(&m_SessionAllocIdQueueCS);

	return ret;
}


#if defined(ALLOC_BY_TLS_MEM_POOL)
void CLanServer::OnDeleteSendPacket(uint64 sessionID, JBuffer& sendRingBuffer)
{
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
		m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendPacekt, to_string(sessionID) + ", FreeMem (DeleteSession)");
	}

#if defined(SESSION_SENDBUFF_SYNC_TEST)
	ReleaseSRWLockExclusive(&session->sendBuffSRWLock);
#endif
}
#else
void CLanServer::OnDeleteSendPacket(UINT64 sessionID, std::vector<std::shared_ptr<JBuffer>>& sendBufferVec)
{
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
		BYTE Pn = payloads[i - 1] ^ (Pb + randKey + (BYTE)(i + 1));
		BYTE En = Pn ^ (Eb + dfPACKET_KEY + (BYTE)(i + 1));

		payloads[i - 1] = En;

		Pb = Pn;
		Eb = En;
	}
}
bool CLanServer::Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, BYTE* payloads) {
	BYTE Pb = checkSum ^ (dfPACKET_KEY + 1);
	BYTE payloadSum = Pb ^ (randKey + 1);
	BYTE Eb = checkSum;

	for (USHORT i = 1; i <= payloadLen; i++) {
		BYTE Pn = payloads[i - 1] ^ (Eb + dfPACKET_KEY + (BYTE)(i + 1));
		BYTE Dn = Pn ^ (Pb + randKey + (BYTE)(i + 1));

		Pb = Pn;
		Eb = payloads[i - 1];
		payloads[i - 1] = Dn;
	}

	// checksum ����
	BYTE payloadSumCmp = 0;
	for (USHORT i = 0; i < payloadLen; i++) {
		payloadSumCmp += payloads[i];
		payloadSumCmp %= 256;
	}
	if (payloadSum != payloadSumCmp) {
		DebugBreak();
		return false;
	}

	return true;
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
	system("cls");   // �ܼ� â �����

	static size_t logCnt = 0;

	static COORD coord;
	coord.X = 0;
	coord.Y = 0;
	SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);

	ServerConsoleLog();

#if defined(SESSION_LOG)
	std::cout << "Total Accept: " << m_TotalAcceptCnt << std::endl;
	std::cout << "Total Login : " << m_TotalLoginCnt << std::endl;
	std::cout << "Total Delete: " << m_TotalDeleteCnt << std::endl;
#endif
#if defined(SENDBUFF_MONT_LOG)
	std::cout << "[�ִ� �۽� ���� ��� ũ��]: " << m_SendBuffOfMaxSize << "                                                " << std::endl;
	std::cout << "[�ִ� �۽� ���� ��� ����]: " << m_SessionOfMaxSendBuff << "                                                " << std::endl;
#endif

#if defined(ALLOC_BY_TLS_MEM_POOL)
	size_t totalAllocMemCnt = m_SerialBuffPoolMgr.GetTotalAllocMemCnt();
	size_t totalFreeMemCnt = m_SerialBuffPoolMgr.GetTotalFreeMemCnt();
	size_t totalIncrementRefCnt = m_SerialBuffPoolMgr.GetTotalIncrementRefCnt();
	size_t totalDecrementRefCnt = m_SerialBuffPoolMgr.GetTotalDecrementRefCnt();
	std::unordered_map<DWORD, stMemoryPoolUseInfo> memInfos = m_SerialBuffPoolMgr.GetMemInfo();
#endif
	
	std::cout << "[m_SessionAllocIdQueue size] "  << m_SessionAllocIdQueue.size() << "                            " << std::endl;
	std::cout << "[m_CreatedSession size] " << m_CreatedSession.size() << "                            " << std::endl;

	std::cout << "[Log Count] " << logCnt++ << "                                                " << std::endl;
#if defined(ALLOC_BY_TLS_MEM_POOL)
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
#endif
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
#endif

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
	for (USHORT i = 0; i <= m_SessionLogIndex; i++) {
		//m_SessionLog[i].sessionID
		
		if (m_SessionLog[i].sessionID == 0) {
			break;
		}
		//if (m_CreatedSession.find(m_SessionLog[i].sessionID) == m_CreatedSession.end()) {
		//	continue;
		//}

		outputFile << "-------------------------------------------------" << std::endl;
		switch (m_SessionLog[i].sessionWork)
		{
		case SESSION_CREATE:
		{
			outputFile << "[Create Session] " << std::endl; 

		}
		break;
		case SESSION_RELEASE:
		{
			outputFile << "[Release Session] " << std::endl;
			outputFile << "Log: " << m_SessionLog[i].log << std::endl;
			if (m_SessionLog[i].workDone) {
				outputFile << "Work Done: " << "Success" << std::endl;
			}
			else {
				outputFile << "Work Done: " << "Fail" << std::endl;
			}
		}
		break;
		case SESSION_DISCONNECT:
		{
			outputFile << "[Discconect Session] " << std::endl;
			outputFile << "Log: " << m_SessionLog[i].log << std::endl;
			if (m_SessionLog[i].workDone) {
				outputFile << "Work Done: " << "Success" << std::endl;
			}
		}
		break;
		case SESSION_ACCEPT_WSARECV:
		{
			outputFile << "[Accept_WSARecv Session] " << std::endl;
		}
		break;
		case SESSION_RETURN_GQCS_Disconn:
		{
			outputFile << "[RETURN GQCS_DISCONN Session] " << std::endl;
			if (m_SessionLog[i].workDone) {
				outputFile << "Work Done: " << "Success" << std::endl;
			}
			else {
				outputFile << "Work Done: " << "Fail" << std::endl;
			}
		}
		break;
		case SESSION_RETURN_GQCS:
		{
			outputFile << "[RETURN GQCS Session] " << std::endl;
			if (m_SessionLog[i].workDone) {
				outputFile << "Work Done: " << "Success" << std::endl;
			}
			else {
				outputFile << "Work Done: " << "Fail" << std::endl;
			}
		}
		break;
		case SESSION_COMPLETE_RECV:
		{
			outputFile << "[Complete Recv Session] " << std::endl;
		}
		break;
		case SESSION_COMPLETE_SEND:
		{
			outputFile << "[Complete Send Session] " << std::endl;
		}
		break;
		case SESSION_WSARECV:
		{
			outputFile << "[WSARecv Session] " << std::endl;
			if (m_SessionLog[i].workDone) {
				outputFile << "Work Done: " << "Success" << std::endl;
			}
			else {
				if (m_SessionLog[i].ioPending) {
					outputFile << "IO Pending: " << "True" << std::endl;
				}
				else {
					outputFile << "IO Pending: " << "False" << std::endl;
				}
			}
		}
		break;
		case SESSION_WSASEND:
		{
			outputFile << "[WSASend Session] " << std::endl;
			if (m_SessionLog[i].workDone) {
				outputFile << "Work Done: " << "Success" << std::endl;
			}
			else {
				if (m_SessionLog[i].ioPending) {
					outputFile << "IO Pending: " << "True" << std::endl;
				}
				else {
					outputFile << "IO Pending: " << "False" << std::endl;
				}
			}
		}
		break;
		case SESSION_ACQUIRE:
		{
			outputFile << "[Acquire Session] " << std::endl;
			if (m_SessionLog[i].workDone) {
				outputFile << "Work Done: " << "Success" << std::endl;
			}
			else {
				outputFile << "Work Done: " << "Fail" << std::endl;
			}
		}
		break;
		case SESSION_RETURN:
		{
			outputFile << "[Return Session] " << std::endl;
			if (m_SessionLog[i].workDone) {
				outputFile << "Work Done: " << "Success" << std::endl;
			}
			else {
				outputFile << "Work Done: " << "Fail" << std::endl;
			}

		}
		break;
		case SESSION_AFTER_SENDPOST:
		{
			outputFile << "[After SendPost Session] " << std::endl;
		}
		break;
		default:
			break;
		}

		outputFile << "sessionID: " << to_string(m_SessionLog[i].sessionID) << std::endl;
		outputFile << "Index:        " << m_SessionLog[i].sessionIndex << std::endl;
		outputFile << "IO Cnt:       " << m_SessionLog[i].iocnt << std::endl;
		outputFile << "Release Flag: " << m_SessionLog[i].releaseFlag << std::endl;
	}

	//////////////////////////////////////////

	// ���� �ݱ�
	outputFile.close();

	std::cout << "������ �����Ǿ����ϴ�: " << filePath << std::endl;
}