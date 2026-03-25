#include <iostream>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

constexpr short SERVER_PORT = 3000;		// 서버 포트 번호
constexpr int BUFFER_SIZE = 4096;			// 버퍼 크기

char g_recv_buffer[BUFFER_SIZE];
char g_send_buffer[BUFFER_SIZE];
WSABUF g_recv_wsa_buf{ BUFFER_SIZE, g_recv_buffer };
WSABUF g_send_wsa_buf{ BUFFER_SIZE, g_send_buffer };
WSAOVERLAPPED g_recv_overlapped{}, g_send_overlapped{};
SOCKET g_s_socket;		// 서버 소켓으로 send, recv 하는거고

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

void send_to_client(int size) {
	g_send_wsa_buf.len = size;
	memcpy(g_send_buffer, g_recv_buffer, size);
	ZeroMemory(&g_send_overlapped, sizeof(g_send_overlapped));
	DWORD sent_size = 0;
	int result = WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_overlapped, send_callback);
	if (result == SOCKET_ERROR) {
		error_display(L"데이터 전송 실패", WSAGetLastError());
		WSACleanup();
		exit(1);
	}
}

void CALLBACK recv_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
	if (error != 0) {
		std::wcout << L"클라이언트와의 연결이 끊어졌습니다. \n";
		closesocket(g_s_socket);
		return;
	}
	std::cout << "Received from client - SIZE: " << bytes_transferred << " / MESSAGE : " << g_recv_buffer << std::endl;

	send_to_client(bytes_transferred);
}

void do_recv() {
	DWORD recv_flag = 0;
	ZeroMemory(&g_recv_overlapped, sizeof(g_recv_overlapped));
	int result = WSARecv(g_s_socket, &g_recv_wsa_buf, 1, nullptr, &recv_flag, &g_recv_overlapped, recv_callback);
	if (result == SOCKET_ERROR) {
		int err_no = WSAGetLastError();
		if (err_no != WSA_IO_PENDING) {	// WSA_IO_PENDING는 정상적인 상황이므로, 다른 에러가 발생했을 때만 처리
			error_display(L"데이터 수신 실패", WSAGetLastError());
			exit(1);
		}
	}
}

void CALLBACK send_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
	if (error != 0) {
		error_display(L"데이터 전송 실패", WSAGetLastError());
		return;
	}
	std::cout << "Sent to client: SIZE: " << bytes_transferred << std::endl << std::endl;

	do_recv();
}

int main() {
	std::wcout.imbue(std::locale("korean"));		// 이걸 안쓰면 이상한 라틴어로 나옴
	WSADATA wsa_data{};
	WSAStartup(MAKEWORD(2, 2), &wsa_data);			// 윈도우에서 인터넷 소켓 프로그래밍하려면 넣어줘야함 (리눅스는 필요없음)
	SOCKET a_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);	// TCP 소켓 생성
	SOCKADDR_IN server_addr{};
	server_addr.sin_family = AF_INET;				// IPv4
	server_addr.sin_port = htons(SERVER_PORT);				// 포트 번호 변환
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);		// 모든 인터페이스에서 연결 허용
	bind(a_socket, reinterpret_cast<SOCKADDR*>(&server_addr), sizeof(server_addr));	// 소켓에 주소 할당
	listen(a_socket, SOMAXCONN);					// 클라이언트 연결 대기
	INT addr_len = sizeof(server_addr);


	for (;;) {
		std::wcout << L"클라이언트의 연결을 기다리는 중....\n";
		g_s_socket = WSAAccept(a_socket, reinterpret_cast<SOCKADDR*>(&server_addr), &addr_len, nullptr, 0);	// 클라이언트 연결 수락
		std::wcout << L"클라이언트가 연결되었습니다. \n";
		if (g_s_socket == INVALID_SOCKET) {
			error_display(L"Accept 실패", WSAGetLastError());
			exit(1);
		}
		do_recv();
	}
	WSACleanup();
}