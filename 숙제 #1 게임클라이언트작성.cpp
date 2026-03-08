#include <iostream>
using namespace std;

int main(void) {
	char board[8][8];

	// 체스판 초기화
	for (int i = 0; i < 8; ++i) {
		for (int j = 0; j < 8; ++j) {
			if ((i + j) % 2 == 0) {		// 맞으면 흰색(-), 아니면 검은색(+) 칸
				board[i][j] = '-';
			}
			else {
				board[i][j] = '+';
			}
		}
	}

	// 체스 말 집어넣기 (4~5행렬에 랜덤하게 집어넣기)
	board[4][4] = 'P';

	for (int i = 0; i < 8; ++i) {
		for (int j = 0; j < 8; ++j) {
			cout << board[i][j];
		}
		cout << endl;
	}
}