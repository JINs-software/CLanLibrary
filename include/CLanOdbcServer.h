#pragma once
#include "CLanServer.h"
#include "DBConnectionPool.h"

class CLanOdbcServer : public CLanServer
{
private:
	INT32						m_DBConnCnt;
	DBConnectionPool*			m_DBConnPool;
	const WCHAR*				m_OdbcConnStr;

public:
	CLanOdbcServer(int32 dbConnectionCnt, const WCHAR* odbcConnStr,
		const char* serverIP, uint16 serverPort,
		DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections, 
		size_t tlsMemPoolDefaultUnitCnt, size_t tlsMemPoolDefaultUnitCapacity,
		bool tlsMemPoolReferenceFlag, bool tlsMemPoolPlacementNewFlag,
		UINT serialBufferSize, 
#if defined(LOCKFREE_SEND_QUEUE)
		uint32 sessionRecvBuffSize
#else
		uint32 sessionSendBuffSize, uint32 sessionRecvBuffSize
#endif
	)
		: m_DBConnCnt(dbConnectionCnt), m_OdbcConnStr(odbcConnStr), 
		CLanServer(serverIP, serverPort, numOfIocpConcurrentThrd, numOfWorkerThreads, maxOfConnections, 
			tlsMemPoolDefaultUnitCnt, tlsMemPoolDefaultUnitCapacity,
			tlsMemPoolReferenceFlag, tlsMemPoolPlacementNewFlag,
			serialBufferSize,
#if defined(LOCKFREE_SEND_QUEUE)
			sessionRecvBuffSize
#else
			sessionSendBuffSize, sessionRecvBuffSize
#endif
			)
	{
		m_DBConnPool = new DBConnectionPool();

		if (!m_DBConnPool->Connect(m_DBConnCnt, m_OdbcConnStr)) {
			std::cout << "CLanOdbcServer::m_DBConnPool->Connect(..) Fail!" << std::endl;
			DebugBreak();
		}
		else {
			std::cout << "CLanOdbcServer::m_DBConnPool->Connect(..) Success!" << std::endl;
		}
	}

	bool Start() {
		if (!CLanServer::Start()) {
			return false;	
		}

		return true;
	}
	void Stop() {
		m_DBConnPool->Clear();
		CLanServer::Stop();
	}

protected:
	DBConnection* HoldDBConnection() {
		return m_DBConnPool->Pop();
	}
	void FreeDBConnection(DBConnection* dbConn) {
		m_DBConnPool->Push(dbConn);
	}

	bool ExecQuery(const wchar_t* query) {
		DBConnection* dbConn = m_DBConnPool->Pop();
		if (dbConn == nullptr) {
			return false;
		}
		else {
			if (!dbConn->Execute(query)) {
				DebugBreak();
				return false;
			}
		}

		return true;
	}
};

