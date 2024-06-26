#include "CLanOdbcServer.h"

CLanOdbcServer::CLanOdbcServer(int32 dbConnectionCnt, const WCHAR* odbcConnStr,
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
	BYTE protocolCode, BYTE packetKey,
	bool recvBufferingMode,
	bool dbConnErrorLogFlag
) 
	: m_DBConnCnt(dbConnectionCnt), m_DBConnFlag(false), m_OdbcConnStr(odbcConnStr), m_DBConnErrorLogFlag(dbConnErrorLogFlag),
		CLanServer(serverIP, serverPort, numOfIocpConcurrentThrd, numOfWorkerThreads, maxOfConnections,
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

bool CLanOdbcServer::Start() {
	m_DBConnPool = new DBConnectionPool(m_DBConnErrorLogFlag);

	if (!m_DBConnPool->Connect(m_DBConnCnt, m_OdbcConnStr)) {
		std::cout << "CLanOdbcServer::m_DBConnPool->Connect(..) Fail!" << std::endl;
		return false;
	}

	std::cout << "CLanOdbcServer::m_DBConnPool->Connect(..) Success!" << std::endl;
	m_DBConnFlag = true;

	if (!CLanServer::Start()) {
		return false;
	}

	return true;
}
void CLanOdbcServer::Stop() {
	if (m_DBConnPool != NULL) {
		m_DBConnPool->Clear();
	}

	if (m_DBConnFlag) {
		CLanServer::Stop();
	}
}

bool CLanOdbcServer::BindParameter(DBConnection* dbConn, INT32 paramIndex, bool* value) {
	if (!dbConn->BindParam(paramIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindParameter(DBConnection* dbConn, INT32 paramIndex, float* value) {
	if (!dbConn->BindParam(paramIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindParameter(DBConnection* dbConn, INT32 paramIndex, double* value) {
	if (!dbConn->BindParam(paramIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindParameter(DBConnection* dbConn, INT32 paramIndex, INT8* value) {
	if (!dbConn->BindParam(paramIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindParameter(DBConnection* dbConn, INT32 paramIndex, INT16* value) {
	if (!dbConn->BindParam(paramIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindParameter(DBConnection* dbConn, INT32 paramIndex, INT32* value) {
	if (!dbConn->BindParam(paramIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindParameter(DBConnection* dbConn, INT32 paramIndex, INT64* value) {
	if (!dbConn->BindParam(paramIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindParameter(DBConnection* dbConn, INT32 paramIndex, TIMESTAMP_STRUCT* value) {
	if (!dbConn->BindParam(paramIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindParameter(DBConnection* dbConn, INT32 paramIndex, const WCHAR* str) {
	if (!dbConn->BindParam(paramIndex, str)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindParameter(DBConnection* dbConn, INT32 paramIndex, const BYTE* bin, INT32 size) {
	if (!dbConn->BindParam(paramIndex, bin, size)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindColumn(DBConnection* dbConn, INT32 columnIndex, bool* value) {
	if (!dbConn->BindCol(columnIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindColumn(DBConnection* dbConn, INT32 columnIndex, float* value) {
	if (!dbConn->BindCol(columnIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindColumn(DBConnection* dbConn, INT32 columnIndex, double* value) {
	if (!dbConn->BindCol(columnIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindColumn(DBConnection* dbConn, INT32 columnIndex, INT8* value) {
	if (!dbConn->BindCol(columnIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindColumn(DBConnection* dbConn, INT32 columnIndex, INT16* value) {
	if (!dbConn->BindCol(columnIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindColumn(DBConnection* dbConn, INT32 columnIndex, INT32* value) {
	if (!dbConn->BindCol(columnIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindColumn(DBConnection* dbConn, INT32 columnIndex, INT64* value) {
	if (!dbConn->BindCol(columnIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindColumn(DBConnection* dbConn, INT32 columnIndex, TIMESTAMP_STRUCT* value) {
	if (!dbConn->BindCol(columnIndex, value)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindColumn(DBConnection* dbConn, INT32 columnIndex, WCHAR* str, INT32 size, SQLLEN* index) {
	if (!dbConn->BindCol(columnIndex, str, size, index)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindColumn(DBConnection* dbConn, INT32 columnIndex, BYTE* bin, INT32 size, SQLLEN* index) {
	if (!dbConn->BindCol(columnIndex, bin, size, index)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}

bool CLanOdbcServer::BindParameter(DBConnection* dbConn, SQLPOINTER dataPtr, SQLUSMALLINT paramIndex, SQLULEN len, SQLSMALLINT cType, SQLSMALLINT sqlType) {
	SQLLEN index = 0;
	if (!dbConn->BindParam(paramIndex, cType, sqlType, len, dataPtr, &index)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}
bool CLanOdbcServer::BindColumn(DBConnection* dbConn, SQLPOINTER outValue, SQLUSMALLINT columnIndex, SQLULEN len, SQLSMALLINT cType) {
	SQLLEN index = 0;
	if (!dbConn->BindCol(columnIndex, cType, len, outValue, &index)) {
		dbConn->Unbind();
		return false;
	}
	return true;
}

void CLanOdbcServer::UnBind(DBConnection* dbConn)
{
	if (dbConn != nullptr) {
		dbConn->Unbind();
	}
}

bool CLanOdbcServer::ExecQuery(DBConnection* dbConn, const wchar_t* query) {
	bool ret = false;
	if (dbConn->Execute(query)) {
		ret = true;
	}
	return ret;
}

bool CLanOdbcServer::FetchQuery(DBConnection* dbConn) {
	if (dbConn == NULL) {
		return false;
	}

	dbConn->Fetch();
	return true;
}

INT32 CLanOdbcServer::GetRowCount(DBConnection* dbConn)
{
	return dbConn->GetRowCount();
}
