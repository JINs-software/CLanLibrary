#pragma once
#include "CLanServer.h"
#include "CLanGroupConfig.h"
#if defined(POLLING_LOCKFREE_QUEUE)
#include "LockFreeQueue.h"
#endif

using SessionID = UINT64;
using GroupID = UINT16;

#if defined(ON_RECV_BUFFERING)
struct stSessionRecvBuff {
	SessionID	sessionID;
	std::queue<std::shared_ptr<JBuffer>> recvDataBuffered;
};
#else
struct stSessionRecvBuff {
	SessionID	sessionID;
	std::shared_ptr<JBuffer> recvData;
};
#endif

//#define RECV_BUFF_QUEUE
#define RECV_BUFF_LIST

class CLanGroupThread;

class CLanGroupServer : public CLanServer
{
public:
#if defined(ALLOC_BY_TLS_MEM_POOL)
	//CLanServer(const char* serverIP, uint16 serverPort,
	//	DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections,
	//	bool tlsMemPoolReferenceFlag = false, bool tlsMemPoolPlacementNewFlag = false,
	//	size_t tlsMemPoolDefaultUnitCnt = TLS_MEM_POOL_DEFAULT_UNIT_CNT, size_t tlsMemPoolDefaultCapacity = TLS_MEM_POOL_DEFAULT_CAPACITY,
	//	uint32 sessionSendBuffSize = SESSION_SEND_BUFFER_DEFAULT_SIZE, uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
	//	bool beNagle = true
	//);
	CLanGroupServer(const char* serverIP, uint16 serverPort, DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections)
		: CLanServer(serverIP, serverPort, numOfIocpConcurrentThrd, numOfWorkerThreads, maxOfConnections) {}
#else
	//CLanServer(const char* serverIP, UINT16 serverPort,
	//	DWORD numOfIocpConcurrentThrd, UINT16 numOfWorkerThreads, UINT16 maxOfConnections,
	//	uint32 sessionSendBuffSize = SESSION_SEND_BUFFER_DEFAULT_SIZE, uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
	//	bool beNagle = true
	//);
	CLanGroupServer(const char* serverIP, uint16 serverPort, DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections,
					uint32 sessionSendBuffSize, uint32 sessionRecvBuffSize)
		: CLanServer(serverIP, serverPort, numOfIocpConcurrentThrd, numOfWorkerThreads, maxOfConnections, sessionSendBuffSize, sessionRecvBuffSize) {}
#endif

public:
	UINT16	m_ActiveGroupThread = 0;
	UINT16	m_Cnt = 0;

private:
	// 세션 - 세션 그룹 식별 자료구조
	std::unordered_map<SessionID, GroupID>	m_SessionGroupID;
	SRWLOCK									m_SessionGroupIDSrwLock;
	// 그룹 별 이벤트
	std::map<GroupID, CLanGroupThread*>		m_GroupThreads;

public:
	//void Stop() {
	//	for (auto iter : m_GroupThreads) {
	//		iter.second->StopGroupThread();
	//	}
	//	CLanServer::Stop();
	//}

public:
	// 그룹 생성 (그룹 식별자 반환, 라이브러리와 컨텐츠는 그룹 식별자를 통해 식별)
	void CreateGroup(GroupID newGroupID, CLanGroupThread* groupThread);
	void DeleteGroup(GroupID delGroupID);

	// 그룹 이동
	void EnterSessionGroup(SessionID sessionID, GroupID enterGroup);
	void LeaveSessionGroup(SessionID sessionID);
	void ForwardSessionGroup(SessionID sessionID, GroupID from, GroupID to);
	//void PostMsgToThreadGroup(GroupID groupID, JBuffer& msg);

protected:
	virtual bool OnWorkerThreadCreate(HANDLE thHnd) { return true; };
	virtual void OnWorkerThreadCreateDone() {};
	virtual void OnWorkerThreadStart() {};
	virtual void OnWorkerThreadEnd() {};

	// 그룹 식별
	// ex) DB로부터 그룹을 식별한다.
	virtual void OnClientJoin(SessionID sessionID) {};
	virtual void OnClientLeave(SessionID sessionID) {};
	//virtual UINT RecvData(JBuffer& recvBuff, JBuffer& dest) = 0;

