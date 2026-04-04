#pragma once

constexpr short PORT = 3500;
constexpr int WORLD_WIDTH = 8;
constexpr int WORLD_HEIGHT = 8;
constexpr int MAX_PLAYERS = 10;
constexpr int MAX_NAME_LEN = 20;

enum PACKET_TYPE { C2S_LOGIN, C2S_MOVE, S2C_LOGIN_RESULT, S2C_AVATAR_INFO, S2C_ADD_PLAYER, S2C_REMOVE_PLAYER, S2C_MOVE_PLAYER };
enum DIRECTION { UP, DOWN, LEFT, RIGHT };

#pragma pack(push, 1)	// 패킷 구조체의 크기를 최소화하기 위해 1바이트 단위로 정렬
struct C2S_Login {
	unsigned char size;		// size가 마이너스일 리 없기 때문에 unsigned로 선언
	PACKET_TYPE type;		// 패킷의 종류
	char username[MAX_NAME_LEN];
};

struct C2S_Move {
	unsigned char size;
	PACKET_TYPE type;
	DIRECTION dir;
};

struct S2C_LoginResult {
	unsigned char size;
	PACKET_TYPE type;
	bool success;
	char message[50];		// 실패했을 때 이유를 담는 메시지
};

struct S2C_AvatarInfo {
	unsigned char size;
	PACKET_TYPE type;
	int playerId;
	short x;	// 가능하면 크기 최소화 (char는 너무 작음. 월드가 커지면 감당 안됨)
	short y;	// unsigned로 해도 되는데 월드의 중앙이 (0, 0)이라고 생각하면 음수도 필요할 수 있음
};

struct S2C_AddPlayer {
	unsigned char size;
	PACKET_TYPE type;
	int playerId;	// short로 하면 동접 최대 32767명까지만 지원. 실무에서는 long long 사용
	char username[MAX_NAME_LEN];
	short x;
	short y;
};

struct S2C_RemovePlayer {
	unsigned char size;
	PACKET_TYPE type;
	int playerId;
};

struct S2C_MovePlayer {
	unsigned char size;
	PACKET_TYPE type;
	int playerId;
	short x;
	short y;
};
#pragma pack(pop)	// 원래의 정렬 방식으로 되돌림