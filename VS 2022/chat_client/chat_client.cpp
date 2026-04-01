// SFML을 사용해서 윈도우를 생성하려면 기본적으로 다음 3개의 헤더파일이 필요하다.
#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <SFML/System.hpp>


#include <iostream>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

const char* SERVER_IP = "25.7.6.204";
constexpr short SERVER_PORT = 3500;
constexpr int BUFFER_SIZE = 200;

char g_recv_buffer[BUFFER_SIZE];
char g_send_buffer[BUFFER_SIZE];
WSABUF g_recv_wsa_buf{ BUFFER_SIZE, g_recv_buffer };
WSABUF g_send_wsa_buf{ BUFFER_SIZE, g_send_buffer };
WSAOVERLAPPED g_recv_overlapped{}, g_send_overlapped{};
SOCKET g_s_socket;

#pragma pack(push, 1)
class PACKET {
public:
    unsigned char m_size;
    unsigned char m_sender_id;
    char m_buf[BUFFER_SIZE];
    PACKET(int sender, char* mess) : m_sender_id(sender)
    {
        m_size = static_cast<int>(strlen(mess) + 3);
        strcpy_s(m_buf, mess);
    }
};
#pragma pack(pop)

void error_display(const wchar_t* msg, int err_no)
{
    WCHAR* lpMsgBuf;
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, err_no,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR)&lpMsgBuf, 0, NULL);
    std::wcout << msg;
    std::wcout << L" === 에러 " << lpMsgBuf << std::endl;
    while (true);   // 디버깅 용
    LocalFree(lpMsgBuf);
}
void recv_from_server();

void CALLBACK recv_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
    if (error != 0) {
        error_display(L"데이터 수신 실패", WSAGetLastError());
        exit(1);
    }

    char* p = g_recv_buffer;
    int remain = bytes_transferred;
    while (remain > 0) {
        PACKET* packet = reinterpret_cast<PACKET*>(p);
        int id = packet->m_sender_id;
        std::cout << "Client [" << id << "] " << packet->m_buf << std::endl;
        remain -= packet->m_size;
        p = p + packet->m_size;       // 다음 패킷의 주소를 가리키게끔 변경
    }
    recv_from_server();
}

void CALLBACK send_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
    if (error != 0) {
        error_display(L"데이터 전송 실패", WSAGetLastError());
        return;
    }
    std::cout << "Sent to server: SIZE: " << bytes_transferred << std::endl;
}

void recv_from_server()
{
    DWORD recv_flag = 0;
    ZeroMemory(&g_recv_overlapped, sizeof(g_recv_overlapped));
    int result = WSARecv(g_s_socket, &g_recv_wsa_buf, 1, nullptr, &recv_flag, &g_recv_overlapped, recv_callback);
    if (result == SOCKET_ERROR) {
        int err_no = WSAGetLastError();
        if (err_no != WSA_IO_PENDING) {
            error_display(L"데이터 수신 실패", err_no);
            exit(1);
        }
    }
}

int main()
{
    // sf namespace는 SFML의 namespace를 뜻함
    // RenderWindow객체를 생성하면 화면에 윈도을 하나 생성한다.
    //         매개 변수는 크기, 타이틀, 생김새
    sf::RenderWindow window(sf::VideoMode(1180, 100), "INPUT WINDOW", sf::Style::Default);
    // Window에 문자열을 출력하기 위해서는 Font 객체가 필요하다.
    //       Font 객체 생성 후 font를 실제로 로딩시켜줘야 한다.
    //       Font는 True Type Font를 사용하고 인터넷에 널려있으니 필요하면 다른 폰트를 다운받아서 사용해도 된다.
    sf::Font font;
    if (!font.loadFromFile("cour.ttf"))
        return EXIT_FAILURE;

    // Event는 window에서 발생한 event 정보를 알려주는 객체이다.
    //        event란 : 키보드 입력, 마우스 이동, 마우스 버튼 입력, window 이동, window resize, window 생성, window 파괴
    sf::Event event;
    // String은 문자열을 window에 출력할 때 사용하는 자료구조
    sf::String playerInput;
    // Text는 입력창을 관리해주는 객체, Window에 Text를 연결하면 Window에서 문자열을 키보드로 입력할 수 있다.
    sf::Text playerText("", font, 20);
    playerText.setPosition(60, 30);             // 입력 문자열의 위치
    playerText.setFillColor(sf::Color::Yellow); // 문자열의 색상

    std::wcout.imbue(std::locale("korean"));
    WSADATA wsa_data{};
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    g_s_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    SOCKADDR_IN server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    int result = WSAConnect(g_s_socket, reinterpret_cast<SOCKADDR*>(&server_addr), sizeof(server_addr), nullptr, nullptr, nullptr, nullptr);
    if (result == SOCKET_ERROR) {
        error_display(L"서버 연결 실패", WSAGetLastError());
        return 1;
    }

    recv_from_server();

    // 이벤트 처리 루프
    // Window프로그래밍 이란?  무한 루프를 돌면서 window에서 발생한 이벤트를 처리하는 프로그램
    while (window.isOpen())     // window가 살아있으면 계속
    {
        window.clear();         // 매번 지우고 새로 그린다.
        while (window.pollEvent(event)) // pollEvent 함수는 event를 하나 큐에서 꺼내고, 이벤트가 없으면 false를 리턴, 있으면 true
        {
            if (event.type == sf::Event::TextEntered)       // window에 타이핑을 했으면.
            {
                // std::cout << event.text.unicode;
                if (event.text.unicode < 128)
                {
                    if (13 == event.text.unicode) {
                        std::string message = playerInput.toAnsiString();
                        playerInput.clear();

                        PACKET send_packet(0, const_cast<char*>(message.c_str()));

                        g_send_wsa_buf.len = send_packet.m_size;
                        memcpy(g_send_wsa_buf.buf, &send_packet, g_send_wsa_buf.len);
                        ZeroMemory(&g_send_overlapped, sizeof(g_send_overlapped));
                        DWORD sent_size = 0;
                        int result = WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_overlapped, send_callback);
                        if (result == SOCKET_ERROR) {
                            error_display(L"데이터 전송 실패", WSAGetLastError());
                            exit(1);
                        }
                    }
                    else playerInput += event.text.unicode;
                    playerText.setString(playerInput);
                }
            }
        }

        window.draw(playerText);        // 사용자가 입력한 문자을 window에 출력
        window.display();               // 실제로 window에 출력한 내용을 모니터에 전송
        SleepEx(0, TRUE);
    }
    return 0;
}