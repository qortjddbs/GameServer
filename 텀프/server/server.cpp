#define NOMINMAX

#include <iostream>
#include <WinSock2.h>
#include <MSWSock.h>
#include <thread>
#include <mutex>
#include <vector>
#include <queue>
#include <chrono>
#include <algorithm>
#include <shared_mutex>
#include <unordered_set>
#include <random>
#include <unordered_map>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_queue.h>
#include <windows.h>
#include <sql.h>
#include <sqlext.h>

#include "protocol_2026.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")
#pragma comment(lib, "odbc32.lib")

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#pragma comment (lib, "lua54.lib")

class IocpServer;
IocpServer* g_server = nullptr; 

std::string UTF8ToANSI(const char* utf8_str) {
	int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
	std::wstring wStr(wLen, 0);
	MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, &wStr[0], wLen);

	int aLen = WideCharToMultiByte(CP_ACP, 0, wStr.c_str(), -1, NULL, 0, NULL, NULL);
	std::string aStr(aLen, 0);
	WideCharToMultiByte(CP_ACP, 0, wStr.c_str(), -1, &aStr[0], aLen, NULL, NULL);
	return aStr;
}

constexpr int VIEW_RANGE = 7;        
constexpr int REGION_SIZE = 10;      
constexpr int MAX_REGION_X = WORLD_WIDTH / REGION_SIZE + 1;
constexpr int MAX_REGION_Y = WORLD_HEIGHT / REGION_SIZE + 1;

struct Region {
	std::unordered_set<int> objects; 
	std::shared_mutex lock;          
};

Region g_regions[MAX_REGION_X][MAX_REGION_Y];


bool g_wall[WORLD_WIDTH][WORLD_HEIGHT] = { false };


void InitServerMap() {
	srand(2022180016);		

	for (int i = 0; i < 5'0000; ++i) {
		int rx = rand() % (WORLD_WIDTH - 2);
		int ry = rand() % (WORLD_HEIGHT - 2);

		g_wall[rx][ry] = true;
		g_wall[rx + 1][ry] = true;
		g_wall[rx][ry + 1] = true;
		g_wall[rx + 1][ry + 1] = true;
	}

	srand(static_cast<unsigned int>(time(NULL)));
}

enum class SessionState { FREE, CONNECTED, INGAME, DEAD };
enum class EventType { EVENT_MOVE, EVENT_RESPAWN, EVENT_DESPAWN, EVENT_HP_RECOVERY, EVENT_AUTO_SAVE, EVENT_BOSS_SKILL, EVENT_BOSS_ULT };
enum class MonsterType { SKELETON, GOBLIN, FLYING_EYE, MUSHROOM };
enum class AiType { FIXED_PEACE, ROAMING_AGGRO };
enum ActionType : char { ACTION_ATTACK = 1, ACTION_HIT = 5, ACTION_DEAD = 6 };
enum class IO_OP { RECV, SEND, ACCEPT, DO_AI, DB_RESULT_LOGIN };
enum class DbTaskType { LOGIN_CHECK, SAVE_PLAYER };

struct DbTask {
	DbTaskType type;
	int session_id;
	char username[MAX_NAME_LEN];

	int level;
	long long exp;
	int hp;
	short x;
	short y;
};

struct TimerEvent 
{
	std::chrono::system_clock::time_point exec_time;	
	int object_id;										
	EventType type;										

	bool operator>(const TimerEvent& other) const {
		return exec_time > other.exec_time;
	}
};

struct IOContext
{
	WSAOVERLAPPED overlapped;
	WSABUF wsabuf;
	char buffer[1024];		
	IO_OP opType;
	SOCKET acceptSocket;	

	int db_level = 1;
	long long db_exp = 0;
	int db_hp = 100;
	short db_x = 0;
	short db_y = 0;

	IOContext(IO_OP op) : opType(op) {
		ZeroMemory(&overlapped, sizeof(overlapped));
		wsabuf.buf = buffer;
		wsabuf.len = sizeof(buffer);
	}
};

class Session
{
public:
	std::mutex sessionLock;
	std::atomic<SessionState> state = SessionState::FREE;

	int id = -1;
	SOCKET socket = INVALID_SOCKET;
	unsigned int sessionIndex = 0;		
	unsigned int playerNum = 0;			

	short x = 0, y = 0;
	unsigned char direction = 3;	
	int hp = 100, max_hp = 100;
	bool is_recovering = false;		
	int attack_power = 50;
	unsigned long long exp = 0;
	unsigned char level = 1;
	char name[MAX_NAME_LEN]{};
	bool is_god = false;

	std::chrono::steady_clock::time_point last_move_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);
	std::chrono::steady_clock::time_point last_attack_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);

	std::chrono::steady_clock::time_point last_hit_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);

	std::atomic<bool> is_active{ false };
	short origin_x = 0, origin_y = 0;	
	MonsterType monster_type = MonsterType::SKELETON;
	AiType ai_type = AiType::FIXED_PEACE;
	int target_id = -1;					
	std::atomic<int> viewers_count{ 0 };	

	std::unordered_set<int> viewList;	
	std::mutex viewLock;				

	IOContext recvContext{ IO_OP::RECV };
	int prevRemainBytes = 0;

	lua_State* L_ai = nullptr; 
	bool is_enraged = false;   

	short skill_target_x = 0;
	short skill_target_y = 0;
	std::chrono::steady_clock::time_point last_skill_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);

	void Reset() {		
		std::lock_guard<std::mutex> lock(sessionLock);
		if (socket != INVALID_SOCKET) {
			closesocket(socket);
			socket = INVALID_SOCKET;
		}
		prevRemainBytes = 0;
		memset(name, 0, sizeof(name));

		std::lock_guard<std::mutex> vlLock(viewLock);
		viewList.clear();

		playerNum++;	
		state.store(SessionState::FREE);

		if (L_ai != nullptr) {
			lua_close(L_ai);
			L_ai = nullptr;
		}
		is_enraged = false;
		is_god = false;
	}

	void SendPacket(void* packet) {
		unsigned char* p = reinterpret_cast<unsigned char*>(packet);
		IOContext* sendContext = new IOContext(IO_OP::SEND);
		memcpy(sendContext->buffer, p, p[0]);
		sendContext->wsabuf.len = p[0];

		DWORD sentBytes = 0;
		int ret = WSASend(socket, &sendContext->wsabuf, 1, &sentBytes, 0, &sendContext->overlapped, nullptr);

		if (ret == SOCKET_ERROR) {
			int err = WSAGetLastError();
			if (err != WSA_IO_PENDING) {
				delete sendContext;
			}
		}
	}

	
	unsigned long long GetUniqueId() {
		return (static_cast<unsigned long long>(playerNum) << 32) | sessionIndex;
	}
};


inline short GetRegionX(short x) {
	return std::max((short)0, std::min((short)(MAX_REGION_X - 1), (short)(x / REGION_SIZE)));
}

inline short GetRegionY(short y) {
	return std::max((short)0, std::min((short)(MAX_REGION_Y - 1), (short)(y / REGION_SIZE)));
}

void GetRespawnPosition(unsigned char level, short& out_x, short& out_y) {
	short offset_x = rand() % 100;
	short offset_y = rand() % 100;

	if (level <= 10) {
		out_x = offset_x;
		out_y = offset_y;
	}
	else if (level <= 20) {
		out_x = WORLD_WIDTH - 100 + offset_x;
		out_y = offset_y;
	}
	else if (level <= 30) {
		out_x = offset_x;
		out_y = WORLD_HEIGHT - 100 + offset_y;
	}
	else {
		out_x = WORLD_WIDTH - 100 + offset_x;
		out_y = WORLD_HEIGHT - 100 + offset_y;
	}

	if (out_x >= WORLD_WIDTH) out_x = WORLD_WIDTH - 1;
	if (out_y >= WORLD_HEIGHT) out_y = WORLD_HEIGHT - 1;
}

inline bool IsInView(short x1, short y1, short x2, short y2) {
	return (abs(x1 - x2) <= VIEW_RANGE) && (abs(y1 - y2) <= VIEW_RANGE);
}

std::unordered_set<int> GetNearbyObjects(short cur_x, short cur_y) {
	std::unordered_set<int> nearby_objs;
	short rx = GetRegionX(cur_x);
	short ry = GetRegionY(cur_y);

	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			short nx = rx + dx;
			short ny = ry + dy;

			if (nx < 0 || nx >= MAX_REGION_X || ny < 0 || ny >= MAX_REGION_Y) continue;

			std::shared_lock<std::shared_mutex> read_lock(g_regions[nx][ny].lock);
			for (int id : g_regions[nx][ny].objects) {
				nearby_objs.insert(id);
			}
		}
	}
	return nearby_objs;
}

