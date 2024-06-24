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
	CLanGroupServer(const char* serverIP, uint16 serverPort,
		DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections,
		size_t tlsMemPoolDefaultUnitCnt, size_t tlsMemPoolDefaultUnitCapacity,
		bool tlsMemPoolReferenceFlag, bool tlsMemPoolPlacementNewFlag,
		UINT serialBufferSize = DEFAULT_SERIAL_BUFFER_SIZE,
#if defined(LOCKFREE_SEND_QUEUE)
		uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
#else
		uint32 sessionSendBuffSize = SESSION_SEND_BUFFER_DEFAULT_SIZE, uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
#endif
		BYTE protocolCode = dfPACKET_CODE, BYTE packetKey = dfPACKET_KEY,
		bool recvBufferingMode = false
	)
		: CLanServer(
			serverIP, serverPort, numOfIocpConcurrentThrd, numOfWorkerThreads, maxOfConnections,
			tlsMemPoolDefaultUnitCnt, tlsMemPoolDefaultUnitCapacity,
			tlsMemPoolReferenceFlag, tlsMemPoolPlacementNewFlag,
			serialBufferSize,
#if defined(LOCKFREE_SEND_QUEUE)
			sessionRecvBuffSize,
#else
			sessionSendBuffSize, sessionRecvBuffSize,
#endif
			protocolCode, packetKey,
			recvBufferingMode
		)
	{}


public:
	UINT16	m_ActiveGroupThread = 0;

private:
	// 세션 - 세션 그룹 식별 자료구조
	std::unordered_map<SessionID, GroupID>	m_SessionGroupMap;
	SRWLOCK									m_SessionGroupMapSrwLock;
	// 그룹 별 이벤트
	std::map<GroupID, CLanGroupThread*>		m_GroupThreads;

public:
	// 그룹 생성 (그룹 식별자 반환, 라이브러리와 컨텐츠는 그룹 식별자를 통해 식별)
	void CreateGroup(GroupID newGroupID, CLanGroupThread* groupThread, bool anySessionMode, bool threadPriorBoost = false);
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

	virtual void ServerConsoleLog();

private:
	virtual void OnRecv(UINT64 sessionID, JSerialBuffer& recvSerialBuff);
	virtual void OnRecv(SessionID sessionID, JBuffer& recvBuff);
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

	CLanGroupServer*				m_ClanGroupServer;

	bool							m_ThreadPriorBoost;

protected:
	GroupID							m_GroupID;

#if defined(ALLOC_BY_TLS_MEM_POOL)
	bool						m_SetTlsMemPoolFlag;
	size_t						m_TlsMemPoolUnitCnt;
	size_t						m_TlsMemPoolCapacity;
#endif

public:
#if defined(ALLOC_BY_TLS_MEM_POOL)
	CLanGroupThread(bool setTlsMemPool, size_t tlsMemPoolUnitCnt, size_t tlsMemPoolCapacity)
		: m_SetTlsMemPoolFlag(setTlsMemPool), m_TlsMemPoolUnitCnt(tlsMemPoolUnitCnt), m_TlsMemPoolCapacity(tlsMemPoolCapacity),
		m_ThreadPriorBoost(false)
#else 
	CLanGroupThread()
#endif
	{
#if !defined(LOCKFREE_MESSAGE_QUEUE)
#if !defined(POLLING_SESSION_MESSAGE_QUEUE)
#if defined(RECV_BUFF_LIST)
		m_RecvQueueSync.accessCnt = 0;
		m_RecvQueueSync.blocked = 0;
		m_RecvQueueSyncTemp.accessCnt = 0;
		m_RecvQueueSyncTemp.blocked = 0;
#endif	// RECV_BUFF_LIST
#endif	// POLLING_SESSION_MESSAGE_QUEUE
#endif

#if defined(SETEVENT_RECEIVE_EVENT)
		m_RecvEvent = CreateEvent(NULL, false, false, NULL);
		m_SessionGroupThreadStopEvent = CreateEvent(NULL, false, false, NULL);
#endif
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
	void InitGroupThread(CLanGroupServer* clanGroupServer, GroupID groupID, bool threadPriorBoost = false) {
		m_ClanGroupServer = clanGroupServer;
		m_GroupID = groupID;
		m_ThreadPriorBoost = threadPriorBoost;
	}
#endif
#else
	void InitGroupThread(CLanGroupServer* clanGroupServer, GroupID groupID) {
		m_ClanGroupServer = clanGroupServer;
		m_GroupID = groupID;
	}
#endif

	void StartGroupThread() {
		m_SessionGroupThread = (HANDLE)_beginthreadex(NULL, 0, SessionGroupThreadFunc, this, 0, NULL);
	}
	void StopGroupThread() {
#if defined(SETEVENT_RECEIVE_EVENT)
		SetEvent(m_SessionGroupThreadStopEvent);
#else 
		m_SessionGroupThreadStopFlag = true;
#endif
	}

	void PushRecvBuff(stSessionRecvBuff& recvBuff);
	void PushRecvBuff(SessionID sessionID, JBuffer* recvData);

	virtual void ConsoleLog() {}
	inline LONG GetGroupThreadMessageQueueSize() {
		return m_LockFreeMessageQueue.GetSize();
	}

private:
	// 이벤트를 받고, 메시지를 읽어 OnRecv 호출
	static UINT __stdcall SessionGroupThreadFunc(void* arg);

protected:	
#if defined(ALLOC_BY_TLS_MEM_POOL)
	inline JBuffer* AllocSerialBuff() {
		return m_ClanGroupServer->AllocSerialBuff();
	}
	inline JBuffer* AllocSerialSendBuff(USHORT length) {
		return m_ClanGroupServer->AllocSerialSendBuff(length);
	}
	inline void FreeSerialBuff(JBuffer* buff) {
		m_ClanGroupServer->FreeSerialBuff(buff);
	}
	inline void AddRefSerialBuff(JBuffer* buff) {
		m_ClanGroupServer->AddRefSerialBuff(buff);
	}
#endif

	void Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads) {
		m_ClanGroupServer->Encode(randKey, payloadLen, checkSum, payloads);
	}
	BYTE GetRandomKey() {
		return m_ClanGroupServer->GetRandomKey();
	}

#if defined(CALCULATE_TRANSACTION_PER_SECOND)
	inline void IncrementRecvTransaction(bool threadSafe, UINT recvSize) {
		m_ClanGroupServer->IncrementRecvTransactions(threadSafe, recvSize);
	}
	inline void IncrementSendTransaction(bool threadSafe, UINT sendSize) {
		m_ClanGroupServer->IncrementSendTransactions(threadSafe, sendSize);
	}
#endif

	void Disconnect(uint64 sessionID);
	bool SendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded = false, bool postToWorker = false);
	bool BufferSendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded = false);
	void SendBufferedPacket(uint64 sessionID, bool postToWorker = false);
	void ForwardSessionGroup(SessionID sessionID, GroupID to);
	//void PostMsgToThreadGroup(GroupID groupID, JBuffer& msg);

protected:
	virtual void OnStart() {};
	virtual void OnMessage(SessionID sessionID, JBuffer& recvData) = 0;
	virtual void OnStop() {};
};