#pragma once
#include <unordered_map>
#include <memory>
#include <SFML/Graphics.hpp>
#include "GameObject.h" // (GameObject 클래스가 선언된 헤더 파일)

class GameManager {
private:
    // 전역 변수였던 g_objects와 g_my_id를 숨김
    // 스마트 포인터(unique_ptr)를 사용하여 메모리 관리를 완전히 자동화
    std::unordered_map<int, std::unique_ptr<GameObject>> m_objects;
    int m_myId = -1;

    // 싱글톤 패턴을 위한 private 생성자
    GameManager() = default;

public:
    static GameManager& GetInstance() {
        static GameManager instance;
        return instance;
    }
    GameManager(const GameManager&) = delete;
    GameManager& operator=(const GameManager&) = delete;

    // 내 아바타 ID 설정 및 가져오기
    void SetMyId(int id) { m_myId = id; }
    int GetMyId() const { return m_myId; }

    // 객체 추가 (소유권을 매니저에게 넘김)
    void AddObject(int id, std::unique_ptr<GameObject> obj) {
        m_objects[id] = std::move(obj);
    }

    // 객체 삭제 (자료구조에서 지우는 순간, unique_ptr 덕분에 메모리도 자동 해제됨!)
    void RemoveObject(int id) {
        m_objects.erase(id);
    }

    // 객체 검색 (수정/접근만 할 수 있도록 원시 포인터 반환)
    GameObject* GetObject(int id) {
        auto it = m_objects.find(id);
        if (it != m_objects.end()) {
            return it->second.get();
        }
        return nullptr; // 없으면 nullptr 반환
    }

    // 내 캐릭터 객체만 쏙 뽑아주는 편의성 함수 (카메라 추적용)
    GameObject* GetMyAvatar() {
        return GetObject(m_myId);
    }

    // 매 프레임 모든 객체의 애니메이션을 갱신하고 그리기
    void UpdateAndDrawAll(sf::RenderWindow& window) {
        for (auto& pair : m_objects) {
            pair.second->updateAnimation();
            pair.second->draw(window);
        }
    }
};