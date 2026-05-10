#include <iostream>
#include <array>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <chrono>
#include <concurrent_priority_queue.h>
#include <tbb/concurrent_unordered_map.h>
#include "protocol_2026.h"

#pragma comment(lib, "WS2_32.lib")
#pragma comment(lib, "MSWSock.lib")
using namespace std;
using namespace std::chrono;

constexpr int BUF_SIZE = 200;
constexpr int VIEW_RANGE = 8;

constexpr int SECTOR_SIZE = 8;
constexpr int MAX_SECTOR_X = WORLD_WIDTH / SECTOR_SIZE + 1;
constexpr int MAX_SECTOR_Y = WORLD_HEIGHT / SECTOR_SIZE + 1;

struct SECTOR {
	unordered_set<int> objects;
	shared_mutex lock;
};
SECTOR g_sectors[MAX_SECTOR_X][MAX_SECTOR_Y];

constexpr int MOVE_COOL_TIME = 1000; // ms

constexpr int EVENT_MOVE = 1;

struct event_type {
	int obj_id;
	system_clock::time_point wakeup_time;
	int event_id;
	int target_id;

	constexpr bool operator < (const event_type& _Left) const
	{
		return (wakeup_time > _Left.wakeup_time);
	}
};
concurrency::concurrent_priority_queue<event_type> timer_queue;

#pragma pack(push, 1)
struct PACKET_HEADER {
	unsigned char size;
	PACKET_TYPE type;
};
#pragma pack(pop)

enum COMP_TYPE { OP_ACCEPT, OP_RECV, OP_SEND, OP_NPCMOVE };
class OVER_EXP {
public:
	WSAOVERLAPPED _over;
	WSABUF _wsabuf;
	char _send_buf[BUF_SIZE];
	COMP_TYPE _comp_type;

	// int _ai_target_obj;
	OVER_EXP()
	{
		_wsabuf.len = BUF_SIZE;
		_wsabuf.buf = _send_buf;
		_comp_type = OP_RECV;
		ZeroMemory(&_over, sizeof(_over));
	}
	OVER_EXP(char* packet)
	{
		_wsabuf.len = packet[0];
		_wsabuf.buf = _send_buf;
		ZeroMemory(&_over, sizeof(_over));
		_comp_type = OP_SEND;
		memcpy(_send_buf, packet, packet[0]);
	}
};

enum S_STATE { ST_FREE, ST_ALLOC, ST_INGAME };
class SESSION {
	OVER_EXP _recv_over;

public:
	mutex _s_lock;
	S_STATE _state;
	int _id;
	SOCKET _socket;
	short	x, y;
	int hp, max_hp;
	unsigned long long exp;
	unsigned char level;
	char	_name[MAX_NAME_LEN];
	int		_prev_remain;

	unordered_set <int> _view_list;
	mutex	_vl;

	int last_move_time;
	std::atomic<bool> _active_npc;
	system_clock::time_point npc_last_move_time;

public:
	SESSION()
	{
		_id = -1;
		_socket = 0;
		x = y = 0;
		hp = max_hp = 100;
		exp = 0; level = 1;
		_name[0] = 0;
		_state = ST_FREE;
		_prev_remain = 0;
		_active_npc = false;
	}

	~SESSION() {}

	void do_recv()
	{
		DWORD recv_flag = 0;
		memset(&_recv_over._over, 0, sizeof(_recv_over._over));
		_recv_over._wsabuf.len = BUF_SIZE - _prev_remain;
		_recv_over._wsabuf.buf = _recv_over._send_buf + _prev_remain;
		WSARecv(_socket, &_recv_over._wsabuf, 1, 0, &recv_flag, &_recv_over._over, 0);
	}

