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

	// 4방향 공격 이펙트 관리를 위한 구조체와 벡터
    struct AttackEffect {
        int x, y;
        sf::Sprite sprite;
        sf::Clock animClock;
        int currentFrame = 0;
        int maxFrames = 10;
        int frameWidth;
		int frameHeight;
    };
    std::vector<AttackEffect> m_effects;

    struct BossSkillEffect {
        int x, y;
        int type; // 0: 경고장판, 1: 폭발
        sf::Clock timer;
    };
    std::vector<BossSkillEffect> m_bossEffects;

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

    void UpdateAll() {
        for (auto& pair : m_objects) {
            pair.second->updateAnimation();
		}
    }

    void DrawAll(sf::RenderWindow& window) {
        for (auto& pair : m_objects) {
            pair.second->draw(window);
        }
	}

    // 매 프레임 모든 객체의 애니메이션을 갱신하고 그리기
    void UpdateAndDrawAll(sf::RenderWindow& window) {
        for (auto& pair : m_objects) {
            pair.second->updateAnimation();
            pair.second->draw(window);
        }
    }

    // 특정 좌표에 이펙트 애니메이션을 생성하는 함수
    void AddAttackEffect(int x, int y) {
        AttackEffect effect;
        effect.x = x;
		effect.y = y;

        // 텍스처 세팅
        auto& texture = ResourceManager::GetInstance().GetTexture("attack_effect");
        effect.sprite.setTexture(texture);

        // 1칸당 픽셀 사이즈 계산 (이미지 전체 가로 길이 / 프레임 개수)
        effect.frameWidth = texture.getSize().x / effect.maxFrames;
        effect.frameHeight = texture.getSize().y;

        // 첫 프레임 잘라내기 및 중심점 세팅
        effect.sprite.setTextureRect(sf::IntRect(0, 0, effect.frameWidth, effect.frameHeight));
        effect.sprite.setOrigin(effect.frameWidth / 2.0f, effect.frameHeight / 2.0f);

        // 타일 크기(64)에 맞게 살짝 크기 조정 (필요시 배율 수정)
        effect.sprite.setScale(2.0f, 2.0f);

        m_effects.push_back(effect);
    }

    // 매 프레임마다 이펙트를 갱신하고 그리는 함수
    void DrawEffects(sf::RenderWindow& window) {
        auto it = m_effects.begin();
        while (it != m_effects.end()) {

            // 0.05초마다 다음 프레임으로 넘기기 (속도 조절 가능)
            if (it->animClock.getElapsedTime().asSeconds() > 0.05f) {
                it->currentFrame++;
                it->animClock.restart();
            }

            // 프레임이 끝까지 도달하면 리스트에서 완전히 삭제 (이펙트 종료)
            if (it->currentFrame >= it->maxFrames) {
                it = m_effects.erase(it);
            }
            else {
                // 다음 프레임 이미지 잘라내기
                it->sprite.setTextureRect(sf::IntRect(it->currentFrame * it->frameWidth, 0, it->frameWidth, it->frameHeight));
                // 캐릭터 좌표계(타일 중앙)에 맞춰서 위치 세팅
                it->sprite.setPosition(it->x * 64.f + 32.f, it->y * 64.f + 32.f);

                window.draw(it->sprite);
                ++it;
            }
        }

        for (auto it = m_bossEffects.begin(); it != m_bossEffects.end(); ) {
            float elapsed = it->timer.getElapsedTime().asSeconds();

            // 컷신 폭발(2)과 궁극기 경고장판(3)은 2.0초 동안 유지됨
            if ((it->type == 0 && elapsed > 1.5f) || (it->type == 1 && elapsed > 0.5f) ||
                ((it->type == 2 || it->type == 3) && elapsed > 2.0f)) {
                it = m_bossEffects.erase(it);
                continue;
            }

            sf::CircleShape shape;
            if (it->type == 0) {
                // 일반 스킬 장판
                shape.setRadius(64.f * 1.5f);
                shape.setOrigin(shape.getRadius(), shape.getRadius());
                shape.setFillColor(sf::Color(255, 0, 0, 80));
                shape.setOutlineColor(sf::Color::Red);
                shape.setOutlineThickness(2.f);
            }
            else if (it->type == 1) {
                // 일반 스킬 폭발
                shape.setRadius(64.f * 1.5f);
                shape.setOrigin(shape.getRadius(), shape.getRadius());
                shape.setFillColor(sf::Color(255, 150, 0, 200));
            }
            else if (it->type == 2) {
                // 즉사기 화면 컷신 피보라 폭발
                shape.setRadius(64.f * 15.f);
                shape.setOrigin(shape.getRadius(), shape.getRadius());
                int alpha = std::max(0, 180 - (int)(elapsed * 90));
                shape.setFillColor(sf::Color(255, 0, 0, alpha));
            }
            // -------------------------------------------------------------
            // [추가] 타입 3: 즉사기 2초 전 경고 보라색/블랙홀 장판!
            // -------------------------------------------------------------
            else if (it->type == 3) {
                shape.setRadius(64.f * 15.f);
                shape.setOrigin(shape.getRadius(), shape.getRadius());
                // 위압감 넘치는 보라색 반투명에 찐한 보라색 테두리
                shape.setFillColor(sf::Color(100, 0, 150, 80));
                shape.setOutlineColor(sf::Color(200, 0, 255));
                shape.setOutlineThickness(3.f);
            }

            shape.setPosition(it->x * 64 + 32, it->y * 64 + 32);
            window.draw(shape);
            it++;
        }
    }

    // 미니맵 렌더링을 위해 객체 맵 원본을 읽기 전용으로 넘겨주는 함수
    const std::unordered_map<int, std::unique_ptr<GameObject>>& GetObjects() const {
        return m_objects;
    }

    void AddSkillEffect(int x, int y, int type) {
        m_bossEffects.push_back({ x, y, type });
    }
};