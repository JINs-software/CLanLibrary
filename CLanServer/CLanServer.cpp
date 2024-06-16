#include "CLanServer.h"
#include <cassert>
#include <process.h>
#include <fstream>
#include <cstdlib> // system �Լ��� ����ϱ� ���� �ʿ�

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
	// ��Ʈ��ũ �ʱ�ȭ
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
	// ���� ���� �ʱ�ȭ
	////////////////////////////////////////////////////////////////////////////////////////////////////
	for (uint16 idx = 1; idx <= m_MaxOfSessions; idx++) {		// sessionID�� �ε��� �� ���� 0�̶�� ���� �������� ���� �����̶�� ���ʰ�
		m_SessionAllocIdQueue.push(idx);
	}
	m_Sessions.resize(m_MaxOfSessions + 1, NULL);
	for (uint16 idx = 1; idx <= m_MaxOfSessions; idx++) {
		m_Sessions[idx] = new stCLanSession(m_SessionRecvBufferSize);
	}
	InitializeCriticalSection(&m_SessionAllocIdQueueCS);

	////////////////////////////////////////////////////////////////////////////////////////////////////
	// IOCP ��ü �ʱ�ȭ
	////////////////////////////////////////////////////////////////////////////////////////////////////
	m_IOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, numOfIocpConcurrentThrd);
#if defined(CLANSERVER_ASSERT)
	if (m_IOCP == NULL) {
		DebugBreak();
	}
