#pragma once
#include <SFML/Graphics.hpp>
#include <string>
#include "..\..\텀프\server\protocol_2026.h"
#include "ResourceManager.h" // 텍스처를 가져오기 위해 포함

// 애니메이션 상태 열거형
enum class AnimState { IDLE, RUN, ATTACK, HIT, DEATH };
enum class ObjectType { PLAYER, SKELETON, GOBLIN, FLYING_EYE, MUSHROOM };

class GameObject {
public:
    int id;
    int x, y;
    char name[MAX_NAME_LEN];
    int hp, max_hp;
    unsigned long long exp;
    unsigned char level;

    sf::Sprite sprite;
    sf::Text nameText;

    // 애니메이션 관리를 위한 변수들
    sf::Clock animClock;
    sf::Clock walkTimer;
    sf::Clock guardTimer;
    AnimState currentState = AnimState::IDLE;
    bool isWalking = false;
    int prev_x = 0, prev_y = 0;
    bool isActionPlaying = false; // 공격/방어 진행 중 플래그

    // 플레이어 피격 판정
    sf::Clock hitTimer;
    bool isHit = false;

    // 프레임 규격 (한 장당 192 x 192)
    int frameWidth = 192;
    int frameHeight = 192;
    ObjectType objectType = ObjectType::PLAYER;

    int currentFrame = 0;       // 현재 프레임 번호
    int maxFrames = 8;          // 총 프레임 개수 (Idle 기준)

	float scale = 1.0f;            // 스프라이트 크기 조절용
    bool isFirstSpawn = true;

    // 생성자 (ResourceManager 적용 완료)
    GameObject() : id(-1), x(0), y(0), hp(0), max_hp(0), exp(0), level(1) {
        memset(name, 0, sizeof(name));

        // 매니저를 통해 초기 텍스처(Idle) 세팅
        sprite.setTexture(ResourceManager::GetInstance().GetTexture("player_idle"));

        sprite.setTextureRect(sf::IntRect(0, 0, frameWidth, frameHeight));
        sprite.setOrigin(frameWidth / 2.0f, frameHeight / 2.0f);
    }

    virtual void setPosition(int new_x, int new_y) {
        if (isFirstSpawn) {
            x = new_x;
			y = new_y;
			prev_x = new_x;
            prev_y = new_y;
            isFirstSpawn = false;
        }
        else {
            if (x != new_x || y != new_y) {
                isWalking = true;
                walkTimer.restart();

                if (new_x < x) sprite.setScale(-scale, scale);
                else if (new_x > x) sprite.setScale(scale, scale);
            }
            else {
                isWalking = false;
            }

            prev_x = x;
            prev_y = y;
            x = new_x;
            y = new_y;
        }
        sprite.setPosition(static_cast<float>(x * 64 + 32), static_cast<float>(y * 64 + 32));

        std::string typeStr = "";
        switch (objectType) {
        case ObjectType::SKELETON:          typeStr = "[skeleton] "; break;
        case ObjectType::GOBLIN:            typeStr = "[goblin] "; break;
        case ObjectType::FLYING_EYE:        typeStr = "[flying_eye] "; break;
        case ObjectType::MUSHROOM:          typeStr = "[mushroom] "; break;
        default:                            typeStr = ""; break; // 플레이어는 종류 생략
        }

        char textBuf[128];
		sprintf_s(textBuf, "%s\n(%d, %d)",typeStr.c_str(), x, y);
        nameText.setString(textBuf);

        sf::FloatRect textRect = nameText.getLocalBounds();
		nameText.setOrigin(textRect.width / 2.0f, textRect.height / 2.0f);
		nameText.setPosition(static_cast<float>(x * 64 + 32), static_cast<float>(y * 64 - 25));
    }

    void setDirection(unsigned char dir) {
        if (dir == 2) sprite.setScale(-scale, scale); // Left
		else sprite.setScale(scale, scale); // Right
    }

