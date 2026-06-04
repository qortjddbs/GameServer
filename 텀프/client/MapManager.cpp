#include "MapManager.h"

void MapManager::Initialize(int width, int height) {
    m_width = width;
    m_height = height;

	// 맵 전체를 평지 타일로 초기화
    m_map.resize(m_height, std::vector<TileInfo>(m_width, { 1, 1, false }));

    // 타일 스프라이트에 텍스처 미리 연결
    m_tileSprite.setTexture(ResourceManager::GetInstance().GetTexture("tilemap"));
    m_shadowSprite.setTexture(ResourceManager::GetInstance().GetTexture("shadow"));

    for (int i = 0; i < 5'0000; ++i) {
        int rx = rand() % (m_width - 2);
        int ry = rand() % (m_height - 2);

        m_map[ry][rx] = { 5, 1, true };
        m_map[ry][rx + 1] = { 5, 1, true };
        m_map[ry + 1][rx] = { 5, 1, true };
        m_map[ry + 1][rx + 1] = { 5, 1, true };
    }
}

void MapManager::Draw(sf::RenderWindow& window, const sf::View& camera) {
	sf::Vector2f center = camera.getCenter();
	sf::Vector2f size = camera.getSize();

	int startX = std::max(0, static_cast<int>((center.x - size.x / 2.0f) / TILE_SIZE) - 1);
    int startY = std::max(0, static_cast<int>((center.y - size.y / 2.0f) / TILE_SIZE) - 1);
	int endX = std::min(m_width - 1, static_cast<int>((center.x + size.x / 2.0f) / TILE_SIZE) + 2);
	int endY = std::min(m_height - 1, static_cast<int>((center.y + size.y / 2.0f) / TILE_SIZE) + 2);

    // 1. 평지(Flat Ground) 먼저 그리기
    for (int y = startY; y < endY; ++y) {
        for (int x = startX; x < endX; ++x) {
            if (m_map[y][x].sheetX != -1 && !m_map[y][x].isElevated) {
                // 시트에서 64x64만큼 잘라내기
                m_tileSprite.setTextureRect(sf::IntRect(m_map[y][x].sheetX * TILE_SIZE, m_map[y][x].sheetY * TILE_SIZE, TILE_SIZE, TILE_SIZE));
                m_tileSprite.setPosition(x * TILE_SIZE, y * TILE_SIZE);
                window.draw(m_tileSprite);
            }
        }
    }

    // 2. 그림자(Shadow) 그리기 (언덕의 한 칸 아래에 그려야 함)
    for (int y = startY; y < endY; ++y) {
        for (int x = startX; x < endX; ++x) {
            if (m_map[y][x].isElevated) {
                // 그림자 먼저 그리기
                m_shadowSprite.setPosition(x * TILE_SIZE - 32, y * TILE_SIZE + 64 - 32);
                window.draw(m_shadowSprite);

                // 언덕 그리기
                m_tileSprite.setTextureRect(sf::IntRect(m_map[y][x].sheetX * TILE_SIZE, m_map[y][x].sheetY * TILE_SIZE, TILE_SIZE, TILE_SIZE));
                m_tileSprite.setPosition(x * TILE_SIZE, y * TILE_SIZE);
                window.draw(m_tileSprite);
            }
        }
    }
}