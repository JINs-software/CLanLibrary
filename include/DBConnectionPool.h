#pragma once
#include "DBConnection.h"
#include <vector>
#include <queue>
#include <mutex>

// ������ �̸� ���� �� �ΰ�, DB ��û�� �ʿ��� ������ Pool���� �����Ѵ�.
// DB Ŀ�ؼ� Ǯ�� ������ �ϳ��� ����(like �̱���)
class DBConnectionPool
{
private:
	// SQL ȯ�� �ڵ�
	SQLHENV						m_SqlEnvironment = SQL_NULL_HANDLE;

	// DB ���� �������� ��� ����
	//std::vector<DBConnection*>	m_DBConnections;
	std::queue<DBConnection*>		m_DBConnectionsQueue;
	std::mutex					m_DBConnectionsMtx;

	bool						m_DBConnErrorLogFlag;

public:
	DBConnectionPool() : DBConnectionPool(false) {}
	DBConnectionPool(bool dbConnErrorLog) : m_DBConnErrorLogFlag(dbConnErrorLog) {}
	~DBConnectionPool() {
		Clear();
	}

	// Ŀ�ؼ� Ǯ�� ����� �Լ�(������ ������ �� �� ���� ȣ��Ǵ� �Լ�)
	// - connectionCount: ���� DB Ŀ�ؼ� ����
	// - connectionString: ���� DB�� ȯ�� ������ ���� ���ڿ�
	bool					Connect(INT32 connectionCount, const WCHAR* connectionString);

	void					Clear();

	// ������ Ŀ�ؼ� Ǯ �� ȹ��(POP)
	DBConnection*			Pop();
	// Ŀ�ؼ� Ǯ�� Ŀ�ؼ� �ݳ�(PUSH)
	void					Push(DBConnection* connection, bool isDisconnected = false, bool tryToConnect = false, const WCHAR* connectionString = NULL);
};

