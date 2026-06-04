#define NOMINMAX // Windows 매크로 충돌 방지
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <WinSock2.h>
#include <winsock.h>
#include <Windows.h>
#include <iostream>
#include <thread>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <chrono>
#include <queue>
#include <array>
#include <memory>
#include <string>
#include <algorithm>

using namespace std;
using namespace chrono;

extern HWND		hWnd;

// 🚨 목표 테스트 인원 설정 (5,000명)
const static int MAX_TEST = 5000;
const static int MAX_CLIENTS = MAX_TEST * 2;
const static int INVALID_ID = -1;
const static int MAX_PACKET_SIZE = 255;
const static int MAX_BUFF_SIZE = 255;

#pragma comment (lib, "ws2_32.lib")

#include "..\..\텀프\server\protocol_2026.h"

HANDLE g_hiocp;

enum OPTYPE { OP_SEND, OP_RECV, OP_DO_MOVE };

high_resolution_clock::time_point last_connect_time;

string g_server_ip = "127.0.0.1";

#pragma pack(push, 1)
struct PACKET_HEADER {
	unsigned char size;
	PACKET_TYPE type;
};
#pragma pack(pop)

struct OverlappedEx {
	WSAOVERLAPPED over;
	WSABUF wsabuf;
	unsigned char IOCP_buf[MAX_BUFF_SIZE];
	OPTYPE event_type;
	int event_target;
};

struct CLIENT {
	int id;
	short x;
	short y;
	atomic_bool connected;

	SOCKET client_socket;
	OverlappedEx recv_over;
	unsigned char packet_buf[MAX_PACKET_SIZE];
	int prev_packet_data;
	int curr_packet_size;
	high_resolution_clock::time_point last_move_time;
};

array<int, MAX_CLIENTS> client_map;
array<CLIENT, MAX_CLIENTS> g_clients;
atomic_int num_connections;
atomic_int client_to_close;
atomic_int active_clients;

int global_delay = 0; // ms단위, 서버 응답 지연율 감시

vector<thread*> worker_threads;
thread test_thread;

float point_cloud[MAX_TEST * 2];

void error_display(const char* msg, int err_no) {
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	std::cout << msg << err_no << std::endl;
	LocalFree(lpMsgBuf);
}

void DisconnectClient(int ci) {
	bool status = true;
	if (true == atomic_compare_exchange_strong(&g_clients[ci].connected, &status, false)) {
		closesocket(g_clients[ci].client_socket);
		active_clients--;
	}
}

void SendPacket(int cl, void* packet) {
	int psize = reinterpret_cast<unsigned char*>(packet)[0];
	OverlappedEx* over = new OverlappedEx;
	over->event_type = OP_SEND;
	memcpy(over->IOCP_buf, packet, psize);
	ZeroMemory(&over->over, sizeof(over->over));
	over->wsabuf.buf = reinterpret_cast<CHAR*>(over->IOCP_buf);
	over->wsabuf.len = psize;

	int ret = WSASend(g_clients[cl].client_socket, &over->wsabuf, 1, NULL, 0, &over->over, NULL);
	if (0 != ret) {
		int err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no)
			error_display("Error in SendPacket: ", err_no);
	}
}

void ProcessPacket(int ci, unsigned char packet[]) {
	PACKET_TYPE type = reinterpret_cast<PACKET_HEADER*>(packet)->type;

	switch (type) {
	case S2C_MOVE_OBJECT: {
		S2C_MoveObject* move_packet = reinterpret_cast<S2C_MoveObject*>(packet);
		if (move_packet->object_id < MAX_CLIENTS) {
			int my_id = client_map[move_packet->object_id];
			if (-1 != my_id) {
				g_clients[my_id].x = move_packet->x;
				g_clients[my_id].y = move_packet->y;
			}
			// 🚨 지연 시간(Lag) 측정
			if (ci == my_id && move_packet->move_time != 0) {
				auto d_ms = duration_cast<milliseconds>(high_resolution_clock::now().time_since_epoch()).count() - move_packet->move_time;
				if (global_delay < d_ms) global_delay++;
				else if (global_delay > d_ms) global_delay--;
			}
		}
		break;
	}
	case S2C_AVATAR_INFO: {
		S2C_AvatarInfo* login_packet = reinterpret_cast<S2C_AvatarInfo*>(packet);
		g_clients[ci].connected = true;
		active_clients++;

		int my_id = ci;
		client_map[login_packet->playerId] = my_id;
		g_clients[my_id].id = login_packet->playerId;
		g_clients[my_id].x = login_packet->x;
		g_clients[my_id].y = login_packet->y;
		break;
	}
						// 🚨 스트레스 테스트에서는 그림을 그리지 않으므로 아래 패킷들은 랙 계산 외엔 무시합니다!
	case S2C_ADD_OBJECT:
	case S2C_REMOVE_OBJECT:
	case S2C_CHAT_MESSAGE:
	case S2C_LOGIN_RESULT:
	case S2C_ACTION: // 공격 모션 무시
		break;
	default:
		break;
	}
}

