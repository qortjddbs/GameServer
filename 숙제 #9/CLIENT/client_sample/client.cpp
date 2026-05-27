#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <iostream>
#include <unordered_map>
#include <Windows.h>
#include <chrono>
using namespace std;

#include "..\..\SERVER\SERVER\protocol_2026.h"

sf::TcpSocket s_socket;

constexpr auto SCREEN_WIDTH = 16;
constexpr auto SCREEN_HEIGHT = 16;

constexpr auto TILE_WIDTH = 65;
constexpr auto WINDOW_WIDTH = SCREEN_WIDTH * TILE_WIDTH;   // size of window
constexpr auto WINDOW_HEIGHT = SCREEN_WIDTH * TILE_WIDTH;

int g_left_x;
int g_top_y;
int g_myid;

sf::RenderWindow* g_window;
sf::Font g_font;

#pragma pack(push, 1)
struct PACKET_HEADER {
	unsigned char size;
	PACKET_TYPE type;
};
#pragma pack(pop)

class OBJECT {
private:
	bool m_showing;
	sf::Sprite m_sprite;

	sf::Text m_name;
	sf::Text m_chat;
	chrono::system_clock::time_point m_mess_end_time;
public:
	int id;
	int m_x, m_y;
	char name[MAX_NAME_LEN];
	OBJECT(sf::Texture& t, int x, int y, int x2, int y2) {
		m_showing = false;
		m_sprite.setTexture(t);
		m_sprite.setTextureRect(sf::IntRect(x, y, x2, y2));
		set_name("NONAME");
		m_mess_end_time = chrono::system_clock::now();
	}
	OBJECT() {
		m_showing = false;
	}
	void show()
	{
		m_showing = true;
	}
	void hide()
	{
		m_showing = false;
	}

	void a_move(int x, int y) {
		m_sprite.setPosition((float)x, (float)y);
	}

	void a_draw() {
		g_window->draw(m_sprite);
	}

	void move(int x, int y) {
		m_x = x;
		m_y = y;
	}
	void draw() {
		if (false == m_showing) return;
		float rx = (m_x - g_left_x) * 65.0f + 1;
		float ry = (m_y - g_top_y) * 65.0f + 1;
		m_sprite.setPosition(rx, ry);
		g_window->draw(m_sprite);
		auto size = m_name.getGlobalBounds();
		if (m_mess_end_time < chrono::system_clock::now()) {
			m_name.setPosition(rx + 32 - size.width / 2, ry - 10);
			g_window->draw(m_name);
		}
		else {
			m_chat.setPosition(rx + 32 - size.width / 2, ry - 10);
			g_window->draw(m_chat);
		}
	}
	void set_name(const char str[]) {
		m_name.setFont(g_font);
		m_name.setString(str);
		if (id < NPC_ID_START) m_name.setFillColor(sf::Color(255, 255, 255));
		else m_name.setFillColor(sf::Color(255, 255, 0));
		m_name.setStyle(sf::Text::Bold);
	}

	void set_chat(const char str[]) {
		m_chat.setFont(g_font);
		m_chat.setString(str);
		m_chat.setFillColor(sf::Color(255, 255, 255));
		m_chat.setStyle(sf::Text::Bold);
		m_mess_end_time = chrono::system_clock::now() + chrono::seconds(3);
	}
};

OBJECT avatar;
unordered_map <int, OBJECT> players;

OBJECT white_tile;
OBJECT black_tile;

sf::Texture* board;
sf::Texture* pieces;

void client_initialize()
{
	board = new sf::Texture;
	pieces = new sf::Texture;
	board->loadFromFile("chessmap.bmp");
	pieces->loadFromFile("chess2.png");
	if (false == g_font.loadFromFile("cour.ttf")) {
		cout << "Font Loading Error!\n";
		system("pause");
		exit(-1);
	}
	white_tile = OBJECT{ *board, 5, 5, TILE_WIDTH, TILE_WIDTH };
	black_tile = OBJECT{ *board, 69, 5, TILE_WIDTH, TILE_WIDTH };
	avatar = OBJECT{ *pieces, 128, 0, 64, 64 };
	avatar.move(4, 4);
}

