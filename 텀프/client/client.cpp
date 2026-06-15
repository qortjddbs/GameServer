#include <SFML/Graphics.hpp>
#include <SFML/System/String.hpp>
#include <iostream>
#include <deque>
#include <string>
#include <locale.h>
#include <windows.h>
#include <ctime>

#include "..\..\텀프\server\protocol_2026.h"
#include "GameObject.h"
#include "GameManager.h"
#include "NetworkManager.h"
#include "ResourceManager.h"
#include "MapManager.h"

using namespace std;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 1280;

struct ChatMessage {
    sf::String text;
    sf::Clock timer;
};

std::deque<ChatMessage> g_chatLog;

std::wstring AnsiToWide(const std::string& ansiStr) {
    if (ansiStr.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_ACP, 0, &ansiStr[0], (int)ansiStr.size(), NULL, 0);
    std::wstring wideStr(size_needed, 0);
    MultiByteToWideChar(CP_ACP, 0, &ansiStr[0], (int)ansiStr.size(), &wideStr[0], size_needed);
    return wideStr;
}

std::string WideToAnsi(const std::wstring& wideStr) {
    if (wideStr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_ACP, 0, &wideStr[0], (int)wideStr.size(), NULL, 0, NULL, NULL);
    std::string ansiStr(size_needed, 0);
    WideCharToMultiByte(CP_ACP, 0, &wideStr[0], (int)wideStr.size(), &ansiStr[0], size_needed, NULL, NULL);
    return ansiStr;
}

