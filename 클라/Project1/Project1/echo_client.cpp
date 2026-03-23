#include <iostream>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

const char* SERVER_IP = "172.30.1.72";		// 127.0.0.1 -> 자기자신에게 연결 (특수한 번호)
constexpr short SERVER_PORT = 3000;		// 서버 포트 번호
constexpr int BUFFER_SIZE = 4096;			// 버퍼 크기

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

int main() {
	std::wcout.imbue(std::locale("korean"));		// 이걸 안쓰면 이상한 라틴어로 나옴
	WSADATA wsa_data{};
	WSAStartup(MAKEWORD(2, 2), &wsa_data);			// 윈도우에서 인터넷 소켓 프로그래밍하려면 넣어줘야함 (리눅스는 필요없음)
	SOCKET s_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP,nullptr, 0, 0);	// TCP 소켓 생성
	SOCKADDR_IN server_addr{};
	server_addr.sin_family = AF_INET;				// IPv4
	server_addr.sin_port = htons(SERVER_PORT);				// 포트 번호 변환
	inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);	// IP 주소 변환
	int result = WSAConnect(s_socket, reinterpret_cast<SOCKADDR*>(&server_addr), sizeof(server_addr), nullptr, nullptr, nullptr, nullptr);
	if (result == SOCKET_ERROR) {
		error_display(L"서버 연결 실패", WSAGetLastError());
		WSACleanup();
		return 1;
	}
	for (;;) {
		char buffer[BUFFER_SIZE];
		std::cout << "Enter message to send: ";
		std::cin.getline(buffer, BUFFER_SIZE);
		
		WSABUF was_buf{ static_cast<ULONG>(strlen(buffer)) + 1, buffer};
		DWORD sent_size = 0;
		int result = WSASend(s_socket, &was_buf, 1, &sent_size, 0, nullptr, nullptr);
		if (result == SOCKET_ERROR) {
			error_display(L"데이터 전송 실패", WSAGetLastError());
			WSACleanup();
			return 1;
		}
		
		char recv_buffer[BUFFER_SIZE]{};
		WSABUF recv_was_buf{ BUFFER_SIZE, recv_buffer };
		DWORD recv_size = 0;
		DWORD recv_flag = 0;
		WSARecv(s_socket, &recv_was_buf, 1, &recv_size, &recv_flag, nullptr, nullptr);

		std::cout << "Received from server: " << recv_buffer << ", SIZE: " << recv_size << std::endl;
	}
	WSACleanup();
}