void client_finish()
{
	players.clear();
	delete board;
	delete pieces;
}

void ProcessPacket(char* ptr)
{
	static bool first_time = true;

	PACKET_TYPE type = reinterpret_cast<PACKET_HEADER*>(ptr)->type;

	switch (type)
	{
	case S2C_LOGIN_RESULT:	// 로그인 성공 여부 수신
	{
		S2C_LoginResult* packet = reinterpret_cast<S2C_LoginResult*>(ptr);
		if (packet->success) {
			cout << "Login Successful: " << packet->message << "\n";
		}
		else {
			cout << "Login Failed: " << packet->message << "\n";
		}
		break;
	}
	case S2C_AVATAR_INFO:	// 로그인 후 내 캐릭터 정보 수신
	{
		S2C_AvatarInfo* packet = reinterpret_cast<S2C_AvatarInfo*>(ptr);
		g_myid = packet->playerId;
		avatar.id = g_myid;
		avatar.move(packet->x, packet->y);
		g_left_x = packet->x - SCREEN_WIDTH / 2;
		g_top_y = packet->y - SCREEN_HEIGHT / 2;
		avatar.show();
		break;
	}

	case S2C_ADD_OBJECT:
	{
		S2C_AddObject* my_packet = reinterpret_cast<S2C_AddObject*>(ptr);
		int id = my_packet->object_id;

		if (id == g_myid) {
			avatar.move(my_packet->x, my_packet->y);
			g_left_x = my_packet->x - SCREEN_WIDTH / 2;
			g_top_y = my_packet->y - SCREEN_HEIGHT / 2;
			avatar.show();
		}
		else if (id < NPC_ID_START) {	// 다른 플레이어 스폰
			players[id] = OBJECT{ *pieces, 0, 0, 64, 64 };
			players[id].id = id;
			players[id].move(my_packet->x, my_packet->y);
			players[id].set_name(my_packet->obj_name);
			players[id].show();
		}
		else {	// NPC 스폰
			players[id] = OBJECT{ *pieces, 256, 0, 64, 64 };
			players[id].id = id;
			players[id].move(my_packet->x, my_packet->y);
			players[id].set_name(my_packet->obj_name);
			players[id].show();
		}
		break;
	}
	case S2C_MOVE_OBJECT:
	{
		S2C_MoveObject* my_packet = reinterpret_cast<S2C_MoveObject*>(ptr);
		int other_id = my_packet->object_id;
		if (other_id == g_myid) {
			avatar.move(my_packet->x, my_packet->y);
			g_left_x = my_packet->x - SCREEN_WIDTH/2;
			g_top_y = my_packet->y - SCREEN_HEIGHT/2;
		}
		else {
			players[other_id].move(my_packet->x, my_packet->y);
		}
		break;
	}

	case S2C_REMOVE_OBJECT:
	{
		S2C_RemoveObject* my_packet = reinterpret_cast<S2C_RemoveObject*>(ptr);
		int other_id = my_packet->object_id;
		if (other_id == g_myid) {
			avatar.hide();
		}
		else {
			players.erase(other_id);
		}
		break;
	}
	case S2C_CHAT_MESSAGE:
	{
		S2C_ChatMessage* my_packet = reinterpret_cast<S2C_ChatMessage*>(ptr);
		int other_id = my_packet->object_id;
		if (other_id == g_myid) {
			avatar.set_chat(my_packet->message);
		}
		else {
			players[other_id].set_chat(my_packet->message);
		}

		break;
	}
	default:
		printf("Unknown PACKET type [%d]\n", ptr[1]);
	}
}

void process_data(char* net_buf, size_t io_byte)
{
	char* ptr = net_buf;
	static size_t in_packet_size = 0;
	static size_t saved_packet_size = 0;
	// 새 프로토콜의 가장 큰 패킷 사이즈를 넉넉하게 커버
	static char packet_buffer[1024];

	while (0 != io_byte) {
		if (0 == in_packet_size) in_packet_size = static_cast<unsigned char>(ptr[0]);
		if (io_byte + saved_packet_size >= in_packet_size) {
			memcpy(packet_buffer + saved_packet_size, ptr, in_packet_size - saved_packet_size);
			ProcessPacket(packet_buffer);
			ptr += in_packet_size - saved_packet_size;
			io_byte -= in_packet_size - saved_packet_size;
			in_packet_size = 0;
			saved_packet_size = 0;
		}
		else {
			memcpy(packet_buffer + saved_packet_size, ptr, io_byte);
			saved_packet_size += io_byte;
			io_byte = 0;
		}
	}
}

