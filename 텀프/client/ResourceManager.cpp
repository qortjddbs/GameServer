#include "ResourceManager.h"

bool ResourceManager::LoadTexture(const std::string& id, const std::string& filename) {
	sf::Texture& texture = m_textures[id]; // 맵에서 id에 해당하는 텍스처를 가져오거나 새로 생성
    if (texture.loadFromFile(filename)) {
        return true;
	}

    std::cout << "[오류] 텍스처 로드 실패: " << filename << std::endl;
	m_textures.erase(id); // 로드 실패 시 맵에서 제거
    return false;
}

sf::Texture& ResourceManager::GetTexture(const std::string& id) {
    // 맵에서 id에 해당하는 텍스처를 찾아서 반환
    return m_textures[id];
}

bool ResourceManager::LoadFont(const std::string& id, const std::string& filename) {
    sf::Font& font = m_fonts[id];

    if (font.loadFromFile(filename)) {
        return true; // 로드 성공
    }

    std::cout << "[오류] 폰트 로드 실패: " << filename << std::endl;
    m_fonts.erase(id);
    return false;
}

sf::Font& ResourceManager::GetFont(const std::string& id) {
    return m_fonts[id];
}