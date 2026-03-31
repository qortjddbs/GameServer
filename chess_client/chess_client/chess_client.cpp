#pragma comment(lib, "ws2_32.lib")
#include <iostream>
#include <WS2tcpip.h>
#include <conio.h>
#include <unordered_map>

#define UP 72
#define DOWN 80
#define RIGHT 77
#define LEFT 75
#define EXIT 113	// q키

// const char* SERVER_IP = "172.30.1.87";		// 127.0.0.1 -> 자기자신에게 연결 (특수한 번호)
constexpr short SERVER_PORT = 3000;		// 서버 포트 번호

char board[8][8];
int current_px = 3;
int current_py = 3;

#pragma pack(push, 1)	// 메모리 꽉꽉 눌러담기
struct KeyPacket {
	int key;
};

struct PosPacket {
	int id;
	int x, y;
};
#pragma pack(pop)		// 원래 설정으로 복귀
struct PlayerInfo {
	int x, y;
};
std::unordered_map<int, PlayerInfo> g_players;		// 플레이어 ID와 위치를 저장하는 맵

PosPacket g_c_pos_packet{};
KeyPacket g_c_key_packet{};
WSABUF g_recv_wsa_buf{ sizeof(PosPacket), reinterpret_cast<char*>(&g_c_pos_packet)};
WSABUF g_send_wsa_buf{ sizeof(KeyPacket), reinterpret_cast<char*>(&g_c_key_packet)};
WSAOVERLAPPED g_recv_overlapped{}, g_send_overlapped{};
SOCKET g_s_socket;

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

void CALLBACK send_callback(DWORD, DWORD, LPWSAOVERLAPPED, DWORD);

void send_to_server() {
	std::wcout << L"방향키를 입력하세요!";
	if (_kbhit()) {
		int key = _getch();
		if (key == 224) {
			key = _getch();
		}
		g_c_key_packet = { key };
		g_send_wsa_buf = { sizeof(KeyPacket), reinterpret_cast<char*>(&g_c_key_packet) };
		DWORD sent_size = 0;
		int result = WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_overlapped, send_callback);
		if (result == SOCKET_ERROR) {
			error_display(L"데이터 전송 실패", WSAGetLastError());
			WSACleanup();
			exit(1);
		}
	}
}

void CALLBACK recv_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
	if (error != 0) {
		error_display(L"데이터 수신 실패", WSAGetLastError());
		exit(1);
	}
	int id = g_c_pos_packet.id;
	g_players[id].x = g_c_pos_packet.x;
	g_players[id].y = g_c_pos_packet.y;

	////if ((current_px + current_py) % 2 == 0) {
	////	board[current_py][current_px] = 'W';
	////}
	////else {
	////	board[current_py][current_px] = 'B';
	////}

	////current_px = g_c_pos_packet.x;
	////current_py = g_c_pos_packet.y;
	////board[current_py][current_px] = '*';

	system("cls");
	//std::cout << "서버로부터 위치 패킷 수신! (x: " << g_c_pos_packet.x << ", y: " << g_c_pos_packet.y << ")" << std::endl;
	//std::cout << "(" << current_px << ", " << current_py << ")" << std::endl;
	char board[8][8];
	InitBoard(board);

	for (const auto& pair : g_players) {
		int id = pair.first;
		int px = pair.second.x;
		int py = pair.second.y;

		char avatar = (id >= 1 && id <= 9) ? ('0' + id) : '*';		// ID가 1~9면 숫자로, 아니면 *로 표시
		board[py][px] = avatar;
	}
	PrintBoard(board);

	DWORD recv_flag = 0;
	ZeroMemory(&g_recv_overlapped, sizeof(g_recv_overlapped));
	WSARecv(g_s_socket, &g_recv_wsa_buf, 1, nullptr, &recv_flag, &g_recv_overlapped, recv_callback);
}

void CALLBACK send_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
	if (error != 0) {
		error_display(L"데이터 전송 실패", WSAGetLastError());
		return;
	}

}

void InputKey() {
	if (_kbhit()) {
		int key = _getch();
		if (key == 224) {
			key = _getch();
		}
		g_c_key_packet = { key };
		g_send_wsa_buf = { sizeof(KeyPacket), reinterpret_cast<char*>(&g_c_key_packet) };

		ZeroMemory(&g_send_overlapped, sizeof(g_send_overlapped));
		DWORD sent_size = 0;
		int result = WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_overlapped, send_callback);
		if (result == SOCKET_ERROR) {
			error_display(L"데이터 전송 실패", WSAGetLastError());
			WSACleanup();
			exit(1);
		}
	}
}

int main(void) {
	std::wcout.imbue(std::locale("korean"));		// 이걸 안쓰면 이상한 라틴어로 나옴

	char server_ip[16];
	std::wcout << L"접속할 서버의 IP주소를 입력하세요 : ";
	std::cin >> server_ip;
	std::cin.ignore();		// 입력 버퍼에 남아있는 엔터 지우기

	WSADATA wsa_data{};
	WSAStartup(MAKEWORD(2, 2), &wsa_data);			// 윈도우에서 인터넷 소켓 프로그래밍하려면 넣어줘야함 (리눅스는 필요없음)
	g_s_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);	// TCP 소켓 생성
	SOCKADDR_IN server_addr{};
	server_addr.sin_family = AF_INET;				// IPv4
	server_addr.sin_port = htons(SERVER_PORT);				// 포트 번호 변환
	inet_pton(AF_INET, server_ip, &server_addr.sin_addr);	// IP 주소 변환
	int result = WSAConnect(g_s_socket, reinterpret_cast<SOCKADDR*>(&server_addr), sizeof(server_addr), nullptr, nullptr, nullptr, nullptr);
	if (result == SOCKET_ERROR) {
		error_display(L"서버 연결 실패", WSAGetLastError());
		WSACleanup();
		return 1;
	}

	DWORD recv_flag = 0;
	ZeroMemory(&g_recv_overlapped, sizeof(g_recv_overlapped));
	WSARecv(g_s_socket, &g_recv_wsa_buf, 1, nullptr, &recv_flag, &g_recv_overlapped, recv_callback);

	for (;;) {
		InputKey();

		//system("cls");
		//PrintBoard(board);

		SleepEx(10, TRUE);		// 10ms 동안 alertable 상태로 대기 (이때 콜백 함수가 실행될 수 있음)
	}
	WSACleanup();
}