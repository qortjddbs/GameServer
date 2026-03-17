#include <iostream>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

const char* SERVER_IP = "127.0.0.1";		// 자기자신에게 연결 (특수한 번호)
constexpr short SERVER_PORT = 54000;		// 서버 포트 번호
constexpr int BUFFER_SIZE = 4096;			// 버퍼 크기

int main() {
	std::wcout.imbue(std::locale("korean"));		// 이걸 안쓰면 이상한 라틴어로 나옴
	WSAStartup(MAKEWORD(2, 2), nullptr);			// 윈도우에서 인터넷 소켓 프로그래밍하려면 넣어줘야함 (리눅스는 필요없음)
	SOCKET s_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP,nullptr, 0, 0);	// TCP 소켓 생성
	SOCKADDR_IN server_addr{};
	server_addr.sin_family = AF_INET;				// IPv4
	server_addr.sin_port = htons(SERVER_PORT);				// 포트 번호 변환
	inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);	// IP 주소 변환
	WSAConnect(s_socket, reinterpret_cast<SOCKADDR*>(&server_addr), sizeof(server_addr), nullptr, nullptr, nullptr, nullptr);
	for (;;) {
		char buffer[BUFFER_SIZE];
		std::cout << "Enter message to send: ";
		std::cin.getline(buffer, BUFFER_SIZE);
		
		WSABUF was_buf{ strlen(buffer) + 1, buffer};
		DWORD sent_size = 0;
		WSASend(s_socket, &was_buf, 1, &sent_size, 0, nullptr, nullptr);
		
		char recv_buffer[BUFFER_SIZE]{};
		WSABUF recv_was_buf{ BUFFER_SIZE, recv_buffer };
		DWORD recv_size = 0;
		DWORD recv_flag = 0;
		WSARecv(s_socket, &recv_was_buf, 1, &recv_size, &recv_flag, nullptr, nullptr);

		std::cout << "Received from server: " << recv_buffer << ", SIZE: " << recv_size << std::endl;
	}
}