bool LoadAllResources() {
	auto& resMgr = ResourceManager::GetInstance();

    // 폰트 로드
    if (!resMgr.LoadFont("main_font", "malgun.ttf")) {
        cout << L"폰트 로드 실패!" << endl;
        return false;
    }

    // 플레이어 로드
    if (!resMgr.LoadTexture("player_idle", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Idle.png") ||
        !resMgr.LoadTexture("player_run", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Run.png") ||
        !resMgr.LoadTexture("player_attack1", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Attack1.png") ||
        !resMgr.LoadTexture("player_attack2", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Attack2.png") ||
        !resMgr.LoadTexture("player_guard", "Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Guard.png")) {
		cout << "플레이어 로드 실패!" << endl;
        return false;
    }

	// 몬스터 로드
    // [해골]
    if (!resMgr.LoadTexture("skeleton_idle", "Assets/Monsters Creatures Fantasy/Sprites/Skeleton/Idle.png") ||
        !resMgr.LoadTexture("skeleton_attack1", "Assets/Monsters Creatures Fantasy/Sprites/Skeleton/Attack1.png") ||
        !resMgr.LoadTexture("skeleton_attack2", "Assets/Monsters Creatures Fantasy/Sprites/Skeleton/Attack2.png") ||
        !resMgr.LoadTexture("skeleton_death", "Assets/Monsters Creatures Fantasy/Sprites/Skeleton/Death.png") ||
        !resMgr.LoadTexture("skeleton_guard", "Assets/Monsters Creatures Fantasy/Sprites/Skeleton/Shield.png") ||
        !resMgr.LoadTexture("skeleton_hit", "Assets/Monsters Creatures Fantasy/Sprites/Skeleton/Take Hit.png") ||
        !resMgr.LoadTexture("skeleton_walk", "Assets/Monsters Creatures Fantasy/Sprites/Skeleton/Walk.png")) {
        cout << "해골 로드 실패!" << endl;
        return false;
    }

    // [고블린]
    if (!resMgr.LoadTexture("goblin_idle", "Assets/Monsters Creatures Fantasy/Sprites/Goblin/Idle.png") ||
        !resMgr.LoadTexture("goblin_attack1", "Assets/Monsters Creatures Fantasy/Sprites/Goblin/Attack1.png") ||
        !resMgr.LoadTexture("goblin_attack2", "Assets/Monsters Creatures Fantasy/Sprites/Goblin/Attack2.png") ||
        !resMgr.LoadTexture("goblin_death", "Assets/Monsters Creatures Fantasy/Sprites/Goblin/Death.png") ||
        !resMgr.LoadTexture("goblin_hit", "Assets/Monsters Creatures Fantasy/Sprites/Goblin/Take Hit.png") ||
        !resMgr.LoadTexture("goblin_walk", "Assets/Monsters Creatures Fantasy/Sprites/Goblin/Run.png")) {
        cout << "고블린 로드 실패!" << endl;
        return false;
    }

    // [박쥐]
    if (!resMgr.LoadTexture("flying_eye_flight", "Assets/Monsters Creatures Fantasy/Sprites/Flying eye/Flight.png") ||
        !resMgr.LoadTexture("flying_eye_attack1", "Assets/Monsters Creatures Fantasy/Sprites/Flying eye/Attack1.png") ||
        !resMgr.LoadTexture("flying_eye_attack2", "Assets/Monsters Creatures Fantasy/Sprites/Flying eye/Attack2.png") ||
        !resMgr.LoadTexture("flying_eye_death", "Assets/Monsters Creatures Fantasy/Sprites/Flying eye/Death.png") ||
        !resMgr.LoadTexture("flying_eye_hit", "Assets/Monsters Creatures Fantasy/Sprites/Flying eye/Take Hit.png")) {
        cout << "박쥐 로드 실패!" << endl;
        return false;
    }

    // [버섯]
    if (!resMgr.LoadTexture("mushroom_idle", "Assets/Monsters Creatures Fantasy/Sprites/mushroom/Idle.png") ||
        !resMgr.LoadTexture("mushroom_attack1", "Assets/Monsters Creatures Fantasy/Sprites/mushroom/Attack1.png") ||
        !resMgr.LoadTexture("mushroom_attack2", "Assets/Monsters Creatures Fantasy/Sprites/mushroom/Attack2.png") ||
        !resMgr.LoadTexture("mushroom_death", "Assets/Monsters Creatures Fantasy/Sprites/mushroom/Death.png") ||
        !resMgr.LoadTexture("mushroom_hit", "Assets/Monsters Creatures Fantasy/Sprites/mushroom/Take Hit.png") ||
        !resMgr.LoadTexture("mushroom_walk", "Assets/Monsters Creatures Fantasy/Sprites/mushroom/Run.png")) {
        cout << "버섯 로드 실패!" << endl;
        return false;
    }

    // 배경 타일맵 로드
    if (!resMgr.LoadTexture("tilemap", "Assets/Tiny Swords/Terrain/Tileset/Tilemap_color1.png") ||
        !resMgr.LoadTexture("shadow", "Assets/Tiny Swords/Terrain/Tileset/Shadow.png") ||
        !resMgr.LoadTexture("rock", "Assets/Tiny Swords/Terrain/Decorations/Rocks/Rock2.png")) {
		cout << "맵 로드 실패!" << endl;
		return false;
    }

    if (!resMgr.LoadTexture("attack_effect", "Assets/Tiny Swords/Particle FX/Fire_02.png")) {
        cout << "공격 이펙트 로드 실패!" << endl;
		return false;
    }

	return true;
}

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

	my_avatar->nameText.setFont(ResourceManager::GetInstance().GetFont("main_font"));
	my_avatar->nameText.setCharacterSize(30);
	my_avatar->nameText.setFillColor(sf::Color::Yellow);
	my_avatar->nameText.setOutlineColor(sf::Color::Black);
	my_avatar->nameText.setOutlineThickness(1.5f);

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
	strcpy_s(new_obj->name, p->obj_name);

    new_obj->hp = p->hp;
    new_obj->max_hp = p->max_hp;
    new_obj->level = p->level;
    new_obj->exp = p->exp;

    new_obj->nameText.setFont(ResourceManager::GetInstance().GetFont("main_font"));
    new_obj->nameText.setCharacterSize(30);
    new_obj->nameText.setFillColor(sf::Color::White);
    new_obj->nameText.setOutlineColor(sf::Color::Black);
    new_obj->nameText.setOutlineThickness(1.5f);
    new_obj->maxFrames = 4;

    // NPC
    if (p->object_id >= 10'0000) {
        if (strstr(new_obj->name, "Skeleton"))              new_obj->objectType = ObjectType::SKELETON;
        else if (strstr(new_obj->name, "Goblin"))           new_obj->objectType = ObjectType::GOBLIN;
        else if (strstr(new_obj->name, "Flying_eye"))       new_obj->objectType = ObjectType::FLYING_EYE;
        else if (strstr(new_obj->name, "Mushroom"))         new_obj->objectType = ObjectType::MUSHROOM;

        new_obj->frameWidth = 150;
		new_obj->frameHeight = 150;
		new_obj->scale = 1.5f;
		new_obj->sprite.setScale(new_obj->scale, new_obj->scale);
		new_obj->sprite.setOrigin(75.0f, 75.0f);

        if (new_obj->objectType == ObjectType::SKELETON)            new_obj->sprite.setTexture(ResourceManager::GetInstance().GetTexture("skeleton_idle"));
        else if (new_obj->objectType == ObjectType::GOBLIN)         new_obj->sprite.setTexture(ResourceManager::GetInstance().GetTexture("goblin_idle"));
        else if (new_obj->objectType == ObjectType::FLYING_EYE)     new_obj->sprite.setTexture(ResourceManager::GetInstance().GetTexture("flying_eye_flight"));
        else if (new_obj->objectType == ObjectType::MUSHROOM)       new_obj->sprite.setTexture(ResourceManager::GetInstance().GetTexture("mushroom_idle"));
    }
    else {
		new_obj->objectType = ObjectType::PLAYER;
    }

    new_obj->setPosition(p->x, p->y);
    new_obj->setDirection(p->direction);

	new_obj->sprite.setTextureRect(sf::IntRect(0, 0, new_obj->frameWidth, new_obj->frameHeight));

    GameManager::GetInstance().AddObject(p->object_id, std::move(new_obj));
}

void OnRemoveObject(char* packet) {
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
        if (p->actionType == 1) {
            obj->doAttack();

            if (obj->objectType == ObjectType::PLAYER) {
                auto& gameMgr = GameManager::GetInstance();
                gameMgr.AddAttackEffect(obj->x, obj->y - 1);
                gameMgr.AddAttackEffect(obj->x, obj->y + 1);
                gameMgr.AddAttackEffect(obj->x - 1, obj->y);
                gameMgr.AddAttackEffect(obj->x + 1, obj->y);
            }
            else {
				GameObject* monsterAvatar = GameManager::GetInstance().GetMyAvatar();
                if (monsterAvatar) {
					int dist = abs(obj->x - monsterAvatar->x) + abs(obj->y - monsterAvatar->y);
                    if (dist <= 2) {
                        if (monsterAvatar->x < obj->x) obj->setDirection(2);
						else if (monsterAvatar->x > obj->x) obj->setDirection(3);
                    }
                }
            }
        }
        else if (p->actionType == 5) obj->doHit();
        else if (p->actionType == 6) obj->doDeath();
    }
}

// 상태 변경을 처리하는 함수
void OnStatusChange(char* packet) {
	auto p = reinterpret_cast<S2C_StatusChange*>(packet);
	GameObject* obj = GameManager::GetInstance().GetObject(p->object_id);

    if (obj) {
		obj->hp = p->hp;
		obj->max_hp = p->max_hp;
		obj->exp = p->exp;
		obj->level = p->level;

        // 내 캐릭터 정보가 갱신됐으면 콘솔에 출력
        if (obj->id == GameManager::GetInstance().GetMyId()) {
            cout << "내 상태 변경 - HP: " << obj->hp << "/" << obj->max_hp
                 << ", EXP: " << obj->exp
                 << ", Level: " << static_cast<int>(obj->level) << endl;
		}
    }
}

void OnChat(char* packet) {
	auto p = reinterpret_cast<S2C_ChatMessage*>(packet);

    time_t t = time(nullptr);
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    char timeBuf[32];
    sprintf_s(timeBuf, " (%02d:%02d)", tm_info.tm_hour, tm_info.tm_min);

    sf::String timeString = sf::String(AnsiToWide(timeBuf));
    sf::String messageString = sf::String(AnsiToWide(p->message));

    // [핵심] 해당 캐릭터(object_id)를 찾아서 말풍선 글씨를 덮어씌움 (새로 입력 시 즉각 반영)
    GameObject* obj = GameManager::GetInstance().GetObject(p->object_id);
    if (obj) {
        obj->chatMsg = messageString;
        obj->chatTimer.restart(); // 타이머 초기화 (다시 5초간 보여줌)
    }

    // 왼쪽 아래 글로벌 채팅 로그도 유지 (전체/주변 말머리 달아서 직관적으로)
    ChatMessage msg;
    sf::String prefix = (p->chatType == 1) ? sf::String(L"[전체] ") : sf::String(L"[지역] ");
    msg.text = prefix + messageString + timeString;
    msg.timer.restart();

    g_chatLog.push_back(msg);
    if (g_chatLog.size() > 10) g_chatLog.pop_front();
}

// ==========================================
// 2. 메인 게임 함수
// ==========================================
int main() {
	setlocale(LC_ALL, "korean");
    wcout.imbue(locale("korean"));

    // ==========================================
    // [초기화 1] 리소스 로드
    // ==========================================
    if (!LoadAllResources()) {
        std::cout << L"리소스 로드 실패! 경로를 확인하세요." << std::endl;
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
	netMgr.RegisterHandler(S2C_REMOVE_OBJECT, OnRemoveObject);
	netMgr.RegisterHandler(S2C_MOVE_OBJECT, OnMoveObject);
    netMgr.RegisterHandler(S2C_ACTION, OnAction);
	netMgr.RegisterHandler(S2C_STATUS_CHANGE, OnStatusChange);
	netMgr.RegisterHandler(S2C_CHAT_MESSAGE, OnChat);
	
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

    bool isChatting = false;
    bool isGlobalChat = false;
    sf::String currentChatInput;

	auto& gameMgr = GameManager::GetInstance();

    sf::Clock moveTimer;
	sf::Clock attackTimer;

    unsigned char comboStep = 0;
    sf::Clock comboResetTimer;
    char lastDir = -1;

    // ==========================================
    // [메인 게임 루프]
    // ==========================================
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) window.close();

            if (event.type == sf::Event::KeyPressed) {
                if (event.key.code == sf::Keyboard::Tab) {
                    if (isChatting)	isGlobalChat = !isGlobalChat;
                }

                if (event.key.code == sf::Keyboard::Return) {
                    if (isChatting) {
                        if (currentChatInput.getSize() > 0) {
                            C2S_Chat chatPacket;
                            chatPacket.size = sizeof(C2S_Chat);
                            chatPacket.type = C2S_CHAT;
                            chatPacket.chatType = isGlobalChat ? 1 : 0;

                            std::string ansiChat = WideToAnsi(currentChatInput.toWideString());
                            strcpy_s(chatPacket.message, ansiChat.c_str());

                            netMgr.SendPacket(&chatPacket);
                            currentChatInput.clear();
                        }
                        isChatting = false;
                    }
                    else {
                        isChatting = true;
                    }
                }
            }
            else if (event.type == sf::Event::TextEntered && isChatting) {
                if (event.text.unicode == '\b') { // 백스페이스
                    if (currentChatInput.getSize() > 0) {
                        currentChatInput.erase(currentChatInput.getSize() - 1, 1);
                    }
                }
                else if (event.text.unicode >= 32 && event.text.unicode != 127) { // 제어 문자 제외
                    currentChatInput += event.text.unicode;
                }
            }
        }

        

        GameObject* myAvatar = gameMgr.GetMyAvatar();

        if (window.hasFocus()) {
            char currentDir = -1;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Up)) currentDir = 0;
            else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Down)) currentDir = 1;
            else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Left)) currentDir = 2;
            else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Right)) currentDir = 3;


            if (currentDir == -1) lastDir = -1;

            // 연속 키 입력 처리 (이동)
            if (myAvatar != nullptr && !isChatting) {
                bool tryAttack = sf::Keyboard::isKeyPressed(sf::Keyboard::A);

                if (tryAttack && attackTimer.getElapsedTime().asSeconds() >= 1.0f && !myAvatar->isActionPlaying) {
                    C2S_Attack attackPacket;
                    memset(&attackPacket, 0, sizeof(attackPacket));
                    attackPacket.size = sizeof(attackPacket);
                    attackPacket.type = C2S_ATTACK;
                    attackPacket.attackType = 1;

                    netMgr.SendPacket(&attackPacket);
                    attackTimer.restart();
                }
                else if (currentDir != -1 && !myAvatar->isActionPlaying && moveTimer.getElapsedTime().asSeconds() >= 0.5f) {
                    C2S_Move movePacket;
                    memset(&movePacket, 0, sizeof(movePacket));
                    movePacket.size = sizeof(movePacket);
                    movePacket.type = C2S_MOVE;
                    movePacket.direction = currentDir;

                    netMgr.SendPacket(&movePacket);
                    moveTimer.restart();
                }
            }
            else lastDir = -1;
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
		gameMgr.DrawEffects(window);                        // 공격 이펙트 그리기
        gameMgr.UpdateAndDrawAll(window);                   // 애니메이션 갱신 및 화면 출력

		window.setView(window.getDefaultView());

        if (myAvatar) {
            auto& resMgr = ResourceManager::GetInstance();

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

			// 채팅 로그 출력
            sf::Text chatText;
			chatText.setFont(resMgr.GetFont("main_font"));
			chatText.setCharacterSize(18);
            chatText.setOutlineThickness(1.0f);

            // 7초가 지난 메시지는 큐에서 완전히 삭제 (깜빡임/흐려짐 방지)
            while (!g_chatLog.empty() && g_chatLog.front().timer.getElapsedTime().asSeconds() > 7.0f) {
                g_chatLog.pop_front();
            }

            float startY = WINDOW_HEIGHT - 130.f; // 입력창 위쪽부터 위로 쌓임

            for (auto it = g_chatLog.rbegin(); it != g_chatLog.rend(); ++it) {
                chatText.setString(it->text);
                chatText.setFillColor(sf::Color::White);
                chatText.setOutlineColor(sf::Color::Black);

                // 글자 길이에 맞춰서 말풍선(배경 박스) 크기 자동 조절
                sf::FloatRect textBounds = chatText.getLocalBounds();
                sf::RectangleShape bubble(sf::Vector2f(textBounds.width + 20.f, 28.f));
                bubble.setPosition(15.f, startY);
                bubble.setFillColor(sf::Color(0, 0, 0, 180)); // 반투명 검은색
                bubble.setOutlineColor(sf::Color(100, 100, 100, 200)); // 회색 테두리
                bubble.setOutlineThickness(1.5f);

                // 글자를 말풍선 상자 안쪽 중앙에 오도록 살짝 이동
                chatText.setPosition(25.f, startY + 2.f);

                window.draw(bubble);
                window.draw(chatText);

                startY -= 35.f; // 다음 메시지는 위로 올려서 출력
            }

            // ---------------------------------------------
            // [타이핑용 입력창 UI 출력]
            // ---------------------------------------------
            if (isChatting) {
                // 반투명한 검은색 배경 박스
                sf::RectangleShape inputBox(sf::Vector2f(600.f, 35.f));
                inputBox.setPosition(15.f, WINDOW_HEIGHT - 60.f);
                inputBox.setFillColor(sf::Color(0, 0, 0, 150));
                window.draw(inputBox);

                sf::Text inputText;
                inputText.setFont(resMgr.GetFont("main_font"));
                inputText.setCharacterSize(22);
                inputText.setFillColor(sf::Color::Yellow);
                inputText.setOutlineColor(sf::Color::Black);
                inputText.setOutlineThickness(1.0f);

                sf::String modeStr = isGlobalChat ? sf::String(L"[전체] : ") : sf::String(L"[지역] : ");
                inputText.setString(modeStr + currentChatInput + sf::String("_"));
                inputText.setPosition(20.f, WINDOW_HEIGHT - 55.f);
                window.draw(inputText);
            }

            // 1. HP 바 배경 & 채우기
            sf::RectangleShape hpBg(sf::Vector2f(500.f, 20.f));
            hpBg.setPosition(WINDOW_WIDTH / 2.f - 250.f, 35.f);
            hpBg.setFillColor(sf::Color(40, 40, 40, 220));
            hpBg.setOutlineColor(sf::Color::Black);
            hpBg.setOutlineThickness(2.f);

            float hpRatio = (float)myAvatar->hp / myAvatar->max_hp;
            if (hpRatio < 0) hpRatio = 0;
            sf::RectangleShape hpFill(sf::Vector2f(500.f * hpRatio, 20.f));
            hpFill.setPosition(WINDOW_WIDTH / 2.f - 250.f, 35.f);
            hpFill.setFillColor(sf::Color(220, 50, 50));

            // 2. EXP 바 배경 & 채우기 (얇은 노란색 선)
            sf::RectangleShape expBg(sf::Vector2f(500.f, 6.f));
            expBg.setPosition(WINDOW_WIDTH / 2.f - 250.f, 57.f); // HP바 바로 아래에 착 붙임
            expBg.setFillColor(sf::Color(20, 20, 20, 220));
            expBg.setOutlineColor(sf::Color::Black);
            expBg.setOutlineThickness(1.f);

            // 경험치 비율 계산 (서버 레벨업 요구치 기준: 레벨 * 100)
            unsigned long long max_exp = myAvatar->level * 100;
            float expRatio = (float)myAvatar->exp / max_exp;
            if (expRatio > 1.0f) expRatio = 1.0f;

            sf::RectangleShape expFill(sf::Vector2f(500.f * expRatio, 6.f));
            expFill.setPosition(WINDOW_WIDTH / 2.f - 250.f, 57.f);
            expFill.setFillColor(sf::Color(255, 200, 0)); // 노란색 경험치

            window.draw(hpBg);
            window.draw(hpFill);
            window.draw(expBg);
            window.draw(expFill);

            // 3. 상태 텍스트 (바 위쪽에 깔끔하게 배치)
            sf::Text uiText;
            uiText.setFont(resMgr.GetFont("main_font"));
            uiText.setCharacterSize(17);
            uiText.setFillColor(sf::Color::White);
            uiText.setOutlineColor(sf::Color::Black);
            uiText.setOutlineThickness(1.5f);

            char uiBuf[256];
            // 소수점 1자리까지 퍼센트로 보여주면 더 좋습니다.
            sprintf_s(uiBuf, "Lv.%d %s | HP: %d / %d | EXP: %.1f%%",
            myAvatar->level, myAvatar->name, myAvatar->hp, myAvatar->max_hp, expRatio * 100.0f);

            uiText.setString(AnsiToWide(uiBuf));

            // 텍스트를 체력바 정중앙 상단에 정렬
            sf::FloatRect textRect = uiText.getLocalBounds();
            uiText.setPosition(WINDOW_WIDTH / 2.f - textRect.width / 2.f, 10.f);

            window.draw(uiText);
		}

        window.display();
    }
	return 0;

}