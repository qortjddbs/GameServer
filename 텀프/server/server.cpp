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
#include <unordered_map>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_queue.h>
#include "protocol_2026.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

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

enum class EventType { EVENT_MOVE, EVENT_RESPAWN, EVENT_DESPAWN };

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

// IO 작업을 관리할 확장 오버랩드 구조체
enum class IO_OP { RECV, SEND, ACCEPT, DO_AI };
struct IOContext
{
	WSAOVERLAPPED overlapped;
	WSABUF wsabuf;
	char buffer[1024];		// 실제 데이터 들어있는 곳
	IO_OP opType;
	SOCKET acceptSocket;	// 새로 들어올 손님의 소켓을 담아둘 바구니

	IOContext(IO_OP op) : opType(op) {
		ZeroMemory(&overlapped, sizeof(overlapped));
		wsabuf.buf = buffer;
		wsabuf.len = sizeof(buffer);
	}
};

// 세션 상태 관리 정의
enum class SessionState { FREE, CONNECTED, INGAME, DEAD };

// 몬스터 AI 설정
enum class MonsterType { SKELETON, GOBLIN, FLYING_EYE, MUSHROOM };
enum class AiType { FIXED_PEACE, ROAMING_AGGRO };

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
	unsigned long long exp = 0;
	unsigned char level = 1;
	char name[MAX_NAME_LEN]{};

	// 몬스터용 변수들
	short origin_x = 0, origin_y = 0;	// 몬스터는 리젠 위치가 고정되어 있으므로 원래 위치 저장
	MonsterType monster_type = MonsterType::SKELETON;
	AiType ai_type = AiType::FIXED_PEACE;
	int target_id = -1;					// 현재 공격 중인 대상

	std::unordered_set<int> viewList;	// 내 화면에 보이고 있는 객체들의 ID
	std::mutex viewLock;				// 시야 목록 전용 자물쇠

	IOContext recvContext{ IO_OP::RECV };
	int prevRemainBytes = 0;

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
	}

	// void* -> 이 포인터가 가리키는 구조체가 뭔진 모르겠지만, 일단 메모리 주소만 넘긴다는 뜻
	// void*를 안쓰면 패킷 종류만큼 오버로딩해서 생성해야됨.
	void SendPacket(void* packet) {
		// 들어온 void*를 unsigned char*로 변환하여 메모리의 맨 앞 1바이트 읽기
		// -> 아 이 구조체가 뭔진 몰라도 크기가 xx바이트라고 파악
		unsigned char* p = reinterpret_cast<unsigned char*>(packet);
		IOContext* sendContext = new IOContext(IO_OP::SEND);
		// OS송신 버퍼 (IOContext->buffer)에 정확히 xx바이트만 복사해 OS로 넘김
		memcpy(sendContext->buffer, p, p[0]);		// void* memcpy(여기에, 이걸, 얼마만큼); 메모리 복사
		sendContext->wsabuf.len = p[0];

		DWORD sentBytes = 0;
		WSASend(socket, &sendContext->wsabuf, 1, &sentBytes, 0, &sendContext->overlapped, nullptr);
		// WSASend(누구한테 보낼건지, 보낼 데이터 버퍼, 버퍼 개수, 참조용 변수, 플래그 (보통 0), send가 완료되었을 때 완료 큐에 넣어줄 영수증, nullptr);
		// 내가 지금 xx바이트짜리 버퍼 줄 테니까 백그라운드에서 전송해달라는 뜻 (비동기)
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

public:
	IocpServer() {
		sessions.resize(MAX_PLAYERS + NUM_NPCS, nullptr);
		// 플레이어용 세션 메모리 할당
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			sessions[i] = new Session();
			sessions[i]->sessionIndex = i;
			player_id_pool.push(i);
		}

		// NPC용 세션 메모리 할당
		for (int i = 0; i < NUM_NPCS; ++i) {
			int real_index = MAX_PLAYERS + i;		// 실제 배열 인덱스
			int npc_id = NPC_ID_START + i;			// 게임 속 NPC의 ID (100만~)

			sessions[real_index] = new Session();
			sessions[real_index]->sessionIndex = real_index;
			sessions[real_index]->id = npc_id;
			sessions[real_index]->state.store(SessionState::INGAME);
			sessions[real_index]->x = rand() % WORLD_WIDTH;
			sessions[real_index]->y = rand() % WORLD_HEIGHT;
			
			// 몬스터 타입 및 리젠 위치 기억
			sessions[real_index]->monster_type = static_cast<MonsterType>(i % 4);
			sessions[real_index]->origin_x = sessions[real_index]->x;
			sessions[real_index]->origin_y = sessions[real_index]->y;

			MonsterType m_type = sessions[real_index]->monster_type;

			if (m_type == MonsterType::SKELETON) {
				sprintf_s(sessions[real_index]->name, "Skeleton_%d", i + 1);
				sessions[real_index]->ai_type = AiType::FIXED_PEACE;
			}
			else if (m_type == MonsterType::GOBLIN) {
				sprintf_s(sessions[real_index]->name, "Goblin_%d", i + 1);
				sessions[real_index]->ai_type = AiType::ROAMING_AGGRO;
			}
			else if (m_type == MonsterType::FLYING_EYE) {
				sprintf_s(sessions[real_index]->name, "Flying_eye_%d", i + 1);
				sessions[real_index]->ai_type = AiType::ROAMING_AGGRO;
			}
			else if (m_type == MonsterType::MUSHROOM) {
				sprintf_s(sessions[real_index]->name, "Mushroom_%d", i + 1);
				sessions[real_index]->ai_type = AiType::FIXED_PEACE;
			}

			short rx = GetRegionX(sessions[real_index]->x);
			short ry = GetRegionY(sessions[real_index]->y);
			{
				std::unique_lock<std::shared_mutex> wl(g_regions[rx][ry].lock);
				g_regions[rx][ry].objects.insert(npc_id);
			}
		}

		// NPC 생성 후 1초~3초 뒤에 움직이라는 지시를 큐에 넣음
		for (int i = 0; i < NUM_NPCS; ++i) {
			int npc_id = NPC_ID_START + i;
			AddTimerEvent(npc_id, EventType::EVENT_MOVE, 1'000);
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

		int threadCount = std::thread::hardware_concurrency(); // 컴퓨터 CPU 코어 개수 반환
		for (int i = 0; i < threadCount; ++i) {
			workers.emplace_back(&IocpServer::WorkerLoop, this);
			// push_back은 객체를 넘기고, emplace_back은 객체 생성에 필요한 인자를 넘김
		}
		// std::cout << "Server Core Successfully Started on Port " << PORT << std::endl;
	}

	void Join() {
		for (auto& th : workers) th.join();
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
		}

		// 기존 시야 복사
		std::unordered_set<int> old_view;
		{
			std::lock_guard<std::mutex> vl(obj->viewLock);
			old_view = obj->viewList;
		}

		// 새로운 시야 계산 (주변 9개 Region 검색)
		std::unordered_set<int> new_view;
		auto near_objs = GetNearbyObjects(nx, ny);
		for (int n_id : near_objs) {
			if (n_id == id) continue;	// 자기 자신은 시야에서 제외
			Session* target = GetSessionId(n_id);
			if (target && target->state.load() == SessionState::INGAME && IsInView(nx, ny, target->x, target->y)) {
				new_view.insert(n_id);
			}
		}

		// 차이점 비교 및 패킷 발송
		for (int n_id : new_view) {
			if (old_view.count(n_id) == 0) {		// 새로 발견
				if (IsPlayer(id)) SendAddObject(id, n_id);	// 내가 유저면 새로 보이는 객체 정보 보내기
				if (IsPlayer(n_id)) SendAddObject(n_id, id);	// 새로 보이는 객체가 유저면 그 유저한테 내 정보 보내기
			}
			else {		// 기존에도 있던 객체
				if (IsPlayer(n_id)) SendMoveObject(n_id, id, nx, ny, move_time);	// 기존에 보이던 객체 위치만 업데이트
			}
		}

		for (int o_id : old_view) {
			if (new_view.count(o_id) == 0) {		// 시야에서 벗어남
				if (IsPlayer(id)) SendRemoveObject(id, o_id);	// 내가 유저면 더이상 보이지 않는 객체 정보 삭제
				if (IsPlayer(o_id)) SendRemoveObject(o_id, id);	// 더이상 보이지 않는 객체가 유저면 그 유저한테 내 정보 제거하라고 보내기
			}
		}

		{
			std::lock_guard<std::mutex> vl(obj->viewLock);
			obj->viewList = new_view;
		}

		if (IsPlayer(id)) SendMoveObject(id, id, nx, ny, move_time);	// 내가 유저면 내 위치 업데이트 패킷도 보내기
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

		// 내 뷰리스트 비우기
		{
			std::lock_guard<std::mutex> vl(target->viewLock);
			target->viewList.clear();
		}

		std::cout << "npc " << id << " is killed." << std::endl;
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

		short nx = npc->x;
		short ny = npc->y;
		bool hasAggroTarget = false;
		short targetX = -1, targetY = -1;

		// 타겟 탐색
		if (npc->ai_type == AiType::ROAMING_AGGRO) {
			std::unordered_set<int> my_view;
			{
				std::lock_guard<std::mutex> vl(npc->viewLock);
				my_view = npc->viewList;
			}

			int closestDist = 9999;
			for (int obj_id : my_view) {
				if (IsPlayer(obj_id)) {
					Session* player = GetSessionId(obj_id);
					if (player && player->state.load() == SessionState::INGAME) {
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

			// 몬스터 사거리 판단
			int attack_range = 1;

			if (dist <= attack_range) {
				// 사거리 안으로 들어왔으면 공격
				
				// 위치는 제자리 고정
				nx = npc->x;
				ny = npc->y;

				// 몬스터 공격
				S2C_Action actionPacket = { sizeof(actionPacket), S2C_ACTION, npc->id, 1 };
				BroadcastToViewers(npc->id, &actionPacket);

				// 플레이어 데미지 처리
				Session* p = GetSessionId(npc->target_id);
				if (p && p->state.load() == SessionState::INGAME) {
					bool isDead = false;	// 데드락 방지용 플래그

					{
						std::lock_guard<std::mutex> pLock(p->sessionLock);
						p->hp -= 20;
						if (p->hp <= 0) {
							p->hp = p->max_hp;
							p->exp /= 2;
							isDead = true;
							npc->target_id = -1;
						}
					}
						if (isDead) {
							std::cout << "[Death] Player " << p->id << " died. Sent to town." << std::endl;
							MoveObject(p->id, 0, 0);

							S2C_StatusChange statusPacket = { sizeof(statusPacket), S2C_STATUS_CHANGE, p->id, p->hp, p->max_hp, p->exp, p->level };
							p->SendPacket(&statusPacket);
							BroadcastToViewers(p->id, &statusPacket);
						}
						else {
							// 안죽었으면 피격 패킷
							S2C_Action hitPacket = { sizeof(hitPacket), S2C_ACTION, p->id, 5 };
							BroadcastToViewers(p->id, &hitPacket);

							S2C_StatusChange statusPacket = { sizeof(statusPacket), S2C_STATUS_CHANGE, p->id, p->hp, p->max_hp, p->exp, p->level };
							p->SendPacket(&statusPacket);
							BroadcastToViewers(p->id, &statusPacket);
						}
				}
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

		AddTimerEvent(npc_id, EventType::EVENT_MOVE, 500);	// 다음 이동 예약 (0.5초마다 이동)
	}

	void ProcessNpcRespawn(int npc_id) {
		Session* npc = GetSessionId(npc_id);
		if (!npc) return;

		std::lock_guard<std::mutex> lock(npc->sessionLock);
		npc->hp = npc->max_hp;
		npc->x = rand() % WORLD_WIDTH;
		npc->y = rand() % WORLD_HEIGHT;
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
		std::cout << "NPC " << npc_id << " respawned at (" << npc->x << ", " << npc->y << ")" << std::endl;
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

				delete ioCtx;
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
			strcpy_s(session->name, loginPacket->username);
			session->x = rand() % WORLD_WIDTH;
			session->y = rand() % WORLD_HEIGHT;
			session->state = SessionState::INGAME;

			// 로그인 성공 및 아바타 정보 전송
			S2C_LoginResult res = { sizeof(res), S2C_LOGIN_RESULT, true, "Welcome!" };
			session->SendPacket(&res);

			S2C_AvatarInfo info = { sizeof(info), S2C_AVATAR_INFO, sessionId, 0, session->x, session->y, session->direction, session->hp, session->max_hp, session->exp, session->level };
			session->SendPacket(&info);

			// 로그인 시 현재 좌표를 Region에 등록 + 주변 객체 목록 갱신
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
					std::lock_guard<std::mutex> vl(session->viewLock);
					session->viewList.insert(n_id);

					SendAddObject(sessionId, n_id);	// 나한테 새로 보이는 객체 정보 보내기
					if (IsPlayer(n_id)) SendAddObject(n_id, sessionId);	// 새로 보이는 객체가 유저면 그 유저한테 내 정보 보내기
				}
			}

			// std::cout << "[Login] Player " << session->name << " entered at (" << session->x << ", " << session->y << ")" << std::endl;
		}
		// 인게임 패킷 (이동, 공격)
		else if (session->state.load() == SessionState::INGAME) {
			// 이동 패킷 처리
			if (type == C2S_MOVE) {
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
				C2S_Attack* atk = reinterpret_cast<C2S_Attack*>(packet);

				// 1. 공격 패킷 뷰리스트에 있는 객체들한테 보내기
				S2C_Action actionPacket = { sizeof(actionPacket), S2C_ACTION, sessionId, atk->attackType };
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
								std::lock_guard<std::mutex> npcLock(npc->sessionLock);
								npc->hp -= 50;	// 임시 데미지

								// 몬스터가 데미지 입고 살아남았고, 어그로가 아니었으면
								if (npc->hp > 0 && npc->target_id == -1) {
									npc->target_id = sessionId;
								}

								if (npc->hp <= 0) {
									npc->hp = 0;
									npc->state.store(SessionState::DEAD);			// 시체 상태로 만들어 무적 판정
									
									exp_gained += (npc->level * npc->level * 2);	// 경험치 획득량 고정 (요구사항)

									S2C_Action deadPacket = { sizeof(deadPacket), S2C_ACTION, target_id, 6 };	// 공격당한 몬스터한테 죽었다는 액션 패킷 보내기
									BroadcastToViewers(target_id, &deadPacket);

									AddTimerEvent(target_id, EventType::EVENT_DESPAWN, 1000);	// 1초 뒤에 시체 제거
									AddTimerEvent(target_id, EventType::EVENT_RESPAWN, 3'0000);	// 30초 뒤에 리스폰
								}
								else {
									// 피격 모션 전송
									S2C_Action hitPacket = { sizeof(hitPacket), S2C_ACTION, target_id, 5 };	// 공격당한 몬스터한테 피격 모션 패킷 보내기
									BroadcastToViewers(target_id, &hitPacket);

									S2C_StatusChange statusPacket = { sizeof(statusPacket), S2C_STATUS_CHANGE, target_id, npc->hp, npc->max_hp, npc->exp, npc->level };
									BroadcastToViewers(target_id, &statusPacket);
								}
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
						std::cout << "Level Up! Player " << session->name << " reached level " << session->level << "!" << std::endl;
					}

					// 내 상태 갱신
					S2C_StatusChange myStatus = { sizeof(myStatus), S2C_STATUS_CHANGE, sessionId, session->hp, session->max_hp, session->exp, session->level };
					session->SendPacket(&myStatus);
					BroadcastToViewers(sessionId, &myStatus);
				}
				// session->SendPacket(&actionPacket);	// 공격하는 본인한테도 패킷 보내기
			} 
		}
	}

	void Disconnect(int id) {
		Session* session = GetSessionId(id);
		if (session) {
			// delete 하지 않고 상태만 비운 다음 번호표 반납
			session->Reset();
			if (id < MAX_PLAYERS) player_id_pool.push(id);
		}
	}
};

int main()
{
	InitServerMap();

	IocpServer server;
	if (server.Initialize()) {
		server.Start();
		server.Join();
	}
}