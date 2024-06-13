#include "DBConnectionPool.h"

/*-------------------
	DBConnectionPool
--------------------*/

DBConnectionPool::DBConnectionPool()
{

}

DBConnectionPool::~DBConnectionPool()
{
	Clear();
}

bool DBConnectionPool::Connect(INT32 connectionCount, const WCHAR* connectionString)
{
	//WRITE_LOCK;
	std::lock_guard<std::mutex> lockGuard(m_Mtx);

	// SQL 환경 핸들을 할당
	if (::SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &_environment) != SQL_SUCCESS)
		return false;

	// SQL 환경 핸들 이용하여 ODBC 버전 설정
	if (::SQLSetEnvAttr(_environment, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) != SQL_SUCCESS)
		return false;

	// connectionCount 갯수만큼 연결을 맺어준다. 
	// DBConnection 객체를 통해 간접적으로 DB에 연결을 요청한다. 
	// DB와 연결된 스레드가 여러 개라면, 커넥션 카운터를 이 스레드 갯수만큼 만들수도 있다.
	for (INT32 i = 0; i < connectionCount; i++)
	{
		//DBConnection* connection = xnew<DBConnection>();
		DBConnection* connection = new DBConnection();

		// 할당 받은 SQL 환경 핸들을 전달한다. 
		if (connection->Connect(_environment, connectionString) == false)
			return false;

		_connections.push_back(connection);
	}

	return true;
}

void DBConnectionPool::Clear()
{
	//WRITE_LOCK;
	std::lock_guard<std::mutex> lockGuard(m_Mtx);

	// SQL 환경 핸들 반환
	if (_environment != SQL_NULL_HANDLE)
	{
		::SQLFreeHandle(SQL_HANDLE_ENV, _environment);
		_environment = SQL_NULL_HANDLE;
	}

	// 커넥션 풀의 커넥션 들에 대한 메모리를 반환
	for (DBConnection* connection : _connections)
		//xdelete(connection);
		delete connection;

	_connections.clear();
}

DBConnection* DBConnectionPool::Pop()
{
	//WRITE_LOCK;
	std::lock_guard<std::mutex> lockGuard(m_Mtx);

	if (_connections.empty())
		return nullptr;

	DBConnection* connection = _connections.back();
	_connections.pop_back();
	return connection;
}

void DBConnectionPool::Push(DBConnection* connection)
{
	//WRITE_LOCK;
	std::lock_guard<std::mutex> lockGuard(m_Mtx);

	_connections.push_back(connection);
}
