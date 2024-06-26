#pragma once
#include <Windows.h>
#include <sql.h>
#include <sqlext.h>
#include <mutex>

enum
{
	WVARCHAR_MAX = 4000,
	BINARY_MAX = 8000
};


class DBConnection
{
private:
	// DB 연결 핸들 
	SQLHDBC			m_DBConnection = SQL_NULL_HANDLE;

	// SQL 상태 관리 핸들
	// API에 인수를 전달하거나, 출력 인수로 데이터를 받을 수 있는 "상태"로 해석
	SQLHSTMT		m_Statement = SQL_NULL_HANDLE;

	bool				m_ConnectionErrorFileLogFlag;
	const wchar_t*		m_ConnectionErrLogFile = L"ConnectionErrLog.txt";
	static std::mutex	m_LogFileMtx;

public:
	DBConnection() : DBConnection(false) {}
	DBConnection(BOOL connectionErrorFileLogFlag) : m_ConnectionErrorFileLogFlag(connectionErrorFileLogFlag) {}
	~DBConnection() {
		Clear();
	}

	bool			Connect(SQLHENV henv, const WCHAR* connectionString);
	void			Clear();

	// DB 커넥션 연결 유지 확인을 위해 단순 쿼리 확인
	bool			Ping();

	// 쿼리를 실행하는 SQL 함수
	bool			Execute(const WCHAR* query);

	// SELECT 계열의 쿼리를 요청할 때 결과를 받기 위한 함수
	// - True 반환: 수신 받을 데이터 존재
	// - False 반환: 쿼리는 성공하였으나 수신 받을 데이터가 존재하지 않은 경우이거나 쿼리 자체가 실패
	bool			Fetch();

	bool			GetSQLData(INT32& data);

	// 데이터가 몇 개가 있는지 확인하기 위한 함수
	// (행 갯수 반환, SQLRowCount(..))
	// ( -1 반환 시 예외 처리 필요 )
	INT32			GetRowCount();

	// 이전 바인딩된 것들을 정리하는 함수
	void			Unbind();

public:
	bool			BindParam(INT32 paramIndex, bool* value);
	bool			BindParam(INT32 paramIndex, float* value);
	bool			BindParam(INT32 paramIndex, double* value);
	bool			BindParam(INT32 paramIndex, INT8* value);
	bool			BindParam(INT32 paramIndex, INT16* value);
	bool			BindParam(INT32 paramIndex, INT32* value);
	bool			BindParam(INT32 paramIndex, INT64* value);
	bool			BindParam(INT32 paramIndex, TIMESTAMP_STRUCT* value);
	bool			BindParam(INT32 paramIndex, const WCHAR* str);
	bool			BindParam(INT32 paramIndex, const BYTE* bin, INT32 size);

	bool			BindCol(INT32 columnIndex, bool* value);
	bool			BindCol(INT32 columnIndex, float* value);
	bool			BindCol(INT32 columnIndex, double* value);
	bool			BindCol(INT32 columnIndex, INT8* value);
	bool			BindCol(INT32 columnIndex, INT16* value);
	bool			BindCol(INT32 columnIndex, INT32* value);
	bool			BindCol(INT32 columnIndex, INT64* value);
	bool			BindCol(INT32 columnIndex, TIMESTAMP_STRUCT* value);
	bool			BindCol(INT32 columnIndex, WCHAR* str, INT32 size, SQLLEN* index);
	bool			BindCol(INT32 columnIndex, BYTE* bin, INT32 size, SQLLEN* index);

public:
	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// SQL 쿼리를 작성할 때 인자들을 전달하기 위한 함수
	// - cType: C형식 식별자
	// - sqlType: ODBC C typedef
	// (https://learn.microsoft.com/ko-kr/sql/odbc/reference/appendixes/c-data-types?view=sql-server-ver16)
	//////////////////////////////////////////////////////////////////////////////////////////////////////

	// BindParam: 쿼리의 'paramIndex' 인덱스 인자를 설정한 값으로 바인딩한다.
	bool			BindParam(SQLUSMALLINT paramIndex, SQLSMALLINT cType, SQLSMALLINT sqlType, SQLULEN len, SQLPOINTER ptr, SQLLEN* index);

	// SQL 실행 후 데이터를 읽기 위한 함수
	bool			BindCol(SQLUSMALLINT columnIndex, SQLSMALLINT cType, SQLULEN len, SQLPOINTER value, SQLLEN* index);

private:
	void			HandleError(SQLRETURN ret,	SQLSMALLINT errMsgBuffLen = 0, SQLWCHAR* errMsgOut = NULL,	SQLSMALLINT* errMsgLenOut = NULL);
	void			HandleError(SQLRETURN ret,	SQLSMALLINT hType,             SQLHANDLE handle,				SQLSMALLINT errMsgBuffLen = 0,	SQLWCHAR* errMsgOut = NULL, SQLSMALLINT* errMsgLenOut = NULL);
	void			ErrorMsgFileLogging(const SQLWCHAR* errMsg, SQLSMALLINT errMsgLen, const std::wstring& filePath);
};