int API_BossChat(lua_State* L);

int API_CastAoESkill(lua_State* L);

class IocpServer
{
private:
	HANDLE hIocp = INVALID_HANDLE_VALUE;
	SOCKET listenSocket = INVALID_SOCKET;
	std::vector<Session*> sessions;
	tbb::concurrent_queue<int> player_id_pool;
	std::vector<std::thread> workers;
	std::priority_queue<TimerEvent, std::vector<TimerEvent>, std::greater<TimerEvent>> timer_queue;
	std::mutex timer_mock;
	tbb::concurrent_queue<DbTask> db_queue;
	std::thread db_worker;
	struct DropItem {
		std::atomic<bool> active{ false };
		int item_id = 0;
		short x = 0;
		short y = 0;
	};
	DropItem g_items[5000];
	tbb::concurrent_queue<int> item_id_pool;

public:
	IocpServer() {
		for (int i = 0; i < 5000; ++i) {
			item_id_pool.push(2000000 + i);
		}

		sessions.resize(MAX_PLAYERS + NUM_NPCS, nullptr);
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			sessions[i] = new Session();
			sessions[i]->sessionIndex = i;
			player_id_pool.push(i);
		}

		lua_State* L = luaL_newstate();
		if (L != nullptr) {
			luaL_openlibs(L);

			if (luaL_dofile(L, "monster_spawn.lua") != LUA_OK) {
				std::cout << "[LUA Error] 파일 읽기 실패: " << lua_tostring(L, -1) << std::endl;
			}
			else {
				lua_getglobal(L, "MonsterSpawns");

				if (lua_istable(L, -1)) {
					lua_pushnil(L);

					int current_npc_id = NPC_ID_START; 

					while (lua_next(L, -2) != 0) {
						if (current_npc_id >= NPC_ID_START + NUM_NPCS) {
							lua_pop(L, 1);
							break;
						}

						lua_getfield(L, -1, "name");
						const char* m_name = lua_tostring(L, -1);
						lua_pop(L, 1);

						lua_getfield(L, -1, "monster_type");
						int m_type = (int)lua_tointeger(L, -1);
						lua_pop(L, 1);

						lua_getfield(L, -1, "ai_type");
						int m_ai = (int)lua_tointeger(L, -1);
						lua_pop(L, 1);

						lua_getfield(L, -1, "level");
						int m_level = (int)lua_tointeger(L, -1);
						lua_pop(L, 1);

						lua_getfield(L, -1, "x");
						int m_x = (int)lua_tointeger(L, -1);
						lua_pop(L, 1);

						lua_getfield(L, -1, "y");
						int m_y = (int)lua_tointeger(L, -1);
						lua_pop(L, 1);

						while (m_x < 0 || m_x >= WORLD_WIDTH || m_y < 0 || m_y >= WORLD_HEIGHT || g_wall[m_x][m_y]) {
							m_x = (rand() % (WORLD_WIDTH - 40)) + 20;
							m_y = (rand() % (WORLD_HEIGHT - 40)) + 20;
						}

						lua_getfield(L, -1, "hp");
						int m_hp = (int)lua_tointeger(L, -1);
						lua_pop(L, 1);

						lua_getfield(L, -1, "atk");
						int m_atk = (int)lua_tointeger(L, -1);
						lua_pop(L, 1);

						int real_index = MAX_PLAYERS + (current_npc_id - NPC_ID_START); 

						sessions[real_index] = new Session();
						Session* npc = sessions[real_index];

						npc->sessionIndex = real_index;
						npc->id = current_npc_id;

						strcpy_s(npc->name, m_name);
						npc->monster_type = static_cast<MonsterType>(m_type);
						npc->ai_type = static_cast<AiType>(m_ai);
						npc->level = m_level;
						npc->x = m_x;
						npc->y = m_y;
						npc->origin_x = m_x;
						npc->origin_y = m_y;
						npc->max_hp = m_hp;
						npc->hp = m_hp;
						npc->attack_power = m_atk;


						if (strncmp(m_name, "Boss_", 5) == 0) {
							npc->L_ai = luaL_newstate();
							luaL_openlibs(npc->L_ai);


							lua_register(npc->L_ai, "API_BossChat", API_BossChat);
							lua_register(npc->L_ai, "API_CastAoESkill", API_CastAoESkill);


							if (luaL_dofile(npc->L_ai, "boss_ai.lua") != LUA_OK) {
								
							}
						}


						short rx = GetRegionX(npc->x);
						short ry = GetRegionY(npc->y);
						{
							std::unique_lock<std::shared_mutex> wl(g_regions[rx][ry].lock);
							g_regions[rx][ry].objects.insert(current_npc_id);
						}


						npc->state.store(SessionState::INGAME);

						current_npc_id++;
						lua_pop(L, 1);
					}
				}
			}
			lua_close(L);
		}
	}

	bool Initialize() {
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
		
		hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);

		listenSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

		SOCKADDR_IN serverAddr{};
		serverAddr.sin_family = AF_INET;
		serverAddr.sin_port = htons(PORT);		
		serverAddr.sin_addr.s_addr = INADDR_ANY;

		if (::bind(listenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) return false;
		if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) return false;
		CreateIoCompletionPort((HANDLE)listenSocket, hIocp, 10000, 0);
		return true;
	}

	void Start() {
		RegisterAccept();
		workers.emplace_back(&IocpServer::TimerLoop, this);

		db_worker = std::thread(&IocpServer::DbWorkerLoop, this);

		int threadCount = std::thread::hardware_concurrency(); 
		for (int i = 0; i < threadCount; ++i) {
			workers.emplace_back(&IocpServer::WorkerLoop, this);
		}
	}

	void Join() {
		for (auto& th : workers) th.join();
		if (db_worker.joinable()) db_worker.join();
	}

	void ExecuteBossAoE(int boss_id, int damage) {
		Session* boss = GetSessionId(boss_id);
		if (!boss) return;

		S2C_SkillEffect eff = { sizeof(eff), S2C_SKILL_EFFECT, boss_id, boss->x, boss->y, 3 };

		auto near_objs = GetNearbyObjects(boss->x, boss->y);
		for (int target_id : near_objs) {
			if (IsPlayer(target_id)) {
				Session* player = GetSessionId(target_id);
				if (player && player->state.load() == SessionState::INGAME) {
					player->SendPacket(&eff);
				}
			}
		}

		boss->skill_target_x = boss->x;
		boss->skill_target_y = boss->y;
		boss->attack_power = damage; 
		AddTimerEvent(boss_id, EventType::EVENT_BOSS_ULT, 10000);
	}

	void BroadcastBossChat(int boss_id, const char* msg) {
		Session* boss = GetSessionId(boss_id);
		if (!boss) return;

		S2C_ChatMessage chatPacket;
		chatPacket.size = sizeof(S2C_ChatMessage);
		chatPacket.type = S2C_CHAT_MESSAGE;
		chatPacket.object_id = boss_id;
		chatPacket.chatType = 2;
		sprintf_s(chatPacket.message, "BOSS  :  이 구역의 지배자는 나다!!");

		auto near_objs = GetNearbyObjects(boss->x, boss->y);
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			Session* pSession = sessions[i];
			if (pSession && pSession->state.load() == SessionState::INGAME) {
				pSession->SendPacket(&chatPacket);
			}
		}
	}

	void ProcessBossUltimate(int boss_id) {
		Session* boss = GetSessionId(boss_id);
		if (!boss || boss->state.load() != SessionState::INGAME) return;

		S2C_SkillEffect eff = { sizeof(eff), S2C_SKILL_EFFECT, boss_id, boss->skill_target_x, boss->skill_target_y, 2 };

		auto near_objs = GetNearbyObjects(boss->skill_target_x, boss->skill_target_y);
		for (int target_id : near_objs) {
			if (IsPlayer(target_id)) {
				Session* player = GetSessionId(target_id);
				if (player && player->state.load() == SessionState::INGAME) {
					int dist = abs(boss->skill_target_x - player->x) + abs(boss->skill_target_y - player->y);

					if (dist <= 15) { 
						player->SendPacket(&eff); 
						HandleDamage(boss_id, target_id, 2000); 

						int push_x = player->x;
						int push_y = player->y;
						if (player->x < boss->skill_target_x) push_x -= 5; else if (player->x > boss->skill_target_x) push_x += 5;
						if (player->y < boss->skill_target_y) push_y -= 5; else if (player->y > boss->skill_target_y) push_y += 5;

						if (push_x >= 0 && push_x < WORLD_WIDTH && push_y >= 0 && push_y < WORLD_HEIGHT && !g_wall[push_x][push_y]) {
							MoveObject(target_id, push_x, push_y);
						}
					}
				}
			}
		}
	}

