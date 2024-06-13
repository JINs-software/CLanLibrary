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

	// SQL ȯ�� �ڵ��� �Ҵ�
	if (::SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &_environment) != SQL_SUCCESS)
		return false;

	// SQL ȯ�� �ڵ� �̿��Ͽ� ODBC ���� ����
	if (::SQLSetEnvAttr(_environment, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) != SQL_SUCCESS)
		return false;

	// connectionCount ������ŭ ������ �ξ��ش�. 
	// DBConnection ��ü�� ���� ���������� DB�� ������ ��û�Ѵ�. 
	// DB�� ����� �����尡 ���� �����, Ŀ�ؼ� ī���͸� �� ������ ������ŭ ������� �ִ�.
	for (INT32 i = 0; i < connectionCount; i++)
	{
		//DBConnection* connection = xnew<DBConnection>();
		DBConnection* connection = new DBConnection();

		// �Ҵ� ���� SQL ȯ�� �ڵ��� �����Ѵ�. 
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

	// SQL ȯ�� �ڵ� ��ȯ
	if (_environment != SQL_NULL_HANDLE)
	{
		::SQLFreeHandle(SQL_HANDLE_ENV, _environment);
		_environment = SQL_NULL_HANDLE;
	}

	// Ŀ�ؼ� Ǯ�� Ŀ�ؼ� �鿡 ���� �޸𸮸� ��ȯ
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