void Worker_Thread() {
	while (true) {
		DWORD io_size;
		ULONG_PTR ci;
		OverlappedEx* over;
		BOOL ret = GetQueuedCompletionStatus(g_hiocp, &io_size, &ci, reinterpret_cast<LPWSAOVERLAPPED*>(&over), INFINITE);

		int client_id = static_cast<int>(ci);
		if (FALSE == ret) {
			DisconnectClient(client_id);
			if (over && OP_SEND == over->event_type) delete over;
			continue;
		}
		if (0 == io_size) {
			DisconnectClient(client_id);
			continue;
		}

		if (OP_RECV == over->event_type) {
			unsigned char* buf = g_clients[ci].recv_over.IOCP_buf;
			unsigned psize = g_clients[ci].curr_packet_size;
			unsigned pr_size = g_clients[ci].prev_packet_data;

			while (io_size > 0) {
				if (0 == psize) psize = buf[0];
				if (io_size + pr_size >= psize) {
					unsigned char packet[MAX_PACKET_SIZE];
					memcpy(packet, g_clients[ci].packet_buf, pr_size);
					memcpy(packet + pr_size, buf, psize - pr_size);

					ProcessPacket(static_cast<int>(ci), packet);

					io_size -= psize - pr_size;
					buf += psize - pr_size;
					psize = 0; pr_size = 0;
				}
				else {
					memcpy(g_clients[ci].packet_buf + pr_size, buf, io_size);
					pr_size += io_size;
					io_size = 0;
				}
			}
			g_clients[ci].curr_packet_size = psize;
			g_clients[ci].prev_packet_data = pr_size;

			DWORD recv_flag = 0;
			int ret = WSARecv(g_clients[ci].client_socket, &g_clients[ci].recv_over.wsabuf, 1, NULL, &recv_flag, &g_clients[ci].recv_over.over, NULL);
			if (SOCKET_ERROR == ret) {
				int err_no = WSAGetLastError();
				if (err_no != WSA_IO_PENDING) DisconnectClient(client_id);
			}
		}
		else if (OP_SEND == over->event_type) {
			delete over;
		}
	}
}

constexpr int DELAY_LIMIT = 100;
constexpr int DELAY_LIMIT2 = 150;
constexpr int ACCEPT_DELY = 10;

