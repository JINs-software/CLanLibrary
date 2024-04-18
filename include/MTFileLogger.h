#pragma once
#include <Windows.h>
#include <mutex>

class MTFileLogger
{
	struct stLog {
		DWORD threadID = 0;

		UINT_PTR ptr0 = 0;
		UINT_PTR ptr1 = 0;
		UINT_PTR ptr2 = 0;
		UINT_PTR ptr3 = 0;

		UINT_PTR ptr4 = 0;
		UINT_PTR ptr5 = 0;
		UINT_PTR ptr6 = 0;
		UINT_PTR ptr7 = 0;
	};

	stLog* m_LogArr;// [USHRT_MAX + 1] ;
	USHORT m_LogIndex;
	USHORT m_IndexTurnCnt;
	std::mutex g_LogPrintMtx;

public:
	MTFileLogger() {
		m_LogArr = new stLog[USHRT_MAX + 1];
		memset(m_LogArr, 0, sizeof(stLog) * USHRT_MAX + 1);
		m_LogIndex = 0;
		m_IndexTurnCnt = 0;
	}

	inline USHORT AllocLogIndex() {
		USHORT logIdx = InterlockedIncrement16((SHORT*)&m_LogIndex);
		if (logIdx == 0) {
			m_IndexTurnCnt++;
		}
		m_LogArr[logIdx].threadID = GetThreadId(GetCurrentThread());
		return logIdx;
	}

	inline stLog& GetLogStruct(USHORT logIdx) {
		return m_LogArr[logIdx];
	}
	
	inline USHORT GetIndexTurnCnt() {
		return m_IndexTurnCnt;
	}
	inline USHORT GetNowIndex() {
		return m_LogIndex;
	}

	inline void LockMTLogger() {
		g_LogPrintMtx.lock();
	}
	inline void UnLockMTLogger() {
		g_LogPrintMtx.unlock();
	}
};

