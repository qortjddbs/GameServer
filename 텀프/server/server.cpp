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
	char buffer[1024];		// 실제 데이터 들어있는 곳
	IO_OP opType;
	SOCKET acceptSocket;	// 새로 들어올 손님의 소켓을 담아둘 바구니

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
	unsigned int sessionIndex = 0;
	unsigned int playerNum = 0;

	short x = 0, y = 0;
	int hp = 100, max_hp = 100;
	unsigned long long exp = 0;
	unsigned char level = 1;
	char name[MAX_NAME_LEN]{};

	IOContext recvContext{ IO_OP::RECV };
	int prevRemainBytes = 0;

	void Reset() {		// Reset 함수를 실행하는동안 lock_guard가 자동으로 {}를 잠구고 해제해줌.
		std::lock_guard<std::mutex> lock(sessionLock);
		state = SessionState::FREE;
		socket = INVALID_SOCKET;
		prevRemainBytes = 0;
	}

	// void* -> 이 포인터가 가리키는 구조체가 뭔진 모르겠지만, 일단 메모리 주소만 넘긴다는 뜻
	// void*를 안쓰면 패킷 종류만큼 오버로딩해서 생성해야됨.
	void SendPacket(void* packet) {
		// 들어온 void*를 unsigned char*로 변환하여 메모리의 맨 앞 1바이트 읽기
		// -> 아 구조체가 뭔진 몰라도 크기가 xx바이트라고 파악
		unsigned char* p = reinterpret_cast<unsigned char*>(packet);
		IOContext* sendContext = new IOContext(IO_OP::SEND);
		// OS송신 버퍼 (IOContext->buffer)에 정확히 xx바이트만 복사해 OS로 넘김
		memcpy(sendContext->buffer, p, p[0]);		// void* memcpy(여기에, 이걸, 얼마만큼); 메모리 복사
		sendContext->wsabuf.len = p[0];

		DWORD sentBytes = 0;
		WSASend(socket, &sendContext->wsabuf, 1, &sentBytes, 0, &sendContext->overlapped, nullptr);
		// WSASend(누구한테 보낼건지, 보낼 데이터 버퍼, 버퍼 개수, 참조용 변수, 플래그 (보통 0), send가 완료되었을 때 완료 큐에 넣어줄 영수증, nullptr);
		// 내가 지금 xx바이트짜리 버퍼 줄 테니까 백그라운드에서 전송해달라는 뜻 (비동기)
	}

	// 크기 고정 배열의 고질적인 ID 재사용 문제 해결. (세션 번호와 접속 번호를 각각 부여)
	unsigned long long GetUniqueId() {
		return (static_cast<unsigned long long>(playerNum) << 32) | sessionIndex;
	}
};

// 전체 서버를 총괄하는 메인 엔진 클래스
class IocpServer
{
private:
	HANDLE hIocp = INVALID_HANDLE_VALUE;
	SOCKET listenSocket = INVALID_SOCKET;
	std::vector<std::shared_ptr<Session>> sessions;
	std::vector<std::thread> workers;

public:
	IocpServer() {
		sessions.resize(MAX_PLAYERS);
		// reserve와의 차이점 : 크기를 늘리고 안을 채우냐 마냐 (resize는 채움)
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			sessions[i] = std::make_shared<Session>();
			sessions[i]->id = i;
		}
	}

	bool Initialize() {
		// 윈도우 소켓 라이브러리 초기화 (Winsock 버전 2.2 사용하겠다는 뜻)
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
		
		// 커널 내부 중앙 우체국(IOCP 핸들) 생성
		// 첫 인자 : IOCP 핸들로 사용할 핸들 (INVALID_HANDLE_VALUE이면 새로 생성) / 마지막 인자 0 : 컴퓨터 CPU 코어 개수만큼 스레드를 알아서 쾌적하게 굴려라.
		hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);

		// 비동기 전용 문지기(Listen) 소켓 생성
		// AF_INET : IPv4, SOCK_STREAM : TCP, IPPROTO_TCP : TCP 프로토콜, WSA_FLAG_OVERLAPPED : 비동기 소켓
		listenSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

		// 서버의 네트워크 주소 및 포트 설정
		SOCKADDR_IN serverAddr{};
		serverAddr.sin_family = AF_INET;
		serverAddr.sin_port = htons(PORT);			// 프로토콜 헤더에 정의된 3500번 포트
		serverAddr.sin_addr.s_addr = INADDR_ANY;	// 내 컴퓨터에 장착된 모든 IP로 들어오는 접속 허용

		// 소켓에 주소 바인딩 및 리슨 상태 전환
		if (::bind(listenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) return false;
		if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) return false;

		// 문지기 소켓(listenSocket)을 중앙 우체국(hIocp)에 등록
		// 10000는 일종의 식별 태그(Completion Key). 나중에 워커 쓰레드가 잠에서 깰 때 10000번 소켓에서 일이 터졌다! 하고 깨어나는데
		// 그걸 보고 다른 유저가 패킷을 보낸 게 아닌, 새로운 손님이 문을 열고 들어온 거(AcceptEx 완료)라고 구분할 수 있음
		CreateIoCompletionPort((HANDLE)listenSocket, hIocp, 10000, 0);
		return true;
	}

	void Start() {
		RegisterAccept();

		int threadCount = std::thread::hardware_concurrency(); // 컴퓨터 CPU 코어 개수 반환
		for (int i = 0; i < threadCount; ++i) {
			workers.emplace_back(&IocpServer::WorkerLoop, this);
			// push_back은 객체를 넘기고, emplace_back은 객체 생성에 필요한 인자를 넘김
		}
		std::cout << "Server Core Successfully Started on Port" << PORT << std::endl;
	}

	void Join() {
		for (auto& th : workers) th.join();
	}

