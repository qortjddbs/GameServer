#include <iostream>
#include <WinSock2.h>
#include <MSWSock.h>
#include <thread>
#include <mutex>
#include <vector>
#include "protocol_2026.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

// IO 작업을 관리할 확장 오버랩드 구조체
enum class IO_OP { RECV, SEND, ACCEPT };
struct IOContext
{
	WSAOVERLAPPED overlapped;
	WSABUF wsabuf;
	char buffer[1024];
	IO_OP opType;

	IOContext(IO_OP op) : opType(op) {
		ZeroMemory(&overlapped, sizeof(overlapped));
		wsabuf.buf = buffer;
		wsabuf.len = sizeof(buffer);
	}
};

// 세션 상태 관리 정의
enum class SessionState { FREE, CONNECTED, INGAME };

// 개별 플레이어를 나타내는 세션 클래스
class Session
{
public:
	std::mutex sessionLock;
	SessionState state = SessionState::FREE;
	int id = -1;
	SOCKET socket = INVALID_SOCKET;

	short x = 0, y = 0;
	int hp = 100, max_hp = 100;
	unsigned long long exp = 0;
	unsigned char level = 1;
	char name[MAX_NAME_LEN]{};

	IOContext recvContext{ IO_OP::RECV };
};

// 전체 서버를 총괄하는 메인 엔진 클래스
class IocpServer
{
private:
	HANDLE hIocp = INVALID_HANDLE_VALUE;
	SOCKET listenSocket = INVALID_SOCKET;
	std::vector<std::shared_ptr<Session>> sessions;
	std::vector<std::thread> workers;
};

int main()
{
	IocpServer server;
}