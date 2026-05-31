#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <iostream>
#include <unordered_map>
#include <string>
#include <memory>

#include "protocol_2026.h" 

using namespace std;

constexpr int WINDOW_WIDTH = 1024;
constexpr int WINDOW_HEIGHT = 768;

sf::RenderWindow* g_window = nullptr;
sf::TcpSocket g_socket;
int g_my_id = -1;

std::unordered_map < std::string, sf::Texture> g_textures;

bool LoadAssets() {
    if (!g_textures["idle"].loadFromFile("Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Idle.png") ||
        !g_textures["run"].loadFromFile("Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Run.png") ||
        !g_textures["attack1"].loadFromFile("Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Attack1.png") ||
        !g_textures["attack2"].loadFromFile("Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Attack2.png") ||
        !g_textures["guard"].loadFromFile("Assets/Tiny Swords/Units/Black Units/Warrior/Warrior_Guard.png")) {
		std::cout << "이미지 로드 실패! 파일 경로를 확인하세요." << std::endl;
        return false;
    }
    return true;
}

enum class AnimState { IDLE, RUN, ATTACK1, ATTACK2, GUARD };

// ==========================================
// 2. 게임 객체 베이스 클래스
// ==========================================
class GameObject {
public:
    int id;
    int x, y;
    char name[MAX_NAME_LEN];
    int hp, max_hp;
    unsigned long long exp;
    unsigned char level;

    sf::Sprite sprite;

    // 애니메이션 관리를 위한 변수들
    sf::Clock animClock;
    AnimState currentState = AnimState::IDLE;
    bool isWalking = false;
    int prev_x = 0, prev_y = 0;
	bool isActionPlaying = false;    // 공격/방어같은 '단발성 액션'이 진행 중인지 확인하는 플래그

    // 프레임 규격 (한 장당 192 x 192)
    const int frameWidth = 192;
	const int frameHeight = 192;

    int currentFrame = 0;       // 현재 보여주고 있는 프레임 번호
    int maxFrames = 6;          // 현재 애니메이션의 총 프레임 개수

    GameObject() : id(-1), x(0), y(0), hp(0), max_hp(0), exp(0), level(1) {
        memset(name, 0, sizeof(name));

        // 초기 텍스처(Idle) 세팅
		sprite.setTexture(g_textures["idle"]);
        maxFrames = 8;

        // 전체 이미지 중 첫 번째 프레임(0, 0) 영역만 잘라서 보여주기
        // sf::IntRect(시작 X, 시작 Y, 자를 가로길이, 자를 세로길이)
		sprite.setTextureRect(sf::IntRect(0, 0, frameWidth, frameHeight));

        // 중심점 잡기 (잘라낸 프레임의 절반 크기)
        sprite.setOrigin(frameWidth / 2.0f, frameHeight / 2.0f);
    }

    virtual void setPosition(int new_x, int new_y) {
        if (x != new_x || y != new_y) {
            isWalking = true;

            // 왼쪽으로 이동하면 스프라이트 좌우 반전 (Flip)
            // (Warrior 에셋이 오른쪽 기준이므로, 왼쪽으로 갈 때는 x Scale을 음수로)
            if (new_x < x) sprite.setScale(-1.0f, 1.0f);
			else if (new_x > x) sprite.setScale(1.0f, 1.0f);
        }
        else {
            isWalking = false;
		}

		prev_x = x;
		prev_y = y;
        x = new_x;
        y = new_y;

        // 서버 좌표를 화면 픽셀로 변환 (타일 크기를 64라고 가정)
        sprite.setPosition(static_cast<float>(x * 64 + 32), static_cast<float>(y * 64 + 32));
    }

    void doAttack() {
        currentState = AnimState::ATTACK1;
        sprite.setTexture(g_textures["attack1"]);
        currentFrame = 0;
        maxFrames = 4;
        isActionPlaying = true;
        animClock.restart();
    }

    void doGuard() {
		currentState = AnimState::GUARD;
        sprite.setTexture(g_textures["guard"]);
        currentFrame = 0;
        maxFrames = 6;
        isActionPlaying = true;
		animClock.restart();
    }

