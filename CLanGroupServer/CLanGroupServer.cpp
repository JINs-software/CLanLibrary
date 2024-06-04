#include "CLanGroupServer.h"

void CLanGroupServer::CreateGroup(GroupID newGroupID, CLanGroupThread* groupThread, bool anySessionMode)
{
	if (m_GroupThreads.find(newGroupID) != m_GroupThreads.end()) {
		DebugBreak();
	}
#if defined(ALLOC_BY_TLS_MEM_POOL)
#if defined(LOCKFREE_MESSAGE_QUEUE) && defined(LOCKFREE_SESSION_MESSAGE_QUEUE)
	groupThread->InitGroupThread(this, newGroupID, &m_SerialBuffPoolMgr, anySessionMode);
#else
	groupThread->InitGroupThread(this, newGroupID, &m_SerialBuffPoolMgr);
#endif
#else
	groupThread->InitGroupThread(this, newGroupID);
#endif
	m_GroupThreads.insert({ newGroupID, groupThread });
}

void CLanGroupServer::DeleteGroup(GroupID delGroupID)
{
	if (m_GroupThreads.find(delGroupID) == m_GroupThreads.end()) {
		DebugBreak();
	}
	m_GroupThreads[delGroupID]->StopGroupThread();
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

#if defined(ON_RECV_BUFFERING)
void CLanGroupServer::OnRecv(UINT64 sessionID, std::queue<JBuffer>& bufferedQueue, size_t recvDataLen)
{
	AcquireSRWLockShared(&m_SessionGroupIDSrwLock);
	if (m_SessionGroupID.find(sessionID) == m_SessionGroupID.end()) {
		DebugBreak();
	}
	UINT16 groupID = m_SessionGroupID[sessionID];
	ReleaseSRWLockShared(&m_SessionGroupIDSrwLock);

#if defined(LOCKFREE_MESSAGE_QUEUE)
	JBuffer* recvData = new JBuffer(recvDataLen);

	while (!bufferedQueue.empty()) {
		JBuffer recvBuff = bufferedQueue.front();
		bufferedQueue.pop();

		UINT dirDeqSize = recvBuff.GetDirectDequeueSize();
		if (dirDeqSize >= recvBuff.GetUseSize()) {
			recvData->Enqueue(recvBuff.GetDequeueBufferPtr(), recvBuff.GetUseSize());
		}
		else {
			recvData->Enqueue(recvBuff.GetDequeueBufferPtr(), dirDeqSize);
			recvData->Enqueue(recvBuff.GetBeginBufferPtr(), recvBuff.GetUseSize() - dirDeqSize);
		}
	}

	m_GroupThreads[groupID]->PushRecvBuff(sessionID, recvData);
#else
	stSessionRecvBuff sessionRecvData;	
	sessionRecvData.sessionID = sessionID;
	sessionRecvData.recvData = std::make_shared<JBuffer>(recvDataLen);

	while (!bufferedQueue.empty()) {
		JBuffer recvBuff = bufferedQueue.front();
		bufferedQueue.pop();

		UINT dirDeqSize = recvBuff.GetDirectDequeueSize();
		if (dirDeqSize >= recvBuff.GetUseSize()) {
			sessionRecvData.recvData->Enqueue(recvBuff.GetDequeueBufferPtr(), recvBuff.GetUseSize());
		}
		else {
			sessionRecvData.recvData->Enqueue(recvBuff.GetDequeueBufferPtr(), dirDeqSize);
			sessionRecvData.recvData->Enqueue(recvBuff.GetBeginBufferPtr(), recvBuff.GetUseSize() - dirDeqSize);
		}
	}

	m_GroupThreads[groupID]->PushRecvBuff(sessionRecvData);
#endif
}
#else
void CLanGroupServer::OnRecv(UINT64 sessionID, JBuffer& recvBuff)
{
	AcquireSRWLockShared(&m_SessionGroupIDSrwLock);
	if (m_SessionGroupID.find(sessionID) == m_SessionGroupID.end()) {
		DebugBreak();
	}
	UINT16 groupID = m_SessionGroupID[sessionID];
	ReleaseSRWLockShared(&m_SessionGroupIDSrwLock);

#if defined(LOCKFREE_MESSAGE_QUEUE)
	//JBuffer recvData(recvBuff.GetUseSize());
	JBuffer* recvData = new JBuffer(recvBuff.GetUseSize());
	UINT dirDeqSize = recvBuff.GetDirectDequeueSize();
	if (dirDeqSize >= recvBuff.GetUseSize()) {
		recvData->Enqueue(recvBuff.GetDequeueBufferPtr(), recvBuff.GetUseSize());
	}
	else {
		recvData->Enqueue(recvBuff.GetDequeueBufferPtr(), dirDeqSize);
		recvData->Enqueue(recvBuff.GetBeginBufferPtr(), recvBuff.GetUseSize() - dirDeqSize);
	}

	m_GroupThreads[groupID]->PushRecvBuff(sessionID, recvData);
#else
	std::shared_ptr<JBuffer> recvData = make_shared<JBuffer>(recvBuff.GetUseSize());
	UINT dirDeqSize = recvBuff.GetDirectDequeueSize();
	if (dirDeqSize >= recvBuff.GetUseSize()) {
		recvData->Enqueue(recvBuff.GetDequeueBufferPtr(), recvBuff.GetUseSize());
	}
	else {
		recvData->Enqueue(recvBuff.GetDequeueBufferPtr(), dirDeqSize);
		recvData->Enqueue(recvBuff.GetBeginBufferPtr(), recvBuff.GetUseSize() - dirDeqSize);
	}

	stSessionRecvBuff sessionRecvBuff{ sessionID, recvData };
	m_GroupThreads[groupID]->PushRecvBuff(sessionRecvBuff);
#endif
}
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////

//#if defined(ON_RECV_BUFFERING)
//void CLanGroupThread::PushRecvBuff(stSessionRecvBuff& bufferedRecvData) {
//#if defined(POLLING_SESSION_MESSAGE_QUEUE)
//	bool isPresent = true;
//	AcquireSRWLockShared(&m_SessionMsgQueueSRWLock);
//	SessionQueueMap::iterator iter = m_SessionMsgQueueMap.find(bufferedRecvData.sessionID);
//	if (iter == m_SessionMsgQueueMap.end()) {
//		isPresent = false;
//	}
//	ReleaseSRWLockShared(&m_SessionMsgQueueSRWLock);
//
//	if (!isPresent) {
//		AcquireSRWLockExclusive(&m_SessionMsgQueueSRWLock);
//		if (m_SessionMsgQueueMap.find(bufferedRecvData.sessionID) == m_SessionMsgQueueMap.end()) {
//			CRITICAL_SECTION* lockPtr = new CRITICAL_SECTION();
//			InitializeCriticalSection(lockPtr);
//			std::pair<SessionQueueMap::iterator, bool> ret = m_SessionMsgQueueMap.insert({ bufferedRecvData.sessionID, std::make_pair(std::queue<shared_ptr<JBuffer>>(), lockPtr) });
//			if (!ret.second) {
//				DebugBreak();
//			}
//			iter = ret.first;
//		}
//		ReleaseSRWLockExclusive(&m_SessionMsgQueueSRWLock);
//	}
//
//	std::queue<std::shared_ptr<JBuffer>>& recvQueue = iter->second.first;
//	CRITICAL_SECTION* recvQueueLock = iter->second.second;
//	EnterCriticalSection(recvQueueLock);
//	while (!bufferedRecvData.recvDataBuffered.empty()) {
//		recvQueue.push(bufferedRecvData.recvDataBuffered.front());
//		bufferedRecvData.recvDataBuffered.pop();
//	}
//	LeaveCriticalSection(recvQueueLock);
//#endif
//}
//#elif defined(LOCKFREE_MESSAGE_QUEUE)
#if defined(LOCKFREE_MESSAGE_QUEUE)
void CLanGroupThread::PushRecvBuff(SessionID sessionID, JBuffer* recvData)
{
#if defined(LOCKFREE_GROUP_MESSAGE_QUEUE)
	m_LockFreeMessageQueue.Enqueue({ sessionID, recvData });
#elif defined(LOCKFREE_SESSION_MESSAGE_QUEUE)
	if (m_AnySessionMode) {
		m_AnySessionMsgQueue.Enqueue({ sessionID, recvData });
	}
	else {
		bool isPresent = true;
		AcquireSRWLockShared(&m_SessionMsgQueueSRWLock);
		SessionQueueMap::iterator iter = m_SessionMsgQueueMap.find(sessionID);
		if (iter == m_SessionMsgQueueMap.end()) {
			isPresent = false;
		}
		ReleaseSRWLockShared(&m_SessionMsgQueueSRWLock);

		if (!isPresent) {
			AcquireSRWLockExclusive(&m_SessionMsgQueueSRWLock);
			if (m_SessionMsgQueueMap.find(sessionID) == m_SessionMsgQueueMap.end()) {
				//CRITICAL_SECTION* lockPtr = new CRITICAL_SECTION();
				//InitializeCriticalSection(lockPtr);
				//std::pair<SessionQueueMap::iterator, bool> ret = m_SessionMsgQueueMap.insert({ recvBuff.sessionID, std::make_pair(std::queue<shared_ptr<JBuffer>>(), lockPtr) });
				//if (!ret.second) {
				//	DebugBreak();
				//}
				//iter = ret.first;

				std::pair<SessionQueueMap::iterator, bool> ret = m_SessionMsgQueueMap.insert({ sessionID, LockFreeQueue<JBuffer*>() });
				if (!ret.second) {
					DebugBreak();
				}
				iter = ret.first;
			}
			ReleaseSRWLockExclusive(&m_SessionMsgQueueSRWLock);
		}

		LockFreeQueue<JBuffer*>& recvQueue = iter->second;
		recvQueue.Enqueue(recvData);
	}
#endif
}
#else
void CLanGroupThread::PushRecvBuff(stSessionRecvBuff& recvBuff) {
#if defined(POLLING_SESSION_MESSAGE_QUEUE)
	bool isPresent = true;
	AcquireSRWLockShared(&m_SessionMsgQueueSRWLock);
	SessionQueueMap::iterator iter = m_SessionMsgQueueMap.find(recvBuff.sessionID);
	if (iter == m_SessionMsgQueueMap.end()) {
		isPresent = false;
	}
	ReleaseSRWLockShared(&m_SessionMsgQueueSRWLock);

	if (!isPresent) {
		AcquireSRWLockExclusive(&m_SessionMsgQueueSRWLock);
		if (m_SessionMsgQueueMap.find(recvBuff.sessionID) == m_SessionMsgQueueMap.end()) {
			CRITICAL_SECTION* lockPtr = new CRITICAL_SECTION();
			InitializeCriticalSection(lockPtr);
			std::pair<SessionQueueMap::iterator, bool> ret = m_SessionMsgQueueMap.insert({ recvBuff.sessionID, std::make_pair(std::queue<shared_ptr<JBuffer>>(), lockPtr) });
			if (!ret.second) {
				DebugBreak();
			}
			iter = ret.first;
		}
		ReleaseSRWLockExclusive(&m_SessionMsgQueueSRWLock);
	}

	std::queue<std::shared_ptr<JBuffer>>& recvQueue = iter->second.first;
	CRITICAL_SECTION* recvQueueLock = iter->second.second;
	EnterCriticalSection(recvQueueLock);
	recvQueue.push(recvBuff.recvData);
	LeaveCriticalSection(recvQueueLock);
#elif defined(SETEVENT_RECEIVE_EVENT) || (POLLING_RECEIVE_EVENT)
#if defined(RECV_BUFF_QUEUE)
	m_RecvQueueMtx.lock();
	m_RecvQueue.push(recvBuff);
	m_temp_PushCnt++;
	m_RecvQueueMtx.unlock();

	SetEvent(m_RecvEvent);
#elif defined(RECV_BUFF_LIST)
	stRecvQueueSync cmp, exg;
	cmp = exg = m_RecvQueueSync;
	cmp.blocked = 0;
	exg.blocked = 0;
	exg.accessCnt += 1;
	do {
		UINT ret = InterlockedCompareExchange((UINT*)&m_RecvQueueSync, *(UINT*)&exg, *(UINT*)&cmp);
		stRecvQueueSync* retSync = (stRecvQueueSync*)&ret;
		if (ret == *(UINT*)&cmp) {
			m_RecvQueueMtx.lock();
			m_RecvQueue.push_back(recvBuff);
			m_temp_PushCnt++;
			m_RecvQueueMtx.unlock();
			InterlockedDecrement((UINT*)&m_RecvQueueSync);
			break;
		}
		else if (retSync->blocked == 0) {
			continue;
		}
		else {	// retSync->blocked == 1
			bool breakFlag = false;
			do {
				cmp = exg = m_RecvQueueSyncTemp;
				cmp.blocked = 0;
				exg.blocked = 0;
				exg.accessCnt += 1;
				ret = InterlockedCompareExchange((UINT*)&m_RecvQueueSyncTemp, *(UINT*)&exg, *(UINT*)&cmp);
				stRecvQueueSync* retSync = (stRecvQueueSync*)&ret;
				if (ret == *(UINT*)&cmp) {
					m_RecvQueueMtx.lock();
					m_RecvQueueTemp.push_back(recvBuff);
					m_temp_PushCnt++;
					m_RecvQueueMtx.unlock();
					InterlockedDecrement((UINT*)&m_RecvQueueSyncTemp);
					breakFlag = true;
					break;
				}
				else if (retSync->blocked == 0) {
					continue;
				}
				else {
					break;
				}
			} while (true);

			if (breakFlag) {
				break;
			}
		}
	} while (true);

#if defined(SETEVENT_RECEIVE_EVENT)
	SetEvent(m_RecvEvent);
#endif
#endif
#endif
}
#endif

UINT __stdcall CLanGroupThread::SessionGroupThreadFunc(void* arg)
{
	CLanGroupThread* groupthread = (CLanGroupThread*)arg;
	groupthread->m_ClanGroupServer->m_ActiveGroupThread++;

	groupthread->OnStart();

	if (groupthread->m_SetTlsMemPoolFlag) {
		groupthread->m_SerialBuffPoolMgr->AllocTlsMemPool(groupthread->m_TlsMemPoolUnitCnt, groupthread->m_TlsMemPoolCapacity);
	}

#if defined(LOCKFREE_MESSAGE_QUEUE)
#if defined(LOCKFREE_GROUP_MESSAGE_QUEUE)
	while (!groupthread->m_SessionGroupThreadStopFlag) {
		static UINT16 dequeueFailCnt = 0;

		std::pair<SessionID, JBuffer*> recvBuff;
		if (groupthread->m_LockFreeMessageQueue.Dequeue(recvBuff)) {
			JBuffer* recvData = recvBuff.second;
			groupthread->OnMessage(recvBuff.first, *recvData);
			delete recvData;

			if (dequeueFailCnt > 0) {
				dequeueFailCnt--;
			}
		}
		else {
			dequeueFailCnt++;
		}
		groupthread->SetServerCnt(dequeueFailCnt);
	}
#elif defined(LOCKFREE_SESSION_MESSAGE_QUEUE)
	while (!groupthread->m_SessionGroupThreadStopFlag) {
		if (groupthread->m_AnySessionMode) {
			std::pair<SessionID, JBuffer*> recvBuff;
			if (groupthread->m_AnySessionMsgQueue.Dequeue(recvBuff)) {
				JBuffer* recvData = recvBuff.second;
				groupthread->OnMessage(recvBuff.first, *recvData);
				delete recvData;
			}
		}
		else {
			AcquireSRWLockShared(&groupthread->m_SessionMsgQueueSRWLock);
			for (auto& iter : groupthread->m_SessionMsgQueueMap) {
				SessionID sessionID = iter.first;
				LockFreeQueue<JBuffer*>& msgQueue = iter.second;
				LONG msgQueueSize = msgQueue.GetSize();
				while (msgQueueSize > 0) {
					JBuffer* recvData;
					if (msgQueue.Dequeue(recvData)) {
						groupthread->OnMessage(sessionID, *recvData);
						delete recvData;
					}
					msgQueueSize--;
				}
			}
			ReleaseSRWLockShared(&groupthread->m_SessionMsgQueueSRWLock);
		}
	}
#endif
#else
#if defined(SETEVENT_RECEIVE_EVENT)
	HANDLE events[2] = { groupthread->m_SessionGroupThreadStopEvent, groupthread->m_RecvEvent };
	while (true) {
		DWORD ret = WaitForMultipleObjects(2, events, false, INFINITE);
		if (ret == WAIT_OBJECT_0) {
			break;
		}
		else if (ret == WAIT_OBJECT_0 + 1) {
#if defined(RECV_BUFF_QUEUE)
			while (true) {
				bool isEmpty = true;
				stSessionRecvBuff recvBuff;
				groupthread->m_RecvQueueMtx.lock();
				if (!groupthread->m_RecvQueue.empty()) {
					recvBuff = groupthread->m_RecvQueue.front();
					groupthread->m_RecvQueue.pop();
					groupthread->m_temp_PopCnt++;
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
#elif defined(RECV_BUFF_LIST)
			stRecvQueueSync blocked;
			blocked.blocked = 1;
			blocked.accessCnt = 0;
			InterlockedOr((LONG*)&groupthread->m_RecvQueueSync, *(LONG*)&blocked);
			while (groupthread->m_RecvQueueSync.accessCnt != 0);

			auto iter = groupthread->m_RecvQueue.begin();
			for (auto iter = groupthread->m_RecvQueue.begin(); iter != groupthread->m_RecvQueue.end(); iter++) {
				stSessionRecvBuff& recvBuff = *iter;
				groupthread->OnMessage(recvBuff.sessionID, *recvBuff.recvData);
				groupthread->m_temp_PopCnt++;
			}
			groupthread->m_RecvQueue.clear();

			InterlockedOr((LONG*)&groupthread->m_RecvQueueSyncTemp, *(LONG*)&blocked);
			while (groupthread->m_RecvQueueSyncTemp.accessCnt != 0);

			groupthread->m_RecvQueue.swap(groupthread->m_RecvQueueTemp);
			InterlockedXor((LONG*)&groupthread->m_RecvQueueSync, *(LONG*)&blocked);
			InterlockedXor((LONG*)&groupthread->m_RecvQueueSyncTemp, *(LONG*)&blocked);
#endif
		}
		else {
			DebugBreak();
		}
	}
#elif defined(POLLING_RECEIVE_EVENT)
	while (!groupthread->m_SessionGroupThreadStopFlag) {
#if defined(RECV_BUFF_LIST)
		if (groupthread->m_RecvQueue.size() == 0) {
			continue;
		}

		stRecvQueueSync blocked;
		blocked.blocked = 1;
		blocked.accessCnt = 0;
		InterlockedOr((LONG*)&groupthread->m_RecvQueueSync, *(LONG*)&blocked);
		while (groupthread->m_RecvQueueSync.accessCnt != 0);

		auto iter = groupthread->m_RecvQueue.begin();
		for (auto iter = groupthread->m_RecvQueue.begin(); iter != groupthread->m_RecvQueue.end(); iter++) {
			stSessionRecvBuff& recvBuff = *iter;
			groupthread->OnMessage(recvBuff.sessionID, *recvBuff.recvData);
			groupthread->m_temp_PopCnt++;
		}
		groupthread->m_RecvQueue.clear();

		InterlockedOr((LONG*)&groupthread->m_RecvQueueSyncTemp, *(LONG*)&blocked);
		while (groupthread->m_RecvQueueSyncTemp.accessCnt != 0);

		groupthread->m_RecvQueue.swap(groupthread->m_RecvQueueTemp);
		InterlockedXor((LONG*)&groupthread->m_RecvQueueSync, *(LONG*)&blocked);
		InterlockedXor((LONG*)&groupthread->m_RecvQueueSyncTemp, *(LONG*)&blocked);
#endif
	}
#elif defined(POLLING_SESSION_MESSAGE_QUEUE)
	while (!groupthread->m_SessionGroupThreadStopFlag) {
		AcquireSRWLockShared(&groupthread->m_SessionMsgQueueSRWLock);
		for (auto& iter : groupthread->m_SessionMsgQueueMap) {
			SessionID sessionID = iter.first;
			std::queue<std::shared_ptr<JBuffer>>& recvQueue = iter.second.first;
			CRITICAL_SECTION* recvQueueLock = iter.second.second;
			if (recvQueue.empty()) {
				continue;
			}

			EnterCriticalSection(recvQueueLock);
			while (!recvQueue.empty()) {
				groupthread->OnMessage(sessionID, *recvQueue.front());
				recvQueue.pop();
			}
			LeaveCriticalSection(recvQueueLock);
			//if (TryEnterCriticalSection(recvQueueLock)) {
			//	while (!recvQueue.empty()) {
			//		groupthread->OnMessage(sessionID, *recvQueue.front());
			//		recvQueue.pop();
			//	}
			//	LeaveCriticalSection(recvQueueLock);
			//}
		}
		ReleaseSRWLockShared(&groupthread->m_SessionMsgQueueSRWLock);
	}
#endif
#endif

	groupthread->m_ClanGroupServer->m_ActiveGroupThread--;
	return 0;
}

//JBuffer* CLanGroupThread::GetSerialSendBuff()
//{
//	JBuffer* serialSendBuff = m_SerialBuffPoolMgr->GetTlsMemPool().AllocMem();
//	serialSendBuff->ClearBuffer();
//	return serialSendBuff;
//}

void CLanGroupThread::Disconnect(uint64 sessionID) {
	m_ClanGroupServer->Disconnect(sessionID);
}
bool CLanGroupThread::SendPacket(uint64 sessionID, JBuffer* sendDataPtr, bool encoded) {
#if defined(ALLOC_BY_TLS_MEM_POOL)
	return m_ClanGroupServer->SendPacket(sessionID, sendDataPtr, encoded);
#else
	shared_ptr<JBuffer> sptr = make_shared<JBuffer>(sendDataPtr);
	return m_ClanGroupServer->SendPacket(sessionID, sptr);
#endif
}
void CLanGroupThread::ForwardSessionGroup(SessionID sessionID, GroupID to) {
	m_ClanGroupServer->ForwardSessionGroup(sessionID, m_GroupID, to);
}
//void CLanGroupThread::PostMsgToThreadGroup(GroupID groupID, JBuffer& msg) {
//	m_ClanGroupServer->PostMsgToThreadGroup(groupID, msg);
//}

