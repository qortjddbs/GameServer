USE [2026_Game_2022180016] -- 본인 학번 DB 이름 확인!
GO

-- =============================================
-- 1. 과제 전용 새로운 테이블 생성 (project_user)
-- =============================================
CREATE TABLE dbo.project_user (
    user_name nchar(10) PRIMARY KEY,
    pos_x int NOT NULL DEFAULT 0,
    pos_y int NOT NULL DEFAULT 0
);
GO

-- 테스트용 데이터 삽입 (과제용 테이블에 미리 캐릭터를 만들어 둠)
INSERT INTO dbo.project_user (user_name, pos_x, pos_y) VALUES ('tom', 10, 10);
INSERT INTO dbo.project_user (user_name, pos_x, pos_y) VALUES ('jame', 20, 20);
INSERT INTO dbo.project_user (user_name, pos_x, pos_y) VALUES ('peter', 30, 30);
GO

-- =============================================
-- 2. 프로시저 생성 및 수정 (새 테이블을 바라보도록 덮어쓰기)
-- =============================================
-- [1] 로그인 확인용 프로시저 (project_user 테이블 조회)
CREATE OR ALTER PROCEDURE sp_login_check
    @UserName nchar(10)
AS
BEGIN
    SET NOCOUNT ON;
    -- 🚨 조회 대상을 기존 user_table에서 project_user로 변경
    SELECT pos_x, pos_y FROM dbo.project_user WHERE user_name = @UserName;
END
GO

-- [2] 로그아웃 시 위치 저장용 프로시저 (project_user 테이블 업데이트)
CREATE OR ALTER PROCEDURE sp_save_position
    @UserName nchar(10),
    @PosX int,
    @PosY int
AS
BEGIN
    SET NOCOUNT ON;
    -- 🚨 업데이트 대상을 기존 user_table에서 project_user로 변경
    UPDATE dbo.project_user 
    SET pos_x = @PosX, pos_y = @PosY 
    WHERE user_name = @UserName;
END
GO