#include "NetworkManager.h"

bool NetworkManager::Connect(const std::string& ip, unsigned short port) {
    if (m_socket.connect(ip, port) != sf::Socket::Done) {
        return false;
    }
    m_socket.setBlocking(false); // 논블로킹 설정
    return true;
}

void NetworkManager::SendPacket(void* packet) {
    unsigned char* p = reinterpret_cast<unsigned char*>(packet);
    m_socket.setBlocking(true);

    size_t sent = 0;
    m_socket.send(packet, p[0], sent);

    m_socket.setBlocking(false);
}

void NetworkManager::Receive() {
    char net_buf[1024];
    size_t received;
    auto recv_result = m_socket.receive(net_buf, sizeof(net_buf), received);

    if (recv_result == sf::Socket::Done && received > 0) {
        // 기존의 process_data 함수 내용을 여기에 이식
        char* ptr = net_buf;
        size_t io_byte = received;

        while (0 != io_byte) {
            if (0 == in_packet_size) in_packet_size = static_cast<unsigned char>(ptr[0]);

            if (io_byte + saved_packet_size >= in_packet_size) {
                memcpy(packet_buffer + saved_packet_size, ptr, in_packet_size - saved_packet_size);

                // switch-case 대신 DispatchPacket 호출!
                DispatchPacket(packet_buffer);

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
    else if (recv_result == sf::Socket::Disconnected) {
        std::cout << "서버와 연결이 끊어졌습니다." << std::endl;
        exit(-1);
    }
}

void NetworkManager::RegisterHandler(PACKET_TYPE type, std::function<void(char*)> handler) {
    m_handlers[type] = handler;
}

// switch-case를 단 5줄로 끝내버리는 궁극의 함수
void NetworkManager::DispatchPacket(char* packet) {
    PACKET_TYPE type = reinterpret_cast<PACKET_HEADER*>(packet)->type;

    auto it = m_handlers.find(type);
    if (it != m_handlers.end()) {
        it->second(packet); // 맵에 등록된 함수를 찾아서 순식간에 실행
    }
    else {
        std::cout << "등록되지 않은 패킷 타입 수신: " << type << std::endl;
    }
}