    void doAttack() {
        currentState = AnimState::ATTACK;
        isActionPlaying = true;
        currentFrame = 0;
        animClock.restart();

        switch (objectType) {
        case ObjectType::PLAYER: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("player_attack1"));
            maxFrames = 6;
            break;
            }
        case ObjectType::SKELETON: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("skeleton_attack1"));
            maxFrames = 8;
            break;
            }
        case ObjectType::GOBLIN: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("goblin_attack1"));
            maxFrames = 8;
            break;
        }
        case ObjectType::FLYING_EYE: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("flying_eye_attack1"));
            maxFrames = 8;
            break;
        }
        case ObjectType::MUSHROOM: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("mushroom_attack1"));
            maxFrames = 8;
            break;
        }
        }


        sprite.setTextureRect(sf::IntRect(0, 0, frameWidth, frameHeight));
    }

    void doHit() {
        currentState = AnimState::HIT;
        isActionPlaying = true;
        currentFrame = 0;
        animClock.restart();

        switch (objectType) {
        case ObjectType::PLAYER: {
            isHit = true;
            hitTimer.restart();

            sprite.setTexture(ResourceManager::GetInstance().GetTexture(isWalking ? "player_run" : "player_idle"));
            maxFrames = isWalking ? 6 : 8;
            break;
        }
        case ObjectType::SKELETON: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("skeleton_hit"));
            maxFrames = 4;
            break;
        }
        case ObjectType::GOBLIN: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("goblin_hit"));
            maxFrames = 4;
            break;
        }
        case ObjectType::FLYING_EYE: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("flying_eye_hit"));
            maxFrames = 4;
            break;
        }
        case ObjectType::MUSHROOM: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("mushroom_hit"));
            maxFrames = 4;
            break;
        }
        }
        sprite.setTextureRect(sf::IntRect(0, 0, frameWidth, frameHeight));
    }

    void doDeath() {
        currentState = AnimState::DEATH;
        isActionPlaying = true;
        currentFrame = 0;
        animClock.restart();

        switch (objectType) {
        case ObjectType::SKELETON: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("skeleton_death"));
            maxFrames = 4;
            break;
        }
        case ObjectType::GOBLIN: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("goblin_death"));
            maxFrames = 4;
            break;
        }
        case ObjectType::FLYING_EYE: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("flying_eye_death"));
            maxFrames = 4;
            break;
        }
        case ObjectType::MUSHROOM: {
            sprite.setTexture(ResourceManager::GetInstance().GetTexture("mushroom_death"));
            maxFrames = 4;
            break;
        }
        }
        sprite.setTextureRect(sf::IntRect(0, 0, frameWidth, frameHeight));
    }

    virtual void updateAnimation() {
        if (isWalking && walkTimer.getElapsedTime().asSeconds() > 0.55f) {
            isWalking = false;
		}

        // 1. 현재 상태에 맞춰 애니메이션 속도 세팅
        float animSpeed = 0.1f;
        if (currentState == AnimState::RUN) animSpeed = 0.05f;
		else if (isActionPlaying) animSpeed = 0.06f;    // 공격은 좀 더 빠르게

        // 2. 타이머 틱 (시간이 다 되면 프레임 1증가)
        if (animClock.getElapsedTime().asSeconds() > animSpeed) {
            if (currentState == AnimState::DEATH && currentFrame == maxFrames - 1) {
                currentFrame = maxFrames - 1;
            }
            else currentFrame++;

            // 단발성 액션이 끝까지 재생되었을 때
            if (isActionPlaying && currentFrame >= maxFrames) {
                if (currentState == AnimState::DEATH) {
                    currentFrame = maxFrames - 1; // 초과 방지
                }
                else {
                    isActionPlaying = false;
                    currentState = isWalking ? AnimState::RUN : AnimState::IDLE;
                    currentFrame = 0;
                    switch (objectType) {
                    case ObjectType::PLAYER: {
                        sprite.setTexture(ResourceManager::GetInstance().GetTexture(isWalking ? "player_run" : "player_idle"));
                        maxFrames = isWalking ? 6 : 8;
                        break;
                    }
                    case ObjectType::SKELETON: {
                        sprite.setTexture(ResourceManager::GetInstance().GetTexture(isWalking ? "skeleton_walk" : "skeleton_idle"));
                        maxFrames = 4;
                        break;
                    } case ObjectType::MUSHROOM: {
                        sprite.setTexture(ResourceManager::GetInstance().GetTexture(isWalking ? "mushroom_walk" : "mushroom_idle"));
                        maxFrames = isWalking ? 8 : 4;
                        break;
                    }
                    case ObjectType::GOBLIN: {
                        sprite.setTexture(ResourceManager::GetInstance().GetTexture(isWalking ? "goblin_walk" : "goblin_idle"));
                        maxFrames = isWalking ? 8 : 4;
                        break;
                    }
                    case ObjectType::FLYING_EYE: {
                        sprite.setTexture(ResourceManager::GetInstance().GetTexture("flying_eye_flight"));
                        maxFrames = 4;
                        break;
                    }
                    }
                }
            } 
            // 일반 루프 애니메이션일 때는 처음으로 반복
            else if (!isActionPlaying && currentFrame >= maxFrames) {
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

                switch (objectType) {
                    case ObjectType::PLAYER: {
                        sprite.setTexture(ResourceManager::GetInstance().GetTexture(isWalking ? "player_run" : "player_idle"));
                        maxFrames = isWalking ? 6 : 8;
                        break;
					}
                    case ObjectType::SKELETON: {
                        sprite.setTexture(ResourceManager::GetInstance().GetTexture(isWalking ? "skeleton_walk" : "skeleton_idle"));
                        maxFrames = 4;
                        break;
					}
                    case ObjectType::GOBLIN: {
                        sprite.setTexture(ResourceManager::GetInstance().GetTexture(isWalking ? "goblin_walk" : "goblin_idle"));
                        maxFrames = isWalking ? 8 : 4;
						break;
                    }
                    case ObjectType::MUSHROOM: {
                        sprite.setTexture(ResourceManager::GetInstance().GetTexture(isWalking ? "mushroom_walk" : "mushroom_idle"));
                        maxFrames = isWalking ? 8 : 4;
                        break;
                    }
                    case ObjectType::FLYING_EYE: {
                        sprite.setTexture(ResourceManager::GetInstance().GetTexture("flying_eye_flight"));
                        maxFrames = 4;
                        break;
                    }
                }
				sprite.setTextureRect(sf::IntRect(0, 0, frameWidth, frameHeight));
            }
        }

        if (isHit) {
            if (hitTimer.getElapsedTime().asSeconds() < 0.2f) {
                sprite.setColor(sf::Color(255, 100, 100, 200));
            }
            else {
                sprite.setColor(sf::Color(255, 255, 255, 255));
                isHit = false;
            }
        }
    }

    virtual void draw(sf::RenderWindow& window) {
        window.draw(sprite);
        window.draw(nameText);
    }
};