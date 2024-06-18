#include "DBConnectionPool.h"

/*-------------------
	DBConnectionPool
--------------------*/
bool DBConnectionPool::Connect(INT32 connectionCount, const WCHAR* connectionString)
{
	// 1. SQL ȯ�� �ڵ��� �Ҵ�
	if (::SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &m_SqlEnvironment) != SQL_SUCCESS)
		return false;

	// 2. SQL ȯ�� �ڵ� �̿��Ͽ� ODBC ���� ����
	if (::SQLSetEnvAttr(m_SqlEnvironment, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) != SQL_SUCCESS)
		return false;

	// 3. connectionCount ������ŭ ������ �ξ��ش�(���� ����). 
	//		- DBConnection ��ü�� ���� ���������� DB�� ������ ��û�Ѵ�. 
	//		- (DB�� ����� �����尡 ���� �����, Ŀ�ؼ� ī���͸� �� ������ ������ŭ ������� �ִ�.)
	std::lock_guard<std::mutex> lockGuard(m_DBConnectionsMtx);
	for (INT32 i = 0; i < connectionCount; i++)
	{
		DBConnection* connection = new DBConnection();
		// �Ҵ� ���� SQL ȯ�� �ڵ��� �����Ѵ�. 
		if (connection->Connect(m_SqlEnvironment, connectionString) == false)
			return false;
		m_DBConnections.push_back(connection);
	}

	return true;
}

void DBConnectionPool::Clear()
{
	// 1. SQL ȯ�� �ڵ� ��ȯ
	if (m_SqlEnvironment != SQL_NULL_HANDLE)
	{
		::SQLFreeHandle(SQL_HANDLE_ENV, m_SqlEnvironment);
		m_SqlEnvironment = SQL_NULL_HANDLE;
	}

	// 2. Ŀ�ؼ� Ǯ�� Ŀ�ؼ� �鿡 ���� �޸𸮸� ��ȯ
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
