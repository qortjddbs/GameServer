#include <iostream>
#include <WS2tcpip.h>
#include <unordered_map>
#pragma comment(lib, "ws2_32.lib")

#define UP 72
#define DOWN 80
#define RIGHT 77
#define LEFT 75
#define EXIT 113	// q키

constexpr short SERVER_PORT = 3000;		// 서버 포트 번호
constexpr int BUFFER_SIZE = 4096;			// 버퍼 크기

#pragma pack(push, 1)
struct KeyPacket {
	int key;
};

struct PosPacket {
	int id;
	int x, y;
};
#pragma pack(pop)

class EXP_OVER {
public:
	WSAOVERLAPPED wsa_over;
	WSABUF wsa_buf;
	PosPacket send_packet;

public:
	EXP_OVER(int id, int x, int y) {
		ZeroMemory(&wsa_over, sizeof(wsa_over));
		send_packet.id = id;
		send_packet.x = x;
		send_packet.y = y;
		wsa_buf.buf = reinterpret_cast<char*>(&send_packet);
		wsa_buf.len = sizeof(PosPacket);
	}
};

void error_display(const wchar_t* msg, int err_no);
void CALLBACK recv_callback(DWORD, DWORD, LPWSAOVERLAPPED, DWORD);
void CALLBACK send_callback(DWORD, DWORD, LPWSAOVERLAPPED, DWORD);

class SESSION {
public:
	int id;
	SOCKET socket;
	int px, py;
	WSAOVERLAPPED recv_over;
	WSABUF recv_wsa_buf;
	KeyPacket recv_packet;

	SESSION() : id(0), socket(INVALID_SOCKET), px(3), py(3) {
		std::cout << "Unexpected Constructor Call Error!\n";
		exit(-1);
	}

	SESSION(int id, SOCKET s) : id(id), socket(s), px(3), py(3) {
		recv_wsa_buf.buf = reinterpret_cast<char*>(&recv_packet);
		recv_wsa_buf.len = sizeof(KeyPacket);
	}

	~SESSION() { closesocket(socket); }

	void do_recv() {
		DWORD recv_flag = 0;
		ZeroMemory(&recv_over, sizeof(recv_over));
		recv_over.hEvent = reinterpret_cast<HANDLE>(id);
		int result = WSARecv(socket, &recv_wsa_buf, 1, 0, &recv_flag, &recv_over, recv_callback);
		if (result == SOCKET_ERROR) {
			int err_no = WSAGetLastError();
			if (err_no != WSA_IO_PENDING) {	// WSA_IO_PENDING는 정상적인 상황이므로, 다른 에러가 발생했을 때만 처리
				error_display(L"데이터 수신 실패", WSAGetLastError());
				exit(1);
			}
		}
	}
};

std::unordered_map<int, SESSION> g_clients;

void error_display(const wchar_t* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	std::wcout << msg << L" : " << lpMsgBuf << std::endl;
	while (true); // 디버깅 용
	LocalFree(lpMsgBuf);
}

void CALLBACK send_callback(DWORD, DWORD, LPWSAOVERLAPPED, DWORD);

