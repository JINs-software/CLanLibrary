#pragma once
#include "DBConnection.h"
#include <vector>
#include <queue>
#include <mutex>

// 연결을 미리 여러 개 맺고, DB 요청이 필요할 때마다 Pool에서 재사용한다.
// DB 커넥션 풀은 전역에 하나만 존재(like 싱글톤)
class DBConnectionPool
{
private:
	// SQL 환경 핸들
	SQLHENV						m_SqlEnvironment = SQL_NULL_HANDLE;

	// DB 연결 단위들을 담는 벡터
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

	// 커넥션 풀을 만드는 함수(서버가 구동될 떄 한 번만 호출되는 함수)
	// - connectionCount: 생성 DB 커넥션 갯수
	// - connectionString: 연결 DB와 환경 설정을 위한 문자열
	bool					Connect(INT32 connectionCount, const WCHAR* connectionString);

	void					Clear();

	// 생성된 커넥션 풀 중 획득(POP)
	DBConnection*			Pop();
	// 커넥션 풀에 커넥션 반납(PUSH)
	void					Push(DBConnection* connection, bool isDisconnected = false, bool tryToConnect = false, const WCHAR* connectionString = NULL);
};

