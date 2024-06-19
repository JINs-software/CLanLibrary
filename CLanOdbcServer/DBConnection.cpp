#include <stdlib.h>
#include <iostream>
#include "DBConnection.h"

bool DBConnection::Connect(SQLHENV henv, const WCHAR* connectionString)
{
	SQLRETURN ret;

	// 1. ���޹��� SQL ȯ�� �ڵ��� �������� ���� Ŀ�ؼ� �ڵ��� �Ҵ�޴´�.
	if (::SQLAllocHandle(SQL_HANDLE_DBC, henv, &m_DBConnection) != SQL_SUCCESS)
		return false;

	// 2. Ŀ�ؼ� ���ڿ��� �������� �������� DB ���� ����
	WCHAR stringBuffer[MAX_PATH] = { 0 };
	::wcscpy_s(stringBuffer, connectionString);

	WCHAR resultString[MAX_PATH] = { 0 };
	SQLSMALLINT resultStringLen = 0;

	ret = ::SQLDriverConnectW(	// WCHAR ���ڿ� ���� ���� �Լ� ȣ��
		m_DBConnection,
		NULL,
		reinterpret_cast<SQLWCHAR*>(stringBuffer),
		_countof(stringBuffer),
		OUT reinterpret_cast<SQLWCHAR*>(resultString),	// ��� �޽��� ����
		_countof(resultString),
		OUT & resultStringLen,
		SQL_DRIVER_NOPROMPT
	);

	if (!(ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO)) {
		std::wcout << L"SQLDriverConnectW return Fail.. (result string: " << resultString << L")" <<std::endl;
		HandleError(ret, SQL_HANDLE_DBC, m_DBConnection);

		return false;
	}

	// 3. statement �ڵ��� �Ҵ� �޴´�.
	if ((ret = ::SQLAllocHandle(SQL_HANDLE_STMT, m_DBConnection, &m_Statement)) != SQL_SUCCESS) {
		HandleError(ret);
		return false;
	}

	return (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO);
}

void DBConnection::Clear()
{
	// �Ҵ���� �ڵ� ����

	if (m_DBConnection != SQL_NULL_HANDLE)
	{
		::SQLFreeHandle(SQL_HANDLE_DBC, m_DBConnection);
		m_DBConnection = SQL_NULL_HANDLE;
	}

	if (m_Statement != SQL_NULL_HANDLE)
	{
		::SQLFreeHandle(SQL_HANDLE_STMT, m_Statement);
		m_Statement = SQL_NULL_HANDLE;
	}
}

bool DBConnection::Execute(const WCHAR* query)
{
	// SQL ������ ���ڷ� �޾�, SQLExecDirect �Լ��� ����
	SQLRETURN ret = ::SQLExecDirectW(m_Statement, (SQLWCHAR*)query, SQL_NTSL);
	if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
		return true;
	}

	HandleError(ret);
	return false;
}

bool DBConnection::Fetch()
{
	// SQLFetch�� ���� ��ȯ ��, ���� ���� �����Ͱ� ����
	SQLRETURN ret = ::SQLFetch(m_Statement);

	switch (ret)
	{
	case SQL_SUCCESS:
	case SQL_SUCCESS_WITH_INFO:
		return true;
	case SQL_NO_DATA:		// ������ �����̳� ��ȯ �����Ͱ� ���� ���
		return false;
	case SQL_ERROR:			// ���� ���� ��ü���� ���� �߻�
		HandleError(ret);
		return false;
	default:
		return true;
	}
}

bool DBConnection::GetSQLData(INT32& data)
{
	SQLLEN indicator;
	SQLINTEGER count;
	SQLRETURN ret = SQLGetData(m_Statement, 1, SQL_C_SLONG, &count, sizeof(count), &indicator);
	if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
		data = count;
		return true;
	}

	return false;
}

INT32 DBConnection::GetRowCount()
{
	SQLLEN count = 0;
	SQLRETURN ret = ::SQLRowCount(m_Statement, OUT & count);

	if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO)
		return static_cast<INT32>(count);

	return -1;
}

