#pragma once
#include <Windows.h>
#include <sql.h>
#include <sqlext.h>

/*----------------
	DBConnection
-----------------*/

class DBConnection
{
public:
	bool			Connect(SQLHENV henv, const WCHAR* connectionString);
	void			Clear();

	// 쿼리를 실행하는 SQL 함수
	bool			Execute(const WCHAR* query);

	// SELECT 계열의 쿼리를 요청할 때 결과를 받기 위한 함수
	bool			Fetch();

	bool			GetSQLData(INT32& data);

	// 데이터가 몇 개가 있는지 확인하기 위한 함수
	INT32			GetRowCount();

	// 바인딩한 것에 대해 정리하는 함수
	void			Unbind();

public:
	// SQL 쿼리를 작성할 때 인자들을 전달하기 위한 함수
	bool			BindParam(SQLUSMALLINT paramIndex, SQLSMALLINT cType, SQLSMALLINT sqlType, SQLULEN len, SQLPOINTER ptr, SQLLEN* index);
	bool			BindCol(SQLUSMALLINT columnIndex, SQLSMALLINT cType, SQLULEN len, SQLPOINTER value, SQLLEN* index);
	void			HandleError(SQLRETURN ret);

private:
	// DB 연결 핸들 
	SQLHDBC			_connection = SQL_NULL_HANDLE;

	// SQL 상태 관리 핸들
	// API에 전달하거나, 출력 인수가 될 수 있는 "상태"로 해석
	SQLHSTMT		_statement = SQL_NULL_HANDLE;
};