	void do_send(void* packet)
	{
		OVER_EXP* sdata = new OVER_EXP{ reinterpret_cast<char*>(packet) };
		WSASend(_socket, &sdata->_wsabuf, 1, 0, 0, &sdata->_over, 0);
	}
	void send_login_result()
	{
		S2C_LoginResult p;
		p.size = sizeof(p);
		p.type = S2C_LOGIN_RESULT;
		p.success = true;
		strcpy_s(p.message, "Login Successful");
		do_send(&p);
	}
	void send_avatar_info()
	{
		S2C_AvatarInfo p;
		p.size = sizeof(p);
		p.type = S2C_AVATAR_INFO;
		p.playerId = _id;
		p.visualId = 0;
		p.x = x;
		p.y = y;
		p.hp = hp;
		p.max_hp = max_hp;
		p.exp = exp;
		p.level = level;
		do_send(&p);
	}
	void send_move_packet(int c_id);
	void send_add_object_packet(int c_id);
	void send_chat_packet(int p_id, const char* mess)
	{
		S2C_ChatMessage p;
		p.size = sizeof(p);
		p.type = S2C_CHAT_MESSAGE;
		p.object_id = p_id;
		strcpy_s(p.message, mess);
		do_send(&p);
	}
	void send_remove_object_packet(int c_id)
	{
		_vl.lock();
		if (_view_list.count(c_id)) _view_list.erase(c_id);
		else { _vl.unlock(); return; }
		_vl.unlock();

		S2C_RemoveObject p;
		p.size = sizeof(p);
		p.type = S2C_REMOVE_OBJECT;
		p.object_id = c_id;
		do_send(&p);
	}
	void do_random_move();
	void heart_beat()
	{
		// NPC의 경우, 일정 시간마다 랜덤한 방향으로 이동하는 기능을 구현한다.
		// 이동한 후에는, 이동한 위치를 주변 플레이어들에게 알려준다.
		do_random_move();
	}
	void wake_up()
	{
		bool expected = false;
		// _active_npc가 false인 경우에만 true로 바꿔주고, true로 바뀐 경우에만 do_random_move()를 호출한다.
		if (false == _active_npc.compare_exchange_strong(expected, true))
			return;

		event_type ev;
		ev.obj_id = _id;
		ev.event_id = EVENT_MOVE;
		ev.target_id = -1;
		ev.wakeup_time = system_clock::now() + milliseconds(MOVE_COOL_TIME);
		timer_queue.push(ev);
	}
};

HANDLE h_iocp;
tbb::concurrent_unordered_map<int, shared_ptr<SESSION>> clients;
SOCKET g_s_socket, g_c_socket;
OVER_EXP g_a_over;

// NPC 구현 첫번째 방법
//  NPC클래스를 별도 제작, NPC컨테이너를 따로 생성한다.
//  장점 : 깔끔하다, 군더더기가 없다.
//  단점 : 플레이어와 NPC가 따로논다. 똑같은 역할을 수행하는 함수를 여러개씩 중복 작성해야 한다.
//         예) bool can_see(int from, int to)
//                 => bool can_see_p2p()
//				    bool can_see_p2n()
//					bool can_see_n2n()

// NPC 구현 두번째 방법  <===== 실습에서 사용할 방법.
//   clients 컨테이너에 NPC도 추가한다.
//   장점 : 플레이어와 NPC를 동일하게 취급할 수 있어서, 프로그래밍 작성 부하가 줄어든다.
//   단점 : 사용하지 않는 멤버들로 인한 메모리 낭비.

// NPC 구현 세번째 방법  (실제로 많이 사용되는 방법)
//   클래스 상속기능을 사용한다.
//     SESSION은 NPC클래스를 상속받아서 네트워크 관련 기능을 추가한 형태로 정의한다.
//       clients컨테이너를 objects컨테이너로 변경하고, 컨테이너는 NPC의 pointer를 저장한다.
//      장점 : 메모리 낭비가 없다, 함수의 중복작성이 필요없다.
//          (포인터로 관리되므로 player id의 중복사용 방지를 구현하기 쉬워진다 => Data Race 방지를 위한 추가 구현이 필요)
//      단점 : 포인터가 사용되고, reinterpret_cast가 필요하다. (별로 단점이 아니다).

