#pragma once
constexpr short PORT = 3500;
constexpr int WORLD_WIDTH = 2000;
constexpr int WORLD_HEIGHT = 2000;
constexpr int MAX_PLAYERS = 10000;
constexpr int NUM_NPCS = 20'0000;
constexpr int NPC_ID_START = 1000000;
constexpr int NPC_MOVE_INTERVAL = 1000; // in milliseconds
constexpr int MAX_NAME_LEN = 20;
constexpr int MAX_CHAT_MSG_LEN = 200;

enum PACKET_TYPE {
	C2S_LOGIN,			// Client to Server: Login request
		
	C2S_MOVE,			// Client to Server: Move request
	
	C2S_CHAT,			// Client to Server: Chat message
	
	C2S_ATTACK,			// Client to Server: Attack request
	
	C2S_TELEPORT,		// Client to Server: Teleport request
	

	C2S_LOGOUT,			// Client to Server: Logout request

	S2C_LOGIN_RESULT,	//	Server to Client: Login result

	S2C_AVATAR_INFO,	//	Server to Client: Avatar information
	S2C_ADD_OBJECT,		//	Server to Client: Add player or NPC		
	S2C_REMOVE_OBJECT,	//	Server to Client: Remove player or NPC
	S2C_MOVE_OBJECT,	//	Server to Client: Move player or NPC
	S2C_CHAT_MESSAGE,	//	Server to Client: Chat message
	S2C_STATUS_CHANGE,	//	Server to Client: Update player or NPC status (e.g., health, buffs)	

	S2C_ACTION,		
	S2C_SKILL_EFFECT,
	S2C_ADD_ITEM,
};

#pragma pack(push, 1) // Ensure no padding between struct members
struct C2S_Login {
	unsigned char size;
	PACKET_TYPE   type;
	char username[MAX_NAME_LEN];
};

struct C2S_Move {
	unsigned char size;
	PACKET_TYPE   type;
	unsigned char direction; // 방향 추가 - 0: Up, 1: Down, 2: Left, 3: Right
	short x;
	short y;
	int move_time; // in milliseconds
};

struct C2S_Chat {
	unsigned char size;
	PACKET_TYPE   type;
	char message[MAX_CHAT_MSG_LEN];
	char chatType;
};

struct C2S_Attack {
	unsigned char size;
	PACKET_TYPE   type;
	unsigned char attackType;
};

struct C2S_Teleport {
	unsigned char size;
	PACKET_TYPE   type;
	short x;
	short y;
};

struct C2S_Logout {
	unsigned char size;
	PACKET_TYPE   type;
};

struct S2C_LoginResult {
	unsigned char size;
	PACKET_TYPE   type;
	bool success;
	char message[50];
};

struct S2C_AvatarInfo {
	unsigned char size;
	PACKET_TYPE   type;
	int playerId;
	int visualId; // for future use (different visual appearances)
	short x;
	short y;
	unsigned char direction; // 방향 추가 - 0: Up, 1: Down, 2: Left, 3: Right
	int hp;
	int max_hp;
	unsigned long long exp;
	unsigned char level;
};

struct S2C_AddObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	int visual_id; // for future use (different visual appearances)
	char obj_name[MAX_NAME_LEN];
	short x;
	short y;
	unsigned char direction; // 방향 추가 - 0: Up, 1: Down, 2: Left, 3: Right
	int hp;
	int max_hp;
	unsigned long long exp;
	unsigned char level;
};

struct S2C_RemoveObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
};

struct S2C_MoveObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	short x;
	short y;
	int move_time; // in milliseconds
};

struct S2C_ChatMessage {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	char message[MAX_CHAT_MSG_LEN];
	char chatType;
};

struct S2C_StatusChange {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	int hp;
	int max_hp;
	unsigned long long exp;
	unsigned char level;
};

// 추가 확장한 프로토콜
struct S2C_Action {
	unsigned char size;
	PACKET_TYPE type;
	int object_id;
	unsigned char actionType;
};

struct S2C_SkillEffect {
	unsigned char size;
	PACKET_TYPE type;
	int object_id;      
	short x;            
	short y;            
	unsigned char effect_type;
};

struct S2C_AddItem {
	unsigned char size;
	PACKET_TYPE type;
	int item_id;
	short x;           
	short y;           
	unsigned char item_type;
};

#pragma pack(pop) // Restore default packing