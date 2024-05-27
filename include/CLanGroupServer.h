#pragma once
#include "CLanServer.h"

using SessionID = UINT64;
using GroupID = UINT16;

struct stSessionRecvBuff {
	SessionID	sessionID;
	std::shared_ptr<JBuffer> recvData;
};



class SessionGroupThread {
private:
	std::queue<stSessionRecvBuff>	m_RecvQueue;
	HANDLE							m_RecvEvent;
	std::mutex						m_RecvQueueMtx;

	HANDLE							m_SessionGroupThread;
	HANDLE							m_SessionGroupThreadStopEvent;

	CLanGroupServer*				m_ClanGroupServer;

protected:
	GroupID							m_GroupID;
	
public:
	SessionGroupThread()
	{
		m_RecvEvent = CreateEvent(NULL, false, false, NULL);
		m_SessionGroupThreadStopEvent = CreateEvent(NULL, false, false, NULL);
		m_SessionGroupThread = (HANDLE)_beginthreadex(NULL, 0, SessionGroupThreadFunc, this, 0, NULL);
	}
	~SessionGroupThread() {
		SetEvent(m_SessionGroupThreadStopEvent);
		WaitForSingleObject(m_SessionGroupThread, INFINITE);
	}
	void SetServer(CLanGroupServer* clanGroupServer, GroupID groupID) {
		m_ClanGroupServer = clanGroupServer;
		m_GroupID = groupID;
	}

	void PushRecvBuff(stSessionRecvBuff& recvBuff) {
		m_RecvQueueMtx.lock();
		m_RecvQueue.push(recvBuff);
		m_RecvQueueMtx.unlock();

		SetEvent(m_RecvEvent);
	}

private:
	// 이벤트를 받고, 메시지를 읽어 OnRecv 호출
	static UINT __stdcall SessionGroupThreadFunc(void* arg);

protected:
	void Disconnect(uint64 sessionID) {
		m_ClanGroupServer->Disconnect(sessionID);
	}
	bool SendPacket(uint64 sessionID, JBuffer* sendDataPtr) {
#if defined(ALLOC_BY_TLS_MEM_POOL)
		m_ClanGroupServer->SendPacket(sessionID, sendDataPtr);
#else
		shared_ptr<JBuffer> sptr = make_shared<JBuffer>(sendDataPtr);
		m_ClanGroupServer->SendPacket(sessionID, sptr);
#endif
	}
	void ForwardSessionGroup(SessionID sessionID, GroupID to) {
		m_ClanGroupServer->ForwardSessionGroup(sessionID, m_GroupID, to);
	}
	void PostMsgToThreadGroup(GroupID groupID, JBuffer& msg) {
		m_ClanGroupServer->PostMsgToThreadGroup(groupID, msg);
	}


	virtual void OnStart() {};
	virtual void OnRecv(SessionID sessionID, JBuffer& recvData) {};
	virtual void OnStop() {};
};

class CLanGroupServer : public CLanServer
{
private:
	// 세션 - 세션 그룹 식별 자료구조
	std::unordered_map<SessionID, GroupID>	m_SessionGroupID;
	SRWLOCK									m_SessionGroupIDSrwLock;

	// 그룹 별 이벤트
	std::map<GroupID, SessionGroupThread*>	m_GroupThreads;

public:
	// 그룹 생성 (그룹 식별자 반환, 라이브러리와 컨텐츠는 그룹 식별자를 통해 식별)
	void CreateGroup(GroupID newGroupID, SessionGroupThread* groupThread);
	void DeleteGroup(GroupID delGroupID);

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
	virtual void OnClientJoin(SessionID sessionID) {};

	virtual void OnClientLeave(SessionID sessionID) {};

	// 세션 그룹을 분류함
	virtual void OnRecv(SessionID sessionID, JBuffer& recvBuff);

	virtual void RecvData(JBuffer& recvBuff, JBuffer& dest) {};
};

