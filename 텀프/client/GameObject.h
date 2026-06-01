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
    sf::Clock walkTimer;
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
            walkTimer.restart();

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

    void doAttack1() {
        currentState = AnimState::ATTACK1;
        sprite.setTexture(ResourceManager::GetInstance().GetTexture("attack1"));
        currentFrame = 0;
        maxFrames = 4;
        isActionPlaying = true;

        sprite.setTextureRect(sf::IntRect(0, 0, frameWidth, frameHeight));
        animClock.restart();
    }

    void doAttack2() {
        currentState = AnimState::ATTACK2;
        sprite.setTexture(ResourceManager::GetInstance().GetTexture("attack2"));
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
        if (isWalking && walkTimer.getElapsedTime().asSeconds() > 0.15f) {
            isWalking = false;
		}

        // 1. 현재 상태에 맞춰 애니메이션 속도 세팅
        float animSpeed = 0.1f;
        if (currentState == AnimState::RUN) animSpeed = 0.05f;
		else if (isActionPlaying) animSpeed = 0.06f;    // 공격/방어는 조금 더 빠르게

        // 2. 타이머 틱 (시간이 다 되면 프레임 1증가)
        if (animClock.getElapsedTime().asSeconds() > animSpeed) {
            currentFrame++;

            // 핵심 해결 부분 : 단발성 액션이 끝까지 재생되었을 때
            if (isActionPlaying && currentFrame >= maxFrames) {
                isActionPlaying = false;

                // 액션이 끝나자마자 현재 방향키를 누르고 있는지 검사해서 상태를 즉시 복구
				currentState = isWalking ? AnimState::RUN : AnimState::IDLE;
				sprite.setTexture(ResourceManager::GetInstance().GetTexture(isWalking ? "run" : "idle"));
                currentFrame = 0;
                maxFrames = isWalking ? 6 : 8;
            } 
            // 일반 루프 애니메이션일 때는 처음으로 반복
            else if (currentFrame >= maxFrames) {
                currentFrame = 0;
			}

            // 텍스처 영역을 잘라서 적용
			sprite.setTextureRect(sf::IntRect(currentFrame * frameWidth, 0, frameWidth, frameHeight));
            
            // 무조건 여기서 딱 한 번만 시계를 리셋하여 무한 루프 폭탄 제거
			animClock.restart();
        }

        // 액션 중이 아닐 때만 실시간으로 걷기/대기 전환
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
				sprite.setTextureRect(sf::IntRect(0, 0, frameWidth, frameHeight));
            }
        }
    }

    virtual void draw(sf::RenderWindow& window) {
        window.draw(sprite);
    }
};