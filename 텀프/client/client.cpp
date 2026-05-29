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

    GameObject() : id(-1), x(0), y(0), hp(0), max_hp(0), exp(0), level(1) {
        memset(name, 0, sizeof(name));
    }

    virtual void setPosition(int new_x, int new_y) {
        x = new_x;
        y = new_y;
    }

    virtual void draw(sf::RenderWindow& window) {
        // 아직 텍스처(이미지)가 없으므로 SFML 기본 원형 그리기 사용!
        // 내 캐릭터는 초록색, 다른 사람은 빨간색 동그라미로 표시
        sf::CircleShape shape(15.0f);
        if (id == g_my_id) shape.setFillColor(sf::Color::Green);
        else shape.setFillColor(sf::Color::Red);

        // 서버 좌표(0~200)가 너무 작아서 안 보일 수 있으니 x5 배율로 뻥튀기해서 그립니다.
        shape.setPosition(static_cast<float>(x * 5), static_cast<float>(y * 5));
        window.draw(shape);
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

    // 게임 루프
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) {
                window.close();
            }
        }

        // 네트워크 수신
        ReceiveNetworkData();

        // 렌더링
        window.clear(sf::Color::Black);
        for (auto& pair : g_objects) {
            pair.second->draw(window);
        }
        window.display();
    }

    return 0;
}