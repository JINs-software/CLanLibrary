#pragma once

#include "CLanServer.h"

class CLanClient : public CLanServer
{
private:
	SOCKET		m_CLanClientSock;
	JBuffer		m_RecvBufferFromCLanServer;
	JBuffer		m_SendBufferToCLanServer;
	std::mutex	m_SendBufferMtx;
	UINT		m_SendFlag;

	WSAOVERLAPPED	m_RecvOverlapped;
	WSAOVERLAPPED	m_SendOverlapped;

	HANDLE			m_CLanNetworkThread;

	static const BYTE	m_EventCnt = 3;
	static enum enEvent {
		enThreadExit = 0,
		enCLanRecv,
		enCLanSend
	};
	HANDLE		m_Events[m_EventCnt];

protected:
	bool Connect(const CHAR* clanServerIP, USHORT clanserverPort, DWORD numOfIocpConcurrentThrd);		//	���ε� IP, ����IP / ��Ŀ������ �� / ���ۿɼ�
	bool Disconnect();							
	
	bool SendPacketToCLanServer(JBuffer* sendPacket, bool encoded = false);
	void SendPostToCLanServer();

	virtual void OnEnterJoinServer() = 0;		//< �������� ���� ���� ��
	virtual void OnLeaveServer() = 0;			//< �������� ������ �������� ��

	virtual void OnRecvFromCLanServer(JBuffer& recvBuff) = 0;

private:
	static UINT __stdcall CLanNetworkFunc(void* arg);
};

