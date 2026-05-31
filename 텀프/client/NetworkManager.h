#pragma once
#include <SFML/Network.hpp>
#include <unordered_map>
#include <functional>
#include <iostream>
#include "..\..\텀프\server\protocol_2026.h"

// 패킷의 공통 헤더 (타입 추출용)
#pragma pack(push, 1)
struct PACKET_HEADER {
    unsigned char size;
    PACKET_TYPE type;
};
#pragma pack(pop)

class NetworkManager {
private:
    sf::TcpSocket m_socket;

    // 패킷 타입(Key)과 실행할 함수(Value)를 연결해두는 맵
    std::unordered_map<PACKET_TYPE, std::function<void(char*)>> m_handlers;

    // 패킷 조립용 버퍼 변수들
    size_t in_packet_size = 0;
    size_t saved_packet_size = 0;
    char packet_buffer[4096];

public:
    // 싱글톤 패턴: 프로그램 전체에서 단 하나의 NetworkManager만 존재하게 함
    static NetworkManager& GetInstance() {
        static NetworkManager instance;
        return instance;
    }

    // 서버 연결
    bool Connect(const std::string& ip, unsigned short port);

    // 패킷 전송
    void SendPacket(void* packet);

    // 네트워크 수신 및 패킷 조립 (매 프레임 호출됨)
    void Receive();

    // 외부에서 "이 패킷이 오면 이 함수를 실행해줘!" 라고 예약하는 기능
    void RegisterHandler(PACKET_TYPE type, std::function<void(char*)> handler);

private:
    NetworkManager() = default; // 싱글톤을 위해 생성자를 숨김

    // 조립된 완제품 패킷을 알맞은 함수로 토스해주는 디스패처
    void DispatchPacket(char* packet);
};