void Adjust_Number_Of_Client() {
	static int delay_multiplier = 1;
	static int max_limit = MAXINT;
	static bool increasing = true;

	if (active_clients >= MAX_TEST) return;
	if (num_connections >= MAX_CLIENTS) return;

	auto duration = high_resolution_clock::now() - last_connect_time;
	if (ACCEPT_DELY * delay_multiplier > duration_cast<milliseconds>(duration).count()) return;

	int t_delay = global_delay;
	if (DELAY_LIMIT2 < t_delay) {
		if (true == increasing) {
			max_limit = active_clients;
			increasing = false;
		}
		if (100 > active_clients) return;
		if (ACCEPT_DELY * 10 > duration_cast<milliseconds>(duration).count()) return;
		last_connect_time = high_resolution_clock::now();
		DisconnectClient(client_to_close);
		client_to_close++;
		return;
	}
	else if (DELAY_LIMIT < t_delay) {
		delay_multiplier = 10;
		return;
	}

	if (max_limit - (max_limit / 20) < active_clients) return;

	increasing = true;
	last_connect_time = high_resolution_clock::now();

	// 🚨 새로운 접속 생성
	g_clients[num_connections].client_socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

	int opt = 1;
	setsockopt(g_clients[num_connections].client_socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));

	SOCKADDR_IN ServerAddr;
	ZeroMemory(&ServerAddr, sizeof(SOCKADDR_IN));
	ServerAddr.sin_family = AF_INET;
	ServerAddr.sin_port = htons(PORT);
	ServerAddr.sin_addr.s_addr = inet_addr(g_server_ip.c_str());

	int Result = WSAConnect(g_clients[num_connections].client_socket, (sockaddr*)&ServerAddr, sizeof(ServerAddr), NULL, NULL, NULL, NULL);
	if (0 != Result) return;

	g_clients[num_connections].curr_packet_size = 0;
	g_clients[num_connections].prev_packet_data = 0;
	ZeroMemory(&g_clients[num_connections].recv_over, sizeof(g_clients[num_connections].recv_over));
	g_clients[num_connections].recv_over.event_type = OP_RECV;
	g_clients[num_connections].recv_over.wsabuf.buf = reinterpret_cast<CHAR*>(g_clients[num_connections].recv_over.IOCP_buf);
	g_clients[num_connections].recv_over.wsabuf.len = sizeof(g_clients[num_connections].recv_over.IOCP_buf);

	CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_clients[num_connections].client_socket), g_hiocp, num_connections, 0);

	C2S_Login l_packet;
	int temp = num_connections;
	sprintf_s(l_packet.username, "Dummy_%d", temp);
	l_packet.size = sizeof(l_packet);
	l_packet.type = C2S_LOGIN;
	SendPacket(num_connections, &l_packet);

	DWORD recv_flag = 0;
	WSARecv(g_clients[num_connections].client_socket, &g_clients[num_connections].recv_over.wsabuf, 1,
		NULL, &recv_flag, &g_clients[num_connections].recv_over.over, NULL);

	num_connections++;
}

void Test_Thread() {
	while (true) {
		Adjust_Number_Of_Client();

		for (int i = 0; i < num_connections; ++i) {
			if (false == g_clients[i].connected) continue;
			// 1초마다 무작위 방향으로 1칸 이동 패킷 발송
			if (g_clients[i].last_move_time + 1s > high_resolution_clock::now()) continue;

			g_clients[i].last_move_time = high_resolution_clock::now();

			char dir = rand() % 4; // 0: 위, 1: 아래, 2: 왼쪽, 3: 오른쪽

			C2S_Move my_packet;
			memset(&my_packet, 0, sizeof(my_packet));
			my_packet.size = sizeof(my_packet);
			my_packet.type = C2S_MOVE;
			my_packet.direction = dir;
			my_packet.x = 0; // 서버가 알아서 현재 좌표 기준으로 계산해 줌
			my_packet.y = 0;
			my_packet.move_time = static_cast<unsigned>(duration_cast<milliseconds>(high_resolution_clock::now().time_since_epoch()).count());

			SendPacket(i, &my_packet);
		}
	}
}

void InitializeNetwork() {
	cout << "접속할 서버의 IP 주소를 입력하세요 (로컬 테스트시 그냥 엔터): ";
	string input_ip;
	getline(cin, input_ip);
	if (!input_ip.empty()) g_server_ip = input_ip;

	for (auto& cl : g_clients) { cl.connected = false; cl.id = INVALID_ID; }
	for (auto& cl : client_map) cl = -1;

	num_connections = 0;
	last_connect_time = high_resolution_clock::now();

	WSADATA	wsadata;
	WSAStartup(MAKEWORD(2, 2), &wsadata);

	g_hiocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, NULL, 0);

	for (int i = 0; i < 6; ++i) worker_threads.push_back(new std::thread{ Worker_Thread });

	test_thread = thread{ Test_Thread };
}

void GetPointCloud(int* size, float** points) {
	int index = 0;
	for (int i = 0; i < num_connections; ++i) {
		if (true == g_clients[i].connected) {
			point_cloud[index * 2] = static_cast<float>(g_clients[i].x);
			point_cloud[index * 2 + 1] = static_cast<float>(g_clients[i].y);
			index++;
		}
	}
	*size = index;
	*points = point_cloud;
}