bool DBConnection::BindParam(INT32 paramIndex, bool* value) {
	SQLLEN index = 0;
	return BindParam(paramIndex, SQL_C_TINYINT, SQL_TINYINT, sizeof(bool), value, &index);
}
bool DBConnection::BindParam(INT32 paramIndex, float* value) {
	SQLLEN index = 0;
	return BindParam(paramIndex, SQL_C_FLOAT, SQL_FLOAT, 0, value, &index);	// �Ϲ� ������ �ƴ� ��� 'len'�� 0
}
bool DBConnection::BindParam(INT32 paramIndex, double* value) {
	SQLLEN index = 0;
	return BindParam(paramIndex, SQL_C_DOUBLE, SQL_DOUBLE, 0, value, &index);
}
bool DBConnection::BindParam(INT32 paramIndex, INT8* value) {
	SQLLEN index = 0;
	return BindParam(paramIndex, SQL_C_TINYINT, SQL_TINYINT, sizeof(INT8), value, &index);
}
bool DBConnection::BindParam(INT32 paramIndex, INT16* value) {
	SQLLEN index = 0;
	return BindParam(paramIndex, SQL_C_SHORT, SQL_SMALLINT, sizeof(INT16), value, &index);
}
bool DBConnection::BindParam(INT32 paramIndex, INT32* value) {
	SQLLEN index = 0;
	return BindParam(paramIndex, SQL_C_LONG, SQL_INTEGER, sizeof(INT32), value, &index);
}
bool DBConnection::BindParam(INT32 paramIndex, INT64* value) {
	SQLLEN index = 0;
	return BindParam(paramIndex, SQL_C_SBIGINT, SQL_BIGINT, sizeof(INT64), value, &index);
}
bool DBConnection::BindParam(INT32 paramIndex, TIMESTAMP_STRUCT* value) {
	SQLLEN index = 0;
	return BindParam(paramIndex, SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, sizeof(TIMESTAMP_STRUCT), value, &index);
}
bool DBConnection::BindParam(INT32 paramIndex, CONST WCHAR* str) {
	SQLLEN index = SQL_NTSL;
	SQLULEN size = static_cast<SQLULEN>((::wcslen(str) + 1) * 2);

	if (size > WVARCHAR_MAX)
		return BindParam(paramIndex, SQL_C_WCHAR, SQL_WLONGVARCHAR, size, (SQLPOINTER)str, &index);
	else
		return BindParam(paramIndex, SQL_C_WCHAR, SQL_WVARCHAR, size, (SQLPOINTER)str, &index);
}
bool DBConnection::BindParam(INT32 paramIndex, const BYTE* bin, INT32 size) {
	SQLLEN index;
	if (bin == nullptr)
	{
		index = SQL_NULL_DATA;
		size = 1;
	}
	else {
		index = size;
	}

	if (size > BINARY_MAX)
		return BindParam(paramIndex, SQL_C_BINARY, SQL_LONGVARBINARY, size, (BYTE*)bin, &index);
	else
		return BindParam(paramIndex, SQL_C_BINARY, SQL_BINARY, size, (BYTE*)bin, &index);
}

bool DBConnection::BindCol(INT32 columnIndex, bool* value)
{
	SQLLEN index = 0;
	return BindCol(columnIndex, SQL_C_TINYINT, sizeof(bool), value, &index);
}

bool DBConnection::BindCol(INT32 columnIndex, float* value)
{
	SQLLEN index = 0;
	return BindCol(columnIndex, SQL_C_FLOAT, sizeof(float), value, &index);
}

bool DBConnection::BindCol(INT32 columnIndex, double* value)
{
	SQLLEN index = 0;
	return BindCol(columnIndex, SQL_C_DOUBLE, sizeof(double), value, &index);
}

bool DBConnection::BindCol(INT32 columnIndex, INT8* value)
{
	SQLLEN index = 0;
	return BindCol(columnIndex, SQL_C_TINYINT, sizeof(INT8), value, &index);
}

bool DBConnection::BindCol(INT32 columnIndex, INT16* value)
{
	SQLLEN index = 0;
	return BindCol(columnIndex, SQL_C_SHORT, sizeof(INT16), value, &index);
}

bool DBConnection::BindCol(INT32 columnIndex, INT32* value)
{
	SQLLEN index = 0;
	return BindCol(columnIndex, SQL_C_LONG, sizeof(INT32), value, &index);
}

bool DBConnection::BindCol(INT32 columnIndex, INT64* value)
{
	SQLLEN index = 0;
	return BindCol(columnIndex, SQL_C_SBIGINT, sizeof(INT64), value, &index);
}

bool DBConnection::BindCol(INT32 columnIndex, TIMESTAMP_STRUCT* value)
{
	SQLLEN index = 0;
	return BindCol(columnIndex, SQL_C_TYPE_TIMESTAMP, sizeof(TIMESTAMP_STRUCT), value, &index);
}

bool DBConnection::BindCol(INT32 columnIndex, WCHAR* str, INT32 size, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_C_WCHAR, size, str, index);
}

bool DBConnection::BindCol(INT32 columnIndex, BYTE* bin, INT32 size, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_BINARY, size, bin, index);
}

void DBConnection::Unbind()
{
	::SQLFreeStmt(m_Statement, SQL_UNBIND);
	::SQLFreeStmt(m_Statement, SQL_RESET_PARAMS);
	::SQLFreeStmt(m_Statement, SQL_CLOSE);
}

