-- monster_spawn.lua

MonsterSpawns = {}
local count = 1

-- ========================================================
-- 1. 고정 배치 보스 몬스터 (유저 시작 지점인 50, 50 근처 배치)
-- ========================================================
MonsterSpawns[count] = { name = "Boss_Mushroom", monster_type = 3, ai_type = 1, level = 99, x = 1000, y = 1000, hp = 50000, atk = 500 }
count = count + 1

-- ========================================================
-- 2. 레벨대별 4대 영역 사냥터 분할 배치 (총 20만 마리 채우기)
-- ========================================================
math.randomseed(os.time())

local name_prefix = { [0] = "Skeleton_", [1] = "Goblin_", [2] = "Flying_eye_", [3] = "Mushroom_" }

-- 20만 마리를 고르게 분배 (구역당 약 5만 마리씩)
for i = count, 200000 do
    local m_type = math.random(0, 3)
    local m_ai = 1
    local m_level = 1
    local rx, ry = 0, 0

    if m_type == 3 then
        -- [1구역: 좌측 상단 (0~1000, 0~1000)] 초보자 지대 - 버섯 (Lv 1~10)
        m_ai = 0 -- Peace 몬스터
        m_level = math.random(1, 10)
        rx = math.random(100, 980)
        ry = math.random(100, 980)
    elseif m_type == 0 then
        -- [2구역: 우측 상단 (1000~2000, 0~1000)] 중급 지대 - 해골 (Lv 11~20)
        m_level = math.random(11, 20)
        rx = math.random(1020, 1900)
        ry = math.random(100, 980)
    elseif m_type == 1 then
        -- [3구역: 좌측 하단 (0~1000, 1000~2000)] 상급 지대 - 고블린 (Lv 21~30)
        m_level = math.random(21, 30)
        rx = math.random(100, 980)
        ry = math.random(1020, 1900)
    else
        -- [4구역: 우측 하단 (1000~2000, 1000~2000)] 최상급 지대 - 플라잉 아이 (Lv 31~40)
        m_level = math.random(31, 40)
        rx = math.random(1020, 1900)
        ry = math.random(1020, 1900)
    end

    MonsterSpawns[i] = {
        name = name_prefix[m_type] .. i,
        monster_type = m_type,
        ai_type = m_ai,
        level = m_level,
        x = rx,
        y = ry,
        hp = 100 + (m_level * 50),       -- [밸런스 조절] 체력 계수 살짝 하향
        atk = 10 + (m_level * 10)        -- [밸런스 조절] 너무 아프지 않게 공격력 하향
    }
end

-- print("[LUA] 레벨별 4대 구역 사냥터 분리 및 20만 마리 배치 완료!")