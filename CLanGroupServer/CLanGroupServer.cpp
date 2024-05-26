#include "CLanGroupServer.h"

void CLanGroupServer::EnterSessionGroup(SessionID sessionID, GroupID enterGroup)
{
	AcquireSRWLockExclusive(&m_SessionGroupIDSrwLock);
	if (m_SessionGroupID.find(sessionID) != m_SessionGroupID.end()) {
		DebugBreak();
	}
	m_SessionGroupID.insert({ sessionID, enterGroup });
	ReleaseSRWLockExclusive(&m_SessionGroupIDSrwLock);
}

void CLanGroupServer::ForwardSessionGroup(SessionID sessionID, GroupID from, GroupID to)
{
	AcquireSRWLockExclusive(&m_SessionGroupIDSrwLock);
	if (m_SessionGroupID.find(sessionID) != m_SessionGroupID.end()) {
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
	size_t copyLen = RecvData(recvBuff, *recvData);
	recvBuff.DirectMoveEnqueueOffset(copyLen);

	m_GroupRecvData[groupID].recvQueueMtx.lock();
	// 2. 큐 삽입
	m_GroupRecvData[groupID].recvQueue.push({ sessionID, recvData });
	// 3. 이벤트 On
	SetEvent(m_GroupRecvData[groupID].recvEvent);
	m_GroupRecvData[groupID].recvQueueMtx.unlock();
}
