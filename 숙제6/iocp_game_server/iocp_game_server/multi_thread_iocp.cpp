#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include <mutex>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <tbb/concurrent_unordered_map.h>
#include "protocol.h"

std::default_random_engine dre;
std::uniform_int_distribution<short> uid_x{ 0, WORLD_WIDTH - 1 };
std::uniform_int_distribution<short> uid_y{ 0, WORLD_HEIGHT - 1 };

#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "WS2_32.lib")
using namespace std;

constexpr int BUF_SIZE = 200;

enum IOType { IO_SEND, IO_RECV, IO_ACCEPT };
enum SESSION_STATE { ST_FREE, ST_ALLOC, ST_INGAME, ST_CLOSE };

class EXP_OVER {
public:
	WSAOVERLAPPED m_over;
	IOType  m_iotype;
	WSABUF	m_wsa;
	char  m_buff[BUF_SIZE];
	EXP_OVER()
	{
		ZeroMemory(&m_over, sizeof(m_over));
		m_wsa.buf = m_buff;
		m_wsa.len = BUF_SIZE;
	}
	EXP_OVER(IOType iot) : m_iotype(iot)
	{
		ZeroMemory(&m_over, sizeof(m_over));
		m_wsa.buf = m_buff;
		m_wsa.len = BUF_SIZE;
	}
};

void error_display(const wchar_t* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	std::wcout << msg;
	std::wcout << L" === 에러 " << lpMsgBuf << std::endl;
	while (true);   // 디버깅 용
	LocalFree(lpMsgBuf);
}

class SESSION {
public:
	SOCKET m_client;
	int m_id;
	std::atomic<SESSION_STATE> m_state;
	std::mutex m_name_lock;

	EXP_OVER m_recv_over;
	int m_prev_recv;
	char m_username[MAX_NAME_LEN];
	short m_x, m_y;
	SESSION() {
		m_state.store(ST_FREE);
		m_id = 999;
		m_client = INVALID_SOCKET;
		m_recv_over.m_iotype = IO_RECV;
		m_x = 0; 		m_y = 0;
		m_prev_recv = 0;
	}
	~SESSION()
	{
		if (m_state.load() != ST_FREE)
			closesocket(m_client);
	}
	void do_recv()
	{
		DWORD recv_flag = 0;
		memset(&m_recv_over.m_over, 0, sizeof(m_recv_over.m_over));
		WSARecv(m_client, &m_recv_over.m_wsa, 1, 0, &recv_flag, &m_recv_over.m_over, nullptr);
	}
	void do_send(int num_bytes, char* mess)
	{
		EXP_OVER* o = new EXP_OVER(IO_SEND);
		o->m_wsa.len = num_bytes;
		memcpy(o->m_buff, mess, num_bytes);
		WSASend(m_client, &o->m_wsa, 1, 0, 0, &o->m_over, nullptr);
	}
	void send_avatar_info()
	{
		S2C_AvatarInfo packet;
		packet.size = sizeof(S2C_AvatarInfo);
		packet.type = S2C_AVATAR_INFO;
		packet.playerId = m_id;
		packet.x = m_x;
		packet.y = m_y;
		do_send(packet.size, reinterpret_cast<char*>(&packet));
	}
	void send_move_packet(int mover, unsigned move_time);
	void send_add_player(int player_id);
	void send_login_success()
	{
		S2C_LoginResult packet;
		packet.size = sizeof(S2C_LoginResult);
		packet.type = S2C_LOGIN_RESULT;
		packet.success = true;
		strncpy_s(packet.message, "Login successful.", sizeof(packet.message));
		do_send(packet.size, reinterpret_cast<char*>(&packet));
	}
	void send_remove_player(int player_id)
	{
		S2C_RemovePlayer packet;
		packet.size = sizeof(S2C_RemovePlayer);
		packet.type = S2C_REMOVE_PLAYER;
		packet.playerId = player_id;
		do_send(packet.size, reinterpret_cast<char*>(&packet));
	}
	void process_packet(unsigned char* p);
};

tbb::concurrent_unordered_map<int, std::atomic<std::shared_ptr<SESSION>>> clients;
std::atomic<int> g_next_id{ 0 };

HANDLE h_iocp;
SOCKET server;
SOCKET client_socket;
EXP_OVER accept_over(IO_ACCEPT);

