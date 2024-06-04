#pragma once
#include "CLanServer.h"
#include "CLanGroupConfig.h"
#if defined(LOCKFREE_MESSAGE_QUEUE)
#include "LockFreeQueue.h"
#endif

using SessionID = UINT64;
using GroupID = UINT16;

struct stSessionRecvBuff {
	SessionID	sessionID;
	std::shared_ptr<JBuffer> recvData;
};

//#define RECV_BUFF_QUEUE
#define RECV_BUFF_LIST

class CLanGroupThread;

class CLanGroupServer : public CLanServer
{
public:
#if defined(ALLOC_BY_TLS_MEM_POOL)
	CLanGroupServer(const char* serverIP, uint16 serverPort, DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections)
		: CLanServer(serverIP, serverPort, numOfIocpConcurrentThrd, numOfWorkerThreads, maxOfConnections) {}
#else
	CLanGroupServer(const char* serverIP, uint16 serverPort, DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections,
					uint32 sessionSendBuffSize, uint32 sessionRecvBuffSize)
		: CLanServer(serverIP, serverPort, numOfIocpConcurrentThrd, numOfWorkerThreads, maxOfConnections, sessionSendBuffSize, sessionRecvBuffSize) {}
#endif

public:
	UINT16	m_ActiveGroupThread = 0;
	UINT16	m_Cnt = 0;

private:
	// ���� - ���� �׷� �ĺ� �ڷᱸ��
	std::unordered_map<SessionID, GroupID>	m_SessionGroupID;
	SRWLOCK									m_SessionGroupIDSrwLock;
	// �׷� �� �̺�Ʈ
	std::map<GroupID, CLanGroupThread*>		m_GroupThreads;

public:
	// �׷� ���� (�׷� �ĺ��� ��ȯ, ���̺귯���� �������� �׷� �ĺ��ڸ� ���� �ĺ�)
	void CreateGroup(GroupID newGroupID, CLanGroupThread* groupThread, bool anySessionMode);
	void DeleteGroup(GroupID delGroupID);

	// �׷� �̵�
	void EnterSessionGroup(SessionID sessionID, GroupID enterGroup);
	void LeaveSessionGroup(SessionID sessionID);
	void ForwardSessionGroup(SessionID sessionID, GroupID from, GroupID to);
	//void PostMsgToThreadGroup(GroupID groupID, JBuffer& msg);

protected:
	virtual bool OnWorkerThreadCreate(HANDLE thHnd) { return true; };
	virtual void OnWorkerThreadCreateDone() {};
	virtual void OnWorkerThreadStart() {};
	virtual void OnWorkerThreadEnd() {};

	// �׷� �ĺ�
	// ex) DB�κ��� �׷��� �ĺ��Ѵ�.
	virtual void OnClientJoin(SessionID sessionID) {};
	virtual void OnClientLeave(SessionID sessionID) {};
	//virtual UINT RecvData(JBuffer& recvBuff, JBuffer& dest) = 0;

	virtual void ServerConsoleLog() {
		std::cout << "Active Group Thread: " << m_ActiveGroupThread << std::endl;
		std::cout << "Dequeue Fail Cnt: " << m_Cnt << std::endl;
	}

private:
	// ���� �׷��� �з���
#if defined(ON_RECV_BUFFERING)
	virtual void OnRecv(SessionID sessionID, std::queue<JBuffer>& recvBuff, size_t recvDataLen);
#else
	virtual void OnRecv(SessionID sessionID, JBuffer& recvBuff);
#endif
};

class CLanGroupThread {
private:

#if defined(LOCKFREE_MESSAGE_QUEUE)

#if defined(LOCKFREE_GROUP_MESSAGE_QUEUE)
	LockFreeQueue<std::pair<SessionID, JBuffer*>>		m_LockFreeMessageQueue;
	BOOL												m_SessionGroupThreadStopFlag = false;
#elif defined(LOCKFREE_SESSION_MESSAGE_QUEUE)
	using SessionQueueMap = std::map<SessionID, LockFreeQueue<JBuffer*>>;
	std::map<SessionID, LockFreeQueue<JBuffer*>>		m_SessionMsgQueueMap;
	SRWLOCK												m_SessionMsgQueueSRWLock;
	BOOL												m_SessionGroupThreadStopFlag = false;

