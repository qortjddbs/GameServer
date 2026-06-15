-- boss_ai.lua

-- 보스가 데미지를 입을 때마다 C++에서 이 함수를 호출해 줍니다.
function OnDamageTaken(my_id, hp, max_hp)
    local ratio = hp / max_hp

    -- 체력이 50% 이하가 되었을 때의 분노 패턴!
    if ratio <= 0.5 then
        -- C++에서 등록해 준 API(함수)들을 호출합니다.
        API_BossChat("어리석은 인간들... 나의 진정한 힘을 보여주마!!")
        API_CastAoESkill(my_id, 500) -- 주변 반경에 500 광역 데미지
        
        return 1 -- 분노 상태 돌입 성공 신호 리턴
    end
    
    return 0 -- 아직 평범한 상태
end