bool is_pc(int object_id)
{
	return object_id < NPC_ID_START;
}
bool is_npc(int object_id)
{
	return object_id >= NPC_ID_START;
}

bool can_see(int from, int to)
{
	if (abs(clients[from]->x - clients[to]->x) > VIEW_RANGE) return false;
	return abs(clients[from]->y - clients[to]->y) <= VIEW_RANGE;
}

short get_sector_x(short x)
{
	return max((short)0, min((short)(MAX_SECTOR_X - 1), (short)(x / SECTOR_SIZE)));
}
short get_sector_y(short y)
{
	return max((short)0, min((short)(MAX_SECTOR_Y - 1), (short)(y / SECTOR_SIZE)));
}

// 주변 9칸 섹터 검색 함수
unordered_set<int> get_near_objects(short cur_x, short cur_y)
{
	unordered_set<int> near_objs;
	short sector_x = get_sector_x(cur_x);
	short sector_y = get_sector_y(cur_y);

	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			short near_sx = sector_x + dx;
			short near_sy = sector_y + dy;
			if (near_sx < 0 || near_sx >= MAX_SECTOR_X || near_sy < 0 || near_sy >= MAX_SECTOR_Y)
				continue;

			shared_lock<shared_mutex> read_lock(g_sectors[near_sx][near_sy].lock);
			for (int id : g_sectors[near_sx][near_sy].objects) {
				near_objs.insert(id);
			}
		}
	}
	return near_objs;
}

void SESSION::do_random_move()
{
	short old_x = x, old_y = y;
	short old_sector_x = get_sector_x(old_x);
	short old_sector_y = get_sector_y(old_y);

	unordered_set<int> old_vl;
	auto near_old = get_near_objects(old_x, old_y);

	for (int obj_id : near_old) {
		if (obj_id == _id) continue;
		auto target = clients[obj_id];
		if (!target || ST_INGAME != target->_state) continue;
		if (is_npc(target->_id)) continue;	// PC만 시야에 넣음 (부하 방지)
		if (can_see(_id, target->_id)) old_vl.insert(obj_id);
	}


	switch (rand() % 4) {
	case 0: if (x < (WORLD_WIDTH - 1)) x++; break;
	case 1: if (x > 0) x--; break;
	case 2: if (y < (WORLD_HEIGHT - 1)) y++; break;
	case 3:if (y > 0) y--; break;
	}

	short new_sector_x = get_sector_x(x);
	short new_sector_y = get_sector_y(y);

	if (old_sector_x != new_sector_x || old_sector_y != new_sector_y) {
		{
			unique_lock<shared_mutex> write_lock(g_sectors[old_sector_x][old_sector_y].lock);
			g_sectors[old_sector_x][old_sector_y].objects.erase(_id);
		}
		{
			unique_lock<shared_mutex> write_lock(g_sectors[new_sector_x][new_sector_y].lock);
			g_sectors[new_sector_x][new_sector_y].objects.insert(_id);
		}
	}

	unordered_set<int> new_vl;
	auto near_new = get_near_objects(x, y);
	for (int obj_id : near_new) {
		if (obj_id == _id) continue;
		auto target = clients[obj_id];
		if (!target || ST_INGAME != target->_state) continue;
		if (is_npc(target->_id)) continue;
		if (can_see(_id, target->_id)) new_vl.insert(obj_id);
	}

	for (auto pl : new_vl) {
		if (0 == old_vl.count(pl)) {
			// 플레이어의 시야에 등장
			clients[pl]->send_add_object_packet(_id);
		}
		else {
			// 플레이어가 계속 보고 있음.
			clients[pl]->send_move_packet(_id);
		}
	}
	///vvcxxccxvvdsvdvds
	for (auto pl : old_vl)
		if (0 == new_vl.count(pl))
				clients[pl]->send_remove_object_packet(_id);

	npc_last_move_time = system_clock::now();
}

