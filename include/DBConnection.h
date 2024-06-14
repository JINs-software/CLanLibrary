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

	// ������ �����ϴ� SQL �Լ�
	bool			Execute(const WCHAR* query);

	// SELECT �迭�� ������ ��û�� �� ����� �ޱ� ���� �Լ�
	bool			Fetch();

	bool			GetSQLData(INT32& data);

	// �����Ͱ� �� ���� �ִ��� Ȯ���ϱ� ���� �Լ�
	INT32			GetRowCount();

	// ���ε��� �Ϳ� ���� �����ϴ� �Լ�
	void			Unbind();

public:
	// SQL ������ �ۼ��� �� ���ڵ��� �����ϱ� ���� �Լ�
	bool			BindParam(SQLUSMALLINT paramIndex, SQLSMALLINT cType, SQLSMALLINT sqlType, SQLULEN len, SQLPOINTER ptr, SQLLEN* index);
	bool			BindCol(SQLUSMALLINT columnIndex, SQLSMALLINT cType, SQLULEN len, SQLPOINTER value, SQLLEN* index);
	void			HandleError(SQLRETURN ret);

private:
	// DB ���� �ڵ� 
	SQLHDBC			_connection = SQL_NULL_HANDLE;

	// SQL ���� ���� �ڵ�
	// API�� �����ϰų�, ��� �μ��� �� �� �ִ� "����"�� �ؼ�
	SQLHSTMT		_statement = SQL_NULL_HANDLE;
};

