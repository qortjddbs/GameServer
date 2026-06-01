#include <SFML/Graphics.hpp>
#include <iostream>
#include <string>

#include "..\..\텀프\server\protocol_2026.h"
#include "GameObject.h"
#include "GameManager.h"
#include "NetworkManager.h"
#include "ResourceManager.h"
#include "MapManager.h"

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

    if (GameManager::GetInstance().GetObject(p->object_id) != nullptr) return;

    // 타 유저나 NPC가 시야에 들어왔을 때 생성
    auto new_obj = std::make_unique<GameObject>();
	new_obj->id = p->object_id;
	new_obj->setPosition(p->x, p->y);
	strcpy_s(new_obj->name, p->obj_name);

    // NPC
    if (p->object_id >= 10'0000) {
        
    }

    // 2. 이름표(Text) 세팅
    auto& resMgr = ResourceManager::GetInstance();
    new_obj->nameText.setFont(resMgr.GetFont("main_font")); // 폰트 장착
    new_obj->nameText.setString(new_obj->name);             // 글씨 설정
    new_obj->nameText.setCharacterSize(18);                 // 글씨 크기
    new_obj->nameText.setFillColor(sf::Color::White);       // 흰색 글씨
    new_obj->nameText.setOutlineColor(sf::Color::Black);    // 검은 테두리
    new_obj->nameText.setOutlineThickness(1.5f);            // 테두리 두께

    // 글씨의 중심점을 중앙으로 맞춰서 머리 위에 예쁘게 뜨도록 설정
    sf::FloatRect textRect = new_obj->nameText.getLocalBounds();
    new_obj->nameText.setOrigin(textRect.width / 2.0f, textRect.height / 2.0f);

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
        if (p->actionType == 1) obj->doAttack1();
		else if (p->actionType == 2) obj->doAttack2();
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

    if (!resMgr.LoadFont("main_font", "cour.ttf")) {
        cout << "폰트 로드 실패!" << endl;
		return -1;
    }

    if (
        !resMgr.LoadTexture("idle", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Idle.png") ||
        !resMgr.LoadTexture("run", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Run.png") ||
        !resMgr.LoadTexture("attack1", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Attack1.png") ||
        !resMgr.LoadTexture("attack2", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Attack2.png") ||
        !resMgr.LoadTexture("guard", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Guard.png") ||
        !resMgr.LoadTexture("tilemap", "Assets/Tiny Swords/Terrain/Tileset/Tilemap_color1.png") ||
        !resMgr.LoadTexture("shadow", "Assets/Tiny Swords/Terrain/Tileset/Shadow.png")
        ) {
        system("pause");
        return -1;
	}

	MapManager::GetInstance().Initialize(WORLD_WIDTH, WORLD_HEIGHT);

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

    unsigned char comboStep = 0;
    sf::Clock comboResetTimer;

    // ==========================================
    // [메인 게임 루프]
    // ==========================================
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) window.close();

            // 단발성 키 입력 처리 (방어)
            if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::X && guardTimer.getElapsedTime().asSeconds() > 0.3f) {
                C2S_Guard guardPacket = { sizeof(C2S_Guard), C2S_GUARD };
                netMgr.SendPacket(&guardPacket);
                guardTimer.restart();
            }
        }

        // 치다가 말았을 때 0.5초가 지나면 콤보 초기화 (다시 1타부터)
        if (comboStep > 0 && comboStep < 3 && comboResetTimer.getElapsedTime().asSeconds() > 0.5f) {
            comboStep = 0;
		}

        // C키를 꾹 누르고 있다면
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::C)) {
            // 3연타를 마쳤다면 긴 쿨타임, 연타 중이라면 짧은 쿨타임
            float requiredCooldown = (comboStep == 3) ? 0.7f : 0.3f;

            if (attackTimer.getElapsedTime().asSeconds() >= requiredCooldown) {
                C2S_Attack attackPacket;
                memset(&attackPacket, 0, sizeof(attackPacket));
				attackPacket.size = sizeof(attackPacket);
				attackPacket.type = C2S_ATTACK;

                if (comboStep == 0 || comboStep == 3) {
                    attackPacket.attackType = 1; // 1타
                    comboStep = 1;
                }
                else if (comboStep == 1) {
                    attackPacket.attackType = 2; // 2타
                    comboStep = 2;
                }
                else if (comboStep == 2) {
                    attackPacket.attackType = 1;
                    comboStep = 3;
                }

                netMgr.SendPacket(&attackPacket);
                attackTimer.restart();
                comboResetTimer.restart();
            }

		}

		GameObject* myAvatar = gameMgr.GetMyAvatar();

        // 연속 키 입력 처리 (이동)
        if (moveTimer.getElapsedTime().asSeconds() >= 0.1f) {
			bool isComboProgressing = (comboStep > 0 && comboResetTimer.getElapsedTime().asSeconds() <= 0.5f);
            
            if (myAvatar != nullptr && !myAvatar->isActionPlaying && !isComboProgressing) {
                int dir = -1;
                if (sf::Keyboard::isKeyPressed(sf::Keyboard::Up)) dir = 0;
                else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Down)) dir = 1;
                else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Left)) dir = 2;
                else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Right)) dir = 3;

                if (dir != -1) {
                    C2S_Move movePacket;

                    // 메모리 0으로 초기화 (안전한 패킷 전송을 위해 권장되는 관행)
                    memset(&movePacket, 0, sizeof(movePacket));

                    movePacket.size = sizeof(movePacket);
                    movePacket.type = C2S_MOVE;
                    movePacket.direction = dir;

                    netMgr.SendPacket(&movePacket);
                    moveTimer.restart();
                }
            }
        }

        // 1. 네트워크 패킷 수신 및 자동 분배
        netMgr.Receive();

        // 3. 렌더링
        window.clear(sf::Color(78, 131, 151));      // 바다 색

        // 게임 월드 그리기
        if (myAvatar) {
            camera.setCenter(myAvatar->sprite.getPosition());
            window.setView(camera);
		}

		MapManager::GetInstance().Draw(window, camera);     // 맵 타일 그리기
        gameMgr.UpdateAndDrawAll(window); // 애니메이션 갱신 및 화면 출력

		window.setView(window.getDefaultView());

        if (myAvatar) {
            sf::Text coordText;
			coordText.setFont(resMgr.GetFont("main_font"));

            char buf[64];
			sprintf_s(buf, "(%d, %d)", myAvatar->x, myAvatar->y);
			coordText.setString(buf);

            // 글씨 꾸미기
            coordText.setCharacterSize(24);
            coordText.setFillColor(sf::Color::White);           // 흰색 글씨
            coordText.setOutlineColor(sf::Color::Black);        // 검은색 테두리 (배경이 밝아도 잘 보이게)
            coordText.setOutlineThickness(2.0f);

            // 위치 고정 (화면 왼쪽 위)
            coordText.setPosition(15.f, 15.f);

            window.draw(coordText);
		}

        window.display();
    }
	return 0;

}