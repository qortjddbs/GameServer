#include "MapManager.h"

void MapManager::Initialize(int width, int height) {
    m_width = width;
    m_height = height;

	// 맵 전체를 평지 타일로 초기화
    m_map.resize(m_height, std::vector<TileInfo>(m_width, { 1, 1, false }));

    // 타일 스프라이트에 텍스처 미리 연결
    m_tileSprite.setTexture(ResourceManager::GetInstance().GetTexture("tilemap"));
	m_rockSprite.setTexture(ResourceManager::GetInstance().GetTexture("rock"));

    m_tileSprite.setColor(sf::Color(150, 50, 50)); // 다크 레드
    m_rockSprite.setColor(sf::Color(200, 100, 100));

	// 서버와 같은 맵을 생성하기 위해 시드 고정
    srand(2022180016);

    for (int i = 0; i < 5'0000; ++i) {
        int rx = rand() % (m_width - 2);
        int ry = rand() % (m_height - 2);

        m_map[ry][rx] = { 5, 1, true };
        m_map[ry][rx + 1] = { 5, 1, true };
        m_map[ry + 1][rx] = { 5, 1, true };
        m_map[ry + 1][rx + 1] = { 5, 1, true };
    }

    // 맵 생성이 끝나면 다른 랜덤 요소들을 위해 시드 초기화
    srand(static_cast<unsigned int>(time(NULL)));
}

void MapManager::Draw(sf::RenderWindow& window, const sf::View& camera) {
	sf::Vector2f center = camera.getCenter();
	sf::Vector2f size = camera.getSize();

	int startX = std::max(0, static_cast<int>((center.x - size.x / 2.0f) / TILE_SIZE) - 1);
    int startY = std::max(0, static_cast<int>((center.y - size.y / 2.0f) / TILE_SIZE) - 1);
	int endX = std::min(m_width - 1, static_cast<int>((center.x + size.x / 2.0f) / TILE_SIZE) + 2);
	int endY = std::min(m_height - 1, static_cast<int>((center.y + size.y / 2.0f) / TILE_SIZE) + 2);

    for (int y = startY; y < endY; ++y) {
        for (int x = startX; x < endX; ++x) {
            m_tileSprite.setTextureRect(sf::IntRect(TILE_SIZE , TILE_SIZE, TILE_SIZE * 2, TILE_SIZE * 2));
            m_tileSprite.setPosition(x * TILE_SIZE, y * TILE_SIZE);
            window.draw(m_tileSprite);

            if (m_map[y][x].isElevated) {
                m_rockSprite.setPosition(x * TILE_SIZE, y * TILE_SIZE + 20);
                window.draw(m_rockSprite);
            }
        }
    }
}