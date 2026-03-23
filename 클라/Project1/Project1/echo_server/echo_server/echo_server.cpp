#include <iostream>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

constexpr short SERVER_PORT = 3000;		// 서버 포트 번호
constexpr int BUFFER_SIZE = 4096;			// 버퍼 크기

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
	for (;;) {
		char recv_buffer[BUFFER_SIZE]{};
		WSABUF recv_was_buf{ BUFFER_SIZE, recv_buffer };
		DWORD recv_size = 0;
		DWORD recv_flag = 0;
		WSARecv(c_socket, &recv_was_buf, 1, &recv_size, &recv_flag, nullptr, nullptr);

		std::cout << "Received from client: " << recv_buffer << ", SIZE: " << recv_size << std::endl;
		
		DWORD sent_size = 0;
		WSABUF send_was_buf = { recv_size, recv_buffer };
		WSASend(c_socket, &send_was_buf, 1, &sent_size, 0, nullptr, nullptr);

		std::cout << recv_size << "Sent to client: " << std::endl;
	}
	WSACleanup();
}