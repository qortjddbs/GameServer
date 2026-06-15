-- monster_spawn.lua

MonsterSpawns = {}
local count = 1

-- ========================================================
-- 1. 고정 배치 몬스터 (이름에 반드시 종족 영단어 포함!)
-- ========================================================
-- 클라이언트가 "Mushroom"을 인식하므로 보스 이름에도 넣어줍니다.
MonsterSpawns[count] = { name = "Boss_Mushroom", monster_type = 3, ai_type = 1, level = 99, x = 1000, y = 1000, hp = 50000, atk = 1000 }
count = count + 1

-- 마을 경비병도 이미지가 필요하므로 해골(Skeleton) 외형을 씌워줍니다.
MonsterSpawns[count] = { name = "Guard_Skeleton", monster_type = 0, ai_type = 0, level = 50, x = 50, y = 50, hp = 10000, atk = 200 }
count = count + 1

-- ========================================================
-- 2. 나머지 일반 몬스터 랜덤 배치 (19만 9998마리 자동 생성)
-- ========================================================
math.randomseed(os.time()) 

-- 몬스터 타입 번호(0~3)에 대응하는 이름 접두사 딕셔너리
local name_prefix = { 
    [0] = "Skeleton_", 
    [1] = "Goblin_", 
    [2] = "Flying_eye_", 
    [3] = "Mushroom_" 
}

for i = count, 200000 do
    local m_type = math.random(0, 3)
    
    local m_ai = 1
    if m_type == 3 then m_ai = 0 end 
    
    local m_level = math.random(1, 35)

    MonsterSpawns[i] = {
        -- [핵심 수정!] 타입에 맞춰서 Skeleton_1, Goblin_2 형태로 이름 생성
        name = name_prefix[m_type] .. i, 
        monster_type = m_type,
        ai_type = m_ai,
        level = m_level,
        x = math.random(20, 1980),
        y = math.random(20, 1980),
        hp = m_level * 100,
        atk = m_level * 20
    }
end

print("[LUA] 20만 마리 스크립트 데이터 생성 완료!")