#endif

	////////////////////////////////////////////////////////////////////////////////////////////////////
	// ������ ���� �ʱ�ȭ
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
	// ���� ���� ���ε�
	if (BindSocket(m_ListenSock, m_ListenSockAddr) == SOCKET_ERROR) {
		return false;
	}
	// ���� ���� Accept �غ�
	if (ListenSocket(m_ListenSock, SOMAXCONN) == SOCKET_ERROR) {
		return false;
	}

	// ���� �ɼ� �߰� (���� ���� ����)
	struct linger linger_option;
	linger_option.l_onoff = 1;  // SO_LINGER ���
	linger_option.l_linger = 0; // ������ ���� �� �ִ� ��� �ð� (��)
	setsockopt(m_ListenSock, SOL_SOCKET, SO_LINGER, (const char*)&linger_option, sizeof(linger_option));

	////////////////////////////////////////////////////////////////////////////////////////////////////
	// ������ ����
	////////////////////////////////////////////////////////////////////////////////////////////////////
	// Accept ������ ����
	m_AcceptThread = (HANDLE)_beginthreadex(NULL, 0, CLanServer::AcceptThreadFunc, this, 0, NULL);
	cout << "[Start Thread] Accept Thread" << endl;

	// IOCP �۾��� ������ ����
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

	// ��� �����尡 ������ �� ȣ��, ������ ������� �۾� ��...
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

	// 4. �ܿ� Ŭ���̾�Ʈ ��� ���� ����
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
bool CLanServer::SendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded, bool postToWorker) {	// sendDataPtr�� deqOffset�� 0���� ����

	stCLanSession* session = AcquireSession(sessionID);
	if (session != nullptr) {				// �ε����� ������ �ٸ� �����̰ų� ���ŵ�(��������) ����

		if (!encoded) {
			///////////////////////////////////////////// ���ڵ� ���� //////////////////////////////////////////////////
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
	if (session != nullptr) {				// �ε����� ������ �ٸ� �����̰ų� ���ŵ�(��������) ����

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
	if (session != nullptr) {				// �ε����� ������ �ٸ� �����̰ų� ���ŵ�(��������) ����

		if (!encoded) {
			///////////////////////////////////////////// ���ڵ� ���� //////////////////////////////////////////////////
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
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� �����͸� Enqueue�� ���� ������ ����" << endl;
			DebugBreak();
		}
		uint32 enqSize = session->sendRingBuffer.Enqueue((BYTE*)&sendDataPtr, sizeof(UINT_PTR));
		if (enqSize < sizeof(UINT_PTR)) {
			// �۽� ��-���ۿ� �۽� �����͸� ������ �� ������ Ȯ���������� �ұ��ϰ�,
			// Enqueue ����� �۽� �������� ũ�⺸�� ���� ��Ȳ �߻�
			cout << "[ERROR, SendPacket] �۽� ��-���ۿ� �۽� ������ ��ü Enqueue ����" << endl;
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
	// AcquireSession ȣ�� ����ȭ
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
	stCLanSession* session = m_Sessions[idx];					// ���� ID�� �ε��� ��Ʈ�� ���� ���� ȹ��
	if (session == nullptr) {									// AcquireSession�� ȣ���ϴ� �������� ã���� �Ͽ��� ������ ȹ���Ͽ��ٴ� ������ �� �� ����
																// (�̹� ������ �����̰ų�, ������ �� ���� �ε��� �ڸ��� ��Ȱ��� ������ �� ����)
#if defined(CLANSERVER_ASSERT)
		DebugBreak();
#endif
		return nullptr;
	}
	else {
		///////////////////////////////////////////////////////////////////
		// ���� ���� ī��Ʈ(ioCnt) ����!
		uint32 uiRef = InterlockedIncrement((uint32*)&session->sessionRef);
		///////////////////////////////////////////////////////////////////
		stSessionRef sessionRef = *((stSessionRef*)&uiRef);
#if defined(CLANSERVER_ASSERT)
		if (sessionRef.ioCnt < 1) {
			DebugBreak();
		}
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
				Disconnect(session->uiId);
			}
			else if (sessionRef.ioCnt == 0 && sessionRef.releaseFlag == 1) {
				// ���ο� ���� ���� ��
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
		// ���ο� ���� ���� ��
		// nothing to do..
	}
#if defined(CLANSERVER_ASSERT)
	else if (sessionRef.ioCnt < 0) {
		DebugBreak();
	}
#endif

	// (1) ioCnt == 1�� ��Ȳ, �� ���� AcquireSession�� ���� ������ ioCnt�� ���Ҵٴ� ��
	// (2) ioCnt == 1�� ��Ȳ���� InterlockedDecrement�� ���� 0���� ����Ǿ��� ��,
	//     �ٸ� iocp �۾��� �����忡 ���� ���ҵ� ��Ȳ�� �����. �̹� ioCnt == 0�� ��Ȳ���� AcquireSession���� 1�� �� ��Ȳ�̱⿡
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
		session->clearSendOverlapped();	// �۽ſ� overlapped ����ü �ʱ�ȭ

		
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
				// => single reader ����
				session->sendBufferQueue.Dequeue(msgPtr, true);

				//session->sendPostedQueue.push(msgPtr);
				// => push ������� ����
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
			session->sendOverlapped.Offset = sendLimit;		// Offset ����� Ȱ���Ͽ� �۽��� �޽��� ������ �㵵�� �Ѵ�.

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
	// SendPost�� ���������� ���� ioCnt >= 1�� ����� ���¿��� ȣ���� ������.
	stSessionID orgID = *((stSessionID*)&sessionID);
	stCLanSession* session = m_Sessions[orgID.idx];

	// ���� �۽� �÷��� off ���¿����� Post�� �����Ͽ� iocp ť�� ���ϸ� ���ҽ�Ų��. 
	// iocp �۾��� �����忡�� �ش� ��û�� ������, 
	// onSendFlag on ���·� SendPost ȣ��, �׸��� ioCnt�� ���ҽ�Ű�� ���� ���� �Ǵ��� ����
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

		// ���� ����
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

		// ���� ID �ε��� ��ȯ
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
	clanserver->m_SerialBuffPoolMgr.AllocTlsMemPool(clanserver->m_TlsMemPoolDefaultUnitCnt, clanserver->m_TlsMemPoolDefaultUnitCapacity, clanserver->m_SerialBufferSize);	// �����ڿ��� ������ Default ���� ����
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
				// ���� ����
				stCLanSession* newSession = clanserver->CreateNewSession(clientSock);
				if (newSession != nullptr) {
#if defined(CALCULATE_TRANSACTION_PER_SECOND)
					clanserver->m_CalcTpsItems[ACCEPT_TRANSACTION]++;
					clanserver->m_TotalTransaction[ACCEPT_TRANSACTION]++;
#endif

					// ���� ���� �̺�Ʈ
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
					// Zero byte recv �׽�Ʈ
					//wsabuf.len = 0;
					DWORD dwFlag = 0;

					//newSession->ioCnt = 1;
					// => ���� Release ���� ����(24.04.08) ����
					// ���� Init �Լ����� IOCnt�� 1�� �ʱ�ȭ�ϴ� ���� �´µ�..
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
	clanserver->m_SerialBuffPoolMgr.AllocTlsMemPool(clanserver->m_TlsMemPoolDefaultUnitCnt, clanserver->m_TlsMemPoolDefaultUnitCapacity, clanserver->m_SerialBufferSize);	// �����ڿ��� ������ Default ���� ����
#endif

	while (true) {
		DWORD transferred = 0;
		ULONG_PTR completionKey;
		WSAOVERLAPPED* overlappedPtr;
		GetQueuedCompletionStatus(clanserver->m_IOCP, &transferred, (PULONG_PTR)&completionKey, &overlappedPtr, INFINITE);
		// transffered == 0���� ���� �б⸦ ������ ���� ������? transferred�� session(lpCompletionKey)�� ���� �ʱ�ȭ�� �Ź� �����ϰ� ���� �ʾƼ� 
		if (overlappedPtr != NULL) {
			/////////////////////////////////////////////////////////////////////////////////////////////
			// [�Ϸ� ����] IOCP_COMPLTED_LPOVERLAPPED_DISCONNECT: iocp �۾��� ������ �� �������� Disconnect ��û ����
			/////////////////////////////////////////////////////////////////////////////////////////////
			if (overlappedPtr == (LPOVERLAPPED)IOCP_COMPLTED_LPOVERLAPPED_DISCONNECT) {
				// ���� ���� �Ǵ�
				stCLanSession* session = (stCLanSession*)completionKey;

				// ���� ���� �Ǵ�
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
					// ���� ����...
					uint64 sessionID = session->uiId;
					if (clanserver->DeleteSession(sessionID)) {
						clanserver->OnClientLeave(sessionID);
					}
				}
			}

			/////////////////////////////////////////////////////////////////////////////////////////////
			// [�Ϸ� ����] IOCP_COMPLTED_LPOVERLAPPED_SENDPOST_REQ: SendPostRequest�� ���� ť�׵� �۽� ��û
			/////////////////////////////////////////////////////////////////////////////////////////////
			else if (overlappedPtr == (LPOVERLAPPED)IOCP_COMPLTED_LPOVERLAPPED_SENDPOST_REQ) {
				stCLanSession* session = (stCLanSession*)completionKey;
				// IOCP �۾��� ������ ������ SendPost ȣ��
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
					// ���� ���� ���� �Ǵ�
					uint64 sessionID = session->uiId;
					if (clanserver->DeleteSession(sessionID)) {
						clanserver->OnClientLeave(sessionID);
					}
				}

			}
			else if (transferred == 0) {
				// ���� ���� �Ǵ�
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
					// ���� ����...
					uint64 sessionID = session->uiId;
					if (clanserver->DeleteSession(sessionID)) {
						clanserver->OnClientLeave(sessionID);
					}
				}
			}
			else {
				//////////////////////////////////////////////////////////////////////////////
				// [�Ϸ� ����] ���� �Ϸ�
				//////////////////////////////////////////////////////////////////////////////
				stCLanSession* session = (stCLanSession*)completionKey;
				if (&(session->recvOverlapped) == overlappedPtr) {
					session->recvRingBuffer.DirectMoveEnqueueOffset(transferred);
					UINT recvBuffSize = session->recvRingBuffer.GetUseSize();
					if (recvBuffSize > clanserver->m_MaxOfRecvBufferSize) {
						clanserver->m_MaxOfRecvBufferSize = recvBuffSize;
					}

					//clanserver->OnRecv(session->uiId, session->recvRingBuffer);	// OnRecv �Լ������� ���� �۽��� �����Ѵ�. 
					// => ���� ��� ���̺귯�� ������ �̵�, ���ڵ� ����
					clanserver->ProcessReceiveMessage(session->uiId, session->recvRingBuffer);

					session->clearRecvOverlapped();
					WSABUF wsabuf;
					wsabuf.buf = (CHAR*)session->recvRingBuffer.GetEnqueueBufferPtr();
					wsabuf.len = session->recvRingBuffer.GetDirectEnqueueSize();
					DWORD dwflag = 0;

					if (wsabuf.len == 0) {
#if defined(CLANSERVER_ASSERT)
						// 0 ����Ʈ ���� ��û�� �߻��ϴ��� Ȯ��
						DebugBreak();
#else
						// �ӽ� ��ġ (�ҿ���)
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
								// ���� ����
								uint64 sessionID = session->uiId;
								if (clanserver->DeleteSession(sessionID)) {
									clanserver->OnClientLeave(sessionID);
								}
							}
						}
					}
				}
				//////////////////////////////////////////////////////////////////////////////
				// [�Ϸ� ����] �۽� �Ϸ�
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
					// �۽� �Ϸ�� ����ȭ ���� ��ť�� �� �޸� ��ȯ
					AcquireSRWLockExclusive(&session->sendBuffSRWLock);
					for (int i = 0; i < session->sendOverlapped.Offset; i++) {
						JBuffer* sendBuff;
						session->sendRingBuffer >> sendBuff;
						//clanserver->m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendBuff, to_string(session->uiId) + ", FreeMem (�۽� �Ϸ�)");
						clanserver->m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendBuff);
					}
					ReleaseSRWLockExclusive(&session->sendBuffSRWLock);
#endif
#else 
					// �۽� �Ϸ�� ����ȭ ���� ��ť�� �� �޸� ��ȯ
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
					//	// ���� ���� ���� �Ǵ�
					//	uint64 sessionID = session->uiId;
					//	if (clanserver->DeleteSession(sessionID)) {
					//		clanserver->OnClientLeave(sessionID);
					//	}
					//}
					//else {
					//	// ���� ���� ���� -> �߰����� �۽� ������ ���� �� SendPost ȣ��
					//	if (session->sendBufferQueue.GetSize() > 0) {
					//		clanserver->SendPost(session->uiId, true);
					//	}
					//	else {
					//		InterlockedExchange(&session->sendFlag, 0);
					//	}
					//}
					// => �۽� ���� ť�� ������ Ȯ�ΰ� sendFlag ������ ���������� ������� �ʴ� ���� �ĺ�
					// ex) �۽� ���� ť ������ == 0 ���� Ȯ���� ���� ����, �ٸ� �����忡�� �۽� ���� ��ť. ������ sendFlag�� ���� On�̱⿡ PostQueue ���� x
					//     �۽� �Ϸ� ó�� ������ ���͵Ǹ� �۽� ���� ť�� �۽� ��Ŷ�� �����Կ��� �ұ��ϰ� �۽����� ���ϴ� ��Ȳ �߻�
					// => ���� ��Ĵ�� sendFlag�� ���� off�ϴ� ����� �ʿ�

					// sendFlag Off
					InterlockedExchange(&session->sendFlag, 0);

					// ���� ���� ���� -> �߰����� �۽� ������ ���� �� SendPost ȣ��
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

					// SendPost������ ���� ��ȿ�� �Ǵ��� ���� �ʴ´�. ���� ioCnt >= 1�� ������ ���¿��� ȣ���� �� �۽� �Ϸ� �Ĺݺο� ioCnt�� ���ҽ��Ѿ� ��.
					uint32 uiRef = InterlockedDecrement((uint32*)&session->sessionRef);
					stSessionRef sessionRef = *((stSessionRef*)&uiRef);
					assert(sessionRef.ioCnt >= 0);
					if (sessionRef.ioCnt == 0) {
						// ���� ���� ���� �Ǵ�
						uint64 sessionID = session->uiId;
						if (clanserver->DeleteSession(sessionID)) {
							clanserver->OnClientLeave(sessionID);
						}
					}
				}
#if defined(CLANSERVER_ASSERT)
				else {
					// ���� �Ϸᵵ �۽� �Ϸᵵ �ƴ� �������� ��Ȳ
					DebugBreak();
				}
#endif
			}
		}
#if defined(CLANSERVER_ASSERT)
		else {
			// 1. IOCP ��ü ��ü�� ����
			// 2. GQCS ȣ�� �� INIFINTE �� ��� �ð��� �ɾ� ���� ���, ��� �ð� �� I/O�� �Ϸ���� ���� ���
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
		// TPS �׸� �б�
		for (int i = 0; i < NUM_OF_TPS_ITEM; i++) {
			clanserver->m_TpsItems[i] = clanserver->m_CalcTpsItems[i];
			//clanserver->m_TotalTransaction[i] += clanserver->m_CalcTpsItems[i];
		}

		// TPS �׸� ����

		// TPS �׸� �ʱ�ȭ
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
		// => �����۸� �����ϱ⿡ ����� ������ ���۰� �� ������ ���� ���� �� ����. Decode�� ���� �ĺ�(checkSum ����)
		//	  ���� ���� ����� ��� ������ Peek�� �ϴ� ���� ����
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
		// => �����۸� �����ϱ⿡ ����� ������ ���۰� �� ������ ���� ���� �� ����. Decode�� ���� �ĺ�(checkSum ����)
		//	  ���� ���� ����� ��� ������ Peek�� �ϴ� ���� ����
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
	system("cls");   // �ܼ� â �����

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
	// TLS �޸� Ǯ
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