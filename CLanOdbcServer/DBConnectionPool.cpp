#include "DBConnectionPool.h"

/*-------------------
	DBConnectionPool
--------------------*/
bool DBConnectionPool::Connect(INT32 connectionCount, const WCHAR* connectionString)
{
	// 1. SQL 환경 핸들을 할당
	if (::SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &m_SqlEnvironment) != SQL_SUCCESS)
		return false;

	// 2. SQL 환경 핸들 이용하여 ODBC 버전 설정
	if (::SQLSetEnvAttr(m_SqlEnvironment, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) != SQL_SUCCESS)
		return false;

	// 3. connectionCount 갯수만큼 연결을 맺어준다(연결 생성). 
	//		- DBConnection 객체를 통해 간접적으로 DB에 연결을 요청한다. 
	//		- (DB와 연결된 스레드가 여러 개라면, 커넥션 카운터를 이 스레드 갯수만큼 만들수도 있다.)
	std::lock_guard<std::mutex> lockGuard(m_DBConnectionsMtx);
	for (INT32 i = 0; i < connectionCount; i++)
	{
		DBConnection* connection = new DBConnection();
		// 할당 받은 SQL 환경 핸들을 전달한다. 
		if (connection->Connect(m_SqlEnvironment, connectionString) == false)
			return false;
		m_DBConnections.push_back(connection);
	}

	return true;
}

void DBConnectionPool::Clear()
{
	// 1. SQL 환경 핸들 반환
	if (m_SqlEnvironment != SQL_NULL_HANDLE)
	{
		::SQLFreeHandle(SQL_HANDLE_ENV, m_SqlEnvironment);
		m_SqlEnvironment = SQL_NULL_HANDLE;
	}

	// 2. 커넥션 풀의 커넥션 들에 대한 메모리를 반환
	std::lock_guard<std::mutex> lockGuard(m_DBConnectionsMtx);
	for (DBConnection* connection : m_DBConnections)
		delete connection;

	m_DBConnections.clear();
}

DBConnection* DBConnectionPool::Pop()
{
	std::lock_guard<std::mutex> lockGuard(m_DBConnectionsMtx);

	if (m_DBConnections.empty())
		return nullptr;

	DBConnection* connection = m_DBConnections.back();
	m_DBConnections.pop_back();
	return connection;
}

void DBConnectionPool::Push(DBConnection* connection)
{
	std::lock_guard<std::mutex> lockGuard(m_DBConnectionsMtx);

	m_DBConnections.push_back(connection);
}