	BOOL												m_AnySessionMode = false;
	LockFreeQueue<std::pair<SessionID, JBuffer*>>		m_AnySessionMsgQueue;
#endif

#else	// !defined(LOCKFREE_MESSAGE_QUEUE)

#if defined(POLLING_SESSION_MESSAGE_QUEUE)
	using SessionQueueMap = std::map<SessionID, std::pair<std::queue<std::shared_ptr<JBuffer>>, CRITICAL_SECTION*>>;
	SessionQueueMap	m_SessionMsgQueueMap;
	SRWLOCK							m_SessionMsgQueueSRWLock;
	BOOL							m_SessionGroupThreadStopFlag = false;
#else	// !defined(POLLING_SESSION_MESSAGE_QUEUE)

#if defined(RECV_BUFF_QUEUE)
	std::queue<stSessionRecvBuff>	m_RecvQueue;
#elif defined(RECV_BUFF_LIST)
	std::list<stSessionRecvBuff>	m_RecvQueue;
	std::list<stSessionRecvBuff>	m_RecvQueueTemp;
	struct stRecvQueueSync {
		UINT accessCnt : 24;
		UINT blocked : 8;
	};
	stRecvQueueSync					m_RecvQueueSync;
	stRecvQueueSync					m_RecvQueueSyncTemp;
#endif	// defined(RECV_BUFF_QUEUE) || defined(RECV_BUFF_LIST)

#if defined(SETEVENT_RECEIVE_EVENT)
	HANDLE							m_RecvEvent;
	HANDLE							m_SessionGroupThreadStopEvent;
#elif defined(POLLING_RECEIVE_EVENT)
	BOOL							m_SessionGroupThreadStopFlag = false;
#endif	// defined(SETEVENT_RECEIVE_EVENT)

#endif	// defined(POLLING_SESSION_MESSAGE_QUEUE)

#endif	// defined(LOCKFREE_MESSAGE_QUEUE)

	UINT							m_temp_PushCnt = 0;
	UINT							m_temp_PopCnt = 0;
	std::mutex						m_RecvQueueMtx;

	HANDLE							m_SessionGroupThread;

	CLanGroupServer* m_ClanGroupServer;

protected:
	GroupID							m_GroupID;

#if defined(ALLOC_BY_TLS_MEM_POOL)
	TlsMemPoolManager<JBuffer>* m_SerialBuffPoolMgr;
	bool						m_SetTlsMemPoolFlag;
	size_t						m_TlsMemPoolUnitCnt;
	size_t						m_TlsMemPoolCapacity;
#endif

public:
#if defined(ALLOC_BY_TLS_MEM_POOL)
	CLanGroupThread(bool setTlsMemPool, size_t tlsMemPoolUnitCnt, size_t tlsMemPoolCapacity)
		: m_SetTlsMemPoolFlag(setTlsMemPool), m_TlsMemPoolUnitCnt(tlsMemPoolUnitCnt), m_TlsMemPoolCapacity(tlsMemPoolCapacity)
#else 
	CLanGroupThread()
#endif
	{
#if !defined(POLLING_SESSION_MESSAGE_QUEUE)
#if defined(RECV_BUFF_LIST)
		m_RecvQueueSync.accessCnt = 0;
		m_RecvQueueSync.blocked = 0;
		m_RecvQueueSyncTemp.accessCnt = 0;
		m_RecvQueueSyncTemp.blocked = 0;
#endif	// RECV_BUFF_LIST
#endif	// POLLING_SESSION_MESSAGE_QUEUE

#if defined(SETEVENT_RECEIVE_EVENT)
		m_RecvEvent = CreateEvent(NULL, false, false, NULL);
		m_SessionGroupThreadStopEvent = CreateEvent(NULL, false, false, NULL);
#endif
		m_SessionGroupThread = (HANDLE)_beginthreadex(NULL, 0, SessionGroupThreadFunc, this, 0, NULL);
	}
	~CLanGroupThread() {
#if defined(SETEVENT_RECEIVE_EVENT)
		SetEvent(m_SessionGroupThreadStopEvent);
#elif defined(POLLING_RECEIVE_EVENT)
		m_SessionGroupThreadStopFlag = true;
#endif
		WaitForSingleObject(m_SessionGroupThread, INFINITE);
	}
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_MESSAGE_QUEUE) && defined(LOCKFREE_SESSION_MESSAGE_QUEUE)
	void InitGroupThread(CLanGroupServer* clanGroupServer, GroupID groupID, TlsMemPoolManager<JBuffer>* serialBuffPoolMgr, bool anySessionMode) {
		m_ClanGroupServer = clanGroupServer;
		m_GroupID = groupID;
		m_SerialBuffPoolMgr = serialBuffPoolMgr;
		m_AnySessionMode = anySessionMode;
	}
