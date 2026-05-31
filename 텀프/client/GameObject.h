#pragma once
#include <SFML/Graphics.hpp>
#include <string>
#include "..\..\텀프\server\protocol_2026.h"
#include "ResourceManager.h" // 텍스처를 가져오기 위해 포함

// 애니메이션 상태 열거형
enum class AnimState { IDLE, RUN, ATTACK1, ATTACK2, GUARD };

class GameObject {
public:
    int id;
    int x, y;
    char name[MAX_NAME_LEN];
    int hp, max_hp;
    unsigned long long exp;
    unsigned char level;

    sf::Sprite sprite;

    // 애니메이션 관리를 위한 변수들
    sf::Clock animClock;
    AnimState currentState = AnimState::IDLE;
    bool isWalking = false;
    int prev_x = 0, prev_y = 0;
    bool isActionPlaying = false; // 공격/방어 진행 중 플래그

    // 프레임 규격 (한 장당 192 x 192)
    const int frameWidth = 192;
    const int frameHeight = 192;

    int currentFrame = 0;       // 현재 프레임 번호
    int maxFrames = 8;          // 총 프레임 개수 (Idle 기준)

    // 생성자 (ResourceManager 적용 완료)
    GameObject() : id(-1), x(0), y(0), hp(0), max_hp(0), exp(0), level(1) {
        memset(name, 0, sizeof(name));

        // 매니저를 통해 초기 텍스처(Idle) 세팅
        sprite.setTexture(ResourceManager::GetInstance().GetTexture("idle"));

        sprite.setTextureRect(sf::IntRect(0, 0, frameWidth, frameHeight));
        sprite.setOrigin(frameWidth / 2.0f, frameHeight / 2.0f);
    }

    virtual void setPosition(int new_x, int new_y) {
        if (x != new_x || y != new_y) {
            isWalking = true;
            if (new_x < x) sprite.setScale(-1.0f, 1.0f);
            else if (new_x > x) sprite.setScale(1.0f, 1.0f);
        }
        else {
            isWalking = false;
        }

        prev_x = x;
        prev_y = y;
        x = new_x;
        y = new_y;

        sprite.setPosition(static_cast<float>(x * 64 + 32), static_cast<float>(y * 64 + 32));
    }

    void doAttack() {
        currentState = AnimState::ATTACK1;
        sprite.setTexture(ResourceManager::GetInstance().GetTexture("attack1"));
        currentFrame = 0;
        maxFrames = 4;
        isActionPlaying = true;

        sprite.setTextureRect(sf::IntRect(0, 0, frameWidth, frameHeight));
        animClock.restart();
    }

    void doGuard() {
        currentState = AnimState::GUARD;
        sprite.setTexture(ResourceManager::GetInstance().GetTexture("guard"));
        currentFrame = 0;
        maxFrames = 6;
        isActionPlaying = true;

        sprite.setTextureRect(sf::IntRect(0, 0, frameWidth, frameHeight));
        animClock.restart();
    }

    virtual void updateAnimation() {
        if (isActionPlaying) {
            if (currentFrame >= maxFrames - 1 && animClock.getElapsedTime().asSeconds() > 0.05f) {
                isActionPlaying = false;
                currentState = AnimState::IDLE;
                sprite.setTexture(ResourceManager::GetInstance().GetTexture("idle"));
                currentFrame = 0;
                maxFrames = 8;
            }
        }

        if (!isActionPlaying) {
            AnimState nextState = isWalking ? AnimState::RUN : AnimState::IDLE;

            if (currentState != nextState) {
                currentState = nextState;
                currentFrame = 0;

                if (currentState == AnimState::RUN) {
                    sprite.setTexture(ResourceManager::GetInstance().GetTexture("run"));
                    maxFrames = 6;
                }
                else {
                    sprite.setTexture(ResourceManager::GetInstance().GetTexture("idle"));
                    maxFrames = 8;
                }
            }
        }

        float animSpeed = (currentState == AnimState::RUN) ? 0.05f : 0.1f;

        if (animClock.getElapsedTime().asSeconds() > animSpeed) {
            currentFrame++;

            if (currentFrame >= maxFrames) {
                if (isActionPlaying) currentFrame = maxFrames - 1;
                else currentFrame = 0;
            }
            sprite.setTextureRect(sf::IntRect(currentFrame * frameWidth, 0, frameWidth, frameHeight));
            animClock.restart();
        }
    }

    virtual void draw(sf::RenderWindow& window) {
        window.draw(sprite);
    }
};