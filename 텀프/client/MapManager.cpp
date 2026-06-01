#include "MapManager.h"

void MapManager::Initialize() {
    m_width = 20;
    m_height = 20;

    // 맵 전체를 빈 공간(-1)으로 초기화
    m_map.resize(m_height, std::vector<TileInfo>(m_width, { -1, -1, false }));

    // 타일 스프라이트에 텍스처 미리 연결
    m_tileSprite.setTexture(ResourceManager::GetInstance().GetTexture("tilemap"));
    m_shadowSprite.setTexture(ResourceManager::GetInstance().GetTexture("shadow"));

    // 테스트용 10x10 풀밭 섬 생성 (화면 중앙쯤)
    for (int y = 5; y < 15; ++y) {
        for (int x = 5; x < 15; ++x) {
            // 스프라이트 시트에서 X:1, Y:1 위치가 '평지 중앙 풀밭' 타일입니다.
            m_map[y][x] = { 1, 1, false };
        }
    }

    // [테스트] 중앙에 언덕(Elevated Ground) 2x2 사이즈로 하나 세워보기
    // 언덕 풀밭 타일은 시트에서 대략 X:5, Y:1 위치에 있습니다. (우측 블록)
    m_map[9][9] = { 5, 1, true };
    m_map[9][10] = { 5, 1, true };
    m_map[10][9] = { 5, 1, true };
    m_map[10][10] = { 5, 1, true };
}

void MapManager::Draw(sf::RenderWindow& window) {
    // 1. 평지(Flat Ground) 먼저 그리기
    for (int y = 0; y < m_height; ++y) {
        for (int x = 0; x < m_width; ++x) {
            if (m_map[y][x].sheetX != -1 && !m_map[y][x].isElevated) {
                // 시트에서 64x64만큼 잘라내기
                m_tileSprite.setTextureRect(sf::IntRect(m_map[y][x].sheetX * TILE_SIZE, m_map[y][x].sheetY * TILE_SIZE, TILE_SIZE, TILE_SIZE));
                m_tileSprite.setPosition(x * TILE_SIZE, y * TILE_SIZE);
                window.draw(m_tileSprite);
            }
        }
    }

    // 2. 그림자(Shadow) 그리기 (언덕의 한 칸 아래에 그려야 함)
    for (int y = 0; y < m_height; ++y) {
        for (int x = 0; x < m_width; ++x) {
            if (m_map[y][x].isElevated) {
                // 가이드북 규칙: 그림자는 언덕보다 Y축으로 1칸(+64px) 아래에 그려야 입체감이 생김!
                // 128x128 텍스처이므로, 중심을 맞추기 위해 x좌표도 살짝 조정해줍니다.
                m_shadowSprite.setPosition((x * TILE_SIZE) - 32, (y * TILE_SIZE) + TILE_SIZE - 32);
                window.draw(m_shadowSprite);
            }
        }
    }

    // 3. 언덕(Elevated Ground) 그리기
    for (int y = 0; y < m_height; ++y) {
        for (int x = 0; x < m_width; ++x) {
            if (m_map[y][x].isElevated) {
                m_tileSprite.setTextureRect(sf::IntRect(m_map[y][x].sheetX * TILE_SIZE, m_map[y][x].sheetY * TILE_SIZE, TILE_SIZE, TILE_SIZE));
                m_tileSprite.setPosition(x * TILE_SIZE, y * TILE_SIZE);
                window.draw(m_tileSprite);
            }
        }
    }
}