	virtual void ServerConsoleLog() {
		std::cout << "Active Group Thread: " << m_ActiveGroupThread << std::endl;
		std::cout << "Dequeue Fail Cnt: " << m_Cnt << std::endl;
	}

private:
	// 세션 그룹을 분류함
#if defined(ON_RECV_BUFFERING)
	virtual void OnRecv(SessionID sessionID, std::queue<JBuffer>& recvBuff);
#else
	virtual void OnRecv(SessionID sessionID, JBuffer& recvBuff);
#endif
};

class CLanGroupThread {
private:
#if !defined(POLLING_SESSION_MESSAGE_QUEUE)
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
#endif	// RECV_BUFF_QUEUE, RECV_BUFF_LIST
#endif	// POLLING_SESSION_MESSAGE_QUEUE

#if defined(SETEVENT_RECEIVE_EVENT)
	HANDLE							m_RecvEvent;
	HANDLE							m_SessionGroupThreadStopEvent;
#elif defined(POLLING_RECEIVE_EVENT)
	BOOL							m_SessionGroupThreadStopFlag = false;
#elif defined(POLLING_SESSION_MESSAGE_QUEUE)
	using SessionQueueMap = std::map<SessionID, std::pair<std::queue<std::shared_ptr<JBuffer>>, CRITICAL_SECTION*>>;
	SessionQueueMap	m_SessionMsgQueueMap;
	SRWLOCK							m_SessionMsgQueueSRWLock;
	BOOL							m_SessionGroupThreadStopFlag = false;
#elif defined(POLLING_LOCKFREE_QUEUE)
	LockFreeQueue<std::pair<SessionID, JBuffer*>>		m_LockFreeMessageQueue;
	BOOL									m_SessionGroupThreadStopFlag = false;
#endif

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
	void InitGroupThread(CLanGroupServer* clanGroupServer, GroupID groupID, TlsMemPoolManager<JBuffer>* serialBuffPoolMgr) {
		m_ClanGroupServer = clanGroupServer;
		m_GroupID = groupID;
		m_SerialBuffPoolMgr = serialBuffPoolMgr;
	}
#else
	void InitGroupThread(CLanGroupServer* clanGroupServer, GroupID groupID) {
		m_ClanGroupServer = clanGroupServer;
		m_GroupID = groupID;
	}
#endif

	void StopGroupThread() {
		m_SessionGroupThreadStopFlag = true;
	}

#if defined(ON_RECV_BUFFERING)
	void PushRecvBuff(stSessionRecvBuff& bufferedRecvData);
#else 
	void PushRecvBuff(stSessionRecvBuff& recvBuff);
	void PushRecvBuff(SessionID sessionID, JBuffer* recvData);
#endif

private:
	// 이벤트를 받고, 메시지를 읽어 OnRecv 호출
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
#if defined(CALCULATE_TRANSACTION_PER_SECOND)
	inline void IncrementRecvTransaction(LONG cnt = 1) {
		m_ClanGroupServer->IncrementRecvTransaction(cnt);
	}
	inline void IncrementRecvTransactionNoGuard(LONG cnt = 1) {
		m_ClanGroupServer->IncrementRecvTransactionNoGuard(cnt);
	}
#endif

	inline void IncrementServerCnt() {
		m_ClanGroupServer->m_Cnt++;
	}
	inline void SetServerCnt(UINT16 cnt) {
		m_ClanGroupServer->m_Cnt = cnt;
	}

	void Disconnect(uint64 sessionID);
	bool SendPacket(uint64 sessionID, JBuffer* sendDataPtr);
	void ForwardSessionGroup(SessionID sessionID, GroupID to);
	//void PostMsgToThreadGroup(GroupID groupID, JBuffer& msg);

	//void Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads);
	//bool Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, BYTE* payloads);

protected:
	virtual void OnStart() {};
	virtual void OnMessage(SessionID sessionID, JBuffer& recvData) = 0;
	virtual void OnStop() {};
};