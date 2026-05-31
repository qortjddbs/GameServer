#include "ResourceManager.h"

bool ResourceManager::LoadTexture(const std::string& id, const std::string& filename) {
    sf::Texture texture;
    // 파일 로드에 성공하면 맵에 이름표(id)를 붙여서 저장
    if (texture.loadFromFile(filename)) {
        m_textures[id] = std::move(texture);
        return true;
    }
    std::cout << "[오류] 텍스처 로드 실패: " << filename << std::endl;
    return false;
}

sf::Texture& ResourceManager::GetTexture(const std::string& id) {
    // 맵에서 id에 해당하는 텍스처를 찾아서 반환
    return m_textures[id];
}

bool ResourceManager::LoadFont(const std::string& id, const std::string& filename) {
    sf::Font font;
    if (font.loadFromFile(filename)) {
        m_fonts[id] = std::move(font);
        return true;
    }
    std::cout << "[오류] 폰트 로드 실패: " << filename << std::endl;
    return false;
}

sf::Font& ResourceManager::GetFont(const std::string& id) {
    return m_fonts[id];
}