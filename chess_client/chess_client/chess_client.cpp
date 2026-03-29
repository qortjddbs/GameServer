#pragma comment(lib, "ws2_32.lib")
#include <iostream>
#include <WS2tcpip.h>
#include <conio.h>

#define UP 72
#define DOWN 80
#define RIGHT 77
#define LEFT 75
#define EXIT 113	// q키

// const char* SERVER_IP = "172.30.1.72";		// 127.0.0.1 -> 자기자신에게 연결 (특수한 번호)
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

void InitBoard(char board[8][8]) {
	for (int i = 0; i < 8; ++i) {
		for (int j = 0; j < 8; ++j) {
			if ((i + j) % 2 == 0) {		// 맞으면 흰색(-), 아니면 검은색(+) 칸
				board[i][j] = 'W';
			}
			else {
				board[i][j] = 'B';
			}
		}
	}
}

void PrintBoard(char board[8][8]) {
	for (int i = 0; i < 8; ++i) {
		for (int j = 0; j < 8; ++j) {
			std::cout << board[i][j];
		}
		std::cout << std::endl;
	}
}

#pragma pack(push, 1)	// 메모리 꽉꽉 눌러담기
struct KeyPacket {
	int key;
};

struct PosPacket {
	int x, y;
};
#pragma pack(pop)		// 원래 설정으로 복귀

int main(void) {
	std::wcout.imbue(std::locale("korean"));		// 이걸 안쓰면 이상한 라틴어로 나옴

	char server_ip[16];
	std::wcout << L"접속할 서버의 IP주소를 입력하세요 : ";
	std::cin >> server_ip;
	std::cin.ignore();		// 입력 버퍼에 남아있는 엔터 지우기

	WSADATA wsa_data{};
	WSAStartup(MAKEWORD(2, 2), &wsa_data);			// 윈도우에서 인터넷 소켓 프로그래밍하려면 넣어줘야함 (리눅스는 필요없음)
	SOCKET s_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);	// TCP 소켓 생성
	SOCKADDR_IN server_addr{};
	server_addr.sin_family = AF_INET;				// IPv4
	server_addr.sin_port = htons(SERVER_PORT);				// 포트 번호 변환
	inet_pton(AF_INET, server_ip, &server_addr.sin_addr);	// IP 주소 변환
	int result = WSAConnect(s_socket, reinterpret_cast<SOCKADDR*>(&server_addr), sizeof(server_addr), nullptr, nullptr, nullptr, nullptr);
	if (result == SOCKET_ERROR) {
		error_display(L"서버 연결 실패", WSAGetLastError());
		WSACleanup();
		return 1;
	}

	char board[8][8];
	InitBoard(board);
	int current_px = 3;
	int current_py = 3;
	board[current_px][current_py] = '*';
	PrintBoard(board);

	for (;;) {
		std::wcout << L"방향키를 입력하세요!";
		int key = _getch();
		if (key == 224) {		// 서버창에 두번 뜨길래 버퍼 비워주기
			key = _getch();
		}
		KeyPacket c_k_packet{ key };
		WSABUF wsa_buf{ sizeof(KeyPacket), reinterpret_cast<char*>(&c_k_packet) };
		DWORD sent_size = 0;
		int result = WSASend(s_socket, &wsa_buf, 1, &sent_size, 0, nullptr, nullptr);
		if (result == SOCKET_ERROR) {
			error_display(L"데이터 전송 실패", WSAGetLastError());
			WSACleanup();
			return 1;
		}

		PosPacket c_p_packet{};
		WSABUF recv_wsa_buf{ sizeof(PosPacket), reinterpret_cast<char*>(&c_p_packet)};
		DWORD recv_size = 0;
		DWORD recv_flag = 0;
		WSARecv(s_socket, &recv_wsa_buf, 1, &recv_size, &recv_flag, nullptr, nullptr);

		if ((current_px + current_py) % 2 == 0) {
			board[current_py][current_px] = 'W';
		}
		else {
			board[current_py][current_px] = 'B';
		}

		current_px = c_p_packet.x;
		current_py = c_p_packet.y;
		board[current_py][current_px] = '*';

		system("cls");
		PrintBoard(board);
	}
	WSACleanup();
}