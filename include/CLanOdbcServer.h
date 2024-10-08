#pragma once
#include "CLanServer.h"
#include "DBConnectionPool.h"

class CLanOdbcServer : public CLanServer
{
private:
	INT32						m_DBConnCnt;
	DBConnectionPool*			m_DBConnPool;		// ���� ���̺귯������ DBConnectionPool�� ����
	bool						m_DBConnFlag;
	const WCHAR*				m_OdbcConnStr;

	bool						m_DBConnErrorLogFlag;

public:
	CLanOdbcServer(int32 dbConnectionCnt, const WCHAR* odbcConnStr,
		const char* serverIP, uint16 serverPort,
		DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections,
		size_t tlsMemPoolDefaultUnitCnt, size_t tlsMemPoolDefaultUnitCapacity,
		bool tlsMemPoolReferenceFlag, bool tlsMemPoolPlacementNewFlag,
		UINT serialBufferSize,
#if defined(LOCKFREE_SEND_QUEUE)
		uint32 sessionRecvBuffSize,
#else
		uint32 sessionSendBuffSize, uint32 sessionRecvBuffSize,
#endif
		BYTE protocolCode = dfPACKET_CODE, BYTE packetKey = dfPACKET_KEY,
		bool recvBufferingMode = false,
		bool dbConnErrorLogFlag = false
	);

	bool Start();
	void Stop();

protected:
	// ������ DB Ŀ�ؼ� �� �ϳ��� Ŀ�ؼ��� ��Ÿ������ ȹ��,	NULL ��ȯ �� ȹ�� ���� DBConnection ���� (pool size: 0)
	inline DBConnection* HoldDBConnection() { return m_DBConnPool->Pop(); }
	// DB Ŀ�ؼ� �ݳ�
	inline void FreeDBConnection(DBConnection* dbConn, bool isDisconnected = false, bool tryToConnect = false) { 
		m_DBConnPool->Push(dbConn,isDisconnected, tryToConnect, m_OdbcConnStr); 
	}

	bool BindParameter(DBConnection* dbConn, INT32 paramIndex, bool* value);
	bool BindParameter(DBConnection* dbConn, INT32 paramIndex, float* value);
	bool BindParameter(DBConnection* dbConn, INT32 paramIndex, double* value);
	bool BindParameter(DBConnection* dbConn, INT32 paramIndex, INT8* value);
	bool BindParameter(DBConnection* dbConn, INT32 paramIndex, INT16* value);
	bool BindParameter(DBConnection* dbConn, INT32 paramIndex, INT32* value);
	bool BindParameter(DBConnection* dbConn, INT32 paramIndex, INT64* value);
	bool BindParameter(DBConnection* dbConn, INT32 paramIndex, TIMESTAMP_STRUCT* value);
	bool BindParameter(DBConnection* dbConn, INT32 paramIndex, const WCHAR* str);
	bool BindParameter(DBConnection* dbConn, INT32 paramIndex, const BYTE* bin, INT32 size);	 
	bool BindColumn(DBConnection* dbConn, INT32 columnIndex, bool* value);
	bool BindColumn(DBConnection* dbConn, INT32 columnIndex, float* value);
	bool BindColumn(DBConnection* dbConn, INT32 columnIndex, double* value);
	bool BindColumn(DBConnection* dbConn, INT32 columnIndex, INT8* value);
	bool BindColumn(DBConnection* dbConn, INT32 columnIndex, INT16* value);
	bool BindColumn(DBConnection* dbConn, INT32 columnIndex, INT32* value);
	bool BindColumn(DBConnection* dbConn, INT32 columnIndex, INT64* value);
	bool BindColumn(DBConnection* dbConn, INT32 columnIndex, TIMESTAMP_STRUCT* value);
	bool BindColumn(DBConnection* dbConn, INT32 columnIndex, WCHAR* str, INT32 size, SQLLEN* index);
	bool BindColumn(DBConnection* dbConn, INT32 columnIndex, BYTE* bin, INT32 size, SQLLEN* index);

	bool BindParameter(DBConnection* dbConn, SQLPOINTER dataPtr, SQLUSMALLINT paramIndex, SQLULEN len, SQLSMALLINT cType, SQLSMALLINT sqlType);
	bool BindColumn(DBConnection* dbConn, SQLPOINTER outValue, SQLUSMALLINT columnIndex, SQLULEN len, SQLSMALLINT cType);

	void UnBind(DBConnection* dbConn);

	bool ExecQuery(DBConnection* dbConn, const wchar_t* query);
	bool FetchQuery(DBConnection* dbConn);
	INT32 GetRowCount(DBConnection* dbConn);

	/**************** ���� ***************/
	/*
		dbConn = HoldDBConnection();				// DB Ŀ�ؼ� ȹ��
		ExecQuery(dbConn, L"CREATE TABLE...");		// ���̺� ���� ���� ����
		FreeDBConnection(dbConn);					// DB Ŀ�ؼ� �ݳ�
	*/

	/* (BindParameter)
		dbConn = HoldDBConnection();				// DB Ŀ�ؼ� ȹ��
		UnBind(dbConn);								// ���� ���ε� ���� ����

		// ���� ���� ���ε�
		int32 gold = 100;
		BindParameter(dbConn, &gold, 1, sizeof(gold), SQL_C_LONG, SQL_INTEGER);
	
		ExecQuery(dbConn, L"INSERT INTO [dbo].[Gold]([gold]) VALUES(?)");
		FreeDBConnection(dbConn);					
	*/

	/* (BindColumn)
		dbConn = HoldDBConnection();				// DB Ŀ�ؼ� ȹ��
		UnBind(dbConn);								// ���� ���ε� ���� ����

		int32 gold = 100;
		BindParameter(dbConn, &gold, 1, sizeof(gold), SQL_C_LONG, SQL_INTEGER);
		int32 outId = 0;
		BindColumn(dbConn, &outId, 1, sizeof(outId), SQL_C_LONG);
		int32 outGold = 0;
		BindColumn(dbConn, &outGold, 2, sizeof(outId), SQL_C_LONG);

		ExecQuery(dbConn, L"SELECT id, gold FROM [dbo].[Gold] WHERE gold = (?)");

		// false ��ȯ ������ ��ġ�ϸ� ���� �࿡ ���� �����͸� ȹ��...
		while(!FetchQuery(dbConn) {
			cout << "Id: " << outId << " Gold : " << outGold << endl;
		}
		FreeDBConnection(dbConn);
	*/
};

