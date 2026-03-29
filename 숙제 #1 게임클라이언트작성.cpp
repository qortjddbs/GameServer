#include <iostream>
#include <conio.h>
#include <windows.h>
using namespace std;

#define UP 72
#define DOWN 80
#define RIGHT 77
#define LEFT 75
#define EXIT 113	// q키

void InitBoard(char board[8][8]) {
	for (int i = 0; i < 8; ++i) {
		for (int j = 0; j < 8; ++j) {
			if ((i + j) % 2 == 0) {		// 맞으면 흰색(-), 아니면 검은색(+) 칸
				board[i][j] = 'W';
			}
			else {
				board[i][j] = 'B';
			}
		}
	}
}

void PrintBoard(char board[8][8]) {
	for (int i = 0; i < 8; ++i) {
		for (int j = 0; j < 8; ++j) {
			cout << board[i][j];
		}
		cout << endl;
	}
}

int main(void) {
	int px = 3;
	int py = 3;

	char board[8][8];

	// 체스판 초기화
	InitBoard(board);

	// 체스 말 집어넣기
	board[py][px] = '*';

	// 체스판 띄우기
	PrintBoard(board);

	// 방향키 누르면 체스 말 이동하고 화면 다시 그리기
	while (1) {
		int key = _getch();
		// cout << key << endl;
		switch (key) {
		case UP:
			if (py > 0) {
				if ((px + py) % 2 == 0) board[py][px] = 'W';
				else board[py][px] = 'B';
				py--;
				board[py][px] = '*';
			}
			break;
		case DOWN:
			if (py < 7) {
				if ((px + py) % 2 == 0) board[py][px] = 'W';
				else board[py][px] = 'B';
				py++;
				board[py][px] = '*';
			}
			break;
		case RIGHT:
			if (px < 7) {
				if ((px + py) % 2 == 0) board[py][px] = 'W';
				else board[py][px] = 'B';
				px++;
				board[py][px] = '*';
			}
			break;
		case LEFT:
			if (px > 0) {
				if ((px + py) % 2 == 0) board[py][px] = 'W';
				else board[py][px] = 'B';
				px--;
				board[py][px] = '*';
			}
			break;
		case EXIT:		// q누르면 나가기
			exit(0);
			break;
		}
		system("cls");
		PrintBoard(board);
	 }
}