void client_main()
{
	char net_buf[1024];
	size_t	received;

	auto recv_result = s_socket.receive(net_buf, 1024, received);
	if (recv_result == sf::Socket::Error)
	{
		wcout << L"Recv 에러!";
		exit(-1);
	}
	if (recv_result == sf::Socket::Disconnected) {
		wcout << L"Disconnected\n";
		exit(-1);
	}
	if (recv_result != sf::Socket::NotReady)
		if (received > 0) process_data(net_buf, received);

	for (int i = 0; i < SCREEN_WIDTH; ++i)
		for (int j = 0; j < SCREEN_HEIGHT; ++j)
		{
			int tile_x = i + g_left_x;
			int tile_y = j + g_top_y;
			if ((tile_x < 0) || (tile_y < 0)) continue;
			if (0 ==(tile_x /3 + tile_y /3) % 2) {
				white_tile.a_move(TILE_WIDTH * i, TILE_WIDTH * j);
				white_tile.a_draw();
			}
			else
			{
				black_tile.a_move(TILE_WIDTH * i, TILE_WIDTH * j);
				black_tile.a_draw();
			}
		}
	avatar.draw();
	for (auto& pl : players) pl.second.draw();
	sf::Text text;
	text.setFont(g_font);
	char buf[100];
	sprintf_s(buf, "(%d, %d)", avatar.m_x, avatar.m_y);
	text.setString(buf);
	g_window->draw(text);
}

void send_packet(void *packet)
{
	unsigned char *p = reinterpret_cast<unsigned char *>(packet);
	size_t sent = 0;
	s_socket.send(packet, p[0], sent);
}

int main()
{
	wcout.imbue(locale("korean"));
	string server_ip;
	cout << "접속할 서버의 IP 주소를 입력하세요 (로컬 테스트시 그냥 엔터): ";
	getline(cin, server_ip);
	if (server_ip.empty()) server_ip = "127.0.0.1";

	string user_id;
	cout << "로그인할 ID를 입력하세요 (예: tom, jame): ";
	getline(cin, user_id);
	if (user_id.empty()) {
		cout << "ID가 입력되지 않아 프로그램을 종료합니다.\n";
		return -1;
	}

	sf::Socket::Status status = s_socket.connect(server_ip, PORT);
	s_socket.setBlocking(false);

	if (status != sf::Socket::Done) {
		wcout << L"서버와 연결할 수 없습니다.\n";
		system("pause");
		exit(-1);
	}

	client_initialize();
	C2S_Login p;
	p.size = sizeof(p);
	p.type = C2S_LOGIN;

	strcpy_s(p.username, user_id.c_str());

	send_packet(&p);
	avatar.set_name(p.username);

	sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "2D CLIENT");
	g_window = &window;

	while (window.isOpen())
	{
		sf::Event event;
		while (window.pollEvent(event))
		{
			if (event.type == sf::Event::Closed)
				window.close();
			if (event.type == sf::Event::KeyPressed) {
				int dx = 0, dy = 0;

				switch (event.key.code) {
				case sf::Keyboard::Left:
					dx = -1;
					break;
				case sf::Keyboard::Right:
					dx = 1;
					break;
				case sf::Keyboard::Up:
					dy = -1;
					break;
				case sf::Keyboard::Down:
					dy = 1;
					break;
				case sf::Keyboard::Escape:
					window.close();
					break;
				}
				if (dx != 0 || dy != 0) {
					C2S_Move p;
					p.size = sizeof(p);
					p.type = C2S_MOVE;
					p.x = avatar.m_x + dx;
					p.y = avatar.m_y + dy;
					p.move_time = 0;
					send_packet(&p);
				}

			}
		}

		window.clear();
		client_main();
		window.display();
	}
	client_finish();

	return 0;
}