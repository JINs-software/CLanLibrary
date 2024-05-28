#include "CLanGroupServer.h"

UINT __stdcall CLanGroupThread::SessionGroupThreadFunc(void* arg)
{
	CLanGroupThread* groupthread = (CLanGroupThread*)arg;

	HANDLE events[2] = { groupthread->m_SessionGroupThreadStopEvent, groupthread->m_RecvEvent };
	while (true) {
		DWORD ret = WaitForMultipleObjects(2, events, false, INFINITE);
		if (ret == WAIT_OBJECT_0) {
			break;
		}
		else if(ret == WAIT_OBJECT_0 + 1) {
			while (true) {
				bool isEmpty = true;
				stSessionRecvBuff recvBuff;
				groupthread->m_RecvQueueMtx.lock();
				if (!groupthread->m_RecvQueue.empty()) {
					recvBuff = groupthread->m_RecvQueue.front();
					groupthread->m_RecvQueue.pop();
					isEmpty = false;
				}
				groupthread->m_RecvQueueMtx.unlock();

				if (!isEmpty) {
					groupthread->OnRecv(recvBuff.sessionID, *recvBuff.recvData);
				}
				else {
					break;
				}
			}
		}
		else {
			DebugBreak();
		}
	}
	return 0;
}

void CLanGroupThread::Disconnect(uint64 sessionID) {
	m_ClanGroupServer->Disconnect(sessionID);
}
bool CLanGroupThread::SendPacket(uint64 sessionID, JBuffer* sendDataPtr) {
#if defined(ALLOC_BY_TLS_MEM_POOL)
	return m_ClanGroupServer->SendPacket(sessionID, sendDataPtr);
#else
	shared_ptr<JBuffer> sptr = make_shared<JBuffer>(sendDataPtr);
	return m_ClanGroupServer->SendPacket(sessionID, sptr);
#endif
}
void CLanGroupThread::ForwardSessionGroup(SessionID sessionID, GroupID to) {
	m_ClanGroupServer->ForwardSessionGroup(sessionID, m_GroupID, to);
}
void CLanGroupThread::PostMsgToThreadGroup(GroupID groupID, JBuffer& msg) {
	m_ClanGroupServer->PostMsgToThreadGroup(groupID, msg);
}

void CLanGroupThread::Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads) {
	m_ClanGroupServer->Encode(randKey, payloadLen, checkSum, payloads);
}
bool CLanGroupThread::Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, BYTE* payloads) {
	return m_ClanGroupServer->Decode(randKey, payloadLen, checkSum, payloads);
}

void CLanGroupServer::CreateGroup(GroupID newGroupID, CLanGroupThread* groupThread)
{
	if (m_GroupThreads.find(newGroupID) != m_GroupThreads.end()) {
		DebugBreak();
	}
	groupThread->SetServer(this, newGroupID);
	m_GroupThreads.insert({ newGroupID, groupThread });
}

void CLanGroupServer::DeleteGroup(GroupID delGroupID)
{
	if (m_GroupThreads.find(delGroupID) == m_GroupThreads.end()) {
		DebugBreak();
	}
	delete m_GroupThreads[delGroupID];
	m_GroupThreads.erase(delGroupID);
}

void CLanGroupServer::EnterSessionGroup(SessionID sessionID, GroupID enterGroup)
{
	AcquireSRWLockExclusive(&m_SessionGroupIDSrwLock);
	if (m_SessionGroupID.find(sessionID) != m_SessionGroupID.end()) {
		DebugBreak();
	}
	m_SessionGroupID.insert({ sessionID, enterGroup });
	ReleaseSRWLockExclusive(&m_SessionGroupIDSrwLock);
}

void CLanGroupServer::LeaveSessionGroup(SessionID sessionID)
{
	AcquireSRWLockExclusive(&m_SessionGroupIDSrwLock);
	if (m_SessionGroupID.find(sessionID) == m_SessionGroupID.end()) {
		DebugBreak();
	}
	m_SessionGroupID.erase(sessionID);
	ReleaseSRWLockExclusive(&m_SessionGroupIDSrwLock);
}

void CLanGroupServer::ForwardSessionGroup(SessionID sessionID, GroupID from, GroupID to)
{
	AcquireSRWLockExclusive(&m_SessionGroupIDSrwLock);
	if (m_SessionGroupID.find(sessionID) == m_SessionGroupID.end()) {
		DebugBreak();
	}
	m_SessionGroupID[sessionID] = to;
	ReleaseSRWLockExclusive(&m_SessionGroupIDSrwLock);
}

void CLanGroupServer::PostMsgToThreadGroup(GroupID, JBuffer& msg)
{
}


void CLanGroupServer::OnRecv(UINT64 sessionID, JBuffer& recvBuff)
{
	AcquireSRWLockShared(&m_SessionGroupIDSrwLock);
	if (m_SessionGroupID.find(sessionID) == m_SessionGroupID.end()) {
		DebugBreak();
	}
	UINT16 groupID = m_SessionGroupID[sessionID];
	ReleaseSRWLockShared(&m_SessionGroupIDSrwLock);

	// case1) 그룹 스레드에 전달
	// groupThread.OnRecv(sessionID, recvBuff);

	// case2) 그룹 스레드가 대기하는 IOCP 큐에 Post 송신
	//			-> GetOverlappedResult가 적절?

	// case3) 그룹 간 락만 걸어 IOCP 작업자 스레드가 로직을 수행하도록 함.


	// 이벤트 깨움 방식
	// 1. 데이터 복사
	std::shared_ptr<JBuffer> recvData = std::make_shared<JBuffer>();
#if defined(CALCULATE_TRANSACTION_PER_SECOND)
	UINT recvCnt = RecvData(recvBuff, *recvData);
	InterlockedAdd(&m_CalcTpsItems[RECV_TRANSACTION], recvCnt);
	InterlockedAdd(&m_TotalTransaction[RECV_TRANSACTION], recvCnt);
#else 
	RecvData(recvBuff, *recvData);
#endif

	stSessionRecvBuff sessionRecvBuff{ sessionID, recvData };
	m_GroupThreads[groupID]->PushRecvBuff(sessionRecvBuff);
}