void SESSION::send_move_packet(int c_id)
{
	S2C_MoveObject p;
	p.size = sizeof(p);
	p.type = S2C_MOVE_OBJECT;
	p.object_id = c_id;
	p.x = clients[c_id]->x;
	p.y = clients[c_id]->y;
	p.move_time = clients[c_id]->last_move_time;
	do_send(&p);
}

void SESSION::send_add_object_packet(int c_id)
{
	S2C_AddObject p;
	p.size = sizeof(p);
	p.type = S2C_ADD_OBJECT;
	p.object_id = c_id;
	p.visual_id = is_npc(c_id) ? 1 : 0;
	strcpy_s(p.obj_name, clients[c_id]->_name);
	p.x = clients[c_id]->x;
	p.y = clients[c_id]->y;
	p.hp = clients[c_id]->hp;
	p.max_hp = clients[c_id]->max_hp;
	p.exp = clients[c_id]->exp;
	p.level = clients[c_id]->level;

	_vl.lock();
	_view_list.insert(c_id);
	_vl.unlock();
	do_send(&p);
}

int get_new_client_id()
{
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		lock_guard <mutex> ll{ clients[i]->_s_lock };

		if (clients[i]->_state == ST_FREE) {
			clients[i]->_state = ST_ALLOC;	// 빈자리 찾았으면 내가 쓴다고 예약 상태로 변경
			return i;
		}
	}
	return -1;
}

void process_packet(int c_id, char* packet)
{
	PACKET_TYPE type = reinterpret_cast<PACKET_HEADER*>(packet)->type;

	switch (type) {
	case C2S_LOGIN: {
		C2S_Login* p = reinterpret_cast<C2S_Login*>(packet);
		strcpy_s(clients[c_id]->_name, p->username);

		{
			lock_guard<mutex> ll{ clients[c_id]->_s_lock };	// lock local (지역 잠그기)
			clients[c_id]->x = rand() % WORLD_WIDTH;
			clients[c_id]->y = rand() % WORLD_HEIGHT;
			clients[c_id]->_state = ST_INGAME;
		}

		short sector_x = get_sector_x(clients[c_id]->x);
		short sector_y = get_sector_y(clients[c_id]->y);
		{
			unique_lock<shared_mutex> write_lock(g_sectors[sector_x][sector_y].lock);
			g_sectors[sector_x][sector_y].objects.insert(c_id);
		}

		clients[c_id]->send_login_result();
		clients[c_id]->send_avatar_info();

		auto near_objs = get_near_objects(clients[c_id]->x, clients[c_id]->y);
		for (int pl_id : near_objs) {
			if (pl_id == c_id) continue;
			auto target = clients[pl_id];
			if (!target || ST_INGAME != target->_state) continue;
			if (can_see(c_id, pl_id)) {
				// 내 캐릭터가 다른 PC에게 보임
				if (is_pc(pl_id)) target->send_add_object_packet(c_id);
				// 다른 객체(PC 또는 NPC)가 내 캐릭터에게 보임
				clients[c_id]->send_add_object_packet(pl_id);

				if (is_npc(pl_id)) {
					target->wake_up();
				}
			}
		}
		break;
	}
	case C2S_MOVE: {
		C2S_Move* p = reinterpret_cast<C2S_Move*>(packet);
		clients[c_id]->last_move_time = p->move_time;

		short old_x = clients[c_id]->x;
		short old_y = clients[c_id]->y;
		short old_sector_x = get_sector_x(old_x);
		short old_sector_y = get_sector_y(old_y);

		// 새 프로토콜은 목적지 좌표(x, y)를 직접 받음
		short x = max((short)0, min((short)(WORLD_WIDTH - 1), p->x));
		short y = max((short)0, min((short)(WORLD_HEIGHT - 1), p->y));
		clients[c_id]->x = x;
		clients[c_id]->y = y;

		short new_sector_x = get_sector_x(x);
		short new_sector_y = get_sector_y(y);

		if (old_sector_x != new_sector_x || old_sector_y != new_sector_y) {
			{
				unique_lock<shared_mutex> write_lock(g_sectors[old_sector_x][old_sector_y].lock);
				g_sectors[old_sector_x][old_sector_y].objects.erase(c_id);
			}
			{
				unique_lock<shared_mutex> write_lock(g_sectors[new_sector_x][new_sector_y].lock);
				g_sectors[new_sector_x][new_sector_y].objects.insert(c_id);
			}
		}

		unordered_set<int> near_list;
		clients[c_id]->_vl.lock();
		unordered_set<int> old_vlist = clients[c_id]->_view_list;
		clients[c_id]->_vl.unlock();

		auto near_objs = get_near_objects(x, y);
		for (auto& cl_id : near_objs) {
			if (cl_id == c_id) continue;
			auto target = clients[cl_id];
			if (!target || ST_INGAME != target->_state) continue;
			if (can_see(c_id, cl_id))
				near_list.insert(cl_id);
		}

		clients[c_id]->send_move_packet(c_id);

		for (auto& pl : near_list) {
			auto& cpl = clients[pl];
			if (is_pc(pl)) {
				cpl->_vl.lock();
				if (cpl->_view_list.count(c_id)) {
					cpl->_vl.unlock();
					cpl->send_move_packet(c_id);
				}
				else {
					cpl->_vl.unlock();
					cpl->send_add_object_packet(c_id);
				}
			}
			if (old_vlist.count(pl) == 0) {
				clients[c_id]->send_add_object_packet(pl);
				if (is_npc(pl))
					clients[pl]->wake_up();	// 이동하며 새로 발견한 NPC 깨우기
			}
		}

		for (auto& pl : old_vlist) {
			if (0 == near_list.count(pl)) {
				clients[c_id]->send_remove_object_packet(pl);
				if (is_pc(pl))
					clients[pl]->send_remove_object_packet(c_id);
			}
		}
		break;
	}
	}
}