void SESSION::send_add_player(int player_id)
{
	auto it = clients.find(player_id);
	if (it == clients.end()) return;
	auto target_session = it->second.load();
	if (nullptr == target_session) return;

	S2C_AddPlayer packet;
	packet.size = sizeof(S2C_AddPlayer);
	packet.type = S2C_ADD_PLAYER;
	packet.playerId = player_id;

	SESSION& ts = *target_session;

	m_name_lock.lock();
	memcpy(packet.username, ts.m_username, sizeof(packet.username));
	m_name_lock.unlock();

	packet.x = ts.m_x;
	packet.y = ts.m_y;
	do_send(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::process_packet(unsigned char* p)
{
	PACKET_TYPE type = *reinterpret_cast<PACKET_TYPE*>(&p[1]);
	switch (type) {
	case C2S_LOGIN: {
		C2S_Login* packet = reinterpret_cast<C2S_Login*>(p);

		m_name_lock.lock();
		strncpy_s(m_username, packet->username, MAX_NAME_LEN);
		m_name_lock.unlock();

		m_state.store(ST_INGAME);
		cout << "Player[" << m_id << "] logged in as " << m_username << "\n";
		for (auto& pair : clients) {
			auto other = pair.second.load();
			if (nullptr == other) continue;
			if (other->m_state.load() != ST_INGAME) continue;
			if (other->m_id == m_id) continue;

			other->send_add_player(m_id);
			send_add_player(other->m_id);
		}
		send_avatar_info();
	}
				  break;
	case C2S_MOVE: {
		C2S_Move* packet = reinterpret_cast<C2S_Move*>(p);
		DIRECTION dir = packet->dir;
		unsigned move_time = packet->move_time;
		switch (dir) {
		case UP: m_y = max(0, m_y - 1); break;
		case DOWN: m_y = min(WORLD_HEIGHT - 1, m_y + 1); break;
		case LEFT: m_x = max(0, m_x - 1); break;
		case RIGHT: m_x = min(WORLD_WIDTH - 1, m_x + 1); break;
		}
		// cout << "Player[" << m_id << "] moved to (" << m_x << ", " << m_y << ")\n";
		for (auto& pair : clients) {
			auto other = pair.second.load();
			if (nullptr == other) continue;
			if (other->m_state.load() == ST_INGAME) {
				other->send_move_packet(m_id, move_time);
			}
		}
	}
				 break;
	default:
		// cout << "Unknown packet type received from player[" << m_id << "].\n";
		break;
	}
}

void SESSION::send_move_packet(int mover, unsigned move_time)
{
	auto it = clients.find(mover);
	if (it == clients.end()) return;
	auto mover_session = it->second.load();
	if (nullptr == mover_session) return;

	S2C_MovePlayer packet;
	packet.size = sizeof(S2C_MovePlayer);
	packet.type = S2C_MOVE_PLAYER;
	packet.playerId = mover;
	packet.x = mover_session->m_x;
	packet.y = mover_session->m_y;
	packet.move_time = move_time;
	do_send(packet.size, reinterpret_cast<char*>(&packet));
}

void send_login_fail(SOCKET client, const char* message)
{
	S2C_LoginResult packet;
	packet.size = sizeof(S2C_LoginResult);
	packet.type = S2C_LOGIN_RESULT;
	packet.success = false;
	strncpy_s(packet.message, message, sizeof(packet.message));
	WSABUF wsa_buf;
	wsa_buf.buf = reinterpret_cast<char*>(&packet);
	wsa_buf.len = packet.size;
	WSASend(client, &wsa_buf, 1, 0, 0, nullptr, nullptr);
}

void worker_thread()
{
	for (;;) {
		DWORD num_bytes;
		ULONG_PTR key;
		LPOVERLAPPED over;
		GetQueuedCompletionStatus(h_iocp, &num_bytes, &key, &over, INFINITE);

		if (over == nullptr) {
			error_display(L"GQCS Errror: ", WSAGetLastError());
			continue;
		}

		EXP_OVER* exp_over = reinterpret_cast<EXP_OVER*>(over);

		switch (exp_over->m_iotype) {
		case IO_ACCEPT:
		{
			cout << "Client connected.\n";
			int new_id = g_next_id.fetch_add(1);

			auto new_session = std::make_shared<SESSION>();
			new_session->m_state.store(ST_ALLOC);
			new_session->m_client = client_socket;
			new_session->m_x = uid_x(dre);
			new_session->m_y = uid_y(dre);
			new_session->m_id = new_id;
			new_session->m_prev_recv = 0;

			clients[new_id].store(new_session);

			CreateIoCompletionPort((HANDLE)client_socket, h_iocp, new_id, 0);

			new_session->send_login_success();
			new_session->do_recv();

			ZeroMemory(&accept_over.m_over, sizeof(accept_over.m_over));

			client_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			AcceptEx(server, client_socket, &accept_over.m_buff, 0,
				sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
				NULL, &accept_over.m_over);
			break;
		}
		case IO_RECV:
		{
			int player_index = static_cast<int>(key);
			// cout << "Client[" << player_index << "] sent a message." << endl;
			auto it = clients.find(player_index);
			if (it == clients.end()) break;

			auto cl = it->second.load();
			if (cl == nullptr) break;
			if (num_bytes == 0) {
				cout << "Client[" << player_index << "] Disconnected.\n";
				cl->m_state.store(ST_CLOSE);

				for (auto& pair : clients) {
					auto other = pair.second.load();
					if (other == nullptr) continue;
					if (other->m_state.load() == ST_INGAME) {
						other->send_remove_player(player_index);
					}
				}

				closesocket(cl->m_client);
				cl->m_client = INVALID_SOCKET;

				it->second.store(nullptr);
				continue;
			}


			unsigned char* p = reinterpret_cast<unsigned char*>(exp_over->m_buff);
			int data_size = num_bytes + cl->m_prev_recv;
			while (data_size > 0) {
				int packet_size = p[0];
				if (packet_size > data_size) break;
				cl->process_packet(p);
				p += packet_size;
				data_size -= packet_size;
			}
			if (data_size > 0) {
				memmove(cl->m_recv_over.m_buff, p, data_size);
				cl->m_prev_recv = data_size;
			}
			cl->do_recv();
		}
		break;
		case IO_SEND: {
			// cout << "Message sent. to client[" << key << "]\n";
			EXP_OVER* o = reinterpret_cast<EXP_OVER*>(over);
			delete o;
		}
					break;
		default:
			// cout << "Unknown IO type." << endl;
			exit(-1);
			break;
		}
	}
}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	server = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);
	server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
	::bind(server, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
	listen(server, SOMAXCONN);
	h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	CreateIoCompletionPort((HANDLE)server, h_iocp, -1, 0);

	client_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

	AcceptEx(server, client_socket, &accept_over.m_buff, 0,
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
		NULL, &accept_over.m_over);

	vector<thread> threads;
	int num_threads = thread::hardware_concurrency();
	for (int i = 0; i < num_threads; ++i)
		threads.emplace_back(worker_thread);

	for (auto& t : threads)
		t.join();

	closesocket(server);
	WSACleanup();
}
