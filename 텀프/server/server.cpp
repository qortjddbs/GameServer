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

#pragma comment (lib, "lua54.lib") // 파일명은 다운받으신 lib 파일 이름에 맞게 수정하세요! (예: lua54.lib, lua5.4.2.lib 등)

class IocpServer;
IocpServer* g_server = nullptr; // 전역 C-API 함수들이 서버 기능에 접근할 수 있게 해주는 마스터 키

std::string UTF8ToANSI(const char* utf8_str) {
	int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
	std::wstring wStr(wLen, 0);
	MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, &wStr[0], wLen);

	int aLen = WideCharToMultiByte(CP_ACP, 0, wStr.c_str(), -1, NULL, 0, NULL, NULL);
	std::string aStr(aLen, 0);
	WideCharToMultiByte(CP_ACP, 0, wStr.c_str(), -1, &aStr[0], aLen, NULL, NULL);
	return aStr;
}

constexpr int VIEW_RANGE = 7;         // 시야 반경 (8칸)
constexpr int REGION_SIZE = 10;       // 한 지역(Region)의 가로/세로 길이
constexpr int MAX_REGION_X = WORLD_WIDTH / REGION_SIZE + 1;
constexpr int MAX_REGION_Y = WORLD_HEIGHT / REGION_SIZE + 1;

struct Region {
	std::unordered_set<int> objects; // 이 지역에 들어있는 플레이어/NPC들의 ID
	std::shared_mutex lock;          // Read(검색)와 Write(이동)를 분리해서 처리할 고급 자물쇠
};

Region g_regions[MAX_REGION_X][MAX_REGION_Y];

// 서버용 장애물 맵 배열
bool g_wall[WORLD_WIDTH][WORLD_HEIGHT] = { false };

// 클라이언트와 똑같은 위치에 벽을 세우는 함수
void InitServerMap() {
	srand(2022180016);		// 클라이언트와 동일한 시드

	for (int i = 0; i < 5'0000; ++i) {
		int rx = rand() % (WORLD_WIDTH - 2);
		int ry = rand() % (WORLD_HEIGHT - 2);

		g_wall[rx][ry] = true;
		g_wall[rx + 1][ry] = true;
		g_wall[rx][ry + 1] = true;
		g_wall[rx + 1][ry + 1] = true;
	}

	// 맵 생성 후 난수 초기화
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
	std::chrono::system_clock::time_point exec_time;	// 실행되어야 할 시간
	int object_id;										// 누가 행동할 것인가 (NPC ID)
	EventType type;										// 어떤 행동을 할 것인가

	// 우선순위 큐를 위해 연산자 오버로딩 : 시간이 '빠른(작은)' 쪽지가 맨 위로 올라오게
	bool operator>(const TimerEvent& other) const {
		return exec_time > other.exec_time;
	}
};

struct IOContext
{
	WSAOVERLAPPED overlapped;
	WSABUF wsabuf;
	char buffer[1024];		// 실제 데이터 들어있는 곳
	IO_OP opType;
	SOCKET acceptSocket;	// 새로 들어올 손님의 소켓을 담아둘 바구니

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

// 개별 플레이어를 나타내는 세션 클래스
class Session
{
public:
	std::mutex sessionLock;
	std::atomic<SessionState> state = SessionState::FREE;

	int id = -1;
	SOCKET socket = INVALID_SOCKET;
	unsigned int sessionIndex = 0;		// 물리적 배열 방 번호
	unsigned int playerNum = 0;			// 이 방을 다녀간 사람 수 (고유 ID용)

	short x = 0, y = 0;
	unsigned char direction = 3;	// 기본 방향 오른쪽
	int hp = 100, max_hp = 100;
	bool is_recovering = false;		// 풀피 아닐때만 회복
	int attack_power = 50;
	unsigned long long exp = 0;
	unsigned char level = 1;
	char name[MAX_NAME_LEN]{};
	bool is_god = false;

	std::chrono::steady_clock::time_point last_move_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);
	std::chrono::steady_clock::time_point last_attack_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);

	// 피격 시 짧은 시간동안 무적
	std::chrono::steady_clock::time_point last_hit_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);

	// 몬스터용 변수들
	std::atomic<bool> is_active{ false };
	short origin_x = 0, origin_y = 0;	// 몬스터는 리젠 위치가 고정되어 있으므로 원래 위치 저장
	MonsterType monster_type = MonsterType::SKELETON;
	AiType ai_type = AiType::FIXED_PEACE;
	int target_id = -1;					// 현재 공격 중인 대상
	std::atomic<int> viewers_count{ 0 };	// 나를 보고 있는 플레이어의 수 (Reference Count)

	std::unordered_set<int> viewList;	// 내 화면에 보이고 있는 객체들의 ID
	std::mutex viewLock;				// 시야 목록 전용 자물쇠

	IOContext recvContext{ IO_OP::RECV };
	int prevRemainBytes = 0;

	lua_State* L_ai = nullptr; // 보스 전용 Lua AI 두뇌 (일반 몬스터는 nullptr)
	bool is_enraged = false;   // 광폭화 상태 체크용 (C++ 단에서도 체크용으로 하나 둡니다)

	short skill_target_x = 0;
	short skill_target_y = 0;
	std::chrono::steady_clock::time_point last_skill_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);

	void Reset() {		// Reset 함수를 실행하는동안 lock_guard가 자동으로 {}를 잠구고 해제해줌.
		std::lock_guard<std::mutex> lock(sessionLock);
		if (socket != INVALID_SOCKET) {
			closesocket(socket);
			socket = INVALID_SOCKET;
		}
		prevRemainBytes = 0;
		memset(name, 0, sizeof(name));

		std::lock_guard<std::mutex> vlLock(viewLock);
		viewList.clear();

		playerNum++;	// 손님이 바뀔 때마다 세대 번호 증가
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

	// 크기 고정 배열의 고질적인 ID 재사용 문제 해결. (세션 번호와 접속 번호를 각각 부여)
	unsigned long long GetUniqueId() {
		return (static_cast<unsigned long long>(playerNum) << 32) | sessionIndex;
	}
};

// 시야 및 Region 헬퍼 함수
// 해당 좌표가 속한 Region 배열의 X, Y 인덱스를 안전하게 계산
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
		// 좌측 상단 마을
		out_x = offset_x;
		out_y = offset_y;
	}
	else if (level <= 20) {
		// 우측 상단 마을
		out_x = WORLD_WIDTH - 100 + offset_x;
		out_y = offset_y;
	}
	else if (level <= 30) {
		// 좌측 하단 마을
		out_x = offset_x;
		out_y = WORLD_HEIGHT - 100 + offset_y;
	}
	else {
		// 우측 하단 마을 (고레벨)
		out_x = WORLD_WIDTH - 100 + offset_x;
		out_y = WORLD_HEIGHT - 100 + offset_y;
	}

	if (out_x >= WORLD_WIDTH) out_x = WORLD_WIDTH - 1;
	if (out_y >= WORLD_HEIGHT) out_y = WORLD_HEIGHT - 1;
}