void disconnect(int c_id)
{
	clients[c_id]->_vl.lock();
	unordered_set <int> vl = clients[c_id]->_view_list;
	clients[c_id]->_vl.unlock();
	for (auto& p_id : vl) {
		if (is_npc(p_id)) continue;
		auto& pl = clients[p_id];
		{
			lock_guard<mutex> ll(pl->_s_lock);
			if (ST_INGAME != pl->_state) continue;
		}
		if (pl->_id == c_id) continue;
		pl->send_remove_object_packet(c_id);
	}
	closesocket(clients[c_id]->_socket);

	short sector_x = get_sector_x(clients[c_id]->x);
	short sector_y = get_sector_y(clients[c_id]->y);
	{
		unique_lock<shared_mutex> write_lock(g_sectors[sector_x][sector_y].lock);
		g_sectors[sector_x][sector_y].objects.erase(c_id);
	}

	lock_guard<mutex> ll(clients[c_id]->_s_lock);
	clients[c_id]->_state = ST_FREE;
}

void do_npc_random_move(int npc_id)
{
	clients[npc_id]->do_random_move();
}

void worker_thread(HANDLE h_iocp)
{
	while (true) {
		DWORD num_bytes;
		ULONG_PTR key;
		WSAOVERLAPPED* over = nullptr;
		BOOL ret = GetQueuedCompletionStatus(h_iocp, &num_bytes, &key, &over, INFINITE);
		OVER_EXP* ex_over = reinterpret_cast<OVER_EXP*>(over);
		if (FALSE == ret) {
			if (ex_over->_comp_type == OP_ACCEPT) cout << "Accept Error";
			else {
				cout << "GQCS Error on client[" << key << "]\n";
				disconnect(static_cast<int>(key));
				if (ex_over->_comp_type == OP_SEND) delete ex_over;
				continue;
			}
		}

		if ((0 == num_bytes) && ((ex_over->_comp_type == OP_RECV) || (ex_over->_comp_type == OP_SEND))) {
			disconnect(static_cast<int>(key));
			if (ex_over->_comp_type == OP_SEND) delete ex_over;
			continue;
		}

		switch (ex_over->_comp_type) {
		case OP_ACCEPT: {
			int client_id = get_new_client_id();
			if (client_id != -1) {
				clients[client_id]->x = 0;
				clients[client_id]->y = 0;
				clients[client_id]->_id = client_id;
				clients[client_id]->_name[0] = 0;
				clients[client_id]->_prev_remain = 0;
				clients[client_id]->_socket = g_c_socket;
				CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_c_socket), h_iocp, client_id, 0);
				clients[client_id]->do_recv();
				g_c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			}
			else {
				cout << "Max user exceeded.\n";
			}
			ZeroMemory(&g_a_over._over, sizeof(g_a_over._over));
			int addr_size = sizeof(SOCKADDR_IN);
			AcceptEx(g_s_socket, g_c_socket, g_a_over._send_buf, 0, addr_size + 16, addr_size + 16, 0, &g_a_over._over);
			break;
		}
		case OP_RECV: {
			int remain_data = num_bytes + clients[key]->_prev_remain;
			char* p = ex_over->_send_buf;
			while (remain_data > 0) {
				int packet_size = static_cast<unsigned char>(p[0]);
				if (packet_size == 0) break;
				if (packet_size <= remain_data) {
					process_packet(static_cast<int>(key), p);
					p = p + packet_size;
					remain_data = remain_data - packet_size;
				}
				else break;
			}
			clients[key]->_prev_remain = remain_data;
			if (remain_data > 0) {
				memcpy(ex_over->_send_buf, p, remain_data);
			}
			clients[key]->do_recv();
			break;
		}
		case OP_SEND:
			delete ex_over;
			break;
		case OP_NPCMOVE:
			delete ex_over;
			int npc_id = static_cast<int>(key);
			do_npc_random_move(npc_id);

			bool has_nearby_player = false;
			auto near_objs = get_near_objects(clients[npc_id]->x, clients[npc_id]->y);
			for (int id : near_objs) {
				if (is_pc(id) && clients[id]->_state == ST_INGAME && can_see(npc_id, id)) {
					has_nearby_player = true;
					break;
				}
			}

			if (has_nearby_player) {
				event_type ev;
				ev.event_id = EVENT_MOVE;
				ev.obj_id = npc_id;
				ev.target_id = -1;
				ev.wakeup_time = system_clock::now() + milliseconds(NPC_MOVE_INTERVAL);
				timer_queue.push(ev);
			}
			else {
				clients[npc_id]->_active_npc = false;	// 플레이어가 주변에 없으면 다시 취침
			}
			break;
		}
	}
}