private:
	void AddTimerEvent(int obj_id, EventType type, int delay_ms) {
		TimerEvent ev;
		ev.object_id = obj_id;
		ev.type = type;
		ev.exec_time = std::chrono::system_clock::now() + std::chrono::milliseconds(delay_ms);

		{
			std::lock_guard<std::mutex> lock(timer_mock);
			timer_queue.push(ev);
		}
	}

	Session* GetSessionId(int id) {
		if (id < MAX_PLAYERS) {
			return sessions[id];		
		}
		else if (id >= NPC_ID_START && id < NPC_ID_START + NUM_NPCS) {
			return sessions[MAX_PLAYERS + (id - NPC_ID_START)];			
		}
		return nullptr;
	}

	bool IsPlayer(int id) { return id < MAX_PLAYERS; }

	void SendAddObject(int to_id, int target_id) {
		Session* to = GetSessionId(to_id);
		Session* target = GetSessionId(target_id);
		if (!to || !target || to->state.load() != SessionState::INGAME || target->state.load() != SessionState::INGAME) return;

		S2C_AddObject packet;
		packet.size = sizeof(S2C_AddObject);
		packet.type = S2C_ADD_OBJECT;
		packet.object_id = target->id;
		packet.visual_id = IsPlayer(target_id) ? 0 : 1;
		strcpy_s(packet.obj_name, target->name);
		packet.x = target->x;
		packet.y = target->y;
		packet.direction = target->direction;
		packet.hp = target->hp;
		packet.max_hp = target->max_hp;
		packet.exp = target->exp;
		packet.level = target->level;
		to->SendPacket(&packet);
	}

	void SendRemoveObject(int to_id, int target_id) {
		Session* to = GetSessionId(to_id);
		if (!to || to->state.load() != SessionState::INGAME) return;

		S2C_RemoveObject packet;
		packet.size = sizeof(S2C_RemoveObject);
		packet.type = S2C_REMOVE_OBJECT;
		packet.object_id = target_id;
		to->SendPacket(&packet);
	}

	void SendMoveObject(int to_id, int target_id, short nx, short ny, unsigned int move_time = 0) {
		Session* to = GetSessionId(to_id);
		if (!to || to->state.load() != SessionState::INGAME) return;

		S2C_MoveObject packet;
		packet.size = sizeof(S2C_MoveObject);
		packet.type = S2C_MOVE_OBJECT;
		packet.object_id = target_id;
		packet.x = nx;
		packet.y = ny;
		packet.move_time = move_time;
		to->SendPacket(&packet);
	}

	void MoveObject(int id, short nx, short ny, unsigned int move_time = 0) {
		Session* obj = GetSessionId(id);
		if (!obj || obj->state.load() != SessionState::INGAME) return;

		short old_x = obj->x;
		short old_y = obj->y;

		{
			std::lock_guard<std::mutex> lock(obj->sessionLock);
			obj->x = nx;
			obj->y = ny;
		}

		short old_rx = GetRegionX(old_x);
		short old_ry = GetRegionY(old_y);
		short new_rx = GetRegionX(nx);
		short new_ry = GetRegionY(ny);

		if (old_rx != new_rx || old_ry != new_ry) {
			
			{
				std::unique_lock<std::shared_mutex> wl(g_regions[old_rx][old_ry].lock);
				g_regions[old_rx][old_ry].objects.erase(id);
			}
			{
				std::unique_lock<std::shared_mutex> wl(g_regions[new_rx][new_ry].lock);
				g_regions[new_rx][new_ry].objects.insert(id);
			}

			Session* obj = GetSessionId(id);
			if (IsPlayer(id) && obj && strncmp(obj->name, "Dummy_", 6) != 0) {
				DbTask task;
				task.type = DbTaskType::SAVE_PLAYER;
				strcpy_s(task.username, obj->name);

				
				task.level = obj->level;
				task.exp = obj->exp;
				task.hp = obj->hp;
				task.x = nx; 
				task.y = ny; 

				db_queue.push(task);
			}
		}

		if (IsPlayer(id)) {
			
			std::unordered_set<int> old_view;
			{
				std::lock_guard<std::mutex> vl(obj->viewLock);
				old_view = obj->viewList;
			}

			
			std::unordered_set<int> new_view;
			auto near_objs = GetNearbyObjects(nx, ny);
			for (int n_id : near_objs) {
				if (n_id == id) continue;

				if (n_id >= 2000000) {
					
					int idx = n_id - 2000000;
					if (g_items[idx].active.load() && IsInView(nx, ny, g_items[idx].x, g_items[idx].y)) {
						new_view.insert(n_id);
					}
				}
				else {
					
					Session* target = GetSessionId(n_id);
					if (target && target->state.load() == SessionState::INGAME && IsInView(nx, ny, target->x, target->y)) {
						new_view.insert(n_id);
					}
				}
			}

			for (int n_id : new_view) {
				if (old_view.count(n_id) == 0) {		
					if (n_id >= 2000000) {
					
						int idx = n_id - 2000000;
						if (g_items[idx].active.load()) {
							S2C_AddItem itemPkt = { sizeof(itemPkt), S2C_ADD_ITEM, n_id, g_items[idx].x, g_items[idx].y, 0 };
							obj->SendPacket(&itemPkt);
						}
					}
					else if (IsPlayer(n_id)) {
						SendAddObject(id, n_id);
						SendAddObject(n_id, id);
					}
					else {
						SendAddObject(id, n_id);
						Session* npc = GetSessionId(n_id);
						if (npc && npc->viewers_count.fetch_add(1) == 0) WakeUpNpc(n_id);
					}
				}
				else {		
					if (n_id < 2000000 && IsPlayer(n_id)) SendMoveObject(n_id, id, nx, ny, move_time);
				}
			}

			
			for (int o_id : old_view) {
				if (new_view.count(o_id) == 0) {
					SendRemoveObject(id, o_id);
					if (o_id < 2000000) { 
						if (IsPlayer(o_id)) SendRemoveObject(o_id, id);
						else {
							Session* npc = GetSessionId(o_id);
							if (npc) npc->viewers_count.fetch_sub(1);
						}
					}
				}
			}

			{
				std::lock_guard<std::mutex> vl(obj->viewLock);
				obj->viewList = new_view;
			}

			SendMoveObject(id, id, nx, ny, move_time);
		}
		else {
			
			S2C_MoveObject packet = { sizeof(S2C_MoveObject), S2C_MOVE_OBJECT, id, nx, ny, move_time };
			BroadcastToViewers(id, &packet);
		}
	}

	
	void BroadcastToViewers(int my_id, void* packet) {
		Session* me = GetSessionId(my_id);
		if (!me) return;

		auto near_objs = GetNearbyObjects(me->x, me->y);
		for (int viewer : near_objs) {
			if (IsPlayer(viewer)) {
				Session* vSession = GetSessionId(viewer);
				
				if (vSession && vSession->state.load() == SessionState::INGAME && IsInView(me->x, me->y, vSession->x, vSession->y)) {
					vSession->SendPacket(packet);
				}
			}
		}
	}

	
	void KillObject(int id) {
		Session* target = GetSessionId(id);
		if (!target) return;
		target->state.store(SessionState::DEAD);


		short rx = GetRegionX(target->x);
		short ry = GetRegionY(target->y);
		{
			std::unique_lock<std::shared_mutex> wl(g_regions[rx][ry].lock);
			g_regions[rx][ry].objects.erase(id);
		}


		auto near_objs = GetNearbyObjects(target->x, target->y);
		for (int viewer : near_objs) {
			if (IsPlayer(viewer)) {
				Session* vSession = GetSessionId(viewer);
				if (vSession && vSession->state.load() == SessionState::INGAME && IsInView(target->x, target->y, vSession->x, vSession->y)) {
					SendRemoveObject(viewer, id);

					{
					std::lock_guard<std::mutex> vl(vSession->viewLock);
					vSession->viewList.erase(id);
					}
				}
			}
		}

		{
			std::lock_guard<std::mutex> vl(target->viewLock);
			if (IsPlayer(id)) {
				for (int v_id : target->viewList) {
					if (!IsPlayer(v_id)) {
						Session* npc = GetSessionId(v_id);
						if (npc) npc->viewers_count.fetch_sub(1);
					}
				}
			}
			else {
				target->viewers_count.store(0);
			}
			target->viewList.clear();
		}

		
	}

	void TimerLoop() {
		while (true) {
			std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
			TimerEvent top_event;
			bool has_event = false;
			{
				std::lock_guard<std::mutex> lock(timer_mock);
				if (!timer_queue.empty()) {
					if (timer_queue.top().exec_time <= now) {

						top_event = timer_queue.top();
						timer_queue.pop();
						has_event = true;
					}
				}
			}


			if (has_event) {
				
				IOContext* aiCtx = new IOContext(IO_OP::DO_AI);
				PostQueuedCompletionStatus(hIocp, static_cast<DWORD>(top_event.type), top_event.object_id, &aiCtx->overlapped);
			}
			else {

				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
	}


	void ProcessNpcMove(int npc_id) {
		Session* npc = GetSessionId(npc_id);
		if (!npc || npc->state.load() != SessionState::INGAME) return;

		int move_delay = 500;
		if (strncmp(npc->name, "Boss_", 5) == 0) move_delay = 1000;

		short nx = npc->x;
		short ny = npc->y;
		bool hasAggroTarget = false;
		short targetX = -1, targetY = -1;


		if (npc->target_id != -1) {
			Session* p = GetSessionId(npc->target_id);
			if (p && p->state.load() == SessionState::INGAME && IsInView(npc->x, npc->y, p->x, p->y)) {
				targetX = p->x;
				targetY = p->y;
				hasAggroTarget = true;
			}
			else {
				npc->target_id = -1;		
			}
		}


		if (!hasAggroTarget && npc->ai_type == AiType::ROAMING_AGGRO) {
			int closestDist = 9999;
			auto near_objs = GetNearbyObjects(npc->x, npc->y);
			for (int obj_id : near_objs) {
				if (IsPlayer(obj_id)) {
					Session* player = GetSessionId(obj_id);
					if (player && player->state.load() == SessionState::INGAME && IsInView(npc->x, npc->y, player->x, player->y)) {
						int dist = abs(npc->x - player->x) + abs(npc->y - player->y);
						if (dist <= 5 && dist < closestDist) {
							hasAggroTarget = true;
							targetX = player->x;
							targetY = player->y;
							closestDist = dist;
							npc->target_id = player->id;	
						}
					}
				}
			}
		}
		
		else if (npc->ai_type == AiType::FIXED_PEACE){
			if (npc->target_id != -1) {		
				Session* p = GetSessionId(npc->target_id);
				if (p && p->state.load() == SessionState::INGAME && IsInView(npc->x, npc->y, p->x, p->y)) {
					targetX = p->x;
					targetY = p->y;
					hasAggroTarget = true;
				}
				else {
					npc->target_id = -1;		
				}
			}
		}


		if (hasAggroTarget) {
			
			int dist = abs(npc->x - targetX) + abs(npc->y - targetY);

			if (strncmp(npc->name, "Boss_", 5) == 0 && dist <= 4) {
				auto now = std::chrono::steady_clock::now();
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - npc->last_skill_time).count() > 5000) {
					npc->last_skill_time = now;
					npc->skill_target_x = targetX;
					npc->skill_target_y = targetY;

					
					S2C_SkillEffect eff = { sizeof(eff), S2C_SKILL_EFFECT, npc_id, targetX, targetY, 0 };
					BroadcastToViewers(npc_id, &eff);

					
					AddTimerEvent(npc_id, EventType::EVENT_BOSS_SKILL, 1500);

					
					AddTimerEvent(npc_id, EventType::EVENT_MOVE, move_delay);
					return;
				}
			}

			
			int attack_range = 1;

			if (dist <= attack_range) {
				
				nx = npc->x;
				ny = npc->y;

				
				S2C_Action actionPacket = { sizeof(actionPacket), S2C_ACTION, npc->id, ActionType::ACTION_ATTACK };
				BroadcastToViewers(npc->id, &actionPacket);

				HandleDamage(npc_id, npc->target_id, npc->attack_power);
			}
			else {
				
				short nextX, nextY;
				if (FindNextStepAStar(npc->x, npc->y, targetX, targetY, nextX, nextY)) {
					nx = nextX;
					ny = nextY;
				}
			}
		}
		else {		
			if (npc->ai_type == AiType::ROAMING_AGGRO) {
				
				char dir = rand() % 4;
				short temp_nx = nx, temp_ny = ny;
				if (dir == 0) temp_ny -= 1;		
				else if (dir == 1) temp_nx += 1;
				else if (dir == 2) temp_ny += 1;
				else if (dir == 3) temp_nx -= 1;

				if (abs(temp_nx - npc->origin_x) <= 10 && abs(temp_ny - npc->origin_y) <= 10) {
					nx = temp_nx;
					ny = temp_ny;
				}
			}
			else if(npc->ai_type == AiType::FIXED_PEACE) {
				
				nx = npc->x;
				ny = npc->y;
			}
		}


		if (nx >= 0 && nx < WORLD_WIDTH && ny >= 0 && ny < WORLD_HEIGHT) {
			if (false == g_wall[nx][ny]) {
				if (nx != npc->x || ny != npc->y) {
					if (nx < npc->x) npc->direction = 2;
					else if (nx > npc->x) npc->direction = 3;

					MoveObject(npc_id, nx, ny);
				}
			}
		}

		if (npc->viewers_count.load() > 0) {
			AddTimerEvent(npc_id, EventType::EVENT_MOVE, move_delay);
		}
		else {
			npc->is_active.store(false);
		}

		
	}

	void ProcessNpcRespawn(int npc_id) {
		Session* npc = GetSessionId(npc_id);
		if (!npc) return;

		std::lock_guard<std::mutex> lock(npc->sessionLock);
		npc->hp = npc->max_hp;

		short spawn_x, spawn_y;
		do {
			spawn_x = (rand() % 1800) + 100;
			spawn_y = (rand() % 1800) + 100;
		} while (g_wall[spawn_x][spawn_y]);

		npc->x = spawn_x;
		npc->y = spawn_y;
		npc->state.store(SessionState::INGAME);


		short rx = GetRegionX(npc->x);
		short ry = GetRegionY(npc->y);
		{
			std::unique_lock<std::shared_mutex> wl(g_regions[rx][ry].lock);
			g_regions[rx][ry].objects.insert(npc_id);
		}

		auto near_objs = GetNearbyObjects(npc->x, npc->y);
		for (int n_id : near_objs) {
			if (n_id == npc_id) continue;
			Session* target = GetSessionId(n_id);
			if (target && target->state.load() == SessionState::INGAME && IsInView(npc->x, npc->y, target->x, target->y)) {
	
				{
					std::lock_guard<std::mutex> vl(npc->viewLock);
					npc->viewList.insert(n_id);
				}
				if (IsPlayer(n_id)) {
					{
						std::lock_guard<std::mutex> vl(target->viewLock);
						target->viewList.insert(npc_id);
					}
					SendAddObject(n_id, npc_id);
				}
			}
			
		}
		AddTimerEvent(npc_id, EventType::EVENT_MOVE, 500);
	}

	void HandleDamage(int attacker_id, int victim_id, int damage) {
		Session* attacker = GetSessionId(attacker_id);
		Session* victim = GetSessionId(victim_id);

		if (!attacker || !victim || attacker->state.load() != SessionState::INGAME || victim->state.load() != SessionState::INGAME) return;
	
		if (IsPlayer(victim_id) && victim->is_god) return;


		if (IsPlayer(victim_id)) {
			auto now = std::chrono::steady_clock::now();

			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - victim->last_hit_time).count() < 1000) {
				return;
			}
			victim->last_hit_time = now;
		}

		bool is_victim_dead = false;
		int exp_gained = 0;
		long long required_exp = 100LL * (1LL << (attacker->level - 1));

		char sysMsg[256];
		if (IsPlayer(attacker_id)) {
			sprintf_s(sysMsg, "%s가 %s를 공격하여 %d의 데미지를 입혔습니다.", attacker->name, victim->name, damage);
			SendSystemMessage(attacker_id, sysMsg);
		}
		else if (IsPlayer(victim_id)) {
			sprintf_s(sysMsg, "%s의 공격으로 %d의 데미지를 입었습니다.", attacker->name, damage);
			SendSystemMessage(victim_id, sysMsg);
		}



		{
			std::lock_guard<std::mutex> lock(victim->sessionLock);
			victim->hp -= damage;


			if (victim->L_ai != nullptr && !victim->is_enraged && victim->hp > 0) {
				lua_getglobal(victim->L_ai, "OnDamageTaken");
				lua_pushinteger(victim->L_ai, victim->id); 
				lua_pushnumber(victim->L_ai, victim->hp);
				lua_pushnumber(victim->L_ai, victim->max_hp);

				
				if (lua_pcall(victim->L_ai, 3, 1, 0) == LUA_OK) {
					int is_enraged_result = (int)lua_tointeger(victim->L_ai, -1);
					if (is_enraged_result == 1) {
						victim->is_enraged = true;
					}
					lua_pop(victim->L_ai, 1);
				}
			}

			if (!IsPlayer(victim_id) && victim->hp > 0 && victim->target_id == -1) {
				victim->target_id = attacker_id;
			}

			if (victim->hp <= 0) {
				victim->hp = 0;
				is_victim_dead = true;
				victim->is_recovering = false;

				if (IsPlayer(victim_id)) {

					victim->hp = victim->max_hp;
					victim->exp /= 2;
					if (!IsPlayer(attacker_id)) attacker->target_id = -1;	
				}
				else {

					victim->state.store(SessionState::DEAD);
					exp_gained = (victim->level * victim->level * 2);

					if (victim->ai_type == AiType::ROAMING_AGGRO) {
						exp_gained *= 2;
					}

					if (rand() % 100 < 30) {
						int new_item_id;
						if (item_id_pool.try_pop(new_item_id)) {
							int idx = new_item_id - 2000000; 
							g_items[idx].item_id = new_item_id;
							g_items[idx].x = victim->x;
							g_items[idx].y = victim->y;
							g_items[idx].active.store(true);


							short rx = GetRegionX(victim->x);
							short ry = GetRegionY(victim->y);
							{
								std::unique_lock<std::shared_mutex> wl(g_regions[rx][ry].lock);
								g_regions[rx][ry].objects.insert(new_item_id);
							}


							S2C_AddItem itemPkt = { sizeof(itemPkt), S2C_ADD_ITEM, new_item_id, victim->x, victim->y, 0 };
							BroadcastToViewers(victim_id, &itemPkt);
						}
					}
				}
			}
			else {
				if (victim->hp < victim->max_hp && !victim->is_recovering) {
					victim->is_recovering = true;
					AddTimerEvent(victim_id, EventType::EVENT_HP_RECOVERY, 5000);
				}
			}
		}


		if (is_victim_dead) {
			if (IsPlayer(victim_id)) {
				short respqwn_x, respawn_y;
				GetRespawnPosition(victim->level, respqwn_x, respawn_y);
				MoveObject(victim_id, respqwn_x, respawn_y);

				S2C_StatusChange statusPacket = { sizeof(statusPacket), S2C_STATUS_CHANGE, victim_id, victim->hp, victim->max_hp, victim->exp, victim->level };
				victim->SendPacket(&statusPacket);
				BroadcastToViewers(victim_id, &statusPacket);
			}
			else {

				S2C_Action deadPacket = { sizeof(deadPacket), S2C_ACTION, victim_id, ActionType::ACTION_DEAD };
				BroadcastToViewers(victim_id, &deadPacket);

				S2C_StatusChange statusPacket = { sizeof(statusPacket), S2C_STATUS_CHANGE, victim_id, victim->hp, victim->max_hp, victim->exp, victim->level };
				BroadcastToViewers(victim_id, &statusPacket);

				AddTimerEvent(victim_id, EventType::EVENT_DESPAWN, 1000);
				AddTimerEvent(victim_id, EventType::EVENT_RESPAWN, 30000);


				if (IsPlayer(attacker_id) && exp_gained > 0) {
					bool is_leveled_up = false;
					int current_level, current_hp, current_max_hp;
					long long current_exp;
					short current_x, current_y;
					char current_name[MAX_NAME_LEN];

					{
						std::lock_guard<std::mutex> myLock(attacker->sessionLock);
						attacker->exp += exp_gained;

						while (attacker->exp >= required_exp) {
							attacker->exp -= required_exp;
							attacker->level++;
							attacker->max_hp += 50;
							attacker->attack_power += 30;
							attacker->hp = attacker->max_hp;
							is_leveled_up = true;

							required_exp = 100LL * (1LL << (attacker->level - 1));
						}

						
						current_level = attacker->level;
						current_hp = attacker->hp;
						current_max_hp = attacker->max_hp;
						current_exp = attacker->exp;
						current_x = attacker->x;
						current_y = attacker->y;
						strcpy_s(current_name, attacker->name);
					}
					char killMsg[256];
					sprintf_s(killMsg, "%s를 처치하여 %d의 경험치를 얻었습니다.", victim->name, exp_gained);
					SendSystemMessage(attacker_id, killMsg);

					if (is_leveled_up && strncmp(current_name, "Dummy_", 6) != 0) {
						DbTask task;
						task.type = DbTaskType::SAVE_PLAYER;
						strcpy_s(task.username, current_name);
						task.level = current_level;
						task.exp = current_exp;
						task.hp = current_hp;
						task.x = current_x;
						task.y = current_y;

						db_queue.push(task);
					}


					S2C_StatusChange myStatus = { sizeof(myStatus), S2C_STATUS_CHANGE, attacker_id, current_hp, current_max_hp, current_exp, current_level };
					attacker->SendPacket(&myStatus);
					BroadcastToViewers(attacker_id, &myStatus);
				}
			}
		}
		else {
			S2C_Action hitPacket = { sizeof(hitPacket), S2C_ACTION, victim_id, ActionType::ACTION_HIT };
			BroadcastToViewers(victim_id, &hitPacket);

			S2C_StatusChange statusPacket = { sizeof(statusPacket), S2C_STATUS_CHANGE, victim_id, victim->hp, victim->max_hp, victim->exp, victim->level };
			if (IsPlayer(victim_id)) victim->SendPacket(&statusPacket);
			BroadcastToViewers(victim_id, &statusPacket);
		}
	}

	void RegisterAccept() {

		SOCKET clientSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

		IOContext* acceptCtx = new IOContext(IO_OP::ACCEPT);
		acceptCtx->acceptSocket = clientSocket;

		AcceptEx(listenSocket, clientSocket, acceptCtx->buffer, 0, 
			sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, nullptr, &acceptCtx->overlapped);
	}

	int AllocateSession(SOCKET clientSocket) {
		int new_id;

		if (player_id_pool.try_pop(new_id)) {
			Session* new_session = GetSessionId(new_id);
			new_session->id = new_id;
			new_session->socket = clientSocket;
			new_session->state = SessionState::CONNECTED;
			return new_id;
		}
		return -1;
	}


	struct AStarNode {
		short x, y;
		int g, h, f;
		bool operator>(const AStarNode& other) const { return f > other.f; }
	};


	bool FindNextStepAStar(short startX, short startY, short targetX, short targetY, short& outX, short& outY) {
		std::priority_queue<AStarNode, std::vector<AStarNode>, std::greater<AStarNode>> pq;

		std::unordered_map<int, std::pair<short, short>> cameFrom;
		std::unordered_map<int, int> costSoFar;

		int startKey = startY * WORLD_WIDTH + startX;
		pq.push({ startX, startY, 0, abs(startX - targetX) + abs(startY - targetY), 0 });
		cameFrom[startKey] = { startX, startY };
		costSoFar[startKey] = 0;

		short dx[] = { 0, 0, -1, 1 };
		short dy[] = { -1, 1, 0, 0 };

		bool found = false;
		int iterations = 0;

		while (!pq.empty()) {
			if (iterations++ > 100) break;	
		
			AStarNode current = pq.top();
			pq.pop();

			if (current.x == targetX && current.y == targetY) {
				found = true;
				break;
			}

			for (int i = 0; i < 4; ++i) {
				short nx = current.x + dx[i];
				short ny = current.y + dy[i];


				if (nx < 0 || nx >= WORLD_WIDTH || ny < 0 || ny >= WORLD_HEIGHT) continue;
				if (g_wall[nx][ny]) continue;

				int newCost = costSoFar[current.y * WORLD_WIDTH + current.x] + 1;
				int nextKey = ny * WORLD_WIDTH + nx;

				if (costSoFar.find(nextKey) == costSoFar.end() || newCost < costSoFar[nextKey]) {
					costSoFar[nextKey] = newCost;
					int heuristic = abs(nx - targetX) + abs(ny - targetY);
					pq.push({ nx, ny, newCost, heuristic, newCost + heuristic });
					cameFrom[nextKey] = { current.x, current.y };
				}
			}
		}

		if (!found) return false;	


		short currX = targetX;
		short currY = targetY;
		while (cameFrom[currY * WORLD_WIDTH + currX].first != startX || cameFrom[currY * WORLD_WIDTH + currX].second != startY) {
			auto prev = cameFrom[currY * WORLD_WIDTH + currX];
			currX = prev.first;
			currY = prev.second;
		}

		outX = currX;
		outY = currY;
		return true;
	}

	void WakeUpNpc(int npc_id) {
		Session* npc = GetSessionId(npc_id);
		if (!npc) return;

		bool expected = false;
		if (npc->is_active.compare_exchange_strong(expected, true)) {
			AddTimerEvent(npc_id, EventType::EVENT_MOVE, 500);
		}
	}

	void ProcessHpRecovery(int player_id) {
		Session* player = GetSessionId(player_id);
		if (!player || player->state.load() != SessionState::INGAME) return;

		bool is_healed = false;
		bool keep_recovering = false;

		{
			std::lock_guard<std::mutex> lock(player->sessionLock);
			
			if (player->is_recovering && player->hp > 0 && player->hp < player->max_hp) {
				int recovery_amount = player->max_hp / 10;
				player->hp += recovery_amount;
				is_healed = true;

				if (player->hp > player->max_hp) {
					player->hp = player->max_hp;
					player->is_recovering = false;
				}
				else {
					keep_recovering = true;
				}
			}
		}

		if (is_healed) {
			S2C_StatusChange statusPacket = { sizeof(statusPacket), S2C_STATUS_CHANGE, player_id, player->hp, player->max_hp, player->exp, player->level };
			player->SendPacket(&statusPacket);
			BroadcastToViewers(player_id, &statusPacket);
		}

		if (keep_recovering) {
			AddTimerEvent(player_id, EventType::EVENT_HP_RECOVERY, 5000);
		}
	}

	void SendSystemMessage(int player_id, const char* msg) {
		Session* player = GetSessionId(player_id);
		if (!player || player->state.load() != SessionState::INGAME) return;

		S2C_ChatMessage chatPacket;
		chatPacket.size = sizeof(S2C_ChatMessage);
		chatPacket.type = S2C_CHAT_MESSAGE;
		chatPacket.object_id = player_id;
		chatPacket.chatType = 2; 
		strcpy_s(chatPacket.message, msg);

		player->SendPacket(&chatPacket);
	}

	void DbWorkerLoop() {
		SQLHENV hEnv = SQL_NULL_HENV;
		SQLHDBC hDbc = SQL_NULL_HDBC;
		SQLRETURN retcode;


		SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
		SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);

		SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);

		SQLWCHAR* connStr = (SQLWCHAR*)L"DRIVER={ODBC Driver 17 for SQL Server};SERVER=localhost\\SQLEXPRESS;DATABASE=GameDB;Trusted_Connection=yes;";
		SQLWCHAR outStr[1024];
		SQLSMALLINT outStrLen;

		retcode = SQLDriverConnect(hDbc, NULL, connStr, SQL_NTS, outStr, 1024, &outStrLen, SQL_DRIVER_NOPROMPT);

		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {

		}
		else {
			std::cout << "[DB Error] Failed to connect to MSSQL!" << std::endl;

			
			SQLWCHAR sqlState[6], msg[1024];
			SQLINTEGER nativeError;
			SQLSMALLINT msgLen;

			if (SQLGetDiagRec(SQL_HANDLE_DBC, hDbc, 1, sqlState, &nativeError, msg, 1024, &msgLen) == SQL_SUCCESS) {
				std::wcout << L"SQLState: " << sqlState << std::endl;
				std::wcout << L"Message: " << msg << std::endl;
			}
			SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
			SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
			return;
		}

		while (true) {
			DbTask task;
			if (db_queue.try_pop(task)) {

				if (task.type == DbTaskType::LOGIN_CHECK) {
					SQLHSTMT hStmt = SQL_NULL_HSTMT;
					SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

					char query[256];
					sprintf_s(query, "SELECT level, exp, hp, x, y FROM users WHERE name = '%s'", task.username);

					int len = MultiByteToWideChar(CP_ACP, 0, query, -1, NULL, 0);
					std::wstring wQuery(len, 0);
					MultiByteToWideChar(CP_ACP, 0, query, -1, &wQuery[0], len);

					retcode = SQLExecDirect(hStmt, (SQLWCHAR*)wQuery.c_str(), SQL_NTS);

					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						IOContext* dbCtx = new IOContext(IO_OP::DB_RESULT_LOGIN);

						if (SQLFetch(hStmt) == SQL_SUCCESS) {
							SQLGetData(hStmt, 1, SQL_C_LONG, &dbCtx->db_level, 0, NULL);
							SQLGetData(hStmt, 2, SQL_C_SBIGINT, &dbCtx->db_exp, 0, NULL);
							SQLGetData(hStmt, 3, SQL_C_LONG, &dbCtx->db_hp, 0, NULL);

							int temp_x, temp_y;
							SQLGetData(hStmt, 4, SQL_C_LONG, &temp_x, 0, NULL);
							SQLGetData(hStmt, 5, SQL_C_LONG, &temp_y, 0, NULL);
							dbCtx->db_x = (short)temp_x;
							dbCtx->db_y = (short)temp_y;
						}
						else {
							SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
							SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

							char insertQuery[256];

							sprintf_s(insertQuery, "INSERT INTO users (name, level, exp, hp, x, y) VALUES ('%s', 1, 0, 100, 100, 100)", task.username);

							int len2 = MultiByteToWideChar(CP_ACP, 0, insertQuery, -1, NULL, 0);
							std::wstring wInsert(len2, 0);
							MultiByteToWideChar(CP_ACP, 0, insertQuery, -1, &wInsert[0], len2);

							SQLExecDirect(hStmt, (SQLWCHAR*)wInsert.c_str(), SQL_NTS);


							dbCtx->db_level = 1;  dbCtx->db_exp = 0;  dbCtx->db_hp = 100;
							dbCtx->db_x = 100;     dbCtx->db_y = 100;
						}
						PostQueuedCompletionStatus(hIocp, 1, task.session_id, &dbCtx->overlapped);
					}
					SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
				}
				else if (task.type == DbTaskType::SAVE_PLAYER) {
					SQLHSTMT hStmt = SQL_NULL_HSTMT;
					SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

					char updateQuery[512];
					sprintf_s(updateQuery, "UPDATE users SET level=%d, exp=%lld, hp=%d, x=%d, y=%d WHERE name='%s'",
						task.level, task.exp, task.hp, task.x, task.y, task.username);

					int len = MultiByteToWideChar(CP_ACP, 0, updateQuery, -1, NULL, 0);
					std::wstring wUpdate(len, 0);
					MultiByteToWideChar(CP_ACP, 0, updateQuery, -1, &wUpdate[0], len);

					SQLExecDirect(hStmt, (SQLWCHAR*)wUpdate.c_str(), SQL_NTS);

					SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
				}
			}
			else {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
	}

	void ProcessAutoSave(int player_id) {
		Session* player = GetSessionId(player_id);

		
		if (!player || player->state.load() != SessionState::INGAME) return;

		if (strncmp(player->name, "Dummy_", 6) != 0) {
			DbTask task;
			task.type = DbTaskType::SAVE_PLAYER;
			strcpy_s(task.username, player->name);


			{
				std::lock_guard<std::mutex> lock(player->sessionLock);
				task.level = player->level;
				task.exp = player->exp;
				task.hp = player->hp;
				task.x = player->x;
				task.y = player->y;
			}
			db_queue.push(task);
			
		}

		AddTimerEvent(player_id, EventType::EVENT_AUTO_SAVE, 60000);
	}

	void ProcessBossSkillFire(int boss_id) {
		Session* boss = GetSessionId(boss_id);
		if (!boss || boss->state.load() != SessionState::INGAME) return;

		short sx = boss->skill_target_x;
		short sy = boss->skill_target_y;


		S2C_SkillEffect eff = { sizeof(eff), S2C_SKILL_EFFECT, boss_id, sx, sy, 1 };
		BroadcastToViewers(boss_id, &eff);

		
		auto near_objs = GetNearbyObjects(sx, sy);
		for (int p_id : near_objs) {
			if (IsPlayer(p_id)) {
				Session* p = GetSessionId(p_id);
				if (p && p->state.load() == SessionState::INGAME) {
					int dist = abs(p->x - sx) + abs(p->y - sy);
					if (dist <= 1) {
						
						HandleDamage(boss_id, p_id, boss->attack_power * 2);

						
						int push_x = p->x;
						int push_y = p->y;

						if (p->x < sx) push_x -= 1; 
						else if (p->x > sx) push_x += 1;

						if (p->y < sy) push_y -= 1;
						else if (p->y > sy) push_y += 1;


						if (push_x >= 0 && push_x < WORLD_WIDTH && push_y >= 0 && push_y < WORLD_HEIGHT && !g_wall[push_x][push_y]) {
							MoveObject(p_id, push_x, push_y);
						}
					}
				}
			}
		}
	}

	void WorkerLoop() {
		while (true) {
			DWORD bytesTransferred = 0;
			ULONG_PTR completionKey = 0;
			WSAOVERLAPPED* overlapped = nullptr;

			BOOL result = GetQueuedCompletionStatus(hIocp, &bytesTransferred, &completionKey, &overlapped, INFINITE);
			IOContext* ioCtx = reinterpret_cast<IOContext*>(overlapped);

			if (!result || (bytesTransferred == 0 && ioCtx->opType != IO_OP::ACCEPT && ioCtx->opType != IO_OP::DO_AI)) {
				if (completionKey != 10000) {
					Disconnect(static_cast<int>(completionKey));
					
				}
				if (ioCtx->opType == IO_OP::SEND) delete ioCtx;		
				continue;	
			}

			int sessionId = static_cast<int>(completionKey);

			switch (ioCtx->opType) {
			case IO_OP::ACCEPT: {
				SOCKET newClientSocket = ioCtx->acceptSocket;
				
				int allocatedId = AllocateSession(newClientSocket);

				if (allocatedId != -1) {	
					CreateIoCompletionPort((HANDLE)newClientSocket, hIocp, allocatedId, 0);

					Session* session = sessions[allocatedId];
					if (session != nullptr) {
						DWORD flags = 0;
						ZeroMemory(&session->recvContext.overlapped, sizeof(WSAOVERLAPPED));
						WSARecv(newClientSocket, &session->recvContext.wsabuf, 1, nullptr, &flags,
							&session->recvContext.overlapped, nullptr);
					}
				}
				else {
					closesocket(newClientSocket);
				}

				RegisterAccept();
				delete ioCtx;		
				break;
			}
			case IO_OP::RECV: {
				ProcessReceive(sessionId, bytesTransferred);
				break;
			}
			case IO_OP::SEND: {
				delete ioCtx;
				break;
			}
			case IO_OP::DO_AI: {
				EventType type = static_cast<EventType>(bytesTransferred);
				int obj_id = static_cast<int>(completionKey);

				if (type == EventType::EVENT_MOVE) ProcessNpcMove(obj_id);
				else if (type == EventType::EVENT_RESPAWN) ProcessNpcRespawn(obj_id);
				else if (type == EventType::EVENT_DESPAWN) KillObject(obj_id);
				else if (type == EventType::EVENT_HP_RECOVERY) ProcessHpRecovery(obj_id);
				else if (type == EventType::EVENT_AUTO_SAVE) ProcessAutoSave(obj_id);
				else if (type == EventType::EVENT_BOSS_SKILL) ProcessBossSkillFire(obj_id);
				else if (type == EventType::EVENT_BOSS_ULT) ProcessBossUltimate(obj_id);

				delete ioCtx;
				break;
			}
			case IO_OP::DB_RESULT_LOGIN: {
				Session* session = GetSessionId(sessionId);
				if (session) {
					std::lock_guard<std::mutex> lock(session->sessionLock);

					session->level = ioCtx->db_level;
					session->exp = ioCtx->db_exp;
					session->hp = ioCtx->db_hp;
					session->max_hp = 100 + ((session->level - 1) * 50); 
					session->attack_power = 30 + ((session->level - 1) * 30);
					session->x = ioCtx->db_x;
					session->y = ioCtx->db_y;
					session->state = SessionState::INGAME;

					S2C_LoginResult res = { sizeof(res), S2C_LOGIN_RESULT, true, "DB Login Success!" };
					session->SendPacket(&res);

					S2C_AvatarInfo info = { sizeof(info), S2C_AVATAR_INFO, sessionId, 0, session->x, session->y, session->direction, session->hp, session->max_hp, session->exp, session->level };
					session->SendPacket(&info);


					short rx = GetRegionX(session->x);
					short ry = GetRegionY(session->y);
					{
						std::unique_lock<std::shared_mutex> wl(g_regions[rx][ry].lock);
						g_regions[rx][ry].objects.insert(sessionId);
					}

					auto near_objs = GetNearbyObjects(session->x, session->y);
					for (int n_id : near_objs) {
						if (n_id == sessionId) continue;
						Session* target = GetSessionId(n_id);
						if (target && target->state.load() == SessionState::INGAME && IsInView(session->x, session->y, target->x, target->y)) {
							{
								std::lock_guard<std::mutex> vl(session->viewLock);
								session->viewList.insert(n_id);
							}
							SendAddObject(sessionId, n_id);

							if (IsPlayer(n_id)) SendAddObject(n_id, sessionId);
							else {
								if (target->viewers_count.fetch_add(1) == 0) {
									target->is_active.store(true);
									AddTimerEvent(n_id, EventType::EVENT_MOVE, 500);
								}
							}
						}
					}
					AddTimerEvent(sessionId, EventType::EVENT_AUTO_SAVE, 60000);
				}
				delete ioCtx;
				break;
			}
			}
		}
	}

	void ProcessReceive(int sessionId, DWORD bytesTransferred) {
		Session* session = sessions[sessionId];
		if (not session || session->state.load() == SessionState::FREE) return;

		int totalBytes = session->prevRemainBytes + bytesTransferred;
		int readPos = 0;

		while (true) {
			if (totalBytes - readPos < 1) break;

			unsigned char packetSize = session->recvContext.buffer[readPos];		
		
			if (totalBytes - readPos < packetSize) break;


			OnPacket(sessionId, &session->recvContext.buffer[readPos]);
			readPos += packetSize;

		}


		int remainBytes = totalBytes - readPos;
		if (remainBytes > 0) {
			memmove(session->recvContext.buffer, &session->recvContext.buffer[readPos], remainBytes);
		}
		
		session->prevRemainBytes = remainBytes;
		DWORD flags = 0;
		session->recvContext.wsabuf.len = sizeof(session->recvContext.buffer) - remainBytes;
		session->recvContext.wsabuf.buf = session->recvContext.buffer + remainBytes;

		ZeroMemory(&session->recvContext.overlapped, sizeof(WSAOVERLAPPED));

		WSARecv(session->socket, &session->recvContext.wsabuf, 1, nullptr, &flags, 
			&session->recvContext.overlapped, nullptr);
	}

	void OnPacket(int sessionId, char* packet) {			
		Session* session = GetSessionId(sessionId);
		if (not session) return;
		
		PACKET_TYPE type = reinterpret_cast<C2S_Login*>(packet)->type;
		
		if (type == C2S_LOGIN) {
			C2S_Login* loginPacket = reinterpret_cast<C2S_Login*>(packet);

			bool is_duplicate = false;
			for (int i = 0; i < MAX_PLAYERS; ++i) {
				if (i == sessionId) continue; 

				Session* other = sessions[i];

				if (other->state.load() != SessionState::FREE && strcmp(other->name, loginPacket->username) == 0) {
					is_duplicate = true;
					break;
				}
			}


			if (is_duplicate) {
				S2C_LoginResult res = { sizeof(res), S2C_LOGIN_RESULT, false, "이미 접속 중인 아이디입니다." };
				session->SendPacket(&res);
				Disconnect(sessionId);
				return;
			}
			


			strcpy_s(session->name, loginPacket->username);

			
			if (strncmp(session->name, "Dummy_", 6) == 0) {
				session->level = (rand() % 40) + 1;
				short spawn_x, spawn_y;

				do {
					GetRespawnPosition(session->level, spawn_x, spawn_y);
				} while (g_wall[spawn_x][spawn_y] == true);

				session->x = spawn_x;
				session->y = spawn_y;
				session->state = SessionState::INGAME;

				S2C_LoginResult res = { sizeof(res), S2C_LOGIN_RESULT, true, "Welcome Dummy!" };
				session->SendPacket(&res);

				S2C_AvatarInfo info = { sizeof(info), S2C_AVATAR_INFO, sessionId, 0, session->x, session->y, session->direction, session->hp, session->max_hp, session->exp, session->level };
				session->SendPacket(&info);

				short rx = GetRegionX(session->x);
				short ry = GetRegionY(session->y);
				{
					std::unique_lock<std::shared_mutex> wl(g_regions[rx][ry].lock);
					g_regions[rx][ry].objects.insert(sessionId);
				}

				auto near_objs = GetNearbyObjects(session->x, session->y);
				for (int n_id : near_objs) {
					if (n_id == sessionId) continue;
					Session* target = GetSessionId(n_id);
					if (target && target->state.load() == SessionState::INGAME && IsInView(session->x, session->y, target->x, target->y)) {
						{
							std::lock_guard<std::mutex> vl(session->viewLock);
							session->viewList.insert(n_id);
						}
						SendAddObject(sessionId, n_id);

						if (IsPlayer(n_id)) SendAddObject(n_id, sessionId);
						else {
							if (target->viewers_count.fetch_add(1) == 0) {
								target->is_active.store(true);
								AddTimerEvent(n_id, EventType::EVENT_MOVE, 500);
							}
						}
					}
				}
			}
			
			else {
				DbTask task;
				task.type = DbTaskType::LOGIN_CHECK;
				task.session_id = sessionId;
				strcpy_s(task.username, session->name);

				db_queue.push(task); 
			}
		}

		else if (session->state.load() == SessionState::INGAME) {
			auto now = std::chrono::steady_clock::now();


			if (type == C2S_MOVE) {
				auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - session->last_move_time).count();

				int move_cooldown = std::max(200, 500 - (session->level - 1) * 10);
				if (duration < move_cooldown - 50) {
					return;
				}

				session->last_move_time = now;
				C2S_Move* movePacket = reinterpret_cast<C2S_Move*>(packet);

				short nx = session->x;
				short ny = session->y;


				if (movePacket->direction == 0) ny -= 1;		
				else if (movePacket->direction == 1) ny += 1;	
				else if (movePacket->direction == 2) nx -= 1;	
				else if (movePacket->direction == 3) nx += 1;	

				if (nx >= 0 && nx < WORLD_WIDTH && ny >= 0 && ny < WORLD_HEIGHT) {
					if (g_wall[nx][ny] == false) {		
						if (nx < session->x) session->direction = 2;
						else if (nx > session->x) session->direction = 3;

						MoveObject(sessionId, nx, ny, movePacket->move_time);


						auto current_near_objs = GetNearbyObjects(nx, ny);
						for (int obj_id : current_near_objs) {
							if (obj_id >= 2000000) { 
								int idx = obj_id - 2000000;

								
								if (g_items[idx].active.load() && g_items[idx].x == nx && g_items[idx].y == ny) {
									bool expected = true;

									if (g_items[idx].active.compare_exchange_strong(expected, false)) {
										item_id_pool.push(obj_id); 


										short rx = GetRegionX(nx); short ry = GetRegionY(ny);
										{
											std::unique_lock<std::shared_mutex> wl(g_regions[rx][ry].lock);
											g_regions[rx][ry].objects.erase(obj_id);
										}


										{
											std::lock_guard<std::mutex> lock(session->sessionLock);
											session->hp = std::min(session->max_hp, session->hp + 500);
										}

										
										S2C_RemoveObject rmPkt = { sizeof(rmPkt), S2C_REMOVE_OBJECT, obj_id };
										BroadcastToViewers(sessionId, &rmPkt);
										session->SendPacket(&rmPkt); 

										S2C_StatusChange statusPkt = { sizeof(statusPkt), S2C_STATUS_CHANGE, sessionId, session->hp, session->max_hp, session->exp, session->level };
										BroadcastToViewers(sessionId, &statusPkt);
										session->SendPacket(&statusPkt);

										SendSystemMessage(sessionId, "HP 포션을 획득하여 체력이 500 회복되었습니다!");
									}
								}
							}
						}
					}
				}
			}

			else if (type == C2S_ATTACK) {
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - session->last_attack_time).count() < 1000) {
					return;		
				}
				session->last_attack_time = now;	

				C2S_Attack* atk = reinterpret_cast<C2S_Attack*>(packet);

				
				S2C_Action actionPacket = { sizeof(actionPacket), S2C_ACTION, sessionId, ActionType::ACTION_ATTACK };
				BroadcastToViewers(sessionId, &actionPacket);
				session->SendPacket(&actionPacket);


				int exp_gained = 0;

				std::unordered_set<int> my_view;
				{
					std::lock_guard<std::mutex> vl(session->viewLock);
					my_view = session->viewList;
				}

				for (int target_id : my_view) {
					if (!IsPlayer(target_id)) {		
						Session* npc = GetSessionId(target_id);
						if (npc && npc->state.load() == SessionState::INGAME) {
							int dx = abs(session->x - npc->x);
							int dy = abs(session->y - npc->y);
							if (dx + dy == 1) {
								HandleDamage(sessionId, target_id, session->attack_power);
							}
						}
					}
				}

				if (exp_gained > 0) {
					std::lock_guard<std::mutex> myLock(session->sessionLock);
					session->exp += exp_gained;

					while (session->exp >= session->level * 100) {
						session->exp -= session->level * 100;
						session->level++;
						session->max_hp += 50;				
						session->hp = session->max_hp;		
					}
					S2C_StatusChange myStatus = { sizeof(myStatus), S2C_STATUS_CHANGE, sessionId, session->hp, session->max_hp, session->exp, session->level };
					session->SendPacket(&myStatus);
					BroadcastToViewers(sessionId, &myStatus);
				}

			}
			else if (type == C2S_CHAT) {
				C2S_Chat* chatPacket = reinterpret_cast<C2S_Chat*>(packet);

				if (chatPacket->message[0] == '/') {

					if (strncmp(chatPacket->message, "/tp ", 4) == 0) {
						int tx, ty;
						if (sscanf_s(chatPacket->message + 4, "%d %d", &tx, &ty) == 2) {
							if (tx >= 0 && tx < WORLD_WIDTH && ty >= 0 && ty < WORLD_HEIGHT) {
								MoveObject(sessionId, static_cast<short>(tx), static_cast<short>(ty));
								SendSystemMessage(sessionId, "지정된 좌표로 순간이동 했습니다.");
							}
						}
						return; 
					}
					else if (strcmp(chatPacket->message, "/god") == 0) {
						session->is_god = !session->is_god;
						if (session->is_god) SendSystemMessage(sessionId, "무적 모드가 활성화되었습니다.");
						else SendSystemMessage(sessionId, "무적 모드가 해제되었습니다.");
						return;
					}
					else if (strncmp(chatPacket->message, "/level ", 7) == 0) {
						int target_level;
						if (sscanf_s(chatPacket->message + 7, "%d", &target_level) == 1) {
							if (target_level > 0 && target_level <= 255) { 
								{
									std::lock_guard<std::mutex> lock(session->sessionLock);
									session->level = target_level;
									session->exp = 0;


									session->max_hp = 100 + ((session->level - 1) * 50);
									session->hp = session->max_hp; 
									session->attack_power = 30 + ((session->level - 1) * 30);
								}

								S2C_StatusChange statusPacket = { sizeof(statusPacket), S2C_STATUS_CHANGE, sessionId, session->hp, session->max_hp, session->exp, session->level };
								session->SendPacket(&statusPacket);
								BroadcastToViewers(sessionId, &statusPacket);

								char msgBuf[128];
								sprintf_s(msgBuf, "레벨이 %d(으)로 변경되었습니다. (공격력: %d / HP: %d)", session->level, session->attack_power, session->max_hp);
								SendSystemMessage(sessionId, msgBuf);
							}
						}
						return;
					}
				}

				S2C_ChatMessage broadcastPacket;
				broadcastPacket.size = sizeof(broadcastPacket);
				broadcastPacket.type = S2C_CHAT_MESSAGE;
				broadcastPacket.object_id = sessionId;
				broadcastPacket.chatType = chatPacket->chatType;
				sprintf_s(broadcastPacket.message, "[%s] %s", session->name, chatPacket->message);

				if (chatPacket->chatType == 1) {
					for (int i = 0; i < MAX_PLAYERS; ++i) {
						Session* pSession = sessions[i];
						if (pSession && pSession->state.load() == SessionState::INGAME) {
							pSession->SendPacket(&broadcastPacket);
						}
					}
				}
				else if (chatPacket->chatType == 0) {
					BroadcastToViewers(sessionId, &broadcastPacket);
				}
			}
		}
	}

	void Disconnect(int id) {
		Session* session = GetSessionId(id);
		if (session) {
			SessionState expected = session->state.load();
			if (expected == SessionState::FREE || !session->state.compare_exchange_strong(expected, SessionState::FREE)) {
				return;
			}
			if (IsPlayer(id)) {
				{
					std::lock_guard<std::mutex> vl(session->viewLock);
					for (int v_id : session->viewList) {
						if (!IsPlayer(v_id)) {
							Session* npc = GetSessionId(v_id);
							if (npc) npc->viewers_count.fetch_sub(1);
						}
					}
				}
				

				if (strncmp(session->name, "Dummy_", 6) != 0) {
					DbTask task;
					task.type = DbTaskType::SAVE_PLAYER;
					strcpy_s(task.username, session->name);
					task.level = session->level;
					task.exp = session->exp;
					task.hp = session->hp;
					task.x = session->x;
					task.y = session->y;

					db_queue.push(task); 
				}
			}
			session->Reset();
			if (id < MAX_PLAYERS) player_id_pool.push(id);
		}
	}
};

int API_BossChat(lua_State* L) {
	int my_id = (int)lua_tointeger(L, 1);
	const char* message = lua_tostring(L, 2);
	std::string ansiMsg = UTF8ToANSI(message);

	if (g_server) g_server->BroadcastBossChat(my_id, ansiMsg.c_str());
	return 0;
}

int API_CastAoESkill(lua_State* L) {
	int my_id = (int)lua_tointeger(L, 1);
	int aoe_damage = (int)lua_tointeger(L, 2);

	if (g_server) g_server->ExecuteBossAoE(my_id, aoe_damage);
	return 0;
}

int main()
{
	InitServerMap();

	IocpServer server;
	g_server = &server;

	if (server.Initialize()) {
		server.Start();
		server.Join();
	}
}