#include "CLanClient.h"

bool CLanClient::InitLanClient(const CHAR* clanServerIP, USHORT clanserverPort)
{
	m_CLanClientSock = CreateWindowSocket_IPv4(true);
	SOCKADDR_IN serverAddr = CreateDestinationADDR(clanServerIP, clanserverPort);
	if (!ConnectSocketTry(m_CLanClientSock, serverAddr)) {
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

void CLanClient::DeleteLanClient()
{
	closesocket(m_CLanClientSock);
	m_ClientSockAlive = false;

	while (m_SendBufferToCLanServer.GetUseSize() >= sizeof(JBuffer*)) {
		JBuffer* sendBuff;
		m_SendBufferToCLanServer >> sendBuff;
		m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendBuff);
	}
}

bool CLanClient::ConnectLanServer(const CHAR* clanServerIP, USHORT clanserverPort)
{
	if (m_ClientSockAlive) {
		return false;
	}

	if (!InitLanClient(clanServerIP, clanserverPort)) {
		return false;
	}

	m_ClientSockAlive = true;

	m_CLanNetworkThread = (HANDLE)_beginthreadex(NULL, 0, CLanClient::CLanNetworkFunc, this, 0, NULL);

	OnEnterJoinServer();

	WSABUF wsabuf;
	DWORD recvBytes;
	DWORD flags = 0;
	wsabuf.buf = (CHAR*)m_RecvBufferFromCLanServer.GetEnqueueBufferPtr();
	wsabuf.len = m_RecvBufferFromCLanServer.GetDirectEnqueueSize();
	int retval = WSARecv(m_CLanClientSock, &wsabuf, 1, &recvBytes, &flags, &m_RecvOverlapped, NULL);
	if (retval == SOCKET_ERROR) {
		if (WSAGetLastError() != ERROR_IO_PENDING) {
			DebugBreak();
			return false;
		}
	}

	return true;
}

bool CLanClient::DisconnectLanServer()
{
	SetEvent(m_Events[enThreadExit]);

	DeleteLanClient();
	OnLeaveServer();

	return true;
}

bool CLanClient::SendPacketToCLanServer(JBuffer* sendPacket, bool encoded)
{
	if (!encoded) {
		// 인코딩 수행
		if (sendPacket->GetUseSize() < sizeof(stMSG_HDR)) {
			DebugBreak();
			return false;
		}
		else {
			UINT offset = 0;
			stMSG_HDR* hdr;// = (stMSG_HDR*)sendDataPtr->GetDequeueBufferPtr();
			while (offset < sendPacket->GetUseSize()) {
				hdr = (stMSG_HDR*)sendPacket->GetBufferPtr(offset);
				offset += sizeof(stMSG_HDR);
				if (hdr->randKey == (BYTE)-1) {
					hdr->randKey = GetRandomKey();
					Encode(hdr->randKey, hdr->len, hdr->checkSum, sendPacket->GetBufferPtr(offset));
				}
				offset += hdr->len;
			}
		}
	}

	{
		std::lock_guard<std::mutex> lockGuard(m_SendBufferMtx);
		if (m_SendBufferToCLanServer.GetFreeSize() < sizeof(UINT_PTR)) {
			DebugBreak();
		}
		else {
			m_SendBufferToCLanServer.Enqueue((BYTE*)&sendPacket, sizeof(UINT_PTR));
		}
	}

	SendPostToCLanServer();

	return true;
}

void CLanClient::SendPostToCLanServer()
{
	if (InterlockedExchange(&m_SendFlag, 1) == 0) {
		{
			std::lock_guard<std::mutex> lockGuard(m_SendBufferMtx);

			DWORD numOfMessages = m_SendBufferToCLanServer.GetUseSize() / sizeof(UINT_PTR);
			WSABUF wsabuffs[WSABUF_ARRAY_DEFAULT_SIZE];

			if (numOfMessages > 0) {
				int sendLimit = min(numOfMessages, WSABUF_ARRAY_DEFAULT_SIZE);
				for (int idx = 0; idx < sendLimit; idx++) {
					JBuffer* msgPtr;
					m_SendBufferToCLanServer.Peek(sizeof(UINT_PTR) * idx, (BYTE*)&msgPtr, sizeof(UINT_PTR));
					wsabuffs[idx].buf = (CHAR*)msgPtr->GetBeginBufferPtr();
					wsabuffs[idx].len = msgPtr->GetUseSize();
					if (wsabuffs[idx].buf == NULL || wsabuffs[idx].len == 0) {
						DebugBreak();
					}
				}
				
				memset(&m_SendOverlapped, 0, sizeof(WSAOVERLAPPED) - sizeof(WSAOVERLAPPED::hEvent));
				m_SendOverlapped.Offset = sendLimit;

				if (WSASend(m_CLanClientSock, wsabuffs, sendLimit, NULL, 0, &m_SendOverlapped, NULL) == SOCKET_ERROR) {
					int errcode = WSAGetLastError();
					if (errcode != WSA_IO_PENDING) {
						// 연결 종료
						DisconnectLanServer();
					}
				}
			}
		}
	}
}

UINT __stdcall CLanClient::CLanNetworkFunc(void* arg)
{
	CLanClient* clanclient = (CLanClient*)arg;
	clanclient->m_SerialBuffPoolMgr.AllocTlsMemPool();

	int retval;
	while (true) {
		retval = WaitForMultipleObjects(m_EventCnt, clanclient->m_Events, FALSE, INFINITE);
		switch (retval) {
		case WAIT_OBJECT_0 + enEvent::enThreadExit:
		{
			return 0;
		}
			break;
		case WAIT_OBJECT_0 + enEvent::enCLanRecv:
		{
			DWORD recvBytes;
			GetOverlappedResult((HANDLE)clanclient->m_CLanClientSock, (LPOVERLAPPED)&clanclient->m_RecvOverlapped, &recvBytes, FALSE);
			if (recvBytes == 0) {
				// 상대측 연결 종료
				clanclient->DeleteLanClient();
				clanclient->OnLeaveServer();
				break;
			}
			else {
				// 수신 완료
				clanclient->m_RecvBufferFromCLanServer.DirectMoveEnqueueOffset(recvBytes);
				while (clanclient->m_RecvBufferFromCLanServer.GetUseSize() >= sizeof(stMSG_HDR)) {
					stMSG_HDR hdr;
					clanclient->m_RecvBufferFromCLanServer.Peek(&hdr);
					if (hdr.code != clanclient->m_ProtocolCode) {
						DebugBreak();
						return false;
					}
					if (clanclient->m_RecvBufferFromCLanServer.GetUseSize() < sizeof(stMSG_HDR) + hdr.len) {
						break;
					}
					clanclient->m_RecvBufferFromCLanServer.DirectMoveDequeueOffset(sizeof(stMSG_HDR));

					if (!clanclient->Decode(hdr.randKey, hdr.len, hdr.checkSum, clanclient->m_RecvBufferFromCLanServer)) {
						DebugBreak();
						return false;
					}

					JSerBuffer recvBuff(clanclient->m_RecvBufferFromCLanServer, hdr.len, true);
					clanclient->OnRecvFromCLanServer(recvBuff);
					clanclient->m_RecvBufferFromCLanServer.DirectMoveDequeueOffset(hdr.len);
				}

				// 수신 대기
				memset(&clanclient->m_RecvOverlapped, 0, sizeof(WSAOVERLAPPED) - sizeof(WSAOVERLAPPED::hEvent));
				WSABUF wsabuf;
				DWORD recvBytes;
				DWORD flags = 0;
				wsabuf.buf = (CHAR*)clanclient->m_RecvBufferFromCLanServer.GetEnqueueBufferPtr();
				wsabuf.len = clanclient->m_RecvBufferFromCLanServer.GetDirectEnqueueSize();
				int retval = WSARecv(clanclient->m_CLanClientSock, &wsabuf, 1, &recvBytes, &flags, &clanclient->m_RecvOverlapped, NULL);
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
			GetOverlappedResult((HANDLE)clanclient->m_CLanClientSock, (LPOVERLAPPED)&clanclient->m_SendOverlapped, &sendBytes, FALSE);
			if (sendBytes == 0) {
				clanclient->DeleteLanClient();
				clanclient->OnLeaveServer();
				break;
			}
			else {
				// 송신 완료
				InterlockedExchange(&clanclient->m_SendFlag, 0);
				{
					std::lock_guard<std::mutex> lockGuard(clanclient->m_SendBufferMtx);

					for (int i = 0; i < clanclient->m_SendOverlapped.Offset; i++) {
						JBuffer* sendBuff;
						clanclient->m_SendBufferToCLanServer >> sendBuff;
						clanclient->m_SerialBuffPoolMgr.GetTlsMemPool().FreeMem(sendBuff);
					}
				}

				if (clanclient->m_SendBufferToCLanServer.GetUseSize() > 0) {
					clanclient->SendPostToCLanServer();
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