void InitializeNPC()
{
	cout << "NPC intialize begin.\n";
	for (int i = NPC_ID_START; i < NPC_ID_START + NUM_NPCS; ++i) {
		auto npc = make_shared<SESSION>();
		npc->x = rand() % WORLD_WIDTH;
		npc->y = rand() % WORLD_HEIGHT;
		npc->_id = i;
		sprintf_s(npc->_name, "NPC%d", i);
		npc->_state = ST_INGAME;
		npc->npc_last_move_time = system_clock::now();
		clients[i] = npc;

		short sector_x = get_sector_x(npc->x);
		short sector_y = get_sector_y(npc->y);
		{
			unique_lock<shared_mutex> write_lock(g_sectors[sector_x][sector_y].lock);
			g_sectors[sector_x][sector_y].objects.insert(i);
		}

		// Wake-up 로직이 구현되어 있으므로 초기 구동 시 강제로 깨울 필요 없음.
		// 플레이어가 접속하여 시야에 들어올 때만 wake_up()이 호출되어 활성화
	}
	cout << "NPC initialize end.\n";
}

//void HB_thread ()
//{
//	using namespace chrono;
//
//	while (true) {
//		auto start_time = chrono::system_clock::now();
//		for (int i = MAX_USER; i < MAX_USER + MAX_NPC; ++i) {
//			if (clients[i]._state != ST_INGAME) continue;
//			clients[i].heart_beat();
//		}
//		auto end_time = chrono::system_clock::now();
//		auto elapsed = end_time - start_time;
//		if (elapsed < chrono::milliseconds(MOVE_COOL_TIME)) {
//			this_thread::sleep_for(chrono::milliseconds(MOVE_COOL_TIME) - elapsed);
//		}
//
//		std::cout << "Elapsed Time : "
//			<< duration_cast<milliseconds>(elapsed).count()
//			<< "ms.\n";
//	}
//}
//
//void ai_thread()
//{
//	while (true) {
//		int elapsed_time = 1000;
//		auto current_time = system_clock::now();
//		for (int i = MAX_USER; i < MAX_USER + MAX_NPC; ++i) {
//			if (clients[i]._state != ST_INGAME) continue;
//			auto duration = duration_cast<milliseconds>(current_time - clients[i].npc_last_move_time).count();
//			if (duration >= MOVE_COOL_TIME) {
//				do_npc_random_move(i);
//				clients[i].npc_last_move_time = current_time;
//
//				if (duration > elapsed_time) elapsed_time++;
//				else if (duration < elapsed_time) elapsed_time--;
//
//			}
//		}
//		std::cout << "Elapsed Time : " << elapsed_time << "ms.\n";
//	}
//}