    virtual void updateAnimation() {
		// 단발성 액션(공격/방어)이 끝났는지 체크
        if (isActionPlaying) {
            if (currentFrame >= maxFrames - 1 && animClock.getElapsedTime().asSeconds() > 0.1f) {
				// 마지막 프레임까지 재생되었다면 액션 종료 및 IDLE로 복귀
                isActionPlaying = false;
				currentState = AnimState::IDLE;
				sprite.setTexture(g_textures["idle"]);
                currentFrame = 0;
				maxFrames = 8;
            }
        }

        // 액션 중이 아닐 때만 걷기/대기 상태 전환 허용
        if (!isActionPlaying) {
			AnimState nextState = isWalking ? AnimState::RUN : AnimState::IDLE;

            if (currentState != nextState) {
				currentState = nextState;
                currentFrame = 0;

                if (currentState == AnimState::RUN) {
                    sprite.setTexture(g_textures["run"]);
                    maxFrames = 6;
                }
                else {
					sprite.setTexture(g_textures["idle"]);
					maxFrames = 8;
                }
            }
		}

        // 시간에 따른 프레임 변경 (0.1초마다 다음 프레임으로)
        if (animClock.getElapsedTime().asSeconds() > 0.1f) {
            currentFrame++;

            // 루프 애니메이션일 때는 0으로 돌아가고, 단발성일 때는 마지막 프레임에 멈춰있게 함
            if (currentFrame >= maxFrames) {
                if (isActionPlaying) {
                    currentFrame = maxFrames - 1; // 단발성 액션은 마지막 프레임에서 멈춤
                }
                else {
                    currentFrame = 0; // 루프 애니메이션은 처음으로 돌아감
				}
            }
        }

		sprite.setTextureRect(sf::IntRect(currentFrame * frameWidth, 0, frameWidth, frameHeight));

        animClock.restart();
    }

    virtual void draw(sf::RenderWindow& window) {
        window.draw(sprite);
    }
};

unordered_map<int, shared_ptr<GameObject>> g_objects;

// ==========================================
// 3. 네트워크 패킷 처리 (수신)
// ==========================================
void ProcessPacket(char* packet) {
    PACKET_TYPE type = reinterpret_cast<C2S_Login*>(packet)->type;

    switch (type) {
    case S2C_LOGIN_RESULT: {
        auto p = reinterpret_cast<S2C_LoginResult*>(packet);
        if (p->success) cout << "[서버] 로그인 성공: " << p->message << endl;
        else cout << "[서버] 로그인 실패!" << endl;
        break;
    }
    case S2C_AVATAR_INFO: {
        auto p = reinterpret_cast<S2C_AvatarInfo*>(packet);
        g_my_id = p->playerId;

        auto my_avatar = make_shared<GameObject>();
        my_avatar->id = g_my_id;
        my_avatar->hp = p->hp;
        my_avatar->max_hp = p->max_hp;
        my_avatar->exp = p->exp;
        my_avatar->level = p->level;
        my_avatar->setPosition(p->x, p->y);

        g_objects[g_my_id] = my_avatar;
        cout << "내 아바타 생성 완료! (ID: " << g_my_id << ", 위치: " << p->x << ", " << p->y << ")" << endl;
        break;
    }
    }
}

// 4. 안전한 패킷 조립 함수 (TCP 스트림 분리)
void process_data(char* net_buf, size_t io_byte) {
    char* ptr = net_buf;
    static size_t in_packet_size = 0;
    static size_t saved_packet_size = 0;
    static char packet_buffer[4096]; // 패킷 조립용 버퍼

    while (0 != io_byte) {
        // 패킷의 맨 앞 1바이트를 읽어 전체 크기 파악
        if (0 == in_packet_size) in_packet_size = static_cast<unsigned char>(ptr[0]);

        // 데이터가 충분히 모였다면 패킷 1개 완성!
        if (io_byte + saved_packet_size >= in_packet_size) {
            memcpy(packet_buffer + saved_packet_size, ptr, in_packet_size - saved_packet_size);
            ProcessPacket(packet_buffer);
            ptr += in_packet_size - saved_packet_size;
            io_byte -= in_packet_size - saved_packet_size;
            in_packet_size = 0;
            saved_packet_size = 0;
        }
        // 아직 데이터가 모자라다면 버퍼에 킵해두고 다음 Recv 대기
        else {
            memcpy(packet_buffer + saved_packet_size, ptr, io_byte);
            saved_packet_size += io_byte;
            io_byte = 0;
        }
    }
}

void ReceiveNetworkData() {
    char net_buf[1024];
    size_t received;
    auto recv_result = g_socket.receive(net_buf, sizeof(net_buf), received);

    if (recv_result == sf::Socket::Done && received > 0) {
        process_data(net_buf, received);
    }
    else if (recv_result == sf::Socket::Disconnected) {
        cout << "서버와 연결이 끊어졌습니다." << endl;
        g_window->close();
    }
}