bool DBConnection::BindParam(SQLUSMALLINT paramIndex, SQLSMALLINT cType, SQLSMALLINT sqlType, SQLULEN len, SQLPOINTER ptr, SQLLEN* index)
{	
	// len: ������ ũ��
	// ptr: ���޵Ǵ� ������(������)
	// index: ���ڿ��� ���� ���� ���� �����Ϳ��� �ʿ��� �μ�, �Ϲ������� ���� ũ�� �������� ��� 0�� ���� ���� ������ �����͸� ����
	SQLRETURN ret = ::SQLBindParameter(m_Statement, paramIndex, SQL_PARAM_INPUT, cType, sqlType, len, 0, ptr, 0, index);
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	{
		HandleError(ret);
		return false;
	}

	return true;
}

bool DBConnection::BindCol(SQLUSMALLINT columnIndex, SQLSMALLINT cType, SQLULEN len, SQLPOINTER value, SQLLEN* index)
{
	SQLRETURN ret = ::SQLBindCol(m_Statement, columnIndex, cType, value, len, index);
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	{
		HandleError(ret);
		return false;
	}

	return true;
}

void DBConnection::HandleError(SQLRETURN ret, SQLSMALLINT errMsgBuffLen, SQLWCHAR* errMsgOut, SQLSMALLINT* errMsgLenOut)
{
	if (ret == SQL_SUCCESS)
		return;

	SQLSMALLINT index = 1;
	SQLWCHAR sqlState[MAX_PATH] = { 0 };
	SQLINTEGER nativeErr = 0;
	SQLWCHAR errMsg[MAX_PATH] = { 0 };		// ���� ���� ����
	SQLSMALLINT msgLen = 0;
	SQLRETURN errorRet = 0;

	SQLSMALLINT errMsgOffset = 0;
	if (errMsgLenOut != NULL) {
		*errMsgLenOut = 0;
	}

	while (true)
	{
		errorRet = ::SQLGetDiagRecW(	// ���� �޽��� ���� �Լ�
			SQL_HANDLE_STMT,
			m_Statement,
			index,
			sqlState,
			OUT &nativeErr,
			errMsg,
			_countof(errMsg),
			OUT &msgLen
		);

		// ������ ���ų�, ������ ���ٸ� ���� Ż��
		if (errorRet == SQL_NO_DATA)
			break;
		if (errorRet == SQL_SUCCESS || errorRet == SQL_SUCCESS_WITH_INFO)
			break;

		if (errMsgBuffLen >= errMsgOffset + msgLen + 1) {
			if (errMsgOut != NULL) {
				std::wcout.imbue(std::locale("kor"));
				memcpy(&errMsgOut[errMsgOffset], errMsg, msgLen);
				errMsgOut[errMsgOffset + msgLen] = NULL;
				errMsgOffset += (msgLen + 1);
			}
		}

		index++;
	}
}

void DBConnection::HandleError(SQLRETURN ret, SQLSMALLINT hType, SQLHANDLE handle, SQLSMALLINT errMsgBuffLen, SQLWCHAR* errMsgOut, SQLSMALLINT* errMsgLenOut)
{
	if (ret == SQL_SUCCESS)
		return;

	SQLSMALLINT index = 1;
	SQLWCHAR sqlState[MAX_PATH] = { 0 };
	SQLINTEGER nativeErr = 0;
	SQLWCHAR errMsg[MAX_PATH] = { 0 };		// ���� ���� ����
	SQLSMALLINT msgLen = 0;
	SQLRETURN errorRet = 0;

	SQLSMALLINT errMsgOffset = 0;
	if (errMsgLenOut != NULL) {
		*errMsgLenOut = 0;
	}

	while (true)
	{
		errorRet = ::SQLGetDiagRecW(	// ���� �޽��� ���� �Լ�
			hType,
			handle,
			index,
			sqlState,
			OUT & nativeErr,
			errMsg,
			_countof(errMsg),
			OUT & msgLen
		);

		// ������ ���ų�, ������ ���ٸ� ���� Ż��
		if (errorRet == SQL_NO_DATA)
			break;
		if (errorRet != SQL_SUCCESS && errorRet != SQL_SUCCESS_WITH_INFO)
			break;

		std::wcout << errMsg << std::endl;

		if (errMsgBuffLen >= errMsgOffset + msgLen + 1) {
			if (errMsgOut != NULL) {
				std::wcout.imbue(std::locale("kor"));
				memcpy(&errMsgOut[errMsgOffset], errMsg, msgLen);
				errMsgOut[errMsgOffset + msgLen] = NULL;
				errMsgOffset += (msgLen + 1);
			}
		}

		index++;
	}
}