#else
	void InitGroupThread(CLanGroupServer* clanGroupServer, GroupID groupID, TlsMemPoolManager<JBuffer>* serialBuffPoolMgr) {
		m_ClanGroupServer = clanGroupServer;
		m_GroupID = groupID;
		m_SerialBuffPoolMgr = serialBuffPoolMgr;
	}
#endif
#else
	void InitGroupThread(CLanGroupServer* clanGroupServer, GroupID groupID) {
		m_ClanGroupServer = clanGroupServer;
		m_GroupID = groupID;
	}
#endif

	void StopGroupThread() {
#if defined(SETEVENT_RECEIVE_EVENT)
		SetEvent(m_SessionGroupThreadStopEvent);
#else 
		m_SessionGroupThreadStopFlag = true;
#endif
	}

//#if defined(ON_RECV_BUFFERING)
//	void PushRecvBuff(stSessionRecvBuff& bufferedRecvData);
//#else 
	void PushRecvBuff(stSessionRecvBuff& recvBuff);
	void PushRecvBuff(SessionID sessionID, JBuffer* recvData);
//#endif

private:
	// �̺�Ʈ�� �ް�, �޽����� �о� OnRecv ȣ��
	static UINT __stdcall SessionGroupThreadFunc(void* arg);

protected:
#if defined(ALLOC_BY_TLS_MEM_POOL)
	inline JBuffer* AllocSerialBuff() {
		return m_ClanGroupServer->AllocSerialBuff();
	}
	inline JBuffer* AllocSerialBuff(const std::string& log) {
		return m_ClanGroupServer->AllocSerialBuff(log);
	}
	inline JBuffer* AllocSerialSendBuff(USHORT length) {
		return m_ClanGroupServer->AllocSerialSendBuff(length);
	}
	inline JBuffer* AllocSerialSendBuff(USHORT length, const std::string& log) {
		return m_ClanGroupServer->AllocSerialSendBuff(length, log);
	}
	inline void FreeSerialBuff(JBuffer* buff) {
		m_ClanGroupServer->FreeSerialBuff(buff);
	}
	inline void FreeSerialBuff(JBuffer* buff, const std::string& log) {
		m_ClanGroupServer->FreeSerialBuff(buff, log);
	}
	inline void AddRefSerialBuff(JBuffer* buff) {
		m_ClanGroupServer->AddRefSerialBuff(buff);
	}
	inline void AddRefSerialBuff(JBuffer* buff, const std::string& log) {
		m_ClanGroupServer->AddRefSerialBuff(buff, log);
	}
#endif

	void Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads) {
		m_ClanGroupServer->Encode(randKey, payloadLen, checkSum, payloads);
	}
	BYTE GetRandomKey() {
		return m_ClanGroupServer->GetRandomKey();
	}

#if defined(CALCULATE_TRANSACTION_PER_SECOND)
	inline void IncrementRecvTransaction(LONG cnt = 1) {
		m_ClanGroupServer->IncrementRecvTransaction(cnt);
	}
	inline void IncrementRecvTransactionNoGuard(LONG cnt = 1) {
		m_ClanGroupServer->IncrementRecvTransactionNoGuard(cnt);
	}
	inline void IncrementSendTransaction(LONG cnt = 1) {
		m_ClanGroupServer->IncrementSendTransaction(cnt);
	}
	inline void IncrementSendTransactionNoGuard(LONG cnt = 1) {
		m_ClanGroupServer->IncrementSendTransactionNoGuard(cnt);
	}
#endif

	inline void IncrementServerCnt() {
		m_ClanGroupServer->m_Cnt++;
	}
	inline void SetServerCnt(UINT16 cnt) {
		m_ClanGroupServer->m_Cnt = cnt;
	}

	void Disconnect(uint64 sessionID);
	bool SendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded = false);
	void ForwardSessionGroup(SessionID sessionID, GroupID to);
	//void PostMsgToThreadGroup(GroupID groupID, JBuffer& msg);

	//void Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads);
	//bool Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, BYTE* payloads);

protected:
	virtual void OnStart() {};
	virtual void OnMessage(SessionID sessionID, JBuffer& recvData) = 0;
	virtual void OnStop() {};
};