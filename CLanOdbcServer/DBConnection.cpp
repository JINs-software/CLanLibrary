#include <stdlib.h>
#include <iostream>
#include "DBConnection.h"

/*----------------
	DBConnection
-----------------*/

bool DBConnection::Connect(SQLHENV henv, const WCHAR* connectionString)
{
	// ���޹��� SQL ȯ�� �ڵ��� �������� ���� Ŀ�ؼ� �ڵ��� �Ҵ�޴´�.
	if (::SQLAllocHandle(SQL_HANDLE_DBC, henv, &_connection) != SQL_SUCCESS)
		return false;

	// Ŀ�ؼ� ���ڿ��� �������� �������� DB ���� ����
	WCHAR stringBuffer[MAX_PATH] = { 0 };
	::wcscpy_s(stringBuffer, connectionString);

	WCHAR resultString[MAX_PATH] = { 0 };
	SQLSMALLINT resultStringLen = 0;

	SQLRETURN ret = ::SQLDriverConnectW(	// WCHAR ���ڿ� ���� ���� �Լ� ȣ��
		_connection,
		NULL,
		reinterpret_cast<SQLWCHAR*>(stringBuffer),
		_countof(stringBuffer),
		OUT reinterpret_cast<SQLWCHAR*>(resultString),	// ��� �޽��� ����
		_countof(resultString),
		OUT & resultStringLen,
		SQL_DRIVER_NOPROMPT
	);

	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
		std::wcout << resultString << std::endl;
		HandleError(ret);
		return false;
	}

	// statement �ڵ� �Ҵ�
	if ((ret = ::SQLAllocHandle(SQL_HANDLE_STMT, _connection, &_statement)) != SQL_SUCCESS) {
		HandleError(ret);
		return false;
	}

	return (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO);
}

void DBConnection::Clear()
{
	// �Ҵ���� �ڵ� ����

	if (_connection != SQL_NULL_HANDLE)
	{
		::SQLFreeHandle(SQL_HANDLE_DBC, _connection);
		_connection = SQL_NULL_HANDLE;
	}

	if (_statement != SQL_NULL_HANDLE)
	{
		::SQLFreeHandle(SQL_HANDLE_STMT, _statement);
		_statement = SQL_NULL_HANDLE;
	}
}

bool DBConnection::Execute(const WCHAR* query)
{
	// SQL ������ ���ڷ� �޾�, SQLExecDirect �Լ��� ����
	SQLRETURN ret = ::SQLExecDirectW(_statement, (SQLWCHAR*)query, SQL_NTSL);
	if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO)
		return true;

	HandleError(ret);
	return false;
}

bool DBConnection::Fetch()
{
	// SQLFetch�� ���� ��ȯ �� ���� ���� �����Ͱ� ����
	SQLRETURN ret = ::SQLFetch(_statement);

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
	SQLRETURN ret = SQLGetData(_statement, 1, SQL_C_SLONG, &count, sizeof(count), &indicator);
	if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
		data = count;
		return true;
	}

	return false;
}

INT32 DBConnection::GetRowCount()
{
	SQLLEN count = 0;
	SQLRETURN ret = ::SQLRowCount(_statement, OUT & count);

	if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO)
		return static_cast<INT32>(count);

	return -1;
}

void DBConnection::Unbind()
{
	::SQLFreeStmt(_statement, SQL_UNBIND);
	::SQLFreeStmt(_statement, SQL_RESET_PARAMS);
	::SQLFreeStmt(_statement, SQL_CLOSE);
}

bool DBConnection::BindParam(SQLUSMALLINT paramIndex, SQLSMALLINT cType, SQLSMALLINT sqlType, SQLULEN len, SQLPOINTER ptr, SQLLEN* index)
{
	SQLRETURN ret = ::SQLBindParameter(_statement, paramIndex, SQL_PARAM_INPUT, cType, sqlType, len, 0, ptr, 0, index);
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	{
		HandleError(ret);
		return false;
	}

	return true;
}

bool DBConnection::BindCol(SQLUSMALLINT columnIndex, SQLSMALLINT cType, SQLULEN len, SQLPOINTER value, SQLLEN* index)
{
	SQLRETURN ret = ::SQLBindCol(_statement, columnIndex, cType, value, len, index);
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	{
		HandleError(ret);
		return false;
	}

	return true;
}

void DBConnection::HandleError(SQLRETURN ret)
{
	if (ret == SQL_SUCCESS)
		return;

	SQLSMALLINT index = 1;
	SQLWCHAR sqlState[MAX_PATH] = { 0 };
	SQLINTEGER nativeErr = 0;
	SQLWCHAR errMsg[MAX_PATH] = { 0 };		// ���� ���� ����
	SQLSMALLINT msgLen = 0;
	SQLRETURN errorRet = 0;

	while (true)
	{
		errorRet = ::SQLGetDiagRecW(	// ���� �޽��� ���� �Լ�
			SQL_HANDLE_STMT,
			_statement,
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


		// TODO : Log
		// ���� ��ȣ ���
		std::wcout.imbue(std::locale("kor"));
		std::wcout << errMsg << std::endl;		// ���� ���� ���
		// ���� �ܰ� ���������� �ܼ� ���

		index++;
	}
}
