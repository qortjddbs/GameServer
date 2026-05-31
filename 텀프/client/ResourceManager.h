#pragma once
#include <SFML/Graphics.hpp>
#include <unordered_map>
#include <string>
#include <iostream>

class ResourceManager {
private:
    // 전역 변수였던 창고들을 클래스 내부로 안전하게 숨김
    std::unordered_map<std::string, sf::Texture> m_textures;
    std::unordered_map<std::string, sf::Font> m_fonts;

    // 싱글톤 패턴: 외부에서 객체를 마음대로 생성하지 못하도록 생성자를 private으로 숨김
    ResourceManager() = default;

public:
    // 프로그램 어디서든 단 하나의 매니저에 접근할 수 있게 해주는 마법의 함수
    static ResourceManager& GetInstance() {
        static ResourceManager instance;
        return instance;
    }

    // 복사 방지 (싱글톤의 철칙)
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    // 리소스 로드 함수
    bool LoadTexture(const std::string& id, const std::string& filename);
    bool LoadFont(const std::string& id, const std::string& filename);

    // 리소스 가져오기 함수 (복사하지 않고 원본의 참조(&)를 반환)
    sf::Texture& GetTexture(const std::string& id);
    sf::Font& GetFont(const std::string& id);
};