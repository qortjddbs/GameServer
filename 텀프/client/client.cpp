#include <SFML/Graphics.hpp>
#include <iostream>
#include <string>

#include "..\..\텀프\server\protocol_2026.h"
#include "GameObject.h"
#include "GameManager.h"
#include "NetworkManager.h"
#include "ResourceManager.h"

using namespace std;

constexpr int WINDOW_WIDTH = 1024;
constexpr int WINDOW_HEIGHT = 768;

// ==========================================
// 1. 패킷 처리 콜백 함수들 (switch-case 완벽 대체)
// ==========================================
void OnLoginResult(char* packet) {
	auto p = reinterpret_cast<S2C_LoginResult*>(packet);
    if (p->success) cout << "[서버] 로그인 성공: " << p->message << endl;
	else cout << "[서버] 로그인 실패!" << endl;
}

void OnAvatarInfo(char* packet) {
    auto p = reinterpret_cast<S2C_AvatarInfo*>(packet);

    auto& gameMgr = GameManager::GetInstance();
    gameMgr.SetMyId(p->playerId);

    auto my_avatar = std::make_unique<GameObject>();
	my_avatar->id = p->playerId;
	my_avatar->hp = p->hp;
	my_avatar->max_hp = p->max_hp;
	my_avatar->exp = p->exp;
    my_avatar->level = p->level;
    my_avatar->setPosition(p->x, p->y);

    gameMgr.AddObject(p->playerId, std::move(my_avatar));
	cout << "내 아바타 생성 완료! (ID: " << p->playerId << ")" << endl;
}

void OnAddObject(char* packet) {
	auto p = reinterpret_cast<S2C_AddObject*>(packet);

    // 타 유저나 NPC가 시야에 들어왔을 때 생성
    auto new_obj = std::make_unique<GameObject>();
	new_obj->id = p->object_id;
	new_obj->setPosition(p->x, p->y);
	strcpy_s(new_obj->name, p->obj_name);
    // (필요 시 이름표 출력 설정 등 추가)

    GameManager::GetInstance().AddObject(p->object_id, std::move(new_obj));
}

void OnRemoeObject(char* packet) {
    auto p = reinterpret_cast<S2C_RemoveObject*>(packet);
    // 시야에서 벗어나거나 접속 종료 시 자동 삭제 및 메모리 해제
    GameManager::GetInstance().RemoveObject(p->object_id);
}

void OnMoveObject(char* packet) {
    auto p = reinterpret_cast<S2C_MoveObject*>(packet);
	GameObject* obj = GameManager::GetInstance().GetObject(p->object_id);
	if (obj) obj->setPosition(p->x, p->y);
}

void OnAction(char* packet) {
    auto p = reinterpret_cast<S2C_Action*>(packet);
    GameObject* obj = GameManager::GetInstance().GetObject(p->object_id);
    if (obj) {
        if (p->actionType == 1) obj->doAttack();
        else if (p->actionType == 3) obj->doGuard();
    }
}

// ==========================================
// 2. 메인 게임 함수
// ==========================================
int main() {
    wcout.imbue(locale("korean"));

    // ==========================================
    // [초기화 1] 리소스 로드
    // ==========================================
	auto& resMgr = ResourceManager::GetInstance();
    if (!resMgr.LoadTexture("idle", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Idle.png") ||
        !resMgr.LoadTexture("run", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Attack1.png") ||
        !resMgr.LoadTexture("attack1", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Attack2.png") ||
        !resMgr.LoadTexture("guard", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Guard.png") ||
        !resMgr.LoadTexture("guard", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Run.png")) {
        system("pause");
        return -1;
	}

    // ==========================================
    // [초기화 2] 네트워크 설정 및 콜백(핸들러) 등록
	// ==========================================
    auto& netMgr = NetworkManager::GetInstance();
	netMgr.RegisterHandler(S2C_LOGIN_RESULT, OnLoginResult);
	netMgr.RegisterHandler(S2C_AVATAR_INFO, OnAvatarInfo);
	netMgr.RegisterHandler(S2C_ADD_OBJECT, OnAddObject);
	netMgr.RegisterHandler(S2C_REMOVE_OBJECT, OnRemoeObject);
	netMgr.RegisterHandler(S2C_MOVE_OBJECT, OnMoveObject);
    netMgr.RegisterHandler(S2C_ACTION, OnAction);
	
    // ==========================================
	// [초기화 3] 서버 접속 및 로그인
	// ==========================================
    string server_ip = "127.0.0.1";
    cout << "서버에 접속합니다 (" << server_ip << ")..." << endl;

    if (!netMgr.Connect(server_ip, PORT)) {
        cout << "서버 연결 실패!" << endl;
        system("pause");
        return -1;
    }

    C2S_Login loginPacket = { sizeof(C2S_Login), C2S_LOGIN, "MyPlayer" };
    netMgr.SendPacket(&loginPacket);

	// ==========================================
    // [초기화 4] 윈도우 창 및 게임 객체 매니저 세팅
    // ==========================================
    sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "2026 Term Proejct Client");
    window.setFramerateLimit(60);
	sf::View camera(sf::FloatRect(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT));

	auto& gameMgr = GameManager::GetInstance();

    sf::Clock moveTimer;
	sf::Clock attackTimer;
	sf::Clock guardTimer;

    // ==========================================
    // [메인 게임 루프]
    // ==========================================
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) window.close();

            // 단발성 키 입력 처리 (공격, 방어)
            if (event.type == sf::Event::KeyPressed) {
                if (event.key.code == sf::Keyboard::C && attackTimer.getElapsedTime().asSeconds() > 1.0f) {
                    C2S_Attack attackPacket = { sizeof(C2S_Attack), C2S_ATTACK };
                    netMgr.SendPacket(&attackPacket);
                    attackTimer.restart();
                }
                else if (event.key.code == sf::Keyboard::X && guardTimer.getElapsedTime().asSeconds() > 1.0f) {
                    C2S_Guard guardPacket = { sizeof(C2S_Guard), C2S_GUARD };
                    netMgr.SendPacket(&guardPacket);
                    guardTimer.restart();
                }
			}
        }

        // 연속 키 입력 처리 (이동)
        if (moveTimer.getElapsedTime().asSeconds() >= 0.1f) {
            int dir = -1;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Up)) dir = 0;
            else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Down)) dir = 1;
            else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Left)) dir = 2;
			else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Right)) dir = 3;

            if (dir != -1) {
                C2S_Move movePacket;
				movePacket.size = sizeof(movePacket);
				movePacket.type = C2S_MOVE;
                movePacket.direction = dir;

                netMgr.SendPacket(&movePacket);
				moveTimer.restart();
            }
        }

        // 1. 네트워크 패킷 수신 및 자동 분배
        netMgr.Receive();

        // 2. 카메라 추적 (내 아바타 중심)
        GameObject* myAvatar = gameMgr.GetMyAvatar();
        if (myAvatar) {
            camera.setCenter(myAvatar->sprite.getPosition());
            window.setView(camera);
        }

        // 3. 렌더링
        window.clear(sf::Color(50, 50, 50));
        gameMgr.UpdateAndDrawAll(window); // 애니메이션 갱신 및 화면 출력
        window.display();
    }
	return 0;

}