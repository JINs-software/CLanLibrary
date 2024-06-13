#pragma once
#include "DBConnection.h"
#include <vector>
#include <mutex>

/*-------------------
	DBConnectionPool
--------------------*/
// ������ �̸� ���� �� �ΰ�, DB ��û�� �ʿ��� ������ Pool���� �����Ѵ�.
// DB Ŀ�ؼ� Ǯ�� ������ �ϳ��� ����(like �̱���)
class DBConnectionPool
{
public:
	DBConnectionPool();
	~DBConnectionPool();

	// Ŀ�ؼ� Ǯ�� ����� �Լ�(�� ���� ������ ������, ���� DB�� ȯ�� ������ ���� ���ڿ�
	// ������ ������ �� �� ���� ȣ��Ǵ� �Լ�
	bool					Connect(INT32 connectionCount, const WCHAR* connectionString);
	void					Clear();

	// Ŀ�ؼ� Ǯ �� �ϳ��� POP
	DBConnection* Pop();
	// Ŀ�ؼ� Ǯ�� �ݳ�(PUSH)
	void					Push(DBConnection* connection);

private:
	// ����ȭ ��ü
	//USE_LOCK;
	std::mutex				m_Mtx;

	// SQL ȯ�� �ڵ�
	SQLHENV					_environment = SQL_NULL_HANDLE;

	// DB ���� �������� ��� ����
	std::vector<DBConnection*>	_connections;

};

