#include "JClient.h"

#define CLIENT_WSABUF_ARRAY_DEFAULT_SIZE	100

bool JClient::InitClient(const CHAR* clanServerIP, USHORT clanserverPort)
{
	m_ClientSock = CreateWindowSocket_IPv4(true);
	SOCKADDR_IN serverAddr = CreateDestinationADDR(clanServerIP, clanserverPort);
	if (!ConnectToDestination(m_ClientSock, serverAddr)) {
		return false;
	}

	for (BYTE i = 0; i < m_EventCnt; i++) {
		if (m_Events[i] == NULL) {
			m_Events[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
		}
		else {
			ResetEvent(m_Events[i]);
		}
	}

	memset(&m_RecvOverlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_SendOverlapped, 0, sizeof(WSAOVERLAPPED));
	m_RecvOverlapped.hEvent = m_Events[enCLanRecv];
	m_SendOverlapped.hEvent = m_Events[enCLanSend];
}

void JClient::DeleteClient()
{
	closesocket(m_ClientSock);
	m_ClientSockAlive = false;

	while (m_SendBuffer.GetUseSize() >= sizeof(JBuffer*)) {
		JBuffer* sendBuff;
		m_SendBuffer >> sendBuff;
		OnSerialSendBufferFree(sendBuff);
	}
}

bool JClient::ConnectToServer(const CHAR* clanServerIP, USHORT clanserverPort)
{
	if (!m_ClientSockAlive) {

		if (!InitClient(clanServerIP, clanserverPort)) {
			return false;
		}

		m_ClientSockAlive = true;

		m_NetworkThread = (HANDLE)_beginthreadex(NULL, 0, JClient::ClientNetworkThreadFunc, this, 0, NULL);

		OnServerConnected();

		WSABUF wsabuf;
		DWORD recvBytes;
		DWORD flags = 0;
		wsabuf.buf = (CHAR*)m_RecvBuffer.GetEnqueueBufferPtr();
		wsabuf.len = m_RecvBuffer.GetDirectEnqueueSize();
		int retval = WSARecv(m_ClientSock, &wsabuf, 1, &recvBytes, &flags, &m_RecvOverlapped, NULL);
		if (retval == SOCKET_ERROR) {
			if (WSAGetLastError() != ERROR_IO_PENDING) {
				DebugBreak();
				return false;
			}
		}
	}

	return true;
}

bool JClient::DisconnectFromServer()
{
	SetEvent(m_Events[enThreadExit]);

	DeleteClient();
	OnServerLeaved();

	return true;
}

bool JClient::SendPacketToServer(JBuffer* sendPacket)
{
	{
		std::lock_guard<std::mutex> lockGuard(m_SendBufferMtx);
		if (m_SendBuffer.GetFreeSize() < sizeof(UINT_PTR)) {
			DebugBreak();
		}
		else {
			m_SendBuffer.Enqueue((BYTE*)&sendPacket, sizeof(UINT_PTR));
		}
	}

	SendPostToServer();

	return true;
}

void JClient::SendPostToServer()
{
	if (InterlockedExchange(&m_SendFlag, 1) == 0) {
		{
			std::lock_guard<std::mutex> lockGuard(m_SendBufferMtx);

			DWORD numOfMessages = m_SendBuffer.GetUseSize() / sizeof(UINT_PTR);
			WSABUF wsabuffs[CLIENT_WSABUF_ARRAY_DEFAULT_SIZE];

			if (numOfMessages > 0) {
				int sendLimit = min(numOfMessages, CLIENT_WSABUF_ARRAY_DEFAULT_SIZE);
				for (int idx = 0; idx < sendLimit; idx++) {
					JBuffer* msgPtr;
					m_SendBuffer.Peek(sizeof(UINT_PTR) * idx, (BYTE*)&msgPtr, sizeof(UINT_PTR));
					wsabuffs[idx].buf = (CHAR*)msgPtr->GetBeginBufferPtr();
					wsabuffs[idx].len = msgPtr->GetUseSize();
					if (wsabuffs[idx].buf == NULL || wsabuffs[idx].len == 0) {
						DebugBreak();
					}
				}

				memset(&m_SendOverlapped, 0, sizeof(WSAOVERLAPPED) - sizeof(WSAOVERLAPPED::hEvent));
				m_SendOverlapped.Offset = sendLimit;

				if (WSASend(m_ClientSock, wsabuffs, sendLimit, NULL, 0, &m_SendOverlapped, NULL) == SOCKET_ERROR) {
					int errcode = WSAGetLastError();
					if (errcode != WSA_IO_PENDING) {
						// 연결 종료
						DisconnectFromServer();
					}
				}
			}
		}
	}
}

UINT __stdcall JClient::ClientNetworkThreadFunc(void* arg)
{
	JClient* client = (JClient*)arg;
	client->OnClientNetworkThreadStart();

	int retval;
	while (true) {
		retval = WaitForMultipleObjects(m_EventCnt, client->m_Events, FALSE, INFINITE);
		switch (retval) {
		case WAIT_OBJECT_0 + enEvent::enThreadExit:
		{
			return 0;
		}
		break;
		case WAIT_OBJECT_0 + enEvent::enCLanRecv:
		{
			DWORD recvBytes;
			GetOverlappedResult((HANDLE)client->m_ClientSock, (LPOVERLAPPED)&client->m_RecvOverlapped, &recvBytes, FALSE);
			if (recvBytes == 0) {
				// 상대측 연결 종료
				client->DeleteClient();
				client->OnServerLeaved();
				break;
			}
			else {
				// 수신 완료
				client->m_RecvBuffer.DirectMoveEnqueueOffset(recvBytes);
				client->OnRecvFromServer(client->m_RecvBuffer);

				// 수신 대기
				memset(&client->m_RecvOverlapped, 0, sizeof(WSAOVERLAPPED) - sizeof(WSAOVERLAPPED::hEvent));
				WSABUF wsabuf;
				DWORD recvBytes;
				DWORD flags = 0;
				wsabuf.buf = (CHAR*)client->m_RecvBuffer.GetEnqueueBufferPtr();
				wsabuf.len = client->m_RecvBuffer.GetDirectEnqueueSize();
				int retval = WSARecv(client->m_ClientSock, &wsabuf, 1, &recvBytes, &flags, &client->m_RecvOverlapped, NULL);
				if (retval == SOCKET_ERROR) {
					if (WSAGetLastError() != ERROR_IO_PENDING) {
						DebugBreak();
						return 1;
					}
				}
			}
		}
		break;
		case WAIT_OBJECT_0 + enEvent::enCLanSend:
		{
			DWORD sendBytes;
			GetOverlappedResult((HANDLE)client->m_ClientSock, (LPOVERLAPPED)&client->m_SendOverlapped, &sendBytes, FALSE);
			if (sendBytes == 0) {
				client->DeleteClient();
				client->OnServerLeaved();
				break;
			}
			else {
				// 송신 완료
				InterlockedExchange(&client->m_SendFlag, 0);
				{
					std::lock_guard<std::mutex> lockGuard(client->m_SendBufferMtx);

					for (int i = 0; i < client->m_SendOverlapped.Offset; i++) {
						JBuffer* sendBuff;
						client->m_SendBuffer >> sendBuff;
						client->OnSerialSendBufferFree(sendBuff);
					}
				}

				if (client->m_SendBuffer.GetUseSize() > 0) {
					client->SendPostToServer();
				}

			}
		}
		break;
		default:
			DebugBreak();
			break;
		}
	}

	return 0;
}