// 두 객체가 서로 시야(VIEW_RANGE) 내에 있는지 확인 (맨해튼 거리 기준)
inline bool IsInView(short x1, short y1, short x2, short y2) {
	return (abs(x1 - x2) <= VIEW_RANGE) && (abs(y1 - y2) <= VIEW_RANGE);
}

// 특정 좌표를 기준으로 "나를 포함한 주변 9개 Region"에 있는 모든 객체 ID를 긁어오는 함수
std::unordered_set<int> GetNearbyObjects(short cur_x, short cur_y) {
	std::unordered_set<int> nearby_objs;
	short rx = GetRegionX(cur_x);
	short ry = GetRegionY(cur_y);

	// 내 Region을 중심으로 3x3 (총 9개) Region을 뒤집니다.
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			short nx = rx + dx;
			short ny = ry + dy;

			// 맵 범위를 벗어나는 Region은 무시
			if (nx < 0 || nx >= MAX_REGION_X || ny < 0 || ny >= MAX_REGION_Y) continue;

			// 읽기 전용 락(shared_lock)을 걸고 안전하게 ID들을 복사해 옵니다.
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

// 전체 서버를 총괄하는 메인 엔진 클래스
class IocpServer
{
private:
	HANDLE hIocp = INVALID_HANDLE_VALUE;
	SOCKET listenSocket = INVALID_SOCKET;
	// 메모리 해제가 없으므로 벡터가 가장 빠름 (free-store에 메모리 할당)
	std::vector<Session*> sessions;
	// 빈 방 번호표 관리용 안전 바구니
	tbb::concurrent_queue<int> player_id_pool;
	std::vector<std::thread> workers;
	std::priority_queue<TimerEvent, std::vector<TimerEvent>, std::greater<TimerEvent>> timer_queue;
	std::mutex timer_mock;
	tbb::concurrent_queue<DbTask> db_queue;
	std::thread db_worker;

public:
	IocpServer() {
		sessions.resize(MAX_PLAYERS + NUM_NPCS, nullptr);
		// 플레이어용 세션 메모리 할당
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			sessions[i] = new Session();
			sessions[i]->sessionIndex = i;
			player_id_pool.push(i);
		}

		// ===============================================
		// [LUA 몬스터 스크립트 기반 배치 적용]
		// 기존 C++ 하드코딩 로직을 완전히 대체합니다.
		// ===============================================
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

					int current_npc_id = NPC_ID_START; // 100000부터 시작

					while (lua_next(L, -2) != 0) {
						// 더 이상 스폰할 공간이 없으면 파싱 중단
						if (current_npc_id >= NPC_ID_START + NUM_NPCS) {
							lua_pop(L, 1);
							break;
						}

						// 1. 데이터 추출
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
							m_x = (rand() % (WORLD_WIDTH - 40)) + 20; // 맵 끝부분은 피해서 랜덤 스폰
							m_y = (rand() % (WORLD_HEIGHT - 40)) + 20;
						}

						lua_getfield(L, -1, "hp");
						int m_hp = (int)lua_tointeger(L, -1);
						lua_pop(L, 1);

						lua_getfield(L, -1, "atk");
						int m_atk = (int)lua_tointeger(L, -1);
						lua_pop(L, 1);

						// 2. 서버 메모리 할당 및 데이터 덮어쓰기
						int real_index = MAX_PLAYERS + (current_npc_id - NPC_ID_START); // 안전한 배열 인덱스 계산!

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

						// [추가] 만약 이 녀석이 보스 몬스터라면 전용 Lua AI를 심어준다!
						if (strncmp(m_name, "Boss_", 5) == 0) {
							npc->L_ai = luaL_newstate();
							luaL_openlibs(npc->L_ai);

							// C++ API 등록
							lua_register(npc->L_ai, "API_BossChat", API_BossChat);
							lua_register(npc->L_ai, "API_CastAoESkill", API_CastAoESkill);

							// 보스 AI 스크립트 로드
							if (luaL_dofile(npc->L_ai, "boss_ai.lua") != LUA_OK) {
								// std::cout << "[LUA Error] 보스 AI 로드 실패!" << std::endl;
							}
						}

						// 맵(Region)에 몬스터 등록
						short rx = GetRegionX(npc->x);
						short ry = GetRegionY(npc->y);
						{
							std::unique_lock<std::shared_mutex> wl(g_regions[rx][ry].lock);
							g_regions[rx][ry].objects.insert(current_npc_id);
						}

						// 게임 상태 활성화
						npc->state.store(SessionState::INGAME);

						current_npc_id++;
						lua_pop(L, 1); // 다음 루프를 위해 값만 pop
					}
					// std::cout << "[시스템] 스크립트 기반 몬스터 배치가 완료되었습니다! (총 " << (current_npc_id - NPC_ID_START) << "마리)" << std::endl;
				}
			}
			lua_close(L);
		}
	}

	bool Initialize() {
		// 윈도우 소켓 라이브러리 초기화 (Winsock 버전 2.2 사용하겠다는 뜻)
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
		
		// 커널 내부 중앙 우체국(IOCP 핸들) 생성
		// 첫 인자 : IOCP 핸들로 사용할 핸들 (INVALID_HANDLE_VALUE이면 새로 생성) / 마지막 인자 0 : 컴퓨터 CPU 코어 개수만큼 스레드를 알아서 쾌적하게 굴려라.
		hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);

		// 비동기 전용 문지기(Listen) 소켓 생성
		// AF_INET : IPv4, SOCK_STREAM : TCP, IPPROTO_TCP : TCP 프로토콜, WSA_FLAG_OVERLAPPED : 비동기 소켓
		listenSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

		// 서버의 네트워크 주소 및 포트 설정
		SOCKADDR_IN serverAddr{};
		serverAddr.sin_family = AF_INET;
		serverAddr.sin_port = htons(PORT);			// 프로토콜 헤더에 정의된 3500번 포트
		serverAddr.sin_addr.s_addr = INADDR_ANY;	// 내 컴퓨터에 장착된 모든 IP로 들어오는 접속 허용

		// 소켓에 주소 바인딩 및 리슨 상태 전환
		if (::bind(listenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) return false;
		if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) return false;

		// 문지기 소켓(listenSocket)을 중앙 우체국(hIocp)에 등록
		// 10000는 일종의 식별 태그(Completion Key). 나중에 워커 쓰레드가 잠에서 깰 때 10000번 소켓에서 일이 터졌다! 하고 깨어나는데
		// 그걸 보고 다른 유저가 패킷을 보낸 게 아닌, 새로운 손님이 문을 열고 들어온 거(AcceptEx 완료)라고 구분할 수 있음
		CreateIoCompletionPort((HANDLE)listenSocket, hIocp, 10000, 0);
		return true;
	}

	void Start() {
		RegisterAccept();

		// 기존 워커 쓰레드 생성 전에 타이머 전담 쓰레드 1개 먼저 생성
		workers.emplace_back(&IocpServer::TimerLoop, this);

		db_worker = std::thread(&IocpServer::DbWorkerLoop, this);

		int threadCount = std::thread::hardware_concurrency(); // 컴퓨터 CPU 코어 개수 반환
		for (int i = 0; i < threadCount; ++i) {
			workers.emplace_back(&IocpServer::WorkerLoop, this);
			// push_back은 객체를 넘기고, emplace_back은 객체 생성에 필요한 인자를 넘김
		}
	}

	void Join() {
		for (auto& th : workers) th.join();
		if (db_worker.joinable()) db_worker.join();
	}

	// 보스 광역기 연산 및 패킷 발송
	void ExecuteBossAoE(int boss_id, int damage) {
		Session* boss = GetSessionId(boss_id);
		if (!boss) return;

		// std::cout << "[BOSS ULTIMATE] 보스가 반경 15칸 즉사기 시전 준비!" << std::endl;

		// 1. 즉사기 경고용 '보라색 거대 장판' 패킷 생성 (이펙트 타입 3번)
		S2C_SkillEffect eff = { sizeof(eff), S2C_SKILL_EFFECT, boss_id, boss->x, boss->y, 3 };

		// 20칸 내 모든 유저에게 경고 장판 전송
		auto near_objs = GetNearbyObjects(boss->x, boss->y);
		for (int target_id : near_objs) {
			if (IsPlayer(target_id)) {
				Session* player = GetSessionId(target_id);
				if (player && player->state.load() == SessionState::INGAME) {
					player->SendPacket(&eff);
				}
			}
		}

		// 2. 2초 뒤에 펑 터지도록 타이머 예약 (좌표와 데미지를 임시로 기억해 둠)
		boss->skill_target_x = boss->x;
		boss->skill_target_y = boss->y;
		boss->attack_power = damage; // 궁극기 데미지(3000) 임시 저장
		AddTimerEvent(boss_id, EventType::EVENT_BOSS_ULT, 10000);
	}

	// 보스 말풍선 띄우기
	void BroadcastBossChat(int boss_id, const char* msg) {
		Session* boss = GetSessionId(boss_id);
		if (!boss) return;

		S2C_ChatMessage chatPacket;
		chatPacket.size = sizeof(S2C_ChatMessage);
		chatPacket.type = S2C_CHAT_MESSAGE;
		chatPacket.object_id = boss_id;
		chatPacket.chatType = 2;
		sprintf_s(chatPacket.message, "BOSS  :  이 구역의 지배자는 나다!!");

		// 시야에 없어도 멀리서 보스의 고함 소리가 들리도록 20칸 내 모든 유저에게 전송
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

		// 지진 및 피보라 폭발 이펙트 (타입 2번)
		S2C_SkillEffect eff = { sizeof(eff), S2C_SKILL_EFFECT, boss_id, boss->skill_target_x, boss->skill_target_y, 2 };

		auto near_objs = GetNearbyObjects(boss->skill_target_x, boss->skill_target_y);
		for (int target_id : near_objs) {
			if (IsPlayer(target_id)) {
				Session* player = GetSessionId(target_id);
				if (player && player->state.load() == SessionState::INGAME) {
					int dist = abs(boss->skill_target_x - player->x) + abs(boss->skill_target_y - player->y);

					if (dist <= 15) { // 15칸 이내에 도망 못친 유저들
						player->SendPacket(&eff); // 화면 흔들림 패킷 전송
						HandleDamage(boss_id, target_id, 2000); // 3000 데미지 쾅!

						// 무자비한 5칸 넉백
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

	// 배열 인덱스 초과를 막는 라우터
	Session* GetSessionId(int id) {
		if (id < MAX_PLAYERS) {
			return sessions[id];		// 유저는 그대로
		}
		else if (id >= NPC_ID_START && id < NPC_ID_START + NUM_NPCS) {
			return sessions[MAX_PLAYERS + (id - NPC_ID_START)];			// NPC는 100만을 빼서 접근!
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

		// 실제 좌표 변경
		{
			std::lock_guard<std::mutex> lock(obj->sessionLock);
			obj->x = nx;
			obj->y = ny;
		}

		// Region 이동 처리
		short old_rx = GetRegionX(old_x);
		short old_ry = GetRegionY(old_y);
		short new_rx = GetRegionX(nx);
		short new_ry = GetRegionY(ny);

		if (old_rx != new_rx || old_ry != new_ry) {
			// Region이 바뀌었으면, 기존 Region에서 나가고 새 Region에 들어감
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

				// 이동 중이므로 락 없이 현재 값 복사 (이미 세션 락이 필요한 연산은 위에서 처리됨)
				task.level = obj->level;
				task.exp = obj->exp;
				task.hp = obj->hp;
				task.x = nx; // 최신 좌표
				task.y = ny; // 최신 좌표

				db_queue.push(task);
			}
		}

		if (IsPlayer(id)) {
			// 플레이어가 이동한 경우에만 뷰리스트 갱신 및 시야 비교 수행
			std::unordered_set<int> old_view;
			{
				std::lock_guard<std::mutex> vl(obj->viewLock);
				old_view = obj->viewList;
			}

			std::unordered_set<int> new_view;
			auto near_objs = GetNearbyObjects(nx, ny);
			for (int n_id : near_objs) {
				if (n_id == id) continue;
				Session* target = GetSessionId(n_id);
				if (target && target->state.load() == SessionState::INGAME && IsInView(nx, ny, target->x, target->y)) {
					new_view.insert(n_id);
				}
			}

			for (int n_id : new_view) {
				if (old_view.count(n_id) == 0) {		// 새로 발견
					if (IsPlayer(n_id)) {
						SendAddObject(id, n_id);
						SendAddObject(n_id, id);
					}
					else {
						SendAddObject(id, n_id);
						Session* npc = GetSessionId(n_id);
						// NPC를 내 시야에 넣으면서 Reference Count 증가 및 깨우기
						if (npc && npc->viewers_count.fetch_add(1) == 0) {
							WakeUpNpc(n_id);
						}
					}
				}
				else {		// 기존에도 있던 객체
					if (IsPlayer(n_id)) SendMoveObject(n_id, id, nx, ny, move_time);	// 기존에 보이던 객체 위치만 업데이트
				}
			}

			for (int o_id : old_view) {
				if (new_view.count(o_id) == 0) {		// 시야에서 벗어남
					SendRemoveObject(id, o_id);
					if (IsPlayer(o_id)) {
						SendRemoveObject(o_id, id);
					}
					else {
						Session* npc = GetSessionId(o_id);
						// NPC가 내 시야에서 벗어났으므로 Reference Count 감소 (0이 되면 알아서 잠듦)
						if (npc) {
							npc->viewers_count.fetch_sub(1);
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
			// NPC일 때는 시야 연산 없이 이동 패킷만 주변에 전송
			S2C_MoveObject packet = { sizeof(S2C_MoveObject), S2C_MOVE_OBJECT, id, nx, ny, move_time };
			BroadcastToViewers(id, &packet);
		}
	}

	// 뷰리스트 대신 물리적인 주변 공간을 싹 뒤져서 패킷 발송
	void BroadcastToViewers(int my_id, void* packet) {
		Session* me = GetSessionId(my_id);
		if (!me) return;

		auto near_objs = GetNearbyObjects(me->x, me->y);
		for (int viewer : near_objs) {
			if (IsPlayer(viewer)) {
				Session* vSession = GetSessionId(viewer);
				// 게임 중이고 시야 거리 안에 있다면 무조건 발송!
				if (vSession && vSession->state.load() == SessionState::INGAME && IsInView(me->x, me->y, vSession->x, vSession->y)) {
					vSession->SendPacket(packet);
				}
			}
		}
	}

	// 객체(플레이어, 몬스터) 사망 시에도 주변 공간을 뒤져서 삭제 패킷 발송
	void KillObject(int id) {
		Session* target = GetSessionId(id);
		if (!target) return;
		target->state.store(SessionState::DEAD);

		// Region에서 제거
		short rx = GetRegionX(target->x);
		short ry = GetRegionY(target->y);
		{
			std::unique_lock<std::shared_mutex> wl(g_regions[rx][ry].lock);
			g_regions[rx][ry].objects.erase(id);
		}

		// 주변에 있는 플레이어들에게 삭제 패킷 쏘기
		auto near_objs = GetNearbyObjects(target->x, target->y);
		for (int viewer : near_objs) {
			if (IsPlayer(viewer)) {
				Session* vSession = GetSessionId(viewer);
				if (vSession && vSession->state.load() == SessionState::INGAME && IsInView(target->x, target->y, vSession->x, vSession->y)) {
					SendRemoveObject(viewer, id);

					{
					// 유저의 뷰리스트에서도 죽은 몬스터 제거
					std::lock_guard<std::mutex> vl(vSession->viewLock);
					vSession->viewList.erase(id);
					}
				}
			}
		}

		// 내 뷰리스트 비우기 & 카운트 다운
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

		// std::cout << "object " << id << " is killed." << std::endl;
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
						// 시간이 다 된 이벤트가 있으면 꺼냄
						top_event = timer_queue.top();
						timer_queue.pop();
						has_event = true;
					}
				}
			}

			// 꺼낸 이벤트가 있다면 처리
			if (has_event) {
				// 함수를 타이머 루프에서 직접 처리하지 않고 Worker 쓰레드들에게 떠넘기기
				// 안그러면 동접 많아질 때 몬스터들 바보됨 (타이머 큐에서 처리하는 것보다 쌓이는 게 빨라서)
				IOContext* aiCtx = new IOContext(IO_OP::DO_AI);
				PostQueuedCompletionStatus(hIocp, static_cast<DWORD>(top_event.type), top_event.object_id, &aiCtx->overlapped);
			}
			else {
				// 아직 처리할 이벤트가 없으면 쓰레드 잠깐 재우기
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
	}

	// 몬스터 타입에 따른 이동 패턴 구현 (A* 알고리즘 기반)
	void ProcessNpcMove(int npc_id) {
		Session* npc = GetSessionId(npc_id);
		if (!npc || npc->state.load() != SessionState::INGAME) return;

		int move_delay = 500;
		if (strncmp(npc->name, "Boss_", 5) == 0) move_delay = 1000;

		short nx = npc->x;
		short ny = npc->y;
		bool hasAggroTarget = false;
		short targetX = -1, targetY = -1;

		// 기존 타겟 추적 유지
		if (npc->target_id != -1) {
			Session* p = GetSessionId(npc->target_id);
			if (p && p->state.load() == SessionState::INGAME && IsInView(npc->x, npc->y, p->x, p->y)) {
				targetX = p->x;
				targetY = p->y;
				hasAggroTarget = true;
			}
			else {
				npc->target_id = -1;		// 시야에서 사라지거나 죽었으면 타겟 초기화
			}
		}

		// 타겟이 없다면 새 타겟 탐색
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
							npc->target_id = player->id;	// 끝까지 쫓아가기 위해 플레이어 기억
						}
					}
				}
			}
		}
		// 피스 몬스터: 평소엔 무시, 누가 나를 때리면 쫓아감
		else if (npc->ai_type == AiType::FIXED_PEACE){
			if (npc->target_id != -1) {		// 피스 몬스터를 누군가 때렸으면
				Session* p = GetSessionId(npc->target_id);
				if (p && p->state.load() == SessionState::INGAME && IsInView(npc->x, npc->y, p->x, p->y)) {
					targetX = p->x;
					targetY = p->y;
					hasAggroTarget = true;
				}
				else {
					npc->target_id = -1;		// 타겟이 시야에서 사라지면 원래대로 돌아가기
				}
			}
		}

		// 이동 경로 결정
		if (hasAggroTarget) {
			// 타겟이 있으면 A*로 쫓아감 (전투 상태)
			
			// 타겟과의 거리 계산
			int dist = abs(npc->x - targetX) + abs(npc->y - targetY);

			if (strncmp(npc->name, "Boss_", 5) == 0 && dist <= 4) {
				auto now = std::chrono::steady_clock::now();
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - npc->last_skill_time).count() > 5000) {
					npc->last_skill_time = now;
					npc->skill_target_x = targetX;
					npc->skill_target_y = targetY;

					// 1. 클라이언트들에게 '경고 장판(0)' 띄우라고 지시
					S2C_SkillEffect eff = { sizeof(eff), S2C_SKILL_EFFECT, npc_id, targetX, targetY, 0 };
					BroadcastToViewers(npc_id, &eff);

					// 2. 1.5초(1500ms) 뒤에 실제 폭발 데미지가 들어가도록 타이머 예약
					AddTimerEvent(npc_id, EventType::EVENT_BOSS_SKILL, 1500);

					// 스킬을 시전하는 동안에는 제자리에 멈춰있도록 이동 예약만 걸고 바로 리턴
					AddTimerEvent(npc_id, EventType::EVENT_MOVE, move_delay);
					return;
				}
			}

			// 몬스터 사거리 판단
			int attack_range = 1;

			if (dist <= attack_range) {
				// 사거리 안으로 들어왔으면 공격
				
				// 위치는 제자리 고정
				nx = npc->x;
				ny = npc->y;

				// 몬스터 공격
				S2C_Action actionPacket = { sizeof(actionPacket), S2C_ACTION, npc->id, ActionType::ACTION_ATTACK };
				BroadcastToViewers(npc->id, &actionPacket);

				HandleDamage(npc_id, npc->target_id, npc->attack_power);
			}
			else {
				// 몬스터 공격 사거리 밖이면 A*로 추적
				short nextX, nextY;
				if (FindNextStepAStar(npc->x, npc->y, targetX, targetY, nextX, nextY)) {
					nx = nextX;
					ny = nextY;
				}
			}
		}
		else {		// 타겟이 없으면
			if (npc->ai_type == AiType::ROAMING_AGGRO) {
				// 로밍 몬스터: 리젠 위치 기준으로 20x20 범위 내에서 무작위로 돌아다님
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
				// 고정 몬스터: 타겟이 없으면 안움직임
				nx = npc->x;
				ny = npc->y;
			}
		}

		// 벽 충돌 검사 및 최종 이동
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
			// 주변에 플레이어가 없으면 비활성화
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

		// Region 재등록 및 뷰리스트 갱신
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
				// 서로의 viewList에 추가해야 상호작용 가능
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
		AddTimerEvent(npc_id, EventType::EVENT_MOVE, 500);	// 이동도 같이 예약
		//std::cout << "NPC " << npc_id << " respawned at (" << npc->x << ", " << npc->y << ")" << std::endl;
	}

	void HandleDamage(int attacker_id, int victim_id, int damage) {
		Session* attacker = GetSessionId(attacker_id);
		Session* victim = GetSessionId(victim_id);

		if (!attacker || !victim || attacker->state.load() != SessionState::INGAME || victim->state.load() != SessionState::INGAME) return;
	
		if (IsPlayer(victim_id) && victim->is_god) return;

		// 플레이어 피격 시 일정시간 무적
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


		// 데미지 적용 및 사망 판정
		{
			std::lock_guard<std::mutex> lock(victim->sessionLock);
			victim->hp -= damage;

			// [추가] 보스 몬스터(L_ai 존재)가 아직 광폭화 상태가 아닐 때만 Lua 호출
			if (victim->L_ai != nullptr && !victim->is_enraged && victim->hp > 0) {
				lua_getglobal(victim->L_ai, "OnDamageTaken");
				lua_pushinteger(victim->L_ai, victim->id); // [추가] 첫 번째 인자로 내 ID를 넘김
				lua_pushnumber(victim->L_ai, victim->hp);
				lua_pushnumber(victim->L_ai, victim->max_hp);

				// 파라미터가 3개가 되었으므로 pcall의 두 번째 인자를 3으로 변경
				if (lua_pcall(victim->L_ai, 3, 1, 0) == LUA_OK) {
					int is_enraged_result = (int)lua_tointeger(victim->L_ai, -1);
					if (is_enraged_result == 1) {
						victim->is_enraged = true;
					}
					lua_pop(victim->L_ai, 1);
				}
			}

			// 몬스터가 맞았고, 기존에 타겟이 없었다면 공격자를 타겟으로 설정
			if (!IsPlayer(victim_id) && victim->hp > 0 && victim->target_id == -1) {
				victim->target_id = attacker_id;
			}

			if (victim->hp <= 0) {
				victim->hp = 0;
				is_victim_dead = true;
				victim->is_recovering = false;

				if (IsPlayer(victim_id)) {
					// 플레이어 사망 시 페널티
					victim->hp = victim->max_hp;
					victim->exp /= 2;
					if (!IsPlayer(attacker_id)) attacker->target_id = -1;	// 공격한 몬스터의 타겟 해제
				}
				else {
					// 몬스터 사망 시 시체 상태로 전환 및 경험치 계산
					victim->state.store(SessionState::DEAD);
					exp_gained = (victim->level * victim->level * 2);

					if (victim->ai_type == AiType::ROAMING_AGGRO) {
						exp_gained *= 2;
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

		// 사망 처리
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
				// 몬스터 사망
				S2C_Action deadPacket = { sizeof(deadPacket), S2C_ACTION, victim_id, ActionType::ACTION_DEAD };
				BroadcastToViewers(victim_id, &deadPacket);

				S2C_StatusChange statusPacket = { sizeof(statusPacket), S2C_STATUS_CHANGE, victim_id, victim->hp, victim->max_hp, victim->exp, victim->level };
				BroadcastToViewers(victim_id, &statusPacket);

				AddTimerEvent(victim_id, EventType::EVENT_DESPAWN, 1000);
				AddTimerEvent(victim_id, EventType::EVENT_RESPAWN, 30000);

				// 플레이어 경험치 및 레벨업 처리
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
							attacker->attack_power += 50;
							attacker->hp = attacker->max_hp;
							is_leveled_up = true;

							required_exp = 100LL * (1LL << (attacker->level - 1));
						}

						// 무거운 I/O 작업을 락 바깥에서 하기 위해 현재 상태를 복사
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

					// 스냅샷으로 찍어둔 안전한 지역 변수를 사용해 패킷 조립 및 전송
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
		// 빈 방(새 소켓) 미리 파두기
		SOCKET clientSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

		// 작업 영수증(확장 Overlapped 구조체) 만들기 - Accept 작업용
		IOContext* acceptCtx = new IOContext(IO_OP::ACCEPT);
		acceptCtx->acceptSocket = clientSocket;

		// OS에게 예약 걸기
		// listenSocket을 통해 손님이 들어오면, 방금 내가 만든 이 빈 방(clientSocket)에 바로 앉혀줘.
		// 그리고 작업이 다 끝나면 이 영수증(acceptCtx)을 IOCP큐에 넣어줘.
		// 중간 파라미터 0 : 접속과 동시에 첫 패킷을 받지 않고 순수하게 연결만 처리하겠다는 뜻
		AcceptEx(listenSocket, clientSocket, acceptCtx->buffer, 0, 
			sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, nullptr, &acceptCtx->overlapped);
	}

	int AllocateSession(SOCKET clientSocket) {
		int new_id;
		// for문 검색 대신 번호표 바구니에서 ID를 즉시 뽑아옴 O(1)
		if (player_id_pool.try_pop(new_id)) {
			Session* new_session = GetSessionId(new_id);
			new_session->id = new_id;
			new_session->socket = clientSocket;
			new_session->state = SessionState::CONNECTED;
			return new_id;
		}
		return -1;	// 빈 번호표 X -> 방 꽉참
	}

	// A* 길찾기용 노드 구조체
	struct AStarNode {
		short x, y;
		int g, h, f;
		bool operator>(const AStarNode& other) const { return f > other.f; }
	};

	// A* 알고리즘으로 타겟을 향한 다음 스텝(nx, ny)을 계산하는 함수
	bool FindNextStepAStar(short startX, short startY, short targetX, short targetY, short& outX, short& outY) {
		std::priority_queue<AStarNode, std::vector<AStarNode>, std::greater<AStarNode>> pq;

		// 2000x2000 배열을 매번 만들면 서버가 터지므로, 탐색한 좌표만 해시맵에 저장 (키: y * 2000 + x)
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
			if (iterations++ > 100) break;	// 서버 랙 방지: 어그로 반경(11x11)을 고려해 최대 100번만 탐색
		
			AStarNode current = pq.top();
			pq.pop();

			if (current.x == targetX && current.y == targetY) {
				found = true;
				break;
			}

			for (int i = 0; i < 4; ++i) {
				short nx = current.x + dx[i];
				short ny = current.y + dy[i];

				// 맵 범위 이탈 및 장애물 검사
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

		if (!found) return false;	// 경로가 막혀있음

		// 타겟에서 역추적하여 첫 번째 스텝 알아내기
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
		// CAS 연산 : is_active가 expected와 같다면 true로 바꾸고 true를 반환
		// 이미 true라면 false를 반환하여 중복 타이머 등록 방지
		if (npc->is_active.compare_exchange_strong(expected, true)) {
			// 잠에서 깨어났으니 0.5초 뒤에 움직이도록 타이머에 등록
			AddTimerEvent(npc_id, EventType::EVENT_MOVE, 500);
		}
	}

	// 06.13 HP 자동회복
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
			AddTimerEvent(player_id, EventType::EVENT_HP_RECOVERY, 5000);	// 다음 회복 예약
		}
	}

	// 06.13 채팅
	void SendSystemMessage(int player_id, const char* msg) {
		Session* player = GetSessionId(player_id);
		if (!player || player->state.load() != SessionState::INGAME) return;

		S2C_ChatMessage chatPacket;
		chatPacket.size = sizeof(S2C_ChatMessage);
		chatPacket.type = S2C_CHAT_MESSAGE;
		chatPacket.object_id = player_id;
		chatPacket.chatType = 2; // 시스템 메시지 타입
		strcpy_s(chatPacket.message, msg);

		player->SendPacket(&chatPacket);
	}

	// 06.15 DB 연동
	void DbWorkerLoop() {
		SQLHENV hEnv = SQL_NULL_HENV;
		SQLHDBC hDbc = SQL_NULL_HDBC;
		SQLRETURN retcode;

		// 1. 환경 핸들 할당 및 ODBC 버전 설정
		SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
		SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);

		// 2. 연결 핸들 할당
		SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);

		// 3. MSSQL 연결 (Windows 인증 방식)
		// 주의: SSMS에서 접속하는 서버 이름이 "localhost\SQLEXPRESS"인지 확인하세요.
		SQLWCHAR* connStr = (SQLWCHAR*)L"DRIVER={ODBC Driver 17 for SQL Server};SERVER=localhost\\SQLEXPRESS;DATABASE=GameDB;Trusted_Connection=yes;";
		SQLWCHAR outStr[1024];
		SQLSMALLINT outStrLen;

		retcode = SQLDriverConnect(hDbc, NULL, connStr, SQL_NTS, outStr, 1024, &outStrLen, SQL_DRIVER_NOPROMPT);

		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
			// std::cout << "[DB] MSSQL Connected Successfully!" << std::endl;
		}
		else {
			std::cout << "[DB Error] Failed to connect to MSSQL!" << std::endl;

			// -----------------------------------------------------------------
			// [추가] ODBC가 뱉어내는 진짜 에러 메시지 추출하기
			// -----------------------------------------------------------------
			SQLWCHAR sqlState[6], msg[1024];
			SQLINTEGER nativeError;
			SQLSMALLINT msgLen;

			if (SQLGetDiagRec(SQL_HANDLE_DBC, hDbc, 1, sqlState, &nativeError, msg, 1024, &msgLen) == SQL_SUCCESS) {
				// 한글 윈도우 환경에서 와이드 문자열(SQLWCHAR)을 출력
				std::wcout << L"SQLState: " << sqlState << std::endl;
				std::wcout << L"Message: " << msg << std::endl;
			}

			// 연결 실패 시 핸들 반환
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
							// 플레이어 초기 설정
							sprintf_s(insertQuery, "INSERT INTO users (name, level, exp, hp, x, y) VALUES ('%s', 1, 0, 100, 100, 100)", task.username);

							int len2 = MultiByteToWideChar(CP_ACP, 0, insertQuery, -1, NULL, 0);
							std::wstring wInsert(len2, 0);
							MultiByteToWideChar(CP_ACP, 0, insertQuery, -1, &wInsert[0], len2);

							SQLExecDirect(hStmt, (SQLWCHAR*)wInsert.c_str(), SQL_NTS);

							// 플레이어 초기 설정
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

	// 06.15 DB 자동저장
	void ProcessAutoSave(int player_id) {
		Session* player = GetSessionId(player_id);

		// 플레이어가 기적적으로 살아있고 게임 중일 때만 저장
		if (!player || player->state.load() != SessionState::INGAME) return;

		// 스트레스 테스트용 더미는 DB 부하 방지를 위해 오토세이브 제외
		if (strncmp(player->name, "Dummy_", 6) != 0) {
			DbTask task;
			task.type = DbTaskType::SAVE_PLAYER;
			strcpy_s(task.username, player->name);

			// 멀티스레드 안전을 위해 데이터를 꺼낼 때만 잠깐 세션락을 잡습니다.
			{
				std::lock_guard<std::mutex> lock(player->sessionLock);
				task.level = player->level;
				task.exp = player->exp;
				task.hp = player->hp;
				task.x = player->x;
				task.y = player->y;
			}
			db_queue.push(task);
			// std::cout << "[Auto-Save] Player " << player->name << "'s data pushed to DB queue." << std::endl;
		}

		// [중요] 다음 1분 뒤에 다시 저장되도록 무한 오토세이브 루프 가동
		AddTimerEvent(player_id, EventType::EVENT_AUTO_SAVE, 60000);
	}

	void ProcessBossSkillFire(int boss_id) {
		Session* boss = GetSessionId(boss_id);
		if (!boss || boss->state.load() != SessionState::INGAME) return;

		short sx = boss->skill_target_x;
		short sy = boss->skill_target_y;

		// 1. 클라이언트들에게 '폭발 이펙트(1)' 터트리라고 지시
		S2C_SkillEffect eff = { sizeof(eff), S2C_SKILL_EFFECT, boss_id, sx, sy, 1 };
		BroadcastToViewers(boss_id, &eff);

		// 2. 해당 장판 위치(sx, sy) 기준 십자 1칸 이내에 아직도 남아있는 멍청한(?) 플레이어 색출
		auto near_objs = GetNearbyObjects(sx, sy);
		for (int p_id : near_objs) {
			if (IsPlayer(p_id)) {
				Session* p = GetSessionId(p_id);
				if (p && p->state.load() == SessionState::INGAME) {
					int dist = abs(p->x - sx) + abs(p->y - sy);
					if (dist <= 1) { // 폭발 반경 안에 있다면
						// 데미지 2배 적용
						HandleDamage(boss_id, p_id, boss->attack_power * 2);

						// 넉백(Knockback) 로직: 폭발 중심점에서 반대 방향으로 1칸 밀쳐냄
						int push_x = p->x;
						int push_y = p->y;

						if (p->x < sx) push_x -= 1; // 왼쪽에 있었으면 더 왼쪽으로
						else if (p->x > sx) push_x += 1;

						if (p->y < sy) push_y -= 1;
						else if (p->y > sy) push_y += 1;

						// 밀려나는 곳이 벽이 아니라면 강제 이동!
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
			// hIocp : 우리가 바라볼 우체국(IOCP 핸들)
			// &bytesTransferred : (출력용) 통신이 완료된 데이터의 크기(몇 바이트인지)를 OS가 여기에 적어줌
			// &completionKey : (출력용) 누구의 작업인지 식별할 수 있는 태그 (기존 손님이 보내면 Session ID (0~9999), 새 손님이면 10000)
			// &overlapped : (출력용) 우리가 WSASend/Recv때 넘겨줬던 그 작업 영수증 구조체의 주소를 OS가 다시 돌려줌
			// INFINITE : 완료된 작업이 큐에 나올 때까지 쓰레드를 무한정 수면(Sleep) 상태로 대기시킴

			// OS는 WSAOVERLAPPED* 껍데기만 돌려주기 때문에, 우리가 원래 정의했던 확장 구조체인
			// IOContext*로 다시 캐스팅하여 이 작업이 ACCEPT인지, RECV인지, SEND인지 파악
			IOContext* ioCtx = reinterpret_cast<IOContext*>(overlapped);

			// 클라이언트가 게임을 강제 종료하거나 랜선이 뽑히면 bytesTransferred가 0으로 오거나
			// result가 FALSE로 오는데, 이 경우 해당 유저(completionKey)의 세션을 리셋(Disconnect) 해줌
			if (!result || (bytesTransferred == 0 && ioCtx->opType != IO_OP::ACCEPT && ioCtx->opType != IO_OP::DO_AI)) {
				if (completionKey != 10000) {
					Disconnect(static_cast<int>(completionKey));
					// std::cout << "[Disconnect] Player " << completionKey << " left." << std::endl;
				}
				if (ioCtx->opType == IO_OP::SEND) delete ioCtx;		// 보내던 중 끊겼으면 메모리 누수 방지 위해 영수증 폐기
				continue;	// 에러가 났으니 아래 로직은 무시하고 다시 GQCS 수면하러 돌아감
			}

			int sessionId = static_cast<int>(completionKey);

			switch (ioCtx->opType) {
			case IO_OP::ACCEPT: {
				// AcceptEx로 들어온 소켓을 꺼냄
				SOCKET newClientSocket = ioCtx->acceptSocket;
				
				// AllocateSession()을 불러 빈 방 번호(ID)를 받음
				int allocatedId = AllocateSession(newClientSocket);

				if (allocatedId != -1) {		// 빈 방이 성공적으로 배정되었다면
					// 그 빈 방(소켓)을 hIocp에 등록 (Key로 방 번호인 sessionId를 줌)
					CreateIoCompletionPort((HANDLE)newClientSocket, hIocp, allocatedId, 0);

					// 그 손님에게 WSARecv를 걸어 첫 패킷 수신을 예약
					Session* session = sessions[allocatedId];
					if (session != nullptr) {
						DWORD flags = 0;
						ZeroMemory(&session->recvContext.overlapped, sizeof(WSAOVERLAPPED));
						WSARecv(newClientSocket, &session->recvContext.wsabuf, 1, nullptr, &flags,
							&session->recvContext.overlapped, nullptr);

						// std::cout << "[Accept] New player connected. Assigned Session ID: " << allocatedId << std::endl;

					}
				}
				else {
					closesocket(newClientSocket);	// 방이 꽉 찼으면 소켓을 닫고 돌려보냄
				}

				RegisterAccept();	// 다음 손님을 위해 accept 예약
				delete ioCtx;		// 다 쓴 영수증 폐기
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

					// 1. DB에서 온 택배 상자(ioCtx)의 데이터를 내 세션에 덮어쓰기
					session->level = ioCtx->db_level;
					session->exp = ioCtx->db_exp;
					session->hp = ioCtx->db_hp;
					session->max_hp = 100 + ((session->level - 1) * 50); // 레벨 비례 최대체력
					session->attack_power = 50 + ((session->level - 1) * 50);
					session->x = ioCtx->db_x;
					session->y = ioCtx->db_y;
					session->state = SessionState::INGAME;

					// 2. 클라이언트에 로그인 성공 패킷 및 내 아바타 정보 쏘기
					S2C_LoginResult res = { sizeof(res), S2C_LOGIN_RESULT, true, "DB Login Success!" };
					session->SendPacket(&res);

					S2C_AvatarInfo info = { sizeof(info), S2C_AVATAR_INFO, sessionId, 0, session->x, session->y, session->direction, session->hp, session->max_hp, session->exp, session->level };
					session->SendPacket(&info);

					// 3. 맵(Region) 등록 및 뷰리스트 갱신 (기존 C2S_LOGIN에 있던 로직 그대로)
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
				delete ioCtx; // 다 쓴 택배 상자 폐기
				break;
			}
			}
		}
	}

	void ProcessReceive(int sessionId, DWORD bytesTransferred) {
		// 패킷 재조립 로직
		Session* session = sessions[sessionId];
		if (not session || session->state.load() == SessionState::FREE) return;

		int totalBytes = session->prevRemainBytes + bytesTransferred;
		int readPos = 0;

		while (true) {
			if (totalBytes - readPos < 1) break;

			unsigned char packetSize = session->recvContext.buffer[readPos];		
			// 새롭게 들어온 데이터의 맨 앞 1바이트는 패킷의 크기 정보이므로, 그걸 읽어서 packetSize에 저장
			if (totalBytes - readPos < packetSize) break;
			// 더 읽어와야 되니까 일단 보류. 패킷이 완성되지 않았으니 지금은 처리하지 말고 다음에 더 읽어서 완성되면 처리

			OnPacket(sessionId, &session->recvContext.buffer[readPos]);
			readPos += packetSize;
			// 패킷 처리했으니까 그만큼 인덱스 뒤로 이동
		}

		// 더 받아올 데이터가 남았으면
		int remainBytes = totalBytes - readPos;
		if (remainBytes > 0) {
			memmove(session->recvContext.buffer, &session->recvContext.buffer[readPos], remainBytes);
			// memmove(목적지, 출발지, 크기)
		}
		
		session->prevRemainBytes = remainBytes;
		DWORD flags = 0;
		session->recvContext.wsabuf.len = sizeof(session->recvContext.buffer) - remainBytes;
		session->recvContext.wsabuf.buf = session->recvContext.buffer + remainBytes;

		ZeroMemory(&session->recvContext.overlapped, sizeof(WSAOVERLAPPED));

		WSARecv(session->socket, &session->recvContext.wsabuf, 1, nullptr, &flags, 
			&session->recvContext.overlapped, nullptr);
	}

	void OnPacket(int sessionId, char* packet) {			// 온전한 패킷 1개가 완성되면 이 함수로 던져줌
		Session* session = GetSessionId(sessionId);
		if (not session) return;
		
		PACKET_TYPE type = reinterpret_cast<C2S_Login*>(packet)->type;
		// 클라이언트가 어떤 패킷을 보냈든, 구조체의 메모리 맨 앞에는 무조건 크기(size),
		// 두 번째에는 무조건 타입(type)이 들어있도록 설계했기 때문에, 아무 패킷 구조체(여기서는 제일 만만한 C2S_Login) 로
		// 포인터를 강제 형변환한 뒤 ->type 을 읽으면, 이 패킷이 어떤 패킷인지 알 수 있음

		// 로그인 처리
		if (type == C2S_LOGIN) {
			C2S_Login* loginPacket = reinterpret_cast<C2S_Login*>(packet);

			bool is_duplicate = false;
			for (int i = 0; i < MAX_PLAYERS; ++i) {
				if (i == sessionId) continue; // 나 자신은 제외

				Session* other = sessions[i];
				// 빈 방이 아니고, 들어오려는 이름과 똑같은 이름이 이미 존재한다면
				if (other->state.load() != SessionState::FREE && strcmp(other->name, loginPacket->username) == 0) {
					is_duplicate = true;
					break;
				}
			}

			// 중복이라면 실패 패킷을 보내고 즉시 쫓아냄
			if (is_duplicate) {
				S2C_LoginResult res = { sizeof(res), S2C_LOGIN_RESULT, false, "이미 접속 중인 아이디입니다." };
				session->SendPacket(&res);
				Disconnect(sessionId);
				return; // DB 큐에 넣지 않고 함수 종료
			}
			// -------------------------------------------------------------

			// 중복이 아니면 정상적으로 이름 세팅
			strcpy_s(session->name, loginPacket->username);

			// 1. 스트레스 테스트용 더미 클라이언트는 DB 안 거치고 즉시 접속 처리
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
			// 2. 실제 플레이어가 접속했을 때는 DB 스레드에게 "정보 찾아와!" 하고 지시서(task)를 보냄
			else {
				DbTask task;
				task.type = DbTaskType::LOGIN_CHECK;
				task.session_id = sessionId;
				strcpy_s(task.username, session->name);

				db_queue.push(task); // DB 스레드쪽으로 택배 발송!
			}
		}
		// 인게임 패킷 (이동, 공격)
		else if (session->state.load() == SessionState::INGAME) {
			auto now = std::chrono::steady_clock::now();

			// 이동 패킷 처리
			if (type == C2S_MOVE) {
				auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - session->last_move_time).count();

				int move_cooldown = std::max(200, 500 - (session->level - 1) * 10);
				// 1. 비정상적인 패킷 폭주 감지 (예: 0.5초가 정상인데 0.05초 만에 또 패킷이 옴)
				// 네트워크 지연(핑 튀는 현상)을 고려하여 여유를 조금 둡니다 (예: 100ms 이하로 들어오면 비정상으로 간주)
				if (duration < move_cooldown - 50) {
					// std::cout << "[Warning] Player " << session->name << " is sending packets too fast! Disconnecting..." << std::endl;
					// Disconnect(sessionId); // 즉시 쫓아냄
					return;
				}

				session->last_move_time = now;

				C2S_Move* movePacket = reinterpret_cast<C2S_Move*>(packet);

				short nx = session->x;
				short ny = session->y;

				// 방향에 따른 다음 좌표 계산
				 if (movePacket->direction == 0) ny -= 1;		// Up
				 else if (movePacket->direction == 1) ny += 1;	// Down
				 else if (movePacket->direction == 2) nx -= 1;	// Left
				 else if (movePacket->direction == 3) nx += 1;	// Right

				if (nx >= 0 && nx < WORLD_WIDTH && ny >= 0 && ny < WORLD_HEIGHT) {
					if (g_wall[nx][ny] == false) {		// 벽이 아닐 때만 이동 허가
						if (nx < session->x) session->direction = 2;
						else if (nx > session->x) session->direction = 3;

						MoveObject(sessionId, nx, ny, movePacket->move_time);
					}
				}
			}
			// 공격 패킷 처리
			else if (type == C2S_ATTACK) {
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - session->last_attack_time).count() < 1000) {
					return;		// 공격 쿨타임 안 지났으면 무시
				}
				session->last_attack_time = now;	// 마지막 공격 시간 갱신

				C2S_Attack* atk = reinterpret_cast<C2S_Attack*>(packet);

				// 1. 공격 패킷 뷰리스트에 있는 객체들한테 보내기
				S2C_Action actionPacket = { sizeof(actionPacket), S2C_ACTION, sessionId, ActionType::ACTION_ATTACK };
				BroadcastToViewers(sessionId, &actionPacket);
				session->SendPacket(&actionPacket);	// 공격하는 본인한테도 패킷 보내기

				// 2. 데미지 및 사망 판정 로직 (+경험치)
				int exp_gained = 0;

				// 맵 전체가 아니라 시야 범위 내의 객체들한테만 공격 패킷 보내기
				std::unordered_set<int> my_view;
				{
					std::lock_guard<std::mutex> vl(session->viewLock);
					my_view = session->viewList;
				}

				for (int target_id : my_view) {
					if (!IsPlayer(target_id)) {		// 몬스터만 공격 가능
						Session* npc = GetSessionId(target_id);
						if (npc && npc->state.load() == SessionState::INGAME) {
							// 4방향 판정 (맨해튼 거리 1)
							int dx = abs(session->x - npc->x);
							int dy = abs(session->y - npc->y);
							if (dx + dy == 1) {
								HandleDamage(sessionId, target_id, session->attack_power);
							}
						}
					}
				}

				// 3. 플레이어 경험치 및 레벨업 처리
				if (exp_gained > 0) {
					std::lock_guard<std::mutex> myLock(session->sessionLock);
					session->exp += exp_gained;

					while (session->exp >= session->level * 100) {
						session->exp -= session->level * 100;
						session->level++;
						session->max_hp += 50;				// 레벨업 보상 (최대체력 증가)
						session->hp = session->max_hp;		// 레벨업하면 풀피
						// std::cout << "Level Up! Player " << session->name << " reached level " << session->level << "!" << std::endl;
					}

					// 내 상태 갱신
					S2C_StatusChange myStatus = { sizeof(myStatus), S2C_STATUS_CHANGE, sessionId, session->hp, session->max_hp, session->exp, session->level };
					session->SendPacket(&myStatus);
					BroadcastToViewers(sessionId, &myStatus);
				}
				// session->SendPacket(&actionPacket);	// 공격하는 본인한테도 패킷 보내기
			}
			else if (type == C2S_CHAT) {
				C2S_Chat* chatPacket = reinterpret_cast<C2S_Chat*>(packet);

				if (chatPacket->message[0] == '/') {
					// 1. 순간이동 명령어 (/tp X Y)
					if (strncmp(chatPacket->message, "/tp ", 4) == 0) {
						int tx, ty;
						if (sscanf_s(chatPacket->message + 4, "%d %d", &tx, &ty) == 2) {
							if (tx >= 0 && tx < WORLD_WIDTH && ty >= 0 && ty < WORLD_HEIGHT) {
								MoveObject(sessionId, static_cast<short>(tx), static_cast<short>(ty));
								SendSystemMessage(sessionId, "지정된 좌표로 순간이동 했습니다.");
							}
						}
						return; // 일반 채팅 패킷 처리를 하지 않고 리턴
					}
					// 2. 무적 명령어 (/god)
					else if (strcmp(chatPacket->message, "/god") == 0) {
						session->is_god = !session->is_god;
						if (session->is_god) SendSystemMessage(sessionId, "무적 모드가 활성화되었습니다.");
						else SendSystemMessage(sessionId, "무적 모드가 해제되었습니다.");
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
					// [전체 채팅] 게임 중인 모든 플레이어에게 전송
					for (int i = 0; i < MAX_PLAYERS; ++i) {
						Session* pSession = sessions[i];
						if (pSession && pSession->state.load() == SessionState::INGAME) {
							pSession->SendPacket(&broadcastPacket);
						}
					}
				}
				else if (chatPacket->chatType == 0) {
					BroadcastToViewers(sessionId, &broadcastPacket);
					// session->SendPacket(&broadcastPacket);
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

					db_queue.push(task); // DB 스레드쪽으로 저장 지시!
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