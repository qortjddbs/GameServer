#include "MapManager.h"

void MapManager::Initialize(int width, int height) {
    m_width = 200;
    m_height = 200;

	// 맵 전체를 평지 타일로 초기화
    m_map.resize(m_height, std::vector<TileInfo>(m_width, { 1, 1, false }));

    // 타일 스프라이트에 텍스처 미리 연결
    m_tileSprite.setTexture(ResourceManager::GetInstance().GetTexture("tilemap"));
    m_shadowSprite.setTexture(ResourceManager::GetInstance().GetTexture("shadow"));

    for (int i = 0; i < 500; ++i) {
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
	int endX = std::min(m_width - 1, static_cast<int>((center.x + size.x / 2.0f) / TILE_SIZE) + 1);

	int startY = std::max(0, static_cast<int>((center.y - size.y / 2.0f) / TILE_SIZE) - 1);
	int endY = std::min(m_height - 1, static_cast<int>((center.y + size.y / 2.0f) / TILE_SIZE) + 1);

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