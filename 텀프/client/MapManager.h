#pragma once
#include <SFML/Graphics.hpp>
#include <vector>
#include "ResourceManager.h"

// 맵의 타일 하나가 가질 정보 (어떤 이미지를 쓸 것인가)
struct TileInfo {
    int sheetX; // 스프라이트 시트 상의 X 칸 (0부터 시작)
    int sheetY; // 스프라이트 시트 상의 Y 칸
    bool isElevated; // 언덕인지 여부 (그림자를 그리기 위해)
};

class MapManager {
private:
    static const int TILE_SIZE = 64;
    int m_width, m_height;

    // 맵 데이터를 저장할 2차원 배열
    std::vector<std::vector<TileInfo>> m_map;

    sf::Sprite m_tileSprite;
    sf::Sprite m_shadowSprite;

    MapManager() = default;

public:
    static MapManager& GetInstance() {
        static MapManager instance;
        return instance;
    }

    // 맵 초기화 및 임시 섬 만들기
    void Initialize();

    // 렌더링 (캐릭터보다 먼저 호출되어야 함)
    void Draw(sf::RenderWindow& window);
};