void timer_thread()
{
	while (true) {
		event_type ev;
		if (timer_queue.try_pop(ev)) {
			auto now = system_clock::now();
			if (ev.wakeup_time <= now) {
				switch (ev.event_id) {
				case EVENT_MOVE:
					OVER_EXP* move_over = new OVER_EXP;
					move_over->_comp_type = OP_NPCMOVE;
					PostQueuedCompletionStatus(h_iocp, -1, ev.obj_id, &move_over->_over);		
					// 두번째 인자는 number of bytes transferred인데, (몇 바이트가 전송이 됐냐)
					// 0을 넣으면 커넥션이 끝났다고 판단할 수 
					// 있기 때문에 0말고 다른 값
					break;
				}
			}
			else {
				// 이래도 돌아가긴 하지만 성능적으로 바람직하지 않음. 
				// why? timer_queue는 여러 쓰레드가 건드리니까 이거 access하는 거 오버헤드가 크다. 
				// 그래서 여기에 푸시하면 안되고 별도 로컬 큐를 만든 다음 거기에 푸시팝하는 게 좋다.
				timer_queue.push(ev);
			}
		}
		else this_thread::sleep_for(chrono::milliseconds(5));
	}
}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);

	for (int i = 0; i < MAX_PLAYERS; ++i) {
		clients[i] = make_shared<SESSION>();
		clients[i]->_id = i;
		clients[i]->_state = ST_FREE;
	}

	g_s_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);	// 3500
	server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
	::bind(g_s_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
	listen(g_s_socket, SOMAXCONN);

	InitializeNPC();

	h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
	CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_s_socket), h_iocp, 9999, 0);
	g_c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	g_a_over._comp_type = OP_ACCEPT;
	int addr_size = sizeof(SOCKADDR_IN);
	AcceptEx(g_s_socket, g_c_socket, g_a_over._send_buf, 0, addr_size + 16, addr_size + 16, 0, &g_a_over._over);

	vector <thread> worker_threads;
	thread ai_th(timer_thread);	// 타이머 전담
	int num_threads = std::thread::hardware_concurrency();
	for (int i = 0; i < num_threads; ++i)
		worker_threads.emplace_back(worker_thread, h_iocp);
	for (auto& th : worker_threads)
		th.join();
	ai_th.join();
	closesocket(g_s_socket);
	WSACleanup();
}
