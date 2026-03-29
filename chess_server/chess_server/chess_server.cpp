#include <iostream>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#define UP 72
#define DOWN 80
#define RIGHT 77
#define LEFT 75
#define EXIT 113	// q키

constexpr short SERVER_PORT = 3000;		// 서버 포트 번호

#pragma pack(push, 1)
struct KeyPacket {
	int key;
};

struct PosPacket {
	int x, y;
};
#pragma pack(pop)

int main() {
	std::wcout.imbue(std::locale("korean"));		// 이걸 안쓰면 이상한 라틴어로 나옴
	WSADATA wsa_data{};
	WSAStartup(MAKEWORD(2, 2), &wsa_data);			// 윈도우에서 인터넷 소켓 프로그래밍하려면 넣어줘야함 (리눅스는 필요없음)
	SOCKET s_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);	// TCP 소켓 생성
	SOCKADDR_IN server_addr{};
	server_addr.sin_family = AF_INET;				// IPv4
	server_addr.sin_port = htons(SERVER_PORT);				// 포트 번호 변환
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind(s_socket, reinterpret_cast<SOCKADDR*>(&server_addr), sizeof(server_addr));	// 소켓에 주소 할당
	listen(s_socket, SOMAXCONN);					// 클라이언트 연결 대기
	INT addr_len = sizeof(server_addr);
	SOCKET c_socket = WSAAccept(s_socket, reinterpret_cast<SOCKADDR*>(&server_addr), &addr_len, nullptr, 0);	// 클라이언트 연결 수락
	
	int px = 3;
	int py = 3;
	
	for (;;) {
		KeyPacket s_k_packet;
		WSABUF recv_was_buf{ sizeof(KeyPacket), reinterpret_cast<char*>(&s_k_packet)};
		DWORD recv_size = 0;
		DWORD recv_flag = 0;
		WSARecv(c_socket, &recv_was_buf, 1, &recv_size, &recv_flag, nullptr, nullptr);

		switch (s_k_packet.key) {
		case UP: {
			std::wcout << L"UP키 입력" << std::endl;
			if (py > 0) py--; 
			break;
		}
			
		case DOWN: {
			std::wcout << L"DOWN키 입력" << std::endl;
			if (py < 7) py++; 
			break;
		}
			
		case RIGHT: {
			std::wcout << L"RIGHT키 입력" << std::endl;
			if (px < 7) px++; break;
		}
			
		case LEFT: {
			std::wcout << L"LEFT키 입력" << std::endl;
			if (px > 0) px--; 
			break;
			}
		}

		std::wcout << L"현재 좌표 : (" << px << ", " << py << ")" << std::endl;
		PosPacket s_p_packet{ px, py };
		WSABUF send_was_buf = { sizeof(s_p_packet), reinterpret_cast<char*>(&s_p_packet)};
		DWORD sent_size = 0;
		WSASend(c_socket, &send_was_buf, 1, &sent_size, 0, nullptr, nullptr);
	}
	WSACleanup();
}