private:
	void RegisterAccept() {
		// 빈 방(새 소켓) 미리 파두기
		SOCKET clientSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

		// 작업 영수증(확장 Overlapped 구조체) 만들기 - Accept 작업용
		IOContext* acceptCtx = new IOContext(IO_OP::ACCEPT);
		acceptCtx->acceptSocket = clientSocket;

		// OS에게 예약 걸기
		// listenSocket을 통해 손님이 들어오면, 방금 내가 만든 이 빈 방(clientSocket)에 바로 앉혀줘.
		// 그리고 작업이 다 끝나면 이 영수증(acceptCtx)을 IOCP큐에 넣어줘.
		// 중간 파라미터 0 : 접속과 동시에 첫 패킷을 받지 않고 순수하게 연결만 처리하겠다는 뜻
		AcceptEx(listenSocket, clientSocket, acceptCtx->buffer, 0, 
			sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, nullptr, &acceptCtx->overlapped);
	}

	int AllocateSession(SOCKET clientSocket) {
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			// 방을 확인하고 상태를 바꾸는 찰나의 순간을 잠금
			// 1번 쓰레드와 2번 쓰레드가 동시에 방을 확인해서 둘 다 빈 방으로 판단할 수 있기 때문
			std::lock_guard<std::mutex> lock(sessions[i]->sessionLock);
			if (sessions[i]->state == SessionState::FREE) {
				sessions[i]->state = SessionState::CONNECTED;
				sessions[i]->socket = clientSocket;
				return i;
			}
		}
		return -1;
	}

	void WorkerLoop() {
		while (true) {
			DWORD bytesTransferred = 0;
			ULONG_PTR completionKey = 0;
			WSAOVERLAPPED* overlapped = nullptr;

			BOOL result = GetQueuedCompletionStatus(hIocp, &bytesTransferred, &completionKey, &overlapped, INFINITE);
			// hIocp : 우리가 바라볼 우체국(IOCP 핸들)
			// &bytesTransferred : (출력용) 통신이 완료된 데이터의 크기(몇 바이트인지)를 OS가 여기에 적어줌
			// &completionKey : (출력용) 누구의 작업인지 식별할 수 있는 태그 (기존 손님이 보내면 Session ID (0~9999), 새 손님이면 10000)
			// &overlapped : (출력용) 우리가 WSASend/Recv때 넘겨줬던 그 작업 영수증 구조체의 주소를 OS가 다시 돌려줌
			// INFINITE : 완료된 작업이 큐에 나올 때까지 쓰레드를 무한정 수면(Sleep) 상태로 대기시킴

			// 클라이언트가 게임을 강제 종료하거나 랜선이 뽑히면 bytesTransferred가 0으로 오거나
			// result가 FALSE로 오는데, 이 경우 해당 유저(completionKey)의 세션을 리셋(Disconnect) 해줌
			if (!result || bytesTransferred == 0) {
				if (completionKey != 10000 && completionKey != 0) {
					Disconnect(static_cast<int>(completionKey));
				}
				continue;	// 에러가 났으니 아래 로직은 무시하고 다시 GQCS 수면하러 돌아감
			}

			// OS는 WSAOVERLAPPED* 껍데기만 돌려주기 때문에, 우리가 원래 정의했던 확장 구조체인
			// IOContext*로 다시 캐스팅하여 이 작업이 ACCEPT인지, RECV인지, SEND인지 파악
			IOContext* ioCtx = reinterpret_cast<IOContext*>(overlapped);
			int sessionId = static_cast<int>(completionKey);

			switch (ioCtx->opType) {
			case IO_OP::ACCEPT: {
				// AcceptEx로 들어온 소켓을 꺼냄
				SOCKET newClientSocket = ioCtx->acceptSocket;
				
				// AllocateSession()을 불러 빈 방 번호(ID)를 받음
				int sessionId = AllocateSession(newClientSocket);

				if (sessionId != -1) {		// 빈 방이 성공적으로 배정되었다면
					// 그 빈 방(소켓)을 hIocp에 등록 (Key로 방 번호인 sessionId를 줌)
					CreateIoCompletionPort((HANDLE)newClientSocket, hIocp, sessionId, 0);

					// 그 손님에게 WSARecv를 걸어 첫 패킷 수신을 예약
					auto session = sessions[sessionId];
					DWORD flags = 0;
					WSARecv(newClientSocket, &session->recvContext.wsabuf, 1, nullptr, &flags, 
						&session->recvContext.overlapped, nullptr);

					std::cout << "[Accept] New player connected. Assigned Session ID: " << sessionId << std::endl;
				}
				else {
					closesocket(newClientSocket);	// 방이 꽉 찼으면 소켓을 닫고 돌려보냄
				}

				RegisterAccept();	// 다음 손님을 위해 accept 예약
				delete ioCtx;		// 다 쓴 영수증 폐기
				break;
			}
			case IO_OP::RECV: {
				ProcessReceive(sessionId, bytesTransferred);
				break;
			}
			case IO_OP::SEND: {
				delete ioCtx;
				break;
			}
			}
		}
	}

	void ProcessReceive(int sessionId, DWORD bytesTransferred) {
		// 패킷 재조립 로직
		auto session = sessions[sessionId];
		int totalBytes = session->prevRemainBytes + bytesTransferred;
		int readPos = 0;

		while (true) {
			if (totalBytes - readPos < 1) break;

			unsigned char packetSize = session->recvContext.buffer[readPos];		
			// 새롭게 들어온 데이터의 맨 앞 1바이트는 패킷의 크기 정보이므로, 그걸 읽어서 packetSize에 저장
			if (totalBytes - readPos < packetSize) break;
			// 더 읽어와야 되니까 일단 보류. 패킷이 완성되지 않았으니 지금은 처리하지 말고 다음에 더 읽어서 완성되면 처리

			OnPacket(sessionId, &session->recvContext.buffer[readPos]);
			readPos += packetSize;
			// 패킷 처리했으니까 그만큼 인덱스 뒤로 이동
		}

		// 더 받아올 데이터가 남았으면
		int remainBytes = totalBytes - readPos;
		if (remainBytes > 0) {
			memmove(session->recvContext.buffer, &session->recvContext.buffer[readPos], remainBytes);
			// memmove(목적지, 출발지, 크기)
		}
		
		session->prevRemainBytes = remainBytes;
		DWORD flags = 0;
		session->recvContext.wsabuf.len = sizeof(session->recvContext.buffer) - remainBytes;
		session->recvContext.wsabuf.buf = session->recvContext.buffer + remainBytes;
		WSARecv(session->socket, &session->recvContext.wsabuf, 1, nullptr, &flags, 
			&session->recvContext.overlapped, nullptr);
	}

	void OnPacket(int sessionId, char* packet) {			// 온전한 패킷 1개가 완성되면 이 함수로 던져줌
		PACKET_TYPE type = reinterpret_cast<C2S_Login*>(packet)->type;
		// 클라이언트가 어떤 패킷을 보냈든, 구조체의 메모리 맨 앞에는 무조건 크기(size),
		// 두 번째에는 무조건 타입(type)이 들어있도록 설계했기 때문에,  아무 패킷 구조체(여기서는 제일 만만한 C2S_Login)
		// 로 포인터를 강제 형변환한 뒤 ->type 을 읽으면, 이 패킷이 어떤 패킷인지 알 수 있음

		if (type == C2S_LOGIN) {
			C2S_Login* loginPacket = reinterpret_cast<C2S_Login*>(packet);
			auto session = sessions[sessionId];

			strcpy_s(session->name, loginPacket->username);
			session->x = rand() % 200;	// 초기 테스트용 가벼운 좌표
			session->y = rand() % 200;
			session->state = SessionState::INGAME;

			// 1. 결과 패킷 발송
			S2C_LoginResult res;
			res.size = sizeof(res);
			res.type = S2C_LOGIN_RESULT;
			res.success = true;
			strcpy_s(res.message, "Welcome!");
			session->SendPacket(&res);

			// 2. 아바타 정보 발송
			S2C_AvatarInfo info;
			info.size = sizeof(info);
			info.type = S2C_AVATAR_INFO;
			info.playerId = sessionId;
			info.x = session->x;
			info.y = session->y;
			info.hp = session->hp;
			info.max_hp = session->max_hp;
			info.level = session->level;
			info.exp = session->exp;
			session->SendPacket(&info);

			std::cout << "[Login] Player " << session->name << " entered at (" << session->x << ", " << session->y << ")" << std::endl;
		}
	}

	void Disconnect(int id) {
		sessions[id]->Reset();
	}
};

int main()
{
	IocpServer server;
	if (server.Initialize()) {
		server.Start();
		server.Join();
	}
}