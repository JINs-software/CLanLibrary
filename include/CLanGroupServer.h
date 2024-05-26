#pragma once
#include "CLanServer.h"

using SessionID = UINT64;
using GroupID = UINT16;

struct stSessionRecvBuff {
	SessionID	sessionID;
	std::shared_ptr<JBuffer> recvData;
};

struct stGroupRecvData {
	std::queue<stSessionRecvBuff>	recvQueue;
	HANDLE							recvEvent;
	std::mutex						recvQueueMtx;
};

class SessionGroupThread {

	//void 
};

class CLanGroupServer : public CLanServer
{
private:
	// ���� - ���� �׷� �ĺ� �ڷᱸ��
	std::unordered_map<SessionID, GroupID>	m_SessionGroupID;
	SRWLOCK									m_SessionGroupIDSrwLock;

	// �׷� �� �̺�Ʈ
	std::map<GroupID, stGroupRecvData>		m_GroupRecvData;

public:
	// �׷� ���� (�׷� �ĺ��� ��ȯ, ���̺귯���� �������� �׷� �ĺ��ڸ� ���� �ĺ�)
	

	// �׷� �̵�
	void EnterSessionGroup(SessionID sessionID, GroupID enterGroup);
	void ForwardSessionGroup(SessionID sessionID, GroupID from, GroupID to);

	void PostMsgToThreadGroup(GroupID groupID, JBuffer& msg);

protected:
	virtual bool OnWorkerThreadCreate(HANDLE thHnd) { return true; };
	virtual void OnWorkerThreadCreateDone() {};
	virtual void OnWorkerThreadStart() {};
	virtual void OnWorkerThreadEnd() {};
	virtual bool OnConnectionRequest(/*IP, Port*/);

	// �׷� �ĺ�
	// ex) DB�κ��� �׷��� �ĺ��Ѵ�.
	virtual void OnClientJoin(SessionID sessionID) = 0;

	virtual void OnClientLeave(SessionID sessionID) = 0;

	// ���� �׷��� �з���
	virtual void OnRecv(SessionID sessionID, JBuffer& recvBuff);

	virtual void RecvData(JBuffer& recvBuff, JBuffer& dest) = 0;
};

