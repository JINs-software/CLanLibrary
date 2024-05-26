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

	// case1) �׷� �����忡 ����
	// groupThread.OnRecv(sessionID, recvBuff);

	// case2) �׷� �����尡 ����ϴ� IOCP ť�� Post �۽�
	//			-> GetOverlappedResult�� ����?

	// case3) �׷� �� ���� �ɾ� IOCP �۾��� �����尡 ������ �����ϵ��� ��.


	// �̺�Ʈ ���� ���
	// 1. ������ ����
	std::shared_ptr<JBuffer> recvData = std::make_shared<JBuffer>();
	size_t copyLen = RecvData(recvBuff, *recvData);
	recvBuff.DirectMoveEnqueueOffset(copyLen);

	m_GroupRecvData[groupID].recvQueueMtx.lock();
	// 2. ť ����
	m_GroupRecvData[groupID].recvQueue.push({ sessionID, recvData });
	// 3. �̺�Ʈ On
	SetEvent(m_GroupRecvData[groupID].recvEvent);
	m_GroupRecvData[groupID].recvQueueMtx.unlock();
}
