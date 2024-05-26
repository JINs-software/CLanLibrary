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
	// 세션 - 세션 그룹 식별 자료구조
	std::unordered_map<SessionID, GroupID>	m_SessionGroupID;
	SRWLOCK									m_SessionGroupIDSrwLock;

	// 그룹 별 이벤트
	std::map<GroupID, stGroupRecvData>		m_GroupRecvData;

public:
	// 그룹 생성 (그룹 식별자 반환, 라이브러리와 컨텐츠는 그룹 식별자를 통해 식별)
	

	// 그룹 이동
	void EnterSessionGroup(SessionID sessionID, GroupID enterGroup);
	void ForwardSessionGroup(SessionID sessionID, GroupID from, GroupID to);

	void PostMsgToThreadGroup(GroupID groupID, JBuffer& msg);

protected:
	virtual bool OnWorkerThreadCreate(HANDLE thHnd) { return true; };
	virtual void OnWorkerThreadCreateDone() {};
	virtual void OnWorkerThreadStart() {};
	virtual void OnWorkerThreadEnd() {};
	virtual bool OnConnectionRequest(/*IP, Port*/);

	// 그룹 식별
	// ex) DB로부터 그룹을 식별한다.
	virtual void OnClientJoin(SessionID sessionID) = 0;

	virtual void OnClientLeave(SessionID sessionID) = 0;

	// 세션 그룹을 분류함
	virtual void OnRecv(SessionID sessionID, JBuffer& recvBuff);

	virtual void RecvData(JBuffer& recvBuff, JBuffer& dest) = 0;
};

