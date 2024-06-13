#pragma once
#include "DBConnection.h"
#include <vector>
#include <mutex>

/*-------------------
	DBConnectionPool
--------------------*/
// 연결을 미리 여러 개 맺고, DB 요청이 필요할 때마다 Pool에서 재사용한다.
// DB 커넥션 풀은 전역에 하나만 존재(like 싱글톤)
class DBConnectionPool
{
public:
	DBConnectionPool();
	~DBConnectionPool();

	// 커넥션 풀을 만드는 함수(몇 개의 연결을 만들지, 연결 DB와 환경 설정을 위한 문자열
	// 서버가 구동될 떄 한 번만 호출되는 함수
	bool					Connect(INT32 connectionCount, const WCHAR* connectionString);
	void					Clear();

	// 커넥션 풀 중 하나를 POP
	DBConnection* Pop();
	// 커넥션 풀에 반납(PUSH)
	void					Push(DBConnection* connection);

private:
	// 동기화 객체
	//USE_LOCK;
	std::mutex				m_Mtx;

	// SQL 환경 핸들
	SQLHENV					_environment = SQL_NULL_HANDLE;

	// DB 연결 단위들을 담는 벡터
	std::vector<DBConnection*>	_connections;

};

