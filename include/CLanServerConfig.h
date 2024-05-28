#pragma once

////////////////////////////////////////////////////
// 송수신 버퍼 기본 크기
////////////////////////////////////////////////////
#define SESSION_SEND_BUFFER_DEFAULT_SIZE	20000
#define SESSION_RECV_BUFFER_DEFAULT_SIZE	20000

////////////////////////////////////////////////////
// 송신 버퍼 동기화 분석
////////////////////////////////////////////////////
// 1. std::mutex 사용
#define SESSION_SENDBUFF_SYNC_TEST
#if defined(SESSION_SENDBUFF_SYNC_TEST)
#include <mutex>
#endif
// 2. (to do) 락-프리 큐

////////////////////////////////////////////////////
// 송신 버퍼 데이터 전달 방식
////////////////////////////////////////////////////
// 1. SendPacket, 송신 버퍼에 전송 데이터 copy 
//#define SEND_RECV_RING_BUFF_COPY_MODE

// 2. SendPacket, 송신 직렬화 버퍼 포인터 전달
#define SEND_RECV_RING_BUFF_SERIALIZATION_MODE
#if defined(SEND_RECV_RING_BUFF_SERIALIZATION_MODE)
#define WSABUF_ARRAY_DEFAULT_SIZE			1000
#endif

////////////////////////////////////////////////////
// 메모리 풀
////////////////////////////////////////////////////
#define ALLOC_BY_TLS_MEM_POOL
#if defined(ALLOC_BY_TLS_MEM_POOL)
#define TLS_MEM_POOL_DEFAULT_UNIT_CNT		100
#define TLS_MEM_POOL_DEFAULT_CAPACITY		100
#define TLS_MEM_POOL_DEFAULT_SURPLUS_SIZE	100
#endif


////////////////////////////////////////////////////
// 로그(log)
////////////////////////////////////////////////////
#define SESSION_LOG		// 세션 삭제(및 생성) 추적 로그
#define SENDBUFF_MONT_LOG		// 송신 버퍼 모니터링용 로그


#define dfPACKET_CODE		0x77
#define dfPACKET_KEY		0x32

#define TRACKING_CLIENT_PORT

////////////////////////////////////////////////////
// TPS(Transaction Per Second)
////////////////////////////////////////////////////
#define CALCULATE_TRANSACTION_PER_SECOND
#if defined(CALCULATE_TRANSACTION_PER_SECOND)
#define		ACCEPT_TRANSACTION		0
#define		RECV_TRANSACTION		1
#define		SEND_TRANSACTION		2
#define		SEND_REQ_TRANSACTION	3
#define		NUM_OF_TPS_ITEM		4
#endif