void CALLBACK recv_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
	int id = reinterpret_cast<int>(overlapped->hEvent);
	if (error != 0) {
		std::wcout << id << L"번 클라이언트와의 연결이 끊어졌습니다. \n";
		g_clients.erase(id);

		for (const auto& pair : g_clients) {
			SOCKET target_socket = pair.second.socket;
			EXP_OVER* ex_over = new EXP_OVER(id, -1, -1);
			WSASend(target_socket, &ex_over->wsa_buf, 1, nullptr, 0, &ex_over->wsa_over, send_callback);
		}
		return;
	}

	int key = g_clients[id].recv_packet.key;

	switch (key) {
	case UP: {
		std::wcout << L"UP키 입력" << std::endl;
		if (g_clients[id].py > 0) g_clients[id].py--;
		break;
		}

	case DOWN: {
		std::wcout << L"DOWN키 입력" << std::endl;
		if (g_clients[id].py < 7) g_clients[id].py++;
		break;
		}

	case RIGHT: {
		std::wcout << L"RIGHT키 입력" << std::endl;
		if (g_clients[id].px < 7) g_clients[id].px++; 
		break;
		}

	case LEFT: {
		std::wcout << L"LEFT키 입력" << std::endl;
		if (g_clients[id].px > 0) g_clients[id].px--;
		break;
		}
	}

	for (auto& pair : g_clients) {
		SOCKET target_socket = pair.second.socket;
		EXP_OVER* ex_over = new EXP_OVER(id, g_clients[id].px, g_clients[id].py);
		std::wcout << L"현재 좌표 : (" << g_clients[id].px << ", " << g_clients[id].py << ")" << std::endl;
		int result = WSASend(target_socket, &ex_over->wsa_buf, 1, nullptr, 0, &ex_over->wsa_over, send_callback);
		if (result == SOCKET_ERROR) {
			error_display(L"데이터 전송 실패", WSAGetLastError());
			WSACleanup();
			exit(1);
		}
	}

	g_clients[id].do_recv();
}

void CALLBACK send_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
	if (error != 0) {
		error_display(L"데이터 전송 실패", WSAGetLastError());
		return;
	}
	EXP_OVER* ex_over = reinterpret_cast<EXP_OVER*>(overlapped);
	delete ex_over;
}

int main() {
	std::wcout.imbue(std::locale("korean"));		// 이걸 안쓰면 이상한 라틴어로 나옴
	WSADATA wsa_data{};
	WSAStartup(MAKEWORD(2, 2), &wsa_data);			// 윈도우에서 인터넷 소켓 프로그래밍하려면 넣어줘야함 (리눅스는 필요없음)
	SOCKET a_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);	// TCP 소켓 생성
	SOCKADDR_IN server_addr{};
	server_addr.sin_family = AF_INET;				// IPv4
	server_addr.sin_port = htons(SERVER_PORT);				// 포트 번호 변환
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind(a_socket, reinterpret_cast<SOCKADDR*>(&server_addr), sizeof(server_addr));	// 소켓에 주소 할당
	listen(a_socket, SOMAXCONN);					// 클라이언트 연결 대기
	
	std::wcout << L"클라이언트의 연결을 기다리는 중....\n";
	int client_num = 0;

	for (;;) {
		SOCKADDR_IN cl_addr{};
		int addr_len = sizeof(cl_addr);
		SOCKET c_socket = WSAAccept(a_socket, reinterpret_cast<SOCKADDR*>(&cl_addr), &addr_len, nullptr, 0);	// 클라이언트 연결 수락
		std::wcout << L"클라이언트가 연결되었습니다. \n";
		if (c_socket == INVALID_SOCKET) {
			error_display(L"Accept 실패", WSAGetLastError());
			continue;
		}

		client_num++;
		std::wcout << client_num << L"번 클라이언트 접속 성공!" << std::endl;

		g_clients.try_emplace(client_num, client_num, c_socket);
		g_clients[client_num].do_recv();

		// 새로 들어온 유저에게 기존 유저들의 위치 전송 (ok)
		for (const auto& pair : g_clients) {
			if (pair.first != client_num) {		// 지금 막 들어온 클라가 아니라면 정보 전송
				EXP_OVER* ex_over = new EXP_OVER(pair.first, pair.second.px, pair.second.py);
				WSASend(c_socket, &ex_over->wsa_buf, 1, nullptr, 0, &ex_over->wsa_over, send_callback);
			}
		}

		// 기존 유저들에게 새로 들어온 유저의 위치 전송 (수정 필요)
		for (const auto& pair : g_clients) {
			EXP_OVER* ex_over = new EXP_OVER(client_num, g_clients[client_num].px, g_clients[client_num].py);
			WSASend(pair.second.socket, &ex_over->wsa_buf, 1, nullptr, 0, &ex_over->wsa_over, send_callback);
		}
	}
	WSACleanup();
}