// ==========================================
// 4. 메인 루프
// ==========================================
int main() {
    // 창 생성 직전에 이미지부터 모두 메모리에 로드!
    if (!LoadAssets()) {
        system("pause");
        return -1;
    }

    wcout.imbue(locale("korean"));

    string server_ip = "127.0.0.1";
    cout << "서버에 접속합니다 (" << server_ip << ")..." << endl;

    if (g_socket.connect(server_ip, PORT) != sf::Socket::Done) {
        cout << "서버 연결 실패!" << endl;
        system("pause");
        return -1;
    }

    // 접속하자마자 서버에 로그인 패킷을 쏴줍니다!
    C2S_Login login_p;
    login_p.size = sizeof(login_p);
    login_p.type = C2S_LOGIN;
    strcpy_s(login_p.username, "MyPlayer");

    if (g_socket.send(&login_p, login_p.size) == sf::Socket::Done) {
        cout << "로그인 패킷 전송 성공!" << endl;
    }
    else {
		cout << "로그인 패킷 전송 실패!" << endl;
    }

	g_socket.setBlocking(false); // 논블로킹 모드로 설정 (게임 루프에서 recv가 멈추지 않도록)

    // 윈도우 생성
    sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "Term Project Client");
    window.setFramerateLimit(60);
    g_window = &window;

    // 나를 따라다닐 카메라(View) 생성
	sf::View camera(sf::FloatRect(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT));

    // 쿨타임 관리를 위한 타이머
    sf::Clock moveTimer;
    sf::Clock attackTimer;

    // 게임 루프
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) window.close();

            // 키보드 단일 입력 처리 - 꾹 누르는 게 아니라 한 번 누를 때마다 발동
            if (event.type == sf::Event::KeyPressed) {

                // C 키 : 공격 - 1초 쿨타임
                if (event.key.code == sf::Keyboard::C) {
                    if (attackTimer.getElapsedTime().asSeconds() >= 1.0f) {
                        // 내 캐릭터 애니메이션 즉시 실행 (클라이언트 예측) / 아직 서버에서 공격 패킷 안보냄
                        if (g_my_id != -1 && g_objects.count(g_my_id)) {
                            g_objects[g_my_id]->doAttack();
                        }

                        // 서버로 공격 패킷 전송
                        C2S_Attack attackPacket;
                        attackPacket.size = sizeof(attackPacket);
                        attackPacket.type = C2S_ATTACK;
                        g_socket.send(&attackPacket, attackPacket.size);

                        attackTimer.restart();  // 쿨타임 리셋
                    }
                }
                else if (event.key.code == sf::Keyboard::X) {
                    if (attackTimer.getElapsedTime().asSeconds() >= 1.0f) {
                        // 내 캐릭터 애니메이션 즉시 실행 (클라이언트 예측) / 아직 서버에서 방어 패킷 안보냄
                        if (g_my_id != -1 && g_objects.count(g_my_id)) {
                            g_objects[g_my_id]->doGuard();
                        }

                        // 서버로 방어 패킷 전송 (아직 프로토콜에 방어 패킷 없음)
                        attackTimer.restart();
                    }
                }
            }

            if (moveTimer.getElapsedTime().asSeconds() >= 0.5f) {
                int dx = 0, dy = 0;

                if (sf::Keyboard::isKeyPressed(sf::Keyboard::Left)) dx = -1;
                else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Right)) dx = 1;
                else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Up)) dy = -1;
                else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Down)) dy = 1;

                if (dx != 0 || dy != 0) {
                    // 서버로 이동 패킷 전송
                    C2S_Move movePacket;
                    movePacket.size = sizeof(movePacket);
                    movePacket.type = C2S_MOVE;
                    // movePacket.direction = /* dx, dy를 통해 프로토콜에 맞는 방향 값(0~3) 산출 */;

                    g_socket.send(&movePacket, movePacket.size);
                    moveTimer.restart();  // 쿨타임 리셋
                }
            }

            // 네트워크 수신
            ReceiveNetworkData();

            // 카메라를 내 캐릭터 위치로 이동시키는 로직
            if (g_my_id != -1 && g_objects.count(g_my_id) > 0) {
                auto& my_avatar = g_objects[g_my_id];

                // 카메라 중심을 내 캐릭터의 현재 화면(픽셀) 좌표로 맞춤
                camera.setCenter(my_avatar->sprite.getPosition());

                // 윈도우에 카메라 설정 적용 (이 순간부터 창은 캐릭터를 중심으로 비춤)
                window.setView(camera);
            }

            // 렌더링
            window.clear(sf::Color(50, 50, 50));    // 짙은 회색 배경

            for (auto& pair : g_objects) {
                pair.second->updateAnimation();
                pair.second->draw(window);
            }
            window.display();
        }
    }

    return 0;
}